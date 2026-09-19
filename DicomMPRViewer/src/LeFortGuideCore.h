#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// LeFortGuideCore
//
// The Le Fort I cutting and drilling guide of the custom-plate workflow, laid
// out from the plan instead of painted by hand (Mohamed et al., Arch Craniofac
// Surg 2026; Abdelhamid et al., Cureus 2025; Benito Anguita et al., JCM 2025):
//
//   · one piece across the midline, bone-borne on the anterior maxillary wall;
//   · a band along the planned osteotomy, where the saw slit runs exactly on the
//     cut. Where the cut crosses the piriform aperture there is no anterior wall
//     at that level, so the band dips onto the alveolar wall below it and the
//     guide does not fall in two at the midline;
//   · a pad round every predictive hole (the plates' screws, drilled through the
//     guide's sleeves before the cut), joined to the band;
//   · the slit interrupted by bridges (always one at the midline) so the two
//     halves of the guide stay rigidly together;
//   · four 1.5 mm fixation screws, above and below the cut at the lateral ends.
//
// It only produces the plan — brush dabs, slot pieces with their ends and
// fixation holes — so the guide is still carved by `GuideDesignCore::Build` in
// one field, and the surgeon can retouch the region with the brush or EDITAR.
// No Qt Widgets.
// ─────────────────────────────────────────────────────────────────────────────

#include "GuideBaseCore.h"
#include "GuideDesignCore.h"
#include "OsteotomyCore.h"
#include "PlateCore.h"

#include <QString>

#include <array>
#include <vector>

class vtkPolyData;

struct LeFortGuideParams
{
    double bandRadiusMm = 5.0;        // brush radius along the cut: the band covers ±5 mm around it
    double holePadMm = 3.0;           // material round each sleeve, past its outer radius
    double lateralMarginMm = 5.0;     // the band runs this far past the outermost predictive hole
    double dabSpacingMm = 2.5;        // along the cut
    double fixationDiameterMm = 1.5;  // guide fixation screws (user's choice)
    double fixationOffsetMm = 6.0;    // above and below the cut
    double minFixationToHoleMm = 5.0; // keep them clear of the predictive holes
    double bridgeSpacingMm = 15.0;    // a bridge across the slit every so often, one at the midline
    double bridgeWidthMm = 3.0;
    double sleeveOuterDiameterMm = 4.2;
};

struct LeFortGuideLayout
{
    bool ok = false;
    QString error;
    QString report;
    GuideBrushPaint paint;                 // the support region
    std::vector<GuideSlot> slotPlan;       // slit pieces between the bridges, each with its ends
    std::vector<GuideFixationHole> fixation;
    std::vector<std::array<double, 3>> cutLine; // where the slit meets the anterior wall
    int dippedBins = 0;                    // bins where the band went below the aperture
};

namespace LeFortGuideCore
{
// `preop` is the wrap of the bone before the cut (its field), `wrapMesh` its surface; `holes` are the
// plates' predictive holes and `path` the planned Le Fort cut, both in pre-operative coordinates.
LeFortGuideLayout Layout(const GuidePreparation& preop, vtkPolyData* wrapMesh, const OsteotomyPath& path,
                         const std::vector<PredictiveHole>& holes, const LeFortGuideParams& params = {});
}
