#include "GuideDesignCore.h"

#include "ImplicitCore.h"
#include "MeshRepairCore.h"
#include "OsteotomyCore.h"
#include "SplintTestGeometry.h"

#include <vtkMassProperties.h>
#include <vtkPolyData.h>

#include <atomic>
#include <cmath>
#include <functional>
#include <iostream>
#include <string>

namespace
{
using namespace splinttest;

// The anterior wall of the maxilla: a vertical face at y = 0 with bone behind it. The guide is built on
// that face, so its thickness runs along y and the saw cut crosses it, as it does in surgery. (With the
// guide lying flat and the cut parallel to it, the slot would be a pocket inside the material instead.)
vtkSmartPointer<vtkPolyData> wrapWall()
{
    return boxMesh({-25.0, 25.0, -8.0, 0.0, -5.0, 25.0}, false, false);
}

// The patch the surgeon marks on the wall.
GuideContour patchContour()
{
    return {{-15.0, 0.0, 2.0}, {15.0, 0.0, 2.0}, {15.0, 0.0, 18.0}, {-15.0, 0.0, 18.0}};
}

// Le Fort I: a nearly horizontal path across the wall at z ≈ 9.4, swept antero-posteriorly.
OsteotomyPath crossingPath()
{
    return OsteotomyCore::LeFortPath({{{-10.0, 5.0, 9.4}, {10.0, 5.0, 9.6}, {-20.0, -5.0, 9.2},
                                       {20.0, -5.0, 9.3}}});
}

// Height of the planned cut at x: the field changes sign along z and does not depend on depth.
double cutHeightAt(const OsteotomyPath& path, double x)
{
    double low = 0.0, high = 20.0;
    for (int i = 0; i < 40; ++i) {
        const double mid = 0.5 * (low + high);
        (OsteotomyCore::PathField(path, {x, 0.0, mid}) < 0.0 ? low : high) = mid;
    }
    return 0.5 * (low + high);
}

GuideSlot wholeRegionSlot()
{
    GuideSlot slot;
    slot.path = crossingPath();
    return slot;
}

// The slot as the user places it: both ends marked on the guide.
GuideSlot slotBetween(double startX, double endX)
{
    GuideSlot slot;
    slot.path = crossingPath();
    slot.start = {startX, 1.45, cutHeightAt(slot.path, startX)};
    slot.end = {endX, 1.45, cutHeightAt(slot.path, endX)};
    slot.hasExtent = true;
    return slot;
}

double volumeOf(vtkPolyData* mesh)
{
    auto mass = vtkSmartPointer<vtkMassProperties>::New();
    mass->SetInputData(mesh);
    mass->Update();
    return mass->GetVolume();
}

GuideDesignParams params(double thickness, double clearance, double blade = 1.0)
{
    GuideDesignParams p;
    p.base.thicknessMm = thickness;
    p.base.clearanceMm = clearance;
    p.base.smallestDetailMm = 0.25;
    p.slot.bladeThicknessMm = blade;
    p.slot.smallestDetailMm = 0.25;
    return p;
}

void testBaseSlotsAndHolesInOneField()
{
    const auto wrap = wrapWall();
    const double thickness = 2.5, clearance = 0.2;
    const double midWall = clearance + 0.5 * thickness;
    const GuidePreparation prepared = GuideDesignCore::Prepare(wrap, params(thickness, clearance));
    require(prepared.ok, "the wrap could not be prepared: " + prepared.error.toStdString());

    // Holes drilled along the surface normal where the user clicked, well above the saw cut.
    const std::array<double, 3> left{-10.0, 0.0, 14.0}, right{10.0, 0.0, 14.0};
    const auto normal = GuideDesignCore::SurfaceNormalAt(prepared, left);
    require(normal[1] > 0.95, "the surface normal does not point out of the wall");
    std::vector<GuideFixationHole> holes;
    for (const auto& center : {left, right})
        holes.push_back({center, normal, 2.0});

    const GuideDesignResult result =
        GuideDesignCore::Build(prepared, patchContour(), {wholeRegionSlot()}, holes, params(thickness, clearance));
    require(result.ok, "the guide could not be built: " + result.error.toStdString());
    require(MeshRepairCore::Analyze(result.mesh).Valid(), "the guide is not a valid closed solid");
    // The slot stops short of the rim (edge margin), so the guide stays in one piece.
    require(result.pieces == 1, "the slot separated the guide: " + std::to_string(result.pieces) + " pieces");

    const auto solid = ImplicitCore::BakeMeshField(result.mesh, 0.15, 3.0);
    require(solid != nullptr, "the guide could not be measured");
    // The holes go through the wall, and 1.6 mm off their axis there is still material.
    for (const auto& center : {left, right}) {
        require(solid->At({center[0], midWall, center[2]}) > 0.0,
                "the fixation hole is not open at x = " + std::to_string(center[0]));
        require(solid->At({center[0] + 1.6, midWall, center[2]}) < 0.0,
                "the hole is wider than its diameter at x = " + std::to_string(center[0]));
    }
    // The slot is open along the planned cut...
    const OsteotomyPath path = crossingPath();
    for (double x : {-10.0, 0.0, 10.0})
        require(solid->At({x, midWall, cutHeightAt(path, x)}) > 0.0,
                "the slot is not open on the planned cut at x = " + std::to_string(x));
    // ...and stops before the edge of the guide: that bridge is what holds it together.
    for (double x : {-14.0, 14.0})
        require(solid->At({x, midWall, cutHeightAt(path, x)}) < 0.0,
                "the slot reaches the edge of the guide at x = " + std::to_string(x));
}

void testSlotEndsWhereItIsPlaced()
{
    const auto wrap = wrapWall();
    const double thickness = 2.5, clearance = 0.2;
    const double midWall = clearance + 0.5 * thickness;
    const GuidePreparation prepared = GuideDesignCore::Prepare(wrap, params(thickness, clearance));
    require(prepared.ok, "the wrap could not be prepared: " + prepared.error.toStdString());

    const GuideDesignResult placed =
        GuideDesignCore::Build(prepared, patchContour(), {slotBetween(-5.0, 5.0)}, {}, params(thickness, clearance));
    require(placed.ok, "the placed slot could not be built: " + placed.error.toStdString());
    require(placed.pieces == 1, "the placed slot separated the guide");

    const auto solid = ImplicitCore::BakeMeshField(placed.mesh, 0.15, 3.0);
    require(solid != nullptr, "the guide could not be measured");
    const OsteotomyPath path = crossingPath();
    // Open between the two marked ends.
    for (double x : {-4.0, 0.0, 4.0})
        require(solid->At({x, midWall, cutHeightAt(path, x)}) > 0.0,
                "the placed slot is not open at x = " + std::to_string(x));
    // Solid beyond them, even though the osteotomy carries on.
    for (double x : {-9.0, 9.0})
        require(solid->At({x, midWall, cutHeightAt(path, x)}) < 0.0,
                "the slot runs past the end the user placed at x = " + std::to_string(x));

    // A shorter slot removes less material than one spanning the whole region.
    const GuideDesignResult whole =
        GuideDesignCore::Build(prepared, patchContour(), {wholeRegionSlot()}, {}, params(thickness, clearance));
    require(whole.ok && volumeOf(placed.mesh) > volumeOf(whole.mesh),
            "the placed slot did not remove less than the full-width one");

    // Both ends on the same spot is a mistake worth reporting.
    const GuideDesignResult bad =
        GuideDesignCore::Build(prepared, patchContour(), {slotBetween(0.0, 0.0)}, {}, params(thickness, clearance));
    require(!bad.ok && !bad.error.isEmpty(), "a slot with both ends on the same point was accepted");
}

void testPreparationIsReused()
{
    const auto wrap = wrapWall();
    const GuidePreparation prepared = GuideDesignCore::Prepare(wrap, params(2.0, 0.0));
    require(prepared.ok && prepared.wrapField != nullptr, "the wrap could not be prepared");

    // The interactive half: thickness and holes change without measuring the wrap again.
    const GuideDesignResult thin = GuideDesignCore::Build(prepared, patchContour(), {}, {}, params(1.5, 0.0));
    const GuideDesignResult thick = GuideDesignCore::Build(prepared, patchContour(), {}, {}, params(3.0, 0.0));
    require(thin.ok && thick.ok, "the guide could not be rebuilt from the same preparation");
    require(thin.pieces == 1 && thick.pieces == 1, "a guide without slots should be one piece");
    require(volumeOf(thick.mesh) > 1.5 * volumeOf(thin.mesh), "the thickness did not change the guide");

    // Holes alone remove material and keep the guide in one piece.
    const std::array<double, 3> center{0.0, 0.0, 10.0};
    const auto normal = GuideDesignCore::SurfaceNormalAt(prepared, center);
    const GuideDesignResult drilled =
        GuideDesignCore::Build(prepared, patchContour(), {}, {{center, normal, 3.0}}, params(3.0, 0.0));
    require(drilled.ok && drilled.pieces == 1, "the drilled guide is not one piece");
    require(volumeOf(drilled.mesh) < volumeOf(thick.mesh), "the fixation hole removed nothing");
    require(drilled.report.contains(QStringLiteral("1 agujero")),
            "the report does not mention the hole: " + drilled.report.toStdString());
}

void testRejectsBadInput()
{
    const auto wrap = wrapWall();
    const GuidePreparation prepared = GuideDesignCore::Prepare(wrap, params(2.0, 0.0));
    require(!GuideDesignCore::Prepare(nullptr, params(2.0, 0.0)).ok, "a guide was prepared without a wrap");
    require(!GuideDesignCore::Build({}, patchContour(), {}, {}, params(2.0, 0.0)).ok,
            "a guide was built without a preparation");
    const GuideContour twoPoints = {patchContour()[0], patchContour()[1]};
    require(!GuideDesignCore::Build(prepared, twoPoints, {}, {}, params(2.0, 0.0)).ok,
            "a guide was built from two points");
    // A degenerate osteotomy is reported, not silently skipped.
    GuideSlot degenerate;
    degenerate.path =
        OsteotomyCore::LeFortPath({{{0.0, 0.0, 1.0}, {0.0, 0.0, 1.0}, {0.0, 0.0, 1.0}, {0.0, 0.0, 1.0}}});
    require(!GuideDesignCore::Build(prepared, patchContour(), {degenerate}, {}, params(2.0, 0.0)).ok,
            "a degenerate path was accepted");

    std::atomic<bool> cancel{true};
    require(!GuideDesignCore::Prepare(wrap, params(2.0, 0.0), &cancel).ok, "Prepare ignored the cancel flag");
    require(!GuideDesignCore::Build(prepared, patchContour(), {wholeRegionSlot()}, {}, params(2.0, 0.0), &cancel).ok,
            "Build ignored the cancel flag");
}
} // namespace

int main()
{
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"base, slots and holes in one field", testBaseSlotsAndHolesInOneField},
        {"slot ends where it is placed", testSlotEndsWhereItIsPlaced},
        {"preparation is reused", testPreparationIsReused},
        {"rejects bad input", testRejectsBadInput},
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
