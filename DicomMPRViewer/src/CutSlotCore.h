#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// CutSlotCore
//
// 3-matic's Cut Slot (Guide group): the saw slots of a guide, taken from the
// planned osteotomy itself. The slot is the slab of the osteotomy's own signed
// field, the very field `OsteotomyCore::SplitByPath` cuts the bone with:
//
//   occupied(p) &= !( |pathField(p)| < bladeThickness / 2 )
//
// so slot and osteotomy coincide by construction, not by numbers that happen to
// agree. A stepped path gives a stepped slot; the field is defined beyond the
// ends of the path, so the slot runs past the sides of the base and the blade
// can enter. That also means a slot crossing the whole base separates it: the
// report says how many pieces came out.
//
// The path field is baked to the grid once (`ImplicitCore::BakeFunction`) and
// the slots are carved into the same field as the base, contoured once.
// No Qt Widgets.
// ─────────────────────────────────────────────────────────────────────────────

#include "ImplicitCore.h"
#include "OsteotomyCore.h"

#include <QString>
#include <vtkSmartPointer.h>

#include <atomic>
#include <vector>

class vtkPolyData;

struct CutSlotParams
{
    double bladeThicknessMm = 0.6; // saw blade, 0.4 - 1.0 mm
    double extensionMm = 3.0;      // how far the slot reaches past the base, for the blade to enter
    double smallestDetailMm = 0.25;
    int smoothingIterations = 8;
};

struct CutSlotResult
{
    bool ok = false;
    QString error;
    QString report;
    vtkSmartPointer<vtkPolyData> mesh;
    int pieces = 0; // shells: a slot right across the base leaves two
    double spacingMm = 0.0;
};

namespace CutSlotCore
{
CutSlotResult CutSlots(vtkPolyData* base, const std::vector<OsteotomyPath>& paths, const CutSlotParams& params = {},
                       const std::atomic<bool>* cancel = nullptr);

// One slot as a field node over `bounds`, for carving base, slots and holes into a single field
// (`GuideDesignCore`). Returns nullptr on an invalid path or when cancelled.
ImplicitCore::NodePtr SlotNode(const OsteotomyPath& path, const double bounds[6], const CutSlotParams& params,
                               const std::atomic<bool>* cancel = nullptr, QString* error = nullptr);
}
