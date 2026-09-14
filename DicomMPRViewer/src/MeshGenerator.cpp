#include "MeshGenerator.h"

#include <vtkAlgorithmOutput.h>
#include <vtkCleanPolyData.h>
#include <vtkDiscreteMarchingCubes.h>
#include <vtkImageConstantPad.h>
#include <vtkImageGaussianSmooth.h>
#include <vtkImageData.h>
#include <vtkImageThreshold.h>
#include <vtkMarchingCubes.h>
#include <vtkPolyData.h>
#include <vtkPolyDataNormals.h>
#include <vtkWindowedSincPolyDataFilter.h>

#include <algorithm>
#include <array>

namespace
{
bool isFineDentalOrNeuralLabel(int label)
{
    return label == 3 || label == 7 || label == 8;
}

bool shouldPreSmoothVolume(int label, int smoothingIterations)
{
    return !isFineDentalOrNeuralLabel(label) && smoothingIterations >= 25;
}

int effectiveSmoothingIterations(int label, int requested)
{
    if (isFineDentalOrNeuralLabel(label)) {
        // Keep tooth cusps, brackets and mandibular canal from being rounded away.
        return std::min(requested, label == 7 ? 6 : 14);
    }
    return requested;
}

double meshPassBandFor(int label, int iterations)
{
    if (isFineDentalOrNeuralLabel(label)) return 0.140;
    return iterations >= 70 ? 0.020
         : iterations >= 40 ? 0.035
         : iterations >= 25 ? 0.060
                            : 0.100;
}

std::array<double, 3> volumeSigmaFor(vtkImageData* labelmap, int iterations)
{
    double spacing[3] = {1.0, 1.0, 1.0};
    labelmap->GetSpacing(spacing);

    const double sigmaMm = iterations >= 70 ? 0.70
                         : iterations >= 40 ? 0.55
                                            : 0.40;
    const double sigmaZMm = iterations >= 70 ? 0.90
                          : iterations >= 40 ? 0.70
                                             : 0.50;

    auto toVoxels = [](double mm, double sp, double lo, double hi) {
        if (sp <= 1e-6) sp = 1.0;
        return std::clamp(mm / sp, lo, hi);
    };

    return {
        toVoxels(sigmaMm,  spacing[0], 0.25, 1.45),
        toVoxels(sigmaMm,  spacing[1], 0.25, 1.45),
        toVoxels(sigmaZMm, spacing[2], 0.35, 1.70)
    };
}
}

vtkSmartPointer<vtkPolyData> MeshGenerator::generateMesh(vtkImageData* labelmap,
                                                         int labelValue,
                                                         bool smooth,
                                                         int smoothingIterations,
                                                         QString* errorMessage)
{
    if (!labelmap || labelmap->GetNumberOfPoints() == 0) {
        if (errorMessage) *errorMessage = "No hay labelmap cargado.";
        return nullptr;
    }
    if (labelValue <= 0) {
        if (errorMessage) *errorMessage = "Label invalido.";
        return nullptr;
    }

    const int iterations = smooth
        ? std::max(0, effectiveSmoothingIterations(labelValue, smoothingIterations))
        : 0;

    // Contouring cannot close a foreground region that ends at the image edge.
    // Add exterior background only, leaving the labelmap and anatomical voids intact.
    int extent[6];
    labelmap->GetExtent(extent);
    auto padded = vtkSmartPointer<vtkImageConstantPad>::New();
    padded->SetInputData(labelmap);
    padded->SetConstant(0);
    padded->SetOutputWholeExtent(extent[0] - 1, extent[1] + 1,
                                 extent[2] - 1, extent[3] + 1,
                                 extent[4] - 1, extent[5] + 1);

    vtkSmartPointer<vtkAlgorithm> surfaceSource;
    if (smooth && shouldPreSmoothVolume(labelValue, iterations)) {
        auto threshold = vtkSmartPointer<vtkImageThreshold>::New();
        threshold->SetInputConnection(padded->GetOutputPort());
        threshold->ThresholdBetween(labelValue, labelValue);
        threshold->SetInValue(1.0);
        threshold->SetOutValue(0.0);
        threshold->SetOutputScalarTypeToFloat();

        const auto sigma = volumeSigmaFor(labelmap, iterations);
        auto gaussian = vtkSmartPointer<vtkImageGaussianSmooth>::New();
        gaussian->SetInputConnection(threshold->GetOutputPort());
        gaussian->SetDimensionality(3);
        gaussian->SetStandardDeviations(sigma[0], sigma[1], sigma[2]);
        gaussian->SetRadiusFactors(2.0, 2.0, 2.0);

        auto cubes = vtkSmartPointer<vtkMarchingCubes>::New();
        cubes->SetInputConnection(gaussian->GetOutputPort());
        cubes->SetValue(0, 0.50);
        cubes->ComputeNormalsOff();
        cubes->ComputeGradientsOff();
        cubes->ComputeScalarsOff();
        cubes->Update();
        surfaceSource = cubes;
    } else {
        auto cubes = vtkSmartPointer<vtkDiscreteMarchingCubes>::New();
        cubes->SetInputConnection(padded->GetOutputPort());
        cubes->SetValue(0, labelValue);
        cubes->Update();
        surfaceSource = cubes;
    }

    auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
    clean->SetInputConnection(surfaceSource->GetOutputPort());
    clean->Update();

    vtkAlgorithmOutput* meshPort = clean->GetOutputPort();
    vtkSmartPointer<vtkWindowedSincPolyDataFilter> smoother;
    if (smooth && iterations > 0) {
        const double passBand = meshPassBandFor(labelValue, iterations);
        smoother = vtkSmartPointer<vtkWindowedSincPolyDataFilter>::New();
        smoother->SetInputConnection(meshPort);
        smoother->SetNumberOfIterations(iterations);
        smoother->BoundarySmoothingOn();
        if (!isFineDentalOrNeuralLabel(labelValue) && iterations >= 25) {
            smoother->FeatureEdgeSmoothingOn();
        } else {
            smoother->FeatureEdgeSmoothingOff();
        }
        smoother->SetFeatureAngle(isFineDentalOrNeuralLabel(labelValue) ? 80.0 : 120.0);
        smoother->SetPassBand(passBand);
        smoother->NonManifoldSmoothingOn();
        smoother->NormalizeCoordinatesOn();
        smoother->Update();
        meshPort = smoother->GetOutputPort();
    }

    auto normals = vtkSmartPointer<vtkPolyDataNormals>::New();
    normals->SetInputConnection(meshPort);
    normals->ConsistencyOn();
    normals->SplittingOff();
    normals->AutoOrientNormalsOn();
    normals->Update();

    vtkPolyData* output = normals->GetOutput();
    if (!output || output->GetNumberOfPoints() == 0) {
        if (errorMessage) *errorMessage = "No se genero geometria para el label seleccionado.";
        return nullptr;
    }

    auto mesh = vtkSmartPointer<vtkPolyData>::New();
    mesh->DeepCopy(output);
    return mesh;
}
