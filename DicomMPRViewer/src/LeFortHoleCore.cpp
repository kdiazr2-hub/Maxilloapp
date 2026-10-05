#include "LeFortHoleCore.h"

#include <algorithm>
#include <cmath>

namespace
{
using Vec3 = std::array<double, 3>;

Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 scale(const Vec3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double norm(const Vec3& a) { return std::sqrt(dot(a, a)); }
Vec3 unit(const Vec3& a, const Vec3& fallback)
{
    const double length = norm(a);
    return length > 1e-12 ? scale(a, 1.0 / length) : fallback;
}

// The band's vertical: the direction its upper edge was raised in. Z when nothing was raised.
Vec3 bandVertical(const LeFortBandProfile& band, const OsteotomyPath& cut)
{
    const size_t count = std::min({band.heights.size(), band.upperCut.points.size(), cut.points.size()});
    for (size_t i = 0; i < count; ++i)
        if (std::abs(band.heights[i]) > 1e-6)
            return unit(scale(sub(band.upperCut.points[i], cut.points[i]), 1.0 / band.heights[i]), {0.0, 0.0, 1.0});
    return {0.0, 0.0, 1.0};
}

// Where the band stands over `site`: the nearest point of the cut seen along the vertical, the band's height
// there (linear along each piece, as the rise is), and how far the site is above the cut.
struct OverCut
{
    double heightMm = 0.0;
    double aboveCutMm = 0.0;
};
OverCut overCut(const Vec3& site, const OsteotomyPath& cut, const std::vector<double>& heights, const Vec3& up)
{
    const auto flat = [&up](const Vec3& v) { return sub(v, scale(up, dot(v, up))); };
    OverCut best;
    double nearest = 1e30;
    for (size_t i = 1; i < cut.points.size() && i < heights.size(); ++i) {
        const Vec3 a = cut.points[i - 1], b = cut.points[i];
        const Vec3 piece = flat(sub(b, a));
        const double length2 = dot(piece, piece);
        const double t = length2 > 1e-12 ? std::clamp(dot(flat(sub(site, a)), piece) / length2, 0.0, 1.0) : 0.0;
        const Vec3 onCut = add(a, scale(sub(b, a), t));
        const double distance = norm(flat(sub(site, onCut)));
        if (distance < nearest) {
            nearest = distance;
            best.heightMm = heights[i - 1] + t * (heights[i] - heights[i - 1]);
            best.aboveCutMm = dot(sub(site, onCut), up);
        }
    }
    return best;
}

// Bone along the drill: from where the line −axis enters the wall to where it leaves it, the zero crossings
// interpolated between samples. The site sits on the surface, so the entry is within the first millimetre.
double thicknessAlong(const ImplicitCore::BakedField& bone, const Vec3& site, const Vec3& axis, double maxMm)
{
    const Vec3 inward = scale(unit(axis, {0.0, 0.0, 1.0}), -1.0);
    const double step = std::clamp(0.25 * bone.spacingMm, 0.02, 0.1);
    double entered = -1.0;
    double previousT = -1.0;
    double previous = bone.At(add(site, scale(inward, -1.0))); // a millimetre outside
    for (double t = -1.0 + step; t <= maxMm + 1e-9; t += step) {
        const double value = bone.At(add(site, scale(inward, t)));
        const double crossing = previous != value ? previousT + step * previous / (previous - value) : t;
        if (entered < 0.0 && previous >= 0.0 && value < 0.0)
            entered = std::max(0.0, crossing);
        else if (entered >= 0.0 && previous < 0.0 && value >= 0.0)
            return std::max(0.0, crossing - entered);
        previous = value;
        previousT = t;
    }
    return entered >= 0.0 ? maxMm - entered : 0.0;
}
} // namespace

namespace LeFortHoleCore
{
LeFortHoleSupport Support(const std::array<double, 3>& site, const std::array<double, 3>& axis,
                          const LeFortHoleContext& context)
{
    LeFortHoleSupport support;
    const LeFortHoleParams& params = context.params;
    if (!context.bone || !context.preopBone) {
        support.reason = QStringLiteral("Falta el hueso antes del corte para evaluar el orificio.");
        return support;
    }

    // Which bone the site is on, before the movement: that is where the drill goes.
    double distance = 0.0;
    const PlateBone bone = context.preopBone(site, &distance);
    if (bone == PlateBone::Unknown || distance > 1.0) {
        support.reason = QStringLiteral("Ahí no hay hueso: marque sobre la pared anterior del maxilar.");
        return support;
    }

    // The band first, for a cranial hole: inside it, or too close above it, the bone is taken out in theatre.
    // Judged before the ring, because the band's own edge would otherwise read as a free margin and the
    // surgeon would be told the wrong reason.
    if (bone == PlateBone::Cranial && !context.band.spans.empty()) {
        const Vec3 up = bandVertical(context.band, context.cut);
        const OverCut over = overCut(site, context.cut, context.band.heights, up);
        // Only where the band exists: the rise reaches the band's own threshold there.
        if (over.heightMm >= params.bandThresholdMm && over.aboveCutMm < over.heightMm + params.bandClearanceMm) {
            support.reason =
                over.aboveCutMm <= over.heightMm
                    ? QStringLiteral("El orificio cae dentro de la franja de hueso que se quita (%1 mm): "
                                     "colóquelo al menos %2 mm por encima de ella.")
                          .arg(over.heightMm, 0, 'f', 1)
                          .arg(params.bandClearanceMm, 0, 'f', 1)
                    : QStringLiteral("El orificio queda a %1 mm de la franja que se quita: deje al menos %2 mm.")
                          .arg(over.aboveCutMm - over.heightMm, 0, 'f', 1)
                          .arg(params.bandClearanceMm, 0, 'f', 1);
            return support;
        }
    }

    // The plates' own rules: bone all round the ring, and the distance to the osteotomy. They take the
    // centre on the planned anatomy, so a segment site is carried there first.
    const Vec3 plannedSite = bone == PlateBone::Segment ? PlateCore::TransformPoint(context.motion, site) : site;
    const Vec3 plannedAxis = bone == PlateBone::Segment ? PlateCore::TransformVector(context.motion, axis) : axis;
    const HoleSeat seat = PlateCore::CheckHoleSeat(plannedSite, plannedAxis, context.plannedBone, context.preopBone,
                                                   context.cut, context.motion, params.seat);
    if (!seat.ok) {
        support.reason = seat.reason;
        return support;
    }

    // And enough bone under the screw.
    support.thicknessMm = thicknessAlong(*context.bone, site, axis, params.maxProbeMm);
    if (support.thicknessMm < params.minThicknessMm) {
        support.verdict = LeFortSupportVerdict::Warning;
        support.reason = QStringLiteral("Hueso de %1 mm bajo el tornillo: menos del grosor mínimo de %2 mm.")
                             .arg(support.thicknessMm, 0, 'f', 1)
                             .arg(params.minThicknessMm, 0, 'f', 1);
        return support;
    }
    support.verdict = LeFortSupportVerdict::Ok;
    return support;
}

LeFortProposal Propose(const LeFortHoleContext& context, const std::vector<LeFortProposedHole>& manual)
{
    LeFortProposal proposal;
    const LeFortHoleParams& params = context.params;
    if (!context.bone || !context.preopBone || context.cut.points.size() != 4)
        return proposal;

    const Vec3 up = unit(context.cut.upAxis, {0.0, 0.0, 1.0});
    const Vec3 front = unit(sub(context.anterior, scale(up, dot(context.anterior, up))), {0.0, -1.0, 0.0});
    const Vec3 lateral = unit(Vec3{up[1] * front[2] - up[2] * front[1], up[2] * front[0] - up[0] * front[2],
                                   up[0] * front[1] - up[1] * front[0]},
                              {1.0, 0.0, 0.0});
    const bool hasBand = !context.band.spans.empty();
    const std::vector<double> noRise(context.cut.points.size(), 0.0);
    const std::vector<double>& rises = hasBand ? context.band.heights : noRise;
    // The band's height under a site, where there is a band at all.
    const auto bandUnder = [&](const OverCut& over) {
        return hasBand && over.heightMm >= params.bandThresholdMm ? over.heightMm : 0.0;
    };

    // A site found on the anterior wall: start in front of the face and walk back into the bone.
    const auto onWall = [&](const Vec3& start, Vec3& site) {
        constexpr double kReachMm = 30.0;
        const double step = std::clamp(0.5 * context.bone->spacingMm, 0.02, 0.1);
        double previous = context.bone->At(start);
        for (double t = step; t <= kReachMm; t += step) {
            const Vec3 p = add(start, scale(front, -t));
            const double value = context.bone->At(p);
            if (previous >= 0.0 && value < 0.0) {
                const double back = step * value / (value - previous); // to the zero crossing
                site = add(p, scale(front, back));
                return true;
            }
            previous = value;
        }
        return false;
    };

    struct Candidate
    {
        LeFortProposedHole hole;
        double rankThickness = 0.0; // to a quarter of a millimetre: real bone is never exactly flat
        double lateralOffset = 0.0;
        double fromWindow = 0.0;
    };
    const double reach = std::max(0.0, params.lateralReachMm);
    const double step = std::max(0.25, params.sampleStepMm);
    static const LeFortPillar kPillars[] = {LeFortPillar::PillarRight, LeFortPillar::PiriformRight,
                                            LeFortPillar::PiriformLeft, LeFortPillar::PillarLeft};
    // The surgeon's holes first, where they were put, judged on the movement as it is now.
    std::vector<Vec3> chosen;
    for (LeFortProposedHole hole : manual) {
        hole.origin = LeFortHoleOrigin::Manual;
        hole.support = Support(hole.center, hole.axis, context);
        chosen.push_back(hole.center);
        proposal.holes.push_back(hole);
    }
    const auto manualCount = [&manual](LeFortPillar pillar, LeFortCutSide side) {
        return static_cast<int>(std::count_if(manual.begin(), manual.end(), [&](const LeFortProposedHole& hole) {
            return hole.pillar == pillar && hole.side == side;
        }));
    };
    for (size_t index = 0; index < 4; ++index) {
        const Vec3& anchor = context.cut.points[index];
        for (const LeFortCutSide side : {LeFortCutSide::Cranial, LeFortCutSide::Segment}) {
            const bool cranial = side == LeFortCutSide::Cranial;
            std::vector<Candidate> candidates;
            for (double u = -reach; u <= reach + 1e-9; u += step) {
                const Vec3 column = add(anchor, scale(lateral, u));
                // Where the window starts in this column: the cut, or the band's upper edge above it.
                const double base = cranial ? bandUnder(overCut(column, context.cut, rises, up)) : 0.0;
                for (double v = params.windowNearMm; v <= params.windowFarMm + 1e-9; v += step) {
                    const double height = cranial ? base + v : -v;
                    Vec3 site;
                    if (!onWall(add(add(column, scale(up, height)), scale(front, 15.0)), site))
                        continue;
                    // The window is checked again where the site actually landed.
                    const OverCut over = overCut(site, context.cut, rises, up);
                    const double fromCut = cranial ? over.aboveCutMm - bandUnder(over) : -over.aboveCutMm;
                    if (fromCut < params.windowNearMm - 1e-6 || fromCut > params.windowFarMm + 1e-6)
                        continue;
                    double distance = 0.0;
                    const PlateBone bone = context.preopBone(site, &distance);
                    if (bone != (cranial ? PlateBone::Cranial : PlateBone::Segment))
                        continue;
                    LeFortProposedHole hole;
                    hole.center = site;
                    hole.axis = PlateCore::BoneNormalAt(*context.bone, site, front);
                    hole.pillar = kPillars[index];
                    hole.side = side;
                    hole.origin = LeFortHoleOrigin::Auto;
                    hole.support = Support(hole.center, hole.axis, context);
                    if (hole.support.verdict != LeFortSupportVerdict::Ok)
                        continue;
                    candidates.push_back({hole, std::round(4.0 * hole.support.thicknessMm) / 4.0, std::abs(u),
                                          fromCut});
                }
            }
            // Thickest bone first; among equals the one nearest the pillar's own line, then nearest the cut.
            std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
                if (a.rankThickness != b.rankThickness)
                    return a.rankThickness > b.rankThickness;
                if (std::abs(a.lateralOffset - b.lateralOffset) > 1e-9)
                    return a.lateralOffset < b.lateralOffset;
                return a.fromWindow < b.fromWindow;
            });
            int placed = manualCount(kPillars[index], side);
            for (const Candidate& candidate : candidates) {
                if (placed >= params.holesPerSide)
                    break;
                bool apart = true;
                for (const Vec3& other : chosen)
                    apart = apart && norm(sub(candidate.hole.center, other)) >= params.pairSpacingMm;
                if (!apart)
                    continue;
                chosen.push_back(candidate.hole.center);
                proposal.holes.push_back(candidate.hole);
                ++placed;
            }
            if (placed < params.holesPerSide) {
                LeFortMissingHoles gap;
                gap.pillar = kPillars[index];
                gap.side = side;
                gap.missing = params.holesPerSide - placed;
                gap.reason = QStringLiteral("Faltan %1 orificio(s) en %2, %3: no hay hueso de al menos %4 mm lejos "
                                            "del borde, del corte%5.")
                                 .arg(gap.missing)
                                 .arg(PillarName(gap.pillar), SideName(gap.side))
                                 .arg(params.minThicknessMm, 0, 'f', 1)
                                 .arg(cranial && hasBand ? QStringLiteral(" y de la franja") : QString());
                proposal.missing.push_back(gap);
            }
        }
    }
    return proposal;
}

