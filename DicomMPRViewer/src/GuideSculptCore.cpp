#include "GuideSculptCore.h"

#include <vtkFloatArray.h>
#include <vtkImageData.h>
#include <vtkMath.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>

namespace
{
using Vec3 = std::array<double, 3>;

constexpr int kBlock = 32;                       // undo granularity, in voxels per side
constexpr std::size_t kHistoryBytes = 300u << 20; // ~300 MB of saved blocks
constexpr std::size_t kHistoryStrokes = 30;

Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double norm(const Vec3& a) { return std::sqrt(dot(a, a)); }

// Distance from p to the segment ab; a stroke is a chain of segments, so a fast drag leaves no gaps.
double distanceToSegment(const Vec3& p, const Vec3& a, const Vec3& b)
{
    const Vec3 ab = sub(b, a);
    const double len2 = dot(ab, ab);
    if (len2 < 1e-12)
        return norm(sub(p, a));
    const double t = std::clamp(dot(sub(p, a), ab) / len2, 0.0, 1.0);
    return norm(Vec3{p[0] - (a[0] + t * ab[0]), p[1] - (a[1] + t * ab[1]), p[2] - (a[2] + t * ab[2])});
}

// One pass of a separable [1 2 1]/4 kernel over a sub-volume, edges clamped.
void blurOnce(std::vector<float>& buffer, const std::array<int, 3>& n)
{
    std::vector<float> temp(buffer.size());
    const auto index = [&](int i, int j, int k) {
        return (static_cast<std::size_t>(k) * n[1] + static_cast<std::size_t>(j)) * n[0] +
               static_cast<std::size_t>(i);
    };
    for (int axis = 0; axis < 3; ++axis) {
        const int limit = n[axis] - 1;
        for (int k = 0; k < n[2]; ++k)
            for (int j = 0; j < n[1]; ++j)
                for (int i = 0; i < n[0]; ++i) {
                    int lo[3] = {i, j, k};
                    int hi[3] = {i, j, k};
                    lo[axis] = std::max(0, lo[axis] - 1);
                    hi[axis] = std::min(limit, hi[axis] + 1);
                    temp[index(i, j, k)] = static_cast<float>(0.25 * buffer[index(lo[0], lo[1], lo[2])] +
                                                              0.5 * buffer[index(i, j, k)] +
                                                              0.25 * buffer[index(hi[0], hi[1], hi[2])]);
                }
        buffer.swap(temp);
    }
}
} // namespace

double SculptGrid::At(const std::array<double, 3>& p) const
{
    if (Empty())
        return 1.0e30;
    const double gx = std::clamp((p[0] - origin[0]) / spacingMm, 0.0, dims[0] - 1.000001);
    const double gy = std::clamp((p[1] - origin[1]) / spacingMm, 0.0, dims[1] - 1.000001);
    const double gz = std::clamp((p[2] - origin[2]) / spacingMm, 0.0, dims[2] - 1.000001);
    const int i = static_cast<int>(gx), j = static_cast<int>(gy), k = static_cast<int>(gz);
    const double tx = gx - i, ty = gy - j, tz = gz - k;
    const auto at = [&](int a, int b, int c) { return static_cast<double>(values[Index(a, b, c)]); };
    const double c00 = at(i, j, k) * (1 - tx) + at(i + 1, j, k) * tx;
    const double c10 = at(i, j + 1, k) * (1 - tx) + at(i + 1, j + 1, k) * tx;
    const double c01 = at(i, j, k + 1) * (1 - tx) + at(i + 1, j, k + 1) * tx;
    const double c11 = at(i, j + 1, k + 1) * (1 - tx) + at(i + 1, j + 1, k + 1) * tx;
    return (c00 * (1 - ty) + c10 * ty) * (1 - tz) + (c01 * (1 - ty) + c11 * ty) * tz;
}

