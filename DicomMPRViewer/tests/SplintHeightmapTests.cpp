#include "SplintHeightmapGenerator.h"

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
constexpr double kPi = 3.14159265358979323846;
constexpr int kTeeth = 12;
constexpr double kArchRx = 24.0;
constexpr double kArchRy = 20.0; // arch apex (anterior) toward +y

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

struct Box
{
    double x0, x1, y0, y1, z0, z1;
};

// Axis-aligned box with outward normals; open faces model intraoral scans
// that end at the gingiva.
vtkSmartPointer<vtkPolyData> boxMesh(const Box& b, bool openTop, bool openBottom)
{
    auto points = vtkSmartPointer<vtkPoints>::New();
    for (int k = 0; k < 8; ++k)
        points->InsertNextPoint((k & 1) ? b.x1 : b.x0, (k & 2) ? b.y1 : b.y0, (k & 4) ? b.z1 : b.z0);
    auto polys = vtkSmartPointer<vtkCellArray>::New();
    auto quad = [&](vtkIdType a, vtkIdType c, vtkIdType d, vtkIdType e) {
        const vtkIdType ids[4] = {a, c, d, e};
        polys->InsertNextCell(4, ids);
    };
    if (!openBottom) quad(0, 2, 3, 1);
    if (!openTop) quad(4, 5, 7, 6);
    quad(0, 1, 5, 4);
    quad(2, 6, 7, 3);
    quad(0, 4, 6, 2);
    quad(1, 3, 7, 5);
    auto mesh = vtkSmartPointer<vtkPolyData>::New();
    mesh->SetPoints(points);
    mesh->SetPolys(polys);
    auto tri = vtkSmartPointer<vtkTriangleFilter>::New();
    tri->SetInputData(mesh);
    tri->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(tri->GetOutput());
    return out;
}

std::array<double, 2> toothCenter(int k)
{
    const double t = kPi * (k + 0.5) / kTeeth;
    return {kArchRx * std::cos(t), kArchRy * std::sin(t)};
}

struct ArchOptions
{
    bool openScan = false;
    bool mushroomUpper = false;
    double toothHalf = 2.5;
};

vtkSmartPointer<vtkPolyData> upperTeeth(const ArchOptions& options)
{
    auto append = vtkSmartPointer<vtkAppendPolyData>::New();
    for (int k = 0; k < kTeeth; ++k) {
        const auto c = toothCenter(k);
        if (options.mushroomUpper) {
            append->AddInputData(boxMesh({c[0] - 3, c[0] + 3, c[1] - 3, c[1] + 3, 1, 3}, false, false));
            append->AddInputData(boxMesh({c[0] - 1, c[0] + 1, c[1] - 1, c[1] + 1, 3, 9}, options.openScan, false));
        } else {
            const double s = options.toothHalf;
            append->AddInputData(boxMesh({c[0] - s, c[0] + s, c[1] - s, c[1] + s, 1, 9}, options.openScan, false));
        }
    }
    append->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(append->GetOutput());
    return out;
}

vtkSmartPointer<vtkPolyData> lowerTeeth(const ArchOptions& options)
{
    auto append = vtkSmartPointer<vtkAppendPolyData>::New();
    for (int k = 0; k < kTeeth; ++k) {
        const auto c = toothCenter(k);
        const double s = options.toothHalf;
        append->AddInputData(boxMesh({c[0] - s, c[0] + s, c[1] - s, c[1] + s, -9, -1}, false, options.openScan));
    }
    append->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(append->GetOutput());
    return out;
}

std::vector<SplintPoint3> guidePoints(double z)
{
    std::vector<SplintPoint3> points;
    for (int k : {0, 5, 11}) {
        const auto c = toothCenter(k);
        const double len = std::hypot(c[0], c[1]);
        points.push_back({c[0] + 2.8 * c[0] / len, c[1] + 2.8 * c[1] / len, z});
    }
    return points;
}

struct Scene
{
    vtkSmartPointer<vtkPolyData> upper;
    vtkSmartPointer<vtkPolyData> lower;
    SplintHeightmapInputs inputs;
};

Scene makeScene(const ArchOptions& options = {}, double upperZ = 5.0, double lowerZ = -5.0)
{
    Scene scene;
    scene.upper = upperTeeth(options);
    scene.lower = lowerTeeth(options);
    scene.inputs.upperTeeth = scene.upper;
    scene.inputs.lowerTeeth = scene.lower;
    scene.inputs.upperPoints = guidePoints(upperZ);
    scene.inputs.lowerPoints = guidePoints(lowerZ);
    scene.inputs.params.gridResolutionMm = 0.4;
    return scene;
}

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
