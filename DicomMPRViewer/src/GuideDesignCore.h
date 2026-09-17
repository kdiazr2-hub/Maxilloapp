#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// GuideDesignCore
//
// The whole guide in one field: base, saw slots, fixation holes and the user's
// figures carved together and contoured once, which is what keeps the splint
// generator from ever producing broken meshes and what the guide inherits here.
// Building the base, meshing it, then re-baking that mesh to cut the slots would
// resample twice and round the wall a second time.
//
//   base  = rounded region prism ∧ layer of the wrap distance ∧ in front of the marked surface
//   guide = (base ∪ added figures) − saw slots − hole cylinders − subtracted figures
//
// Figures are the Boolean tools of the module: cylinders, boxes and spheres with
// exact measurements, or imported STL shapes, each placed with a local frame and
// either added to the guide or subtracted from it (a thin box makes a straight
// saw slot, a cylinder a drill sleeve's bore). They are fields like everything
// else, so a subtraction can never leave a broken mesh.
//
// Split as `SplintHeightmapGenerator` is: Prepare bakes the wrap once (slow),
// Build reruns on every change of contour, thickness, slots, holes or figures.
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

enum class GuideFigureShape
{
    Cylinder, // diameter, length along local z
    Box,      // width (x), height (y), depth (z)
    Sphere,   // diameter
    Mesh      // imported STL, in local coordinates
};

enum class GuideFigureOperation
{
    Subtract,
    Add
};

// A Boolean tool placed on the guide. Its geometry is centred on the local origin; `matrix` (row-major,
// local → world) puts it in place, so moving it with the gizmo only changes the matrix.
struct GuideFigure
{
    GuideFigureShape shape = GuideFigureShape::Cylinder;
    GuideFigureOperation operation = GuideFigureOperation::Subtract;
    std::array<double, 16> matrix{1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0};
    double diameterMm = 3.0;
    double lengthMm = 12.0;
    double widthMm = 12.0;
    double heightMm = 1.0;
    double depthMm = 12.0;
    vtkSmartPointer<vtkPolyData> mesh; // Mesh shape only
    QString sourcePath;                // Mesh shape: the imported file, kept in the project
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
    int pieces = 0; // a slot or a figure across the guide separates it
    std::array<double, 3> projectionAxis{0.0, 0.0, 1.0};
    double spacingMm = 0.0;
};

namespace GuideDesignCore
{
GuidePreparation Prepare(vtkPolyData* wrap, const GuideDesignParams& params = {},
                         const std::atomic<bool>* cancel = nullptr);
GuideDesignResult Build(const GuidePreparation& prepared, const GuideContour& contour,
                        const std::vector<GuideSlot>& slotPlan, const std::vector<GuideFixationHole>& holes,
                        const std::vector<GuideFigure>& figures, const GuideDesignParams& params = {},
                        const std::atomic<bool>* cancel = nullptr);
// Without figures.
GuideDesignResult Build(const GuidePreparation& prepared, const GuideContour& contour,
                        const std::vector<GuideSlot>& slotPlan, const std::vector<GuideFixationHole>& holes,
                        const GuideDesignParams& params = {}, const std::atomic<bool>* cancel = nullptr);
// Outward normal of the wrap, to drill a hole or seat a figure along it where the user clicked.
std::array<double, 3> SurfaceNormalAt(const GuidePreparation& prepared, const std::array<double, 3>& point);

// A local frame at `center` whose z axis is `zAxis` (a figure placed on the surface points out of it).
std::array<double, 16> FrameAt(const std::array<double, 3>& center, const std::array<double, 3>& zAxis);
// The figure as a field node in world coordinates; nullptr (and `error`) when it has no geometry.
ImplicitCore::NodePtr FigureNode(const GuideFigure& figure, double detailMm, QString* error = nullptr);
// The figure as a mesh in world coordinates, for display and the gizmo.
vtkSmartPointer<vtkPolyData> FigurePreview(const GuideFigure& figure);
}
