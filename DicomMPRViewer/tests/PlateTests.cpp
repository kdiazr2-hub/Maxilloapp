#include "PlateCore.h"

#include "GuideDesignCore.h"
#include "MeshRepairCore.h"
#include "OsteotomyCore.h"
#include "SplintTestGeometry.h"
#include "WrapCore.h"

#include <QJsonDocument>

#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkImplicitPolyDataDistance.h>
#include <vtkStaticCellLocator.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>

#include <cmath>
#include <functional>
#include <iostream>
#include <string>

namespace
{
using namespace splinttest;
using Vec3 = std::array<double, 3>;

// The anterior maxilla, seen as it is in the tests of the guides: a wall whose face looks forward (+y).
// The cranial base is above the cut (z >= 10), the Le Fort segment below it (z <= 8).
vtkSmartPointer<vtkPolyData> cranialBase() { return boxMesh({-25.0, 25.0, -10.0, 0.0, 10.0, 30.0}, false, false); }
vtkSmartPointer<vtkPolyData> segmentBeforeCut() { return boxMesh({-25.0, 25.0, -10.0, 0.0, -10.0, 8.0}, false, false); }

// The planned motion: 3 mm advancement and 1 mm impaction, with a small yaw so rotation matters too.
std::array<double, 16> plannedMotion()
{
    auto transform = vtkSmartPointer<vtkTransform>::New();
    transform->PostMultiply();
    transform->RotateZ(2.0);
    transform->Translate(0.0, 3.0, 1.0);
    std::array<double, 16> matrix{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            matrix[static_cast<size_t>(4 * r + c)] = transform->GetMatrix()->GetElement(r, c);
    return matrix;
}

vtkSmartPointer<vtkPolyData> moved(vtkPolyData* mesh, const std::array<double, 16>& matrix)
{
    auto transform = vtkSmartPointer<vtkTransform>::New();
    transform->SetMatrix(matrix.data());
    auto filter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
    filter->SetInputData(mesh);
    filter->SetTransform(transform);
    filter->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(filter->GetOutput());
    return out;
}

// The planned cut, between the two pieces at z ≈ 9, swept front to back.
OsteotomyPath leFortCut()
{
    return OsteotomyCore::LeFortPath({{{-10.0, 5.0, 9.0}, {10.0, 5.0, 9.0}, {-20.0, -5.0, 9.0}, {20.0, -5.0, 9.0}}});
}

// A paranasal plate on the right side, holes clicked top to bottom: two on the cranial base, two on the
// segment in its planned position (its face is now at y ≈ 3).
PlateDesign paranasalPlate(const std::array<double, 16>& motion)
{
    PlateDesign plate;
    plate.name = QStringLiteral("Placa derecha");
    plate.kind = PlateTemplate::Paranasal;
    for (const Vec3& p : {Vec3{-10.0, 0.0, 20.0}, Vec3{-10.0, 0.0, 14.0}}) {
        PlateHole hole;
        hole.center = p;
        hole.axis = {0.0, 1.0, 0.0};
        plate.holes.push_back(hole);
    }
    // On the segment: picked on the moved face, so carry a point of the pre-operative face along the motion.
    for (const Vec3& p : {Vec3{-10.0, 0.0, 4.0}, Vec3{-10.0, 0.0, -2.0}}) {
        PlateHole hole;
        hole.center = PlateCore::TransformPoint(motion, p);
        hole.axis = PlateCore::TransformVector(motion, {0.0, 1.0, 0.0});
        plate.holes.push_back(hole);
    }
    plate.struts = PlateCore::TemplateStruts(PlateTemplate::Paranasal, 4, 0);
    return plate;
}

double distance(const Vec3& a, const Vec3& b)
{
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

// Signed distance to the closed, outward mesh (negative inside), probed at each point.
std::vector<bool> inside(vtkPolyData* mesh, const std::vector<Vec3>& points)
{
    auto distance = vtkSmartPointer<vtkImplicitPolyDataDistance>::New();
    distance->SetInput(mesh);
    std::vector<bool> out;
    for (const Vec3& p : points)
        out.push_back(distance->EvaluateFunction(p[0], p[1], p[2]) < 0.0);
    return out;
}

// ── Motion ────────────────────────────────────────────────────────────────────
void testRigidMotionIsRecoveredFromTheMeshes()
{
    const auto before = segmentBeforeCut();
    const std::array<double, 16> motion = plannedMotion();
    const auto after = moved(before, motion);
    std::array<double, 16> found{};
    double rms = 1.0;
    QString error;
    require(PlateCore::RigidMotion(before, after, found, &rms, &error), error.toStdString());
    require(rms < 1e-6, "the rigid fit left a residual on a rigid motion: " + std::to_string(rms));
    for (size_t i = 0; i < 16; ++i)
        require(std::abs(found[i] - motion[i]) < 1e-6, "the recovered motion differs at element " + std::to_string(i));

    // Going back is the inverse.
    const Vec3 p{3.0, -2.0, 5.0};
    const Vec3 there = PlateCore::TransformPoint(found, p);
    const Vec3 back = PlateCore::TransformPoint(PlateCore::Invert(found), there);
    require(distance(back, p) < 1e-9, "the inverse motion does not bring a point back");

    // A re-cut mesh cannot be read as a motion, and neither can a deformed one.
    require(!PlateCore::RigidMotion(before, cranialBase(), found, nullptr, &error) && !error.isEmpty(),
            "a different mesh was accepted as the moved segment");
    auto stretched = vtkSmartPointer<vtkTransform>::New();
    stretched->Scale(1.1, 1.0, 1.0);
    auto filter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
    filter->SetInputData(before);
    filter->SetTransform(stretched);
    filter->Update();
    require(!PlateCore::RigidMotion(before, filter->GetOutput(), found, &rms, &error),
            "a deformed segment was accepted as rigidly moved");
}

// ── Holes ─────────────────────────────────────────────────────────────────────
void testTemplatesJoinTheHoles()
{
    const auto paranasal = PlateCore::TemplateStruts(PlateTemplate::Paranasal, 4, 0);
    require(paranasal.size() == 1 && paranasal[0] == std::vector<int>({0, 1, 2, 3}),
            "the paranasal plate is not one strut through its holes");
    const auto shapeL = PlateCore::TemplateStruts(PlateTemplate::LShape, 4, 3);
    require(shapeL.size() == 3, "the L plate does not have two arms and a bar");
    require(shapeL[1] == std::vector<int>({4, 5, 6}), "the buttress arm is not the second set of holes");
    require(shapeL[2] == std::vector<int>({3, 6}), "the bar does not join the lowest hole of each arm");
}

void testPredictiveHolesGoBackWithTheSegment()
{
    const std::array<double, 16> motion = plannedMotion();
    const auto segmentPlanned = moved(segmentBeforeCut(), motion);
    std::vector<PlateDesign> plates{paranasalPlate(motion)};
    PlateCore::AssignBones(plates, cranialBase(), segmentPlanned);
    require(plates[0].holes[0].bone == PlateBone::Cranial && plates[0].holes[1].bone == PlateBone::Cranial,
            "the holes above the cut are not on the cranial base");
    require(plates[0].holes[2].bone == PlateBone::Segment && plates[0].holes[3].bone == PlateBone::Segment,
            "the holes below the cut are not on the segment");

    const auto holes = PlateCore::PredictHoles(plates, motion, leFortCut(), {0.0, -5.0, -1.0});
    require(holes.size() == 4, "not every hole was predicted");
    // The cranial base does not move: those holes are drilled where the plate has them.
    require(distance(holes[0].preopCenter, holes[0].plannedCenter) < 1e-9, "a cranial hole moved");
    // The segment's holes go back to where that bone is before the cut: the very points they came from.
    require(distance(holes[2].preopCenter, {-10.0, 0.0, 4.0}) < 1e-6 &&
                distance(holes[3].preopCenter, {-10.0, 0.0, -2.0}) < 1e-6,
            "a segment hole did not go back to its pre-operative position");
    require(std::abs(holes[2].preopAxis[1] - 1.0) < 1e-6, "the drilling axis did not rotate back with the segment");
    // Measured before the cut, on the right side of it.
    require(std::abs(holes[1].cutDistanceMm - 5.0) < 0.5 && std::abs(holes[2].cutDistanceMm - 5.0) < 0.5,
            "the distance to the osteotomy is wrong: " + std::to_string(holes[1].cutDistanceMm) + ", " +
                std::to_string(holes[2].cutDistanceMm));
    for (const PredictiveHole& hole : holes)
        require(!hole.wrongSide, "a hole was reported on the wrong side of the cut");

    const PlateCheck clean = PlateCore::Check(plates, holes);
    require(clean.Ok(), "a correct plate raised warnings: " + clean.warnings.join(QStringLiteral(" | ")).toStdString());

    // One hole too close to the cut, and a plate holding the segment with a single screw, are both reported.
    PlateParams strict;
    strict.minCutDistanceMm = 6.0;
    require(!PlateCore::Check(plates, holes, strict).Ok(), "a hole close to the cut was not reported");
    std::vector<PlateDesign> oneScrew = plates;
    oneScrew[0].holes.pop_back();
    oneScrew[0].struts = PlateCore::TemplateStruts(PlateTemplate::Paranasal, 3, 0);
    const PlateCheck weak = PlateCore::Check(oneScrew, PlateCore::PredictHoles(oneScrew, motion, leFortCut(), {0.0, -5.0, -1.0}));
    require(!weak.Ok() && weak.warnings.join(QString()).contains(QStringLiteral("segmento")),
            "a segment held by one screw was not reported");
    // A hole labelled segment but sitting above the cut is on the wrong side.
    std::vector<PlateDesign> mislabelled = plates;
    mislabelled[0].holes[0].bone = PlateBone::Segment;
    const auto confused = PlateCore::PredictHoles(mislabelled, motion, leFortCut(), {0.0, -5.0, -1.0});
    require(confused[0].wrongSide, "a segment hole above the cut was not flagged");
}

void testSleevesSitOnThePreoperativeHoles()
{
    const std::array<double, 16> motion = plannedMotion();
    std::vector<PlateDesign> plates{paranasalPlate(motion)};
    PlateCore::AssignBones(plates, cranialBase(), moved(segmentBeforeCut(), motion));
    const auto holes = PlateCore::PredictHoles(plates, motion, leFortCut(), {0.0, -5.0, -1.0});
    SleeveParams sleeve;
    const auto figures = PlateCore::SleeveFigures(holes, sleeve);
    require(figures.size() == 2 * holes.size(), "each hole needs a sleeve body and its bore");
    for (size_t i = 0; i < holes.size(); ++i) {
        const GuideFigure& body = figures[2 * i];
        const GuideFigure& bore = figures[2 * i + 1];
        require(body.operation == GuideFigureOperation::Add && bore.operation == GuideFigureOperation::Subtract,
                "the sleeve is not an added body with a subtracted bore");
        require(std::abs(bore.diameterMm - sleeve.boreDiameterMm) < 1e-9, "the bore is not the drill's diameter");
        // The bore runs through the pre-operative hole along its axis.
        const Vec3 center{bore.matrix[3], bore.matrix[7], bore.matrix[11]};
        const Vec3 zAxis{bore.matrix[2], bore.matrix[6], bore.matrix[10]};
        require(distance(center, holes[i].preopCenter) < 1e-9, "the bore is not on the pre-operative hole");
        require(std::abs(zAxis[0] * holes[i].preopAxis[0] + zAxis[1] * holes[i].preopAxis[1] +
                         zAxis[2] * holes[i].preopAxis[2] - 1.0) < 1e-9,
                "the bore is not along the pre-operative drilling axis");
        // The body stands off the bone, so it never enters it.
        const Vec3 bodyCenter{body.matrix[3], body.matrix[7], body.matrix[11]};
        const double along = (bodyCenter[0] - holes[i].preopCenter[0]) * holes[i].preopAxis[0] +
                             (bodyCenter[1] - holes[i].preopCenter[1]) * holes[i].preopAxis[1] +
                             (bodyCenter[2] - holes[i].preopCenter[2]) * holes[i].preopAxis[2];
        require(along - 0.5 * body.lengthMm >= 0.5 - 1e-9, "the sleeve body reaches into the bone");
    }
}

// ── The plate ─────────────────────────────────────────────────────────────────
// A paranasal plate built on the bone as `motion` leaves it, the way the app builds it: a wrap that only
// smooths the bone (1 mm closing), and the real bones named under each point.
PlateBuildResult buildOnPlannedBone(const std::array<double, 16>& motion)
{
    const auto segmentPlanned = moved(segmentBeforeCut(), motion);
    const auto cranial = cranialBase();
    WrapParams wrapParams;
    wrapParams.gapClosingMm = 1.0;
    wrapParams.smallestDetailMm = 0.3;
    const WrapResult wrap = WrapCore::Wrap({cranial, segmentPlanned}, wrapParams);
    require(wrap.ok, "the planned bone could not be wrapped: " + wrap.error.toStdString());
    GuideDesignParams prepareParams;
    prepareParams.base.smallestDetailMm = 0.3;
    const GuidePreparation planned = GuideDesignCore::Prepare(wrap.mesh, prepareParams);
    require(planned.ok, "the planned wrap could not be measured");

    std::vector<PlateDesign> plates{paranasalPlate(motion)};
    PlateCore::AssignBones(plates, cranial, segmentPlanned);

    std::vector<std::pair<PlateBone, vtkSmartPointer<vtkStaticCellLocator>>> locators;
    for (const auto& [bone, mesh] : {std::pair{PlateBone::Cranial, cranial}, std::pair{PlateBone::Segment, segmentPlanned}}) {
        auto locator = vtkSmartPointer<vtkStaticCellLocator>::New();
        locator->SetDataSet(mesh);
        locator->BuildLocator();
        locators.emplace_back(bone, locator);
    }
    const PlateBoneQuery boneAt = [locators](const Vec3& p, double* distanceMm) {
        double best = 1e30;
        PlateBone nearest = PlateBone::Unknown;
        for (const auto& [bone, locator] : locators) {
            double closest[3] = {};
            vtkIdType cell = -1;
            int subId = 0;
            double d2 = 0.0;
            locator->FindClosestPoint(p.data(), closest, cell, subId, d2);
            if (std::sqrt(d2) < best) {
                best = std::sqrt(d2);
                nearest = bone;
            }
        }
        if (distanceMm)
            *distanceMm = best;
        return nearest;
    };

    PlateParams params;
    params.smallestDetailMm = 0.15;
    const PlateBuildResult built = PlateCore::Build(planned, plates[0], params, boneAt);
    require(built.ok, "the plate was not built: " + built.error.toStdString());
    require(built.pieces == 1, "the plate did not bridge the cut: " + std::to_string(built.pieces) + " pieces");
    const MeshCheck mesh = MeshRepairCore::Analyze(built.mesh);
    require(mesh.Valid(), "the plate is not a closed mesh: " + mesh.Summary().toStdString());
    require(built.maxFitGapMm < 0.35, "the plate floats over the bone under a hole: " + std::to_string(built.maxFitGapMm));
    return built;
}

void testPlateBridgesTheCutOnThePlannedBone()
{
    const std::array<double, 16> motion = plannedMotion();
    const PlateBuildResult built = buildOnPlannedBone(motion);

    // 1 mm of titanium on the cranial face (y = 0), between the two cranial holes.
    const auto wall = inside(built.mesh, {{-10.0, 0.5, 17.0}, {-10.0, 1.5, 17.0}, {-10.0, -0.4, 17.0}});
    require(wall[0] && !wall[1] && !wall[2], "the plate is not 1 mm thick on the bone");
    // And on the segment, which moved forward: its face is now around y = 3.
    const Vec3 onSegment = PlateCore::TransformPoint(motion, {-10.0, 0.5, 1.5});
    const Vec3 offSegment = PlateCore::TransformPoint(motion, {-10.0, 1.6, 1.5});
    const auto low = inside(built.mesh, {onSegment, offSegment});
    require(low[0] && !low[1], "the plate does not sit on the moved segment");
    // The screw goes through: the hole's centre is empty, while the ring around it is metal.
    const auto hole = inside(built.mesh, {{-10.0, 0.5, 14.0}, {-10.0, 0.5, 16.2}});
    require(!hole[0] && hole[1], "the screw hole is not open inside its ring");
    // The countersink widens the hole at the outer face.
    const auto sink = inside(built.mesh, {{-10.0, 0.95, 15.6}, {-10.0, 0.1, 15.6}});
    require(!sink[0] && sink[1], "there is no countersink at the outer face");
}

// The case the surgeon hit: the maxilla lowered 6 mm and advanced 2 mm opens an 8 mm gap at the cut. The arm
// must cross it in one straight bar from the cranial edge to the segment's edge — not dive into the gap, not
// wrap round the cut faces, not break.
void testPlateBridgesAWideGap()
{
    auto transform = vtkSmartPointer<vtkTransform>::New();
    transform->Translate(0.0, 2.0, -6.0);
    std::array<double, 16> motion{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            motion[static_cast<size_t>(4 * r + c)] = transform->GetMatrix()->GetElement(r, c);
    const PlateBuildResult built = buildOnPlannedBone(motion);
    require(built.bridgedMm > 6.0, "the arm did not bridge the gap: " + std::to_string(built.bridgedMm) + " mm");

    // The cranial base ends at z = 10 (face y = 0); the segment now starts at z = 2 (face y = 2). Halfway,
    // the bar is on the line between the two edges...
    const auto bar = inside(built.mesh, {{-10.0, 1.5, 6.0}, {-10.0, 0.9, 8.5}, {-10.0, 2.2, 3.5}});
    require(bar[0] && bar[1] && bar[2], "there is no straight bar across the gap");
    // ...and nothing hangs down into the gap or runs along the cut faces behind the front.
    const auto gap = inside(built.mesh, {{-10.0, -1.5, 6.0}, {-10.0, -1.0, 9.3}, {-10.0, 1.0, 2.6}, {-10.0, -3.0, 4.0}});
    for (size_t i = 0; i < gap.size(); ++i)
        require(!gap[i], "the plate dips into the gap at probe " + std::to_string(i));
    // Uniform thickness right across: crossing the bar front to back at three heights in the gap, the metal
    // is 1 mm thick (a little more measured horizontally, the bar leans about 14°), and in one piece.
    for (const double z : {4.0, 6.0, 8.0}) {
        std::vector<Vec3> line;
        for (double y = -2.0; y <= 4.0; y += 0.02)
            line.push_back({-10.0, y, z});
        const auto hits = inside(built.mesh, line);
        int metal = 0, runs = 0;
        for (size_t i = 0; i < hits.size(); ++i) {
            metal += hits[i] ? 1 : 0;
            runs += hits[i] && (i == 0 || !hits[i - 1]) ? 1 : 0;
        }
        const double thickness = 0.02 * metal;
        require(runs == 1 && thickness > 0.9 && thickness < 1.25,
                "the bar is not 1 mm thick at z = " + std::to_string(z) + ": " + std::to_string(thickness) + " mm");
    }
}

// Real bone is curved and rough. A long arm around a curved wall (80° of a 25 mm radius, with ±0.25 mm of
// voxel-scale roughness) must follow the surface in one piece, not cut the chord through the bone nor stop and
// bridge at every bump.
void testPlateFollowsACurvedRoughBone()
{
    const double radius = 25.0;
    ImplicitCore::BuildResult cylinder = ImplicitCore::Build(
        ImplicitCore::Cylinder({0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, radius, 15.0), nullptr, 0.3);
    require(cylinder.ok, "the curved bone could not be built");
    vtkPolyData* bone = cylinder.mesh;
    for (vtkIdType id = 0; id < bone->GetNumberOfPoints(); ++id) {
        double p[3] = {};
        bone->GetPoint(id, p);
        const double r = std::hypot(p[0], p[1]);
        if (r < 1e-6 || std::abs(p[2]) > 14.0)
            continue;
        const double bump = 0.25 * std::sin(3.1 * p[0]) * std::cos(2.7 * p[2]); // voxel-scale roughness
        p[0] += bump * p[0] / r;
        p[1] += bump * p[1] / r;
        bone->GetPoints()->SetPoint(id, p);
    }
    WrapParams wrapParams;
    wrapParams.gapClosingMm = 1.0;
    wrapParams.smallestDetailMm = 0.3;
    const WrapResult wrap = WrapCore::Wrap({bone}, wrapParams);
    require(wrap.ok, "the curved bone could not be wrapped");
    GuideDesignParams prepareParams;
    prepareParams.base.smallestDetailMm = 0.3;
    const GuidePreparation planned = GuideDesignCore::Prepare(wrap.mesh, prepareParams);
    require(planned.ok, "the curved wrap could not be measured");

    PlateDesign plate;
    plate.name = QStringLiteral("Placa curva");
    for (const double degrees : {-40.0, 0.0, 40.0}) {
        const double a = degrees * 3.14159265358979323846 / 180.0;
        plate.holes.push_back({{radius * std::sin(a), radius * std::cos(a), 0.0}, {std::sin(a), std::cos(a), 0.0},
                               PlateBone::Segment});
    }
    plate.struts = PlateCore::TemplateStruts(PlateTemplate::Paranasal, 3, 0);
    auto locator = vtkSmartPointer<vtkStaticCellLocator>::New();
    locator->SetDataSet(bone);
    locator->BuildLocator();
    const PlateBoneQuery boneAt = [locator](const Vec3& p, double* distanceMm) {
        double closest[3] = {};
        vtkIdType cell = -1;
        int subId = 0;
        double d2 = 0.0;
        locator->FindClosestPoint(p.data(), closest, cell, subId, d2);
        if (distanceMm)
            *distanceMm = std::sqrt(d2);
        return PlateBone::Segment;
    };
    PlateParams params;
    params.smallestDetailMm = 0.15;
    const PlateBuildResult built = PlateCore::Build(planned, plate, params, boneAt);
    require(built.ok, "the curved plate was not built: " + built.error.toStdString());
    require(built.pieces == 1, "the curved plate came apart: " + std::to_string(built.pieces) + " pieces");
    require(built.bridgedMm < 2.0, "the arm bridged over bone it should follow: " + std::to_string(built.bridgedMm) + " mm");
    for (const double degrees : {-20.0, 20.0}) {
        const double a = degrees * 3.14159265358979323846 / 180.0;
        const Vec3 onPlate{(radius + 0.7) * std::sin(a), (radius + 0.7) * std::cos(a), 0.0};
        // The chord between two holes 40° apart passes 25·cos 20° = 23.5 mm from the axis: inside the bone.
        const double chord = radius * std::cos(20.0 * 3.14159265358979323846 / 180.0) + 0.7;
        const Vec3 onChord{chord * std::sin(a), chord * std::cos(a), 0.0};
        const auto probe = inside(built.mesh, {onPlate, onChord});
        require(probe[0], "the plate does not lie on the curved bone at " + std::to_string(degrees) + "°");
        require(!probe[1], "the plate cuts the chord through the bone at " + std::to_string(degrees) + "°");
    }
}

void testPlatesTravelWithTheProject()
{
    PlateDesign plate = paranasalPlate(plannedMotion());
    plate.side = PlateSide::Left;
    plate.kind = PlateTemplate::LShape;
    plate.holes[1].bone = PlateBone::Cranial;
    plate.holes[2].bone = PlateBone::Segment;
    const QByteArray text = QJsonDocument(PlateCore::ToJson(plate)).toJson();
    const PlateDesign back = PlateCore::FromJson(QJsonDocument::fromJson(text).object());
    require(back.name == plate.name && back.side == PlateSide::Left && back.kind == PlateTemplate::LShape,
            "the plate's name, side or template did not survive");
    require(back.holes.size() == plate.holes.size() && back.struts == plate.struts, "holes or struts were lost");
    require(back.holes[1].bone == PlateBone::Cranial && back.holes[2].bone == PlateBone::Segment,
            "the bone of each hole was lost");
    require(distance(back.holes[3].center, plate.holes[3].center) < 1e-9, "a hole moved on the way");

    PlateParams params;
    params.thicknessMm = 1.3;
    params.minScrewsPerBone = 3;
    const PlateParams paramsBack = PlateCore::ParamsFromJson(PlateCore::ParamsToJson(params));
    require(std::abs(paramsBack.thicknessMm - 1.3) < 1e-12 && paramsBack.minScrewsPerBone == 3,
            "the plate parameters did not survive");
    require(std::abs(PlateCore::ParamsFromJson({}).thicknessMm - PlateParams{}.thicknessMm) < 1e-12,
            "an empty object does not give the defaults");
}
} // namespace

int main()
{
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"the rigid motion is recovered from the meshes", testRigidMotionIsRecoveredFromTheMeshes},
        {"templates join the holes", testTemplatesJoinTheHoles},
        {"predictive holes go back with the segment", testPredictiveHolesGoBackWithTheSegment},
        {"sleeves sit on the pre-operative holes", testSleevesSitOnThePreoperativeHoles},
        {"the plate bridges the cut on the planned bone", testPlateBridgesTheCutOnThePlannedBone},
        {"the plate bridges a wide gap in one straight bar", testPlateBridgesAWideGap},
        {"the plate follows a curved, rough bone", testPlateFollowsACurvedRoughBone},
        {"plates travel with the project", testPlatesTravelWithTheProject},
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
