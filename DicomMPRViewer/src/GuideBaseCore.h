#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// GuideBaseCore
//
// 3-matic's Create Base (Guide group): the region the user marks on the wrap
// becomes a solid of uniform thickness that follows the anatomy.
//
//   occupied(p) = inside the region prism  ∧  clearance <= dWrap(p) <= clearance + thickness
//                 ∧  on the marked side (the prism would otherwise pick up the far wall too)
//
// dWrap is the real signed distance to the wrap solid, not an interval along an
// axis: that is what keeps the thickness uniform where the wall turns oblique —
// the zygomatic buttresses, exactly where the guide needs its stiffness.
//
// The region is marked the way the composite is (`CompositeBlockCore`): points
// placed on the surface, in any order, forming a polygon swept along a
// projection axis. Everything is carved into one field and contoured once
// (`ImplicitCore`), so the base comes out closed.
// No Qt Widgets.
// ─────────────────────────────────────────────────────────────────────────────

#include "ImplicitCore.h"

#include <QJsonArray>
#include <QString>
#include <vtkSmartPointer.h>

#include <array>
#include <atomic>
#include <vector>

class vtkPolyData;

// Points marked on the wrap, in placement order.
using GuideContour = std::vector<std::array<double, 3>>;

struct GuideBaseParams
{
    double thicknessMm = 2.5;      // wall thickness of the guide
    double clearanceMm = 0.0;      // gap left against the anatomy
    double smallestDetailMm = 0.25;
    int smoothingIterations = 12;  // also rounds the cut edge of the base
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
// Outward direction of the marked patch: the field gradient at the points, averaged. The polygon is
// projected along it, so it also decides which way the region prism is swept.
std::array<double, 3> ProjectionAxis(vtkPolyData* wrap, const GuideContour& contour, double detailMm = 0.5);
// Closed polyline through the marked points (display).
vtkSmartPointer<vtkPolyData> ContourPolyline(const GuideContour& contour);

GuideBaseResult CreateBase(vtkPolyData* wrap, const GuideContour& contour, const GuideBaseParams& params = {},
                           const std::atomic<bool>* cancel = nullptr);

// The same base as a field node, so slots and holes can be carved into one field and contoured once
// (`GuideDesignCore`). `spanMm` is how far the region prism reaches through the anatomy.
ImplicitCore::NodePtr BaseNode(const std::shared_ptr<const ImplicitCore::BakedField>& wrapField,
                               const GuideContour& contour, const GuideBaseParams& params,
                               const std::array<double, 3>& projectionAxis, double spanMm);
// Outward normal of the wrap at a point, from an already baked field.
std::array<double, 3> NormalAt(const ImplicitCore::BakedField& field, const std::array<double, 3>& point);

QJsonArray ContourToJson(const GuideContour& contour);
GuideContour ContourFromJson(const QJsonArray& array);
}
