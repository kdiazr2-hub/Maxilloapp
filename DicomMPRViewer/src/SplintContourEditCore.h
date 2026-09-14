#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// SplintContourEditCore
//
// Interactive editing of the splint contour in the occlusal (u, v) plane: a
// grabbed vertex moves by the drag delta and its neighbours follow with a
// cosine falloff over an influence radius given as a percentage of the contour
// perimeter (1–50 %, 20 % by default; Alt + vertical drag changes it).
// ─────────────────────────────────────────────────────────────────────────────

#include "SplintHeightmapGenerator.h"

#include <optional>
#include <vector>

namespace SplintContourEditCore
{
inline constexpr double kMinInfluencePercent = 1.0;
inline constexpr double kMaxInfluencePercent = 50.0;
inline constexpr double kDefaultInfluencePercent = 20.0;
inline constexpr double kInfluencePercentPerPixel = 0.1;

struct Handle
{
    int contour = -1;
    int vertex = -1;
    double distanceMm = 0.0;
};

double Perimeter(const SplintContourUV& contour);
double ClampInfluence(double percent);
// Dragging up (negative deltaY) grows the radius.
double InfluenceAfterDrag(double percent, double deltaYPixels);
// Inserts vertices so no segment is longer than maxSegmentMm; original vertices are kept.
SplintContourUV Densify(const SplintContourUV& contour, double maxSegmentMm);
std::optional<Handle> FindHandle(const std::vector<SplintContourUV>& contours, const SplintPointUV& point,
                                 double maxDistanceMm);
// 1 at the grabbed vertex, 0 at and beyond radiusMm (arc length along the contour).
double InfluenceWeight(double arcDistanceMm, double radiusMm);
// Apply to the contour captured when the drag started, with the total delta.
SplintContourUV Drag(const SplintContourUV& contour, int vertex, const SplintPointUV& delta, double influencePercent);
}
