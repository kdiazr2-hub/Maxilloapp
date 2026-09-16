#include "ImplicitCore.h"
#include "MeshRepairCore.h"
#include "SplintTestGeometry.h"

#include <vtkImageData.h>
#include <vtkMassProperties.h>
#include <vtkMatrix4x4.h>
#include <vtkPolyData.h>
#include <vtkTransform.h>

#include <atomic>
#include <cmath>
#include <functional>
#include <iostream>
#include <string>

namespace
{
using namespace splinttest;
using namespace ImplicitCore;

constexpr double kPiValue = 3.14159265358979323846;

// Volumes are compared against the analytic ones, so smoothing is off and the mesh is left as
// polygonised: repairing orients every shell outward, which makes vtkMassProperties add a hollow
// solid's cavity instead of subtracting it. Repair is covered on its own in testRepairedOutput().
PolygonizeOptions sharpOptions()
{
    PolygonizeOptions options;
    options.smoothingIterations = 0;
    options.repair = false;
    return options;
}

double volumeOf(vtkPolyData* mesh)
{
    auto mass = vtkSmartPointer<vtkMassProperties>::New();
    mass->SetInputData(mesh);
    mass->Update();
    return mass->GetVolume();
}

BuildResult built(const NodePtr& node, double spacing = 0.3)
{
    return Build(node, nullptr, spacing, sharpOptions());
}

void requireVolume(const NodePtr& node, double expected, double tolerance, const std::string& what)
{
    const BuildResult result = built(node);
    require(result.ok, what + " could not be built: " + result.error.toStdString());
    // Marching cubes leaves a few zero-area triangles where the surface meets grid points exactly;
    // the repair pass drops them, and what it returns must be a valid solid.
    const MeshRepairResult repaired = MeshRepairCore::Repair(result.mesh);
    require(repaired.ok, what + " is not a valid closed solid: " + repaired.after.Summary().toStdString());
    const double volume = volumeOf(result.mesh);
    require(std::abs(volume - expected) <= tolerance * expected,
            what + " volume is " + std::to_string(volume) + ", expected " + std::to_string(expected));
}

void testPointValues()
{
    const auto sphere = Sphere({1.0, 2.0, 3.0}, 5.0);
    require(std::abs(Value(sphere, {1.0, 2.0, 3.0}) + 5.0) < 1e-9, "sphere centre is not -radius");
    require(std::abs(Value(sphere, {6.0, 2.0, 3.0})) < 1e-9, "sphere surface is not zero");
    require(std::abs(Value(sphere, {9.0, 2.0, 3.0}) - 3.0) < 1e-9, "distance outside the sphere is wrong");

    const auto box = ImplicitCore::Box({0.0, 0.0, 0.0}, {2.0, 3.0, 4.0});
    require(std::abs(Value(box, {0.0, 0.0, 0.0}) + 2.0) < 1e-9, "box centre distance is not the smallest half size");
    require(std::abs(Value(box, {5.0, 0.0, 0.0}) - 3.0) < 1e-9, "distance beside the box is wrong");
    require(std::abs(Value(box, {2.0, 3.0, 4.0})) < 1e-9, "box corner is not on the surface");

    // Half space: negative where the normal points away from.
    const auto below = HalfSpace({0.0, 0.0, 10.0}, {0.0, 0.0, 1.0});
    require(Value(below, {0.0, 0.0, 0.0}) < 0.0 && Value(below, {0.0, 0.0, 20.0}) > 0.0,
            "half space sign is inverted");

    // Operators are the arithmetic of the field.
    const auto a = Sphere({-2.0, 0.0, 0.0}, 4.0);
    const auto b = Sphere({2.0, 0.0, 0.0}, 4.0);
    const Vec3 p{1.0, 1.0, 1.0};
    require(std::abs(Value(Union(a, b), p) - std::min(Value(a, p), Value(b, p))) < 1e-12, "union is not min");
    require(std::abs(Value(Intersect(a, b), p) - std::max(Value(a, p), Value(b, p))) < 1e-12,
            "intersection is not max");
    require(std::abs(Value(Subtract(a, b), p) - std::max(Value(a, p), -Value(b, p))) < 1e-12,
            "subtraction is not max(a, -b)");
    require(std::abs(Value(Offset(a, 1.5), p) - (Value(a, p) - 1.5)) < 1e-12, "offset does not shift the field");
    require(std::abs(Value(Negate(a), p) + Value(a, p)) < 1e-12, "negation does not flip the field");
}

void testPrimitiveVolumes()
{
    requireVolume(Sphere({0.0, 0.0, 0.0}, 10.0), 4.0 / 3.0 * kPiValue * 1000.0, 0.01, "sphere");
    requireVolume(ImplicitCore::Box({1.0, -2.0, 0.5}, {8.0, 6.0, 4.0}), 16.0 * 12.0 * 8.0, 0.01, "box");
    requireVolume(Cylinder({0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, 6.0, 5.0), kPiValue * 36.0 * 10.0, 0.01, "cylinder");
    // Frustum: (pi h / 3)(R^2 + R r + r^2)
    requireVolume(Cone({0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, 6.0, 3.0, 5.0),
                  kPiValue * 10.0 / 3.0 * (36.0 + 18.0 + 9.0), 0.02, "frustum");
    requireVolume(Torus({0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, 10.0, 3.0), 2.0 * kPiValue * kPiValue * 10.0 * 9.0, 0.02,
                  "torus");
    requireVolume(Capsule({0.0, 0.0, -5.0}, {0.0, 0.0, 5.0}, 4.0),
                  kPiValue * 16.0 * 10.0 + 4.0 / 3.0 * kPiValue * 64.0, 0.02, "capsule");
    const std::vector<Vec3> square = {{-5.0, -5.0, 0.0}, {5.0, -5.0, 0.0}, {5.0, 5.0, 0.0}, {-5.0, 5.0, 0.0}};
    requireVolume(Prism(square, {0.0, 0.0, 1.0}, 5.0), 100.0 * 10.0, 0.01, "prism");
    // A rounded box keeps the same bounds but loses the corners.
    const BuildResult rounded = built(RoundedBox({0.0, 0.0, 0.0}, {6.0, 6.0, 6.0}, 2.0));
    require(rounded.ok, "rounded box could not be built");
    double b[6] = {};
    rounded.mesh->GetBounds(b);
    require(std::abs(b[1] - 6.0) < 0.2 && std::abs(b[0] + 6.0) < 0.2, "rounded box does not keep its bounds");
    const double roundedVolume = volumeOf(rounded.mesh);
    require(roundedVolume < 12.0 * 12.0 * 12.0 && roundedVolume > 0.9 * 12.0 * 12.0 * 12.0,
            "rounded box volume is not just under the box volume");
}

void testBooleans()
{
    const double sphere8 = 4.0 / 3.0 * kPiValue * 512.0;
    const auto a = Sphere({-4.0, 0.0, 0.0}, 8.0);
    const auto b = Sphere({4.0, 0.0, 0.0}, 8.0);
    // Two equal spheres at distance d overlap by pi (2r - d)^2 (d^2 + 4 d r) / (12 d).
    const double d = 8.0, r = 8.0;
    const double lens = kPiValue * (2 * r - d) * (2 * r - d) * (d * d + 4 * d * r) / (12 * d);
    requireVolume(Union(a, b), 2.0 * sphere8 - lens, 0.02, "union of two spheres");
    requireVolume(Intersect(a, b), lens, 0.03, "intersection of two spheres");
    // Sphere cut by a plane through its centre: a hemisphere.
    requireVolume(Subtract(Sphere({0.0, 0.0, 0.0}, 10.0), HalfSpace({0.0, 0.0, 0.0}, {0.0, 0.0, 1.0})),
                  0.5 * 4.0 / 3.0 * kPiValue * 1000.0, 0.02, "sphere minus a half space");
    requireVolume(Offset(Sphere({0.0, 0.0, 0.0}, 10.0), 2.0), 4.0 / 3.0 * kPiValue * 12.0 * 12.0 * 12.0, 0.01,
                  "offset sphere");
    requireVolume(Hollow(Sphere({0.0, 0.0, 0.0}, 10.0), 2.0),
                  4.0 / 3.0 * kPiValue * (1000.0 - 512.0), 0.02, "hollow sphere shell");
    // Layer: the material between two distances from the surface, as a base plate on a wrap.
    requireVolume(Layer(Sphere({0.0, 0.0, 0.0}, 10.0), 1.0, 3.0),
                  4.0 / 3.0 * kPiValue * (13.0 * 13.0 * 13.0 - 11.0 * 11.0 * 11.0), 0.02, "offset layer");
}

void testTransformAndBounds()
{
    auto transform = vtkSmartPointer<vtkTransform>::New();
    transform->Translate(10.0, -5.0, 2.0);
    transform->RotateZ(35.0);
    const auto moved = Transformed(ImplicitCore::Box({0.0, 0.0, 0.0}, {4.0, 2.0, 1.0}), transform->GetMatrix());
    require(std::abs(Value(moved, {10.0, -5.0, 2.0}) + 1.0) < 1e-9, "transformed box centre moved");
    requireVolume(moved, 8.0 * 4.0 * 2.0, 0.03, "transformed box");

    double bounds[6] = {};
    require(Bounds(Sphere({0.0, 0.0, 0.0}, 3.0), bounds) && std::abs(bounds[1] - 3.0) < 1e-9, "sphere bounds wrong");
    require(!Bounds(HalfSpace({0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}), bounds), "a half space cannot be bounded");
    // An unbounded node needs explicit bounds.
    const BuildResult unbounded = Build(HalfSpace({0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}), nullptr, 0.5);
    require(!unbounded.ok && !unbounded.error.isEmpty(), "unbounded build was not reported");
    // Intersecting with a bounded solid makes it bounded again.
    require(Bounds(Intersect(Sphere({0.0, 0.0, 0.0}, 3.0), HalfSpace({0.0, 0.0, 0.0}, {0.0, 0.0, 1.0})), bounds),
            "intersection with a sphere is not bounded");
}

void testMeshField()
{
    // A 20 mm cube baked to a field: distances inside, on the surface and outside.
    const auto cube = boxMesh({-10.0, 10.0, -10.0, 10.0, -10.0, 10.0}, false, false);
    QString error;
    const auto field = BakeMeshField(cube, 0.25, 4.0, nullptr, &error);
    require(field != nullptr, "cube field was not baked: " + error.toStdString());
    const auto node = Field(field);
    require(std::abs(Value(node, {0.0, 0.0, 0.0}) + 10.0) < 0.6, "centre of the cube is not 10 mm deep");
    require(std::abs(Value(node, {10.0, 0.0, 0.0})) < 0.5, "the cube surface is not near zero");
    require(std::abs(Value(node, {13.0, 0.0, 0.0}) - 3.0) < 0.6, "distance outside the cube is wrong");
    require(Value(node, {0.0, 0.0, 9.0}) < 0.0 && Value(node, {0.0, 0.0, 11.0}) > 0.0,
            "the sign does not flip at the surface");

    // Rebuilding from the field returns the same solid.
    requireVolume(node, 20.0 * 20.0 * 20.0, 0.03, "cube rebuilt from its field");

    // Fields take part in the same arithmetic: a hole drilled through the cube.
    const auto drilled = Subtract(node, Cylinder({0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, 3.0, 20.0));
    requireVolume(drilled, 8000.0 - kPiValue * 9.0 * 20.0, 0.03, "cube with a drilled hole");

    QString emptyError;
    require(BakeMeshField(nullptr, 0.25, 2.0, nullptr, &emptyError) == nullptr && !emptyError.isEmpty(),
            "an empty mesh was not reported");
}

// The default options (smoothing + repair) still give a valid closed solid.
void testRepairedOutput()
{
    const BuildResult result = Build(Sphere({0.0, 0.0, 0.0}, 10.0), nullptr, 0.3);
    require(result.ok, "repaired sphere could not be built: " + result.error.toStdString());
    const MeshCheck check = MeshRepairCore::Analyze(result.mesh);
    require(check.Valid(), "the repaired sphere is not a valid closed solid: " + check.Summary().toStdString());
    const double volume = volumeOf(result.mesh);
    require(std::abs(volume - 4.0 / 3.0 * kPiValue * 1000.0) < 0.03 * 4.0 / 3.0 * kPiValue * 1000.0,
            "smoothing and repair changed the sphere volume too much: " + std::to_string(volume));
}

void testCancellation()
{
    std::atomic<bool> cancel{true};
    double bounds[6] = {-10.0, 10.0, -10.0, 10.0, -10.0, 10.0};
    require(Evaluate(Sphere({0.0, 0.0, 0.0}, 5.0), bounds, 0.3, &cancel) == nullptr,
            "evaluation ignored the cancel flag");
    const BuildResult result = Build(Sphere({0.0, 0.0, 0.0}, 5.0), bounds, 0.3, {}, &cancel);
    require(!result.ok && !result.error.isEmpty(), "build ignored the cancel flag");
    const auto cube = boxMesh({-5.0, 5.0, -5.0, 5.0, -5.0, 5.0}, false, false);
    QString error;
    require(BakeMeshField(cube, 0.3, 1.0, &cancel, &error) == nullptr, "baking ignored the cancel flag");
}
} // namespace

int main()
{
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"point values", testPointValues},
        {"primitive volumes", testPrimitiveVolumes},
        {"booleans", testBooleans},
        {"transform and bounds", testTransformAndBounds},
        {"mesh field", testMeshField},
        {"repaired output", testRepairedOutput},
        {"cancellation", testCancellation},
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
