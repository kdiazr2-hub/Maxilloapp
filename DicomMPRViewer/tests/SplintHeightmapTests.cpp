#include "SplintHeightmapGenerator.h"
#include "SplintTestGeometry.h"

#include <vtkSphereSource.h>
#include <vtkCellArray.h>
#include <vtkFeatureEdges.h>
#include <vtkFloatArray.h>
#include <vtkImplicitPolyDataDistance.h>
#include <vtkMassProperties.h>
#include <vtkAppendPolyData.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>
#include <vtkTriangleFilter.h>
#include <vtkActor.h>
#include <vtkCamera.h>
#include <vtkPNGWriter.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkSTLWriter.h>
#include <vtkWindowToImageFilter.h>

#include <atomic>
#include <filesystem>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using namespace splinttest;

SplintHeightmapResult generateOk(const SplintHeightmapInputs& inputs, const std::string& what)
{
    SplintHeightmapResult result = SplintHeightmapGenerator::Generate(inputs);
    require(result.ok, what + ": generation failed: " + result.error.toStdString());
    require(result.mesh && result.mesh->GetNumberOfPolys() > 0, what + ": empty mesh");
    return result;
}

vtkIdType openEdges(vtkPolyData* mesh)
{
    auto edges = vtkSmartPointer<vtkFeatureEdges>::New();
    edges->SetInputData(mesh);
    edges->BoundaryEdgesOn();
    edges->NonManifoldEdgesOn();
    edges->FeatureEdgesOff();
    edges->ManifoldEdgesOff();
    edges->Update();
    return edges->GetOutput()->GetNumberOfCells();
}

double volumeOf(vtkPolyData* mesh)
{
    auto mass = vtkSmartPointer<vtkMassProperties>::New();
    mass->SetInputData(mesh);
    mass->Update();
    return mass->GetVolume();
}

bool insideMesh(vtkPolyData* mesh, double x, double y, double z)
{
    auto distance = vtkSmartPointer<vtkImplicitPolyDataDistance>::New();
    distance->SetInput(mesh);
    double p[3] = {x, y, z};
    return distance->EvaluateFunction(p) < 0.0;
}

double contourArea(const SplintHeightmapResult& result)
{
    double area = 0.0;
    for (const auto& c : result.contoursUV)
        area += SplintHeightmapGenerator::ContourArea(c);
    return area;
}

double contourPerimeter(const SplintHeightmapResult& result)
{
    double perimeter = 0.0;
    for (const auto& c : result.contoursUV)
        for (size_t i = 0; i < c.size(); ++i) {
            const auto& a = c[i];
            const auto& b = c[(i + 1) % c.size()];
            perimeter += std::hypot(b[0] - a[0], b[1] - a[1]);
        }
    return perimeter;
}

void testClosedMeshAndFrame()
{
    for (double fillet : {0.0, 0.5, 1.0}) {
        for (double offset : {1.0, 3.0}) {
            Scene scene = makeScene();
            scene.inputs.params.filletMm = fillet;
            scene.inputs.params.edgeOffsetMm = offset;
            const auto result = generateOk(scene.inputs, "closed mesh");
            const vtkIdType open = openEdges(result.mesh);
            if (open != 0)
                std::cerr << "fillet=" << fillet << " offset=" << offset << " open edges=" << open << '\n';
            require(open == 0, "splint mesh has boundary or non-manifold edges");
            require(result.frame.normal[2] > 0.99, "occlusal normal does not point to the maxilla");
            require(result.frame.anteriorResolved && result.frame.axisV[1] > 0.9, "anterior direction not resolved");
            require(result.contoursUV.size() == 1, "expected a single U-shaped contour");
        }
    }

    Scene scene = makeScene();
    const auto result = generateOk(scene.inputs, "thickness");
    auto* thickness = vtkFloatArray::SafeDownCast(
        result.mesh->GetPointData()->GetArray(SplintHeightmapGenerator::ThicknessArrayName));
    require(thickness && thickness->GetNumberOfTuples() == result.mesh->GetNumberOfPoints(), "missing thickness array");
    require(result.p05ThicknessMm > 0.5 && result.p05ThicknessMm < 3.0, "implausible thickness percentile");
    SplintHeightmapGenerator::ApplyThicknessColors(result.mesh, 1.5, 3.0);
    require(result.mesh->GetPointData()->GetScalars() != nullptr, "thickness colours not applied");
    require(result.contourWorld && result.contourWorld->GetNumberOfLines() == 1, "missing world contour");
}

