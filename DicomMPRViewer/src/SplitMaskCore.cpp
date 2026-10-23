#include "SplitMaskCore.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include <vtkImageData.h>
#include <vtkMatrix3x3.h>

namespace
{
constexpr int kIntensityBuckets = 4096;

struct Grid
{
    int extent[6] = {};
    int nx = 0;
    int ny = 0;
    int nz = 0;
    vtkIdType n = 0;
    double spacing[3] = {1.0, 1.0, 1.0};
    double direction[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
};

Grid gridOf(vtkImageData* image)
{
    Grid grid;
    if (!image) return grid;
    image->GetExtent(grid.extent);
    grid.nx = grid.extent[1] - grid.extent[0] + 1;
    grid.ny = grid.extent[3] - grid.extent[2] + 1;
    grid.nz = grid.extent[5] - grid.extent[4] + 1;
    if (grid.nx > 0 && grid.ny > 0 && grid.nz > 0)
        grid.n = static_cast<vtkIdType>(grid.nx) * grid.ny * grid.nz;
    image->GetSpacing(grid.spacing);
    if (auto* matrix = image->GetDirectionMatrix())
        vtkMatrix3x3::DeepCopy(grid.direction, matrix->GetData());
    for (double& step : grid.spacing)
        if (!(step > 0.0)) step = 1.0;
    return grid;
}

bool sameGeometry(const Grid& a, const Grid& b)
{
    return a.n > 0 && a.n == b.n
        && a.extent[0] == b.extent[0] && a.extent[2] == b.extent[2] && a.extent[4] == b.extent[4]
        && a.nx == b.nx && a.ny == b.ny && a.nz == b.nz;
}

vtkIdType indexOf(int x, int y, int z, int nx, int ny)
{
    return static_cast<vtkIdType>(x) + static_cast<vtkIdType>(nx) * (y + static_cast<vtkIdType>(ny) * z);
}

double stepLength(const Grid& grid, int dx, int dy, int dz)
{
    const double vx = dx * grid.spacing[0];
    const double vy = dy * grid.spacing[1];
    const double vz = dz * grid.spacing[2];
    const double* m = grid.direction;
    const double px = m[0] * vx + m[1] * vy + m[2] * vz;
    const double py = m[3] * vx + m[4] * vy + m[5] * vz;
    const double pz = m[6] * vx + m[7] * vy + m[8] * vz;
    return std::sqrt(px * px + py * py + pz * pz);
}

struct BucketQueue
{
    std::vector<int> index;
    std::vector<float> distance;
    size_t head = 0;

    void push(int voxel, float dist)
    {
        index.push_back(voxel);
        distance.push_back(dist);
    }

