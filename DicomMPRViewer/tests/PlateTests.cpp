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
#include <set>
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

// The back and front of the plate along a line across it: the smallest and largest `u` with metal on it,
// where `at(u)` walks out of the bone. `found` is false where the line misses the plate altogether.
struct Section
{
    bool found = false;
    double back = 0.0, front = 0.0;
    int runs = 0; // one run is one wall of plate; more than one means the line crossed it twice
    double thickness() const { return front - back; }
};

Section sectionOf(vtkPolyData* mesh, const std::function<Vec3(double)>& at, double from, double to, double step)
{
    std::vector<Vec3> line;
    for (double u = from; u <= to; u += step)
        line.push_back(at(u));
    const std::vector<bool> hits = inside(mesh, line);
    Section section;
    for (size_t i = 0; i < hits.size(); ++i) {
        if (!hits[i])
            continue;
        const double u = from + step * static_cast<double>(i);
        if (!section.found) {
            section.found = true;
            section.back = u;
        }
        section.front = u;
        section.runs += (i == 0 || !hits[i - 1]) ? 1 : 0;
    }
    return section;
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
    std::set<std::pair<int, int>> edges;
    for (const std::vector<int>& strut : shapeL)
        for (size_t i = 0; i + 1 < strut.size(); ++i)
            require(edges.insert(std::minmax(strut[i], strut[i + 1])).second,
                    "the L plate contains a duplicated edge");

    const auto splintless = PlateCore::SplintlessStruts({4, 4, 4, 4});
    require(splintless.size() == 5, "the splintless plate does not have four pillars and one lower union");
    require(splintless[0] == std::vector<int>({0, 1, 2, 3}) &&
                splintless[3] == std::vector<int>({12, 13, 14, 15}),
            "the splintless pillar indices do not follow the clinical marking order");
    require(splintless[4] == std::vector<int>({7, 3, 11, 15}),
            "the splintless lower union does not run lateral-right to lateral-left");
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
// What the app hands PlateCore as the keep-out: the real bone where the plan puts it, and the same bones with
// the segment also where it was before the movement, whose union fills the osteotomy gap. An arm crossing the
// cut is pulled taut over the second one, so it ramps across the step instead of dropping into the cut.
PlateKeepOut keepOutFor(vtkPolyData* cranial, vtkPolyData* segmentPlanned, vtkPolyData* segmentBefore, double detail)
{
    WrapParams tightParams;
    tightParams.gapClosingMm = 0.5;
    tightParams.smallestDetailMm = detail;
    GuideDesignParams prepareParams;
    prepareParams.base.smallestDetailMm = detail;
    PlateKeepOut keepOut;
    const WrapResult tight = WrapCore::Wrap({cranial, segmentPlanned}, tightParams);
    require(tight.ok, "the real bone could not be wrapped: " + tight.error.toStdString());
    const GuidePreparation tightPrepared = GuideDesignCore::Prepare(tight.mesh, prepareParams);
    require(tightPrepared.ok, "the real bone could not be measured");
    keepOut.bone = tightPrepared.wrapField;
    const WrapResult both = WrapCore::Wrap({cranial, segmentPlanned, segmentBefore}, tightParams);
    require(both.ok, "the bone and the gap could not be wrapped: " + both.error.toStdString());
    const GuidePreparation bothPrepared = GuideDesignCore::Prepare(both.mesh, prepareParams);
    require(bothPrepared.ok, "the bone and the gap could not be measured");
    keepOut.boneAndGap = bothPrepared.wrapField;
    return keepOut;
}

// A paranasal plate built on the bone as `motion` leaves it, the way the app builds it: a wrap that
// regularises the bone (3 mm closing), and the real bones named under each point, kept clear of the cut.
PlateBuildResult buildOnPlannedBone(const std::array<double, 16>& motion)
{
    const auto segmentPlanned = moved(segmentBeforeCut(), motion);
    const auto cranial = cranialBase();
    WrapParams wrapParams;
    wrapParams.gapClosingMm = 3.0;
    wrapParams.smallestDetailMm = 0.3;
    const WrapResult wrap = WrapCore::Wrap({cranial, segmentPlanned}, wrapParams);
    require(wrap.ok, "the planned bone could not be wrapped: " + wrap.error.toStdString());
    GuideDesignParams prepareParams;
    prepareParams.base.smallestDetailMm = 0.3;
    const GuidePreparation planned = GuideDesignCore::Prepare(wrap.mesh, prepareParams);
    require(planned.ok, "the planned wrap could not be measured");

    std::vector<PlateDesign> plates{paranasalPlate(motion)};
    PlateCore::AssignBones(plates, cranial, segmentPlanned);

    PlateParams params;
    params.smallestDetailMm = 0.15;
    const PlateBoneQuery boneAt = PlateCore::MakeBoneQuery(
        cranial, segmentPlanned, motion, leFortCut(), params.cutEdgeMarginMm + 0.5 * params.widthMm);
    const PlateBuildResult built =
        PlateCore::Build(planned, plates[0], params, boneAt, nullptr,
                         keepOutFor(cranial, segmentPlanned, segmentBeforeCut(), 0.3));
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
    // The countersink widens the hole at the outer face: 1.6 mm from the hole's axis — outside the 1.05 mm bore,
    // inside the 1.8 mm seat — there is metal near the inner face and none just under the outer one. The faces
    // are measured on the plate itself (it seats up to a voxel off the bone, never into it).
    std::vector<Vec3> across;
    for (double y = -1.0; y <= 3.0; y += 0.01)
        across.push_back({-10.0, y, 17.0});
    const auto section = inside(built.mesh, across);
    double innerFace = 1e9, outerFace = -1e9;
    for (size_t i = 0; i < section.size(); ++i)
        if (section[i]) {
            innerFace = std::min(innerFace, across[i][1]);
            outerFace = std::max(outerFace, across[i][1]);
        }
    require(outerFace - innerFace > 0.85 && outerFace - innerFace < 1.15,
            "the plate is not 1 mm thick between the holes: " + std::to_string(outerFace - innerFace));
    const auto sink = inside(built.mesh, {{-10.0, outerFace - 0.08, 15.6}, {-10.0, innerFace + 0.15, 15.6}});
    require(!sink[0] && sink[1], "there is no countersink at the outer face");
}

// The case the surgeon hit: the maxilla lowered 6 mm and advanced 2 mm opens an 8 mm gap at the cut. The arm
// crosses it as one ramp between the two anterior faces — the shape of a real patient-specific implant — with
// nothing behind those faces and nothing standing out in front of the advanced one.
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

    // Nothing hangs behind the anterior faces or runs along the cut surfaces.
    const auto gap = inside(built.mesh, {{-10.0, -1.5, 6.0}, {-10.0, -1.0, 9.3},
                                         {-10.0, -3.0, 4.0}});
    for (size_t i = 0; i < gap.size(); ++i)
        require(!gap[i], "the plate dips into the gap at probe " + std::to_string(i));

    // Across the gap the plate is one wall of titanium, 1 mm thick, whose front travels from the cranial face
    // (y = 0) to the advanced one (y = 2) without ever going beyond it and without a step: over any 2 mm of
    // height it moves less than 1.5 mm, so a surgeon could bend it.
    double previousFront = 0.0;
    bool first = true;
    for (double z = 3.5; z <= 9.0; z += 0.5) {
        const Section across =
            sectionOf(built.mesh, [z](double y) { return Vec3{-10.0, y, z}; }, -2.0, 5.0, 0.02);
        require(across.found, "the ramp is interrupted at z = " + std::to_string(z));
        require(across.runs == 1 && across.thickness() > 0.9 && across.thickness() < 1.25,
                "the ramp is not 1 mm thick at z = " + std::to_string(z) + ": " +
                    std::to_string(across.thickness()) + " mm");
        require(across.front < 3.2, "the ramp stands out in front of the advanced face at z = " + std::to_string(z));
        require(across.back > -0.35, "the ramp is behind the cranial face at z = " + std::to_string(z));
        // Going up, from the advanced face back to the cranial one: always retreating, never in a step.
        if (!first)
            require(across.front - previousFront < 0.05 && across.front - previousFront > -0.75,
                    "the ramp has a step at z = " + std::to_string(z) + ": " +
                        std::to_string(across.front - previousFront) + " mm in half a millimetre");
        previousFront = across.front;
        first = false;
    }
}

