#include "MaskToObjectCore.h"
#include "BoneCavityFill.h"
#include <QString>
#include <vtkActor.h>
#include <vtkCamera.h>
#include <vtkFeatureEdges.h>
#include <vtkImageData.h>
#include <vtkImplicitPolyDataDistance.h>
#include <vtkMath.h>
#include <vtkNrrdReader.h>
#include <vtkPNGWriter.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkWindowToImageFilter.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

using Surface = MaskToObjectCore::Surface;

static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

// Edges folding more than 30°: CT slice terraces produce many of them.
static vtkIdType sharpEdges(vtkPolyData* mesh)
{
    auto edges = vtkSmartPointer<vtkFeatureEdges>::New();
    edges->SetInputData(mesh);
    edges->BoundaryEdgesOff();
    edges->NonManifoldEdgesOff();
    edges->ManifoldEdgesOff();
    edges->FeatureEdgesOn();
    edges->SetFeatureAngle(30.0);
    edges->Update();
    return edges->GetOutput()->GetNumberOfLines();
}

// RMS vertical distance of the top surface to the plane z = 0.3 x + 3 (mm).
static double slopeResidual(vtkPolyData* mesh)
{
    double sum = 0.0;
    int count = 0;
    for (vtkIdType i = 0; i < mesh->GetNumberOfPoints(); ++i) {
        double p[3];
        mesh->GetPoint(i, p);
        const double residual = p[2] - (0.3 * p[0] + 3.0);
        if (p[0] < 8.0 || p[0] > 22.0 || p[1] < 8.0 || p[1] > 22.0 || std::abs(residual) > 1.0) continue;
        sum += residual * residual;
        ++count;
    }
    require(count > 50, "Slope fixture has no top surface");
    return std::sqrt(sum / count);
}

// A plane sampled on 0.6 mm slices: the exact surface is a staircase, the smooth one is not.
static void testSmoothRemovesSliceTerraces()
{
    auto mask = vtkSmartPointer<vtkImageData>::New();
    mask->SetDimensions(60, 60, 40);
    mask->SetSpacing(0.5, 0.5, 0.6);
    mask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    for (int z = 0; z < 40; ++z)
        for (int y = 0; y < 60; ++y)
            for (int x = 0; x < 60; ++x) {
                const bool inside = x >= 4 && x <= 55 && y >= 4 && y <= 55 && z >= 2 && z * 0.6 < 0.3 * x * 0.5 + 3.0;
                mask->SetScalarComponentFromDouble(x, y, z, 0, inside ? 5 : 0);
            }
    const auto exact = MaskToObjectCore::Convert(mask, 5, nullptr, Surface::Exact);
    const auto smooth = MaskToObjectCore::Convert(mask, 5, nullptr, Surface::Smooth);
    require(exact && smooth, "Slope conversion failed");
    const double exactResidual = slopeResidual(exact);
    const double smoothResidual = slopeResidual(smooth);
    // Only the sloped top counts: the block's real side and bottom edges must stay sharp.
    const auto topSharpEdges = [](vtkPolyData* mesh) {
        auto edges = vtkSmartPointer<vtkFeatureEdges>::New();
        edges->SetInputData(mesh);
        edges->BoundaryEdgesOff();
        edges->NonManifoldEdgesOff();
        edges->ManifoldEdgesOff();
        edges->FeatureEdgesOn();
        edges->SetFeatureAngle(30.0);
        edges->Update();
        vtkIdType count = 0;
        vtkPolyData* lines = edges->GetOutput();
        for (vtkIdType i = 0; i < lines->GetNumberOfPoints(); ++i) {
            double p[3];
            lines->GetPoint(i, p);
            count += p[0] >= 8.0 && p[0] <= 22.0 && p[1] >= 8.0 && p[1] <= 22.0 &&
                     std::abs(p[2] - (0.3 * p[0] + 3.0)) <= 1.0;
        }
        return count;
    };
    const vtkIdType exactSharp = topSharpEdges(exact);
    const vtkIdType smoothSharp = topSharpEdges(smooth);
    std::cout << "slope RMS exact " << exactResidual << " mm, smooth " << smoothResidual << " mm; sharp edges "
              << exactSharp << " vs " << smoothSharp << '\n';
    require(smoothResidual < 0.6 * exactResidual, "Smooth surface kept the slice terraces");
    require(smoothSharp * 10 < exactSharp, "Smooth surface still folds at the slice terraces");
}

