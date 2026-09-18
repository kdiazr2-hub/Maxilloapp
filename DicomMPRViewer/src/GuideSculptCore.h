#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// GuideSculptCore
//
// Freeform's clay for our guides. Freeform sculpts voxels; `ImplicitCore` is
// already a signed distance grid (negative inside), so the tools here edit that
// grid in place and re-contour it — never a chain of mesh booleans, and never a
// mesh smoothed again and again (every stroke starts from the grid, so nothing
// accumulates but the edit itself).
//
//   Suavizar  φ += w·λ·(G∗φ − φ)          Cera caliente  melt / smooth / ± δ·w
//   Añadir    φ  = min(φ,  capsule)       Quitar         φ = max(φ, −capsule)
//   Aplanar   φ += w·s·(d − φ)            Recortar       φ = max(φ, −prism)
//
// Two fields are protected and re-applied after every stroke, so no brush can
// undo the plan:
//   · the anatomy   φ = max(φ, clearance − wrapDistance)
//   · the keep-out  φ = max(φ, −keepOut)   (saw slots, holes, subtracted figures;
//     `GuideDesignCore::KeepOutNode` builds it from the very same nodes the guide
//     was carved with, baked once when the edit session opens)
//
// Undo is per stroke, in 32³ blocks: the blocks a stroke touches are copied
// before it changes them, and undo/redo swap them back, so a stroke costs only
// what it actually reached.
//
// No Qt Widgets.
// ─────────────────────────────────────────────────────────────────────────────

#include "ImplicitCore.h"

#include <QString>
#include <vtkSmartPointer.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <vector>

class vtkPolyData;

// The clay: a mutable signed distance grid, negative inside, same layout as `ImplicitCore::BakedField`.
struct SculptGrid
{
    std::vector<float> values;
    std::array<int, 3> dims{0, 0, 0};
    std::array<double, 3> origin{0.0, 0.0, 0.0};
    double spacingMm = 0.25;

    bool Empty() const { return values.empty() || dims[0] < 2 || dims[1] < 2 || dims[2] < 2; }
    std::size_t Index(int i, int j, int k) const
    {
        return (static_cast<std::size_t>(k) * static_cast<std::size_t>(dims[1]) + static_cast<std::size_t>(j)) *
                   static_cast<std::size_t>(dims[0]) +
               static_cast<std::size_t>(i);
    }
    std::array<double, 3> Point(int i, int j, int k) const
    {
        return {origin[0] + i * spacingMm, origin[1] + j * spacingMm, origin[2] + k * spacingMm};
    }
    // Trilinear, clamped at the grid edge.
    double At(const std::array<double, 3>& p) const;
};

// What the brushes may never fill in again.
struct SculptLimits
{
    std::shared_ptr<const ImplicitCore::BakedField> wrapField; // the anatomy the guide sits on
    double clearanceMm = 0.0;
    std::shared_ptr<const ImplicitCore::BakedField> keepOut;   // slots, holes and subtracted figures
};

enum class SculptTool
{
    Smooth,
    HotWax,
    Add,    // a ball of material added along the stroke
    Remove, // the same ball taken away
    Flatten
};

enum class HotWaxMode
{
    Melt,   // a wider smoothing, reaching past the brush
    Smooth,
    Add,
    Remove
};

enum class SmoothScope
{
    Inside, // only within the sphere
    Around  // and 50 % past it, so the edit blends into the surface
};

enum class FlattenMode
{
    Flatten, // towards the plane, both ways
    Scrape,  // only takes material away
    Fill     // only adds it
};

// One dab of a stroke. `previous` closes the gap to the dab before it, so dragging leaves a continuous
// trail instead of a row of balls.
struct SculptBrush
{
    SculptTool tool = SculptTool::Smooth;
    std::array<double, 3> center{0.0, 0.0, 0.0};
    std::array<double, 3> previous{0.0, 0.0, 0.0};
    bool hasPrevious = false;
    double radiusMm = 3.0;
    double level = 0.5; // 0-1 slider; the effect grows 50-fold from end to end, as Freeform's does
    HotWaxMode wax = HotWaxMode::Melt;
    SmoothScope scope = SmoothScope::Inside;
    FlattenMode flatten = FlattenMode::Flatten;
};

// An edit session over one guide: the grid, its limits, the strokes and their undo.
class SculptSession
{
public:
    // Bakes the guide into the grid. `paddingMm` must leave room for material added outside it.
    bool Reset(vtkPolyData* guide, double spacingMm, double paddingMm, QString* error = nullptr,
               const std::atomic<bool>* cancel = nullptr);
    void SetLimits(const SculptLimits& limits) { m_limits = limits; }
    bool Ready() const { return !m_grid.Empty(); }
    const SculptGrid& Grid() const { return m_grid; }

    // press → dabs → release. Everything between one Begin and its End is a single undo step.
    void BeginStroke();
    void ApplyBrush(const SculptBrush& brush);
    void EndStroke();

    // Freeform's Split Piece: a polygon drawn on the guide, swept along the view direction.
    // `keepInside` keeps what is inside the polygon instead of cutting it away.
    bool Trim(const std::vector<std::array<double, 3>>& polygon, const std::array<double, 3>& viewDirection,
              bool keepInside, QString* error = nullptr);
    // Freeform's Select Lump: everything but the largest connected piece is dropped.
    int KeepLargestPiece();

    bool CanUndo() const { return !m_undo.empty(); }
    bool CanRedo() const { return !m_redo.empty(); }
    bool Undo();
    bool Redo();
    void ClearHistory();

    // `fast` is the preview during a drag: no smoothing, no repair. The full contour is what the guide
    // becomes when the stroke ends.
    vtkSmartPointer<vtkPolyData> Contour(bool fast, const std::atomic<bool>* cancel = nullptr) const;

private:
    struct Edit
    {
        std::vector<std::pair<int, std::vector<float>>> blocks; // block id → its contents before the stroke
        std::size_t bytes = 0;
    };

    void saveBlocks(const std::array<int, 3>& lo, const std::array<int, 3>& hi);
    void applyLimits(const std::array<int, 3>& lo, const std::array<int, 3>& hi);
    void applyEdit(Edit& edit); // swaps the saved blocks with the grid, so undo and redo are one operation
    std::array<int, 3> voxelOf(const std::array<double, 3>& p) const;
    bool boxAround(const std::array<double, 3>& a, const std::array<double, 3>& b, double radiusMm, int marginVoxels,
                   std::array<int, 3>& lo, std::array<int, 3>& hi) const;
    void smoothDab(const SculptBrush& brush, double lambda, int passes, double reachFactor);
    void offsetDab(const SculptBrush& brush, double delta, double reachFactor);
    void ballDab(const SculptBrush& brush, bool add);
    void flattenDab(const SculptBrush& brush);

    SculptGrid m_grid;
    SculptLimits m_limits;
    std::array<int, 3> m_blockCounts{0, 0, 0};
    bool m_strokeOpen = false;
    std::map<int, std::vector<float>> m_stroke; // blocks saved during the stroke in progress
    std::array<int, 3> m_strokeLo{0, 0, 0};
    std::array<int, 3> m_strokeHi{-1, -1, -1};
    std::deque<Edit> m_undo;
    std::deque<Edit> m_redo;
    std::size_t m_historyBytes = 0;
};

namespace GuideSculptCore
{
// The level slider, as Freeform's: the right end is 50 times the left.
double LevelScale(double level, double lowest);
// Falloff: 1 at the centre, 0 at `reachMm`, smooth in between.
double Falloff(double distanceMm, double reachMm);
}
