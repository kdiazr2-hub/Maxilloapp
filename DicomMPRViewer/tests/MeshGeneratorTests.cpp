#include "MeshGenerator.h"

#include <vtkCellArray.h>
#include <vtkPoints.h>
#include <vtkActor.h>
#include <vtkCamera.h>
#include <vtkDataArray.h>
#include <vtkDiscreteMarchingCubes.h>
#include <vtkFeatureEdges.h>
#include <vtkImageData.h>
#include <vtkImplicitPolyDataDistance.h>
#include <vtkMatrix3x3.h>
#include <vtkPointData.h>
#include <vtkPNGWriter.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkSmartPointer.h>
#include <vtkTextActor.h>
#include <vtkTextProperty.h>
#include <vtkWindowToImageFilter.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
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

vtkSmartPointer<vtkImageData> imageWithBone(int label, bool hollow, bool oriented)
{
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetExtent(4, 34, -8, 22, 3, 23);
    image->SetSpacing(0.5, 0.8, 1.2);
    image->SetOrigin(21.0, -30.0, 46.0);
    if (oriented) {
        const double direction[9] = {0, -1, 0, 1, 0, 0, 0, 0, 1};
        image->SetDirectionMatrix(direction);
    }
    image->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    image->GetPointData()->GetScalars()->FillComponent(0, 0);
    for (int z = 3; z <= 23; ++z) {
        for (int y = -4; y <= 18; ++y) {
            for (int x = 8; x <= 30; ++x) {
                const bool lumen = x >= 15 && x <= 23 && y >= 3 && y <= 11;
                if (!hollow || !lumen)
                    image->SetScalarComponentFromDouble(x, y, z, 0, label);
            }
        }
    }
    return image;
}

bool contains(vtkImageData* image, vtkPolyData* mesh, double x, double y, double z)
{
    double index[3] = {x, y, z};
    double point[3];
    image->TransformContinuousIndexToPhysicalPoint(index, point);
    auto distance = vtkSmartPointer<vtkImplicitPolyDataDistance>::New();
    distance->SetInput(mesh);
    return distance->EvaluateFunction(point) < 0;
}

// A wall one voxel thick (the anterior maxilla, a sinus or orbital wall at CT resolution) must stay in the mesh
// at the strongest smoothing. The Gaussian pre-smoothing used to average it below the contour level and the
// bone came out full of holes (user's report, 2026-10-05: "salen más huecos").
void checkThinWallSurvivesSmoothing(int iterations)
{
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetExtent(0, 40, 0, 40, 0, 30);
    image->SetSpacing(0.3, 0.3, 0.3);
    image->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    image->GetPointData()->GetScalars()->FillComponent(0, 0);
    for (int y = 5; y <= 35; ++y)
        for (int x = 5; x <= 35; ++x)
            image->SetScalarComponentFromDouble(x, y, 15, 0, 5); // the wall, one voxel thick at z = 15
    QString error;
    const auto mesh = MeshGenerator::generateMesh(image, 5, true, iterations, &error);
    require(mesh && mesh->GetNumberOfPoints() > 0, "a one-voxel bone wall vanished from the smoothed mesh");
    // No hole: every point of the wall's middle lies between the two faces.
    auto distance = vtkSmartPointer<vtkImplicitPolyDataDistance>::New();
    distance->SetInput(mesh);
    for (int y = 10; y <= 30; y += 2)
        for (int x = 10; x <= 30; x += 2) {
            double p[3] = {0.3 * x, 0.3 * y, 0.3 * 15};
            require(distance->EvaluateFunction(p) < 0, "the smoothed thin wall has a hole");
        }
}

void checkScanBoundary(int label, bool hollow, int iterations, bool oriented)
{
    auto image = imageWithBone(label, hollow, oriented);
    auto before = vtkSmartPointer<vtkImageData>::New();
    before->DeepCopy(image);
    const auto inputTime = image->GetMTime();
    QString error;
    auto mesh = MeshGenerator::generateMesh(image, label, iterations > 0, iterations, &error);
    require(mesh && mesh->GetNumberOfCells() > 0, "Missing generated surface");
    const auto boundaries = openEdges(mesh);
    if (boundaries != 0)
        std::cerr << "label=" << label << " hollow=" << hollow << " smooth=" << iterations
                  << " open/non-manifold edges=" << boundaries << '\n';
    require(boundaries == 0, "Surface is open at the scan boundary");
    require(contains(image, mesh, 11, 7, 13), "Bone wall is not a solid region");
    require(contains(image, mesh, 19, 7, 13), "The space between the cortices stayed hollow");
    require(!contains(image, mesh, 19, 7, 0), "Surface extends beyond scan footprint");
    require(image->GetMTime() == inputTime, "Input labelmap was modified");
    require(image->GetExtent()[0] == 4 && image->GetExtent()[5] == 23, "Input extent changed");
    require(std::memcmp(image->GetScalarPointer(), before->GetScalarPointer(),
                        image->GetNumberOfPoints()) == 0, "Input labels changed");
    if (iterations == 0) {
        double bounds[6];
        mesh->GetBounds(bounds);
        double first[3], last[3];
        const double firstIndex[3] = {7.5, -4.5, 2.5};
        const double lastIndex[3] = {30.5, 18.5, 23.5};
        image->TransformContinuousIndexToPhysicalPoint(firstIndex, first);
        image->TransformContinuousIndexToPhysicalPoint(lastIndex, last);
        for (int axis = 0; axis < 3; ++axis) {
            require(std::abs(bounds[2 * axis] - std::min(first[axis], last[axis])) < 1e-5,
                    "Surface minimum moved in physical coordinates");
            require(std::abs(bounds[2 * axis + 1] - std::max(first[axis], last[axis])) < 1e-5,
                    "Surface maximum moved in physical coordinates");
        }
    }
}

