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
    QString pathError;
    const auto cut = OsteotomyCore::PreparePathField(path, &pathError);
    if (!path.valid || !cut) {
        layout.error = pathError.isEmpty() ? QStringLiteral("Falta la trayectoria de la osteotomía Le Fort.") : pathError;
        return layout;
    }
    const ImplicitCore::BakedField& field = *preop.wrapField;

    // The guide's frame comes from the pre-operative osteotomy itself. If
    // predictive holes are supplied by an older plan, their drill axes can
    // refine the anterior direction, but they are not required.
    Vec3 front = unit(path.depthAxis, {0.0, 1.0, 0.0});
    // The sweep axis of an osteotomy plane has no intrinsic sign. LeFortPath
    // stores its points as pilar R, piriform R, piriform L, pilar L, so the
    // piriform midpoint gives us a stable anatomical anterior hint. Without
    // this check, an oriented CT whose depth axis points posteriorly produces
    // small guide islands on the zygoma and skull instead of a maxillary band.
    Vec3 surfaceFront{0.0, 0.0, 0.0};
    int surfaceNormals = 0;
    for (const Vec3& point : path.points) {
        const Vec3 onBone = ontoSurface(field, point);
        const Vec3 normal = GuideBaseCore::NormalAt(field, onBone);
        // Lateral components cancel between both sides; retain points whose
        // normal actually informs the antero-posterior sweep direction.
        if (std::abs(dot(normal, front)) > 0.25) {
            surfaceFront = add(surfaceFront, normal);
            ++surfaceNormals;
        }
    }
    if (surfaceNormals > 0 && norm(surfaceFront) > 0.25) {
        if (dot(front, surfaceFront) < 0.0)
            front = scale(front, -1.0);
    } else if (path.points.size() >= 4) {
        const Vec3 piriform = scale(add(path.points[1], path.points[2]), 0.5);
        const Vec3 pillars = scale(add(path.points.front(), path.points.back()), 0.5);
        const Vec3 anteriorHint = sub(piriform, pillars);
        if (dot(front, anteriorHint) < 0.0)
            front = scale(front, -1.0);
    }
    Vec3 center{0.0, 0.0, 0.0};
    for (const Vec3& point : path.points)
        center = add(center, scale(point, 1.0 / path.points.size()));
    if (!holes.empty()) {
        front = {0.0, 0.0, 0.0};
        center = {0.0, 0.0, 0.0};
        for (const PredictiveHole& hole : holes) {
            front = add(front, unit(hole.preopAxis));
            center = add(center, scale(hole.preopCenter, 1.0 / holes.size()));
        }
        front = unit(front, path.depthAxis);
    }
    Vec3 up = unit(sub(path.upAxis, scale(front, dot(path.upAxis, front))), {0.0, 0.0, 1.0});
    const Vec3 lateral = unit(cross(up, front), {1.0, 0.0, 0.0});
    const auto lateralOf = [&](const Vec3& p) { return dot(sub(p, center), lateral); };
    double pathDepthMin = 1e30, pathDepthMax = -1e30;
    for (const Vec3& point : path.points) {
        const double depth = dot(sub(point, center), front);
        pathDepthMin = std::min(pathDepthMin, depth);
        pathDepthMax = std::max(pathDepthMax, depth);
    }

    double lowest = 1e30, highest = -1e30;
    if (holes.empty()) {
        for (const Vec3& point : path.points) {
            lowest = std::min(lowest, lateralOf(point));
            highest = std::max(highest, lateralOf(point));
        }
    } else {
        for (const PredictiveHole& hole : holes) {
            lowest = std::min(lowest, lateralOf(hole.preopCenter));
            highest = std::max(highest, lateralOf(hole.preopCenter));
        }
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
        const double depth = dot(sub(p, center), front);
        // The surgical landmarks already delimit the relevant facial depth.
        // A full-skull envelope can contain another surface with the same cut
        // field tens of millimetres posteriorly; it must never be considered.
        constexpr double kDepthMarginMm = 12.0;
        if (depth < pathDepthMin - kDepthMarginMm || depth > pathDepthMax + kDepthMarginMm)
            continue;
        if (dot(GuideBaseCore::NormalAt(field, p), front) < 0.35)
            continue; // not the anterior wall: the nasal floor, the cut face, the back of the sinus
        candidates.push_back({p, f, s, depth});
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
    for (const BandPoint& b : band) {
        // Where the cut crosses the piriform aperture the band has to span open air. A full-width band there
        // became a slab over the nose; a strap is enough to hold the two halves together.
        layout.paint.push_back({b.point, b.onCut ? bandRadius : 0.75 * bandRadius, false});
    }

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
                layout.paint.push_back({ontoSurface(field, along), std::min(pad, bandRadius), false});
            }
        }
    }

    // The guide is screwed to the bone before anything is drilled or cut (Gander et al. 2015: "each was fixed
    // with two 1.5-mm-diameter standard osteosynthesis screws"; Ho et al. 2025: "temporarily fixed with two or
    // four monocortical positioning screws"). They go on the cranial side of the cut, which does not move, at
    // each end of the band and clear of the definitive plate holes; without plates the older layout of four
    // (above and below) is kept, since then nothing else holds the guide down.
    const auto clearOfHoles = [&](const Vec3& p) {
        for (const PredictiveHole& hole : holes)
            if (norm(sub(p, hole.preopCenter)) < params.minFixationToHoleMm)
                return false;
        for (const GuideFixationHole& other : layout.fixation)
            if (norm(sub(p, other.center)) < params.minFixationToHoleMm)
                return false;
        return true;
    };
    {
        // With plates, only the cranial side: a screw below the cut would be drilled into the piece that is
        // about to be mobilised, and its hole would travel away with the maxilla.
        const std::vector<double> sides = holes.empty() ? std::vector<double>{1.0, -1.0} : std::vector<double>{1.0};
        for (const int endSide : {-1, 1}) {
            for (const double sign : sides) {
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
    }

    // The cells. A row above and a row below the slit, staggered along the cut, each taken at the envelope
    // vertex nearest its place on the anterior wall and kept clear of the slit, of every sleeve and screw, and
    // of the rim. They are subtracted figures, so `GuideDesignCore` carves them with everything else and the
    // edit session cannot fill them in again.
    const double cellRadius = 0.5 * std::max(0.0, params.latticeCellMm);
    const double cellMargin = std::max(0.0, params.latticeMarginMm);
    const double rowOffset = std::max(0.0, params.latticeSlitClearMm) + cellRadius + cellMargin;
    if (cellRadius > 0.3 && rowOffset + cellRadius + cellMargin <= bandRadius) {
        const double cellStep = std::max(2.0 * cellRadius + 1.0, params.latticeSpacingMm);
        int row = 0;
        for (const double wanted : {rowOffset, -rowOffset}) {
            for (double s = lowest + 0.5 * cellStep + 0.5 * cellStep * row; s <= highest; s += cellStep) {
                const int bin = std::clamp(static_cast<int>(std::floor((s - lowest) / spacing)), 0, bins - 1);
                const auto found = byBin.find(bin);
                if (found == byBin.end())
                    continue;
                const Candidate* pick = nullptr;
                for (const Candidate* c : found->second)
                    if (std::abs(c->field - wanted) < 1.0 && (!pick || c->depth > pick->depth))
                        pick = c;
                if (!pick)
                    continue;
                // Clear of everything that must stay solid.
                bool clear = true;
                for (const PredictiveHole& hole : holes)
                    clear = clear && norm(sub(pick->point, hole.preopCenter)) >
                                         0.5 * params.sleeveOuterDiameterMm + cellRadius + cellMargin;
                for (const GuideFixationHole& screw : layout.fixation)
                    clear = clear && norm(sub(pick->point, screw.center)) >
                                         0.5 * screw.diameterMm + cellRadius + cellMargin + 1.5;
                if (!clear)
                    continue;
                // And well inside the band: the cell has to fall within one dab, with material left round it.
                bool inside = false;
                for (const GuideBrushStroke& dab : layout.paint)
                    inside = inside || (!dab.erase && norm(sub(pick->point, dab.center)) <=
                                                          dab.radiusMm - cellRadius - cellMargin);
                if (!inside)
                    continue;
                GuideFigure cell;
                cell.shape = GuideFigureShape::Cylinder;
                cell.operation = GuideFigureOperation::Subtract;
                cell.diameterMm = 2.0 * cellRadius;
                cell.lengthMm = 40.0; // through the wall either way
                cell.matrix = GuideDesignCore::FrameAt(pick->point, GuideBaseCore::NormalAt(field, pick->point));
                layout.figures.push_back(cell);
            }
            ++row;
        }
    }

    // The slit, in pieces between bridges: one at the midline (lateral 0 is the middle of the plates) and
    // every `bridgeSpacingMm` from it. The slit runs the whole length of the band, because the slab it is cut
    // with IS the planned osteotomy and `GuideDesignCore` clips it to the guide's own material: where there is
    // no guide there is no slit. Ending each piece at the last envelope vertex that happened to sit exactly on
    // the cut left a row of short stubs instead of a saw slit (user's report, 2026-09-20).
    const double halfBridge = 0.5 * std::max(0.5, params.bridgeWidthMm);
    const double period = std::max(4.0 * halfBridge, params.bridgeSpacingMm);
    const auto atLateral = [&](double s) { return add(center, scale(lateral, s)); };
    const int firstBridge = static_cast<int>(std::floor((lowest + 0.5 * period) / period));
    const int lastBridge = static_cast<int>(std::ceil((highest + 0.5 * period) / period));
    double pieceStart = lowest;
    for (int index = firstBridge; index <= lastBridge + 1; ++index) {
        // Each bridge is a `bridgeWidthMm` band of uncut guide centred on `index * period`; the midline
        // (lateral 0, index 0) always has one.
        const double bridgeAt = period * index;
        const double pieceEnd = std::min(highest, bridgeAt - halfBridge);
        if (pieceEnd - pieceStart >= 3.0) {
            GuideSlot slot;
            slot.path = path;
            slot.start = atLateral(pieceStart);
            slot.end = atLateral(pieceEnd);
            slot.hasExtent = true;
            layout.slotPlan.push_back(slot);
        }
        pieceStart = std::max(pieceStart, bridgeAt + halfBridge);
        if (pieceStart >= highest)
            break;
    }

    layout.report = QStringLiteral("Guía de corte Le Fort: banda de %1 mm sobre el corte (%2 tramo(s) por debajo de la "
                                   "apertura), %3 camisa(s), %4 tramo(s) de ranura con puentes, %5 tornillo(s) de "
                                   "fijación de %6 mm.")
                        .arg(highest - lowest, 0, 'f', 0)
                        .arg(layout.dippedBins)
                        .arg(holes.size())
                        .arg(layout.slotPlan.size())
                        .arg(layout.fixation.size())
                        .arg(params.fixationDiameterMm, 0, 'f', 1) +
                    QStringLiteral(" Celdas del entramado: %1.").arg(layout.figures.size());
    layout.ok = true;
    return layout;
}
}
