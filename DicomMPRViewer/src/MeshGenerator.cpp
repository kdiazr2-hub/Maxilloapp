#include "MeshGenerator.h"
#include "BoneCavityFill.h"

#include <vtkAlgorithmOutput.h>
#include <vtkCleanPolyData.h>
#include <vtkDiscreteMarchingCubes.h>
#include <vtkImageConstantPad.h>
#include <vtkImageGaussianSmooth.h>
#include <vtkImageData.h>
#include <vtkDataArray.h>
#include <vtkPointData.h>
#include <vtkMarchingCubes.h>
#include <vtkPolyData.h>
#include <vtkPolyDataNormals.h>
#include <vtkWindowedSincPolyDataFilter.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

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

// The smoothing of the bone's distance field, in mm. Wider than the old blur of the 0/1 mask: it has to round
// off the 1.5 mm blocks a coarse-grid prediction leaves, and the distance field keeps the volume while it does.
double boneSigmaMmFor(int iterations)
{
    return iterations >= 70 ? 1.0 : iterations >= 40 ? 0.8 : 0.6;
}

}

namespace
{
// Squared Euclidean distance transform along one line (Felzenszwalb & Huttenlocher), `w` = spacing² of the axis.
void edt1d(const float* f, float* d, int n, float w, std::vector<int>& v, std::vector<float>& z)
{
    constexpr float kInf = 1e20f;
    int k = 0;
    v[0] = 0;
    z[0] = -kInf;
    z[1] = kInf;
    for (int q = 1; q < n; ++q) {
        if (f[q] >= kInf)
            continue;
        if (f[v[k]] >= kInf) { // nothing kept yet but a far value: replace it
            v[k] = q;
            z[k] = -kInf;
            z[k + 1] = kInf;
            continue;
        }
        float s = ((f[q] + w * q * q) - (f[v[k]] + w * v[k] * v[k])) / (2.0f * w * (q - v[k]));
        while (k > 0 && s <= z[k]) {
            --k;
            s = ((f[q] + w * q * q) - (f[v[k]] + w * v[k] * v[k])) / (2.0f * w * (q - v[k]));
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = kInf;
    }
    if (f[v[0]] >= kInf) {
        for (int q = 0; q < n; ++q)
            d[q] = kInf;
        return;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < q)
            ++k;
        const float dq = static_cast<float>(q - v[k]);
        d[q] = w * dq * dq + f[v[k]];
    }
}

// Squared distance (mm²) from every voxel to the nearest voxel where `seed` is true, with anisotropic spacing.
std::vector<float> squaredDistanceTo(const std::vector<unsigned char>& seed, bool seedValue, const int dims[3],
                                     const double spacing[3])
{
    const size_t total = static_cast<size_t>(dims[0]) * dims[1] * dims[2];
    std::vector<float> g(total);
    for (size_t i = 0; i < total; ++i)
        g[i] = (seed[i] != 0) == seedValue ? 0.0f : 1e20f;
    const int longest = std::max({dims[0], dims[1], dims[2]});
    std::vector<float> line(longest), out(longest), z(longest + 1);
    std::vector<int> v(longest);
    const size_t stride[3] = {1, static_cast<size_t>(dims[0]), static_cast<size_t>(dims[0]) * dims[1]};
    for (int axis = 0; axis < 3; ++axis) {
        const int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
        const float w = static_cast<float>(spacing[axis] * spacing[axis]);
        for (int j = 0; j < dims[a2]; ++j)
            for (int i = 0; i < dims[a1]; ++i) {
                const size_t base = i * stride[a1] + j * stride[a2];
                for (int q = 0; q < dims[axis]; ++q)
                    line[q] = g[base + q * stride[axis]];
                edt1d(line.data(), out.data(), dims[axis], w, v, z);
                for (int q = 0; q < dims[axis]; ++q)
                    g[base + q * stride[axis]] = out[q];
            }
    }
    return g;
}

// The bone as a smooth field, positive inside, to be contoured at 0 (user's report, 2026-10-19: the surface came
// out in terraces). A label predicted on a coarser grid — TotalSegmentator works at 1.5 mm — and handed back on the
// CT's grid is made of blocks, and so is a label on thick slices. Blurring the 0/1 mask cannot round them off when
// every bone voxel is also forced inside (the old floor kept the corner of every block), so instead:
//  - the signed distance to the label's boundary, in mm, is what gets smoothed (it carries the shape, not the
//    blocks, and is the shape-based interpolation that bridges thick slices);
//  - only the medial voxels of THIN parts (a ridge of the inside distance no deeper than `thinMm`) are kept inside,
//    so a one-voxel wall never vanishes while the corners of thick bone round off;
//  - on anisotropic grids the field is refined to near-isotropic voxels by linear interpolation before contouring.
// The field covers the label's bounding box only (plus the smoothing reach), not the whole scan.
vtkSmartPointer<vtkImageData> smoothBoneField(vtkImageData* labels, int labelValue, double sigmaMm)
{
    int extent[6];
    labels->GetExtent(extent);
    double spacing[3];
    labels->GetSpacing(spacing);
    const int full[3] = {extent[1] - extent[0] + 1, extent[3] - extent[2] + 1, extent[5] - extent[4] + 1};
    vtkDataArray* scalars = labels->GetPointData()->GetScalars();
    int lo[3] = {full[0], full[1], full[2]}, hi[3] = {-1, -1, -1};
    for (int k = 0; k < full[2]; ++k)
        for (int j = 0; j < full[1]; ++j)
            for (int i = 0; i < full[0]; ++i) {
                const vtkIdType id = i + static_cast<vtkIdType>(full[0]) * (j + static_cast<vtkIdType>(full[1]) * k);
                if (static_cast<int>(std::lround(scalars->GetTuple1(id))) != labelValue)
                    continue;
                const int ijk[3] = {i, j, k};
                for (int a = 0; a < 3; ++a) {
                    lo[a] = std::min(lo[a], ijk[a]);
                    hi[a] = std::max(hi[a], ijk[a]);
                }
            }
    if (hi[0] < 0)
        return nullptr;
    // Crop with room for the blur and for the outside distance to settle.
    int cropLo[3], dims[3];
    for (int a = 0; a < 3; ++a) {
        const int margin = static_cast<int>(std::ceil((3.0 * sigmaMm + 2.0 * spacing[a]) / spacing[a])) + 2;
        cropLo[a] = std::max(0, lo[a] - margin);
        const int cropHi = std::min(full[a] - 1, hi[a] + margin);
        dims[a] = cropHi - cropLo[a] + 1;
    }
    const size_t total = static_cast<size_t>(dims[0]) * dims[1] * dims[2];
    std::vector<unsigned char> inside(total, 0);
    for (int k = 0; k < dims[2]; ++k)
        for (int j = 0; j < dims[1]; ++j)
            for (int i = 0; i < dims[0]; ++i) {
                const vtkIdType id = (i + cropLo[0]) +
                                     static_cast<vtkIdType>(full[0]) *
                                         ((j + cropLo[1]) + static_cast<vtkIdType>(full[1]) * (k + cropLo[2]));
                inside[i + static_cast<size_t>(dims[0]) * (j + static_cast<size_t>(dims[1]) * k)] =
                    static_cast<int>(std::lround(scalars->GetTuple1(id))) == labelValue ? 1 : 0;
            }
    const std::vector<float> toOutside = squaredDistanceTo(inside, false, dims, spacing);
    const std::vector<float> toInside = squaredDistanceTo(inside, true, dims, spacing);
    std::vector<float> field(total);
    for (size_t i = 0; i < total; ++i)
        field[i] = inside[i] ? std::sqrt(toOutside[i]) : -std::sqrt(toInside[i]);

    // The medial voxels of thin parts: a local maximum of the inside distance, no deeper than `thinMm`.
    const double minSpacing = std::min({spacing[0], spacing[1], spacing[2]});
    const float thinMm = static_cast<float>(std::max(1.5, 2.0 * minSpacing));
    std::vector<unsigned char> ridge(total, 0);
    const size_t stride[3] = {1, static_cast<size_t>(dims[0]), static_cast<size_t>(dims[0]) * dims[1]};
    for (int k = 0; k < dims[2]; ++k)
        for (int j = 0; j < dims[1]; ++j)
            for (int i = 0; i < dims[0]; ++i) {
                const size_t c = i + stride[1] * j + stride[2] * k;
                if (!inside[c] || field[c] > thinMm)
                    continue;
                const int ijk[3] = {i, j, k};
                bool top = true;
                for (int a = 0; a < 3 && top; ++a) {
                    if (ijk[a] > 0 && field[c - stride[a]] > field[c] + 1e-4f)
                        top = false;
                    if (ijk[a] + 1 < dims[a] && field[c + stride[a]] > field[c] + 1e-4f)
                        top = false;
                }
                ridge[c] = top ? 1 : 0;
            }

    // Blur in millimetres, the same on every axis.
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetExtent(extent[0] + cropLo[0], extent[0] + cropLo[0] + dims[0] - 1, extent[2] + cropLo[1],
                     extent[2] + cropLo[1] + dims[1] - 1, extent[4] + cropLo[2], extent[4] + cropLo[2] + dims[2] - 1);
    image->SetSpacing(spacing);
    image->SetOrigin(labels->GetOrigin());
    image->SetDirectionMatrix(labels->GetDirectionMatrix());
    image->AllocateScalars(VTK_FLOAT, 1);
    std::copy(field.begin(), field.end(), static_cast<float*>(image->GetScalarPointer()));
    auto gaussian = vtkSmartPointer<vtkImageGaussianSmooth>::New();
    gaussian->SetInputData(image);
    gaussian->SetDimensionality(3);
    gaussian->SetStandardDeviations(sigmaMm / spacing[0], sigmaMm / spacing[1], sigmaMm / spacing[2]);
    gaussian->SetRadiusFactors(3.0, 3.0, 3.0);
    gaussian->Update();
    auto smoothed = vtkSmartPointer<vtkImageData>::New();
    smoothed->DeepCopy(gaussian->GetOutput());
    float* values = static_cast<float*>(smoothed->GetScalarPointer());
    const float keep = static_cast<float>(0.5 * minSpacing);
    for (size_t i = 0; i < total; ++i)
        if (ridge[i] && values[i] < keep)
            values[i] = keep;

    // Near-isotropic voxels for contouring: linear interpolation between slices.
    int factor[3];
    size_t refined = 1;
    for (int a = 0; a < 3; ++a) {
        factor[a] = std::clamp(static_cast<int>(std::lround(spacing[a] / minSpacing)), 1, 4);
        refined *= static_cast<size_t>((dims[a] - 1) * factor[a] + 1);
    }
    if ((factor[0] == 1 && factor[1] == 1 && factor[2] == 1) || refined > 200'000'000ull)
        return smoothed;
    const int rdims[3] = {(dims[0] - 1) * factor[0] + 1, (dims[1] - 1) * factor[1] + 1, (dims[2] - 1) * factor[2] + 1};
    auto fine = vtkSmartPointer<vtkImageData>::New();
    int sext[6];
    smoothed->GetExtent(sext);
    fine->SetExtent(sext[0] * factor[0], sext[0] * factor[0] + rdims[0] - 1, sext[2] * factor[1],
                    sext[2] * factor[1] + rdims[1] - 1, sext[4] * factor[2], sext[4] * factor[2] + rdims[2] - 1);
    fine->SetSpacing(spacing[0] / factor[0], spacing[1] / factor[1], spacing[2] / factor[2]);
    fine->SetOrigin(smoothed->GetOrigin());
    fine->SetDirectionMatrix(smoothed->GetDirectionMatrix());
    fine->AllocateScalars(VTK_FLOAT, 1);
    float* out = static_cast<float*>(fine->GetScalarPointer());
    const auto at = [&](int i, int j, int k) { return values[i + stride[1] * j + stride[2] * k]; };
    for (int k = 0; k < rdims[2]; ++k) {
        const int k0 = std::min(k / factor[2], dims[2] - 1), k1 = std::min(k0 + 1, dims[2] - 1);
        const float tk = static_cast<float>(k - k0 * factor[2]) / factor[2];
        for (int j = 0; j < rdims[1]; ++j) {
            const int j0 = std::min(j / factor[1], dims[1] - 1), j1 = std::min(j0 + 1, dims[1] - 1);
            const float tj = static_cast<float>(j - j0 * factor[1]) / factor[1];
            for (int i = 0; i < rdims[0]; ++i) {
                const int i0 = std::min(i / factor[0], dims[0] - 1), i1 = std::min(i0 + 1, dims[0] - 1);
                const float ti = static_cast<float>(i - i0 * factor[0]) / factor[0];
                const float c00 = at(i0, j0, k0) * (1 - ti) + at(i1, j0, k0) * ti;
                const float c10 = at(i0, j1, k0) * (1 - ti) + at(i1, j1, k0) * ti;
                const float c01 = at(i0, j0, k1) * (1 - ti) + at(i1, j0, k1) * ti;
                const float c11 = at(i0, j1, k1) * (1 - ti) + at(i1, j1, k1) * ti;
                out[i + static_cast<size_t>(rdims[0]) * (j + static_cast<size_t>(rdims[1]) * k)] =
                    (c00 * (1 - tj) + c10 * tj) * (1 - tk) + (c01 * (1 - tj) + c11 * tj) * tk;
            }
        }
    }
    return fine;
}
} // namespace