namespace GuideSculptCore
{
double LevelScale(double level, double lowest)
{
    return lowest * std::pow(50.0, std::clamp(level, 0.0, 1.0));
}

double Falloff(double distanceMm, double reachMm)
{
    if (reachMm <= 0.0)
        return 0.0;
    const double t = std::clamp(distanceMm / reachMm, 0.0, 1.0);
    return 1.0 - (3.0 * t * t - 2.0 * t * t * t); // smoothstep, 1 at the centre and 0 at the rim
}
} // namespace GuideSculptCore

// ── The grid ──────────────────────────────────────────────────────────────────
bool SculptSession::Reset(vtkPolyData* guide, double spacingMm, double paddingMm, QString* error,
                          const std::atomic<bool>* cancel)
{
    m_grid = SculptGrid{};
    ClearHistory();
    if (!guide || guide->GetNumberOfPolys() == 0) {
        if (error)
            *error = QStringLiteral("No hay guía que editar.");
        return false;
    }
    const double spacing = std::clamp(spacingMm, 0.1, 1.0);
    // The padding is what the brushes can add outside the guide; 3 mm is a generous bead of wax.
    const auto baked = ImplicitCore::BakeMeshField(guide, spacing, std::max(3.0, paddingMm), cancel, error);
    if (!baked) {
        if (error && error->isEmpty())
            *error = QStringLiteral("No se pudo medir la guía.");
        return false;
    }
    m_grid.values.assign(baked->values.begin(), baked->values.end());
    m_grid.dims = baked->dims;
    m_grid.origin = baked->origin;
    m_grid.spacingMm = baked->spacingMm;
    for (int a = 0; a < 3; ++a)
        m_blockCounts[static_cast<std::size_t>(a)] = (m_grid.dims[static_cast<std::size_t>(a)] + kBlock - 1) / kBlock;
    return true;
}

std::array<int, 3> SculptSession::voxelOf(const Vec3& p) const
{
    std::array<int, 3> v{};
    for (int a = 0; a < 3; ++a)
        v[static_cast<std::size_t>(a)] =
            static_cast<int>(std::floor((p[static_cast<std::size_t>(a)] - m_grid.origin[static_cast<std::size_t>(a)]) /
                                        m_grid.spacingMm));
    return v;
}

bool SculptSession::boxAround(const Vec3& a, const Vec3& b, double radiusMm, int marginVoxels,
                              std::array<int, 3>& lo, std::array<int, 3>& hi) const
{
    if (m_grid.Empty())
        return false;
    const int margin = std::max(0, marginVoxels) + 1;
    const int reach = static_cast<int>(std::ceil(radiusMm / m_grid.spacingMm)) + margin;
    const auto va = voxelOf(a);
    const auto vb = voxelOf(b);
    for (int axis = 0; axis < 3; ++axis) {
        const auto s = static_cast<std::size_t>(axis);
        lo[s] = std::max(0, std::min(va[s], vb[s]) - reach);
        hi[s] = std::min(m_grid.dims[s] - 1, std::max(va[s], vb[s]) + reach);
        if (lo[s] > hi[s])
            return false;
    }
    return true;
}

