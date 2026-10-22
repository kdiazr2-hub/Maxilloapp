#include "ThresholdMaskCore.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <vector>

#include <vtkImageData.h>

namespace
{
struct Grid
{
    int extent[6] = {};
    int nx = 0;
    int ny = 0;
    int nz = 0;
    int comps = 1;
    vtkIdType n = 0;
};

Grid gridOf(vtkImageData* image)
{
    Grid grid;
    if (!image) return grid;
    image->GetExtent(grid.extent);
    grid.nx = grid.extent[1] - grid.extent[0] + 1;
    grid.ny = grid.extent[3] - grid.extent[2] + 1;
    grid.nz = grid.extent[5] - grid.extent[4] + 1;
    grid.comps = std::max(1, image->GetNumberOfScalarComponents());
    if (grid.nx > 0 && grid.ny > 0 && grid.nz > 0)
        grid.n = static_cast<vtkIdType>(grid.nx) * grid.ny * grid.nz;
    return grid;
}

template <typename T, typename Fn>
void forEach(vtkImageData* image, const Grid& grid, Fn fn)
{
    auto* base = static_cast<T*>(image->GetScalarPointer(grid.extent[0], grid.extent[2], grid.extent[4]));
    if (!base) return;
    for (vtkIdType i = 0; i < grid.n; ++i)
        fn(i, static_cast<double>(base[i * grid.comps]));
}

template <typename Fn>
void forEachScalar(vtkImageData* image, const Grid& grid, Fn fn)
{
    switch (image->GetScalarType()) {
        case VTK_SHORT:          forEach<short>(image, grid, fn); break;
        case VTK_UNSIGNED_SHORT: forEach<unsigned short>(image, grid, fn); break;
        case VTK_FLOAT:          forEach<float>(image, grid, fn); break;
        case VTK_DOUBLE:         forEach<double>(image, grid, fn); break;
        case VTK_INT:            forEach<int>(image, grid, fn); break;
        case VTK_UNSIGNED_INT:   forEach<unsigned int>(image, grid, fn); break;
        case VTK_UNSIGNED_CHAR:  forEach<unsigned char>(image, grid, fn); break;
        case VTK_SIGNED_CHAR:    forEach<signed char>(image, grid, fn); break;
        default:
            for (int z = grid.extent[4]; z <= grid.extent[5]; ++z)
                for (int y = grid.extent[2]; y <= grid.extent[3]; ++y)
                    for (int x = grid.extent[0]; x <= grid.extent[1]; ++x) {
                        const vtkIdType i = (x - grid.extent[0])
                            + static_cast<vtkIdType>(grid.nx) * ((y - grid.extent[2])
                            + static_cast<vtkIdType>(grid.ny) * (z - grid.extent[4]));
                        fn(i, image->GetScalarComponentAsDouble(x, y, z, 0));
                    }
            break;
    }
}

bool inRange(double hu, double minHu, double maxHu)
{
    if (minHu > maxHu) std::swap(minHu, maxHu);
    return std::isfinite(hu) && hu >= minHu && hu <= maxHu;
}

vtkIdType indexOf(int x, int y, int z, int nx, int ny)
{
    return static_cast<vtkIdType>(x) + static_cast<vtkIdType>(nx) * (y + static_cast<vtkIdType>(ny) * z);
}

void keepLargest(unsigned char* mask, const Grid& grid)
{
    const vtkIdType n = grid.n;
    std::vector<unsigned char> seen(static_cast<size_t>(n), 0);
    vtkIdType bestCount = 0;
    vtkIdType bestSeed = -1;
    std::vector<vtkIdType> stack;
    const int nx = grid.nx, ny = grid.ny, nz = grid.nz;

    auto flood = [&](vtkIdType seed, unsigned char mark) -> vtkIdType {
        vtkIdType count = 0;
        stack.clear();
        stack.push_back(seed);
        seen[static_cast<size_t>(seed)] = mark;
        while (!stack.empty()) {
            const vtkIdType i = stack.back();
            stack.pop_back();
            ++count;
            const int z = static_cast<int>(i / (static_cast<vtkIdType>(nx) * ny));
            const vtkIdType rem = i - static_cast<vtkIdType>(z) * nx * ny;
            const int y = static_cast<int>(rem / nx);
            const int x = static_cast<int>(rem - static_cast<vtkIdType>(y) * nx);
            const int next[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
            for (const auto& d : next) {
                const int xx = x + d[0], yy = y + d[1], zz = z + d[2];
                if (xx < 0 || yy < 0 || zz < 0 || xx >= nx || yy >= ny || zz >= nz) continue;
                const vtkIdType j = indexOf(xx, yy, zz, nx, ny);
                if (!mask[j] || seen[static_cast<size_t>(j)]) continue;
                seen[static_cast<size_t>(j)] = mark;
                stack.push_back(j);
            }
        }
        return count;
    };

    for (vtkIdType i = 0; i < n; ++i) {
        if (!mask[i] || seen[static_cast<size_t>(i)]) continue;
        const vtkIdType count = flood(i, 1);
        if (count > bestCount) {
            bestCount = count;
            bestSeed = i;
        }
    }
    if (bestSeed < 0) return;

    std::fill(seen.begin(), seen.end(), 0);
    flood(bestSeed, 2);
    for (vtkIdType i = 0; i < n; ++i)
        if (seen[static_cast<size_t>(i)] != 2)
            mask[i] = 0;
}

void fillHoles(unsigned char* mask, const Grid& grid, int label)
{
    const vtkIdType n = grid.n;
    const int nx = grid.nx, ny = grid.ny, nz = grid.nz;
    std::vector<unsigned char> outside(static_cast<size_t>(n), 0);
    std::vector<vtkIdType> stack;

    auto consider = [&](int x, int y, int z) {
        const vtkIdType i = indexOf(x, y, z, nx, ny);
        if (mask[i] || outside[static_cast<size_t>(i)]) return;
        outside[static_cast<size_t>(i)] = 1;
        stack.push_back(i);
    };

    for (int z = 0; z < nz; ++z)
        for (int y = 0; y < ny; ++y) {
            consider(0, y, z);
            if (nx > 1) consider(nx - 1, y, z);
        }
    for (int z = 0; z < nz; ++z)
        for (int x = 0; x < nx; ++x) {
            consider(x, 0, z);
            if (ny > 1) consider(x, ny - 1, z);
        }
    for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x) {
            consider(x, y, 0);
            if (nz > 1) consider(x, y, nz - 1);
        }

    while (!stack.empty()) {
        const vtkIdType i = stack.back();
        stack.pop_back();
        const int z = static_cast<int>(i / (static_cast<vtkIdType>(nx) * ny));
        const vtkIdType rem = i - static_cast<vtkIdType>(z) * nx * ny;
        const int y = static_cast<int>(rem / nx);
        const int x = static_cast<int>(rem - static_cast<vtkIdType>(y) * nx);
        const int next[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
        for (const auto& d : next) {
            const int xx = x + d[0], yy = y + d[1], zz = z + d[2];
            if (xx < 0 || yy < 0 || zz < 0 || xx >= nx || yy >= ny || zz >= nz) continue;
            consider(xx, yy, zz);
        }
    }

    const auto value = static_cast<unsigned char>(std::clamp(label, 1, 255));
    for (vtkIdType i = 0; i < n; ++i)
        if (!mask[i] && !outside[static_cast<size_t>(i)])
            mask[i] = value;
}
}

HuHistogram ThresholdMaskCore::histogram(vtkImageData* volume, int bins)
{
    HuHistogram result;
    if (!volume || bins < 1) return result;
    const Grid grid = gridOf(volume);
    if (grid.n <= 0) return result;

    double minimum = std::numeric_limits<double>::infinity();
    double maximum = -std::numeric_limits<double>::infinity();
    forEachScalar(volume, grid, [&](vtkIdType, double hu) {
        if (!std::isfinite(hu)) return;
        minimum = std::min(minimum, hu);
        maximum = std::max(maximum, hu);
        ++result.voxels;
    });
    if (!std::isfinite(minimum)) return result;

    result.minimum = minimum;
    result.maximum = maximum;
    bins = std::max(1, bins);
    result.counts.assign(static_cast<size_t>(bins), 0);
    result.binWidth = bins == 1 ? 1.0 : (maximum - minimum) / static_cast<double>(bins);
    if (result.binWidth <= 0.0) result.binWidth = 1.0;

    forEachScalar(volume, grid, [&](vtkIdType, double hu) {
        if (!std::isfinite(hu)) return;
        int bin = static_cast<int>((hu - minimum) / result.binWidth);
        if (bin >= bins) bin = bins - 1;
        if (bin < 0) bin = 0;
        ++result.counts[static_cast<size_t>(bin)];
    });
    return result;
}

vtkSmartPointer<vtkImageData> ThresholdMaskCore::blank(vtkImageData* volume)
{
    if (!volume) return nullptr;
    auto out = vtkSmartPointer<vtkImageData>::New();
    out->CopyStructure(volume);
    out->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    if (auto* dst = static_cast<unsigned char*>(out->GetScalarPointer()))
        std::fill(dst, dst + out->GetNumberOfPoints(), static_cast<unsigned char>(0));
    return out;
}

vtkSmartPointer<vtkImageData> ThresholdMaskCore::unsignedLabelmap(vtkImageData* source)
{
    if (!source || source->GetNumberOfPoints() == 0) return nullptr;
    if (source->GetScalarType() == VTK_UNSIGNED_CHAR && source->GetNumberOfScalarComponents() == 1)
        return source;

    auto out = blank(source);
    const Grid grid = gridOf(source);
    auto* dst = static_cast<unsigned char*>(out->GetScalarPointer());
    forEachScalar(source, grid, [&](vtkIdType i, double value) {
        if (!std::isfinite(value)) return;
        const int rounded = static_cast<int>(std::lround(value));
        dst[i] = static_cast<unsigned char>(std::clamp(rounded, 0, 255));
    });
    return out;
}

void ThresholdMaskCore::paintSlice(vtkImageData* volume, vtkImageData* labelmap,
                                   int axis, int slice, double minHu, double maxHu, int label)
{
    if (!volume || !labelmap || axis < 0 || axis > 2) return;
    const Grid volumeGrid = gridOf(volume);
    const Grid labelGrid = gridOf(labelmap);
    if (volumeGrid.n <= 0 || volumeGrid.n != labelGrid.n) return;
    if (slice < volumeGrid.extent[axis * 2] || slice > volumeGrid.extent[axis * 2 + 1]) return;

    const int value = std::clamp(label, 0, 255);
    auto* dst = static_cast<unsigned char*>(labelmap->GetScalarPointer());
    if (!dst) return;

    const int nx = volumeGrid.nx, ny = volumeGrid.ny;
    const int x0 = volumeGrid.extent[0], y0 = volumeGrid.extent[2], z0 = volumeGrid.extent[4];
    auto paintAt = [&](int x, int y, int z, double hu) {
        const vtkIdType i = indexOf(x - x0, y - y0, z - z0, nx, ny);
        dst[i] = inRange(hu, minHu, maxHu) ? static_cast<unsigned char>(value) : 0;
    };

    if (volume->GetScalarType() == VTK_SHORT && volumeGrid.comps == 1) {
        auto* src = static_cast<short*>(volume->GetScalarPointer(x0, y0, z0));
        auto visit = [&](int x, int y, int z) {
            paintAt(x, y, z, static_cast<double>(src[indexOf(x - x0, y - y0, z - z0, nx, ny)]));
        };
        if (axis == 2) {
            for (int y = volumeGrid.extent[2]; y <= volumeGrid.extent[3]; ++y)
                for (int x = volumeGrid.extent[0]; x <= volumeGrid.extent[1]; ++x)
                    visit(x, y, slice);
        } else if (axis == 1) {
            for (int z = volumeGrid.extent[4]; z <= volumeGrid.extent[5]; ++z)
                for (int x = volumeGrid.extent[0]; x <= volumeGrid.extent[1]; ++x)
                    visit(x, slice, z);
        } else {
            for (int z = volumeGrid.extent[4]; z <= volumeGrid.extent[5]; ++z)
                for (int y = volumeGrid.extent[2]; y <= volumeGrid.extent[3]; ++y)
                    visit(slice, y, z);
        }
    } else {
        auto visit = [&](int x, int y, int z) {
            paintAt(x, y, z, volume->GetScalarComponentAsDouble(x, y, z, 0));
        };
        if (axis == 2) {
            for (int y = volumeGrid.extent[2]; y <= volumeGrid.extent[3]; ++y)
                for (int x = volumeGrid.extent[0]; x <= volumeGrid.extent[1]; ++x)
                    visit(x, y, slice);
        } else if (axis == 1) {
            for (int z = volumeGrid.extent[4]; z <= volumeGrid.extent[5]; ++z)
                for (int x = volumeGrid.extent[0]; x <= volumeGrid.extent[1]; ++x)
                    visit(x, slice, z);
        } else {
            for (int z = volumeGrid.extent[4]; z <= volumeGrid.extent[5]; ++z)
                for (int y = volumeGrid.extent[2]; y <= volumeGrid.extent[3]; ++y)
                    visit(slice, y, z);
        }
    }
    labelmap->Modified();
}

vtkSmartPointer<vtkImageData> ThresholdMaskCore::build(vtkImageData* volume, const ThresholdRequest& request,
                                                      std::int64_t* voxels)
{
    if (voxels) *voxels = 0;
    auto out = blank(volume);
    if (!out) return nullptr;
    const Grid grid = gridOf(volume);
    if (grid.n <= 0) return nullptr;

    auto* dst = static_cast<unsigned char*>(out->GetScalarPointer());
    const auto value = static_cast<unsigned char>(std::clamp(request.label, 1, 255));
    std::int64_t count = 0;
    forEachScalar(volume, grid, [&](vtkIdType i, double hu) {
        if (!inRange(hu, request.minHu, request.maxHu)) return;
        dst[i] = value;
        ++count;
    });
    if (count == 0) return nullptr;

    if (request.keepLargest)
        keepLargest(dst, grid);
    if (request.fillHoles)
        fillHoles(dst, grid, request.label);

    count = 0;
    for (vtkIdType i = 0; i < grid.n; ++i)
        if (dst[i]) ++count;
    if (voxels) *voxels = count;
    if (count == 0) return nullptr;
    return out;
}

std::int64_t ThresholdMaskCore::stamp(vtkImageData* destination, vtkImageData* mask, int label,
                                      const int* clearLabels, int clearCount)
{
    if (!destination || !mask) return -1;
    const Grid dstGrid = gridOf(destination);
    const Grid maskGrid = gridOf(mask);
    if (dstGrid.n <= 0 || dstGrid.n != maskGrid.n) return -1;
    if (destination->GetScalarType() != VTK_UNSIGNED_CHAR || mask->GetScalarType() != VTK_UNSIGNED_CHAR)
        return -1;
    if (destination->GetNumberOfScalarComponents() != 1 || mask->GetNumberOfScalarComponents() != 1)
        return -1;

    auto* dst = static_cast<unsigned char*>(destination->GetScalarPointer());
    auto* src = static_cast<unsigned char*>(mask->GetScalarPointer());
    const auto value = static_cast<unsigned char>(std::clamp(label, 1, 255));
    std::int64_t written = 0;
    for (vtkIdType i = 0; i < dstGrid.n; ++i) {
        int current = dst[i];
        for (int c = 0; c < clearCount; ++c) {
            if (current == clearLabels[c]) {
                current = 0;
                break;
            }
        }
        if (src[i] && current == 0) {
            dst[i] = value;
            ++written;
        } else {
            dst[i] = static_cast<unsigned char>(current);
        }
    }
    destination->Modified();
    return written;
}

std::int64_t ThresholdMaskCore::stampMask(vtkImageData* destination, vtkImageData* mask,
                                          const int* clearLabels, int clearCount)
{
    if (!destination || !mask) return -1;
    const Grid dstGrid = gridOf(destination);
    const Grid maskGrid = gridOf(mask);
    if (dstGrid.n <= 0 || dstGrid.n != maskGrid.n) return -1;
    if (destination->GetScalarType() != VTK_UNSIGNED_CHAR || mask->GetScalarType() != VTK_UNSIGNED_CHAR)
        return -1;
    if (destination->GetNumberOfScalarComponents() != 1 || mask->GetNumberOfScalarComponents() != 1)
        return -1;

    auto* dst = static_cast<unsigned char*>(destination->GetScalarPointer());
    auto* src = static_cast<unsigned char*>(mask->GetScalarPointer());
    std::int64_t written = 0;
    for (vtkIdType i = 0; i < dstGrid.n; ++i) {
        int current = dst[i];
        for (int c = 0; c < clearCount; ++c) {
            if (current == clearLabels[c]) {
                current = 0;
                break;
            }
        }
        if (src[i] && current == 0) {
            dst[i] = src[i];
            ++written;
        } else {
            dst[i] = static_cast<unsigned char>(current);
        }
    }
    destination->Modified();
    return written;
}

namespace
{
struct Offset { int x, y, z; };

std::vector<Offset> ballOffsets(const double spacing[3], double radiusMm)
{
    std::vector<Offset> ball;
    const int rx = std::max(1, static_cast<int>(std::ceil(radiusMm / spacing[0])));
    const int ry = std::max(1, static_cast<int>(std::ceil(radiusMm / spacing[1])));
    const int rz = std::max(1, static_cast<int>(std::ceil(radiusMm / spacing[2])));
    for (int dz = -rz; dz <= rz; ++dz)
        for (int dy = -ry; dy <= ry; ++dy)
            for (int dx = -rx; dx <= rx; ++dx) {
                if (dx == 0 && dy == 0 && dz == 0) continue;
                const double reach = dx * spacing[0] * dx * spacing[0]
                    + dy * spacing[1] * dy * spacing[1]
                    + dz * spacing[2] * dz * spacing[2];
                if (reach <= radiusMm * radiusMm)
                    ball.push_back({dx, dy, dz});
            }
    return ball;
}
}

bool ThresholdMaskCore::separateMandible(vtkImageData* mask)
{
    // Teeth and condyles glue the jaws, so a height valley never opens.
    // Eroding 2.2 mm drops those contacts on a head CT and leaves the mandible
    // as the wide piece in front of and below the vault. The cervical column
    // is the posterior piece under the skull; it is not grown back. The
    // mandible may not climb past its own top into the anterior maxilla, or
    // the upper teeth pull the whole midface down with it.
    if (!mask || mask->GetScalarType() != VTK_UNSIGNED_CHAR || mask->GetNumberOfScalarComponents() != 1)
        return false;
    const Grid grid = gridOf(mask);
    if (grid.n <= 0) return false;
    auto* voxels = static_cast<unsigned char*>(mask->GetScalarPointer());
    if (!voxels) return false;

    double spacing[3] = {1.0, 1.0, 1.0};
    mask->GetSpacing(spacing);
    for (double& step : spacing)
        if (!(step > 1e-6)) step = 1.0;

    const std::vector<Offset> ball = ballOffsets(spacing, 2.2);
    if (ball.empty()) return false;

    const int nx = grid.nx, ny = grid.ny, nz = grid.nz;
    const vtkIdType n = grid.n;
    std::vector<unsigned char> eroded(static_cast<size_t>(n), 0);
    for (int z = 0; z < nz; ++z)
        for (int y = 0; y < ny; ++y)
            for (int x = 0; x < nx; ++x) {
                const vtkIdType i = indexOf(x, y, z, nx, ny);
                if (!voxels[i]) continue;
                bool inside = true;
                for (const auto& offset : ball) {
                    const int xx = x + offset.x, yy = y + offset.y, zz = z + offset.z;
                    if (xx < 0 || yy < 0 || zz < 0 || xx >= nx || yy >= ny || zz >= nz
                        || !voxels[indexOf(xx, yy, zz, nx, ny)]) {
                        inside = false;
                        break;
                    }
                }
                if (inside) eroded[static_cast<size_t>(i)] = 1;
            }

    struct Piece {
        vtkIdType count = 0;
        double sumX = 0;
        double sumY = 0;
        double sumZ = 0;
        int minX = 0;
        int maxX = -1;
        int minY = 0;
        int maxY = -1;
        int minZ = 0;
        int maxZ = -1;
    };
    std::vector<int> pieceOf(static_cast<size_t>(n), 0);
    std::vector<Piece> pieces(1);
    std::vector<vtkIdType> stack;
    const int next[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    for (vtkIdType start = 0; start < n; ++start) {
        if (!eroded[static_cast<size_t>(start)] || pieceOf[static_cast<size_t>(start)]) continue;
        const int id = static_cast<int>(pieces.size());
        pieces.push_back(Piece{});
        auto& piece = pieces.back();
        piece.minX = nx;
        piece.minY = ny;
        piece.minZ = nz;
        stack.clear();
        stack.push_back(start);
        pieceOf[static_cast<size_t>(start)] = id;
        while (!stack.empty()) {
            const vtkIdType i = stack.back();
            stack.pop_back();
            const int z = static_cast<int>(i / (static_cast<vtkIdType>(nx) * ny));
            const vtkIdType rem = i - static_cast<vtkIdType>(z) * nx * ny;
            const int y = static_cast<int>(rem / nx);
            const int x = static_cast<int>(rem - static_cast<vtkIdType>(y) * nx);
            ++piece.count;
            piece.sumX += x * spacing[0];
            piece.sumY += y * spacing[1];
            piece.sumZ += z * spacing[2];
            piece.minX = std::min(piece.minX, x);
            piece.maxX = std::max(piece.maxX, x);
            piece.minY = std::min(piece.minY, y);
            piece.maxY = std::max(piece.maxY, y);
            piece.minZ = std::min(piece.minZ, z);
            piece.maxZ = std::max(piece.maxZ, z);
            for (const auto& d : next) {
                const int xx = x + d[0], yy = y + d[1], zz = z + d[2];
                if (xx < 0 || yy < 0 || zz < 0 || xx >= nx || yy >= ny || zz >= nz) continue;
                const vtkIdType j = indexOf(xx, yy, zz, nx, ny);
                if (!eroded[static_cast<size_t>(j)] || pieceOf[static_cast<size_t>(j)]) continue;
                pieceOf[static_cast<size_t>(j)] = id;
                stack.push_back(j);
            }
        }
    }
    if (pieces.size() < 2) return false;

    int cranium = 1;
    for (int id = 2; id < static_cast<int>(pieces.size()); ++id)
        if (pieces[static_cast<size_t>(id)].maxZ > pieces[static_cast<size_t>(cranium)].maxZ)
            cranium = id;
    const auto& skull = pieces[static_cast<size_t>(cranium)];
    if (skull.count <= 0) return false;
    const double skullY = skull.sumY / static_cast<double>(skull.count);
    const double skullZ = skull.sumZ / static_cast<double>(skull.count);
    const double skullMinZ = skull.minZ * spacing[2];

    constexpr double kApartMm = 15.0;
    constexpr double kMinWidthMm = 25.0;
    int mandible = -1;
    for (int id = 1; id < static_cast<int>(pieces.size()); ++id) {
        if (id == cranium) continue;
        const auto& piece = pieces[static_cast<size_t>(id)];
        if (piece.count < 500) continue;
        const double width = (piece.maxX - piece.minX) * spacing[0];
        if (width < kMinWidthMm) continue;
        const double meanY = piece.sumY / static_cast<double>(piece.count);
        const double meanZ = piece.sumZ / static_cast<double>(piece.count);
        const bool belowTheVault = piece.maxZ * spacing[2] + 10.0 < skull.maxZ * spacing[2];
        if (meanY > skullY - kApartMm || meanZ > skullZ - kApartMm || !belowTheVault) continue;
        if (mandible < 0 || piece.count > pieces[static_cast<size_t>(mandible)].count)
            mandible = id;
    }
    if (mandible < 0) return false;

    const auto& jaw = pieces[static_cast<size_t>(mandible)];
    const double jawMeanY = jaw.sumY / static_cast<double>(jaw.count);
    const double jawTop = jaw.maxZ * spacing[2] + 3.0;
    std::vector<int> spine;
    for (int id = 1; id < static_cast<int>(pieces.size()); ++id) {
        if (id == cranium || id == mandible) continue;
        const auto& piece = pieces[static_cast<size_t>(id)];
        if (piece.count < 200) continue;
        const double meanY = piece.sumY / static_cast<double>(piece.count);
        const bool posterior = meanY > jawMeanY + 20.0;
        const bool underTheSkull = piece.maxZ * spacing[2] < skullMinZ - 3.0;
        if (posterior && underTheSkull)
            spine.push_back(id);
    }

    std::vector<unsigned char> assigned(static_cast<size_t>(n), 0);
    std::deque<vtkIdType> queue;
    auto seedPiece = [&](int id, unsigned char label) {
        for (vtkIdType i = 0; i < n; ++i) {
            if (pieceOf[static_cast<size_t>(i)] != id) continue;
            assigned[static_cast<size_t>(i)] = label;
            queue.push_back(i);
        }
    };
    seedPiece(cranium, 5);
    seedPiece(mandible, 6);
    for (int id : spine)
        seedPiece(id, 7);
    while (!queue.empty()) {
        const vtkIdType i = queue.front();
        queue.pop_front();
        const int z = static_cast<int>(i / (static_cast<vtkIdType>(nx) * ny));
        const vtkIdType rem = i - static_cast<vtkIdType>(z) * nx * ny;
        const int y = static_cast<int>(rem / nx);
        const int x = static_cast<int>(rem - static_cast<vtkIdType>(y) * nx);
        const unsigned char label = assigned[static_cast<size_t>(i)];
        for (const auto& d : next) {
            const int xx = x + d[0], yy = y + d[1], zz = z + d[2];
            if (xx < 0 || yy < 0 || zz < 0 || xx >= nx || yy >= ny || zz >= nz) continue;
            const vtkIdType j = indexOf(xx, yy, zz, nx, ny);
            if (!voxels[j] || assigned[static_cast<size_t>(j)]) continue;
            // Past the top of the lower teeth, the anterior bone is the maxilla.
            if (label == 6 && y * spacing[1] < jawMeanY && z * spacing[2] > jawTop) continue;
            assigned[static_cast<size_t>(j)] = label;
            queue.push_back(j);
        }
    }

    bool wroteMandible = false;
    for (vtkIdType i = 0; i < n; ++i) {
        const unsigned char label = assigned[static_cast<size_t>(i)];
        if (label == 7) voxels[i] = 0;
        else if (label == 5 || label == 6) voxels[i] = label;
        wroteMandible = wroteMandible || voxels[i] == 6;
    }
    if (wroteMandible) mask->Modified();
    return wroteMandible;
}

void ThresholdMaskCore::sealThinBone(vtkImageData* mask, vtkImageData* volume,
                                     double radiusMm, double minHu)
{
    if (!mask || !volume || radiusMm <= 0.0) return;
    if (mask->GetScalarType() != VTK_UNSIGNED_CHAR || mask->GetNumberOfScalarComponents() != 1) return;
    const Grid grid = gridOf(mask);
    const Grid volumeGrid = gridOf(volume);
    if (grid.n <= 0 || grid.n != volumeGrid.n) return;
    auto* voxels = static_cast<unsigned char*>(mask->GetScalarPointer());
    if (!voxels) return;

    double spacing[3] = {1.0, 1.0, 1.0};
    mask->GetSpacing(spacing);
    for (double& step : spacing)
        if (!(step > 1e-6)) step = 1.0;

    const int nx = grid.nx, ny = grid.ny, nz = grid.nz;
    auto huAt = [&](int x, int y, int z) -> double {
        return volume->GetScalarComponentAsDouble(grid.extent[0] + x, grid.extent[2] + y, grid.extent[4] + z, 0);
    };
    // A ball does not fit inside a one-voxel wall, so a hole there would survive it.
    // A short chord does: bone of one label on both sides, nothing but bone-density
    // voxels between them. Two millimetres cannot cross the nose or an orbit.
    const double maxSpan = std::max(radiusMm * 2.0, radiusMm);
    const int axes[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    std::vector<unsigned char> fill(static_cast<size_t>(grid.n), 0);
    for (const auto& axis : axes) {
        const int dx = axis[0], dy = axis[1], dz = axis[2];
        const double step = std::abs(dx * spacing[0] + dy * spacing[1] + dz * spacing[2]);
        const int limit = std::max(1, static_cast<int>(std::floor(maxSpan / step)));
        for (int z = 0; z < nz; ++z)
            for (int y = 0; y < ny; ++y)
                for (int x = 0; x < nx; ++x) {
                    const unsigned char label = voxels[indexOf(x, y, z, nx, ny)];
                    if (!label) continue;
                    for (int dist = 2; dist <= limit; ++dist) {
                        const int x2 = x + dx * dist, y2 = y + dy * dist, z2 = z + dz * dist;
                        if (x2 < 0 || y2 < 0 || z2 < 0 || x2 >= nx || y2 >= ny || z2 >= nz) break;
                        const unsigned char other = voxels[indexOf(x2, y2, z2, nx, ny)];
                        if (other == 0) continue;
                        if (other != label) break;
                        bool open = true;
                        for (int s = 1; s < dist; ++s) {
                            const int xs = x + dx * s, ys = y + dy * s, zs = z + dz * s;
                            if (voxels[indexOf(xs, ys, zs, nx, ny)] != 0) { open = false; break; }
                            const double hu = huAt(xs, ys, zs);
                            if (!std::isfinite(hu) || hu < minHu) { open = false; break; }
                        }
                        if (!open) break;
                        for (int s = 1; s < dist; ++s) {
                            const int xs = x + dx * s, ys = y + dy * s, zs = z + dz * s;
                            fill[static_cast<size_t>(indexOf(xs, ys, zs, nx, ny))] = label;
                        }
                        break;
                    }
                }
    }
    bool changed = false;
    for (vtkIdType i = 0; i < grid.n; ++i) {
        if (!fill[static_cast<size_t>(i)] || voxels[i]) continue;
        voxels[i] = fill[static_cast<size_t>(i)];
        changed = true;
    }
    if (changed) mask->Modified();
}
