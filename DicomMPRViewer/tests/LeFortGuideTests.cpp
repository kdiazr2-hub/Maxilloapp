#include "LeFortGuideCore.h"
#include "GuideEngraveCore.h"
#include "LeFortMotionCore.h"

#include "GuideDesignCore.h"
#include "MeshRepairCore.h"
#include "OsteotomyCore.h"
#include "PlateCore.h"
#include "SplintTestGeometry.h"
#include "WrapCore.h"

#include <vtkAppendPolyData.h>
#include <vtkImplicitPolyDataDistance.h>
#include <vtkPolyData.h>
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

// The anterior maxilla before the cut, face towards +y. The cranial base has a piriform aperture at the
// midline (|x| < 6, from the cut up to z = 20): at the level of the cut there is no anterior wall there.
std::vector<vtkSmartPointer<vtkPolyData>> cranialPieces()
{
    return {boxMesh({-25.0, -6.0, -10.0, 0.0, 10.0, 30.0}, false, false),
            boxMesh({6.0, 25.0, -10.0, 0.0, 10.0, 30.0}, false, false),
            boxMesh({-6.0, 6.0, -10.0, 0.0, 20.0, 30.0}, false, false)};
}
vtkSmartPointer<vtkPolyData> segmentBeforeCut() { return boxMesh({-25.0, 25.0, -10.0, 0.0, -10.0, 8.0}, false, false); }

OsteotomyPath leFortCut()
{
    return OsteotomyCore::LeFortPath({{{-10.0, 5.0, 9.0}, {10.0, 5.0, 9.0}, {-20.0, -5.0, 9.0}, {20.0, -5.0, 9.0}}});
}

// Lowered 6 mm and advanced 2 mm: the motion only changes where the segment's holes sit on the plates.
std::array<double, 16> plannedMotion()
{
    return {1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 2.0, 0.0, 0.0, 1.0, -6.0, 0.0, 0.0, 0.0, 1.0};
}

vtkSmartPointer<vtkPolyData> merged(const std::vector<vtkSmartPointer<vtkPolyData>>& meshes)
{
    auto append = vtkSmartPointer<vtkAppendPolyData>::New();
    for (const auto& mesh : meshes)
        append->AddInputData(mesh);
    append->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(append->GetOutput());
    return out;
}

// Two paranasal plates, one each side of the aperture, holes clicked top to bottom on the planned bone.
std::vector<PredictiveHole> predictiveHoles()
{
    const auto motion = plannedMotion();
    std::vector<PlateDesign> plates;
    for (const double x : {-12.0, 12.0}) {
        PlateDesign plate;
        plate.name = x < 0 ? QStringLiteral("Placa derecha") : QStringLiteral("Placa izquierda");
        for (const Vec3& p : {Vec3{x, 0.0, 20.0}, Vec3{x, 0.0, 14.0}})
            plate.holes.push_back({p, {0.0, 1.0, 0.0}, PlateBone::Unknown});
        for (const Vec3& p : {Vec3{x, 0.0, 4.0}, Vec3{x, 0.0, -2.0}})
            plate.holes.push_back({PlateCore::TransformPoint(motion, p), {0.0, 1.0, 0.0}, PlateBone::Unknown});
        plate.struts = PlateCore::TemplateStruts(PlateTemplate::Paranasal, 4, 0);
        plates.push_back(plate);
    }
    auto segmentPlanned = vtkSmartPointer<vtkPolyData>::New();
    {
        auto transform = vtkSmartPointer<vtkTransform>::New();
        transform->SetMatrix(motion.data());
        auto filter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
        filter->SetInputData(segmentBeforeCut());
        filter->SetTransform(transform);
        filter->Update();
        segmentPlanned->DeepCopy(filter->GetOutput());
    }
    PlateCore::AssignBones(plates, merged(cranialPieces()), segmentPlanned);
    return PlateCore::PredictHoles(plates, motion, leFortCut(), {0.0, -5.0, -1.0});
}

struct Prepared
{
    WrapResult wrap;
    GuidePreparation preparation;
    GuideDesignParams design;
};

// The anterior nasal spine: a 3 mm-wide spur at the midline of the aperture's floor, 4 mm proud of the face.
vtkSmartPointer<vtkPolyData> nasalSpine() { return boxMesh({-1.5, 1.5, 0.0, 4.0, 5.0, 8.0}, false, false); }

Prepared preoperativeBone(bool withSpine = false)
{
    Prepared out;
    std::vector<vtkPolyData*> meshes;
    const auto cranial = cranialPieces();
    const auto segment = segmentBeforeCut();
    const auto spine = nasalSpine();
    for (const auto& mesh : cranial)
        meshes.push_back(mesh);
    meshes.push_back(segment);
    if (withSpine)
        meshes.push_back(spine);
    WrapParams wrapParams;
    wrapParams.gapClosingMm = 1.5;
    wrapParams.smallestDetailMm = 0.4;
    out.wrap = WrapCore::Wrap(meshes, wrapParams);
    require(out.wrap.ok, "the bone before the cut could not be wrapped");
    out.design.base.smallestDetailMm = 0.4;
    out.design.slot.smallestDetailMm = 0.4;
    // The bone itself, so nothing built on the wrap can end up inside it. The wrap is quantised to voxel
    // centres and stands up to a voxel inside the surface it wraps.
    out.design.bone = ImplicitCore::BakeMeshField(meshes, 0.4, 8.0);
    require(out.design.bone != nullptr, "the bone field could not be baked");
    out.preparation = GuideDesignCore::Prepare(out.wrap.mesh, out.design);
    require(out.preparation.ok, "the envelope could not be measured");
    return out;
}

