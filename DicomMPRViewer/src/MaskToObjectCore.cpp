#include "MaskToObjectCore.h"
#include "MeshGenerator.h"

#include <vtkDataArray.h>
#include <vtkExtractVOI.h>
#include <vtkFlyingEdges3D.h>
#include <vtkImageConstantPad.h>
#include <vtkImageData.h>
#include <vtkImageGaussianSmooth.h>
#include <vtkImageThreshold.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>
#include <vtkPolyDataNormals.h>
#include <vtkWindowedSincPolyDataFilter.h>

#include <algorithm>
#include <climits>

namespace
{
constexpr int kMarginVoxels = 3; // blur support (3 sigma) plus background to close the surface

// Index extent of the voxels carrying the label; false when the label is absent.
bool labelExtent(vtkImageData* mask, int label, int result[6])
{
    int extent[6];
    mask->GetExtent(extent);
    vtkDataArray* scalars = mask->GetPointData()->GetScalars();
    const unsigned char* bytes = scalars->GetDataType() == VTK_UNSIGNED_CHAR
        ? static_cast<const unsigned char*>(scalars->GetVoidPointer(0)) : nullptr;
    result[0] = result[2] = result[4] = INT_MAX;
    result[1] = result[3] = result[5] = INT_MIN;
    vtkIdType id = 0;
    for (int z = extent[4]; z <= extent[5]; ++z)
        for (int y = extent[2]; y <= extent[3]; ++y)
            for (int x = extent[0]; x <= extent[1]; ++x, ++id) {
                const bool hit = bytes ? bytes[id] == label : scalars->GetTuple1(id) == label;
                if (!hit) continue;
                result[0] = std::min(result[0], x); result[1] = std::max(result[1], x);
                result[2] = std::min(result[2], y); result[3] = std::max(result[3], y);
                result[4] = std::min(result[4], z); result[5] = std::max(result[5], z);
            }
    return result[0] <= result[1];
}

vtkSmartPointer<vtkPolyData> smoothSurface(vtkImageData* mask, int label, QString* error)
{
    // Anti-aliased surface: blur the binary label slightly and contour it halfway, so the surface
    // interpolates between voxels instead of following the CT slice staircase. Solid filling stays
    // solid and a one-voxel bone plate survives (its blurred peak stays above 0.5); the mask is read only.
    int bounds[6];
    if (!labelExtent(mask, label, bounds)) {
        if (error) *error = QStringLiteral("La máscara seleccionada está vacía.");
        return nullptr;
    }
    int extent[6];
    mask->GetExtent(extent);
    auto region = vtkSmartPointer<vtkExtractVOI>::New();
    region->SetInputData(mask);
    region->SetVOI(std::max(extent[0], bounds[0] - kMarginVoxels), std::min(extent[1], bounds[1] + kMarginVoxels),
                   std::max(extent[2], bounds[2] - kMarginVoxels), std::min(extent[3], bounds[3] + kMarginVoxels),
                   std::max(extent[4], bounds[4] - kMarginVoxels), std::min(extent[5], bounds[5] + kMarginVoxels));

    auto binary = vtkSmartPointer<vtkImageThreshold>::New();
    binary->SetInputConnection(region->GetOutputPort());
    binary->ThresholdBetween(label, label);
    binary->SetInValue(1.0);
    binary->SetOutValue(0.0);
    binary->SetOutputScalarTypeToFloat();

    // Background around the label, also beyond the image border, so the surface closes.
    auto padded = vtkSmartPointer<vtkImageConstantPad>::New();
    padded->SetInputConnection(binary->GetOutputPort());
    padded->SetConstant(0.0);
    padded->SetOutputWholeExtent(bounds[0] - kMarginVoxels, bounds[1] + kMarginVoxels, bounds[2] - kMarginVoxels,
                                 bounds[3] + kMarginVoxels, bounds[4] - kMarginVoxels, bounds[5] + kMarginVoxels);

    const double sigma = MaskToObjectCore::SmoothSigmaVoxels;
    auto blur = vtkSmartPointer<vtkImageGaussianSmooth>::New();
    blur->SetInputConnection(padded->GetOutputPort());
    blur->SetDimensionality(3);
    blur->SetStandardDeviations(sigma, sigma, sigma);
    blur->SetRadiusFactors(3.0, 3.0, 3.0);

    auto contour = vtkSmartPointer<vtkFlyingEdges3D>::New(); // applies the direction matrix
    contour->SetInputConnection(blur->GetOutputPort());
    contour->SetValue(0, 0.5);
    contour->ComputeNormalsOff();
    contour->ComputeGradientsOff();
    contour->ComputeScalarsOff();

    // Light non-shrinking pass for the remaining voxel-scale ripple.
    auto sinc = vtkSmartPointer<vtkWindowedSincPolyDataFilter>::New();
    sinc->SetInputConnection(contour->GetOutputPort());
    sinc->SetNumberOfIterations(20);
    sinc->SetPassBand(0.01);
    sinc->NormalizeCoordinatesOn();
    sinc->BoundarySmoothingOn();
    sinc->FeatureEdgeSmoothingOff();
    sinc->NonManifoldSmoothingOn();

    auto normals = vtkSmartPointer<vtkPolyDataNormals>::New();
    normals->SetInputConnection(sinc->GetOutputPort());
    normals->ConsistencyOn();
    normals->SplittingOff();
    normals->AutoOrientNormalsOff(); // inner cavity shells must keep pointing into the cavity
    normals->Update();

    vtkPolyData* output = normals->GetOutput();
    if (!output || output->GetNumberOfPolys() == 0) {
        if (error) *error = QStringLiteral("No se generó geometría para la máscara seleccionada.");
        return nullptr;
    }
    auto result = vtkSmartPointer<vtkPolyData>::New();
    result->DeepCopy(output);
    return result;
}
} // namespace

vtkSmartPointer<vtkPolyData> MaskToObjectCore::Convert(vtkImageData* mask, int label, QString* error, Surface surface)
{
    if (error) error->clear();
    if (!mask || !mask->GetPointData()->GetScalars() || mask->GetNumberOfScalarComponents() != 1) {
        if (error) *error = QStringLiteral("Se necesita una máscara de etiquetas válida.");
        return nullptr;
    }
    if (label <= 0) {
        if (error) *error = QStringLiteral("Etiqueta inválida.");
        return nullptr;
    }
    if (surface == Surface::Exact)
        return MeshGenerator::generateMesh(mask, label, false, 0, error);
    return smoothSurface(mask, label, error);
}