vtkSmartPointer<vtkPolyData> MeshGenerator::generateMesh(vtkImageData* labelmap,
                                                         int labelValue,
                                                         bool smooth,
                                                         int smoothingIterations,
                                                         QString* errorMessage,
                                                         bool closeCortices,
                                                         vtkImageData* hounsfield)
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
    // Add exterior background only. Bone labels first fill marrow whose Hounsfield value
    // is bone; air and the orbits stay hollow.
    const auto cortical = closeCortices ? fillBetweenCortices(labelmap, labelValue, 20.0, hounsfield) : nullptr;
    vtkImageData* contourSource = cortical ? cortical.Get() : labelmap;
    int extent[6];
    contourSource->GetExtent(extent);
    auto padded = vtkSmartPointer<vtkImageConstantPad>::New();
    padded->SetInputData(contourSource);
    padded->SetConstant(0);
    padded->SetOutputWholeExtent(extent[0] - 1, extent[1] + 1,
                                 extent[2] - 1, extent[3] + 1,
                                 extent[4] - 1, extent[5] + 1);

    vtkSmartPointer<vtkAlgorithm> surfaceSource;
    if (smooth && shouldPreSmoothVolume(labelValue, iterations)) {
        padded->Update();
        const auto field = smoothBoneField(padded->GetOutput(), labelValue, boneSigmaMmFor(iterations));
        if (!field) {
            if (errorMessage) *errorMessage = "No se genero geometria para el label seleccionado.";
            return nullptr;
        }
        auto cubes = vtkSmartPointer<vtkMarchingCubes>::New();
        cubes->SetInputData(field);
        cubes->SetValue(0, 0.0);
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
    // Contours already point out of the selected label, including into cavities.
    // Auto-orienting each disconnected shell would turn an inner cavity inside out.
    normals->AutoOrientNormalsOff();
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

vtkSmartPointer<vtkPolyData> MeshGenerator::smoothToothMesh(vtkPolyData* mesh)
{
    if (!mesh || mesh->GetNumberOfPoints() == 0)
        return nullptr;
    auto smoother = vtkSmartPointer<vtkWindowedSincPolyDataFilter>::New();
    smoother->SetInputData(mesh);
    smoother->SetNumberOfIterations(40);
    smoother->SetPassBand(0.06);
    smoother->BoundarySmoothingOn();
    smoother->FeatureEdgeSmoothingOff();
    smoother->NonManifoldSmoothingOn();
    smoother->NormalizeCoordinatesOn();
    smoother->Update();
    auto normals = vtkSmartPointer<vtkPolyDataNormals>::New();
    normals->SetInputConnection(smoother->GetOutputPort());
    normals->ConsistencyOn();
    normals->SplittingOff();
    normals->AutoOrientNormalsOff();
    normals->Update();
    if (!normals->GetOutput() || normals->GetOutput()->GetNumberOfPoints() == 0)
        return nullptr;
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(normals->GetOutput());
    return out;
}
