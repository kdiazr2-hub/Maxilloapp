#include "BoneSplitterService.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <vector>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>

#include <vtkImageData.h>
#include <vtkSmartPointer.h>

#include "NrrdVolumeExporter.h"

#ifndef APP_SOURCE_DIR
#define APP_SOURCE_DIR ""
#endif

namespace
{
struct ComponentInfo
{
    std::int64_t count = 0;
    double sumZ = 0.0;
    double meanZ() const { return count > 0 ? sumZ / static_cast<double>(count) : 0.0; }
};

struct LocalSplitResult
{
    bool ok = false;
    QString error;
    vtkSmartPointer<vtkImageData> output;
    std::int64_t maxillaCount = 0;
    std::int64_t mandibleCount = 0;
};

static bool isBoneLabel(vtkImageData* image, int x, int y, int z)
{
    return std::lround(image->GetScalarComponentAsDouble(x, y, z, 0)) == 1;
}

static LocalSplitResult splitBoneLabelLocally(vtkImageData* labelmap)
{
    LocalSplitResult result;
    if (!labelmap || labelmap->GetNumberOfPoints() == 0) {
        result.error = "No hay labelmap disponible para dividir.";
        return result;
    }

    int extent[6] = {};
    labelmap->GetExtent(extent);
    const int nx = extent[1] - extent[0] + 1;
    const int ny = extent[3] - extent[2] + 1;
    const int nz = extent[5] - extent[4] + 1;
    const auto total = static_cast<size_t>(nx) * static_cast<size_t>(ny) * static_cast<size_t>(nz);

    auto flat = [=](int lx, int ly, int lz) -> size_t {
        return static_cast<size_t>(lx) +
               static_cast<size_t>(nx) *
                   (static_cast<size_t>(ly) +
                    static_cast<size_t>(ny) * static_cast<size_t>(lz));
    };
    auto decode = [=](size_t idx, int& lx, int& ly, int& lz) {
        lx = static_cast<int>(idx % static_cast<size_t>(nx));
        idx /= static_cast<size_t>(nx);
        ly = static_cast<int>(idx % static_cast<size_t>(ny));
        lz = static_cast<int>(idx / static_cast<size_t>(ny));
    };

    std::vector<unsigned char> state(total, 0); // 0 non-bone, 1 pending, 2 visited
    std::vector<std::int64_t> zProfile(static_cast<size_t>(nz), 0);
    std::int64_t boneCount = 0;
    for (int lz = 0; lz < nz; ++lz) {
        const int z = extent[4] + lz;
        for (int ly = 0; ly < ny; ++ly) {
            const int y = extent[2] + ly;
            for (int lx = 0; lx < nx; ++lx) {
                const int x = extent[0] + lx;
                if (!isBoneLabel(labelmap, x, y, z)) continue;
                state[flat(lx, ly, lz)] = 1;
                ++zProfile[static_cast<size_t>(lz)];
                ++boneCount;
            }
        }
    }

    if (boneCount == 0) {
        result.error = "No se encontraron voxeles de hueso (label 1).";
        return result;
    }

    constexpr int dirs[6][3] = {
        { 1, 0, 0}, {-1, 0, 0}, {0,  1, 0},
        { 0,-1, 0}, { 0, 0, 1}, {0,  0,-1}
    };

    auto collectComponents = [&] {
        std::vector<ComponentInfo> components;
        std::deque<size_t> queue;
        for (size_t start = 0; start < total; ++start) {
            if (state[start] != 1) continue;

            ComponentInfo component;
            state[start] = 2;
            queue.push_back(start);
            while (!queue.empty()) {
                const size_t idx = queue.front();
                queue.pop_front();

                int lx = 0, ly = 0, lz = 0;
                decode(idx, lx, ly, lz);
                ++component.count;
                component.sumZ += static_cast<double>(lz);

                for (const auto& d : dirs) {
                    const int nx2 = lx + d[0];
                    const int ny2 = ly + d[1];
                    const int nz2 = lz + d[2];
                    if (nx2 < 0 || nx2 >= nx ||
                        ny2 < 0 || ny2 >= ny ||
                        nz2 < 0 || nz2 >= nz) {
                        continue;
                    }
                    const size_t nidx = flat(nx2, ny2, nz2);
                    if (state[nidx] != 1) continue;
                    state[nidx] = 2;
                    queue.push_back(nidx);
                }
            }
            components.push_back(component);
        }
        std::sort(components.begin(), components.end(),
                  [](const ComponentInfo& a, const ComponentInfo& b) {
                      return a.count > b.count;
                  });
        return components;
    };

    auto components = collectComponents();

    auto output = vtkSmartPointer<vtkImageData>::New();
    output->DeepCopy(labelmap);

    const bool useComponents =
        components.size() >= 2 &&
        components[1].count >= static_cast<std::int64_t>(components[0].count * 0.03);

    if (useComponents) {
        const double splitZ = (components[0].meanZ() + components[1].meanZ()) * 0.5;

        std::fill(state.begin(), state.end(), 0);
        for (int lz = 0; lz < nz; ++lz) {
            const int z = extent[4] + lz;
            for (int ly = 0; ly < ny; ++ly) {
                const int y = extent[2] + ly;
                for (int lx = 0; lx < nx; ++lx) {
                    const int x = extent[0] + lx;
                    if (isBoneLabel(labelmap, x, y, z)) state[flat(lx, ly, lz)] = 1;
                }
            }
        }

        std::deque<size_t> queue;
        std::vector<size_t> voxels;
        for (size_t start = 0; start < total; ++start) {
            if (state[start] != 1) continue;

            ComponentInfo component;
            voxels.clear();
            state[start] = 2;
            queue.push_back(start);
            while (!queue.empty()) {
                const size_t idx = queue.front();
                queue.pop_front();
                voxels.push_back(idx);

                int lx = 0, ly = 0, lz = 0;
                decode(idx, lx, ly, lz);
                ++component.count;
                component.sumZ += static_cast<double>(lz);

                for (const auto& d : dirs) {
                    const int nx2 = lx + d[0];
                    const int ny2 = ly + d[1];
                    const int nz2 = lz + d[2];
                    if (nx2 < 0 || nx2 >= nx ||
                        ny2 < 0 || ny2 >= ny ||
                        nz2 < 0 || nz2 >= nz) {
                        continue;
                    }
                    const size_t nidx = flat(nx2, ny2, nz2);
                    if (state[nidx] != 1) continue;
                    state[nidx] = 2;
                    queue.push_back(nidx);
                }
            }

            const int label = component.meanZ() >= splitZ ? 5 : 6;
            for (size_t idx : voxels) {
                int lx = 0, ly = 0, lz = 0;
                decode(idx, lx, ly, lz);
                output->SetScalarComponentFromDouble(
                    extent[0] + lx, extent[2] + ly, extent[4] + lz, 0, label);
            }
            if (label == 5) result.maxillaCount += component.count;
            else result.mandibleCount += component.count;
        }
    } else {
        int zMin = -1;
        int zMax = -1;
        for (int lz = 0; lz < nz; ++lz) {
            if (zProfile[static_cast<size_t>(lz)] <= 0) continue;
            if (zMin < 0) zMin = lz;
            zMax = lz;
        }
        if (zMin < 0 || zMax - zMin < 3) {
            result.error = "Hueso insuficiente para dividir maxilar y mandíbula.";
            return result;
        }

        const int margin = std::max(1, (zMax - zMin) / 8);
        const int searchStart = zMin + margin;
        const int searchEnd = zMax - margin;
        int splitZ = (zMin + zMax) / 2;
        std::int64_t bestCount = std::numeric_limits<std::int64_t>::max();
        for (int lz = searchStart; lz <= searchEnd; ++lz) {
            const auto count = zProfile[static_cast<size_t>(lz)];
            if (count < bestCount) {
                bestCount = count;
                splitZ = lz;
            }
        }

        for (int lz = 0; lz < nz; ++lz) {
            const int z = extent[4] + lz;
            const int label = lz >= splitZ ? 5 : 6;
            for (int ly = 0; ly < ny; ++ly) {
                const int y = extent[2] + ly;
                for (int lx = 0; lx < nx; ++lx) {
                    const int x = extent[0] + lx;
                    if (!isBoneLabel(labelmap, x, y, z)) continue;
                    output->SetScalarComponentFromDouble(x, y, z, 0, label);
                    if (label == 5) ++result.maxillaCount;
                    else ++result.mandibleCount;
                }
            }
        }
    }

    if (result.maxillaCount == 0 || result.mandibleCount == 0) {
        result.error = "La división dejó una estructura vacía. Revise la máscara de hueso.";
        return result;
    }

    result.output = output;
    result.ok = true;
    return result;
}

struct ConservativeComponent
{
    std::int64_t count = 0;
    double sumZ = 0.0;
    int minX = std::numeric_limits<int>::max();
    int maxX = std::numeric_limits<int>::min();
    int minZ = std::numeric_limits<int>::max();
    int maxZ = std::numeric_limits<int>::min();
    std::vector<size_t> voxels;