// ── Undo in blocks ────────────────────────────────────────────────────────────
void SculptSession::saveBlocks(const std::array<int, 3>& lo, const std::array<int, 3>& hi)
{
    if (!m_strokeOpen)
        return;
    for (int a = 0; a < 3; ++a) {
        const auto s = static_cast<std::size_t>(a);
        m_strokeLo[s] = m_strokeHi[s] < m_strokeLo[s] ? lo[s] : std::min(m_strokeLo[s], lo[s]);
        m_strokeHi[s] = std::max(m_strokeHi[s], hi[s]);
    }
    for (int bz = lo[2] / kBlock; bz <= hi[2] / kBlock; ++bz)
        for (int by = lo[1] / kBlock; by <= hi[1] / kBlock; ++by)
            for (int bx = lo[0] / kBlock; bx <= hi[0] / kBlock; ++bx) {
                const int id = (bz * m_blockCounts[1] + by) * m_blockCounts[0] + bx;
                if (m_stroke.count(id))
                    continue;
                std::vector<float> copy;
                const int x1 = std::min(m_grid.dims[0], (bx + 1) * kBlock);
                const int y1 = std::min(m_grid.dims[1], (by + 1) * kBlock);
                const int z1 = std::min(m_grid.dims[2], (bz + 1) * kBlock);
                copy.reserve(static_cast<std::size_t>(x1 - bx * kBlock) * (y1 - by * kBlock) * (z1 - bz * kBlock));
                for (int k = bz * kBlock; k < z1; ++k)
                    for (int j = by * kBlock; j < y1; ++j)
                        for (int i = bx * kBlock; i < x1; ++i)
                            copy.push_back(m_grid.values[m_grid.Index(i, j, k)]);
                m_stroke.emplace(id, std::move(copy));
            }
}

void SculptSession::applyEdit(Edit& edit)
{
    for (auto& [id, saved] : edit.blocks) {
        const int bx = id % m_blockCounts[0];
        const int by = (id / m_blockCounts[0]) % m_blockCounts[1];
        const int bz = id / (m_blockCounts[0] * m_blockCounts[1]);
        const int x1 = std::min(m_grid.dims[0], (bx + 1) * kBlock);
        const int y1 = std::min(m_grid.dims[1], (by + 1) * kBlock);
        const int z1 = std::min(m_grid.dims[2], (bz + 1) * kBlock);
        std::size_t at = 0;
        for (int k = bz * kBlock; k < z1; ++k)
            for (int j = by * kBlock; j < y1; ++j)
                for (int i = bx * kBlock; i < x1; ++i, ++at)
                    std::swap(saved[at], m_grid.values[m_grid.Index(i, j, k)]);
    }
}

void SculptSession::BeginStroke()
{
    m_stroke.clear();
    m_strokeLo = {0, 0, 0};
    m_strokeHi = {-1, -1, -1};
    m_strokeOpen = true;
}

void SculptSession::EndStroke()
{
    if (!m_strokeOpen)
        return;
    m_strokeOpen = false;
    if (m_strokeHi[0] >= m_strokeLo[0])
        applyLimits(m_strokeLo, m_strokeHi); // the plan wins over every brush
    if (m_stroke.empty())
        return;
    Edit edit;
    edit.blocks.reserve(m_stroke.size());
    for (auto& [id, saved] : m_stroke) {
        edit.bytes += saved.size() * sizeof(float);
        edit.blocks.emplace_back(id, std::move(saved));
    }
    m_stroke.clear();
    m_historyBytes += edit.bytes;
    m_undo.push_back(std::move(edit));
    m_redo.clear();
    while (m_undo.size() > kHistoryStrokes || (m_historyBytes > kHistoryBytes && m_undo.size() > 1)) {
        m_historyBytes -= m_undo.front().bytes;
        m_undo.pop_front();
    }
}

bool SculptSession::Undo()
{
    if (m_undo.empty())
        return false;
    Edit edit = std::move(m_undo.back());
    m_undo.pop_back();
    m_historyBytes -= edit.bytes;
    applyEdit(edit); // the blocks now hold what the grid had, so redo replays them
    m_redo.push_back(std::move(edit));
    return true;
}

bool SculptSession::Redo()
{
    if (m_redo.empty())
        return false;
    Edit edit = std::move(m_redo.back());
    m_redo.pop_back();
    applyEdit(edit);
    m_historyBytes += edit.bytes;
    m_undo.push_back(std::move(edit));
    return true;
}

void SculptSession::ClearHistory()
{
    m_undo.clear();
    m_redo.clear();
    m_stroke.clear();
    m_strokeOpen = false;
    m_historyBytes = 0;
}

