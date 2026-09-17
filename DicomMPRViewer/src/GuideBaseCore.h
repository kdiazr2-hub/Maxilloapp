#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// GuideBaseCore
//
// 3-matic's Create Base (Guide group): the region the user marks on the wrap
// becomes a solid of uniform thickness that follows the anatomy.
//
//   occupied(p) = inside the rounded region prism
//                 ∧  clearance <= dWrap(p) <= clearance + thickness
//                 ∧  in front of the first surface seen along the projection axis
//
// dWrap is the real signed distance to the wrap solid, not an interval along an
// axis: that is what keeps the thickness uniform where the wall turns oblique —
// the zygomatic buttresses, exactly where the guide needs its stiffness.
//
// The marked points outline a polygon in the plane of the projection axis. It is
// rounded (morphological opening and closing by `cornerRadiusMm`) so the guide's
// rim is smooth, and kept as a 2D signed distance. The base only covers the
// surface the user marked: a height map of the first surface seen along the axis
// keeps the wall off whatever lies behind it (the back of a thin wall, the far
// side of the bone) and lets it wrap past the silhouette by no more than its own
// thickness. Everything is carved into one field and contoured once
// (`ImplicitCore`), so the base comes out closed.
// No Qt Widgets.
// ─────────────────────────────────────────────────────────────────────────────

#include "ImplicitCore.h"

#include <QJsonArray>
#include <QString>
#include <vtkSmartPointer.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

class vtkPolyData;

// Points marked on the wrap, in placement order.
using GuideContour = std::vector<std::array<double, 3>>;

// One dab of the region brush on the wrap: paints (or erases) a column of `radiusMm` through the guide wall,
// along the surface normal at `center`. Dabs apply in order, so painting over an erased spot paints it again.
struct GuideBrushStroke
{
    std::array<double, 3> center{0.0, 0.0, 0.0};
    double radiusMm = 4.0;
    bool erase = false;
};
using GuideBrushPaint = std::vector<GuideBrushStroke>;

struct GuideBaseParams
{
    double thicknessMm = 2.5;      // wall thickness of the guide
    double clearanceMm = 0.0;      // gap left against the anatomy
    double smallestDetailMm = 0.25;
    int smoothingIterations = 30;  // windowed sinc on the contoured guide
    double cornerRadiusMm = 3.0;   // rounding of the marked outline
    // Finish of the rim, like a printed guide pad: the wall thins towards the edge over `edgeTaperMm`, down to
    // `edgeThicknessFraction` of its thickness, and the edge is rounded over `edgeRoundMm`. Zeros give a square rim.
    double edgeTaperMm = 4.0;
    double edgeThicknessFraction = 0.4;
    double edgeRoundMm = 1.2;
};

// The marked patch, rounded and seen along its projection axis.
struct GuideRegion
{
    bool valid = false;
    QString error;
    std::array<double, 3> axis{0.0, 0.0, 1.0};
    std::shared_ptr<const ImplicitCore::BakedPlanarField> outline; // rounded region: 2D signed distance
    std::shared_ptr<const ImplicitCore::BakedPlanarField> front;   // heights the base must stay above
    std::shared_ptr<const ImplicitCore::BakedField> paint;         // brushed columns (brush regions only)
};

struct GuideBaseResult
{
    bool ok = false;
    QString error;
    QString report;
    vtkSmartPointer<vtkPolyData> mesh;
    std::array<double, 3> projectionAxis{0.0, 0.0, 1.0};
    double spacingMm = 0.0;
};

namespace GuideBaseCore
{
inline constexpr int MinContourPoints = 3;

bool ContourValid(const GuideContour& contour, QString* error = nullptr);
// Outward direction of the marked patch: the field gradient at the points, averaged.
std::array<double, 3> ProjectionAxis(vtkPolyData* wrap, const GuideContour& contour, double detailMm = 0.5);
// Outward normal of the wrap at a point, from an already baked field.
std::array<double, 3> NormalAt(const ImplicitCore::BakedField& field, const std::array<double, 3>& point);
// Straight polyline through the marked points, in order (display while marking).
vtkSmartPointer<vtkPolyData> ContourPolyline(const GuideContour& contour);

// Rounds the marked outline and measures the surface in front of it. The params give the corner radius and
// how far past the silhouette the wall may wrap (its own thickness).
GuideRegion MakeRegion(const std::shared_ptr<const ImplicitCore::BakedField>& wrapField, const GuideContour& contour,
                       const GuideBaseParams& params);
// The region painted with the brush: exactly the surface under the dabs (minus the erased ones), in front of
// whatever lies behind it, whatever order it was painted in.
bool PaintValid(const GuideBrushPaint& paint, QString* error = nullptr);
GuideRegion MakeBrushRegion(const std::shared_ptr<const ImplicitCore::BakedField>& wrapField,
                            const GuideBrushPaint& paint, const GuideBaseParams& params);
QJsonArray PaintToJson(const GuideBrushPaint& paint);
GuideBrushPaint PaintFromJson(const QJsonArray& array);
// The rounded outline laid on the surface, lifted slightly so it is not hidden by it (display).
vtkSmartPointer<vtkPolyData> RegionOutline(const GuideRegion& region, double liftMm = 0.3);

GuideBaseResult CreateBase(vtkPolyData* wrap, const GuideContour& contour, const GuideBaseParams& params = {},
                           const std::atomic<bool>* cancel = nullptr);

// The same base as a field node, so slots, holes and figures can be carved into one field and contoured
// once (`GuideDesignCore`). `spanMm` is how far the region prism reaches through the anatomy.
ImplicitCore::NodePtr BaseNode(const std::shared_ptr<const ImplicitCore::BakedField>& wrapField,
                               const GuideRegion& region, const GuideBaseParams& params, double spanMm);
// The rounded region swept along the axis on its own. Shrinking it (a negative offset) keeps the saw slots
// away from the rim so the guide stays in one piece.
ImplicitCore::NodePtr RegionPrism(const GuideRegion& region, double spanMm);

QJsonArray ContourToJson(const GuideContour& contour);
GuideContour ContourFromJson(const QJsonArray& array);
}
