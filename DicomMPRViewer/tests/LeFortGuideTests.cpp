#include "LeFortGuideCore.h"

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

Prepared preoperativeBone()
{
    Prepared out;
    std::vector<vtkPolyData*> meshes;
    const auto cranial = cranialPieces();
    const auto segment = segmentBeforeCut();
    for (const auto& mesh : cranial)
        meshes.push_back(mesh);
    meshes.push_back(segment);
    WrapParams wrapParams;
    wrapParams.gapClosingMm = 1.5;
    wrapParams.smallestDetailMm = 0.4;
    out.wrap = WrapCore::Wrap(meshes, wrapParams);
    require(out.wrap.ok, "the bone before the cut could not be wrapped");
    out.design.base.smallestDetailMm = 0.4;
    out.design.slot.smallestDetailMm = 0.4;
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
            if (distance->EvaluateFunction(x, 1.0, 11.0) >= 0.0)
                continue; // no guide wall above the cut here (a hole, the end of the band)
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
