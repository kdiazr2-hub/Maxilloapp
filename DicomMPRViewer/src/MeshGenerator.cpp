#include "MeshGenerator.h"
#include "BoneCavityFill.h"

#include <vtkAlgorithmOutput.h>
#include <vtkCleanPolyData.h>
#include <vtkDiscreteMarchingCubes.h>
#include <vtkImageConstantPad.h>
#include <vtkImageData.h>
#include <vtkDataArray.h>
#include <vtkPointData.h>
#include <vtkType.h>
#include <vtkMarchingCubes.h>
#include <vtkPolyData.h>
#include <vtkPolyDataNormals.h>
#include <vtkWindowedSincPolyDataFilter.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <thread>
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
// Slices of `count` over the hardware threads (same shape as ImplicitCore's). The bone field of a whole skull is
// ~10^8 voxels: one core took tens of seconds per label on the UI thread (user's report, 2026-10-19: "se congeló").
void parallelFor(int count, const std::function<void(int, int)>& body)
{
    if (count <= 0)
        return;
    const int hardware = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    const int threads = std::min(hardware, std::max(1, count / 2));
    if (threads <= 1) {
        body(0, count);
        return;
    }
    const int chunk = (count + threads - 1) / threads;
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(threads));
    for (int t = 0; t < threads; ++t) {
        const int begin = t * chunk;
        const int end = std::min(count, begin + chunk);
        if (begin < end)
            pool.emplace_back(body, begin, end);
    }
    for (auto& thread : pool)
        thread.join();
}

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

// Runs `line(buffer, n, stride)` on every grid line along `axis`, over the threads. Each thread copies its line
// out, works on the copy and writes it back, so the lines never share memory.
void forEachLine(const int dims[3], int axis,
                 const std::function<void(std::vector<float>& line, size_t base, size_t stride)>& work)
{
    const int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
    const size_t stride[3] = {1, static_cast<size_t>(dims[0]), static_cast<size_t>(dims[0]) * dims[1]};
    parallelFor(dims[a2], [&](int begin, int end) {
        std::vector<float> line(static_cast<size_t>(dims[axis]));
        for (int j = begin; j < end; ++j)
            for (int i = 0; i < dims[a1]; ++i)
                work(line, i * stride[a1] + j * stride[a2], stride[axis]);
    });
}

// Squared distance (mm²) from every voxel to the nearest voxel where `seed` is true, with anisotropic spacing,
// written into `g`.
void squaredDistanceTo(const std::vector<unsigned char>& seed, bool seedValue, const int dims[3],
                       const double spacing[3], std::vector<float>& g)
{
    const size_t total = static_cast<size_t>(dims[0]) * dims[1] * dims[2];
    g.resize(total);
    for (size_t i = 0; i < total; ++i)
        g[i] = (seed[i] != 0) == seedValue ? 0.0f : 1e20f;
    for (int axis = 0; axis < 3; ++axis) {
        const int n = dims[axis];
        const float w = static_cast<float>(spacing[axis] * spacing[axis]);
        forEachLine(dims, axis, [&](std::vector<float>& line, size_t base, size_t stride) {
            thread_local std::vector<float> out, z;
            thread_local std::vector<int> v;
            if (out.size() < static_cast<size_t>(n)) {
                out.resize(n);
                z.resize(n + 1);
                v.resize(n);
            }
            for (int q = 0; q < n; ++q)
                line[q] = g[base + q * stride];
            edt1d(line.data(), out.data(), n, w, v, z);
            for (int q = 0; q < n; ++q)
                g[base + q * stride] = out[q];
        });
    }
}

// Separable Gaussian blur in place, truncated at three sigmas, the image edge clamped.
void gaussianInPlace(float* values, const int dims[3], const double spacing[3], double sigmaMm)
{
    for (int axis = 0; axis < 3; ++axis) {
        const double sigma = sigmaMm / spacing[axis];
        const int radius = static_cast<int>(std::ceil(3.0 * sigma));
        if (radius < 1 || dims[axis] < 2)
            continue;
        std::vector<float> kernel(static_cast<size_t>(2 * radius + 1));
        double sum = 0.0;
        for (int t = -radius; t <= radius; ++t)
            sum += kernel[t + radius] = static_cast<float>(std::exp(-0.5 * t * t / (sigma * sigma)));
        for (float& k : kernel)
            k = static_cast<float>(k / sum);
        const int n = dims[axis];
        forEachLine(dims, axis, [&](std::vector<float>& line, size_t base, size_t stride) {
            for (int q = 0; q < n; ++q)
                line[q] = values[base + q * stride];
            for (int q = 0; q < n; ++q) {
                float acc = 0.0f;
                for (int t = -radius; t <= radius; ++t)
                    acc += kernel[t + radius] * line[std::clamp(q + t, 0, n - 1)];
                values[base + q * stride] = acc;
            }
        });
    }
}