void testTheGuideIsLaidOutFromThePlan()
{
    const auto holes = predictiveHoles();
    require(holes.size() == 8, "the plates did not give eight predictive holes");
    const Prepared bone = preoperativeBone();
    const LeFortGuideLayout layout = LeFortGuideCore::Layout(bone.preparation, bone.wrap.mesh, leFortCut(), holes);
    require(layout.ok, "the guide was not laid out: " + layout.error.toStdString());

    // The band follows the cut on the anterior wall, and dips below the aperture at the midline.
    require(!layout.cutLine.empty(), "the slit found no anterior wall");
    for (const Vec3& p : layout.cutLine)
        require(std::abs(OsteotomyCore::PathField(leFortCut(), p)) < 1.0 && std::abs(p[0]) > 5.0,
                "a slit point is off the cut or inside the aperture");
    require(layout.dippedBins > 0, "the band did not go round the aperture");

    // Every predictive hole sits inside the painted support.
    for (const PredictiveHole& hole : holes) {
        bool covered = false;
        for (const GuideBrushStroke& dab : layout.paint) {
            const double d = std::hypot(std::hypot(dab.center[0] - hole.preopCenter[0], dab.center[1] - hole.preopCenter[1]),
                                        dab.center[2] - hole.preopCenter[2]);
            covered = covered || d + 2.1 <= dab.radiusMm;
        }
        require(covered, "a predictive hole's sleeve is not on the guide");
    }

    // The slit comes in pieces, with a bridge at the midline: no piece crosses it.
    require(layout.slotPlan.size() >= 2, "the slit was not split by bridges");
    for (const GuideSlot& slot : layout.slotPlan)
        require(slot.hasExtent && (std::min(slot.start[0], slot.end[0]) > 0.0 || std::max(slot.start[0], slot.end[0]) < 0.0),
                "a slit piece runs across the midline");

    // The guide is screwed down before anything is drilled or cut, as both published protocols do. With plates
    // those positioning screws go on the cranial side only: the maxilla is about to move away.
    require(layout.fixation.size() == 2, "the guide has no positioning screws: " +
                                             std::to_string(layout.fixation.size()));
    for (const GuideFixationHole& screw : layout.fixation) {
        require(screw.center[2] > 9.0, "a positioning screw was placed below the cut");
        for (const PredictiveHole& hole : holes) {
            const double dx = screw.center[0] - hole.preopCenter[0], dy = screw.center[1] - hole.preopCenter[1],
                         dz = screw.center[2] - hole.preopCenter[2];
            require(std::sqrt(dx * dx + dy * dy + dz * dz) > 4.0, "a positioning screw lands on a plate hole");
        }
    }
}

// A drill sleeve whose pad touches nothing else is a ring of guide floating over the bone: the build reports
// several pieces and the surgeon gets a hole with no material under it (user's report, 2026-09-20). Whatever
// the plates ask for, the painted region has to come out as one patch.
void testEverySleeveHasGuideUnderIt()
{
    const Prepared bone = preoperativeBone();
    // A plate hole far out on the lateral wall, well beyond the band round the cut.
    std::vector<PredictiveHole> holes = predictiveHoles();
    PredictiveHole stray = holes.front();
    stray.preopCenter = {-23.0, 0.0, 26.0};
    stray.plannedCenter = stray.preopCenter;
    stray.preopAxis = {0.0, 1.0, 0.0};
    stray.bone = PlateBone::Cranial;
    holes.push_back(stray);

    const LeFortGuideLayout layout = LeFortGuideCore::Layout(bone.preparation, bone.wrap.mesh, leFortCut(), holes);
    require(layout.ok, layout.error.toStdString());

    // One patch: every dab reaches every other through overlaps, the stray hole's pad included.
    std::vector<int> parent(layout.paint.size());
    for (size_t i = 0; i < parent.size(); ++i)
        parent[i] = static_cast<int>(i);
    const std::function<int(int)> root = [&parent](int i) {
        while (parent[static_cast<size_t>(i)] != i)
            i = parent[static_cast<size_t>(i)] = parent[static_cast<size_t>(parent[static_cast<size_t>(i)])];
        return i;
    };
    const auto span = [](const Vec3& a, const Vec3& b) {
        return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
    };
    for (size_t i = 0; i < layout.paint.size(); ++i)
        for (size_t j = i + 1; j < layout.paint.size(); ++j)
            if (span(layout.paint[i].center, layout.paint[j].center) <=
                layout.paint[i].radiusMm + layout.paint[j].radiusMm - 0.5)
                parent[static_cast<size_t>(root(static_cast<int>(i)))] = root(static_cast<int>(j));
    std::set<int> components;
    for (size_t i = 0; i < layout.paint.size(); ++i)
        components.insert(root(static_cast<int>(i)));
    require(components.size() == 1,
            "the painted guide came out in " + std::to_string(components.size()) + " patches");

    // And the stray hole really is painted over.
    bool covered = false;
    for (const GuideBrushStroke& dab : layout.paint)
        covered = covered || span(dab.center, stray.preopCenter) < dab.radiusMm;
    require(covered, "the stray sleeve has no guide under it");
}

