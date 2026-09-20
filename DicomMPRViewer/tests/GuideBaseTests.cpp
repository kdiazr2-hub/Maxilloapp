#include "GuideBaseCore.h"

#include "ImplicitCore.h"
#include "MeshRepairCore.h"
#include "SplintTestGeometry.h"

#include <vtkMassProperties.h>
#include <vtkPolyData.h>
#include <vtkSphereSource.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>
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

vtkSmartPointer<vtkPolyData> sphereMesh(double radius)
{
    auto source = vtkSmartPointer<vtkSphereSource>::New();
    source->SetRadius(radius);
    source->SetThetaResolution(80);
    source->SetPhiResolution(80);
    auto tri = vtkSmartPointer<vtkTriangleFilter>::New();
    tri->SetInputConnection(source->GetOutputPort());
    tri->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(tri->GetOutput());
    return out;
}

vtkSmartPointer<vtkPolyData> transformed(vtkPolyData* mesh, vtkTransform* transform)
{
    auto filter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
    filter->SetInputData(mesh);
    filter->SetTransform(transform);
    filter->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(filter->GetOutput());
    return out;
}

double volumeOf(vtkPolyData* mesh)
{
    auto mass = vtkSmartPointer<vtkMassProperties>::New();
    mass->SetInputData(mesh);
    mass->Update();
    return mass->GetVolume();
}

// Ring of points on a sphere at a given polar angle: the region the surgeon would mark.
GuideContour capContour(double radius, double polarDegrees, int count)
{
    GuideContour contour;
    const double polar = polarDegrees * kPiValue / 180.0;
    for (int k = 0; k < count; ++k) {
        const double theta = 2.0 * kPiValue * k / count;
        contour.push_back({radius * std::sin(polar) * std::cos(theta), radius * std::sin(polar) * std::sin(theta),
                           radius * std::cos(polar)});
    }
    return contour;
}

GuideBaseParams params(double thickness, double clearance, double detail = 0.3)
{
    GuideBaseParams p;
    p.thicknessMm = thickness;
    p.clearanceMm = clearance;
    p.smallestDetailMm = detail;
    // Square rim: these tests measure the wall itself; the tapered, rounded finish has its own test.
    p.edgeTaperMm = 0.0;
    p.edgeRoundMm = 0.0;
    return p;
}

void testBaseFollowsTheAnatomy()
{
    const double radius = 20.0;
    const auto wrap = sphereMesh(radius);
    const GuideContour contour = capContour(radius, 40.0, 8);
    const double thickness = 2.5, clearance = 0.3;
    const GuideBaseResult result = GuideBaseCore::CreateBase(wrap, contour, params(thickness, clearance));
    require(result.ok, "the base could not be created: " + result.error.toStdString());
    require(MeshRepairCore::Analyze(result.mesh).Valid(), "the base is not a valid closed solid");
    require(result.projectionAxis[2] > 0.95, "the projection axis does not point out of the marked patch");

    // Uniform thickness: every vertex sits between the clearance and the far face, measured as a
    // distance from the sphere, and the patch does not spill past the marked ring.
    const double tolerance = 0.7; // voxel plus smoothing
    const double rimRadius = radius * std::sin(40.0 * kPiValue / 180.0);
    double p[3] = {};
    for (vtkIdType i = 0; i < result.mesh->GetNumberOfPoints(); ++i) {
        result.mesh->GetPoint(i, p);
        const double distance = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) - radius;
        require(distance > clearance - tolerance && distance < clearance + thickness + tolerance,
                "the base wall is not of uniform thickness: " + std::to_string(distance) + " mm from the wrap");
        require(std::hypot(p[0], p[1]) < rimRadius + tolerance,
                "the base spills outside the marked outline: " + std::to_string(std::hypot(p[0], p[1])) + " mm");
        require(p[2] > 0.0, "the base also grew on the far side of the sphere");
    }
    // A spherical cap of that angle, walled at that thickness.
    const double capArea = 2.0 * kPiValue * radius * radius * (1.0 - std::cos(40.0 * kPiValue / 180.0));
    const double volume = volumeOf(result.mesh);
    require(std::abs(volume - capArea * thickness) < 0.2 * capArea * thickness,
            "the base volume does not match the marked cap: " + std::to_string(volume));
}