    void add(size_t idx, int lx, int lz)
    {
        voxels.push_back(idx);
        ++count;
        sumZ += static_cast<double>(lz);
        minX = std::min(minX, lx);
        maxX = std::max(maxX, lx);
        minZ = std::min(minZ, lz);
        maxZ = std::max(maxZ, lz);
    }

    double meanZ() const
    {
        return count > 0 ? sumZ / static_cast<double>(count) : 0.0;
    }

    int spanX() const
    {
        return maxX >= minX ? maxX - minX + 1 : 0;
    }

    int spanZ() const
    {
        return maxZ >= minZ ? maxZ - minZ + 1 : 0;
    }
};

static LocalSplitResult splitBoneLabelConservatively(vtkImageData* labelmap)
{
    LocalSplitResult result;
    if (!labelmap || labelmap->GetNumberOfPoints() == 0) {
        result.error = "No hay labelmap disponible para dividir.";
        return result;
    }

    int extent[6] = {};
    labelmap->GetExtent(extent);
    const int nx = extent[1] - extent[0] + 1;
    const int ny = extent[3] - extent[2] + 1;
    const int nz = extent[5] - extent[4] + 1;
    const auto total = static_cast<size_t>(nx) * static_cast<size_t>(ny) * static_cast<size_t>(nz);

    auto flat = [=](int lx, int ly, int lz) -> size_t {
        return static_cast<size_t>(lx) +
               static_cast<size_t>(nx) *
                   (static_cast<size_t>(ly) +
                    static_cast<size_t>(ny) * static_cast<size_t>(lz));
    };
    auto decode = [=](size_t idx, int& lx, int& ly, int& lz) {
        lx = static_cast<int>(idx % static_cast<size_t>(nx));
        idx /= static_cast<size_t>(nx);
        ly = static_cast<int>(idx % static_cast<size_t>(ny));
        lz = static_cast<int>(idx / static_cast<size_t>(ny));
    };

    std::vector<std::int64_t> zProfile(static_cast<size_t>(nz), 0);
    std::int64_t boneCount = 0;
    int zMin = -1;
    int zMax = -1;
    for (int lz = 0; lz < nz; ++lz) {
        const int z = extent[4] + lz;
        for (int ly = 0; ly < ny; ++ly) {
            const int y = extent[2] + ly;
            for (int lx = 0; lx < nx; ++lx) {
                const int x = extent[0] + lx;
                if (!isBoneLabel(labelmap, x, y, z)) continue;
                ++zProfile[static_cast<size_t>(lz)];
                ++boneCount;
                if (zMin < 0) zMin = lz;
                zMax = lz;
            }
        }
    }

    if (boneCount == 0) {
        result.error = "No se encontraron voxeles de hueso (label 1).";
        return result;
    }
    if (zMin < 0 || zMax - zMin < 4) {
        result.error = "Hueso insuficiente para dividir maxilar y mandibula.";
        return result;
    }

    const int height = zMax - zMin + 1;
    std::vector<double> smoothZ(static_cast<size_t>(nz), 0.0);
    for (int lz = 0; lz < nz; ++lz) {
        std::int64_t sum = 0;
        int samples = 0;
        for (int dz = -2; dz <= 2; ++dz) {
            const int iz = lz + dz;
            if (iz < 0 || iz >= nz) continue;
            sum += zProfile[static_cast<size_t>(iz)];
            ++samples;
        }
        smoothZ[static_cast<size_t>(lz)] =
            samples > 0 ? static_cast<double>(sum) / static_cast<double>(samples) : 0.0;
    }

    // The inter-arch gap is searched only in the middle third. This avoids
    // choosing empty slices at the top of the skull or bottom of the neck.
    int searchStart = zMin + std::max(2, static_cast<int>(height * 0.32));
    int searchEnd = zMin + std::max(3, static_cast<int>(height * 0.62));
    searchStart = std::min(searchStart, zMax - 1);
    searchEnd = std::min(std::max(searchEnd, searchStart), zMax);

    int splitZ = (searchStart + searchEnd) / 2;
    double bestCount = std::numeric_limits<double>::max();
    for (int lz = searchStart; lz <= searchEnd; ++lz) {
        const double count = smoothZ[static_cast<size_t>(lz)];
        if (count < bestCount) {
            bestCount = count;
            splitZ = lz;
        }
    }

    const int maxillaUpper =
        std::min(zMax, splitZ + std::max(4, static_cast<int>((zMax - splitZ) * 0.25)));
    const int minMandibleSpanX = std::max(12, static_cast<int>(nx * 0.14));
    const int lowerHeight = std::max(1, splitZ - zMin + 1);

    // Estimate the anterior facial band on the Y axis from the lower/mid-face
    // jaw profile. This is the key guard against including posterior skull base
    // and cervical vertebrae in a simple HU-based bone mask.
    std::vector<double> yScore(static_cast<size_t>(ny), 0.0);
    const int jawSearchZMin = zMin + std::max(0, static_cast<int>(height * 0.08));
    const int jawSearchZMax = std::min(
        zMax, splitZ + std::max(2, static_cast<int>(height * 0.12)));
    for (int ly = 0; ly < ny; ++ly) {
        int xMin = nx;
        int xMax = -1;
        std::int64_t count = 0;
        const int y = extent[2] + ly;
        for (int lz = jawSearchZMin; lz <= jawSearchZMax; ++lz) {
            const int z = extent[4] + lz;
            for (int lx = 0; lx < nx; ++lx) {
                const int x = extent[0] + lx;
                if (!isBoneLabel(labelmap, x, y, z)) continue;
                xMin = std::min(xMin, lx);
                xMax = std::max(xMax, lx);
                ++count;
            }
        }
        if (count <= 0 || xMax < xMin) continue;
        const int spanX = xMax - xMin + 1;
        if (spanX < static_cast<int>(nx * 0.12)) continue;
        yScore[static_cast<size_t>(ly)] =
            static_cast<double>(count) * static_cast<double>(spanX);
    }

    int jawPeakY = ny / 2;
    double peakYScore = 0.0;
    for (int ly = 0; ly < ny; ++ly) {
        const double score = yScore[static_cast<size_t>(ly)];
        if (score > peakYScore) {
            peakYScore = score;
            jawPeakY = ly;
        }
    }

    int yBandMin = 0;
    int yBandMax = ny - 1;
    if (peakYScore > 0.0) {
        const double threshold = peakYScore * 0.10;
        yBandMin = jawPeakY;
        yBandMax = jawPeakY;
        while (yBandMin > 0 &&
               yScore[static_cast<size_t>(yBandMin - 1)] >= threshold) {
            --yBandMin;
        }
        while (yBandMax + 1 < ny &&
               yScore[static_cast<size_t>(yBandMax + 1)] >= threshold) {
            ++yBandMax;
        }

        const int pad = std::max(8, static_cast<int>(ny * 0.08));
        yBandMin = std::max(0, yBandMin - pad);
        yBandMax = std::min(ny - 1, yBandMax + pad);

        const int maxBandWidth = std::max(32, static_cast<int>(ny * 0.46));
        if (yBandMax - yBandMin + 1 > maxBandWidth) {
            yBandMin = std::max(0, jawPeakY - maxBandWidth / 2);
            yBandMax = std::min(ny - 1, yBandMin + maxBandWidth - 1);
            yBandMin = std::max(0, yBandMax - maxBandWidth + 1);
        }
    }

    auto output = vtkSmartPointer<vtkImageData>::New();
    output->DeepCopy(labelmap);

    for (int lz = 0; lz < nz; ++lz) {
        const int z = extent[4] + lz;
        for (int ly = 0; ly < ny; ++ly) {
            const int y = extent[2] + ly;
            for (int lx = 0; lx < nx; ++lx) {
                const int x = extent[0] + lx;
                if (isBoneLabel(labelmap, x, y, z)) {
                    output->SetScalarComponentFromDouble(x, y, z, 0, 0.0);
                }
            }
        }
    }

    // Maxilla: keep a conservative mid-face slab above the inter-arch split.
    // This prevents a generic bone mask from turning the full cranium into maxilla.
    for (int lz = splitZ; lz <= maxillaUpper; ++lz) {
        const int z = extent[4] + lz;
        for (int ly = yBandMin; ly <= yBandMax; ++ly) {
            const int y = extent[2] + ly;
            for (int lx = 0; lx < nx; ++lx) {
                const int x = extent[0] + lx;
                if (!isBoneLabel(labelmap, x, y, z)) continue;
                output->SetScalarComponentFromDouble(x, y, z, 0, 5.0);
                ++result.maxillaCount;
            }
        }
    }

    constexpr int dirs[6][3] = {
        { 1, 0, 0}, {-1, 0, 0}, {0,  1, 0},
        { 0,-1, 0}, { 0, 0, 1}, {0,  0,-1}
    };
    std::vector<unsigned char> state(total, 0);
    for (int lz = zMin; lz <= splitZ; ++lz) {
        const int z = extent[4] + lz;
        for (int ly = yBandMin; ly <= yBandMax; ++ly) {
            const int y = extent[2] + ly;
            for (int lx = 0; lx < nx; ++lx) {
                const int x = extent[0] + lx;
                if (isBoneLabel(labelmap, x, y, z)) state[flat(lx, ly, lz)] = 1;
            }
        }
    }

    std::vector<ConservativeComponent> components;
    std::deque<size_t> queue;
    for (size_t start = 0; start < total; ++start) {
        if (state[start] != 1) continue;
        ConservativeComponent component;
        state[start] = 2;
        queue.push_back(start);
        while (!queue.empty()) {
            const size_t idx = queue.front();
            queue.pop_front();

            int lx = 0, ly = 0, lz = 0;
            decode(idx, lx, ly, lz);
            component.add(idx, lx, lz);

            for (const auto& d : dirs) {
                const int nx2 = lx + d[0];
                const int ny2 = ly + d[1];
                const int nz2 = lz + d[2];
                if (nx2 < 0 || nx2 >= nx ||
                    ny2 < 0 || ny2 >= ny ||
                    nz2 < 0 || nz2 >= nz) {
                    continue;
                }
                const size_t nidx = flat(nx2, ny2, nz2);
                if (state[nidx] != 1) continue;
                state[nidx] = 2;
                queue.push_back(nidx);
            }
        }
        components.push_back(std::move(component));
    }

    double bestScore = 0.0;
    for (const auto& component : components) {
        if (component.count < 20) continue;
        const double score =
            static_cast<double>(component.count) *
            static_cast<double>(std::max(1, component.spanX()));
        bestScore = std::max(bestScore, score);
    }

    for (const auto& component : components) {
        if (component.count < 20) continue;
        const double score =
            static_cast<double>(component.count) *
            static_cast<double>(std::max(1, component.spanX()));
        const bool broadEnough = component.spanX() >= minMandibleSpanX;
        const bool importantComponent = bestScore > 0.0 && score >= bestScore * 0.08;
        const bool deepInferiorNarrow =
            component.meanZ() < zMin + lowerHeight * 0.18 &&
            component.spanX() < static_cast<int>(nx * 0.24);
        const bool tallCentralStack =
            component.spanZ() > static_cast<int>(lowerHeight * 0.80) &&
            component.spanX() < static_cast<int>(nx * 0.22);

        if (!(broadEnough && importantComponent) || deepInferiorNarrow || tallCentralStack) {
            continue;
        }

        for (size_t idx : component.voxels) {
            int lx = 0, ly = 0, lz = 0;
            decode(idx, lx, ly, lz);
            output->SetScalarComponentFromDouble(
                extent[0] + lx, extent[2] + ly, extent[4] + lz, 0, 6.0);
            ++result.mandibleCount;
        }
    }

    if (result.mandibleCount == 0 && !components.empty()) {
        auto best = std::max_element(
            components.begin(), components.end(),
            [](const ConservativeComponent& a, const ConservativeComponent& b) {
                const double scoreA =
                    static_cast<double>(a.count) * static_cast<double>(std::max(1, a.spanX()));
                const double scoreB =
                    static_cast<double>(b.count) * static_cast<double>(std::max(1, b.spanX()));
                return scoreA < scoreB;
            });
        if (best != components.end()) {
            for (size_t idx : best->voxels) {
                int lx = 0, ly = 0, lz = 0;
                decode(idx, lx, ly, lz);
                output->SetScalarComponentFromDouble(
                    extent[0] + lx, extent[2] + ly, extent[4] + lz, 0, 6.0);
                ++result.mandibleCount;
            }
        }
    }

    if (result.maxillaCount == 0 || result.mandibleCount == 0) {
        result.error = "La division dejo una estructura vacia. Revise la mascara de hueso.";
        return result;
    }

    result.output = output;
    result.ok = true;
    return result;
}

struct MandibleComponent
{
    std::int64_t count = 0;
    double sumY = 0.0;
    double sumZ = 0.0;
    int minX = std::numeric_limits<int>::max();
    int maxX = std::numeric_limits<int>::min();
    int minY = std::numeric_limits<int>::max();
    int maxY = std::numeric_limits<int>::min();
    int minZ = std::numeric_limits<int>::max();
    int maxZ = std::numeric_limits<int>::min();
    std::vector<size_t> voxels;