// ── The protected fields ──────────────────────────────────────────────────────
void SculptSession::applyLimits(const std::array<int, 3>& lo, const std::array<int, 3>& hi)
{
    if (m_grid.Empty() || (!m_limits.wrapField && !m_limits.keepOut))
        return;
    for (int k = lo[2]; k <= hi[2]; ++k)
        for (int j = lo[1]; j <= hi[1]; ++j)
            for (int i = lo[0]; i <= hi[0]; ++i) {
                const Vec3 p = m_grid.Point(i, j, k);
                double value = m_grid.values[m_grid.Index(i, j, k)];
                if (m_limits.wrapField)
                    value = std::max(value, m_limits.clearanceMm - m_limits.wrapField->At(p));
                if (m_limits.keepOut)
                    value = std::max(value, -m_limits.keepOut->At(p));
                m_grid.values[m_grid.Index(i, j, k)] = static_cast<float>(value);
            }
}

// ── The brushes ───────────────────────────────────────────────────────────────
void SculptSession::ApplyBrush(const SculptBrush& brush)
{
    if (m_grid.Empty() || !(brush.radiusMm > 0.0))
        return;
    switch (brush.tool) {
    case SculptTool::Smooth:
        // The level runs 50-fold, as Freeform's does; "Alrededor" blends half a radius past the sphere.
        smoothDab(brush, std::min(1.0, GuideSculptCore::LevelScale(brush.level, 0.02)), 2,
                  brush.scope == SmoothScope::Around ? 1.5 : 1.0);
        break;
    case SculptTool::HotWax:
        switch (brush.wax) {
        case HotWaxMode::Melt: // wax running: a wider kernel, reaching past the brush
            smoothDab(brush, std::min(1.0, GuideSculptCore::LevelScale(brush.level, 0.04)), 4, 1.5);
            break;
        case HotWaxMode::Smooth:
            smoothDab(brush, std::min(1.0, GuideSculptCore::LevelScale(brush.level, 0.02)), 2, 1.0);
            break;
        case HotWaxMode::Add:
            offsetDab(brush, -brush.level * 4.0 * m_grid.spacingMm, 1.0);
            break;
        case HotWaxMode::Remove:
            offsetDab(brush, brush.level * 4.0 * m_grid.spacingMm, 1.0);
            break;
        }
        break;
    case SculptTool::Add:
        ballDab(brush, true);
        break;
    case SculptTool::Remove:
        ballDab(brush, false);
        break;
    case SculptTool::Flatten:
        flattenDab(brush);
        break;
    }
}

void SculptSession::smoothDab(const SculptBrush& brush, double lambda, int passes, double reachFactor)
{
    const Vec3 a = brush.hasPrevious ? brush.previous : brush.center;
    const Vec3 b = brush.center;
    const double reach = brush.radiusMm * reachFactor;
    std::array<int, 3> lo{}, hi{};
    if (!boxAround(a, b, reach, passes + 1, lo, hi))
        return;
    saveBlocks(lo, hi);
    const std::array<int, 3> n{hi[0] - lo[0] + 1, hi[1] - lo[1] + 1, hi[2] - lo[2] + 1};
    std::vector<float> buffer(static_cast<std::size_t>(n[0]) * n[1] * n[2]);
    for (int k = 0; k < n[2]; ++k)
        for (int j = 0; j < n[1]; ++j)
            for (int i = 0; i < n[0]; ++i)
                buffer[(static_cast<std::size_t>(k) * n[1] + j) * n[0] + i] =
                    m_grid.values[m_grid.Index(lo[0] + i, lo[1] + j, lo[2] + k)];
    for (int pass = 0; pass < std::max(1, passes); ++pass)
        blurOnce(buffer, n);
    for (int k = 0; k < n[2]; ++k)
        for (int j = 0; j < n[1]; ++j)
            for (int i = 0; i < n[0]; ++i) {
                const Vec3 p = m_grid.Point(lo[0] + i, lo[1] + j, lo[2] + k);
                const double w = GuideSculptCore::Falloff(distanceToSegment(p, a, b), reach);
                if (w <= 0.0)
                    continue;
                const std::size_t at = m_grid.Index(lo[0] + i, lo[1] + j, lo[2] + k);
                const double before = m_grid.values[at];
                const double blurred = buffer[(static_cast<std::size_t>(k) * n[1] + j) * n[0] + i];
                m_grid.values[at] = static_cast<float>(before + w * lambda * (blurred - before));
            }
}