void renderComparison(const std::filesystem::path& path)
{
    auto image = imageWithBone(6, true, false);
    auto original = vtkSmartPointer<vtkDiscreteMarchingCubes>::New();
    original->SetInputData(image);
    original->SetValue(0, 6);
    original->Update();
    auto corrected = MeshGenerator::generateMesh(image, 6, false, 0, nullptr);
    auto window = vtkSmartPointer<vtkRenderWindow>::New();
    window->SetOffScreenRendering(1);
    window->SetSize(1000, 500);
    window->SetMultiSamples(0);
    const std::array<vtkPolyData*, 2> meshes = {original->GetOutput(), corrected};
    for (int side = 0; side < 2; ++side) {
        auto renderer = vtkSmartPointer<vtkRenderer>::New();
        renderer->SetViewport(side * 0.5, 0, (side + 1) * 0.5, 1);
        renderer->SetBackground(0.08, 0.10, 0.11);
        auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
        mapper->SetInputData(meshes[side]);
        mapper->ScalarVisibilityOff();
        auto actor = vtkSmartPointer<vtkActor>::New();
        actor->SetMapper(mapper);
        actor->GetProperty()->SetColor(0.86, 0.83, 0.76);
        renderer->AddActor(actor);
        auto text = vtkSmartPointer<vtkTextActor>::New();
        text->SetInput(side == 0 ? "Antes: borde abierto" : "Despues: pared cerrada");
        text->GetTextProperty()->SetFontSize(22);
        text->GetTextProperty()->SetColor(1, 1, 1);
        text->SetPosition(20, 460);
        renderer->AddActor2D(text);
        const auto center = meshes[side]->GetCenter();
        auto camera = renderer->GetActiveCamera();
        camera->SetFocalPoint(center);
        camera->SetPosition(center[0] + 50, center[1] - 70, center[2] + 100);
        camera->SetViewUp(0, 0, 1);
        camera->ParallelProjectionOn();
        camera->SetParallelScale(25);
        renderer->ResetCameraClippingRange();
        window->AddRenderer(renderer);
    }
    window->Render();
    auto capture = vtkSmartPointer<vtkWindowToImageFilter>::New();
    capture->SetInput(window);
    capture->ReadFrontBufferOff();
    capture->SetInputBufferTypeToRGB();
    capture->Update();
    for (int side = 0; side < 2; ++side) {
        int renderedPixels = 0;
        for (int y = 60; y < 430; ++y) {
            for (int x = side * 500 + 40; x < side * 500 + 460; ++x) {
                const auto* rgb = static_cast<unsigned char*>(capture->GetOutput()->GetScalarPointer(x, y, 0));
                if (rgb[0] > 75 && rgb[1] > 75 && rgb[2] > 60) ++renderedPixels;
            }
        }
        require(renderedPixels > 1000, "Empty comparison render");
    }
    std::filesystem::create_directories(path.parent_path());
    auto writer = vtkSmartPointer<vtkPNGWriter>::New();
    writer->SetFileName(path.string().c_str());
    writer->SetInputConnection(capture->GetOutputPort());
    writer->Write();
    require(std::filesystem::exists(path), "Comparison image was not written");
    window->Finalize();
}