    bool empty() const { return head >= index.size(); }
};

bool cancelled(std::atomic<bool>* cancel)
{
    return cancel && cancel->load(std::memory_order_relaxed);
}
}

vtkSmartPointer<vtkImageData> SplitMaskCore::split(
    vtkImageData* originMask,
    vtkImageData* intensities,
    const SplitRequest& request,
    std::atomic<bool>* cancel,
    void (*progress)(double fraction, void* user),
    void* progressUser)
{
    if (!originMask || !intensities) return nullptr;
    if (originMask->GetScalarType() != VTK_UNSIGNED_CHAR || originMask->GetNumberOfScalarComponents() != 1)
        return nullptr;
    const Grid maskGrid = gridOf(originMask);
    const Grid intensityGrid = gridOf(intensities);
    if (!sameGeometry(maskGrid, intensityGrid)) return nullptr;

    const int regionCount = std::clamp(request.regionCount, 1, 16);
    auto out = vtkSmartPointer<vtkImageData>::New();
    out->CopyStructure(originMask);
    out->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    auto* output = static_cast<unsigned char*>(out->GetScalarPointer());
    if (!output) return nullptr;
    std::fill(output, output + maskGrid.n, static_cast<unsigned char>(0));

    const auto* origin = static_cast<const unsigned char*>(originMask->GetScalarPointer());
    if (!origin) return nullptr;
    const unsigned char wanted = static_cast<unsigned char>(std::clamp(request.originLabel, 0, 255));

    int x0 = maskGrid.nx, y0 = maskGrid.ny, z0 = maskGrid.nz;
    int x1 = -1, y1 = -1, z1 = -1;
    std::int64_t maskCount = 0;
    for (int z = 0; z < maskGrid.nz; ++z)
        for (int y = 0; y < maskGrid.ny; ++y)
            for (int x = 0; x < maskGrid.nx; ++x) {
                const vtkIdType i = indexOf(x, y, z, maskGrid.nx, maskGrid.ny);
                if (origin[i] != wanted) continue;
                ++maskCount;
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                z0 = std::min(z0, z);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
                z1 = std::max(z1, z);
            }
    if (maskCount == 0) return out;

    x0 = std::max(0, x0 - 2);
    y0 = std::max(0, y0 - 2);
    z0 = std::max(0, z0 - 2);
    x1 = std::min(maskGrid.nx - 1, x1 + 2);
    y1 = std::min(maskGrid.ny - 1, y1 + 2);
    z1 = std::min(maskGrid.nz - 1, z1 + 2);
    const int cnx = x1 - x0 + 1;
    const int cny = y1 - y0 + 1;
    const int cnz = z1 - z0 + 1;
    const int cropN = cnx * cny * cnz;
    auto localOf = [&](int x, int y, int z) {
        return (x - x0) + cnx * ((y - y0) + cny * (z - z0));
    };
    auto insideCrop = [&](int x, int y, int z) {
        return x >= x0 && y >= y0 && z >= z0 && x <= x1 && y <= y1 && z <= z1;
    };

    std::vector<unsigned char> inMask(static_cast<size_t>(cropN), 0);
    std::vector<std::uint16_t> intensityBucket(static_cast<size_t>(cropN), 0);
    double iMin = std::numeric_limits<double>::infinity();
    double iMax = -std::numeric_limits<double>::infinity();
    for (int z = z0; z <= z1; ++z)
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const int local = localOf(x, y, z);
                if (origin[indexOf(x, y, z, maskGrid.nx, maskGrid.ny)] != wanted) continue;
                inMask[static_cast<size_t>(local)] = 1;
                const int gx = x + maskGrid.extent[0];
                const int gy = y + maskGrid.extent[2];
                const int gz = z + maskGrid.extent[4];
                const double hu = intensities->GetScalarComponentAsDouble(gx, gy, gz, 0);
                if (!std::isfinite(hu)) continue;
                iMin = std::min(iMin, hu);
                iMax = std::max(iMax, hu);
            }
    if (!std::isfinite(iMin)) {
        iMin = 0.0;
        iMax = 0.0;
    }
    const double span = std::max(0.0, iMax - iMin);
    for (int local = 0; local < cropN; ++local) {
        if (!inMask[static_cast<size_t>(local)]) continue;
        const int x = x0 + local % cnx;
        const int y = y0 + (local / cnx) % cny;
        const int z = z0 + local / (cnx * cny);
        const int gx = x + maskGrid.extent[0];
        const int gy = y + maskGrid.extent[2];
        const int gz = z + maskGrid.extent[4];
        const double hu = intensities->GetScalarComponentAsDouble(gx, gy, gz, 0);
        double unit = span > 0.0 && std::isfinite(hu) ? (iMax - hu) / span : 0.0;
        unit = std::clamp(unit, 0.0, 1.0);
        intensityBucket[static_cast<size_t>(local)] =
            static_cast<std::uint16_t>(std::lround(unit * (kIntensityBuckets - 1)));
    }

    std::vector<std::int8_t> region(static_cast<size_t>(cropN), -1);
    std::vector<float> best(static_cast<size_t>(cropN), std::numeric_limits<float>::infinity());
    std::vector<unsigned char> done(static_cast<size_t>(cropN), 0);
    std::vector<BucketQueue> buckets(static_cast<size_t>(kIntensityBuckets));

    double neighborLength[3][3][3] = {};
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                neighborLength[dz + 1][dy + 1][dx + 1] = stepLength(maskGrid, dx, dy, dz);

