#include "WrapCore.h"

#include "MeshRepairCore.h"
#include "SplintTestGeometry.h"

#include <vtkMassProperties.h>
#include <vtkPolyData.h>
#include <vtkPolyDataConnectivityFilter.h>
#include <vtkSphereSource.h>
#include <vtkTriangleFilter.h>

#include <atomic>
#include <cmath>
#include <functional>
#include <iostream>
#include <string>

namespace
{
using namespace splinttest;

constexpr double kPiValue = 3.14159265358979323846;

double volumeOf(vtkPolyData* mesh)
{
    auto mass = vtkSmartPointer<vtkMassProperties>::New();
    mass->SetInputData(mesh);
    mass->Update();
    return mass->GetVolume();
}

int regions(vtkPolyData* mesh)
{
    auto connectivity = vtkSmartPointer<vtkPolyDataConnectivityFilter>::New();
    connectivity->SetInputData(mesh);
    connectivity->SetExtractionModeToAllRegions();
    connectivity->Update();
    return connectivity->GetNumberOfExtractedRegions();
}

// Sphere left open at the pole: the hole is 2 R sin(startPhi) across.
vtkSmartPointer<vtkPolyData> holedSphere(double radius, double startPhiDegrees)
{
    auto source = vtkSmartPointer<vtkSphereSource>::New();
    source->SetRadius(radius);
    source->SetThetaResolution(72);
    source->SetPhiResolution(72);
    source->SetStartPhi(startPhiDegrees);
    auto tri = vtkSmartPointer<vtkTriangleFilter>::New();
    tri->SetInputConnection(source->GetOutputPort());
    tri->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(tri->GetOutput());
    return out;
}

WrapParams params(double gapClosingMm, double smallestDetailMm = 0.3)
{
    WrapParams p;
    p.gapClosingMm = gapClosingMm;
    p.smallestDetailMm = smallestDetailMm;
    return p;
}

void testWrapsAClosedSolid()
{
    const auto cube = boxMesh({-10.0, 10.0, -10.0, 10.0, -10.0, 10.0}, false, false);
    const WrapResult result = WrapCore::Wrap({cube}, params(1.0));
    require(result.ok, "the cube could not be wrapped: " + result.error.toStdString());
    require(MeshRepairCore::Analyze(result.mesh).Valid(), "the wrap is not a valid closed solid");
    require(regions(result.mesh) == 1, "the wrap of one cube is not a single shell");
    const double volume = volumeOf(result.mesh);
    require(std::abs(volume - 8000.0) < 0.06 * 8000.0,
            "the wrap changed the cube volume: " + std::to_string(volume));
    require(!result.report.isEmpty(), "the wrap reports nothing");
}

void testGapClosingBridgesAndSeals()
{
    // Two cubes 0.8 mm apart: bridged only when the gap closing reaches across.
    const auto left = boxMesh({-10.0, 0.0, -5.0, 5.0, -5.0, 5.0}, false, false);
    const auto right = boxMesh({0.8, 10.8, -5.0, 5.0, -5.0, 5.0}, false, false);
    const WrapResult bridged = WrapCore::Wrap({left, right}, params(1.5));
    require(bridged.ok, "the pair could not be wrapped: " + bridged.error.toStdString());
    require(regions(bridged.mesh) == 1, "a 0.8 mm gap was not bridged with 1.5 mm of gap closing");
    require(MeshRepairCore::Analyze(bridged.mesh).Valid(), "the bridged wrap is not a valid solid");

    const WrapResult apart = WrapCore::Wrap({left, right}, params(0.2));
    require(apart.ok, "the pair could not be wrapped with a small gap closing: " + apart.error.toStdString());
    require(regions(apart.mesh) == 2, "0.2 mm of gap closing should leave the two cubes apart");

    // A hole smaller than the gap closing disappears and the solid stays solid.
    const auto sphere = holedSphere(10.0, 6.0); // hole about 2.1 mm across
    const double full = 4.0 / 3.0 * kPiValue * 1000.0;
    const WrapResult sealed = WrapCore::Wrap({sphere}, params(1.5));
    require(sealed.ok, "the holed sphere could not be wrapped: " + sealed.error.toStdString());
    require(MeshRepairCore::Analyze(sealed.mesh).Valid(), "the sealed sphere is not a valid closed solid");
    const double sealedVolume = volumeOf(sealed.mesh);
    require(sealedVolume > 0.85 * full,
            "the hole was not closed, the wrap is a shell: " + std::to_string(sealedVolume));

    // With a gap closing below the hole size it stays open, so the interior is not filled.
    const WrapResult open = WrapCore::Wrap({sphere}, params(0.1));
    require(!open.ok || volumeOf(open.mesh) < 0.5 * full,
            "a hole larger than the gap closing was sealed anyway");
}

void testWrapsOpenSurfaces()
{
    // An open box (a scan that ends at the gingiva) still comes back closed.
    const auto open = boxMesh({-10.0, 10.0, -10.0, 10.0, -10.0, 10.0}, true, false);
    const WrapResult result = WrapCore::Wrap({open}, params(1.0));
    require(result.ok, "the open box could not be wrapped: " + result.error.toStdString());
    const MeshCheck check = MeshRepairCore::Analyze(result.mesh);
    require(check.Closed(), "the wrap of an open surface is not closed: " + check.Summary().toStdString());
}

void testDetailAndFailures()
{
    const auto cube = boxMesh({-10.0, 10.0, -10.0, 10.0, -10.0, 10.0}, false, false);
    const WrapResult fine = WrapCore::Wrap({cube}, params(1.0, 0.3));
    const WrapResult coarse = WrapCore::Wrap({cube}, params(1.0, 0.8));
    require(fine.ok && coarse.ok, "the cube could not be wrapped at both resolutions");
    require(coarse.spacingMm > fine.spacingMm, "the smallest detail did not change the grid");
    require(coarse.mesh->GetNumberOfPolys() < fine.mesh->GetNumberOfPolys(),
            "a coarser detail did not give a lighter mesh");

    const WrapResult empty = WrapCore::Wrap({}, params(1.0));
    require(!empty.ok && !empty.error.isEmpty(), "wrapping nothing was not reported");

    std::atomic<bool> cancel{true};
    const WrapResult cancelled = WrapCore::Wrap({cube}, params(1.0), &cancel);
    require(!cancelled.ok && !cancelled.error.isEmpty(), "the wrap ignored the cancel flag");
}
} // namespace

int main()
{
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"wraps a closed solid", testWrapsAClosedSolid},
        {"gap closing bridges and seals", testGapClosingBridgesAndSeals},
        {"wraps open surfaces", testWrapsOpenSurfaces},
        {"detail and failures", testDetailAndFailures},
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
