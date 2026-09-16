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

// A wall to sit on: the guide is built on its top face, z = 0, outward normal +z.
vtkSmartPointer<vtkPolyData> wrapWall()
{
    return boxMesh({-25.0, 25.0, -15.0, 15.0, -8.0, 0.0}, false, false);
}

GuideContour patchContour()
{
    return {{-15.0, -10.0, 0.0}, {15.0, -10.0, 0.0}, {15.0, 10.0, 0.0}, {-15.0, 10.0, 0.0}};
}

// A Le Fort-like path crossing the wall of the guide (which spans z = 0.2 to 2.7).
OsteotomyPath crossingPath()
{
    return OsteotomyCore::LeFortPath({{{-10.0, 10.0, 1.4}, {10.0, 10.0, 1.6}, {-20.0, -10.0, 1.2},
                                       {20.0, -10.0, 1.3}}});
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
    const GuidePreparation prepared = GuideDesignCore::Prepare(wrap, params(thickness, clearance));
    require(prepared.ok, "the wrap could not be prepared: " + prepared.error.toStdString());

    // Holes drilled along the surface normal where the user clicked.
    const std::array<double, 3> left{-10.0, 5.0, 0.0}, right{10.0, 5.0, 0.0};
    const auto normal = GuideDesignCore::SurfaceNormalAt(prepared, left);
    require(normal[2] > 0.95, "the surface normal does not point out of the wall");
    std::vector<GuideFixationHole> holes;
    for (const auto& center : {left, right})
        holes.push_back({center, normal, 2.0});

    const GuideDesignResult result =
        GuideDesignCore::Build(prepared, patchContour(), {crossingPath()}, holes, params(thickness, clearance));
    require(result.ok, "the guide could not be built: " + result.error.toStdString());
    require(MeshRepairCore::Analyze(result.mesh).Valid(), "the guide is not a valid closed solid");
    // One slot right across the patch leaves two pieces.
    require(result.pieces == 2, "expected two pieces after the slot, got " + std::to_string(result.pieces));

    const auto solid = ImplicitCore::BakeMeshField(result.mesh, 0.15, 3.0);
    require(solid != nullptr, "the guide could not be measured");
    // Near the outer face: the slot crosses the wall at mid height, so measuring the holes there would
    // read the slot instead.
    const double outerWall = clearance + thickness - 0.3;
    // The holes go through the wall, and 1.6 mm off their axis there is still material.
    for (const auto& center : {left, right}) {
        const std::array<double, 3> inHole{center[0], center[1], outerWall};
        const std::array<double, 3> beside{center[0], center[1] + 1.6, outerWall};
        require(solid->At(inHole) > 0.0, "the fixation hole is not open at x = " + std::to_string(center[0]));
        require(solid->At(beside) < 0.0, "the hole is wider than its diameter at x = " + std::to_string(center[0]));
    }
    // The slot is open on the planned cut and closed a blade's width away.
    const OsteotomyPath path = crossingPath();
    for (double x : {-12.0, 0.0, 12.0}) {
        double low = -5.0, high = 5.0;
        for (int i = 0; i < 40; ++i) {
            const double mid = 0.5 * (low + high);
            (OsteotomyCore::PathField(path, {x, 0.0, mid}) < 0.0 ? low : high) = mid;
        }
        const double z = 0.5 * (low + high);
        require(solid->At({x, 0.0, z}) > 0.0, "the slot is not open on the planned cut at x = " + std::to_string(x));
    }
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
    const auto normal = GuideDesignCore::SurfaceNormalAt(prepared, {0.0, 0.0, 0.0});
    const GuideDesignResult drilled =
        GuideDesignCore::Build(prepared, patchContour(), {}, {{{0.0, 0.0, 0.0}, normal, 3.0}}, params(3.0, 0.0));
    require(drilled.ok && drilled.pieces == 1, "the drilled guide is not one piece");
    require(volumeOf(drilled.mesh) < volumeOf(thick.mesh), "the fixation hole removed nothing");
    require(drilled.report.contains(QStringLiteral("1 agujero")), "the report does not mention the hole: " +
                                                                      drilled.report.toStdString());
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
    const OsteotomyPath degenerate =
        OsteotomyCore::LeFortPath({{{0.0, 0.0, 1.0}, {0.0, 0.0, 1.0}, {0.0, 0.0, 1.0}, {0.0, 0.0, 1.0}}});
    require(!GuideDesignCore::Build(prepared, patchContour(), {degenerate}, {}, params(2.0, 0.0)).ok,
            "a degenerate path was accepted");

    std::atomic<bool> cancel{true};
    require(!GuideDesignCore::Prepare(wrap, params(2.0, 0.0), &cancel).ok, "Prepare ignored the cancel flag");
    require(!GuideDesignCore::Build(prepared, patchContour(), {crossingPath()}, {}, params(2.0, 0.0), &cancel).ok,
            "Build ignored the cancel flag");
}
} // namespace

int main()
{
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"base, slots and holes in one field", testBaseSlotsAndHolesInOneField},
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