static void renderPng(vtkPolyData* mesh, const std::string& path, double zoom, double focusHeight)
{
    auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    mapper->SetInputData(mesh);
    mapper->ScalarVisibilityOff();
    auto actor = vtkSmartPointer<vtkActor>::New();
    actor->SetMapper(mapper);
    actor->GetProperty()->SetColor(0.89, 0.85, 0.78);
    auto renderer = vtkSmartPointer<vtkRenderer>::New();
    renderer->AddActor(actor);
    renderer->SetBackground(0.12, 0.12, 0.15);
    auto window = vtkSmartPointer<vtkRenderWindow>::New();
    window->SetOffScreenRendering(1);
    window->SetSize(800, 900);
    window->AddRenderer(renderer);
    double b[6];
    mesh->GetBounds(b);
    const double center[3] = {(b[0] + b[1]) / 2, (b[2] + b[3]) / 2, b[4] + (b[5] - b[4]) * focusHeight};
    auto* camera = renderer->GetActiveCamera();
    camera->SetPosition(0.0, -1.0, 0.0); // LPS: the face looks to -Y
    camera->SetFocalPoint(0.0, 0.0, 0.0);
    camera->SetViewUp(0.0, 0.0, 1.0);
    renderer->ResetCamera();
    const double distance = camera->GetDistance();
    camera->SetFocalPoint(center);
    camera->SetPosition(center[0], center[1] - distance, center[2]);
    camera->Zoom(zoom);
    renderer->ResetCameraClippingRange();
    window->Render();
    auto capture = vtkSmartPointer<vtkWindowToImageFilter>::New();
    capture->SetInput(window);
    capture->Update();
    auto png = vtkSmartPointer<vtkPNGWriter>::New();
    png->SetFileName(path.c_str());
    png->SetInputConnection(capture->GetOutputPort());
    png->Write();
}

// Optional manual check on a real segmentation: MASK_TO_OBJECT_LABELMAP (NRRD) and
// MASK_TO_OBJECT_OUT (folder for frontal and close-up renders of both surfaces).
static void realDataReport()
{
    const char* path = std::getenv("MASK_TO_OBJECT_LABELMAP");
    const char* out = std::getenv("MASK_TO_OBJECT_OUT");
    if (!path || !out) return;
    auto reader = vtkSmartPointer<vtkNrrdReader>::New();
    reader->SetFileName(path);
    reader->Update();
    vtkImageData* mask = reader->GetOutput();
    double spacing[3];
    mask->GetSpacing(spacing);
    for (int label : {5, 6}) {
        std::array<vtkSmartPointer<vtkPolyData>, 2> meshes;
        for (Surface surface : {Surface::Exact, Surface::Smooth}) {
            const bool smooth = surface == Surface::Smooth;
            const auto start = std::chrono::steady_clock::now();
            QString error;
            const auto mesh = MaskToObjectCore::Convert(mask, label, &error, surface);
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            require(mesh != nullptr, error.toStdString().c_str());
            std::cout << "real label " << label << (smooth ? " smooth: " : " exact: ") << mesh->GetNumberOfPolys()
                      << " triangles, " << sharpEdges(mesh) << " sharp edges, " << seconds << " s\n";
            const std::string base = std::string(out) + "/label" + std::to_string(label) + (smooth ? "_smooth" : "_exact");
            renderPng(mesh, base + ".png", 1.0, 0.5);
            renderPng(mesh, base + "_closeup.png", 2.4, 0.3);
            meshes[smooth ? 1 : 0] = mesh;
        }
        auto distance = vtkSmartPointer<vtkImplicitPolyDataDistance>::New();
        distance->SetInput(meshes[0]);
        double worst = 0.0, sum = 0.0;
        int count = 0;
        for (vtkIdType i = 0; i < meshes[1]->GetNumberOfPoints(); i += 23) {
            double p[3];
            meshes[1]->GetPoint(i, p);
            const double d = std::abs(distance->EvaluateFunction(p));
            worst = std::max(worst, d);
            sum += d;
            ++count;
        }
        std::cout << "real label " << label << " smooth-to-exact distance mean " << sum / std::max(count, 1) << " max "
                  << worst << " mm (sampled), voxel diagonal " << vtkMath::Norm(spacing) << " mm\n";
    }
}