void SculptSession::offsetDab(const SculptBrush& brush, double delta, double reachFactor)
{
    const Vec3 a = brush.hasPrevious ? brush.previous : brush.center;
    const Vec3 b = brush.center;
    const double reach = brush.radiusMm * reachFactor;
    std::array<int, 3> lo{}, hi{};
    if (!boxAround(a, b, reach, 0, lo, hi))
        return;
    saveBlocks(lo, hi);
    for (int k = lo[2]; k <= hi[2]; ++k)
        for (int j = lo[1]; j <= hi[1]; ++j)
            for (int i = lo[0]; i <= hi[0]; ++i) {
                const Vec3 p = m_grid.Point(i, j, k);
                const double w = GuideSculptCore::Falloff(distanceToSegment(p, a, b), reach);
                if (w <= 0.0)
                    continue;
                m_grid.values[m_grid.Index(i, j, k)] += static_cast<float>(w * delta);
            }
}

void SculptSession::ballDab(const SculptBrush& brush, bool add)
{
    const Vec3 a = brush.hasPrevious ? brush.previous : brush.center;
    const Vec3 b = brush.center;
    std::array<int, 3> lo{}, hi{};
    if (!boxAround(a, b, brush.radiusMm, 0, lo, hi))
        return;
    saveBlocks(lo, hi);
    // A capsule from the previous dab to this one: the trail is continuous however fast the drag is.
    const auto ball = ImplicitCore::Capsule(a, b, brush.radiusMm);
    for (int k = lo[2]; k <= hi[2]; ++k)
        for (int j = lo[1]; j <= hi[1]; ++j)
            for (int i = lo[0]; i <= hi[0]; ++i) {
                const double d = ImplicitCore::Value(ball, m_grid.Point(i, j, k));
                const std::size_t at = m_grid.Index(i, j, k);
                m_grid.values[at] =
                    static_cast<float>(add ? std::min<double>(m_grid.values[at], d) : std::max<double>(m_grid.values[at], -d));
            }
}