void testUniformOnAnObliqueWall()
{
    // A slab tilted 30 degrees: an interval along a fixed axis would thicken by 1/cos(30) = 15%.
    auto tilt = vtkSmartPointer<vtkTransform>::New();
    tilt->RotateX(30.0);
    const auto wrap = transformed(boxMesh({-20.0, 20.0, -20.0, 20.0, -6.0, 0.0}, false, false), tilt);
    const double thickness = 2.0, clearance = 0.0;
    // Marked patch on the tilted top face.
    GuideContour contour;
    for (const auto& corner : std::vector<std::array<double, 2>>{{-8.0, -8.0}, {8.0, -8.0}, {8.0, 8.0}, {-8.0, 8.0}}) {
        double in[4] = {corner[0], corner[1], 0.0, 1.0};
        double out[4] = {};
        tilt->GetMatrix()->MultiplyPoint(in, out);
        contour.push_back({out[0], out[1], out[2]});
    }
    const GuideBaseResult result = GuideBaseCore::CreateBase(wrap, contour, params(thickness, clearance, 0.25));
    require(result.ok, "the base on the oblique wall failed: " + result.error.toStdString());
    require(MeshRepairCore::Analyze(result.mesh).Valid(), "the oblique base is not a valid closed solid");

    // Measured against the wrap itself, not along an axis.
    const auto field = ImplicitCore::BakeMeshField(wrap, 0.2, 8.0);
    require(field != nullptr, "the check field could not be baked");
    const double tolerance = 0.6;
    double p[3] = {};
    double deepest = 0.0;
    for (vtkIdType i = 0; i < result.mesh->GetNumberOfPoints(); ++i) {
        result.mesh->GetPoint(i, p);
        const double distance = field->At({p[0], p[1], p[2]});
        deepest = std::max(deepest, distance);
        require(distance > clearance - tolerance && distance < clearance + thickness + tolerance,
                "the wall thickens on the oblique surface: " + std::to_string(distance) + " mm");
    }
    require(deepest > thickness - tolerance, "the base never reaches its full thickness");
}

void testRejectsBadInput()
{
    const auto wrap = sphereMesh(20.0);
    const GuideContour contour = capContour(20.0, 40.0, 8);
    QString error;
    require(!GuideBaseCore::ContourValid({contour[0], contour[1]}, &error) && !error.isEmpty(),
            "two points were accepted");
    require(!GuideBaseCore::CreateBase(wrap, {contour[0], contour[1]}).ok, "a two-point base was built");
    require(!GuideBaseCore::CreateBase(nullptr, contour).ok, "a base without a wrap was built");
    // All the points on one spot: no area to sweep.
    require(!GuideBaseCore::ContourValid({contour[0], contour[0], contour[0]}), "coincident points were accepted");

    std::atomic<bool> cancel{true};
    const GuideBaseResult cancelled = GuideBaseCore::CreateBase(wrap, contour, params(2.5, 0.3), &cancel);
    require(!cancelled.ok && !cancelled.error.isEmpty(), "the base ignored the cancel flag");

    require(GuideBaseCore::ContourFromJson(GuideBaseCore::ContourToJson(contour)) == contour,
            "the contour did not survive the JSON round trip");
    require(GuideBaseCore::ContourPolyline(contour)->GetNumberOfLines() == 1, "the contour polyline is missing");
}

