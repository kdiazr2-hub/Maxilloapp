#include "BoneCavityFill.h"

#include <algorithm>
#include <cmath>
#include <vector>
#include <vtkDataArray.h>
#include <vtkImageConnectivityFilter.h>
#include <vtkImageData.h>
#include <vtkMatrix3x3.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>

BoneCavityFillResult fillEnclosedBoneCavity(
    vtkImageData* labelmap, int boneLabel, const std::array<double, 3>& physicalSeed)
{
    BoneCavityFillResult result;
    auto fail = [&](const char* error) {
        result.error = QString::fromUtf8(error);
        return result;
    };
    if (!labelmap || labelmap->GetNumberOfPoints() == 0 ||
        labelmap->GetNumberOfScalarComponents() != 1 || !labelmap->GetPointData()->GetScalars())
        return fail("No hay una máscara válida.");
    if (boneLabel != 1 && boneLabel != 5 && boneLabel != 6)
        return fail("Seleccione Hueso, Maxilar o Mandíbula.");

    int extent[6];
    labelmap->GetExtent(extent);
    double index[3];
    labelmap->TransformPhysicalPointToContinuousIndex(physicalSeed.data(), index);
    int seed[3];
    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(physicalSeed[axis]) || !std::isfinite(index[axis]) ||
            index[axis] < extent[2 * axis] || index[axis] > extent[2 * axis + 1])
            return fail("El punto está fuera de la máscara.");
        seed[axis] = static_cast<int>(std::round(index[axis]));
    }
    auto writable = [](double value) { return value == 0.0 || value == 2.0; };
    if (!writable(labelmap->GetScalarComponentAsDouble(seed[0], seed[1], seed[2], 0)))
        return fail("El punto debe estar dentro de la cavidad, no sobre el hueso ni otra estructura segmentada.");

    // Crop connectivity analysis to the bone bounds, with an exterior margin.
    int roi[6] = {extent[1], extent[0], extent[3], extent[2], extent[5], extent[4]};
    bool found = false;
    auto* scalars = labelmap->GetPointData()->GetScalars();
    vtkIdType id = 0;
    for (int z = extent[4]; z <= extent[5]; ++z)
        for (int y = extent[2]; y <= extent[3]; ++y)
            for (int x = extent[0]; x <= extent[1]; ++x, ++id) {
                if (scalars->GetTuple1(id) != boneLabel) continue;
                found = true;
                const int ijk[3] = {x, y, z};
                for (int a = 0; a < 3; ++a) {
                    roi[2 * a] = std::min(roi[2 * a], ijk[a]);
                    roi[2 * a + 1] = std::max(roi[2 * a + 1], ijk[a]);
                }
            }
    if (!found) return fail("La máscara seleccionada no contiene hueso.");
    for (int a = 0; a < 3; ++a) {
        roi[2 * a] = std::max(extent[2 * a], roi[2 * a] - 1);
        roi[2 * a + 1] = std::min(extent[2 * a + 1], roi[2 * a + 1] + 1);
        if (seed[a] <= roi[2 * a] || seed[a] >= roi[2 * a + 1])
            return fail("El punto no está en una cavidad cerrada del hueso.");
    }
    // Use index coordinates inside VTK's connectivity filter, independently of
    // the acquisition direction matrix. Preserve original metadata in the result.
    auto openSpace = vtkSmartPointer<vtkImageData>::New();
    openSpace->SetExtent(roi);
    openSpace->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    for (int z = roi[4]; z <= roi[5]; ++z)
        for (int y = roi[2]; y <= roi[3]; ++y)
            for (int x = roi[0]; x <= roi[1]; ++x)
                *static_cast<unsigned char*>(openSpace->GetScalarPointer(x, y, z)) =
                    labelmap->GetScalarComponentAsDouble(x, y, z, 0) != boneLabel;

    auto seedPoints = vtkSmartPointer<vtkPoints>::New();
    seedPoints->InsertNextPoint(seed[0], seed[1], seed[2]);
    auto seeds = vtkSmartPointer<vtkPolyData>::New();
    seeds->SetPoints(seedPoints);
    auto connectivity = vtkSmartPointer<vtkImageConnectivityFilter>::New();
    connectivity->SetInputData(openSpace);
    connectivity->SetSeedData(seeds);
    connectivity->SetScalarRange(1.0, 1.0);
    connectivity->SetExtractionModeToSeededRegions();
    connectivity->SetLabelModeToConstantValue();
    connectivity->SetLabelConstantValue(1);
    connectivity->SetLabelScalarTypeToUnsignedChar();
    connectivity->Update();
    auto* region = connectivity->GetOutput();
    std::vector<vtkIdType> changed;
    for (int z = roi[4]; z <= roi[5]; ++z)
        for (int y = roi[2]; y <= roi[3]; ++y)
            for (int x = roi[0]; x <= roi[1]; ++x) {
                if (region->GetScalarComponentAsDouble(x, y, z, 0) != 1.0) continue;
                if (x == roi[0] || x == roi[1] || y == roi[2] || y == roi[3] || z == roi[4] || z == roi[5])
                    return fail("La cavidad comunica con el exterior o con el borde del TAC. No se aplicó ningún relleno.");
                // VTK uses six neighbours. Reject diagonal leaks as well, rather
                // than treating a corner-connected opening as a sealed cortex.
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx)
                            if (openSpace->GetScalarComponentAsDouble(x + dx, y + dy, z + dz, 0) != 0.0 &&
                                region->GetScalarComponentAsDouble(x + dx, y + dy, z + dz, 0) == 0.0)
                                return fail("El contorno presenta una abertura diagonal. Revise la máscara antes de rellenar.");
                const double old = labelmap->GetScalarComponentAsDouble(x, y, z, 0);
                if (!writable(old)) continue;
                int ijk[3] = {x, y, z};
                changed.push_back(labelmap->ComputePointId(ijk));
                result.replacedSoftTissue |= old == 2.0;
            }
    if (changed.empty()) return fail("No hay una cavidad rellenable en ese punto.");

    result.labelmap = vtkSmartPointer<vtkImageData>::New();
    result.labelmap->DeepCopy(labelmap);
    for (const auto pointId : changed)
        result.labelmap->GetPointData()->GetScalars()->SetTuple1(pointId, boneLabel);
    result.labelmap->Modified();
    result.addedVoxels = static_cast<vtkIdType>(changed.size());
    const double* spacing = labelmap->GetSpacing();
    result.addedVolumeMm3 = result.addedVoxels * std::abs(
        spacing[0] * spacing[1] * spacing[2] * labelmap->GetDirectionMatrix()->Determinant());
    return result;
}
