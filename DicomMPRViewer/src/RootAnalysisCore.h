#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// RootAnalysisCore
//
// Step 1 of the Le Fort guide assistant (spec asistente-guia-lefort, user's real case of 2026-10-05): how long
// the roots of the upper teeth are and how far their apices lie below the planned cut. A cut closer than 5 mm
// to an apex risks the tooth, so every apex nearer than that is flagged, named or not.
//
// The teeth come as one mesh (DentalSegmentator separates the upper teeth from the bone, not one tooth from
// another). An apex is a local height maximum of the teeth along the cut — the tip of a root — at least
// `minApexSpacingMm` from the next, seen from above (molars behind one another are told apart); its length runs down to the lowest point of the teeth in its column
// (the cusp). The canine is the longest root within `canineReachMm` of the piriform point of its side, the
// first molar the root nearest the pillar point: the cut's own landmarks, so no tooth numbering is needed.
//
// No Qt Widgets.
// ─────────────────────────────────────────────────────────────────────────────

#include "OsteotomyCore.h"

#include <QString>
#include <QStringList>

#include <array>
#include <vector>

class vtkPolyData;

enum class RootTooth
{
    FirstMolarRight,
    CanineRight,
    CanineLeft,
    FirstMolarLeft
};

struct RootApex
{
    std::array<double, 3> apex{0.0, 0.0, 0.0};  // the root's tip
    std::array<double, 3> cusp{0.0, 0.0, 0.0};  // the lowest point of the tooth in its column
    std::array<double, 3> onCut{0.0, 0.0, 0.0}; // straight above the apex, on the cut
    double lengthMm = 0.0;                       // apex to cusp, along the vertical
    double cutDistanceMm = 0.0;                  // apex to the cut, along the vertical (negative: above it)
    bool tooClose = false;                       // under `minCutDistanceMm`
};

struct RootAnalysisParams
{
    double minCutDistanceMm = 5.0; // user's choice, 2026-10-05
    double binMm = 1.0;
    double minApexSpacingMm = 4.0;
    double columnRadiusMm = 2.5;   // how far round the apex the cusp is looked for
    double canineReachMm = 8.0;    // from the piriform point, seen from above
    double molarReachMm = 8.0;     // from the pillar point, seen from above
};

struct RootAnalysis
{
    bool ok = false;
    QString error;
    std::vector<RootApex> apices;           // right to left along the cut
    std::array<int, 4> named{-1, -1, -1, -1}; // index into `apices` per RootTooth, −1 when not identified
    QStringList warnings;                   // in Spanish, one per canine or first molar too close to the cut
    QString report;
};

namespace RootAnalysisCore
{
// `teeth` are the upper teeth before the cut (any mesh; only its points are read), `cut` the planned Le Fort
// cut with its four points (pilar D, piriforme D, piriforme I, pilar I) and its upward axis.
RootAnalysis Analyze(vtkPolyData* teeth, const OsteotomyPath& cut, const RootAnalysisParams& params = {});

QString ToothName(RootTooth tooth);
}