    int cursor = 0;
    auto push = [&](int local, float dist) {
        const int bucket = intensityBucket[static_cast<size_t>(local)];
        buckets[static_cast<size_t>(bucket)].push(local, dist);
        if (bucket < cursor) cursor = bucket;
    };

    const double minSpacing = std::min(maskGrid.spacing[0], std::min(maskGrid.spacing[1], maskGrid.spacing[2]));
    if (request.markers) {
        for (int m = 0; m < request.markerCount; ++m) {
            const SplitMarker& marker = request.markers[m];
            if (marker.region < 0 || marker.region >= regionCount || !(marker.radiusMm > 0.0)) continue;
            double ijk[3] = {};
            originMask->TransformPhysicalPointToContinuousIndex(marker.centerMm, ijk);
            const int cx = static_cast<int>(std::floor(ijk[0] + 0.5)) - maskGrid.extent[0];
            const int cy = static_cast<int>(std::floor(ijk[1] + 0.5)) - maskGrid.extent[2];
            const int cz = static_cast<int>(std::floor(ijk[2] + 0.5)) - maskGrid.extent[4];
            const int reach = std::max(1, static_cast<int>(std::ceil(marker.radiusMm / minSpacing)) + 1);
            for (int z = cz - reach; z <= cz + reach; ++z)
                for (int y = cy - reach; y <= cy + reach; ++y)
                    for (int x = cx - reach; x <= cx + reach; ++x) {
                        if (!insideCrop(x, y, z)) continue;
                        const int local = localOf(x, y, z);
                        if (!inMask[static_cast<size_t>(local)]) continue;
                        double point[3] = {};
                        const double continuous[3] = {
                            static_cast<double>(x + maskGrid.extent[0]),
                            static_cast<double>(y + maskGrid.extent[2]),
                            static_cast<double>(z + maskGrid.extent[4])};
                        originMask->TransformContinuousIndexToPhysicalPoint(continuous, point);
                        const double dx = point[0] - marker.centerMm[0];
                        const double dy = point[1] - marker.centerMm[1];
                        const double dz = point[2] - marker.centerMm[2];
                        if (dx * dx + dy * dy + dz * dz > marker.radiusMm * marker.radiusMm) continue;
                        if (!(0.0f < best[static_cast<size_t>(local)])) continue;
                        best[static_cast<size_t>(local)] = 0.0f;
                        region[static_cast<size_t>(local)] = static_cast<std::int8_t>(marker.region);
                        push(local, 0.0f);
                    }
        }
    }

    std::int64_t finished = 0;
    std::int64_t sinceReport = 0;
    auto report = [&]() {
        if (!progress || maskCount <= 0) return;
        progress(std::clamp(static_cast<double>(finished) / static_cast<double>(maskCount), 0.0, 1.0), progressUser);
    };