void checkToothSmoothingFlattensASpike()
{
    constexpr int n = 11;
    auto points = vtkSmartPointer<vtkPoints>::New();
    auto polys = vtkSmartPointer<vtkCellArray>::New();
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x)
            points->InsertNextPoint(x, y, (x == 5 && y == 5) ? 3.0 : 0.0);
    }
    auto id = [](int x, int y) { return y * n + x; };
    for (int y = 0; y < n - 1; ++y) {
        for (int x = 0; x < n - 1; ++x) {
            vtkIdType a[3] = {id(x, y), id(x + 1, y), id(x + 1, y + 1)};
            vtkIdType b[3] = {id(x, y), id(x + 1, y + 1), id(x, y + 1)};
            polys->InsertNextCell(3, a);
            polys->InsertNextCell(3, b);
        }
    }
    auto mesh = vtkSmartPointer<vtkPolyData>::New();
    mesh->SetPoints(points);
    mesh->SetPolys(polys);
    const auto smooth = MeshGenerator::smoothToothMesh(mesh);
    require(smooth && smooth->GetNumberOfPoints() == points->GetNumberOfPoints(),
            "tooth smoothing dropped the surface");
    double spike[3] = {};
    mesh->GetPoint(id(5, 5), spike);
    require(spike[2] > 2.5, "smoothing changed the input mesh");
    smooth->GetPoint(id(5, 5), spike);
    require(spike[2] < 1.5, "the marching-cubes spike was left rough");
    double corner[3] = {};
    smooth->GetPoint(id(0, 0), corner);
    require(std::abs(corner[2]) < 0.8, "tooth smoothing pulled the border off the surface");
}
} // namespace

// Thick CT slices left the bone in terraces, one step per slice (user's report, 2026-10-19: "sale muy rugosa,
// no sale lisa"). A sphere seen with 0.4 x 0.4 x 1.25 mm voxels must come out round: its radius may not wobble
// with the slices, and its volume must stay the sphere's.
void checkThickSlicesDoNotTerrace(double sz, double block)
{
    const double sx = 0.4, sy = 0.4, radius = 8.0;
    auto image = vtkSmartPointer<vtkImageData>::New();
    const int nz = static_cast<int>(std::ceil(20.0 / sz));
    image->SetExtent(0, 59, 0, 59, 0, nz - 1);
    image->SetSpacing(sx, sy, sz);
    image->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    image->GetPointData()->GetScalars()->FillComponent(0, 0);
    const double c[3] = {29.5 * sx, 29.5 * sy, 0.5 * (nz - 1) * sz};
    // `block` > 0: the mask was predicted on a coarser grid and brought back by nearest neighbour, as a network
    // working at 1.5 mm hands its mask back on the CT's own grid.
    const auto snap = [block](double v) { return block > 0.0 ? (std::floor(v / block) + 0.5) * block : v; };
    for (int z = 0; z < nz; ++z)
        for (int y = 0; y <= 59; ++y)
            for (int x = 0; x <= 59; ++x) {
                const double d = std::sqrt(std::pow(snap(x * sx) - c[0], 2) + std::pow(snap(y * sy) - c[1], 2) +
                                           std::pow(snap(z * sz) - c[2], 2));
                if (d <= radius)
                    image->SetScalarComponentFromDouble(x, y, z, 0, 6);
            }
    QString error;
    const auto mesh = MeshGenerator::generateMesh(image, 6, true, 40, &error);
    require(mesh && mesh->GetNumberOfPoints() > 0, "the sphere gave no mesh");
    require(openEdges(mesh) == 0, "the smoothed sphere is not closed");
    // How far the surface wanders off the sphere, away from its poles (where a slice cuts it almost flat).
    double sum = 0.0, sumSq = 0.0;
    int n = 0;
    for (vtkIdType i = 0; i < mesh->GetNumberOfPoints(); ++i) {
        double p[3];
        mesh->GetPoint(i, p);
        const double dz = p[2] - c[2];
        if (std::abs(dz) > 0.7 * radius)
            continue;
        const double r = std::sqrt(std::pow(p[0] - c[0], 2) + std::pow(p[1] - c[1], 2) + dz * dz);
        sum += r;
        sumSq += r * r;
        ++n;
    }
    const double mean = sum / n;
    const double wobble = std::sqrt(std::max(0.0, sumSq / n - mean * mean));
    std::cout << "slices " << sz << " mm, block " << block << " mm: mean radius " << mean << " mm, wobble " << wobble << " mm\n";
    require(wobble < 0.07, "the surface still shows the slices as terraces");
    require(std::abs(mean - radius) < 0.4, "the sphere shrank or grew");
}

int main(int argc, char** argv)
{
    try {
        checkToothSmoothingFlattensASpike();
        for (int label : {1, 5, 6}) {
            for (bool hollow : {false, true}) {
                for (int iterations : {0, 15, 40}) {
                    for (bool oriented : {false, true})
                        checkScanBoundary(label, hollow, iterations, oriented);
                }
            }
        }
        for (int iterations : {25, 40, 70})
            checkThinWallSurvivesSmoothing(iterations);
        checkThickSlicesDoNotTerrace(1.25, 0.0);
        checkThickSlicesDoNotTerrace(2.5, 0.0);
        // A mask handed back in 1.5 mm blocks is refined against the CT in the segmentation script
        // (refine_coarse_bone_mask), not here: no smoothing rounds those blocks off without the anatomy.
        if (argc > 1) renderComparison(argv[1]);
        std::cout << "Mesh scan-boundary tests passed (36 cases).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
