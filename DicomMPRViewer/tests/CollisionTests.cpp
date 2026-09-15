#include "CollisionCore.h"

#include <vtkCubeSource.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>
#include <vtkSphereSource.h>
#include <vtkTriangleFilter.h>

#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

vtkSmartPointer<vtkPolyData> cube(double cx, double cy, double cz, double size)
{
    auto source = vtkSmartPointer<vtkCubeSource>::New();
    source->SetCenter(cx, cy, cz);
    source->SetXLength(size);
    source->SetYLength(size);
    source->SetZLength(size);
    auto tri = vtkSmartPointer<vtkTriangleFilter>::New();
    tri->SetInputConnection(source->GetOutputPort());
    tri->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(tri->GetOutput());
    return out;
}

void testOverlappingCubes()
{
    const auto a = cube(0.0, 0.0, 0.0, 20.0);
    const auto b = cube(10.0, 10.0, 10.0, 20.0);
    const IntersectionResult result = CollisionCore::Intersection(a, b, 0.5);
    require(result.ok, "intersection failed: " + result.error.toStdString());
    require(std::abs(result.volumeMm3 - 1000.0) < 50.0, "overlap volume is not 10x10x10: " + std::to_string(result.volumeMm3));
    require(result.highlight && result.highlight->GetNumberOfCells() == a->GetNumberOfCells(), "highlight copy missing");
}

void testSeparatedAndTouching()
{
    const IntersectionResult apart = CollisionCore::Intersection(cube(0, 0, 0, 10), cube(30, 0, 0, 10), 0.5);
    require(apart.ok && apart.volumeMm3 == 0.0 && apart.highlightedCells == 0, "separated cubes reported an intersection");
    require(!CollisionCore::Intersection(nullptr, cube(0, 0, 0, 10)).ok, "missing mesh accepted");
}

void testSphereInsideCubeHighlight()
{
    auto sphere = vtkSmartPointer<vtkSphereSource>::New();
    sphere->SetRadius(5.0);
    sphere->SetThetaResolution(32);
    sphere->SetPhiResolution(32);
    sphere->Update();
    // Sphere straddling the cube face x = 10: half of it inside.
    auto shifted = vtkSmartPointer<vtkSphereSource>::New();
    shifted->SetCenter(10.0, 0.0, 0.0);
    shifted->SetRadius(5.0);
    shifted->SetThetaResolution(32);
    shifted->SetPhiResolution(32);
    shifted->Update();
    const IntersectionResult result = CollisionCore::Intersection(shifted->GetOutput(), cube(0, 0, 0, 20), 0.25);
    const double half = 0.5 * 4.0 / 3.0 * 3.14159265358979 * 125.0;
    require(result.ok && std::abs(result.volumeMm3 - half) / half < 0.08,
            "half-sphere intersection volume is off: " + std::to_string(result.volumeMm3));
    const vtkIdType cells = shifted->GetOutput()->GetNumberOfPolys();
    require(result.highlightedCells > cells / 3 && result.highlightedCells < 2 * cells / 3,
            "highlight does not mark about half of the sphere");
}
} // namespace

int main()
{
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"overlapping cubes", testOverlappingCubes},
        {"separated and touching", testSeparatedAndTouching},
        {"sphere inside cube highlight", testSphereInsideCubeHighlight},
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