void testTheGuideIsOnePieceWithAnOpenSlit()
{
    const auto holes = predictiveHoles();
    const Prepared bone = preoperativeBone();
    const LeFortGuideLayout layout = LeFortGuideCore::Layout(bone.preparation, bone.wrap.mesh, leFortCut(), holes);
    require(layout.ok, layout.error.toStdString());
    const GuideRegion region =
        GuideBaseCore::MakeBrushRegion(bone.preparation.wrapField, layout.paint, bone.design.base);
    require(region.valid, "the laid-out region is not valid: " + region.error.toStdString());
    const GuideDesignResult guide = GuideDesignCore::Build(bone.preparation, region, layout.slotPlan, layout.fixation,
                                                           PlateCore::SleeveFigures(holes), bone.design);
    require(guide.ok, "the guide was not built: " + guide.error.toStdString());
    require(guide.pieces == 1, "the guide came apart: " + std::to_string(guide.pieces) + " pieces");
    require(MeshRepairCore::Analyze(guide.mesh).Valid(), "the guide is not a closed mesh");

    // And none of it is inside the bone. A guide that sinks into the maxilla cannot be seated (user's report,
    // 2026-09-21: "la guía metida dentro del lefort").
    {
        double deepest = 0.0;
        int inside = 0;
        for (vtkIdType id = 0; id < guide.mesh->GetNumberOfPoints(); ++id) {
            double q[3] = {};
            guide.mesh->GetPoint(id, q);
            const double d = bone.design.bone->At({q[0], q[1], q[2]});
            deepest = std::min(deepest, d);
            inside += d < -0.25 ? 1 : 0;
        }
        require(inside == 0, "the guide reaches into the bone: " + std::to_string(inside) + " vertices, deepest " +
                                 std::to_string(-deepest) + " mm");
    }

    // The slit is open on the planned cut (z = 9), through the guide's wall, everywhere the band actually
    // reaches the cut; 3 mm above it the wall is solid. A slit piece also spans the stretches where the band
    // dips below the aperture, and there the guide has no material on the cut to open. The band is 8 mm wide,
    // so 2 mm above the cut is well inside it wherever it runs along the cut.
    auto distance = vtkSmartPointer<vtkImplicitPolyDataDistance>::New();
    distance->SetInput(guide.mesh);
    int probed = 0;
    for (const GuideSlot& slot : layout.slotPlan) {
        const double from = std::min(slot.start[0], slot.end[0]);
        const double to = std::max(slot.start[0], slot.end[0]);
        for (const auto& onCut : layout.cutLine) {
            if (onCut[0] < from + 1.0 || onCut[0] > to - 1.0)
                continue;
            const double x = onCut[0];
            // Every slot keeps `edgeMarginMm` (2 mm) of material to the guide's edge, so the guide holds together;
            // since the band stops at the piriform rim (no guide in the nose) that edge is on the cut there.
            if (distance->EvaluateFunction(x, 1.0, 11.0) >= 0.0 || distance->EvaluateFunction(x - 2.5, 1.0, 11.0) >= 0.0 ||
                distance->EvaluateFunction(x + 2.5, 1.0, 11.0) >= 0.0)
                continue; // no guide wall above the cut here, or the guide's edge is near (a hole, the end of the band)
            for (const double y : {0.6, 1.2})
                require(distance->EvaluateFunction(x, y, 9.0) > 0.0,
                        "the slit is closed at x = " + std::to_string(x) + ", y = " + std::to_string(y));
            ++probed;
        }
    }
    require(probed >= 5, "the slit was hardly opened anywhere on the cut: " + std::to_string(probed) + " points");
    // And the bridges hold: at the midline bridge of each side the wall is solid on the cut.
    for (const double x : {15.0, -15.0})
        require(distance->EvaluateFunction(x, 1.0, 9.0) < 0.0, "there is no bridge across the slit at x = " +
                                                                  std::to_string(x));
}

void testWorksBeforePlatesAndRefusesWithoutEnvelope()
{
    const Prepared bone = preoperativeBone();
    const LeFortGuideLayout noPlates = LeFortGuideCore::Layout(bone.preparation, bone.wrap.mesh, leFortCut(), {});
    require(noPlates.ok, "the pre-operative guide still depends on plates: " + noPlates.error.toStdString());
    require(!noPlates.paint.empty() && !noPlates.slotPlan.empty() && noPlates.fixation.size() == 4,
            "the plate-free guide lacks its band, slit or fixation screws");
    const LeFortGuideLayout noEnvelope = LeFortGuideCore::Layout({}, nullptr, leFortCut(), predictiveHoles());
    require(!noEnvelope.ok && !noEnvelope.error.isEmpty(), "a guide was laid out without an envelope");
}

void testTheGuideFindsTheAnteriorWallWhenTheSweepAxisIsReversed()
{
    const Prepared bone = preoperativeBone();
    OsteotomyPath reversed = leFortCut();
    for (double& value : reversed.depthAxis)
        value = -value;
    const LeFortGuideLayout layout = LeFortGuideCore::Layout(bone.preparation, bone.wrap.mesh, reversed, {});
    require(layout.ok, "the reversed sweep axis prevented guide layout: " + layout.error.toStdString());
    require(!layout.paint.empty(), "the reversed sweep axis produced no support band");

    double meanDepth = 0.0;
    for (const GuideBrushStroke& dab : layout.paint)
        meanDepth += dab.center[1] / static_cast<double>(layout.paint.size());
    require(meanDepth > -2.0, "the guide was laid out on the posterior wall instead of the anterior maxilla");

    const GuideRegion region =
        GuideBaseCore::MakeBrushRegion(bone.preparation.wrapField, layout.paint, bone.design.base);
    const GuideDesignResult guide = GuideDesignCore::Build(bone.preparation, region, layout.slotPlan,
                                                            layout.fixation, {}, bone.design);
    require(guide.ok && guide.pieces == 1,
            "the anterior guide from a reversed sweep axis is not one piece");
}

