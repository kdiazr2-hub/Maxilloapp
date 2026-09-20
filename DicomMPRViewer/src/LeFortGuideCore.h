#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// LeFortGuideCore
//
// The Le Fort I cutting guide, laid out on pre-operative anatomy before the
// definitive-position custom plate is transferred to the pre-operative model (Gander et al., JCMFS 2015;
// Surg 2026; Abdelhamid et al., Cureus 2025; Benito Anguita et al., JCM 2025):
//
//   · one piece across the midline, bone-borne on the anterior maxillary wall,
//     laid out as an openwork frame: it grips more of the wall, stays light and
//     flexes onto the bone, which is what makes a printed guide seat passively
//     (Gander et al. 2015), and the surgeon sees the bone through it;
//   · a band along the planned osteotomy, where the saw slit runs exactly on the
//     cut. Where the cut crosses the piriform aperture there is no anterior wall
//     at that level, so the band dips onto the alveolar wall below it and the
//     guide does not fall in two at the midline;
//   · pads and drill sleeves at every predictive plate hole, using the same centre and drilling vector;
//   · the slit continuous along the whole cut, interrupted only by bridges
//     (always one at the midline) so the two halves of the guide stay rigidly
//     together. It comes from the plan, not from the envelope: a slit made only
//     where a wrap vertex happened to land on the cut came out as stubs;
//   · 1.5 mm positioning screws that hold the guide while the holes are drilled
//     and the cut is made: one at each end above the cut with plates (the
//     cranial side does not move), four above and below without them.
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
    double bandRadiusMm = 8.0;        // brush radius along the cut: the band covers ±8 mm around it
    double holePadMm = 1.8;           // material round each sleeve, past its outer radius
    // The band is an openwork frame, not a solid plate: a row of cells above and below the slit.
    double latticeCellMm = 2.6;       // cell diameter; 0 turns the lattice off
    double latticeSpacingMm = 4.4;    // between cell centres along the cut
    double latticeMarginMm = 1.1;     // material left round every cell
    double latticeSlitClearMm = 2.0;  // material left between a cell and the slit
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
    std::vector<GuideFigure> figures;      // the lattice cells, subtracted
    std::vector<std::array<double, 3>> cutLine; // where the slit meets the anterior wall
    int dippedBins = 0;                    // bins where the band went below the aperture
};

namespace LeFortGuideCore
{
// `preop` is the wrap of the bone before the cut (its field), `wrapMesh` its surface and `path` the planned
// Le Fort cut. `holes` are the definitive plate holes carried back to the pre-operative anatomy.
LeFortGuideLayout Layout(const GuidePreparation& preop, vtkPolyData* wrapMesh, const OsteotomyPath& path,
                         const std::vector<PredictiveHole>& holes, const LeFortGuideParams& params = {});
}