int main()
{
    try {
        for (int label : {1, 5, 6}) {
            auto mask = vtkSmartPointer<vtkImageData>::New();
            mask->SetDimensions(21, 21, 21);
            mask->SetSpacing(0.4, 0.7, 1.2);
            mask->SetOrigin(12, -30, 40);
            const double direction[9] = {0, -1, 0, 1, 0, 0, 0, 0, 1};
            mask->SetDirectionMatrix(direction);
            mask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
            for (int z = 0; z < 21; ++z)
                for (int y = 0; y < 21; ++y)
                    for (int x = 0; x < 21; ++x) {
                        const bool outer = x >= 2 && x <= 18 && y >= 2 && y <= 18 && z >= 2 && z <= 18;
                        const bool inner = x >= 4 && x <= 16 && y >= 4 && y <= 16 && z >= 4 && z <= 16;
                        mask->SetScalarComponentFromDouble(x, y, z, 0, outer ? inner ? 2 : label : 0);
                    }
            std::array<double, 3> seed;
            const double index[3] = {10, 10, 10};
            mask->TransformContinuousIndexToPhysicalPoint(index, seed.data());
            for (Surface surface : {Surface::Exact, Surface::Smooth}) {
                const auto unfilled = MaskToObjectCore::Convert(mask, label, nullptr, surface);
                require(unfilled != nullptr, "Unfilled conversion failed");
                auto distance = vtkSmartPointer<vtkImplicitPolyDataDistance>::New();
                distance->SetInput(unfilled);
                require(distance->EvaluateFunction(seed.data()) > 0, "Conversion invented new bone filling");
            }
            auto filled = fillEnclosedBoneCavity(mask, label, seed);
            require(filled.labelmap && filled.addedVoxels > 0, "Cavity fixture was not filled");
            auto snapshot = vtkSmartPointer<vtkImageData>::New();
            snapshot->DeepCopy(filled.labelmap);
            const auto stamp = filled.labelmap->GetMTime();
            QString error;
            const auto mesh = MaskToObjectCore::Convert(filled.labelmap, label, &error);
            require(mesh && error.isEmpty(), "Filled mask conversion failed");
            auto distance = vtkSmartPointer<vtkImplicitPolyDataDistance>::New();
            distance->SetInput(mesh);
            require(distance->EvaluateFunction(seed.data()) < 0, "The object lost the committed filling");
            const auto smooth = MaskToObjectCore::Convert(filled.labelmap, label, &error, Surface::Smooth);
            require(smooth && error.isEmpty(), "Smooth conversion failed");
            require(filled.labelmap->GetMTime() == stamp, "Conversion modified the source mask");
            require(std::memcmp(snapshot->GetScalarPointer(), filled.labelmap->GetScalarPointer(),
                                snapshot->GetNumberOfPoints()) == 0, "Conversion changed label values");
            // The smooth object keeps the filling and stays near the voxel boundary (direction matrix applied).
            auto smoothDistance = vtkSmartPointer<vtkImplicitPolyDataDistance>::New();
            smoothDistance->SetInput(smooth);
            require(smoothDistance->EvaluateFunction(seed.data()) < 0, "The smooth object lost the committed filling");
            double spacing[3];
            filled.labelmap->GetSpacing(spacing);
            double worst = 0.0;
            for (vtkIdType i = 0; i < smooth->GetNumberOfPoints(); ++i) {
                double p[3];
                smooth->GetPoint(i, p);
                worst = std::max(worst, std::abs(distance->EvaluateFunction(p)));
            }
            // Sharp box corners round by up to about one voxel diagonal; smooth anatomy moves far less.
            require(worst <= vtkMath::Norm(spacing), "The smooth object moved away from the mask boundary");
            // A one-voxel feature must survive exact conversion, independent of display smoothing.
            filled.labelmap->SetScalarComponentFromDouble(19, 10, 10, 0, label);
            const auto detailed = MaskToObjectCore::Convert(filled.labelmap, label);
            const double tipIndex[3] = {19, 10, 10};
            double tip[3];
            filled.labelmap->TransformContinuousIndexToPhysicalPoint(tipIndex, tip);
            distance->SetInput(detailed);
            require(distance->EvaluateFunction(tip) < 0, "Conversion smoothed away a voxel feature");
        }
        testSmoothRemovesSliceTerraces();
        require(!MaskToObjectCore::Convert(nullptr, 5), "Invalid mask accepted");
        require(!MaskToObjectCore::Convert(nullptr, 5, nullptr, Surface::Smooth), "Invalid mask accepted (smooth)");
        realDataReport();
        std::cout << "MaskToObjectTests OK\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
