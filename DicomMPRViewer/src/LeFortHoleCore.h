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
#include <vector>

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

    // Where Propose looks: per pillar and side of the cut, between `windowNearMm` and `windowFarMm` from the
    // cut (segment) or from the band's upper edge (cranial), within `lateralReachMm` of the pillar's point.
    int holesPerSide = 2;         // 2 above + 2 below at each pillar (user's choice, 2026-10-04)
    double windowNearMm = 4.0;
    double windowFarMm = 12.0;
    double lateralReachMm = 8.0;
    double pairSpacingMm = 6.5;   // between any two holes: 5.6 mm rings that do not overlap
    double sampleStepMm = 1.0;    // grid the sites are sought on
    // Proposed sites only: the drill axis within 60° of the anterior. Past that the wall faces sideways
    // (the zygoma beyond the buttress) and the guide does not sit there (user's case, 2026-10-05).
    double minAnteriorFacing = 0.5;
    // Roots: the drill path, to the screw's depth, keeps this far from the upper teeth.
    double rootClearanceMm = 1.0;
    double screwDepthMm = 6.0;
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
    // The upper teeth (`ImplicitCore::BakeMeshField` of «Dientes superiores»), when segmented: a drill path
    // that reaches a root is refused. Null skips the check.
    const ImplicitCore::BakedField* teeth = nullptr;
    // The patient's anterior, out of the face. The cut's sweep axis has no sign of its own, so the caller
    // gives it (LeFortGuideCore works it out the same way for the guide's frame).
    std::array<double, 3> anterior{0.0, -1.0, 0.0}; // DICOM LPS: anterior is −Y
    LeFortHoleParams params;
};

enum class LeFortPillar
{
    PillarRight,   // zygomaticomaxillary buttress, right (the cut's first point)
    PiriformRight, // piriform rim, right
    PiriformLeft,
    PillarLeft
};

enum class LeFortCutSide
{
    Cranial, // above the cut: does not move
    Segment  // the Le Fort segment: moves with the plan
};

enum class LeFortHoleOrigin
{
    Auto,  // proposed by the app
    Manual // placed or moved by the surgeon
};

struct LeFortProposedHole
{
    std::array<double, 3> center{0.0, 0.0, 0.0}; // on the bone before the cut
    std::array<double, 3> axis{0.0, 0.0, 1.0};   // out of the bone; the drill goes along −axis
    LeFortPillar pillar = LeFortPillar::PiriformRight;
    LeFortCutSide side = LeFortCutSide::Cranial;
    LeFortHoleOrigin origin = LeFortHoleOrigin::Auto;
    LeFortHoleSupport support;
};

// A pillar and side of the cut that did not get all its holes, and why.
struct LeFortMissingHoles
{
    LeFortPillar pillar = LeFortPillar::PiriformRight;
    LeFortCutSide side = LeFortCutSide::Cranial;
    int missing = 0;
    QString reason; // in Spanish
};

struct LeFortProposal
{
    std::vector<LeFortProposedHole> holes;
    std::vector<LeFortMissingHoles> missing;
};

namespace LeFortHoleCore
{
// Whether a screw may go at `site` (on the pre-operative bone) drilled along −`axis` (`axis` points out of
// the bone).
LeFortHoleSupport Support(const std::array<double, 3>& site, const std::array<double, 3>& axis,
                          const LeFortHoleContext& context);

// The holes the guide proposes by itself: `holesPerSide` above and below the cut at each of the four pillars
// (the cut's points), on bone `Support` does not refuse — Ok first, then thin bone with its warning (the
// anterior wall is often under 2 mm; user's case 2026-10-05) — the thickest first, every pair at least
// `pairSpacingMm` apart. What a pillar cannot take is listed in `missing` with the reason most of its sites were
// refused for. Deterministic.
// `manual` are the holes the surgeon placed or moved: each is kept where it is, judged again (the movement
// may have changed), counts towards its pillar and side, and keeps the proposed ones `pairSpacingMm` away.
LeFortProposal Propose(const LeFortHoleContext& context, const std::vector<LeFortProposedHole>& manual = {});

// A hole the surgeon moved: `picked` (a click on the guide or the bone) is brought onto the bone before the
// cut, the drill axis is the bone's own normal there (`PlateCore::BoneNormalAt`), the side of the cut is the
// bone it landed on, and it is judged by `Support`. It keeps its pillar and becomes manual. Whether to accept
// it is the caller's: a refusal leaves the hole where it was (spec, "orificio movido a mal sitio").
LeFortProposedHole MoveHole(const LeFortProposedHole& hole, const std::array<double, 3>& picked,
                            const LeFortHoleContext& context);

// The guide's holes as the drill sites its sleeves are built at (`PlateCore::SleeveFigures`): centre and axis
// on the bone before the cut, and the bone each one is in. They belong to no plate.
std::vector<PredictiveHole> DrillSites(const std::vector<LeFortProposedHole>& holes);

// How many of the plates' holes have no guide hole within `toleranceMm`: earlier plates the guide no longer
// drills for (spec, "placas anteriores": the report says so).
int UnmatchedPlateHoles(const std::vector<LeFortProposedHole>& guideHoles, const std::vector<PredictiveHole>& plateHoles,
                        double toleranceMm = 1.0);

// Spanish names, for reports.
QString PillarName(LeFortPillar pillar);
QString SideName(LeFortCutSide side);
}
