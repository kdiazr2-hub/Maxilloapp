#include "CutSlotCore.h"

#include "ImplicitCore.h"
#include "MeshRepairCore.h"
#include "OsteotomyCore.h"
#include "SplintTestGeometry.h"

#include <vtkMassProperties.h>
#include <vtkPolyData.h>
#include <vtkPolyDataConnectivityFilter.h>

#include <atomic>
#include <cmath>
#include <functional>
#include <iostream>
#include <string>

namespace
{
using namespace splinttest;

// Le Fort I landmarks, as in OsteotomyCoreTests.
const std::array<OstPoint3, 4> kLeFort = {{{-10.0, 35.0, 20.0}, {10.0, 35.0, 21.0}, {-25.0, 10.0, 15.0},
                                           {25.0, 10.0, 14.0}}};

// A plate straddling the cut, standing on the anterior wall: 3 mm thick in depth, tall enough for the
// path to cross it from side to side.
vtkSmartPointer<vtkPolyData> basePlate()
{
    return boxMesh({-30.0, 30.0, 14.0, 17.0, 8.0, 26.0}, false, false);
}

double volumeOf(vtkPolyData* mesh)
{
    auto mass = vtkSmartPointer<vtkMassProperties>::New();
    mass->SetInputData(mesh);
    mass->Update();
    return mass->GetVolume();
}

CutSlotParams params(double blade, double detail = 0.25)
{
    CutSlotParams p;
    p.bladeThicknessMm = blade;
    p.smallestDetailMm = detail;
    return p;
}

// Height on the cut surface at (x, y): the field changes sign along z.
double cutHeightAt(const OsteotomyPath& path, double x, double y)
{
    double low = 8.0, high = 26.0;
    for (int i = 0; i < 40; ++i) {
        const double mid = 0.5 * (low + high);
        if (OsteotomyCore::PathField(path, {x, y, mid}) < 0.0)
            low = mid;
        else
            high = mid;
    }
    return 0.5 * (low + high);
}

void testSlotFollowsThePlannedCut()
{
    const OsteotomyPath path = OsteotomyCore::LeFortPath(kLeFort);
    require(path.valid, "the Le Fort path is not valid: " + path.error.toStdString());
    const auto plate = basePlate();
    const double blade = 1.0;
    const CutSlotResult result = CutSlotCore::CutSlots(plate, {path}, params(blade));
    require(result.ok, "the slot could not be cut: " + result.error.toStdString());
    require(MeshRepairCore::Analyze(result.mesh).Valid(), "the slotted guide is not a valid closed solid");
    // The field runs past the ends of the path, so a slot across the base separates it.
    require(result.pieces == 2, "a slot right across the plate should leave two pieces, got " +
                                    std::to_string(result.pieces));

    // What the slot removed is a slab of the blade's thickness: area of the cut inside the plate.
    const double removed = volumeOf(plate) - volumeOf(result.mesh);
    const double cutArea = 3.0 * 62.0; // plate depth by the path across it
    require(removed > 0.6 * blade * cutArea && removed < 1.6 * blade * cutArea,
            "the slot removed " + std::to_string(removed) + " mm³, expected about " +
                std::to_string(blade * cutArea));

    // Mid-plane of the slot on the planned cut, walls at the blade's half width.
    const auto solid = ImplicitCore::BakeMeshField(result.mesh, 0.2, 3.0);
    require(solid != nullptr, "the slotted guide could not be measured");
    for (double x : {-20.0, -10.0, 0.0, 10.0, 20.0}) {
        const double y = 15.5;
        const double z = cutHeightAt(path, x, y);
        require(solid->At({x, y, z}) > 0.0,
                "the slot is not open on the planned cut at x = " + std::to_string(x));
        require(solid->At({x, y, z + 1.3}) < 0.0 && solid->At({x, y, z - 1.3}) < 0.0,
                "the slot is wider than the blade at x = " + std::to_string(x));
    }
}

void testSeveralPathsAndBladeWidth()
{
    const OsteotomyPath path = OsteotomyCore::LeFortPath(kLeFort);
    const auto plate = basePlate();
    const CutSlotResult thin = CutSlotCore::CutSlots(plate, {path}, params(0.4));
    const CutSlotResult thick = CutSlotCore::CutSlots(plate, {path}, params(1.0));
    require(thin.ok && thick.ok, "the plate could not be slotted at both blade widths");
    require(volumeOf(thick.mesh) < volumeOf(thin.mesh), "a thicker blade did not remove more material");

    // A second, lower cut: both slots are carved into the same field.
    const std::array<OstPoint3, 4> lower = {{{-10.0, 35.0, 11.0}, {10.0, 35.0, 11.0}, {-25.0, 10.0, 11.0},
                                             {25.0, 10.0, 11.0}}};
    const OsteotomyPath second = OsteotomyCore::LeFortPath(lower);
    require(second.valid, "the second path is not valid: " + second.error.toStdString());
    const CutSlotResult both = CutSlotCore::CutSlots(plate, {path, second}, params(1.0));
    require(both.ok, "two slots could not be cut: " + both.error.toStdString());
    require(volumeOf(both.mesh) < volumeOf(thick.mesh), "the second slot removed nothing");
    require(both.pieces == 3, "two slots across the plate should leave three pieces, got " +
                                  std::to_string(both.pieces));
    require(both.report.contains(QStringLiteral("2 corte")), "the report does not say how many cuts: " +
                                                                 both.report.toStdString());
}

void testRejectsBadInput()
{
    const OsteotomyPath path = OsteotomyCore::LeFortPath(kLeFort);
    const auto plate = basePlate();
    require(!CutSlotCore::CutSlots(nullptr, {path}).ok, "a slot without a base was cut");
    require(!CutSlotCore::CutSlots(plate, {}).ok, "slots without paths were cut");
    // Four identical landmarks give no path at all.
    const OsteotomyPath degenerate = OsteotomyCore::LeFortPath({kLeFort[0], kLeFort[0], kLeFort[0], kLeFort[0]});
    const CutSlotResult bad = CutSlotCore::CutSlots(plate, {degenerate}, params(1.0));
    require(!bad.ok && !bad.error.isEmpty(), "a degenerate path was accepted");

    std::atomic<bool> cancel{true};
    const CutSlotResult cancelled = CutSlotCore::CutSlots(plate, {path}, params(1.0), &cancel);
    require(!cancelled.ok && !cancelled.error.isEmpty(), "the slot ignored the cancel flag");
}
} // namespace

int main()
{
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"slot follows the planned cut", testSlotFollowsThePlannedCut},
        {"several paths and blade width", testSeveralPathsAndBladeWidth},
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