    void add(size_t idx, int lx, int ly, int lz)
    {
        voxels.push_back(idx);
        ++count;
        sumY += static_cast<double>(ly);
        sumZ += static_cast<double>(lz);
        minX = std::min(minX, lx);
        maxX = std::max(maxX, lx);
        minY = std::min(minY, ly);
        maxY = std::max(maxY, ly);
        minZ = std::min(minZ, lz);
        maxZ = std::max(maxZ, lz);
    }

    double meanY() const
    {
        return count > 0 ? sumY / static_cast<double>(count) : 0.0;
    }

    double meanZ() const
    {
        return count > 0 ? sumZ / static_cast<double>(count) : 0.0;
    }

    int spanX() const
    {
        return maxX >= minX ? maxX - minX + 1 : 0;
    }

    int spanY() const
    {
        return maxY >= minY ? maxY - minY + 1 : 0;
    }

    int spanZ() const
    {
        return maxZ >= minZ ? maxZ - minZ + 1 : 0;
    }
};

static LocalSplitResult splitBoneLabelMandibleOnly(vtkImageData* labelmap)
{
    LocalSplitResult result;
    if (!labelmap || labelmap->GetNumberOfPoints() == 0) {
        result.error = "No hay labelmap disponible para dividir.";
        return result;
    }

    int extent[6] = {};
    labelmap->GetExtent(extent);
    const int nx = extent[1] - extent[0] + 1;
    const int ny = extent[3] - extent[2] + 1;
    const int nz = extent[5] - extent[4] + 1;
    const auto total = static_cast<size_t>(nx) * static_cast<size_t>(ny) * static_cast<size_t>(nz);

    auto flat = [=](int lx, int ly, int lz) -> size_t {
        return static_cast<size_t>(lx) +
               static_cast<size_t>(nx) *
                   (static_cast<size_t>(ly) +
                    static_cast<size_t>(ny) * static_cast<size_t>(lz));
    };
    auto decode = [=](size_t idx, int& lx, int& ly, int& lz) {
        lx = static_cast<int>(idx % static_cast<size_t>(nx));
        idx /= static_cast<size_t>(nx);
        ly = static_cast<int>(idx % static_cast<size_t>(ny));
        lz = static_cast<int>(idx / static_cast<size_t>(ny));
    };

    std::vector<unsigned char> state(total, 0);
    std::vector<std::int64_t> zProfile(static_cast<size_t>(nz), 0);
    std::int64_t boneCount = 0;
    int zMin = -1;
    int zMax = -1;

    for (int lz = 0; lz < nz; ++lz) {
        const int z = extent[4] + lz;
        for (int ly = 0; ly < ny; ++ly) {
            const int y = extent[2] + ly;
            for (int lx = 0; lx < nx; ++lx) {
                const int x = extent[0] + lx;
                if (!isBoneLabel(labelmap, x, y, z)) continue;
                state[flat(lx, ly, lz)] = 1;
                ++zProfile[static_cast<size_t>(lz)];
                ++boneCount;
                if (zMin < 0) zMin = lz;
                zMax = lz;
            }
        }
    }

    if (boneCount == 0) {
        result.error = "No se encontro mascara de hueso original (label 1). Ejecute Hueso antes de Max/Mand.";
        return result;
    }
    if (zMin < 0 || zMax - zMin < 4) {
        result.error = "Hueso insuficiente para aislar mandibula.";
        return result;
    }

    const int height = zMax - zMin + 1;
    std::vector<double> smoothZ(static_cast<size_t>(nz), 0.0);
    for (int lz = 0; lz < nz; ++lz) {
        std::int64_t sum = 0;
        int samples = 0;
        for (int dz = -2; dz <= 2; ++dz) {
            const int iz = lz + dz;
            if (iz < 0 || iz >= nz) continue;
            sum += zProfile[static_cast<size_t>(iz)];
            ++samples;
        }
        smoothZ[static_cast<size_t>(lz)] =
            samples > 0 ? static_cast<double>(sum) / static_cast<double>(samples) : 0.0;
    }

    int searchStart = zMin + std::max(2, static_cast<int>(height * 0.28));
    int searchEnd = zMin + std::max(3, static_cast<int>(height * 0.62));
    searchStart = std::min(searchStart, zMax - 1);
    searchEnd = std::min(std::max(searchEnd, searchStart), zMax);

    int splitZ = (searchStart + searchEnd) / 2;
    double bestValley = std::numeric_limits<double>::max();
    for (int lz = searchStart; lz <= searchEnd; ++lz) {
        const double count = smoothZ[static_cast<size_t>(lz)];
        if (count < bestValley) {
            bestValley = count;
            splitZ = lz;
        }
    }

    constexpr int dirs[6][3] = {
        { 1, 0, 0}, {-1, 0, 0}, {0,  1, 0},
        { 0,-1, 0}, { 0, 0, 1}, {0,  0,-1}
    };

    // The mandible may be connected to the maxilla through touching teeth or
    // threshold bridges. Search components only in the lower facial slab first;
    // this virtual cut breaks those superior bridges before selecting a seed.
    std::fill(state.begin(), state.end(), 0);
    const int seedUpperZ =
        std::min(zMax, splitZ + std::max(2, static_cast<int>(height * 0.05)));
    for (int lz = zMin; lz <= seedUpperZ; ++lz) {
        const int z = extent[4] + lz;
        for (int ly = 0; ly < ny; ++ly) {
            const int y = extent[2] + ly;
            for (int lx = 0; lx < nx; ++lx) {
                const int x = extent[0] + lx;
                if (isBoneLabel(labelmap, x, y, z)) {
                    state[flat(lx, ly, lz)] = 1;
                }
            }
        }
    }

    std::vector<MandibleComponent> components;
    std::deque<size_t> queue;
    for (size_t start = 0; start < total; ++start) {
        if (state[start] != 1) continue;

        MandibleComponent component;
        state[start] = 2;
        queue.push_back(start);
        while (!queue.empty()) {
            const size_t idx = queue.front();
            queue.pop_front();

            int lx = 0, ly = 0, lz = 0;
            decode(idx, lx, ly, lz);
            component.add(idx, lx, ly, lz);

            for (const auto& d : dirs) {
                const int nx2 = lx + d[0];
                const int ny2 = ly + d[1];
                const int nz2 = lz + d[2];
                if (nx2 < 0 || nx2 >= nx ||
                    ny2 < 0 || ny2 >= ny ||
                    nz2 < 0 || nz2 >= nz) {
                    continue;
                }
                const size_t nidx = flat(nx2, ny2, nz2);
                if (state[nidx] != 1) continue;
                state[nidx] = 2;
                queue.push_back(nidx);
            }
        }
        components.push_back(std::move(component));
    }

    if (components.empty()) {
        result.error = "No se encontraron componentes oseas para dividir.";
        return result;
    }

    const int minMandibleSpanX = std::max(12, static_cast<int>(nx * 0.15));
    const int maxMandibleSpanZ = std::max(8, static_cast<int>(height * 0.55));

    int bestIndex = -1;
    double bestScore = -1.0;
    for (int i = 0; i < static_cast<int>(components.size()); ++i) {
        const auto& component = components[static_cast<size_t>(i)];
        if (component.count < 50) continue;
        if (component.spanX() < minMandibleSpanX) continue;

        const double zNorm =
            (component.meanZ() - static_cast<double>(zMin)) / static_cast<double>(height);
        const bool touchesInferiorEdge =
            component.minZ <= zMin + std::max(1, static_cast<int>(height * 0.04));
        const bool tooTallForMandible = component.spanZ() > maxMandibleSpanZ;
        const bool lowNeckComponent =
            touchesInferiorEdge && zNorm < 0.34;

        if (tooTallForMandible || lowNeckComponent) continue;

        // Mandible is usually a broad, compact component in the lower-mid
        // facial third. Vertebrae may be wide, but they are lower and/or taller.
        const double idealZ = 0.38;
        const double zWeight =
            1.0 - std::min(0.85, std::abs(zNorm - idealZ) / 0.42);
        const double topReach =
            static_cast<double>(component.maxZ - zMin) / static_cast<double>(height);
        const double compactZ =
            1.0 / (1.0 + static_cast<double>(component.spanZ()) /
                         std::max(1.0, height * 0.22));
        const double inferiorPenalty = touchesInferiorEdge ? 0.25 : 1.0;
        const double score =
            static_cast<double>(component.count) *
            static_cast<double>(component.spanX()) *
            (1.0 + topReach) * zWeight * compactZ * inferiorPenalty;

        if (score > bestScore) {
            bestScore = score;
            bestIndex = i;
        }
    }

    if (bestIndex < 0) {
        // Last resort: choose the broadest component that does not look like
        // the inferior cervical stack. This keeps the tool usable on cropped scans.
        for (int i = 0; i < static_cast<int>(components.size()); ++i) {
            const auto& component = components[static_cast<size_t>(i)];
            if (component.count < 50 || component.spanX() < minMandibleSpanX) continue;
            const bool touchesInferiorEdge =
                component.minZ <= zMin + std::max(1, static_cast<int>(height * 0.04));
            const double score =
                static_cast<double>(component.spanX()) *
                static_cast<double>(component.count) *
                (touchesInferiorEdge ? 0.35 : 1.0);
            if (score > bestScore) {
                bestScore = score;
                bestIndex = i;
            }
        }
        if (bestIndex < 0) {
            result.error = "No se pudo aislar una componente mandibular confiable.";
            return result;
        }
    }

    const auto& seedComponent = components[static_cast<size_t>(bestIndex)];

    // Grow the selected lower-slab seed back into the source bone mask, but
    // keep it under the inter-arch cut and near the selected Y range. This
    // recovers mandibular body/ramus while preventing the fill from climbing
    // into maxilla/cranium or crossing to the cervical stack.
    std::fill(state.begin(), state.end(), 0);
    const int growUpperZ =
        std::min(zMax, splitZ + std::max(2, static_cast<int>(height * 0.10)));
    const int yPad = std::max(6, static_cast<int>(ny * 0.06));
    const int growMinY = std::max(0, seedComponent.minY - yPad);
    const int growMaxY = std::min(ny - 1, seedComponent.maxY + yPad);
    for (int lz = zMin; lz <= growUpperZ; ++lz) {
        const int z = extent[4] + lz;
        for (int ly = growMinY; ly <= growMaxY; ++ly) {
            const int y = extent[2] + ly;
            for (int lx = 0; lx < nx; ++lx) {
                const int x = extent[0] + lx;
                if (isBoneLabel(labelmap, x, y, z)) {
                    state[flat(lx, ly, lz)] = 1;
                }
            }
        }
    }

    std::vector<size_t> mandibleVoxels;
    for (size_t idx : seedComponent.voxels) {
        int lx = 0, ly = 0, lz = 0;
        decode(idx, lx, ly, lz);
        if (lz < zMin || lz > growUpperZ || ly < growMinY || ly > growMaxY) continue;
        const size_t start = flat(lx, ly, lz);
        if (state[start] != 1) continue;
        state[start] = 2;
        queue.push_back(start);
        while (!queue.empty()) {
            const size_t current = queue.front();
            queue.pop_front();
            mandibleVoxels.push_back(current);

            int cx = 0, cy = 0, cz = 0;
            decode(current, cx, cy, cz);
            for (const auto& d : dirs) {
                const int nx2 = cx + d[0];
                const int ny2 = cy + d[1];
                const int nz2 = cz + d[2];
                if (nx2 < 0 || nx2 >= nx ||
                    ny2 < growMinY || ny2 > growMaxY ||
                    nz2 < zMin || nz2 > growUpperZ) {
                    continue;
                }
                const size_t nidx = flat(nx2, ny2, nz2);
                if (state[nidx] != 1) continue;
                state[nidx] = 2;
                queue.push_back(nidx);
            }
        }
    }

    auto output = vtkSmartPointer<vtkImageData>::New();
    output->DeepCopy(labelmap);

    // DentalSegmentator-like behavior for this stage:
    //   label 6 = isolated mandible
    //   label 5 = every remaining voxel from the source bone mask
    for (int lz = 0; lz < nz; ++lz) {
        const int z = extent[4] + lz;
        for (int ly = 0; ly < ny; ++ly) {
            const int y = extent[2] + ly;
            for (int lx = 0; lx < nx; ++lx) {
                const int x = extent[0] + lx;
                if (!isBoneLabel(labelmap, x, y, z)) continue;
                output->SetScalarComponentFromDouble(x, y, z, 0, 5.0);
                ++result.maxillaCount;
            }
        }
    }

    for (size_t idx : mandibleVoxels) {
        int lx = 0, ly = 0, lz = 0;
        decode(idx, lx, ly, lz);
        output->SetScalarComponentFromDouble(
            extent[0] + lx, extent[2] + ly, extent[4] + lz, 0, 6.0);
        --result.maxillaCount;
        ++result.mandibleCount;
    }

    if (result.maxillaCount <= 0 || result.mandibleCount <= 0) {
        result.error = "La division dejo una estructura vacia. Revise la mascara de hueso.";
        return result;
    }

    result.output = output;
    result.ok = true;
    return result;
}
}

