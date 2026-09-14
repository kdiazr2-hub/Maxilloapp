#include "SegmentationMaskEditor.h"
#include "MPRView.h"   // MPROrientation

#include <vtkCoordinate.h>
#include <vtkImageData.h>
#include <vtkRenderer.h>
#include <vtkSmartPointer.h>

// ─────────────────────────────────────────────────────────────────────────────
// Ray-casting even-odd point-in-polygon test.
// poly: 2-D polygon vertices as (x, y) pairs.
// ─────────────────────────────────────────────────────────────────────────────
static bool pointInPolygon(double px, double py,
                           const QVector<std::pair<double, double>>& poly)
{
    bool inside = false;
    const int n = poly.size();
    for (int i = 0, j = n - 1; i < n; j = i++) {
        const double xi = poly[i].first,  yi = poly[i].second;
        const double xj = poly[j].first,  yj = poly[j].second;
        if (((yi > py) != (yj > py)) &&
            (px < (xj - xi) * (py - yi) / (yj - yi) + xi)) {
            inside = !inside;
        }
    }
    return inside;
}

// ─────────────────────────────────────────────────────────────────────────────
bool SegmentationMaskEditor::applyLasso(
    vtkImageData*           labelmap,
    vtkRenderer*            renderer,
    const QVector<QPointF>& vtkDisplayPoints,
    MPROrientation          orientation,
    int                     sliceIndex,
    int                     label,
    bool                    addMode)
{
    if (!labelmap || !renderer || vtkDisplayPoints.size() < 3)
        return false;

    // ── Determine which world axes correspond to the slice plane ──────────
    // Axial:    H = X(0), V = Y(1), N = Z(2)
    // Coronal:  H = X(0), V = Z(2), N = Y(1)
    // Sagittal: H = Y(1), V = Z(2), N = X(0)
    int axisH = 0, axisV = 1, axisN = 2;
    switch (orientation) {
        case MPROrientation::Axial:    axisH = 0; axisV = 1; axisN = 2; break;
        case MPROrientation::Coronal:  axisH = 0; axisV = 2; axisN = 1; break;
        case MPROrientation::Sagittal: axisH = 1; axisV = 2; axisN = 0; break;
    }

    // ── Convert display polygon vertices → world 2-D coordinates ─────────
    auto coord = vtkSmartPointer<vtkCoordinate>::New();
    coord->SetCoordinateSystemToDisplay();

    QVector<std::pair<double, double>> worldPoly;
    worldPoly.reserve(vtkDisplayPoints.size());
    for (const QPointF& dp : vtkDisplayPoints) {
        coord->SetValue(dp.x(), dp.y(), 0.0);
        double* w = coord->GetComputedWorldValue(renderer);
        worldPoly.push_back({w[axisH], w[axisV]});
    }

    // ── Axis-aligned bounding box (world) for early-out ───────────────────
    double minH = worldPoly[0].first,  maxH = worldPoly[0].first;
    double minV = worldPoly[0].second, maxV = worldPoly[0].second;
    for (const auto& p : worldPoly) {
        if (p.first  < minH) minH = p.first;
        if (p.first  > maxH) maxH = p.first;
        if (p.second < minV) minV = p.second;
        if (p.second > maxV) maxV = p.second;
    }

    // ── Labelmap metadata ──────────────────────────────────────────────────
    int    extent[6]  = {};
    double origin[3]  = {};
    double spacing[3] = {1.0, 1.0, 1.0};
    labelmap->GetExtent(extent);
    labelmap->GetOrigin(origin);
    labelmap->GetSpacing(spacing);

    bool modified = false;

    // ── Iterate voxels in the slice plane ─────────────────────────────────
    for (int iH = extent[axisH * 2]; iH <= extent[axisH * 2 + 1]; ++iH) {
        const double wH = origin[axisH] + iH * spacing[axisH];
        if (wH < minH || wH > maxH) continue;

        for (int iV = extent[axisV * 2]; iV <= extent[axisV * 2 + 1]; ++iV) {
            const double wV = origin[axisV] + iV * spacing[axisV];
            if (wV < minV || wV > maxV) continue;

            if (!pointInPolygon(wH, wV, worldPoly)) continue;

            // Build full IJK index
            int ijk[3];
            ijk[axisH] = iH;
            ijk[axisV] = iV;
            ijk[axisN] = sliceIndex;

            // Bounds guard
            if (ijk[0] < extent[0] || ijk[0] > extent[1]) continue;
            if (ijk[1] < extent[2] || ijk[1] > extent[3]) continue;
            if (ijk[2] < extent[4] || ijk[2] > extent[5]) continue;

            const double cur = labelmap->GetScalarComponentAsDouble(
                ijk[0], ijk[1], ijk[2], 0);

            if (addMode && static_cast<int>(cur) == 0) {
                labelmap->SetScalarComponentFromDouble(
                    ijk[0], ijk[1], ijk[2], 0,
                    static_cast<double>(label));
                modified = true;
            } else if (!addMode && static_cast<int>(cur) == label) {
                labelmap->SetScalarComponentFromDouble(
                    ijk[0], ijk[1], ijk[2], 0, 0.0);
                modified = true;
            }
        }
    }

    if (modified)
        labelmap->Modified();

    return modified;
}