void testStaysOnTheMarkedFace()
{
    // A thin wall, 2 mm thick: the marked face is its top. Anything behind it — under the back face — is not the
    // tissue the user selected, even though it lies within the region and within the wall's reach.
    const auto plate = boxMesh({-20.0, 20.0, -20.0, 20.0, -2.0, 0.0}, false, false);
    const GuideContour square = {{-12.0, -12.0, 0.0}, {12.0, -12.0, 0.0}, {12.0, 12.0, 0.0}, {-12.0, 12.0, 0.0}};
    const GuideBaseResult result = GuideBaseCore::CreateBase(plate, square, params(2.5, 0.3, 0.25));
    require(result.ok, "the base on the thin wall failed: " + result.error.toStdString());
    require(result.projectionAxis[2] > 0.95, "the axis does not point out of the marked face");
    double b[6] = {};
    result.mesh->GetBounds(b);
    require(b[4] > -0.6, "the base grew behind the marked face, down to z = " + std::to_string(b[4]));

    // Marked past the edge of the wall: the rim wraps the edge by no more than the wall, never down the back.
    const GuideContour pastEdge = {{-26.0, -12.0, 0.0}, {12.0, -12.0, 0.0}, {12.0, 12.0, 0.0}, {-26.0, 12.0, 0.0}};
    const GuideBaseResult lip = GuideBaseCore::CreateBase(plate, pastEdge, params(2.5, 0.3, 0.25));
    require(lip.ok, "the base marked past the edge failed: " + lip.error.toStdString());
    lip.mesh->GetBounds(b);
    const double wallReach = 0.3 + 2.5 + 0.7;
    require(b[4] > -wallReach, "the rim ran down the side past its own thickness, to z = " + std::to_string(b[4]));
}

void testRoundedOutline()
{
    const auto plate = boxMesh({-20.0, 20.0, -20.0, 20.0, -2.0, 0.0}, false, false);
    const GuideContour square = {{-12.0, -12.0, 0.0}, {12.0, -12.0, 0.0}, {12.0, 12.0, 0.0}, {-12.0, 12.0, 0.0}};
    GuideBaseParams rounded = params(2.5, 0.0, 0.25);
    rounded.cornerRadiusMm = 4.0;
    const GuideBaseResult result = GuideBaseCore::CreateBase(plate, square, rounded);
    require(result.ok, "the rounded base failed: " + result.error.toStdString());
    // The square's corners are cut by the radius: nothing reaches the exact corner, but the edges keep their place.
    double p[3] = {};
    double nearestCorner = 1e9, farthestEdge = 0.0;
    for (vtkIdType i = 0; i < result.mesh->GetNumberOfPoints(); ++i) {
        result.mesh->GetPoint(i, p);
        nearestCorner = std::min(nearestCorner, std::hypot(12.0 - std::abs(p[0]), 12.0 - std::abs(p[1])));
        farthestEdge = std::max(farthestEdge, std::abs(p[0]));
    }
    require(nearestCorner > 0.3 * rounded.cornerRadiusMm, "the corners of the marked outline were not rounded");
    require(farthestEdge > 11.0 && farthestEdge < 12.8, "rounding moved the straight edges: " + std::to_string(farthestEdge));

    // The outline shown while marking is the rounded one, laid on the surface.
    const auto field = ImplicitCore::BakeMeshField(plate, 0.25, 6.0);
    const GuideRegion region = GuideBaseCore::MakeRegion(field, square, rounded);
    require(region.valid, "the region could not be made: " + region.error.toStdString());
    const auto outline = GuideBaseCore::RegionOutline(region, 0.3);
    require(outline->GetNumberOfPoints() > 20 && outline->GetNumberOfLines() >= 1, "the rounded outline is missing");
    double ob[6] = {};
    outline->GetBounds(ob);
    require(std::abs(ob[4] - 0.3) < 0.4 && std::abs(ob[5] - 0.3) < 0.4, "the outline does not lie on the surface");
    require(ob[1] < 12.5 && ob[1] > 11.0, "the outline does not follow the marked edge");

    // A region smaller than the rounding is refused with a reason.
    const GuideContour tiny = {{-1.0, -1.0, 0.0}, {1.0, -1.0, 0.0}, {1.0, 1.0, 0.0}, {-1.0, 1.0, 0.0}};
    const GuideRegion none = GuideBaseCore::MakeRegion(field, tiny, rounded);
    require(!none.valid && !none.error.isEmpty(), "a region smaller than the rounding was accepted");
}