LeFortProposedHole MoveHole(const LeFortProposedHole& hole, const std::array<double, 3>& picked,
                            const LeFortHoleContext& context)
{
    LeFortProposedHole moved = hole;
    moved.origin = LeFortHoleOrigin::Manual;
    moved.center = picked;
    if (context.bone) {
        // Down the bone's own gradient onto its surface: the click lands on the guide, a few tenths to a few
        // millimetres off the bone.
        const ImplicitCore::BakedField& bone = *context.bone;
        const double h = std::max(0.05, bone.spacingMm);
        Vec3 p = picked;
        for (int iteration = 0; iteration < 12; ++iteration) {
            const double value = bone.At(p);
            if (std::abs(value) < 1e-3)
                break;
            const Vec3 gradient{(bone.At({p[0] + h, p[1], p[2]}) - bone.At({p[0] - h, p[1], p[2]})) / (2.0 * h),
                                (bone.At({p[0], p[1] + h, p[2]}) - bone.At({p[0], p[1] - h, p[2]})) / (2.0 * h),
                                (bone.At({p[0], p[1], p[2] + h}) - bone.At({p[0], p[1], p[2] - h})) / (2.0 * h)};
            const double g2 = dot(gradient, gradient);
            if (g2 < 1e-8)
                break;
            p = sub(p, scale(gradient, value / g2));
        }
        moved.center = p;
        moved.axis = PlateCore::BoneNormalAt(bone, p, unit(hole.axis, context.anterior));
    }
    if (context.preopBone) {
        double distance = 0.0;
        const PlateBone bone = context.preopBone(moved.center, &distance);
        if (bone == PlateBone::Cranial)
            moved.side = LeFortCutSide::Cranial;
        else if (bone == PlateBone::Segment)
            moved.side = LeFortCutSide::Segment;
    }
    moved.support = Support(moved.center, moved.axis, context);
    return moved;
}

