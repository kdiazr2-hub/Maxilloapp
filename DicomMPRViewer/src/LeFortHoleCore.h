#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// LeFortHoleCore
//
// The fixation holes of the Le Fort cutting guide: whether a site can take a screw, and (Propose) where the
// guide puts them by itself. A hole is drilled through the guide on the bone BEFORE the cut, so every site
// here is in pre-operative coordinates; the plate phase carries the segment's holes to the planned position.
//
// A site is judged three ways:
//   · the rules the plates already enforce (`PlateCore::CheckHoleSeat`): bone all round the screw's ring and
//     4 mm to the osteotomy. A screw on a free margin has nothing to hold it (user's rule, 2026-09-20);
//   · a cranial hole may not go into the band an impaction takes out, nor within 4 mm of its upper edge:
//     that bone is removed in theatre (user's decision, 2026-10-04);
//   · the bone under the screw has to be at least 2.0 mm thick along the drill. Thinner bone is a warning,
//     not a refusal: the surgeon decides (user's decision, 2026-10-04).
// Thickness is measured on the segmented bone itself (`ImplicitCore::BakeMeshField` of the meshes, negative
// inside the wall), never on a wrap: a wrap fills the sinus and would read the whole maxilla as solid.
//
// No Qt Widgets.
// ─────────────────────────────────────────────────────────────────────────────

#include "ImplicitCore.h"
#include "LeFortMotionCore.h"
#include "OsteotomyCore.h"
#include "PlateCore.h"

#include <QString>

#include <array>

enum class LeFortSupportVerdict
{
    Ok,
    Warning, // accepted, marked, with the reason: thin bone
    Rejected // not allowed: margin, too near the cut, inside the band, no bone
};

struct LeFortHoleSupport
{
    double thicknessMm = 0.0; // bone along the drill, from the surface to where it leaves the wall
    LeFortSupportVerdict verdict = LeFortSupportVerdict::Rejected;
    QString reason; // in Spanish, ready to show; empty when Ok
};

struct LeFortHoleParams
{
    double minThicknessMm = 2.0;  // under this, a warning (user's choice, 2026-10-04)
    double bandClearanceMm = 4.0; // a cranial hole stays this far above the band's upper edge
    double bandThresholdMm = 0.5; // where the rise is under this there is no band (LeFortBandParams)
    double maxProbeMm = 15.0;     // thickness is not measured past this
    PlateParams seat;             // ring, edge and cut-distance rules shared with the plates
};

struct LeFortHoleContext
{
    // The bone before the cut, as `ImplicitCore::BakeMeshField` of the cranial base and the Le Fort segment
    // (negative inside the wall). Thickness is measured on it.
    const ImplicitCore::BakedField* bone = nullptr;
    // Nearest-bone queries without a cut margin (`PlateCore::MakeBoneQuery` with an invalid path): on the
    // planned anatomy, and on the same bones before the movement.
    PlateBoneQuery plannedBone;
    PlateBoneQuery preopBone;
    OsteotomyPath cut;                // the planned Le Fort cut, pre-operative coordinates
    std::array<double, 16> motion{};  // the segment's motion, pre-operative → planned, row-major
    LeFortBandProfile band;           // empty (no spans) when there is nothing to take out
    LeFortHoleParams params;
};

namespace LeFortHoleCore
{
// Whether a screw may go at `site` (on the pre-operative bone) drilled along −`axis` (`axis` points out of
// the bone).
LeFortHoleSupport Support(const std::array<double, 3>& site, const std::array<double, 3>& axis,
                          const LeFortHoleContext& context);
}