// Real bone is curved and rough. A long arm around a curved wall (80° of a 25 mm radius, with ±0.25 mm of
// voxel-scale roughness) must follow the surface in one piece, not cut the chord through the bone nor stop and
// bridge at every bump.
// The case of the surgeon's second report. The paranasal wall faces forward AND outward (here 30°); the plan
// advances the segment 8 mm straight forward and lowers it 2 mm. Near the cut the bone turns into the cut face:
// an arm that followed it went down into the gap and collided with the other bone. The plate must seat on the
// anterior faces, ramp between them across the osteotomy and leave the space behind it empty.
void testPlateBridgesALargeAdvancementFlat()
{
    auto turn = vtkSmartPointer<vtkTransform>::New();
    turn->RotateZ(-30.0);
    std::array<double, 16> rotation{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            rotation[static_cast<size_t>(4 * r + c)] = turn->GetMatrix()->GetElement(r, c);
    const std::array<double, 16> motion{1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 8.0, 0.0, 0.0, 1.0, -2.0, 0.0, 0.0, 0.0, 1.0};
    const auto cranial = moved(cranialBase(), rotation);
    const auto segmentBefore = moved(segmentBeforeCut(), rotation);
    const auto segmentPlanned = moved(segmentBefore, motion);
    const OsteotomyPath cut = OsteotomyCore::TransformPath(leFortCut(), turn->GetMatrix());
    const auto place = [&](const Vec3& p, bool onSegment) {
        const Vec3 turned = PlateCore::TransformPoint(rotation, p);
        return onSegment ? PlateCore::TransformPoint(motion, turned) : turned;
    };
    const Vec3 facing = PlateCore::TransformVector(rotation, {0.0, 1.0, 0.0});

    WrapParams wrapParams;
    wrapParams.gapClosingMm = 1.0;
    wrapParams.smallestDetailMm = 0.3;
    const WrapResult wrap = WrapCore::Wrap({cranial, segmentPlanned}, wrapParams);
    require(wrap.ok, "the planned bone could not be wrapped");
    GuideDesignParams prepareParams;
    prepareParams.base.smallestDetailMm = 0.3;
    const GuidePreparation planned = GuideDesignCore::Prepare(wrap.mesh, prepareParams);
    require(planned.ok, "the planned wrap could not be measured");

    PlateDesign plate;
    plate.name = QStringLiteral("Placa");
    for (const auto& [p, onSegment] : {std::pair{Vec3{-10.0, 0.0, 20.0}, false}, std::pair{Vec3{-10.0, 0.0, 14.0}, false},
                                       std::pair{Vec3{-10.0, 0.0, 4.0}, true}, std::pair{Vec3{-10.0, 0.0, -2.0}, true}})
        plate.holes.push_back({place(p, onSegment), facing, PlateBone::Unknown});
    plate.struts = PlateCore::TemplateStruts(PlateTemplate::Paranasal, 4, 0);
    std::vector<PlateDesign> plates{plate};
    PlateCore::AssignBones(plates, cranial, segmentPlanned);
    PlateParams params;
    params.smallestDetailMm = 0.15;
    const PlateBuildResult built = PlateCore::Build(planned, plates[0], params,
                                                    PlateCore::MakeBoneQuery(
                                                        cranial, segmentPlanned, motion, cut,
                                                        params.cutEdgeMarginMm + 0.5 * params.widthMm),
                                                    nullptr, keepOutFor(cranial, segmentPlanned, segmentBefore, 0.3));
    require(built.ok, "the plate was not built: " + built.error.toStdString());
    require(built.pieces == 1, "the plate came apart: " + std::to_string(built.pieces) + " pieces");
    require(built.bridgedMm > 5.0, "the arm did not bridge the advancement: " + std::to_string(built.bridgedMm) + " mm");

    // In the arm's plane (a section through its line), in wall coordinates: u runs forward out of the cranial
    // wall, z up. The cranial wall's front is u = 0 above z = 10; the segment's front is 8 mm ahead below z = 6.
    const Vec3 origin = place({-10.0, 0.0, 0.0}, false);
    const Vec3 forward{0.0, 1.0, 0.0};
    const auto at = [&](double u, double z) { return Vec3{origin[0] + forward[0] * u, origin[1] + forward[1] * u, z}; };
    // The gap stays empty: behind the fronts, between the pieces.
    for (const auto& [u, z] : {std::pair{-1.0, 8.0}, std::pair{-2.0, 9.0}, std::pair{2.0, 5.5}, std::pair{-1.0, 6.5}})
        require(!inside(built.mesh, {at(u, z)})[0], "the plate goes into the gap at u = " + std::to_string(u) +
                                                           ", z = " + std::to_string(z));
    // One ramp between the two faces: at every height it is one wall of titanium, its front moves forward
    // steadily from the cranial face towards the advanced one and never passes it.
    // The line is oblique to this 30° wall, so read the two faces off the bones themselves along the very
    // same line rather than from the millimetres of the plan.
    const double advancedFace = sectionOf(segmentPlanned, [&](double u) { return at(u, 0.0); }, -5.0, 15.0, 0.02).front;
    const double cranialFace = sectionOf(cranial, [&](double u) { return at(u, 14.0); }, -5.0, 15.0, 0.02).front;
    // The arms stop 5 mm short of the cut on each bone, so the ramp runs from about z = 2 to about z = 11;
    // higher than that the plate is seated on the cranium and the section crosses it twice as it lifts off.
    double previousFront = 1e9, previousMoved = 1e9;
    for (double z = 3.0; z <= 11.0; z += 0.5) {
        const Section across = sectionOf(built.mesh, [&](double u) { return at(u, z); }, -2.0, 11.0, 0.02);
        require(across.found, "the ramp is interrupted at z = " + std::to_string(z));
        require(across.runs == 1, "the ramp doubles back on itself at z = " + std::to_string(z));
        require(across.front < advancedFace + 2.0, // the line crosses the 1 mm plate obliquely
                "the ramp stands out in front of the advanced face at z = " + std::to_string(z) + ": " +
                    std::to_string(across.front) + " against " + std::to_string(advancedFace));
        require(across.back > cranialFace - 0.5, "the ramp is behind the cranial face at z = " + std::to_string(z));
        // No step anywhere. A right-angled dogleg travels the whole 8 mm of the advancement between two
        // neighbouring heights and turns twice; a ramp moves a little at a time and its slope changes slowly.
        const double moved = across.front - previousFront;
        if (previousFront < 1e8) {
            require(std::abs(moved) < 2.5, "the ramp has a step at z = " + std::to_string(z) + ": " +
                                               std::to_string(moved) + " mm in half a millimetre");
            if (previousMoved < 1e8)
                require(std::abs(moved - previousMoved) < 1.0,
                        "the ramp turns a corner at z = " + std::to_string(z) + ": " +
                            std::to_string(moved - previousMoved));
            previousMoved = moved;
        }
        previousFront = across.front;
    }
    // And it really did cross: above the ramp the plate is seated on the cranial face again.
    const Section seated = sectionOf(built.mesh, [&](double u) { return at(u, 13.0); }, -2.0, 11.0, 0.02);
    require(seated.found && seated.back < cranialFace + 1.2, // the line crosses the wall obliquely
            "the arm did not come back onto the cranium: " + std::to_string(seated.back));
}

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

    plate.kind = PlateTemplate::Splintless;
    const PlateDesign splintlessBack = PlateCore::FromJson(PlateCore::ToJson(plate));
    require(splintlessBack.kind == PlateTemplate::Splintless, "the splintless design type did not survive");

    PlateParams params;
    params.thicknessMm = 1.3;
    params.cutEdgeMarginMm = 4.0;
    params.minScrewsPerBone = 3;
    const PlateParams paramsBack = PlateCore::ParamsFromJson(PlateCore::ParamsToJson(params));
    require(std::abs(paramsBack.thicknessMm - 1.3) < 1e-12 &&
                std::abs(paramsBack.cutEdgeMarginMm - 4.0) < 1e-12 && paramsBack.minScrewsPerBone == 3,
            "the plate parameters did not survive");
    require(std::abs(PlateCore::ParamsFromJson(QJsonObject{{QStringLiteral("cutEdgeMarginMm"), 2.0}}).cutEdgeMarginMm -
                         3.5) < 1e-12,
            "a saved plate kept an unsafe legacy margin at the osteotomy");
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
        {"the plate bends at a wide osteotomy gap", testPlateBridgesAWideGap},
        {"the plate steps across a large advancement", testPlateBridgesALargeAdvancementFlat},
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