void testDefaultResolution()
{
    Scene scene = makeScene();
    scene.inputs.params.gridResolutionMm = 0.2;
    const auto start = std::chrono::steady_clock::now();
    const auto result = generateOk(scene.inputs, "default resolution");
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    require(openEdges(result.mesh) == 0, "default-resolution mesh is not closed");
    std::cout << "  0.2 mm grid: " << ms << " ms, " << result.mesh->GetNumberOfPolys() << " triangles\n"
              << "  " << result.report.toStdString() << '\n';
}

vtkSmartPointer<vtkImplicitPolyDataDistance> distanceTo(vtkPolyData* mesh)
{
    auto distance = vtkSmartPointer<vtkImplicitPolyDataDistance>::New();
    distance->SetInput(mesh);
    return distance;
}

bool inside(vtkImplicitPolyDataDistance* distance, double x, double y, double z)
{
    double p[3] = {x, y, z};
    return distance->EvaluateFunction(p) < 0.0;
}

void testImpression()
{
    Scene with = makeScene();
    const auto a = generateOk(with.inputs, "impression on");
    const auto c3 = toothCenter(3);
    const auto c0 = toothCenter(0);
    require(!insideMesh(a.mesh, c3[0], c3[1], 3.0), "upper tooth is embedded in the splint with impressions on");
    require(!insideMesh(a.mesh, c3[0], c3[1], -3.0), "lower tooth is embedded in the splint with impressions on");
    require(insideMesh(a.mesh, c3[0], c3[1], 0.0), "no occlusal material between the arches");
    // Within the guaranteed half edge offset (contour smoothing may round the rest).
    require(insideMesh(a.mesh, c0[0] + 2.5 + 0.3, c0[1], 3.0), "impression walls do not wrap the upper teeth");

    // Without impression each side is flat at cusp level and never enters a tooth.
    Scene without = makeScene();
    without.inputs.params.impressionUpper = false;
    without.inputs.params.impressionLower = false;
    const auto b = generateOk(without.inputs, "impression off");
    require(openEdges(b.mesh) == 0, "flat splint is not closed");
    const auto distance = distanceTo(b.mesh);
    for (int k = 0; k < kTeeth; ++k) {
        const auto c = toothCenter(k);
        for (double dx : {-2.2, 0.0, 2.2})
            for (double dy : {-2.2, 0.0, 2.2})
                for (double z : {1.1, 2.0, 4.0}) {
                    require(!inside(distance, c[0] + dx, c[1] + dy, z), "flat splint enters an upper tooth");
                    require(!inside(distance, c[0] + dx, c[1] + dy, -z), "flat splint enters a lower tooth");
                }
    }
    double bounds[6] = {};
    b.mesh->GetBounds(bounds);
    std::cout << "  flat splint z range: [" << bounds[4] << ", " << bounds[5] << "]\n";
    require(bounds[5] < 1.0 && bounds[5] > 0.6, "flat top is not at cusp level");
    require(bounds[4] > -1.0 && bounds[4] < -0.6, "flat bottom is not at cusp level");
    require(bounds[5] - bounds[4] < 2.0, "flat splint still wraps the teeth");
}

void testHorizontalClearance()
{
    constexpr double clearance = 0.8;
    constexpr double grid = 0.2;
    Scene scene = makeScene();
    scene.inputs.params.gridResolutionMm = grid;
    scene.inputs.params.clearanceMm = clearance;
    scene.inputs.params.edgeOffsetMm = 3.0;
    scene.inputs.params.computeThickness = false;
    const auto result = generateOk(scene.inputs, "horizontal clearance");
    const auto distance = distanceTo(result.mesh);

    // Buccal (+x) vertical wall of the most distal upper tooth.
    const auto c = toothCenter(0);
    const double wallX = c[0] + 2.5;
    double gap = -1.0;
    for (double d = 0.0; d <= 2.5; d += 0.05) {
        if (inside(distance, wallX + d, c[1], 3.0)) {
            gap = d;
            break;
        }
    }
    std::cout << "  horizontal gap at a vertical tooth wall: " << gap << " mm (clearance " << clearance << ")\n";
    require(gap >= clearance - grid, "horizontal clearance smaller than requested");
    require(gap <= clearance + 3 * grid, "no splint wall next to the tooth");
    require(!inside(distance, c[0], c[1], 1.0 - clearance + grid), "vertical clearance smaller than requested");
}

