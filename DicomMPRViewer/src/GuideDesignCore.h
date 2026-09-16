#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// GuideDesignCore
//
// The whole guide in one field: base, saw slots and fixation holes carved
// together and contoured once, which is what keeps the splint generator from
// ever producing broken meshes and what the guide inherits here. Building the
// base, meshing it, then re-baking that mesh to cut the slots would resample
// twice and round the wall a second time.
//
//   base = region prism ∧ layer of the wrap distance ∧ near side
//   guide = base − saw slots (the osteotomy's own field) − hole cylinders
//
// Split as `SplintHeightmapGenerator` is: Prepare bakes the wrap once (slow),
// Build reruns on every change of contour, thickness, slots or holes.
// No Qt Widgets.
// ─────────────────────────────────────────────────────────────────────────────

#include "CutSlotCore.h"
#include "GuideBaseCore.h"
#include "ImplicitCore.h"

#include <QString>
#include <vtkSmartPointer.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

class vtkPolyData;

// Placed by hand on the guide, drilled along the surface normal. Mirrors the splint's
// `SplintWireHole`; only guide fixation for now, no plate pre-drilling.
struct GuideFixationHole
{
    std::array<double, 3> center{0.0, 0.0, 0.0};
    std::array<double, 3> axis{0.0, 0.0, 1.0};
    double diameterMm = 2.0;
};

// One saw slot: which osteotomy it follows and, optionally, where along it the user placed its ends.
// Without ends the slot runs the whole marked region; either way it is clipped to the region shrunk by
// `edgeMarginMm`, so it never reaches the rim and the guide stays in one piece.
struct GuideSlot
{
    OsteotomyPath path;
    std::array<double, 3> start{0.0, 0.0, 0.0};
    std::array<double, 3> end{0.0, 0.0, 0.0};
    bool hasExtent = false; // true once the user has placed both ends
};

struct GuideDesignParams
{
    GuideBaseParams base;
    CutSlotParams slot;
    double holeLengthMm = 30.0; // cylinder length, through the wall either way
    double edgeMarginMm = 2.0;  // material left between any slot and the edge of the guide
};

// The slow half: the wrap measured once.
struct GuidePreparation
{
    bool ok = false;
    QString error;
    std::shared_ptr<const ImplicitCore::BakedField> wrapField;
    double spanMm = 0.0; // how far the region prism has to reach
    double spacingMm = 0.0;
};

struct GuideDesignResult
{
    bool ok = false;
    QString error;
    QString report;
    vtkSmartPointer<vtkPolyData> mesh;
    int pieces = 0; // a slot across the guide separates it
    std::array<double, 3> projectionAxis{0.0, 0.0, 1.0};
    double spacingMm = 0.0;
};

namespace GuideDesignCore
{
GuidePreparation Prepare(vtkPolyData* wrap, const GuideDesignParams& params = {},
                         const std::atomic<bool>* cancel = nullptr);
GuideDesignResult Build(const GuidePreparation& prepared, const GuideContour& contour,
                        const std::vector<GuideSlot>& slotPlan, const std::vector<GuideFixationHole>& holes,
                        const GuideDesignParams& params = {}, const std::atomic<bool>* cancel = nullptr);
// Outward normal of the wrap, to drill a hole along it where the user clicked.
std::array<double, 3> SurfaceNormalAt(const GuidePreparation& prepared, const std::array<double, 3>& point);
}