void testTheGuideIgnoresADistantSkullSurfaceInSavedProjects()
{
    const auto cranial = cranialPieces();
    const auto segment = segmentBeforeCut();
    const auto distantSkull = boxMesh({-30.0, 30.0, 35.0, 45.0, -10.0, 30.0}, false, false);
    std::vector<vtkPolyData*> meshes;
    for (const auto& mesh : cranial)
        meshes.push_back(mesh);
    meshes.push_back(segment);
    meshes.push_back(distantSkull);

    WrapParams wrapParams;
    wrapParams.gapClosingMm = 1.5;
    wrapParams.smallestDetailMm = 0.4;
    const WrapResult wrap = WrapCore::Wrap(meshes, wrapParams);
    require(wrap.ok, "the saved-project envelope with distant skull could not be wrapped");
    GuideDesignParams design;
    design.base.smallestDetailMm = 0.4;
    design.slot.smallestDetailMm = 0.4;
    const GuidePreparation prepared = GuideDesignCore::Prepare(wrap.mesh, design);
    require(prepared.ok, "the saved-project envelope could not be prepared");

    const LeFortGuideLayout layout = LeFortGuideCore::Layout(prepared, wrap.mesh, leFortCut(), {});
    require(layout.ok && !layout.paint.empty(), "the saved-project guide could not be laid out");
    for (const GuideBrushStroke& dab : layout.paint)
        require(dab.center[1] < 20.0, "the guide jumped from the osteotomy to a distant skull surface");
}
// ── The band an impaction takes out ─────────────────────────────────────────

// A rise of the whole segment, or a roll about the antero-posterior axis at cut height (the rise is −0.1·x:
// the right side goes up, the left down).
LeFortBandProfile bandFor(double riseMm)
{
    const std::array<double, 16> m{1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, riseMm, 0.0, 0.0, 0.0, 1.0};
    const LeFortBandProfile band = LeFortMotionCore::Band(leFortCut(), m);
    require(band.ok && !band.spans.empty(), "the band could not be computed for the test");
    return band;
}
LeFortBandProfile rolledBand()
{
    const double s = 0.1, c = std::sqrt(1.0 - s * s);
    // Rotation about +Y through (0, 0, 9): z' = −s·x + c·(z − 9) + 9, x' = c·x + s·(z − 9).
    const std::array<double, 16> m{c, 0.0, s, -9.0 * s, 0.0, 1.0, 0.0, 0.0, -s, 0.0, c, 9.0 - 9.0 * c, 0.0, 0.0, 0.0, 1.0};
    const LeFortBandProfile band = LeFortMotionCore::Band(leFortCut(), m);
    require(band.ok && !band.spans.empty(), "the rolled band could not be computed for the test");
    return band;
}

bool isUpper(const GuideSlot& slot, const LeFortBandProfile& band)
{
    return !slot.path.points.empty() && !band.upperCut.points.empty() &&
           std::abs(slot.path.points.front()[2] - band.upperCut.points.front()[2]) < 1e-6 &&
           std::abs(slot.path.points.back()[2] - band.upperCut.points.back()[2]) < 1e-6;
}
std::vector<GuideSlot> upperSlots(const LeFortGuideLayout& layout, const LeFortBandProfile& band)
{
    std::vector<GuideSlot> out;
    for (const GuideSlot& slot : layout.slotPlan)
        if (isUpper(slot, band))
            out.push_back(slot);
    return out;
}

// spec §Behaviour (mixed) and §Acceptance 2: the second slit follows the band's upper edge and exists only where
// the segment rises — here the right side up to x = −5.
void testABandAddsAnUpperSlitOnlyWhereTheSegmentRises()
{
    const Prepared bone = preoperativeBone();
    const LeFortBandProfile band = rolledBand();
    const LeFortGuideLayout layout =
        LeFortGuideCore::Layout(bone.preparation, bone.wrap.mesh, leFortCut(), predictiveHoles(), {}, &band);
    require(layout.ok, layout.error.toStdString());
    const std::vector<GuideSlot> upper = upperSlots(layout, band);
    require(!upper.empty(), "there is no slit along the band's upper edge");
    for (const GuideSlot& slot : upper)
        require(slot.hasExtent && std::max(slot.start[0], slot.end[0]) <= -5.0 + 0.5,
                "the upper slit runs where the segment does not rise: up to x = " +
                    std::to_string(std::max(slot.start[0], slot.end[0])));
    // The Le Fort slit is still there, both sides.
    bool right = false, left = false;
    for (const GuideSlot& slot : layout.slotPlan) {
        if (isUpper(slot, band))
            continue;
        right = right || std::min(slot.start[0], slot.end[0]) < -5.0;
        left = left || std::max(slot.start[0], slot.end[0]) > 5.0;
    }
    require(right && left, "the Le Fort slit lost a side");
    require(layout.report.contains(QStringLiteral("franja")), "the report does not mention the band");
}

// The upper slit is broken by the same bridges as the Le Fort slit, so the strip between them stays held.
void testTheUpperSlitSharesTheBridges()
{
    const Prepared bone = preoperativeBone();
    const LeFortBandProfile band = bandFor(3.0);
    const LeFortGuideLayout layout =
        LeFortGuideCore::Layout(bone.preparation, bone.wrap.mesh, leFortCut(), predictiveHoles(), {}, &band);
    require(layout.ok, layout.error.toStdString());
    const std::vector<GuideSlot> upper = upperSlots(layout, band);
    require(upper.size() >= 2, "the upper slit was not split by bridges");
    for (const GuideSlot& top : upper) {
        const double from = std::min(top.start[0], top.end[0]), to = std::max(top.start[0], top.end[0]);
        require(from > 0.0 || to < 0.0, "an upper slit piece runs across the midline");
        bool underALowerPiece = false;
        for (const GuideSlot& low : layout.slotPlan) {
            if (isUpper(low, band))
                continue;
            const double a = std::min(low.start[0], low.end[0]), b = std::max(low.start[0], low.end[0]);
            underALowerPiece = underALowerPiece || (from >= a - 0.01 && to <= b + 0.01);
        }
        require(underALowerPiece, "an upper slit piece crosses a bridge of the Le Fort slit");
    }
}

