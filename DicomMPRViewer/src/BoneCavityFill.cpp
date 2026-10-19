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


vtkSmartPointer<vtkImageData> fillBetweenCortices(vtkImageData* labelmap, int boneLabel, double maxGapMm,
                                                  vtkImageData* hounsfield, double minimumHu)
{
    if (!labelmap || labelmap->GetNumberOfPoints() == 0 || !labelmap->GetPointData()->GetScalars() ||
        labelmap->GetNumberOfScalarComponents() != 1 || maxGapMm <= 0.0)
        return nullptr;
    if (boneLabel != 1 && boneLabel != 5 && boneLabel != 6)
        return nullptr;

    int extent[6];
    labelmap->GetExtent(extent);
    auto* scalars = labelmap->GetPointData()->GetScalars();
    int roi[6] = {extent[1], extent[0], extent[3], extent[2], extent[5], extent[4]};
    bool found = false;
    vtkIdType id = 0;
    for (int z = extent[4]; z <= extent[5]; ++z)
        for (int y = extent[2]; y <= extent[3]; ++y)
            for (int x = extent[0]; x <= extent[1]; ++x, ++id)
                if (scalars->GetTuple1(id) == boneLabel) {
                    found = true;
                    roi[0] = std::min(roi[0], x); roi[1] = std::max(roi[1], x);
                    roi[2] = std::min(roi[2], y); roi[3] = std::max(roi[3], y);
                    roi[4] = std::min(roi[4], z); roi[5] = std::max(roi[5], z);
                }
    if (!found)
        return nullptr;

    double spacing[3];
    labelmap->GetSpacing(spacing);
    for (double& s : spacing)
        if (!(s > 1e-6))
            s = 1.0;

    const int nx = roi[1] - roi[0] + 1;
    const int ny = roi[3] - roi[2] + 1;
    const int nz = roi[5] - roi[4] + 1;
    const size_t n = static_cast<size_t>(nx) * ny * nz;
    // 0 = air or soft tissue that may become bone, 1 = this cortex, 2 = another label.
    std::vector<unsigned char> kind(n, 0);
    std::vector<unsigned char> fill(n, 0);
    const auto at = [&](int x, int y, int z) {
        return (static_cast<size_t>(z - roi[4]) * ny + static_cast<size_t>(y - roi[2])) * nx +
               static_cast<size_t>(x - roi[0]);
    };
    const auto inside = [&](int x, int y, int z) {
        return x >= roi[0] && x <= roi[1] && y >= roi[2] && y <= roi[3] && z >= roi[4] && z <= roi[5];
    };
    for (int z = roi[4]; z <= roi[5]; ++z)
        for (int y = roi[2]; y <= roi[3]; ++y)
            for (int x = roi[0]; x <= roi[1]; ++x) {
                const double value = labelmap->GetScalarComponentAsDouble(x, y, z, 0);
                unsigned char k = 0;
                if (value == boneLabel)
                    k = 1;
                else if (value != 0.0 && value != 2.0)
                    k = 2;
                kind[at(x, y, z)] = k;
            }

    const bool haveHu = hounsfield && hounsfield->GetPointData() && hounsfield->GetPointData()->GetScalars();
    bool aligned = false;
    if (haveHu) {
        int maskExtent[6], ctExtent[6];
        labelmap->GetExtent(maskExtent);
        hounsfield->GetExtent(ctExtent);
        aligned = true;
        for (int i = 0; i < 6; ++i)
            aligned = aligned && maskExtent[i] == ctExtent[i];
        for (int i = 0; i < 3 && aligned; ++i)
            aligned = std::abs(labelmap->GetSpacing()[i] - hounsfield->GetSpacing()[i]) <= 1e-4 &&
                      std::abs(labelmap->GetOrigin()[i] - hounsfield->GetOrigin()[i]) <= 1e-3;
        for (int row = 0; row < 3 && aligned; ++row)
            for (int col = 0; col < 3; ++col)
                aligned = aligned && std::abs(labelmap->GetDirectionMatrix()->GetElement(row, col) -
                                              hounsfield->GetDirectionMatrix()->GetElement(row, col)) <= 1e-5;
    }
    const auto marrow = [&](int x, int y, int z) {
        if (!haveHu)
            return true;
        int ix = x, iy = y, iz = z;
        if (!aligned) {
            const int index[3] = {x, y, z};
            double physical[3];
            labelmap->TransformIndexToPhysicalPoint(index, physical);
            double continuous[3];
            hounsfield->TransformPhysicalPointToContinuousIndex(physical, continuous);
            if (!std::isfinite(continuous[0]) || !std::isfinite(continuous[1]) || !std::isfinite(continuous[2]))
                return false;
            ix = static_cast<int>(std::lround(continuous[0]));
            iy = static_cast<int>(std::lround(continuous[1]));
            iz = static_cast<int>(std::lround(continuous[2]));
        }
        int ctExtent[6];
        hounsfield->GetExtent(ctExtent);
        if (ix < ctExtent[0] || ix > ctExtent[1] || iy < ctExtent[2] || iy > ctExtent[3] || iz < ctExtent[4] ||
            iz > ctExtent[5])
            return false;
        const double hu = hounsfield->GetScalarComponentAsDouble(ix, iy, iz, 0);
        return std::isfinite(hu) && hu >= minimumHu;
    };

    // Axis chords only. A diagonal across the nasal wall or the orbital floor is a short
    // gap and would be filled; the marrow of a jaw still meets one of the three axes.
    const int axes[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (const auto& axis : axes) {
        const int dx = axis[0], dy = axis[1], dz = axis[2];
        for (int z = roi[4]; z <= roi[5]; ++z)
            for (int y = roi[2]; y <= roi[3]; ++y)
                for (int x = roi[0]; x <= roi[1]; ++x) {
                    if (inside(x - dx, y - dy, z - dz))
                        continue;
                    bool haveBone = false;
                    int boneX = 0, boneY = 0, boneZ = 0;
                    for (int cx = x, cy = y, cz = z; inside(cx, cy, cz); cx += dx, cy += dy, cz += dz) {
                        const unsigned char k = kind[at(cx, cy, cz)];
                        if (k == 1) {
                            if (haveBone) {
                                const double gap = std::hypot((cx - boneX) * spacing[0], (cy - boneY) * spacing[1],
                                                              (cz - boneZ) * spacing[2]);
                                if (gap <= maxGapMm) {
                                    for (int mx = boneX + dx, my = boneY + dy, mz = boneZ + dz;
                                         mx != cx || my != cy || mz != cz; mx += dx, my += dy, mz += dz)
                                        if (kind[at(mx, my, mz)] == 0 && marrow(mx, my, mz))
                                            fill[at(mx, my, mz)] = 1;
                                }
                            }
                            boneX = cx;
                            boneY = cy;
                            boneZ = cz;
                            haveBone = true;
                        } else if (k == 2) {
                            haveBone = false;
                        }
                    }
                }
    }

    // Dark marrow (ramus, cranial base) is above air and still below dense cancellous bone.
    // An orbit or the nasal airway is that same density and opens anteriorly (LPS -Y); the ramus
    // does not, its cortex is in front of the marrow.
    if (haveHu) {
        const double anterior[3] = {0.0, -1.0, 0.0};
        auto* direction = labelmap->GetDirectionMatrix();
        int axis = 0;
        double dominant = 0.0;
        for (int i = 0; i < 3; ++i) {
            double along = 0.0;
            for (int row = 0; row < 3; ++row)
                along += direction->GetElement(row, i) * anterior[row];
            along /= spacing[i];
            if (std::abs(along) > std::abs(dominant)) {
                dominant = along;
                axis = i;
            }
        }
        const int step = dominant >= 0.0 ? 1 : -1;
        const int reach = std::max(1, static_cast<int>(std::ceil(25.0 / spacing[axis])));
        for (int z = roi[4]; z <= roi[5]; ++z)
            for (int y = roi[2]; y <= roi[3]; ++y)
                for (int x = roi[0]; x <= roi[1]; ++x) {
                    if (!fill[at(x, y, z)] || !marrow(x, y, z))
                        continue;
                    int sample[3] = {x, y, z};
                    if (!aligned) {
                        const int index[3] = {x, y, z};
                        double physical[3], continuous[3];
                        labelmap->TransformIndexToPhysicalPoint(index, physical);
                        hounsfield->TransformPhysicalPointToContinuousIndex(physical, continuous);
                        sample[0] = static_cast<int>(std::lround(continuous[0]));
                        sample[1] = static_cast<int>(std::lround(continuous[1]));
                        sample[2] = static_cast<int>(std::lround(continuous[2]));
                    }
                    int ctExtent[6];
                    hounsfield->GetExtent(ctExtent);
                    if (sample[0] < ctExtent[0] || sample[0] > ctExtent[1] || sample[1] < ctExtent[2] ||
                        sample[1] > ctExtent[3] || sample[2] < ctExtent[4] || sample[2] > ctExtent[5])
                        continue;
                    if (hounsfield->GetScalarComponentAsDouble(sample[0], sample[1], sample[2], 0) >= kDenseMarrowHu)
                        continue;
                    bool cortexInFront = false;
                    int cursor[3] = {x, y, z};
                    for (int s = 0; s < reach; ++s) {
                        cursor[axis] += step;
                        if (!inside(cursor[0], cursor[1], cursor[2]))
                            break;
                        if (kind[at(cursor[0], cursor[1], cursor[2])] != 0) {
                            cortexInFront = true;
                            break;
                        }
                    }
                    if (!cortexInFront)
                        fill[at(x, y, z)] = 0;
                }
    }

    vtkIdType added = 0;
    for (unsigned char voxel : fill)
        if (voxel)
            ++added;
    if (added == 0)
        return nullptr;

    auto out = vtkSmartPointer<vtkImageData>::New();
    out->DeepCopy(labelmap);
    for (int z = roi[4]; z <= roi[5]; ++z)
        for (int y = roi[2]; y <= roi[3]; ++y)
            for (int x = roi[0]; x <= roi[1]; ++x)
                if (fill[at(x, y, z)])
                    out->SetScalarComponentFromDouble(x, y, z, 0, boneLabel);
    out->Modified();
    return out;
}