void testTaperedRoundedRim()
{
    const auto plate = boxMesh({-25.0, 25.0, -25.0, 25.0, -2.0, 0.0}, false, false);
    const GuideContour square = {{-12.0, -12.0, 0.0}, {12.0, -12.0, 0.0}, {12.0, 12.0, 0.0}, {-12.0, 12.0, 0.0}};
    GuideBaseParams finish = params(3.0, 0.2, 0.25);
    finish.cornerRadiusMm = 0.0;
    finish.smoothingIterations = 0;
    finish.edgeTaperMm = 4.0;
    finish.edgeThicknessFraction = 0.4;
    finish.edgeRoundMm = 1.0;
    const GuideBaseResult result = GuideBaseCore::CreateBase(plate, square, finish);
    require(result.ok, "the tapered base failed: " + result.error.toStdString());
    require(MeshRepairCore::Analyze(result.mesh).Valid(), "the tapered base is not a valid closed solid");

    // Highest point of the outer face in bands measured from the rim at |x| = 12.
    const auto topIn = [&](double fromRim, double toRim) {
        double top = -1e9;
        double p[3] = {};
        for (vtkIdType i = 0; i < result.mesh->GetNumberOfPoints(); ++i) {
            result.mesh->GetPoint(i, p);
            const double rim = 12.0 - std::max(std::abs(p[0]), std::abs(p[1]));
            if (rim >= fromRim && rim <= toRim)
                top = std::max(top, p[2]);
        }
        return top;
    };
    const double centre = topIn(8.0, 12.0);
    const double middle = topIn(3.5, 4.5);
    const double edge = topIn(-1.0, 0.7);
    require(centre > 0.2 + 3.0 - 0.4, "the centre of the guide lost its thickness: " + std::to_string(centre));
    require(middle > 0.2 + 3.0 * 0.8, "the wall thins too early: " + std::to_string(middle));
    require(edge < 0.2 + 3.0 * 0.6, "the wall does not thin towards the rim: " + std::to_string(edge));
    require(edge < middle && middle <= centre + 0.1, "the thickness does not fall off towards the rim");

    // With the finish off the rim stays square: full thickness right to the edge.
    GuideBaseParams square_ = finish;
    square_.edgeTaperMm = 0.0;
    square_.edgeRoundMm = 0.0;
    const GuideBaseResult squareRim = GuideBaseCore::CreateBase(plate, square, square_);
    require(squareRim.ok, "the square-rim base failed");
    double b[6] = {};
    squareRim.mesh->GetBounds(b);
    double edgeTop = -1e9, p[3] = {};
    for (vtkIdType i = 0; i < squareRim.mesh->GetNumberOfPoints(); ++i) {
        squareRim.mesh->GetPoint(i, p);
        if (12.0 - std::max(std::abs(p[0]), std::abs(p[1])) < 0.7)
            edgeTop = std::max(edgeTop, p[2]);
    }
    require(edgeTop > 0.2 + 3.0 - 0.4, "with the finish off the rim should keep its thickness");
}

// Builds the base of a brushed region on its own, as GuideDesignCore does.
vtkSmartPointer<vtkPolyData> brushedBase(vtkPolyData* wrap, const GuideBrushPaint& paint, const GuideBaseParams& p,
                                        GuideRegion* regionOut = nullptr)
{
    const auto field = ImplicitCore::BakeMeshField(wrap, p.smallestDetailMm, 6.0);
    const GuideRegion region = GuideBaseCore::MakeBrushRegion(field, paint, p);
    if (regionOut)
        *regionOut = region;
    if (!region.valid)
        return nullptr;
    const auto node = GuideBaseCore::BaseNode(field, region, p, 80.0);
    double bounds[6] = {};
    if (!ImplicitCore::Bounds(node, bounds))
        return nullptr;
    ImplicitCore::PolygonizeOptions options;
    options.smoothingIterations = 0;
    options.repair = false; // volumes are compared
    const ImplicitCore::BuildResult built = ImplicitCore::Build(node, bounds, p.smallestDetailMm, options);
    return built.ok ? built.mesh : nullptr;
}