// Minimum thickness of the vertices inside an xy box, excluding the flange
// rims at the point surfaces (|z| >= 4).
double minThicknessIn(const SplintHeightmapResult& result, double x0, double x1, double y0, double y1,
                      std::array<double, 3>* where = nullptr)
{
    auto* thickness = vtkFloatArray::SafeDownCast(
        result.mesh->GetPointData()->GetArray(SplintHeightmapGenerator::ThicknessArrayName));
    require(thickness != nullptr, "missing thickness array");
    double best = 1e9;
    double p[3] = {};
    for (vtkIdType i = 0; i < result.mesh->GetNumberOfPoints(); ++i) {
        result.mesh->GetPoint(i, p);
        if (std::abs(p[2]) >= 4.0 || p[0] < x0 || p[0] > x1 || p[1] < y0 || p[1] > y1)
            continue;
        if (thickness->GetValue(i) < best) {
            best = thickness->GetValue(i);
            if (where)
                *where = {p[0], p[1], p[2]};
        }
    }
    return best;
}

void testThinFins()
{
    // Incisors k=5 / k=6 are 6.26 mm apart: half size 2.78 leaves a 0.7 mm
    // embrasure, i.e. a 0.5 mm splint fin after the 0.1 mm clearances.
    ArchOptions narrow;
    narrow.toothHalf = 2.78;
    const auto c5 = toothCenter(5);
    const double finX = 0.0;
    const double finY = c5[1];

    Scene keep = makeScene(narrow);
    keep.inputs.params.gridResolutionMm = 0.2;
    keep.inputs.params.minFeatureMm = 0.0;
    Scene removed = makeScene(narrow);
    removed.inputs.params.gridResolutionMm = 0.2;
    removed.inputs.params.minFeatureMm = 0.8;
    const auto a = generateOk(keep.inputs, "fins kept");
    const auto b = generateOk(removed.inputs, "fins removed");

    require(insideMesh(a.mesh, finX, finY, 3.0), "test geometry has no embrasure fin to remove");
    require(!insideMesh(b.mesh, finX, finY, 3.0), "minimum feature filter kept the upper embrasure fin");
    require(!insideMesh(b.mesh, finX, finY, -3.0), "minimum feature filter kept the lower embrasure fin");

    const double localA = minThicknessIn(a, finX - 1.5, finX + 1.5, finY - 3.0, finY + 3.0);
    const double localB = minThicknessIn(b, finX - 1.5, finX + 1.5, finY - 3.0, finY + 3.0);
    std::array<double, 3> whereA{}, whereB{};
    const double globalA = minThicknessIn(a, -1e9, 1e9, -1e9, 1e9, &whereA);
    const double globalB = minThicknessIn(b, -1e9, 1e9, -1e9, 1e9, &whereB);
    const double ratio = volumeOf(b.mesh) / volumeOf(a.mesh);
    std::cout << "  embrasure min thickness: " << localA << " mm -> " << localB << " mm\n"
              << "  interior min thickness: " << globalA << " mm at (" << whereA[0] << ", " << whereA[1] << ", " << whereA[2]
              << ") -> " << globalB << " mm at (" << whereB[0] << ", " << whereB[1] << ", " << whereB[2] << ")\n"
              << "  volume ratio " << ratio << '\n';
    require(localB > 0.34 && localB > localA, "embrasure thickness did not increase");
    require(std::abs(ratio - 1.0) < 0.03, "minimum feature filter changed the volume too much");
    require(openEdges(b.mesh) == 0, "fin-filtered mesh is not closed");
}