void SculptSession::flattenDab(const SculptBrush& brush)
{
    const Vec3 a = brush.hasPrevious ? brush.previous : brush.center;
    const Vec3 b = brush.center;
    std::array<int, 3> lo{}, hi{};
    if (!boxAround(a, b, brush.radiusMm, 0, lo, hi))
        return;
    // The plane of the surface under the brush: principal axes of the voxels straddling the isosurface.
    Vec3 centroid{0.0, 0.0, 0.0};
    std::vector<Vec3> surface;
    for (int k = lo[2]; k <= hi[2]; ++k)
        for (int j = lo[1]; j <= hi[1]; ++j)
            for (int i = lo[0]; i <= hi[0]; ++i) {
                const Vec3 p = m_grid.Point(i, j, k);
                if (std::abs(m_grid.values[m_grid.Index(i, j, k)]) > m_grid.spacingMm)
                    continue;
                if (distanceToSegment(p, a, b) > brush.radiusMm)
                    continue;
                surface.push_back(p);
                for (int axis = 0; axis < 3; ++axis)
                    centroid[static_cast<std::size_t>(axis)] += p[static_cast<std::size_t>(axis)];
            }
    if (surface.size() < 8)
        return;
    for (int axis = 0; axis < 3; ++axis)
        centroid[static_cast<std::size_t>(axis)] /= static_cast<double>(surface.size());
    double covariance[3][3] = {};
    for (const Vec3& p : surface) {
        const Vec3 d = sub(p, centroid);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                covariance[r][c] += d[static_cast<std::size_t>(r)] * d[static_cast<std::size_t>(c)];
    }
    double* rows[3] = {covariance[0], covariance[1], covariance[2]};
    double eigenvalues[3] = {};
    double vectorStore[3][3] = {};
    double* vectors[3] = {vectorStore[0], vectorStore[1], vectorStore[2]};
    if (vtkMath::Jacobi(rows, eigenvalues, vectors) == 0)
        return;
    Vec3 normal{vectors[0][2], vectors[1][2], vectors[2][2]}; // vtkMath sorts them descending: the last is the normal
    const double length = norm(normal);
    if (length < 1e-9)
        return;
    for (int axis = 0; axis < 3; ++axis)
        normal[static_cast<std::size_t>(axis)] /= length;
    // Pointing out of the solid, like the field's own gradient.
    const double h = m_grid.spacingMm;
    const Vec3 gradient{m_grid.At({b[0] + h, b[1], b[2]}) - m_grid.At({b[0] - h, b[1], b[2]}),
                        m_grid.At({b[0], b[1] + h, b[2]}) - m_grid.At({b[0], b[1] - h, b[2]}),
                        m_grid.At({b[0], b[1], b[2] + h}) - m_grid.At({b[0], b[1], b[2] - h})};
    if (dot(normal, gradient) < 0.0)
        normal = {-normal[0], -normal[1], -normal[2]};
    saveBlocks(lo, hi);
    const double strength = std::clamp(brush.level, 0.0, 1.0);
    for (int k = lo[2]; k <= hi[2]; ++k)
        for (int j = lo[1]; j <= hi[1]; ++j)
            for (int i = lo[0]; i <= hi[0]; ++i) {
                const Vec3 p = m_grid.Point(i, j, k);
                const double w = GuideSculptCore::Falloff(distanceToSegment(p, a, b), brush.radiusMm);
                if (w <= 0.0)
                    continue;
                const std::size_t at = m_grid.Index(i, j, k);
                const double before = m_grid.values[at];
                const double planed = before + w * strength * (dot(sub(p, centroid), normal) - before);
                m_grid.values[at] = static_cast<float>(brush.flatten == FlattenMode::Scrape ? std::max(before, planed)
                                                       : brush.flatten == FlattenMode::Fill ? std::min(before, planed)
                                                                                            : planed);
            }
}

// ── Trim and islands ──────────────────────────────────────────────────────────
bool SculptSession::Trim(const std::vector<Vec3>& polygon, const Vec3& viewDirection, bool keepInside, QString* error)
{
    if (m_grid.Empty()) {
        if (error)
            *error = QStringLiteral("No hay guía que recortar.");
        return false;
    }
    if (polygon.size() < 3) {
        if (error)
            *error = QStringLiteral("Marque al menos tres puntos del recorte.");
        return false;
    }
    if (norm(viewDirection) < 1e-9) {
        if (error)
            *error = QStringLiteral("No se pudo tomar la dirección de la vista.");
        return false;
    }
    const auto prism = ImplicitCore::Prism(polygon, viewDirection, -1.0); // infinite along the view
    if (!prism) {
        if (error)
            *error = QStringLiteral("El contorno de recorte no es válido.");
        return false;
    }
    BeginStroke();
    const std::array<int, 3> lo{0, 0, 0};
    const std::array<int, 3> hi{m_grid.dims[0] - 1, m_grid.dims[1] - 1, m_grid.dims[2] - 1};
    saveBlocks(lo, hi);
    for (int k = 0; k < m_grid.dims[2]; ++k)
        for (int j = 0; j < m_grid.dims[1]; ++j)
            for (int i = 0; i < m_grid.dims[0]; ++i) {
                const double d = ImplicitCore::Value(prism, m_grid.Point(i, j, k));
                const std::size_t at = m_grid.Index(i, j, k);
                m_grid.values[at] = static_cast<float>(std::max<double>(m_grid.values[at], keepInside ? d : -d));
            }
    KeepLargestPiece();
    EndStroke();
    return true;
}