// spec §Acceptance 12: with the band the guide is still one piece, both slits open, the strip between them held.
void testTheGuideWithABandIsOnePieceWithBothSlitsOpen()
{
    const auto holes = predictiveHoles();
    const Prepared bone = preoperativeBone();
    const LeFortBandProfile band = bandFor(3.0); // upper edge at z = 12
    const LeFortGuideLayout layout =
        LeFortGuideCore::Layout(bone.preparation, bone.wrap.mesh, leFortCut(), holes, {}, &band);
    require(layout.ok, layout.error.toStdString());
    const GuideRegion region =
        GuideBaseCore::MakeBrushRegion(bone.preparation.wrapField, layout.paint, bone.design.base);
    require(region.valid, "the laid-out region is not valid: " + region.error.toStdString());
    const GuideDesignResult guide = GuideDesignCore::Build(bone.preparation, region, layout.slotPlan, layout.fixation,
                                                           PlateCore::SleeveFigures(holes), bone.design);
    require(guide.ok, "the guide was not built: " + guide.error.toStdString());
    require(guide.pieces == 1, "the guide with a band came apart: " + std::to_string(guide.pieces) + " pieces");

    auto distance = vtkSmartPointer<vtkImplicitPolyDataDistance>::New();
    distance->SetInput(guide.mesh);
    int probed = 0;
    for (const GuideSlot& slot : upperSlots(layout, band)) {
        const double from = std::min(slot.start[0], slot.end[0]) + 1.0;
        const double to = std::max(slot.start[0], slot.end[0]) - 1.0;
        for (double x = from; x <= to; x += 1.0) {
            if (distance->EvaluateFunction(x, 1.0, 14.0) >= 0.0)
                continue; // no guide wall above the band here
            require(distance->EvaluateFunction(x, 1.0, 12.0) > 0.0,
                    "the upper slit is closed at x = " + std::to_string(x));
            ++probed;
        }
    }
    require(probed >= 5, "the upper slit was hardly opened anywhere: " + std::to_string(probed) + " points");
}

// The band is held on both sides: the paint reaches 2 mm past its upper edge where it runs on the wall, the
// positioning screws sit above it, and no lattice cell opens next to either slit.
void testATallBandIsCoveredAndKeptClear()
{
    const Prepared bone = preoperativeBone();
    const LeFortBandProfile band = bandFor(7.0); // upper edge at z = 16
    LeFortGuideParams params;
    const LeFortGuideLayout layout =
        LeFortGuideCore::Layout(bone.preparation, bone.wrap.mesh, leFortCut(), predictiveHoles(), params, &band);
    require(layout.ok, layout.error.toStdString());
    for (const double x : {-15.0, 15.0}) {
        const Vec3 above{x, 0.0, 18.0};
        bool covered = false;
        for (const GuideBrushStroke& dab : layout.paint)
            covered = covered || std::hypot(std::hypot(dab.center[0] - above[0], dab.center[1] - above[1]),
                                            dab.center[2] - above[2]) <= dab.radiusMm - 1.0;
        require(covered, "the guide does not reach above the band at x = " + std::to_string(x));
    }
    require(!layout.fixation.empty(), "the guide lost its positioning screws");
    for (const GuideFixationHole& screw : layout.fixation)
        require(screw.center[2] >= 16.0 + 4.0 - 1e-6, "a positioning screw sits in or next to the band: z = " +
                                                         std::to_string(screw.center[2]));
    // And built, it is still one piece: the strip between the two slits hangs on the shared bridges.
    {
        const auto holes = predictiveHoles();
        const GuideRegion region =
            GuideBaseCore::MakeBrushRegion(bone.preparation.wrapField, layout.paint, bone.design.base);
        const GuideDesignResult guide = GuideDesignCore::Build(bone.preparation, region, layout.slotPlan,
                                                               layout.fixation, PlateCore::SleeveFigures(holes), bone.design);
        require(guide.ok && guide.pieces == 1, "the guide with a 7 mm band came apart: " + std::to_string(guide.pieces));
    }
    const double clear = params.latticeSlitClearMm + 0.5 * params.latticeCellMm - 0.25;
    for (const GuideFigure& cell : layout.figures) {
        const double z = cell.matrix[11];
        require(std::abs(z - 9.0) >= clear && std::abs(z - 16.0) >= clear,
                "a lattice cell opens next to a slit at z = " + std::to_string(z));
    }
}

// Without a band (or an empty one) the layout is exactly the plain one.
void testAnEmptyBandLeavesTheLayoutAsItWas()
{
    const Prepared bone = preoperativeBone();
    const auto holes = predictiveHoles();
    const LeFortGuideLayout plain = LeFortGuideCore::Layout(bone.preparation, bone.wrap.mesh, leFortCut(), holes);
    LeFortBandProfile empty;
    const LeFortGuideLayout withEmpty =
        LeFortGuideCore::Layout(bone.preparation, bone.wrap.mesh, leFortCut(), holes, {}, &empty);
    require(plain.ok && withEmpty.ok, "a plain layout failed");
    require(plain.paint.size() == withEmpty.paint.size() && plain.slotPlan.size() == withEmpty.slotPlan.size() &&
                plain.figures.size() == withEmpty.figures.size() && plain.fixation.size() == withEmpty.fixation.size(),
            "an empty band changed the layout");
    for (size_t i = 0; i < plain.paint.size(); ++i)
        require(plain.paint[i].center == withEmpty.paint[i].center && plain.paint[i].radiusMm == withEmpty.paint[i].radiusMm,
                "an empty band moved the paint");
}