void testPointsControlHeight()
{
    Scene low = makeScene({}, 5.0, -5.0);
    Scene high = makeScene({}, 7.0, -5.0);
    const auto a = generateOk(low.inputs, "points z=5");
    const auto b = generateOk(high.inputs, "points z=7");
    double ba[6] = {}, bb[6] = {};
    a.mesh->GetBounds(ba);
    b.mesh->GetBounds(bb);
    const double rise = bb[5] - ba[5];
    std::cout << "  top rise for +2 mm points: " << rise << " mm\n";
    require(rise > 1.5 && rise < 2.5, "raising the upper points did not raise the splint top by ~2 mm");
    require(volumeOf(b.mesh) > volumeOf(a.mesh), "raising the upper points did not add material");
    require(std::abs(ba[4] - bb[4]) < 0.5, "moving upper points changed the lower side");
}

void testEdgeOffset()
{
    Scene one = makeScene();
    Scene two = makeScene();
    one.inputs.params.edgeOffsetMm = 1.0;
    two.inputs.params.edgeOffsetMm = 2.0;
    const auto a = generateOk(one.inputs, "offset 1");
    const auto b = generateOk(two.inputs, "offset 2");
    const double gain = (contourArea(b) - contourArea(a)) / contourPerimeter(a);
    std::cout << "  area gain per mm of offset / perimeter: " << gain << '\n';
    require(gain > 0.7 && gain < 1.3, "edge offset does not grow the contour by ~1 mm");
}

void testOpenScan()
{
    ArchOptions open;
    open.openScan = true;
    Scene closedScene = makeScene();
    Scene openScene = makeScene(open);
    const auto a = generateOk(closedScene.inputs, "closed scan");
    const auto b = generateOk(openScene.inputs, "open scan");
    require(openEdges(b.mesh) == 0, "open scan produced an open splint");
    const double ratio = volumeOf(b.mesh) / volumeOf(a.mesh);
    std::cout << "  open/closed volume ratio: " << ratio << '\n';
    require(std::abs(ratio - 1.0) < 0.03, "open scan changes the splint volume");
}

void testUndercutDirection()
{
    ArchOptions mushroom;
    mushroom.mushroomUpper = true;
    const auto c = toothCenter(5);
    const double z = 6.5;

    Scene off = makeScene(mushroom, 8.0, -5.0);
    off.inputs.anteriorDirectionWorld = SplintPoint3{0.0, 1.0, 0.0};
    const auto a = generateOk(off.inputs, "undercut off");
    require(!insideMesh(a.mesh, c[0], c[1] - 2.2, z), "vertical insertion kept a posterior undercut");
    require(!insideMesh(a.mesh, c[0], c[1] + 2.2, z), "vertical insertion kept an anterior undercut");

    Scene on = makeScene(mushroom, 8.0, -5.0);
    on.inputs.anteriorDirectionWorld = SplintPoint3{0.0, 1.0, 0.0};
    on.inputs.params.undercutUpper = true;
    on.inputs.params.undercutAngleUpperDeg = 45.0;
    const auto b = generateOk(on.inputs, "undercut on");
    require(openEdges(b.mesh) == 0, "tilted insertion mesh is not closed");
    require(!insideMesh(b.mesh, c[0], c[1] + 2.2, z), "undercut toward the insertion direction was not removed");
    require(insideMesh(b.mesh, c[0], c[1] - 2.2, z), "undercut opposite to the insertion direction was not kept");
}

