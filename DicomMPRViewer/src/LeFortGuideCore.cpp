#include "LeFortGuideCore.h"

#include <vtkPolyData.h>

#include <algorithm>
#include <cmath>
#include <map>

namespace
{
using Vec3 = std::array<double, 3>;

Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 scale(const Vec3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double norm(const Vec3& a) { return std::sqrt(dot(a, a)); }
Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
Vec3 unit(const Vec3& a, const Vec3& fallback = {0.0, 0.0, 1.0})
{
    const double length = norm(a);
    return length > 1e-12 ? scale(a, 1.0 / length) : fallback;
}

// A point of the envelope's anterior surface near the cut, in the guide's own frame.
struct Candidate
{
    Vec3 point;
    double field = 0.0;   // the osteotomy's signed field: 0 on the cut, negative on the segment
    double lateral = 0.0; // right ↔ left
    double depth = 0.0;   // how far forward: the anterior wall is the most forward surface
};

// Walks a point along the field's gradient onto its zero level: a point dropped onto the envelope.
Vec3 ontoSurface(const ImplicitCore::BakedField& field, Vec3 p)
{
    const double h = std::max(0.05, field.spacingMm);
    for (int iteration = 0; iteration < 8; ++iteration) {
        const double value = field.At(p);
        if (std::abs(value) < 1e-3)
            break;
        const Vec3 gradient{(field.At({p[0] + h, p[1], p[2]}) - field.At({p[0] - h, p[1], p[2]})) / (2.0 * h),
                            (field.At({p[0], p[1] + h, p[2]}) - field.At({p[0], p[1] - h, p[2]})) / (2.0 * h),
                            (field.At({p[0], p[1], p[2] + h}) - field.At({p[0], p[1], p[2] - h})) / (2.0 * h)};
        const double g2 = dot(gradient, gradient);
        if (g2 < 1e-8)
            break;
        p = sub(p, scale(gradient, value / g2));
    }
    return p;
}
} // namespace

namespace LeFortGuideCore
{
LeFortGuideLayout Layout(const GuidePreparation& preop, vtkPolyData* wrapMesh, const OsteotomyPath& path,
                         const std::vector<PredictiveHole>& holes, const LeFortGuideParams& params)
{
    LeFortGuideLayout layout;
    if (!preop.ok || !preop.wrapField || !wrapMesh || wrapMesh->GetNumberOfPoints() == 0) {
        layout.error = QStringLiteral("Calcule primero la envolvente del hueso antes del corte.");
        return layout;
    }
    if (holes.empty()) {
        layout.error = QStringLiteral("Cree primero las placas: la guía se construye alrededor de sus agujeros.");
        return layout;
    }
    QString pathError;
    const auto cut = OsteotomyCore::PreparePathField(path, &pathError);
    if (!path.valid || !cut) {
        layout.error = pathError.isEmpty() ? QStringLiteral("Falta la trayectoria de la osteotomía Le Fort.") : pathError;
        return layout;
    }
    const ImplicitCore::BakedField& field = *preop.wrapField;

    // The guide's frame: forward is where the drills come from (the sleeves' mean axis), up is the cranial
    // side of the cut, and lateral runs from one side of the maxilla to the other.
    Vec3 front{0.0, 0.0, 0.0}, center{0.0, 0.0, 0.0};
    for (const PredictiveHole& hole : holes) {
        front = add(front, unit(hole.preopAxis));
        center = add(center, scale(hole.preopCenter, 1.0 / holes.size()));
    }
    front = unit(front, {0.0, -1.0, 0.0});
    Vec3 up = unit(sub(path.upAxis, scale(front, dot(path.upAxis, front))), {0.0, 0.0, 1.0});
    const Vec3 lateral = unit(cross(up, front), {1.0, 0.0, 0.0});
    const auto lateralOf = [&](const Vec3& p) { return dot(sub(p, center), lateral); };

    double lowest = 1e30, highest = -1e30;
    for (const PredictiveHole& hole : holes) {
        lowest = std::min(lowest, lateralOf(hole.preopCenter));
        highest = std::max(highest, lateralOf(hole.preopCenter));
    }
    lowest -= params.lateralMarginMm;
    highest += params.lateralMarginMm;

    // The anterior surface near the cut: envelope vertices facing forward, within the band's reach.
    const double reach = std::max(12.0, params.fixationOffsetMm + 2.0);
    std::vector<Candidate> candidates;
    for (vtkIdType id = 0; id < wrapMesh->GetNumberOfPoints(); ++id) {
        double raw[3] = {};
        wrapMesh->GetPoint(id, raw);
        const Vec3 p{raw[0], raw[1], raw[2]};
        const double s = lateralOf(p);
        if (s < lowest - 1.0 || s > highest + 1.0)
            continue;
        const double f = OsteotomyCore::FieldAt(*cut, p);
        if (std::abs(f) > reach)
            continue;
        if (dot(GuideBaseCore::NormalAt(field, p), front) < 0.35)
            continue; // not the anterior wall: the nasal floor, the cut face, the back of the sinus
        candidates.push_back({p, f, s, dot(sub(p, center), front)});
    }
    if (candidates.empty()) {
        layout.error = QStringLiteral("No se encontró la pared anterior del maxilar alrededor del corte.");
        return layout;
    }

    // Bins along the cut, right to left. In each, the slit's point on the anterior wall; where the cut crosses
    // the aperture and has no anterior wall, the band dips to the alveolar wall just below it instead.
    const double spacing = std::max(0.5, params.dabSpacingMm);
    const int bins = std::max(1, static_cast<int>(std::ceil((highest - lowest) / spacing)));
    std::map<int, std::vector<const Candidate*>> byBin;
    for (const Candidate& c : candidates) {
        const int bin = std::clamp(static_cast<int>(std::floor((c.lateral - lowest) / spacing)), 0, bins - 1);
        byBin[bin].push_back(&c);
    }
    struct BandPoint
    {
        Vec3 point;
        double lateral = 0.0;
        bool onCut = false;
    };
    std::vector<BandPoint> band;
    for (int bin = 0; bin < bins; ++bin) {
        const auto found = byBin.find(bin);
        if (found == byBin.end())
            continue;
        const Candidate* onCut = nullptr;
        const Candidate* below = nullptr;
        for (const Candidate* c : found->second) {
            if (std::abs(c->field) < 0.75 && (!onCut || c->depth > onCut->depth))
                onCut = c;
            if (c->field < -1.0 && c->field > -reach && (!below || c->field > below->field + 0.25 ||
                                                         (std::abs(c->field - below->field) <= 0.25 && c->depth > below->depth)))
                below = c;
        }
        if (onCut) {
            band.push_back({onCut->point, onCut->lateral, true});
            layout.cutLine.push_back(onCut->point);
        } else if (below) {
            band.push_back({below->point, below->lateral, false});
            ++layout.dippedBins;
        }
    }
    if (band.empty() || layout.cutLine.empty()) {
        layout.error = QStringLiteral("La osteotomía no cruza la pared anterior entre los agujeros de las placas.");
        return layout;
    }

    const double bandRadius = std::max(1.0, params.bandRadiusMm);
    for (const BandPoint& b : band)
        layout.paint.push_back({b.point, bandRadius, false});

    // A pad round each predictive hole, and a stem joining it to the band if it is further than the two reach.
    const double pad = 0.5 * params.sleeveOuterDiameterMm + params.holePadMm;
    for (const PredictiveHole& hole : holes) {
        layout.paint.push_back({hole.preopCenter, pad, false});
        const BandPoint* nearest = nullptr;
        double best = 1e30;
        for (const BandPoint& b : band) {
            const double d = norm(sub(b.point, hole.preopCenter));
            if (d < best) {
                best = d;
                nearest = &b;
            }
        }
        if (nearest && best > pad + bandRadius - 1.5) {
            const int steps = static_cast<int>(std::ceil(best / spacing));
            for (int k = 1; k < steps; ++k) {
                const Vec3 along = add(hole.preopCenter, scale(sub(nearest->point, hole.preopCenter),
                                                               static_cast<double>(k) / steps));
                layout.paint.push_back({ontoSurface(field, along), 0.75 * pad, false});
            }
        }
    }

    // Four fixation screws: above and below the cut at each lateral end, clear of the predictive holes.
    const auto clearOfHoles = [&](const Vec3& p) {
        for (const PredictiveHole& hole : holes)
            if (norm(sub(p, hole.preopCenter)) < params.minFixationToHoleMm)
                return false;
        for (const GuideFixationHole& other : layout.fixation)
            if (norm(sub(p, other.center)) < params.minFixationToHoleMm)
                return false;
        return true;
    };
    for (const int endSide : {-1, 1}) {
        for (const double sign : {1.0, -1.0}) {
            const double wanted = sign * params.fixationOffsetMm;
            bool placed = false;
            // From the end inwards, a bin at a time, until a spot clear of the holes turns up.
            for (int step = 0; step < bins && !placed; ++step) {
                const int bin = endSide < 0 ? step : bins - 1 - step;
                const auto found = byBin.find(bin);
                if (found == byBin.end())
                    continue;
                const Candidate* pick = nullptr;
                for (const Candidate* c : found->second)
                    if (std::abs(c->field - wanted) < 1.0 && (!pick || c->depth > pick->depth))
                        pick = c;
                if (!pick || !clearOfHoles(pick->point))
                    continue;
                GuideFixationHole screw;
                screw.center = pick->point;
                screw.axis = GuideBaseCore::NormalAt(field, pick->point);
                screw.diameterMm = params.fixationDiameterMm;
                layout.fixation.push_back(screw);
                layout.paint.push_back({pick->point, 3.5, false});
                placed = true;
            }
        }
    }

    // The slit, in pieces between bridges: one at the midline (lateral 0 is the middle of the plates) and
    // every `bridgeSpacingMm` from it. Each piece is a slot limited to its two ends on the cut line.
    std::vector<BandPoint> onCut;
    for (const BandPoint& b : band)
        if (b.onCut)
            onCut.push_back(b);
    const double halfBridge = 0.5 * std::max(0.5, params.bridgeWidthMm);
    const double period = std::max(4.0 * halfBridge, params.bridgeSpacingMm);
    const auto bridgeIndex = [&](double s) { return static_cast<int>(std::floor((s + 0.5 * period) / period)); };
    const auto inBridge = [&](double s) {
        const double offset = s - period * bridgeIndex(s);
        return std::abs(offset) < halfBridge;
    };
    std::vector<BandPoint> run;
    const auto closeRun = [&] {
        if (run.size() >= 2 && std::abs(run.back().lateral - run.front().lateral) >= 3.0) {
            GuideSlot slot;
            slot.path = path;
            slot.start = run.front().point;
            slot.end = run.back().point;
            slot.hasExtent = true;
            layout.slotPlan.push_back(slot);
        }
        run.clear();
    };
    for (size_t i = 0; i < onCut.size(); ++i) {
        const BandPoint& b = onCut[i];
        // The slit is the planned cut itself; a bin without an envelope vertex right on it is not a break.
        // Only a real hole in the anterior wall (the aperture) or a bridge ends a piece.
        const bool gapBefore = i > 0 && (b.lateral - onCut[i - 1].lateral > std::max(6.0, 2.5 * spacing) ||
                                         bridgeIndex(b.lateral) != bridgeIndex(onCut[i - 1].lateral));
        if (gapBefore)
            closeRun();
        if (!inBridge(b.lateral))
            run.push_back(b);
        else
            closeRun();
    }
    closeRun();

    layout.report = QStringLiteral("Guía de corte Le Fort: banda de %1 mm sobre el corte (%2 tramo(s) por debajo de la "
                                   "apertura), %3 camisa(s), %4 tramo(s) de ranura con puentes, %5 tornillo(s) de "
                                   "fijación de %6 mm.")
                        .arg(highest - lowest, 0, 'f', 0)
                        .arg(layout.dippedBins)
                        .arg(holes.size())
                        .arg(layout.slotPlan.size())
                        .arg(layout.fixation.size())
                        .arg(params.fixationDiameterMm, 0, 'f', 1);
    layout.ok = true;
    return layout;
}
}
