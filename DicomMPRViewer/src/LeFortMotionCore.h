#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// LeFortMotionCore
//
// What the planned Le Fort I movement means for the cutting guide. Only the changes of plane count:
// the segment going up or down, the clockwise / counter-clockwise rotation (more up at the front than at
// the back, or the reverse) and one side going up more than the other. Advancement, set-back, lateral
// shift and rotation in the horizontal plane are the plate's business (user's decision, 2026-10-04).
//
// Where the segment goes up it runs into the bone above the cut, and that bone has to come out: a band
// above the Le Fort cut whose height at each point is how far that point of the segment rises. The
// height is the vertical component of the segment's rigid motion at the point, Z·(M·p − p): it has no
// term in a horizontal translation and none in a rotation about the vertical, so the advancement drops
// out by construction, and it is the same "Z +x impactación" REPOSICIÓN shows. It is affine in p, so
// along each straight piece of the cut it is linear and the band's upper edge is exact from the four
// Le Fort points and the places where the height crosses the threshold.
//
// No Qt Widgets.
// ─────────────────────────────────────────────────────────────────────────────

#include "OsteotomyCore.h"

#include <QString>

#include <array>
#include <utility>
#include <vector>

enum class LeFortBandKind
{
    NoPlaneChange, // nowhere above the threshold: advancement, set-back, lateral shift or yaw only
    Impaction,     // the segment goes up everywhere along the cut
    Descent,       // it goes down everywhere along the cut
    Mixed          // up in some places, down or level in others
};

enum class LeFortPitch
{
    None,
    CounterClockwise, // the front goes up more than the back (anterior impaction)
    Clockwise         // the back goes up more than the front
};

enum class LeFortCant
{
    None,
    RightHigher,
    LeftHigher
};

struct LeFortBandProfile
{
    bool ok = false;
    bool noMotion = false; // the segment has no planned movement (REPOSICIÓN not done)
    QString error;         // in Spanish, ready to show

    // Height of the band at each point of the cut, in the cut's order (pilar D, piriforme D, piriforme I,
    // pilar I): positive where the segment rises, negative where it drops, in mm.
    std::vector<double> heights;
    // Stretches of the cut where the height reaches the threshold, as arc length along the cut from its
    // first point (mm), right to left. The ends sit exactly where the height crosses the threshold.
    std::vector<std::pair<double, double>> spans;
    // The cut with every point raised by its height: the band's upper edge. Meaningful inside `spans`.
    OsteotomyPath upperCut;

    LeFortBandKind kind = LeFortBandKind::NoPlaneChange;
    LeFortPitch pitch = LeFortPitch::None;
    LeFortCant cant = LeFortCant::None;
    QString report; // in Spanish: the height at the four points and the largest one
};

struct LeFortBandParams
{
    std::array<double, 3> vertical{0.0, 0.0, 1.0}; // the patient's vertical (oriented frame, Frankfurt)
    double thresholdMm = 0.5;                      // below this there is no band: about one saw kerf
    double planeDifferenceMm = 0.5;                // pitch / cant are reported from this difference on
    double noMotionMm = 0.2;                       // translation under which the segment did not move...
    double noMotionDeg = 0.3;                      // ...together with a rotation under this
};

namespace LeFortMotionCore
{
// `cut` is the planned Le Fort cut before the movement; `motion` the segment's rigid motion, row-major,
// pre-operative → planned (as `PlateCore::RigidMotion` returns it).
LeFortBandProfile Band(const OsteotomyPath& cut, const std::array<double, 16>& motion,
                       const LeFortBandParams& params = {});

// Length of the cut from its first point to its last, through every point (mm).
double CutLength(const OsteotomyPath& cut);
// The point of the cut at `arcLengthMm` from its first point, clamped to the cut.
std::array<double, 3> PointAlongCut(const OsteotomyPath& cut, double arcLengthMm);
}