void testContourOverrideAndErrors()
{
    Scene scene = makeScene();
    const auto base = generateOk(scene.inputs, "base");

    SplintContourUV rectangle;
    for (const auto& corner : {std::array<double, 2>{-10, 14}, {10, 14}, {10, 24}, {-10, 24}}) {
        const auto local = base.frame.ToLocal({corner[0], corner[1], 0.0});
        rectangle.push_back({local[0], local[1]});
    }
    Scene edited = makeScene();
    edited.inputs.contourOverrideUV = {rectangle};
    const auto r = generateOk(edited.inputs, "contour override");
    require(r.contoursUV.size() == 1 && r.contoursUV.front().size() == 4, "override contour not kept");
    double b[6] = {};
    r.mesh->GetBounds(b);
    require(b[0] > -10.8 && b[1] < 10.8 && b[2] > 13.2 && b[3] < 24.8, "override contour not respected");
    require(openEdges(r.mesh) == 0, "override mesh is not closed");

    Scene missing = makeScene();
    missing.inputs.upperPoints.pop_back();
    auto e = SplintHeightmapGenerator::Generate(missing.inputs);
    require(!e.ok && e.error.contains(QStringLiteral("Faltan puntos superiores")), "missing points not reported");

    Scene cancelled = makeScene();
    std::atomic<bool> cancel{true};
    cancelled.inputs.cancel = &cancel;
    e = SplintHeightmapGenerator::Generate(cancelled.inputs);
    require(!e.ok && e.error.contains(QStringLiteral("cancelado")), "cancellation not reported");

    Scene outside = makeScene({}, 30.0, 20.0);
    e = SplintHeightmapGenerator::Generate(outside.inputs);
    require(!e.ok && e.error.contains(QStringLiteral("no cruzan")), "band without teeth not reported");
}
// Dense geometry far above the band (e.g. the maxillary bone of a composite)
// must be cropped before ray casting without changing the splint.
void testCropKeepsResult()
{
    Scene plain = makeScene();
    Scene withBone = makeScene();
    auto sphere = vtkSmartPointer<vtkSphereSource>::New();
    sphere->SetCenter(0.0, 10.0, 40.0);
    sphere->SetRadius(25.0);
    sphere->SetThetaResolution(300);
    sphere->SetPhiResolution(300);
    auto append = vtkSmartPointer<vtkAppendPolyData>::New();
    append->AddInputData(withBone.upper);
    append->AddInputConnection(sphere->GetOutputPort());
    append->Update();
    withBone.upper = vtkSmartPointer<vtkPolyData>::New();
    withBone.upper->DeepCopy(append->GetOutput());
    withBone.inputs.upperTeeth = withBone.upper;

    const auto a = generateOk(plain.inputs, "without bone");
    const auto start = std::chrono::steady_clock::now();
    const SplintHeightmapPrepared prep = SplintHeightmapGenerator::Prepare(withBone.inputs);
    const auto prepareMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    require(prep.ok, "prepare with bone failed: " + prep.error.toStdString());
    std::cout << "  upper triangles used " << prep.upperTrianglesUsed << " of " << prep.upperTrianglesTotal
              << ", prepare " << prepareMs << " ms\n";
    require(prep.upperTrianglesTotal > 100000, "test bone is not dense");
    require(prep.upperTrianglesUsed <= plain.upper->GetNumberOfPolys(), "bone above the band was not cropped");
    require(prep.lowerTrianglesUsed == prep.lowerTrianglesTotal, "lower teeth were cropped");

    const auto b = SplintHeightmapGenerator::Build(prep, withBone.inputs);
    require(b.ok, "build with bone failed: " + b.error.toStdString());
    const double ratio = volumeOf(b.mesh) / volumeOf(a.mesh);
    require(std::abs(ratio - 1.0) < 0.005, "cropping changed the splint");
}

std::array<double, 2> radialOf(int tooth)
{
    const auto c = toothCenter(tooth);
    const double len = std::hypot(c[0], c[1]);
    return {c[0] / len, c[1] / len};
}

void testBevel()
{
    Scene scene = makeScene();
    const auto plain = generateOk(scene.inputs, "no bevel");
    const auto c5 = toothCenter(5);
    const auto r5 = radialOf(5);
    const auto c0 = toothCenter(0);
    const auto r0 = radialOf(0);
    const auto at = [](const std::array<double, 2>& c, const std::array<double, 2>& r, double offset, double z) {
        return SplintPoint3{c[0] + r[0] * offset, c[1] + r[1] * offset, z};
    };
    const SplintPoint3 anteriorOuter = at(c5, r5, 2.8, 0.0);
    const SplintPoint3 posteriorOuter = at(c0, r0, 2.8, 0.0);
    require(insideMesh(plain.mesh, anteriorOuter[0], anteriorOuter[1], 0.0), "probe outside the splint without bevel");

    // Slanted plane from 0.5 mm (upper) to 2.5 mm (lower) outside the tooth centre.
    SplintBevel bevel{at(c5, r5, 0.5, 5.0), at(c5, r5, 2.5, -5.0)};
    SplintPoint3 origin{}, normal{};
    require(SplintHeightmapGenerator::BevelPlane(bevel, plain.frame, origin, normal), "bevel plane rejected");
    require(normal[1] > 0.5, "bevel normal does not point anteriorly");
    SplintBevel alongArch{at(toothCenter(4), radialOf(4), 3.0, 0.0), at(toothCenter(7), radialOf(7), 3.0, 0.0)};
    require(!SplintHeightmapGenerator::BevelPlane(alongArch, plain.frame, origin, normal),
            "a line along the arch must not define a bevel");

    scene.inputs.extras.bevel = bevel;
    const auto beveled = generateOk(scene.inputs, "bevel");
    require(openEdges(beveled.mesh) == 0, "beveled splint is not closed");
    require(!insideMesh(beveled.mesh, anteriorOuter[0], anteriorOuter[1], 0.0), "bevel did not remove the anterior rim");
    require(insideMesh(beveled.mesh, c5[0], c5[1], 0.0), "bevel removed occlusal material under the tooth");
    require(insideMesh(beveled.mesh, posteriorOuter[0], posteriorOuter[1], 0.0), "bevel removed posterior material");
    require(volumeOf(beveled.mesh) < volumeOf(plain.mesh), "bevel did not reduce the volume");
    require(beveled.report.contains(QStringLiteral("Bisel aplicado")), "report does not mention the bevel");
}

