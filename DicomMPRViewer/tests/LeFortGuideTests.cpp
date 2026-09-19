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

    // Four 1.5 mm fixation screws: two above the cut, two below, clear of the sleeves.
    require(layout.fixation.size() == 4, "the guide does not have four fixation screws: " +
                                             std::to_string(layout.fixation.size()));
    int above = 0, below = 0;
    for (const GuideFixationHole& screw : layout.fixation) {
        require(std::abs(screw.diameterMm - 1.5) < 1e-9, "a fixation screw is not 1.5 mm");
        const double f = OsteotomyCore::PathField(leFortCut(), screw.center);
        above += f > 3.0 ? 1 : 0;
        below += f < -3.0 ? 1 : 0;
        for (const PredictiveHole& hole : holes)
            require(std::hypot(std::hypot(screw.center[0] - hole.preopCenter[0], screw.center[1] - hole.preopCenter[1]),
                               screw.center[2] - hole.preopCenter[2]) >= 5.0 - 1e-9,
                    "a fixation screw is on top of a predictive hole");
    }
    require(above == 2 && below == 2, "the fixation screws are not two above and two below the cut");
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

    // The slit is open on the planned cut (z = 9) in the middle of each piece, through the guide's wall;
    // 3 mm above the cut the wall is solid.
    auto distance = vtkSmartPointer<vtkImplicitPolyDataDistance>::New();
    distance->SetInput(guide.mesh);
    for (const GuideSlot& slot : layout.slotPlan) {
        const double x = 0.5 * (slot.start[0] + slot.end[0]);
        for (const double y : {0.6, 1.2})
            require(distance->EvaluateFunction(x, y, 9.0) > 0.0,
                    "the slit is closed at x = " + std::to_string(x) + ", y = " + std::to_string(y));
        require(distance->EvaluateFunction(x, 1.0, 12.0) < 0.0,
                "the guide is not solid beside the slit at x = " + std::to_string(x));
    }
    // And the bridges hold: at the midline bridge of each side the wall is solid on the cut.
    for (const double x : {15.0, -15.0})
        require(distance->EvaluateFunction(x, 1.0, 9.0) < 0.0, "there is no bridge across the slit at x = " +
                                                                  std::to_string(x));
}

void testRefusesWithoutPlatesOrEnvelope()
{
    const Prepared bone = preoperativeBone();
    const LeFortGuideLayout noPlates = LeFortGuideCore::Layout(bone.preparation, bone.wrap.mesh, leFortCut(), {});
    require(!noPlates.ok && !noPlates.error.isEmpty(), "a guide was laid out without plates");
    const LeFortGuideLayout noEnvelope = LeFortGuideCore::Layout({}, nullptr, leFortCut(), predictiveHoles());
    require(!noEnvelope.ok && !noEnvelope.error.isEmpty(), "a guide was laid out without an envelope");
}
} // namespace

int main()
{
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"the guide is laid out from the plan", testTheGuideIsLaidOutFromThePlan},
        {"the guide is one piece with an open slit", testTheGuideIsOnePieceWithAnOpenSlit},
        {"refuses without plates or envelope", testRefusesWithoutPlatesOrEnvelope},
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