// spec §Acceptance 8 (core part): a hole the surgeon moves takes its pad with it. This already holds — the layout
// is a function of the holes — and is kept as a guard for the band work.
void testAMovedHoleTakesItsPadWithIt()
{
    const Prepared bone = preoperativeBone();
    const LeFortBandProfile band = bandFor(3.0);
    std::vector<PredictiveHole> holes = predictiveHoles();
    const Vec3 before = holes.front().preopCenter;
    holes.front().preopCenter = {before[0] - 4.0, before[1], before[2] + 2.0};
    const LeFortGuideLayout layout =
        LeFortGuideCore::Layout(bone.preparation, bone.wrap.mesh, leFortCut(), holes, {}, &band);
    require(layout.ok, layout.error.toStdString());
    bool atNew = false, atOld = false;
    for (const GuideBrushStroke& dab : layout.paint) {
        atNew = atNew || dab.center == holes.front().preopCenter;
        atOld = atOld || dab.center == before;
    }
    require(atNew && !atOld, "the moved hole's pad did not move with it");
}
// The direction out of the face, as the guide lays itself out: the cut's sweep axis has no sign of its own,
// and the hole proposal needs the same one the layout uses.
void testTheAnteriorDirectionIsOutOfTheFaceWhicheverWayTheSweepAxisPoints()
{
    const Prepared bone = preoperativeBone();
    OsteotomyPath reversed = leFortCut();
    for (double& value : reversed.depthAxis)
        value = -value;
    for (const OsteotomyPath& path : {leFortCut(), reversed}) {
        const std::array<double, 3> front = LeFortGuideCore::AnteriorDirection(*bone.preparation.wrapField, path);
        require(front[1] > 0.9, "the anterior direction does not point out of the face: (" + std::to_string(front[0]) +
                                    ", " + std::to_string(front[1]) + ", " + std::to_string(front[2]) + ")");
    }
}

// spec §Behaviour (franja visible) and the user's report of 2026-10-05: the band is the bone itself between the
// Le Fort cut and the band's upper edge — the same two surfaces the slits are carved from — only where the
// segment rises at least 0.5 mm.
void testTheBandIsTheBoneBetweenTheTwoCuts()
{
    const auto cranial = merged(cranialPieces()); // cranial bone from z = 10 (the cut is at z = 9)
    const auto uniform = LeFortGuideCore::BandOnBone(cranial, leFortCut(), bandFor(4.0)); // band z 9..13
    require(uniform && uniform->GetNumberOfPolys() > 0, "a 4 mm impaction drew no band on the bone");
    double lowest = 1e9, highest = -1e9;
    bool front = false;
    for (vtkIdType i = 0; i < uniform->GetNumberOfPoints(); ++i) {
        double p[3];
        uniform->GetPoint(i, p);
        lowest = std::min(lowest, p[2]);
        highest = std::max(highest, p[2]);
        front = front || std::abs(p[1]) < 1e-6;
    }
    require(lowest >= 10.0 - 1e-6 && std::abs(highest - 13.0) <= 0.05,
            "the band is not the bone from the cut up to 4 mm above it: z " + std::to_string(lowest) + ".." +
                std::to_string(highest));
    require(front, "the band does not lie on the anterior wall");

    // Rolled: only the right side rises (rise = −0.1·x), and only where it reaches 0.5 mm.
    const auto rolled = LeFortGuideCore::BandOnBone(cranial, leFortCut(), rolledBand());
    require(rolled && rolled->GetNumberOfPolys() > 0, "the rolled band drew nothing");
    for (vtkIdType i = 0; i < rolled->GetNumberOfPoints(); ++i) {
        double p[3];
        rolled->GetPoint(i, p);
        require(p[0] <= -5.0 + 0.3, "the band was drawn where the segment does not rise: x = " + std::to_string(p[0]));
        require(p[2] <= 9.0 - 0.1 * p[0] + 0.1, "the band is taller than the rise there: z = " + std::to_string(p[2]));
    }

    const std::array<double, 16> descent{1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, -3.0, 0.0, 0.0, 0.0, 1.0};
    const auto none = LeFortGuideCore::BandOnBone(cranial, leFortCut(), LeFortMotionCore::Band(leFortCut(), descent));
    require(!none || none->GetNumberOfPolys() == 0, "a descent drew a band");
}

// The guide covers the whole cut, pillar to pillar, however few holes it drills (user's report, 2026-10-05: on
// a real maxilla the proposal found one sound site and the guide came out as a block round it). The holes can
// only widen the band, never narrow it.
void testTheGuideCoversTheWholeCutWithASingleHole()
{
    const Prepared bone = preoperativeBone();
    PredictiveHole only;
    only.bone = PlateBone::Cranial;
    only.preopCenter = {-18.0, 0.0, 20.0};
    only.preopAxis = {0.0, 1.0, 0.0};
    const LeFortGuideLayout layout = LeFortGuideCore::Layout(bone.preparation, bone.wrap.mesh, leFortCut(), {only});
    require(layout.ok, "the guide was not laid out: " + layout.error.toStdString());
    double left = 1e9, right = -1e9;
    for (const GuideBrushStroke& dab : layout.paint) {
        left = std::min(left, dab.center[0]);
        right = std::max(right, dab.center[0]);
    }
    require(left <= -18.0 && right >= 18.0, "the guide does not run from pillar to pillar: x " + std::to_string(left) +
                                                ".." + std::to_string(right));
}