std::vector<PredictiveHole> DrillSites(const std::vector<LeFortProposedHole>& holes)
{
    std::vector<PredictiveHole> sites;
    sites.reserve(holes.size());
    for (const LeFortProposedHole& hole : holes) {
        PredictiveHole site;
        site.bone = hole.side == LeFortCutSide::Cranial ? PlateBone::Cranial : PlateBone::Segment;
        site.preopCenter = hole.center;
        site.preopAxis = hole.axis;
        site.plannedCenter = hole.center;
        site.plannedAxis = hole.axis;
        sites.push_back(site);
    }
    return sites;
}

int UnmatchedPlateHoles(const std::vector<LeFortProposedHole>& guideHoles, const std::vector<PredictiveHole>& plateHoles,
                        double toleranceMm)
{
    int unmatched = 0;
    for (const PredictiveHole& plateHole : plateHoles) {
        const bool matched = std::any_of(guideHoles.begin(), guideHoles.end(), [&](const LeFortProposedHole& hole) {
            return norm(sub(hole.center, plateHole.preopCenter)) <= toleranceMm;
        });
        unmatched += matched ? 0 : 1;
    }
    return unmatched;
}

QString PillarName(LeFortPillar pillar)
{
    switch (pillar) {
    case LeFortPillar::PillarRight: return QStringLiteral("pilar cigomático derecho");
    case LeFortPillar::PiriformRight: return QStringLiteral("reborde piriforme derecho");
    case LeFortPillar::PiriformLeft: return QStringLiteral("reborde piriforme izquierdo");
    case LeFortPillar::PillarLeft: return QStringLiteral("pilar cigomático izquierdo");
    }
    return {};
}

QString SideName(LeFortCutSide side)
{
    return side == LeFortCutSide::Cranial ? QStringLiteral("encima del corte") : QStringLiteral("debajo del corte");
}
}