void testWireHoles()
{
    Scene scene = makeScene();
    const auto plain = generateOk(scene.inputs, "no holes");
    const auto c3 = toothCenter(3);
    const SplintPoint3 axis =
        SplintHeightmapGenerator::WireHoleAxis(SplintHoleOrientation::SurfaceNormal, {0.0, 0.0, -1.0}, std::nullopt, plain.frame);
    require(axis[2] > 0.99, "surface-normal hole axis should point to the maxilla");

    scene.inputs.extras.wireHoles.push_back({{c3[0], c3[1], 0.0}, axis, 1.5});
    const auto holed = generateOk(scene.inputs, "holes");
    require(openEdges(holed.mesh) == 0, "splint with a hole is not closed");
    require(insideMesh(plain.mesh, c3[0], c3[1], 0.0), "probe not in the splint");
    require(!insideMesh(holed.mesh, c3[0], c3[1], 0.0), "wire hole not subtracted");
    require(insideMesh(holed.mesh, c3[0] + 1.6, c3[1], 0.0), "wire hole removed too much");

    const auto c5 = toothCenter(5);
    const auto r5 = radialOf(5);
    const std::optional<SplintBevel> bevel = SplintBevel{
        {c5[0] + r5[0] * 0.5, c5[1] + r5[1] * 0.5, 5.0}, {c5[0] + r5[0] * 2.5, c5[1] + r5[1] * 2.5, -5.0}};
    SplintPoint3 origin{}, normal{};
    require(SplintHeightmapGenerator::BevelPlane(*bevel, plain.frame, origin, normal), "bevel plane rejected");
    const SplintPoint3 bevelAxis =
        SplintHeightmapGenerator::WireHoleAxis(SplintHoleOrientation::Bevel, {0.0, 0.0, 1.0}, bevel, plain.frame);
    require(std::abs(bevelAxis[0] * normal[0] + bevelAxis[1] * normal[1] + bevelAxis[2] * normal[2]) < 1e-6,
            "bevel-oriented hole is not parallel to the bevel face");
    require(bevelAxis[2] > 0.5, "bevel-oriented hole does not point to the maxilla");
}

void testBracketMarks()
{
    Scene scene = makeScene();
    const auto plain = generateOk(scene.inputs, "no marks");
    const auto c3 = toothCenter(3);
    const auto c8 = toothCenter(8);
    require(insideMesh(plain.mesh, c3[0], c3[1], 0.55), "upper probe not in the splint");
    require(insideMesh(plain.mesh, c8[0], c8[1], -0.55), "lower probe not in the splint");

    scene.inputs.extras.bracketOffsetMm = 0.8;
    scene.inputs.extras.bracketMarks.push_back({{c3[0], c3[1], 1.0}, 1.5});
    scene.inputs.extras.bracketMarks.push_back({{c8[0], c8[1], -1.0}, 1.5});
    const auto marked = generateOk(scene.inputs, "marks");
    require(openEdges(marked.mesh) == 0, "splint with bracket margins is not closed");
    require(!insideMesh(marked.mesh, c3[0], c3[1], 0.55), "upper bracket margin not applied");
    require(!insideMesh(marked.mesh, c8[0], c8[1], -0.55), "lower bracket margin not applied");
    require(insideMesh(marked.mesh, c8[0], c8[1], 0.55), "upper margin leaked to an unmarked tooth");
    require(insideMesh(marked.mesh, c3[0], c3[1], -0.55), "lower margin leaked to an unmarked tooth");
    require(marked.report.contains(QStringLiteral("brackets")), "report does not mention bracket margins");
}