void testBrushedRegion()
{
    // A thin wall: the brush paints its top face only.
    const auto plate = boxMesh({-25.0, 25.0, -25.0, 25.0, -2.0, 0.0}, false, false);
    GuideBaseParams p = params(2.5, 0.2, 0.25);
    const double radius = 4.0;
    GuideBrushPaint paint;
    for (double x = -8.0; x <= 8.0; x += 2.0) // a stroke along x, painted in two passes
        for (double y : {-2.0, 2.0})
            paint.push_back({{x, y, 0.0}, radius, false});

    GuideRegion region;
    const auto base = brushedBase(plate, paint, p, &region);
    require(region.valid, "the brushed region was refused: " + region.error.toStdString());
    require(region.axis[2] > 0.95, "the brushed patch does not face out of the wall");
    require(base && base->GetNumberOfPolys() > 0, "the brushed base is empty");
    double b[6] = {};
    base->GetBounds(b);
    require(b[4] > -0.6, "the brushed base grew behind the wall, down to z = " + std::to_string(b[4]));
    require(b[5] < 0.2 + 2.5 + 0.6, "the brushed base is thicker than the wall");
    // It covers the painted band — the dabs' discs — and not more.
    require(b[0] > -8.0 - radius - 0.8 && b[1] < 8.0 + radius + 0.8 && b[2] > -2.0 - radius - 0.8 &&
                b[3] < 2.0 + radius + 0.8,
            "the brushed base spills past the painted dabs: [" + std::to_string(b[0]) + ", " +
                std::to_string(b[1]) + "] x [" + std::to_string(b[2]) + ", " + std::to_string(b[3]) + "]");
    require(b[1] > 8.0 + radius - 1.0 && b[3] > 2.0 + radius - 1.0, "the brushed base does not reach the painted edge");
    // Area of a stadium 16 mm long, 4 + 2r wide, walled at 2.5 mm.
    const double area = 16.0 * (4.0 + 2.0 * radius) + kPiValue * (2.0 + radius) * (2.0 + radius);
    const double volume = volumeOf(base);
    require(volume > 0.7 * area * 2.5 && volume < 1.2 * area * 2.5,
            "the brushed base volume " + std::to_string(volume) + " does not match the painted band");

    // Erasing with the brush takes the material away where it passes.
    GuideBrushPaint erased = paint;
    erased.push_back({{0.0, 0.0, 0.0}, 3.0, true});
    const auto withHole = brushedBase(plate, erased, p);
    require(withHole && volumeOf(withHole) < volume - 0.5 * kPiValue * 9.0 * 2.5,
            "erasing with the brush did not remove material");
    const auto holeField = ImplicitCore::BakeMeshField(withHole, 0.15, 3.0);
    require(holeField->At({0.0, 0.0, 1.45}) > 0.0, "the erased spot is still solid");

    // Painting the same dabs in another order gives the same guide.
    GuideBrushPaint reversed(paint.rbegin(), paint.rend());
    const auto again = brushedBase(plate, reversed, p);
    require(again && std::abs(volumeOf(again) - volume) < 0.01 * volume, "the painting order changed the guide");

    // Only erasing is not a region; the paint survives JSON.
    require(!GuideBaseCore::PaintValid({{{0.0, 0.0, 0.0}, 3.0, true}}), "a region made only of erasing was accepted");
    const GuideBrushPaint back = GuideBaseCore::PaintFromJson(GuideBaseCore::PaintToJson(erased));
    require(back.size() == erased.size() && back.back().erase && back.back().center == erased.back().center &&
                std::abs(back.front().radiusMm - radius) < 1e-9,
            "the brush paint did not survive JSON");
}
} // namespace

int main()
{
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"base follows the anatomy", testBaseFollowsTheAnatomy},
        {"uniform on an oblique wall", testUniformOnAnObliqueWall},
        {"rejects bad input", testRejectsBadInput},
        {"stays on the marked face", testStaysOnTheMarkedFace},
        {"rounded outline", testRoundedOutline},
        {"brushed region", testBrushedRegion},
        {"tapered rounded rim", testTaperedRoundedRim},
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