// ─────────────────────────────────────────────────────────────────────────────
BoneSplitterService::BoneSplitterService(QObject* parent)
    : QObject(parent)
{}

BoneSplitterService::~BoneSplitterService()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(3000);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
bool BoneSplitterService::isRunning() const
{
    return m_process && m_process->state() != QProcess::NotRunning;
}

// ─────────────────────────────────────────────────────────────────────────────
QString BoneSplitterService::findPython() const
{
    // 1. Same env-var the existing segmentator service uses
    const QString env = QProcessEnvironment::systemEnvironment()
                            .value("DENTALSEGMENTATOR_PYTHON").trimmed();
    if (!env.isEmpty() && QFileInfo::exists(env)) return env;

    // 2. System PATH
    for (const char* name : {"python3", "python"}) {
        const QString p = QStandardPaths::findExecutable(QString::fromLatin1(name));
        if (!p.isEmpty()) return p;
    }
    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
QString BoneSplitterService::findScript() const
{
    // Next to the executable (deployed path)
    const QString deployed =
        QDir(QCoreApplication::applicationDirPath())
            .filePath("scripts/split_maxilla_mandible.py");
    if (QFileInfo::exists(deployed)) return deployed;

    // Source tree (development path)
    const QString srcDir = QString::fromLocal8Bit(APP_SOURCE_DIR);
    if (!srcDir.isEmpty()) {
        const QString devPath =
            QDir(srcDir).filePath("scripts/split_maxilla_mandible.py");
        if (QFileInfo::exists(devPath)) return devPath;
    }

    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
void BoneSplitterService::emitError(const QString& msg)
{
    emit progressChanged(0);
    emit errorOccurred(msg);
}

// ─────────────────────────────────────────────────────────────────────────────
void BoneSplitterService::split(vtkSmartPointer<vtkImageData> labelmap,
                                const QString& outputDir)
{
    if (isRunning()) {
        emitError("Ya hay una operacion de division en curso.");
        return;
    }
    if (!labelmap || labelmap->GetNumberOfPoints() == 0) {
        emitError("No hay labelmap disponible para dividir.");
        return;
    }

    emit statusChanged("Dividiendo hueso en maxilar y mandibula...");
    emit progressChanged(5);

    const auto splitResult = splitBoneLabelMandibleOnly(labelmap);
    if (!splitResult.ok) {
        emitError(splitResult.error);
        return;
    }

    QDir().mkpath(outputDir);
    m_outputPath = QDir(outputDir).filePath("bone_split_output.nrrd");

    emit statusChanged("Guardando labelmap Max/Mand...");
    emit progressChanged(90);

    QString localExportErr;
    if (!NrrdVolumeExporter::exportToFile(splitResult.output, m_outputPath, &localExportErr)) {
        emitError("Error exportando resultado Max/Mand: " + localExportErr);
        return;
    }

    emit statusChanged(
        QString("Max/Mand listo. Maxilar: %1 vox, mandibula: %2 vox.")
            .arg(splitResult.maxillaCount)
            .arg(splitResult.mandibleCount));
    emit progressChanged(100);
    emit splitFinished(m_outputPath);
    return;
    if (isRunning()) {
        emitError("Ya hay una operación de división en curso.");
        return;
    }
    if (!labelmap || labelmap->GetNumberOfPoints() == 0) {
        emitError("No hay labelmap disponible para dividir.");
        return;
    }

    // ── Locate Python and script ──────────────────────────────────────────
    const QString python = findPython();
    if (python.isEmpty()) {
        emitError(
            "No se encontró Python.\n\n"
            "Instale Python con numpy y scipy:\n"
            "  pip install numpy scipy\n\n"
            "O defina la variable de entorno DENTALSEGMENTATOR_PYTHON "
            "apuntando al ejecutable de Python.");
        return;
    }
    const QString script = findScript();
    if (script.isEmpty()) {
        emitError("No se encontró el script split_maxilla_mandible.py "
                  "en el directorio de la aplicación.");
        return;
    }

    // ── Export labelmap to a temp NRRD ───────────────────────────────────
    emit statusChanged("Exportando labelmap...");
    emit progressChanged(2);

    const QString inputNrrd =
        QDir(outputDir).filePath("bone_split_input.nrrd");
    m_outputPath =
        QDir(outputDir).filePath("bone_split_output.nrrd");

    QString exportErr;
    if (!NrrdVolumeExporter::exportToFile(labelmap, inputNrrd, &exportErr)) {
        emitError("Error exportando labelmap: " + exportErr);
        return;
    }

    // ── Launch Python ──────────────────────────────────────────────────────
    emit statusChanged("Ejecutando análisis de componentes...");
    emit progressChanged(5);

    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);

    connect(m_process, &QProcess::readyRead,
            this, &BoneSplitterService::onReadyRead);
    connect(m_process,
            static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(
                &QProcess::finished),
            this, &BoneSplitterService::onProcessFinished);
    connect(m_process, &QProcess::errorOccurred,
            this, &BoneSplitterService::onProcessError);

    m_process->start(python, {script, inputNrrd, m_outputPath});
}

// ─────────────────────────────────────────────────────────────────────────────
void BoneSplitterService::onReadyRead()
{
    if (!m_process) return;
    const QString text =
        QString::fromLocal8Bit(m_process->readAll()).trimmed();
    if (text.isEmpty()) return;

    // Each line may be "N% message" or just text
    for (const QString& line : text.split('\n', Qt::SkipEmptyParts)) {
        const QString ln = line.trimmed();
        emit statusChanged(ln);

        // Parse "N% …" progress tokens
        static const QRegularExpression re(R"(^(\d+)%\s)");
        const auto m = re.match(ln);
        if (m.hasMatch())
            emit progressChanged(m.captured(1).toInt());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void BoneSplitterService::onProcessFinished(int exitCode,
                                            QProcess::ExitStatus status)
{
    m_process->deleteLater();
    m_process = nullptr;

    if (status != QProcess::NormalExit || exitCode != 0) {
        emitError(QString("El script de división terminó con error (código %1).\n"
                          "Asegúrese de tener numpy y scipy instalados:\n"
                          "  pip install numpy scipy")
                      .arg(exitCode));
        return;
    }
    if (!QFileInfo::exists(m_outputPath)) {
        emitError("El script terminó sin generar el archivo de salida.");
        return;
    }

    emit progressChanged(100);
    emit splitFinished(m_outputPath);
}

// ─────────────────────────────────────────────────────────────────────────────
void BoneSplitterService::onProcessError(QProcess::ProcessError error)
{
    Q_UNUSED(error)
    const QString msg = m_process ? m_process->errorString() : "Error desconocido";
    if (m_process) { m_process->deleteLater(); m_process = nullptr; }
    emitError("Error iniciando Python: " + msg);
}