// Optional visual check: writes the default-resolution splint as STL and a PNG
// (thickness colours over the teeth) into outputDir.
void writeArtifacts(const std::string& outputDir)
{
    Scene scene = makeScene();
    scene.inputs.params.gridResolutionMm = 0.2;
    const auto result = generateOk(scene.inputs, "artifacts");
    SplintHeightmapGenerator::ApplyThicknessColors(result.mesh, 1.5, 3.0);
    std::filesystem::create_directories(outputDir);

    auto stl = vtkSmartPointer<vtkSTLWriter>::New();
    stl->SetFileName((outputDir + "/splint-heightmap.stl").c_str());
    stl->SetInputData(result.mesh);
    stl->Write();

    auto window = vtkSmartPointer<vtkRenderWindow>::New();
    window->SetOffScreenRendering(1);
    window->SetSize(1400, 700);
    window->SetMultiSamples(0);
    const double viewports[2][4] = {{0.0, 0.0, 0.5, 1.0}, {0.5, 0.0, 1.0, 1.0}};
    for (int side = 0; side < 2; ++side) {
        auto renderer = vtkSmartPointer<vtkRenderer>::New();
        renderer->SetViewport(viewports[side]);
        renderer->SetBackground(0.10, 0.11, 0.13);
        auto addActor = [&](vtkPolyData* mesh, bool scalars, double opacity) {
            auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
            mapper->SetInputData(mesh);
            mapper->SetScalarVisibility(scalars);
            auto actor = vtkSmartPointer<vtkActor>::New();
            actor->SetMapper(mapper);
            actor->GetProperty()->SetColor(0.85, 0.85, 0.80);
            actor->GetProperty()->SetOpacity(opacity);
            renderer->AddActor(actor);
        };
        addActor(result.mesh, true, 1.0);
        if (side == 1) {
            addActor(scene.upper, false, 0.35);
            addActor(scene.lower, false, 0.35);
        }
        auto camera = renderer->GetActiveCamera();
        camera->SetFocalPoint(0, 10, 0);
        camera->SetPosition(side == 0 ? 0 : 55, side == 0 ? -10 : -60, side == 0 ? -90 : 45);
        camera->SetViewUp(0, side == 0 ? 1 : 0, side == 0 ? 0 : 1);
        renderer->ResetCamera();
        window->AddRenderer(renderer);
    }
    window->Render();
    auto capture = vtkSmartPointer<vtkWindowToImageFilter>::New();
    capture->SetInput(window);
    capture->ReadFrontBufferOff();
    capture->Update();
    auto png = vtkSmartPointer<vtkPNGWriter>::New();
    png->SetFileName((outputDir + "/splint-heightmap.png").c_str());
    png->SetInputConnection(capture->GetOutputPort());
    png->Write();
    window->Finalize();
    require(std::filesystem::exists(outputDir + "/splint-heightmap.png"), "PNG was not written");
}
} // namespace

int main(int argc, char** argv)
{
    if (argc > 1) {
        try {
            writeArtifacts(argv[1]);
            std::cout << "Artifacts written to " << argv[1] << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL artifacts: " << error.what() << '\n';
            return 1;
        }
    }

    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"closed mesh and frame", testClosedMeshAndFrame},
        {"default resolution", testDefaultResolution},
        {"impression", testImpression},
        {"horizontal clearance", testHorizontalClearance},
        {"thin fins", testThinFins},
        {"points control height", testPointsControlHeight},
        {"edge offset", testEdgeOffset},
        {"open scan", testOpenScan},
        {"undercut direction", testUndercutDirection},
        {"contour override and errors", testContourOverrideAndErrors},
        {"crop keeps result", testCropKeepsResult},
        {"bevel", testBevel},
        {"wire holes", testWireHoles},
        {"bracket marks", testBracketMarks},
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