// The user's rule (2026-10-05): the guide runs from the nasomaxillary to the maxillomalar pillar on each side,
// never into the nose, and the bridge joining the sides passes below the aperture clear of the anterior nasal
// spine — 3 mm between the spine and the bridge's upper edge.
void testTheGuideStaysOutOfTheNoseAndClearOfTheSpine()
{
    const Prepared bone = preoperativeBone(true);
    const LeFortGuideLayout layout = LeFortGuideCore::Layout(bone.preparation, bone.wrap.mesh, leFortCut(), {});
    require(layout.ok, "the guide was not laid out: " + layout.error.toStdString());
    bool bridged = false;
    for (const GuideBrushStroke& dab : layout.paint) {
        if (dab.erase)
            continue;
        const double x = dab.center[0], z = dab.center[2], r = dab.radiusMm;
        // Into the aperture (|x| < 6, z 8.5..20 — above its floor): no dab reaches over the piriform rim.
        const double dx = std::max(0.0, std::abs(x) - 6.0), dz = std::max({0.0, 8.5 - z, z - 20.0});
        require(std::hypot(dx, dz) >= r - 0.5, "a dab reaches into the nose: x " + std::to_string(x) + ", z " +
                                                   std::to_string(z) + ", r " + std::to_string(r));
        // Across the midline: the bridge, its upper edge 3 mm below the spine's foot (z = 5).
        if (std::abs(x) < 3.0) {
            bridged = true;
            require(z + r <= 5.0 - 3.0 + 0.5, "the bridge is not clear of the nasal spine: z " + std::to_string(z) +
                                                  ", r " + std::to_string(r));
        }
    }
    require(bridged, "the two sides are not joined across the midline");
}

// spec asistente-guia-lefort §Acceptance 5 (user's case of 2026-10-05): two guides, right and left, each from
// the nasomaxillary to the maxillomalar pillar, each in one piece, each held by two positioning screws, and
// nothing of either in the nose.
void testTwoGuidesOneEachSide()
{
    const Prepared bone = preoperativeBone(true);
    LeFortGuideParams params;
    params.separateSides = true;
    const LeFortGuideLayout layout = LeFortGuideCore::Layout(bone.preparation, bone.wrap.mesh, leFortCut(), {}, params);
    require(layout.ok, "the guides were not laid out: " + layout.error.toStdString());
    // Patches by real overlap of the dabs.
    const size_t n = layout.paint.size();
    std::vector<size_t> parent(n);
    for (size_t i = 0; i < n; ++i)
        parent[i] = i;
    const std::function<size_t(size_t)> find = [&](size_t i) { return parent[i] == i ? i : parent[i] = find(parent[i]); };
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j) {
            const auto& a = layout.paint[i];
            const auto& b = layout.paint[j];
            const double d = std::hypot(std::hypot(a.center[0] - b.center[0], a.center[1] - b.center[1]), a.center[2] - b.center[2]);
            if (d < a.radiusMm + b.radiusMm - 1.0)
                parent[find(i)] = find(j);
        }
    std::set<size_t> rightRoots, leftRoots;
    for (size_t i = 0; i < n; ++i) {
        const auto& dab = layout.paint[i];
        (dab.center[0] < 0.0 ? rightRoots : leftRoots).insert(find(i));
        const double dx = std::max(0.0, std::abs(dab.center[0]) - 6.0), dz = std::max({0.0, 8.5 - dab.center[2], dab.center[2] - 20.0});
        require(std::hypot(dx, dz) >= dab.radiusMm - 0.5, "a dab reaches into the nose at x " + std::to_string(dab.center[0]));
    }
    require(rightRoots.size() == 1 && leftRoots.size() == 1, "each side is not one patch: " + std::to_string(rightRoots.size()) +
                                                                 " right, " + std::to_string(leftRoots.size()) + " left");
    require(*rightRoots.begin() != *leftRoots.begin(), "the two guides are joined");
    int right = 0, left = 0;
    for (const GuideFixationHole& screw : layout.fixation)
        (screw.center[0] < 0.0 ? right : left) += 1;
    require(right >= 2 && left >= 2, "each guide needs two positioning screws: " + std::to_string(right) + " right, " +
                                         std::to_string(left) + " left");
    const GuideRegion region = GuideBaseCore::MakeBrushRegion(bone.preparation.wrapField, layout.paint, bone.design.base);
    require(region.valid, "the region is not valid: " + region.error.toStdString());
    const GuideDesignResult guides =
        GuideDesignCore::Build(bone.preparation, region, layout.slotPlan, layout.fixation, layout.figures, bone.design);
    require(guides.ok && guides.pieces == 2, "two guides were expected, the build gave " + std::to_string(guides.pieces) +
                                                 " pieces");
}

// The engraving is a solid of the text: as wide as `TextWidth` says, standing `reliefMm` proud of its base.
void testTextIsASolidOfItsMeasuredWidth()
{
    const auto solid = GuideEngraveCore::TextSolid(QStringLiteral("20406"), 3.0, 0.6);
    require(solid && solid->GetNumberOfPolys() > 0, "the text gave no solid");
    double b[6];
    solid->GetBounds(b);
    const double width = GuideEngraveCore::TextWidth(QStringLiteral("20406"), 3.0);
    require(std::abs((b[1] - b[0]) - width) < 0.6, "the text is " + std::to_string(b[1] - b[0]) + " mm wide, not " +
                                                       std::to_string(width));
    require(std::abs(b[0] + b[1]) < 0.2 && std::abs(b[2] + b[3]) < 0.2, "the text is not centred on its origin");
    require(b[5] > 0.55 && b[5] < 0.65 && b[4] < -0.3, "the text does not stand 0.6 mm proud of its sunk base");
    require(MeshRepairCore::Analyze(solid).Valid(), "the text is not a closed solid");
}