int SculptSession::KeepLargestPiece()
{
    if (m_grid.Empty())
        return 0;
    const std::size_t total = m_grid.values.size();
    std::vector<int> component(total, -1);
    std::vector<std::size_t> sizes;
    std::deque<std::size_t> queue;
    const int nx = m_grid.dims[0], ny = m_grid.dims[1], nz = m_grid.dims[2];
    for (int k0 = 0; k0 < nz; ++k0)
        for (int j0 = 0; j0 < ny; ++j0)
            for (int i0 = 0; i0 < nx; ++i0) {
                const std::size_t seed = m_grid.Index(i0, j0, k0);
                if (m_grid.values[seed] >= 0.0f || component[seed] >= 0)
                    continue;
                const int id = static_cast<int>(sizes.size());
                sizes.push_back(0);
                component[seed] = id;
                queue.push_back(seed);
                while (!queue.empty()) {
                    const std::size_t at = queue.front();
                    queue.pop_front();
                    ++sizes[static_cast<std::size_t>(id)];
                    const int i = static_cast<int>(at % static_cast<std::size_t>(nx));
                    const int j = static_cast<int>((at / static_cast<std::size_t>(nx)) % static_cast<std::size_t>(ny));
                    const int k = static_cast<int>(at / (static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny)));
                    const int steps[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
                    for (const auto& s : steps) {
                        const int ni = i + s[0], nj = j + s[1], nk = k + s[2];
                        if (ni < 0 || nj < 0 || nk < 0 || ni >= nx || nj >= ny || nk >= nz)
                            continue;
                        const std::size_t next = m_grid.Index(ni, nj, nk);
                        if (m_grid.values[next] >= 0.0f || component[next] >= 0)
                            continue;
                        component[next] = id;
                        queue.push_back(next);
                    }
                }
            }
    if (sizes.size() <= 1)
        return static_cast<int>(sizes.size());
    const int largest = static_cast<int>(std::distance(sizes.begin(), std::max_element(sizes.begin(), sizes.end())));
    const float outside = static_cast<float>(m_grid.spacingMm);
    for (std::size_t at = 0; at < total; ++at)
        if (component[at] >= 0 && component[at] != largest)
            m_grid.values[at] = outside; // the island's shell is already positive: no crossing is left
    return static_cast<int>(sizes.size());
}

// ── Contour ───────────────────────────────────────────────────────────────────
vtkSmartPointer<vtkPolyData> SculptSession::Contour(bool fast, const std::atomic<bool>* cancel) const
{
    if (m_grid.Empty())
        return nullptr;
    if (cancel && cancel->load())
        return nullptr;
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(m_grid.dims[0], m_grid.dims[1], m_grid.dims[2]);
    image->SetSpacing(m_grid.spacingMm, m_grid.spacingMm, m_grid.spacingMm);
    image->SetOrigin(m_grid.origin[0], m_grid.origin[1], m_grid.origin[2]);
    auto values = vtkSmartPointer<vtkFloatArray>::New();
    values->SetName("SculptField");
    values->SetNumberOfComponents(1);
    values->SetNumberOfTuples(static_cast<vtkIdType>(m_grid.values.size()));
    std::copy(m_grid.values.begin(), m_grid.values.end(), values->GetPointer(0));
    image->GetPointData()->SetScalars(values);

    ImplicitCore::PolygonizeOptions options;
    options.smoothingIterations = fast ? 0 : 10; // the preview during a drag is raw; the result is finished
    options.passBand = 0.1;
    options.repair = !fast;
    return ImplicitCore::Polygonize(image, options);
}