    while (true) {
        while (cursor < kIntensityBuckets && buckets[static_cast<size_t>(cursor)].empty())
            ++cursor;
        if (cursor >= kIntensityBuckets) break;
        if ((sinceReport & 4095) == 0 && cancelled(cancel)) return nullptr;
        auto& bucket = buckets[static_cast<size_t>(cursor)];
        const int local = bucket.index[bucket.head];
        const float queued = bucket.distance[bucket.head];
        ++bucket.head;
        ++sinceReport;
        if (done[static_cast<size_t>(local)]) continue;
        if (queued > best[static_cast<size_t>(local)] + 1.0e-4f) continue;
        done[static_cast<size_t>(local)] = 1;
        ++finished;
        if ((finished & 65535) == 0) report();

        const int x = x0 + local % cnx;
        const int y = y0 + (local / cnx) % cny;
        const int z = z0 + local / (cnx * cny);
        const std::int8_t owner = region[static_cast<size_t>(local)];
        const float from = best[static_cast<size_t>(local)];
        for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0 && dz == 0) continue;
                    const int xx = x + dx, yy = y + dy, zz = z + dz;
                    if (!insideCrop(xx, yy, zz)) continue;
                    const int next = localOf(xx, yy, zz);
                    if (!inMask[static_cast<size_t>(next)] || done[static_cast<size_t>(next)]) continue;
                    const float nd = from + static_cast<float>(neighborLength[dz + 1][dy + 1][dx + 1]);
                    if (!(nd + 1.0e-4f < best[static_cast<size_t>(next)])) continue;
                    best[static_cast<size_t>(next)] = nd;
                    region[static_cast<size_t>(next)] = owner;
                    push(next, nd);
                }
    }
    if (cancelled(cancel)) return nullptr;

    if (request.assignIslandsToNearest) {
        const double diagonal = std::sqrt(
            std::pow(cnx * maskGrid.spacing[0], 2.0)
            + std::pow(cny * maskGrid.spacing[1], 2.0)
            + std::pow(cnz * maskGrid.spacing[2], 2.0));
        double quantum = std::max(minSpacing * 0.25, diagonal / 8192.0);
        if (!(quantum > 0.0)) quantum = 1.0;
        const int distanceBuckets = std::max(2, static_cast<int>(std::ceil(diagonal / quantum)) + 2);
        std::vector<BucketQueue> byDistance(static_cast<size_t>(distanceBuckets));
        std::vector<float> nearDist(static_cast<size_t>(cropN), std::numeric_limits<float>::infinity());
        std::vector<unsigned char> claimed(static_cast<size_t>(cropN), 0);
        int distanceCursor = 0;
        auto pushDistance = [&](int local, float dist) {
            int bucket = static_cast<int>(dist / static_cast<float>(quantum));
            bucket = std::clamp(bucket, 0, distanceBuckets - 1);
            byDistance[static_cast<size_t>(bucket)].push(local, dist);
            if (bucket < distanceCursor) distanceCursor = bucket;
        };
        for (int local = 0; local < cropN; ++local) {
            if (!inMask[static_cast<size_t>(local)] || region[static_cast<size_t>(local)] < 0) continue;
            nearDist[static_cast<size_t>(local)] = 0.0f;
            pushDistance(local, 0.0f);
        }
        while (true) {
            while (distanceCursor < distanceBuckets && byDistance[static_cast<size_t>(distanceCursor)].empty())
                ++distanceCursor;
            if (distanceCursor >= distanceBuckets) break;
            if (cancelled(cancel)) return nullptr;
            auto& bucket = byDistance[static_cast<size_t>(distanceCursor)];
            const int local = bucket.index[bucket.head];
            const float queued = bucket.distance[bucket.head];
            ++bucket.head;
            if (claimed[static_cast<size_t>(local)]) continue;
            if (queued > nearDist[static_cast<size_t>(local)] + 1.0e-4f) continue;
            claimed[static_cast<size_t>(local)] = 1;
            const int x = x0 + local % cnx;
            const int y = y0 + (local / cnx) % cny;
            const int z = z0 + local / (cnx * cny);
            const std::int8_t owner = region[static_cast<size_t>(local)];
            for (int dz = -1; dz <= 1; ++dz)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0 && dz == 0) continue;
                        const int xx = x + dx, yy = y + dy, zz = z + dz;
                        if (!insideCrop(xx, yy, zz)) continue;
                        const int next = localOf(xx, yy, zz);
                        if (claimed[static_cast<size_t>(next)]) continue;
                        const float nd = nearDist[static_cast<size_t>(local)]
                            + static_cast<float>(neighborLength[dz + 1][dy + 1][dx + 1]);
                        if (!(nd + 1.0e-4f < nearDist[static_cast<size_t>(next)])) continue;
                        nearDist[static_cast<size_t>(next)] = nd;
                        region[static_cast<size_t>(next)] = owner;
                        pushDistance(next, nd);
                    }
        }
    }

    const unsigned char unassigned = static_cast<unsigned char>(regionCount + 1);
    for (int z = z0; z <= z1; ++z)
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const int local = localOf(x, y, z);
                if (!inMask[static_cast<size_t>(local)]) continue;
                const std::int8_t owner = region[static_cast<size_t>(local)];
                output[indexOf(x, y, z, maskGrid.nx, maskGrid.ny)] =
                    owner < 0 ? unassigned : static_cast<unsigned char>(owner + 1);
                if (!done[static_cast<size_t>(local)]) ++finished;
            }
    report();
    if (progress) progress(1.0, progressUser);
    return out;
}