// spec asistente-guia-lefort §Acceptance 5: each guide carries the case number and its side, on its own
// material and clear of its screws, and the two stay two.
void testEachGuideIsEngravedWithTheCaseAndItsSide()
{
    const Prepared bone = preoperativeBone(true);
    LeFortGuideParams params;
    params.separateSides = true;
    params.caseLabel = QStringLiteral("20406");
    const LeFortGuideLayout layout = LeFortGuideCore::Layout(bone.preparation, bone.wrap.mesh, leFortCut(), {}, params);
    require(layout.ok, layout.error.toStdString());
    require(layout.labels.size() == 4, "two labels per guide were expected, got " + std::to_string(layout.labels.size()));
    for (const LeFortGuideLabel& label : layout.labels) {
        const bool right = label.center[0] < 0.0;
        require(label.text == QStringLiteral("20406") || label.text == (right ? QStringLiteral("DER") : QStringLiteral("IZQ")),
                "a label says " + label.text.toStdString() + " on the " + (right ? "right" : "left"));
        // Its centre and both ends lie on the guide's material.
        for (const double along : {-0.5, 0.0, 0.5}) {
            const Vec3 p{label.center[0] + along * label.widthMm * label.reading[0],
                         label.center[1] + along * label.widthMm * label.reading[1],
                         label.center[2] + along * label.widthMm * label.reading[2]};
            bool onGuide = false;
            for (const GuideBrushStroke& dab : layout.paint)
                onGuide = onGuide || std::hypot(std::hypot(dab.center[0] - p[0], dab.center[1] - p[1]), dab.center[2] - p[2]) <=
                                         dab.radiusMm - 0.5;
            require(onGuide, "the label " + label.text.toStdString() + " runs off its guide");
        }
        // No screw hole within the text's footprint, with a millimetre to spare.
        for (const GuideFixationHole& screw : layout.fixation) {
            const Vec3 d{screw.center[0] - label.center[0], screw.center[1] - label.center[1], screw.center[2] - label.center[2]};
            const double t = std::clamp(d[0] * label.reading[0] + d[1] * label.reading[1] + d[2] * label.reading[2],
                                        -0.5 * label.widthMm, 0.5 * label.widthMm);
            const double off = std::hypot(std::hypot(d[0] - t * label.reading[0], d[1] - t * label.reading[1]),
                                          d[2] - t * label.reading[2]);
            require(off >= 0.5 * 3.0 + 0.5 * screw.diameterMm + 1.0, "the label " + label.text.toStdString() + " sits on a screw");
        }
    }
    int added = 0;
    for (const GuideFigure& figure : layout.figures)
        added += figure.operation == GuideFigureOperation::Add && figure.shape == GuideFigureShape::Mesh ? 1 : 0;
    require(added == 4, "the labels are not added figures of the guide");
    const GuideRegion region = GuideBaseCore::MakeBrushRegion(bone.preparation.wrapField, layout.paint, bone.design.base);
    const GuideDesignResult guides =
        GuideDesignCore::Build(bone.preparation, region, layout.slotPlan, layout.fixation, layout.figures, bone.design);
    require(guides.ok && guides.pieces == 2, "the engraved guides came out in " + std::to_string(guides.pieces) + " pieces");
    // And they come apart into the right guide and the left one, for two STL files.
    const LeFortGuidePair pair = LeFortGuideCore::SplitBySide(guides.mesh, leFortCut());
    require(pair.right && pair.left && pair.right->GetNumberOfPolys() > 0 && pair.left->GetNumberOfPolys() > 0,
            "the guides did not split into right and left");
    double r[6], l[6];
    pair.right->GetBounds(r);
    pair.left->GetBounds(l);
    const bool rightIsNegative = leFortCut().points.front()[0] < 0.0;
    require(rightIsNegative ? (r[1] < 0.0 && l[0] > 0.0) : (r[0] > 0.0 && l[1] < 0.0), "the right and left guides are swapped");
    require(MeshRepairCore::Analyze(pair.right).Valid() && MeshRepairCore::Analyze(pair.left).Valid(),
            "a split guide is not a closed mesh");
}

} // namespace

int main()
{
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"the guide is laid out from the plan", testTheGuideIsLaidOutFromThePlan},
        {"every sleeve has guide under it", testEverySleeveHasGuideUnderIt},
        {"the guide is one piece with an open slit", testTheGuideIsOnePieceWithAnOpenSlit},
        {"works before plates and refuses without envelope", testWorksBeforePlatesAndRefusesWithoutEnvelope},
        {"the guide finds the anterior wall when the sweep axis is reversed",
         testTheGuideFindsTheAnteriorWallWhenTheSweepAxisIsReversed},
        {"the guide ignores a distant skull surface in saved projects",
         testTheGuideIgnoresADistantSkullSurfaceInSavedProjects},
        {"a band adds an upper slit only where the segment rises", testABandAddsAnUpperSlitOnlyWhereTheSegmentRises},
        {"the upper slit shares the bridges", testTheUpperSlitSharesTheBridges},
        {"the guide with a band is one piece with both slits open", testTheGuideWithABandIsOnePieceWithBothSlitsOpen},
        {"a tall band is covered and kept clear", testATallBandIsCoveredAndKeptClear},
        {"an empty band leaves the layout as it was", testAnEmptyBandLeavesTheLayoutAsItWas},
        {"a moved hole takes its pad with it", testAMovedHoleTakesItsPadWithIt},
        {"the anterior direction is out of the face whichever way the sweep axis points",
         testTheAnteriorDirectionIsOutOfTheFaceWhicheverWayTheSweepAxisPoints},
        {"the band is the bone between the two cuts", testTheBandIsTheBoneBetweenTheTwoCuts},
        {"the guide covers the whole cut with a single hole", testTheGuideCoversTheWholeCutWithASingleHole},
        {"the guide stays out of the nose and clear of the spine", testTheGuideStaysOutOfTheNoseAndClearOfTheSpine},
        {"two guides, one each side", testTwoGuidesOneEachSide},
        {"text is a solid of its measured width", testTextIsASolidOfItsMeasuredWidth},
        {"each guide is engraved with the case and its side", testEachGuideIsEngravedWithTheCaseAndItsSide},
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