// Calls `visit(index, isLabel)` for every voxel of `labels`, reading the scalars through their own type.
template <typename Visit>
void forEachLabelVoxel(vtkImageData* labels, int labelValue, Visit&& visit)
{
    vtkDataArray* scalars = labels->GetPointData()->GetScalars();
    const vtkIdType count = scalars->GetNumberOfTuples();
    switch (scalars->GetDataType()) {
        vtkTemplateMacro({
            const auto* p = static_cast<const VTK_TT*>(scalars->GetVoidPointer(0));
            const int components = scalars->GetNumberOfComponents();
            for (vtkIdType id = 0; id < count; ++id)
                visit(id, static_cast<int>(std::lround(static_cast<double>(p[id * components]))) == labelValue);
        });
    default:
        for (vtkIdType id = 0; id < count; ++id)
            visit(id, static_cast<int>(std::lround(scalars->GetTuple1(id))) == labelValue);
    }
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
    int lo[3] = {full[0], full[1], full[2]}, hi[3] = {-1, -1, -1};
    forEachLabelVoxel(labels, labelValue, [&](vtkIdType id, bool isLabel) {
        if (!isLabel)
            return;
        const int ijk[3] = {static_cast<int>(id % full[0]), static_cast<int>((id / full[0]) % full[1]),
                            static_cast<int>(id / (static_cast<vtkIdType>(full[0]) * full[1]))};
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], ijk[a]);
            hi[a] = std::max(hi[a], ijk[a]);
        }
    });
    if (hi[0] < 0)
        return nullptr;
    // Crop with room for the blur and for the outside distance to settle.
    int cropLo[3], cropHi[3], dims[3];
    for (int a = 0; a < 3; ++a) {
        const int margin = static_cast<int>(std::ceil((3.0 * sigmaMm + 2.0 * spacing[a]) / spacing[a])) + 2;
        cropLo[a] = std::max(0, lo[a] - margin);
        cropHi[a] = std::min(full[a] - 1, hi[a] + margin);
        dims[a] = cropHi[a] - cropLo[a] + 1;
    }
    const size_t total = static_cast<size_t>(dims[0]) * dims[1] * dims[2];
    std::vector<unsigned char> inside(total, 0);
    forEachLabelVoxel(labels, labelValue, [&](vtkIdType id, bool isLabel) {
        if (!isLabel)
            return;
        const int i = static_cast<int>(id % full[0]) - cropLo[0];
        const int j = static_cast<int>((id / full[0]) % full[1]) - cropLo[1];
        const int k = static_cast<int>(id / (static_cast<vtkIdType>(full[0]) * full[1])) - cropLo[2];
        inside[i + static_cast<size_t>(dims[0]) * (j + static_cast<size_t>(dims[1]) * k)] = 1;
    });

    // The signed distance goes straight into the image the blur works on: one float grid besides the distance
    // transform's own (a whole skull at 0.3 mm is ~10^8 voxels, 400 MB per grid).
    auto smoothed = vtkSmartPointer<vtkImageData>::New();
    smoothed->SetExtent(extent[0] + cropLo[0], extent[0] + cropHi[0], extent[2] + cropLo[1], extent[2] + cropHi[1],
                        extent[4] + cropLo[2], extent[4] + cropHi[2]);
    smoothed->SetSpacing(spacing);
    smoothed->SetOrigin(labels->GetOrigin());
    smoothed->SetDirectionMatrix(labels->GetDirectionMatrix());
    smoothed->AllocateScalars(VTK_FLOAT, 1);
    float* values = static_cast<float*>(smoothed->GetScalarPointer());
    {
        std::vector<float> squared;
        squaredDistanceTo(inside, false, dims, spacing, squared);
        for (size_t i = 0; i < total; ++i)
            values[i] = inside[i] ? std::sqrt(squared[i]) : 0.0f;
        squaredDistanceTo(inside, true, dims, spacing, squared);
        for (size_t i = 0; i < total; ++i)
            if (!inside[i])
                values[i] = -std::sqrt(squared[i]);
    }

    // The medial voxels of thin parts: a local maximum of the inside distance, no deeper than `thinMm`. Kept in
    // `inside` itself (2 = ridge), which is not needed as a mask any more.
    const double minSpacing = std::min({spacing[0], spacing[1], spacing[2]});
    const float thinMm = static_cast<float>(std::max(1.5, 2.0 * minSpacing));
    const size_t stride[3] = {1, static_cast<size_t>(dims[0]), static_cast<size_t>(dims[0]) * dims[1]};
    parallelFor(dims[2], [&](int kBegin, int kEnd) {
        for (int k = kBegin; k < kEnd; ++k)
            for (int j = 0; j < dims[1]; ++j)
                for (int i = 0; i < dims[0]; ++i) {
                    const size_t c = i + stride[1] * j + stride[2] * k;
                    if (!inside[c] || values[c] > thinMm)
                        continue;
                    const int ijk[3] = {i, j, k};
                    bool top = true;
                    for (int a = 0; a < 3 && top; ++a) {
                        if (ijk[a] > 0 && values[c - stride[a]] > values[c] + 1e-4f)
                            top = false;
                        if (ijk[a] + 1 < dims[a] && values[c + stride[a]] > values[c] + 1e-4f)
                            top = false;
                    }
                    if (top)
                        inside[c] = 2;
                }
    });

    // Blur in millimetres, the same on every axis.
    gaussianInPlace(values, dims, spacing, sigmaMm);
    const float keep = static_cast<float>(0.5 * minSpacing);
    for (size_t i = 0; i < total; ++i)
        if (inside[i] == 2 && values[i] < keep)
            values[i] = keep;
    std::vector<unsigned char>().swap(inside);

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
    parallelFor(rdims[2], [&](int kBegin, int kEnd) {
        for (int k = kBegin; k < kEnd; ++k) {
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
    });
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
