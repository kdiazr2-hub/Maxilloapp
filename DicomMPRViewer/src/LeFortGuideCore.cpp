#include "LeFortGuideCore.h"

#include <vtkCellArray.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>

#include <algorithm>
#include <cmath>
#include <functional>
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
// Joins every painted patch to the largest one with a strap of dabs laid on the surface, and says how many it
// had to add. A pad round a drill sleeve that touches nothing else becomes a ring of guide floating in the air
// over the bone — the build reports the guide in several pieces and the surgeon gets a sleeve with no material
// under it (user's report, 2026-09-20: "que no queden espacios donde se perforó sin material").
int connectPaint(const ImplicitCore::BakedField& field, GuideBrushPaint& paint)
{
    if (paint.size() < 2)
        return 0;
    int added = 0;
    for (int pass = 0; pass < 8; ++pass) {
        // Components: two dabs hold together only where they really overlap, since the region is opened by a
        // millimetre before it is carved.
        std::vector<int> parent(paint.size());
        for (size_t i = 0; i < parent.size(); ++i)
            parent[i] = static_cast<int>(i);
        const std::function<int(int)> root = [&parent](int i) {
            while (parent[static_cast<size_t>(i)] != i)
                i = parent[static_cast<size_t>(i)] = parent[static_cast<size_t>(parent[static_cast<size_t>(i)])];
            return i;
        };
        for (size_t i = 0; i < paint.size(); ++i)
            for (size_t j = i + 1; j < paint.size(); ++j)
                if (norm(sub(paint[i].center, paint[j].center)) <= paint[i].radiusMm + paint[j].radiusMm - 0.5)
                    parent[static_cast<size_t>(root(static_cast<int>(i)))] = root(static_cast<int>(j));
        std::map<int, std::vector<size_t>> components;
        for (size_t i = 0; i < paint.size(); ++i)
            components[root(static_cast<int>(i))].push_back(i);
        if (components.size() < 2)
            break;
        // The largest patch is the band; everything else is joined to it, nearest points first.
        auto biggest = components.begin();
        for (auto it = components.begin(); it != components.end(); ++it)
            if (it->second.size() > biggest->second.size())
                biggest = it;
        const std::vector<size_t>& band = biggest->second;
        for (const auto& [key, members] : components) {
            if (key == biggest->first)
                continue;
            size_t from = members.front(), to = band.front();
            double best = 1e30;
            for (const size_t a : members)
                for (const size_t b : band) {
                    const double d = norm(sub(paint[a].center, paint[b].center));
                    if (d < best) {
                        best = d;
                        from = a;
                        to = b;
                    }
                }
            const double strap = std::max(2.5, 0.6 * std::min(paint[from].radiusMm, paint[to].radiusMm));
            const int steps = std::max(2, static_cast<int>(std::ceil(best / (0.8 * strap))));
            for (int k = 1; k < steps; ++k) {
                const Vec3 along = add(paint[from].center, scale(sub(paint[to].center, paint[from].center),
                                                                 static_cast<double>(k) / steps));
                paint.push_back({ontoSurface(field, along), strap, false});
                ++added;
            }
        }
    }
    return added;
}
} // namespace

namespace LeFortGuideCore
{
std::array<double, 3> AnteriorDirection(const ImplicitCore::BakedField& field, const OsteotomyPath& path)
{
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
    return front;
}

vtkSmartPointer<vtkPolyData> BandRibbon(const ImplicitCore::BakedField& bone, const OsteotomyPath& cut,
                                        const LeFortBandProfile& band, const std::array<double, 3>& anterior,
                                        double stepMm, double liftMm)
{
    auto ribbon = vtkSmartPointer<vtkPolyData>::New();
    const size_t count = std::min({cut.points.size(), band.heights.size(), band.upperCut.points.size()});
    if (band.spans.empty() || count < 2)
        return ribbon;
    // The band's vertical: the direction its upper edge was raised in.
    Vec3 up{0.0, 0.0, 1.0};
    for (size_t i = 0; i < count; ++i)
        if (std::abs(band.heights[i]) > 1e-6) {
            up = unit(scale(sub(band.upperCut.points[i], cut.points[i]), 1.0 / band.heights[i]), up);
            break;
        }
    const Vec3 front = unit(sub(anterior, scale(up, dot(anterior, up))), {0.0, -1.0, 0.0});
    // The rise at an arc length along the cut: linear along each straight piece.
    const auto riseAt = [&](double s) {
        double travelled = 0.0;
        for (size_t i = 1; i < count; ++i) {
            const double length = norm(sub(cut.points[i], cut.points[i - 1]));
            if (s <= travelled + length || i + 1 == count) {
                const double t = length > 1e-9 ? std::clamp((s - travelled) / length, 0.0, 1.0) : 0.0;
                return band.heights[i - 1] + t * (band.heights[i] - band.heights[i - 1]);
            }
            travelled += length;
        }
        return band.heights.back();
    };
    // From in front of the face back onto the wall, no further than 10 mm behind the point it started from:
    // through the aperture there is no wall, and what lies behind it is not where the band is cut.
    const double walk = std::clamp(0.5 * bone.spacingMm, 0.02, 0.1);
    const auto onWall = [&](const Vec3& point, Vec3& site) {
        const Vec3 start = add(point, scale(front, 15.0));
        double previous = bone.At(start);
        for (double t = walk; t <= 25.0; t += walk) {
            const Vec3 p = add(start, scale(front, -t));
            const double value = bone.At(p);
            if (previous >= 0.0 && value < 0.0) {
                const double back = walk * value / (value - previous);
                site = add(add(p, scale(front, back)), scale(front, liftMm));
                return true;
            }
            previous = value;
        }
        return false;
    };
    // At the cut itself the segmentation often has a gap (the kerf, or a thin seam). The wall is then taken a
    // little above or below and brought back to the height asked for, keeping its depth.
    const auto nearWall = [&](const Vec3& point, Vec3& site) {
        for (const double offset : {0.0, 0.5, -0.5, 1.0, -1.0, 1.5, -1.5})
            if (onWall(add(point, scale(up, offset)), site)) {
                site = sub(site, scale(up, offset));
                return true;
            }
        return false;
    };

    auto points = vtkSmartPointer<vtkPoints>::New();
    auto strips = vtkSmartPointer<vtkCellArray>::New();
    const double step = std::max(0.1, stepMm);
    for (const auto& [from, to] : band.spans) {
        vtkIdType lastLower = -1, lastUpper = -1;
        for (double s = from;; s = std::min(to, s + step)) {
            const Vec3 onCut = LeFortMotionCore::PointAlongCut(cut, s);
            Vec3 lower, upper;
            if (nearWall(onCut, lower) && nearWall(add(onCut, scale(up, riseAt(s))), upper)) {
                const vtkIdType l = points->InsertNextPoint(lower.data());
                const vtkIdType u = points->InsertNextPoint(upper.data());
                if (lastLower >= 0) {
                    const vtkIdType a[3] = {lastLower, l, u}, b[3] = {lastLower, u, lastUpper};
                    strips->InsertNextCell(3, a);
                    strips->InsertNextCell(3, b);
                }
                lastLower = l;
                lastUpper = u;
            } else {
                lastLower = lastUpper = -1; // no wall here: the strip breaks
            }
            if (s >= to)
                break;
        }
    }
    ribbon->SetPoints(points);
    ribbon->SetPolys(strips);
    return ribbon;
}

LeFortGuideLayout Layout(const GuidePreparation& preop, vtkPolyData* wrapMesh, const OsteotomyPath& path,
                         const std::vector<PredictiveHole>& holes, const LeFortGuideParams& params,
                         const LeFortBandProfile* impaction)
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

    // The guide's frame comes from the pre-operative osteotomy itself.
    Vec3 front = AnteriorDirection(field, path);
    Vec3 center{0.0, 0.0, 0.0};
    for (const Vec3& point : path.points)
        center = add(center, scale(point, 1.0 / path.points.size()));
    if (!holes.empty()) {
        // Only the centre: the band is laid out around the plates. The direction stays the osteotomy's own
        // sweep axis, checked against the bone above. Averaging the holes' drill axes tilted the whole frame
        // whenever the envelope gave a hole a skewed normal, and the band came out leaning (user's report,
        // 2026-09-21: "mal orientada").
        center = {0.0, 0.0, 0.0};
        for (const PredictiveHole& hole : holes)
            center = add(center, scale(hole.preopCenter, 1.0 / holes.size()));
    }
    Vec3 up = unit(sub(path.upAxis, scale(front, dot(path.upAxis, front))), {0.0, 0.0, 1.0});
    const Vec3 lateral = unit(cross(up, front), {1.0, 0.0, 0.0});
    const auto lateralOf = [&](const Vec3& p) { return dot(sub(p, center), lateral); };

    // The bone an impaction takes out: the band's height along the guide, sampled along the cut every quarter
    // millimetre (it is linear on each piece of the cut) and looked up by lateral position.
    struct BandSample
    {
        double lateral = 0.0;
        double height = 0.0;
    };
    std::vector<BandSample> bandSamples;
    if (impaction && !impaction->spans.empty() && impaction->heights.size() == path.points.size() &&
        impaction->upperCut.points.size() == path.points.size()) {
        for (size_t i = 1; i < path.points.size(); ++i) {
            const double length = norm(sub(path.points[i], path.points[i - 1]));
            const int steps = std::max(1, static_cast<int>(std::ceil(length / 0.25)));
            for (int k = (i == 1 ? 0 : 1); k <= steps; ++k) {
                const double t = static_cast<double>(k) / steps;
                const Vec3 p = add(path.points[i - 1], scale(sub(path.points[i], path.points[i - 1]), t));
                bandSamples.push_back({lateralOf(p), impaction->heights[i - 1] +
                                                         t * (impaction->heights[i] - impaction->heights[i - 1])});
            }
        }
    }
    const auto bandAt = [&](double s) {
        const BandSample* nearest = nullptr;
        for (const BandSample& sample : bandSamples)
            if (!nearest || std::abs(sample.lateral - s) < std::abs(nearest->lateral - s))
                nearest = &sample;
        return nearest && nearest->height >= params.bandThresholdMm ? nearest->height : 0.0;
    };
    double tallestBand = 0.0;
    for (const BandSample& sample : bandSamples)
        tallestBand = std::max(tallestBand, sample.height);
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
    const double reach = std::max(12.0, tallestBand + std::max(params.fixationOffsetMm, params.bandRadiusMm) + 2.0);
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
    // Over an impaction band the guide also has to hold the upper slit, with material past it: a second row of
    // dabs centred on the band's upper edge, where the band runs on the wall.
    if (!bandSamples.empty()) {
        for (const BandPoint& b : band) {
            const double rise = bandAt(b.lateral);
            if (!b.onCut || rise <= 0.0 || rise + params.bandMarginAboveMm <= bandRadius - 1.5)
                continue;
            const int bin = std::clamp(static_cast<int>(std::floor((b.lateral - lowest) / spacing)), 0, bins - 1);
            const auto found = byBin.find(bin);
            if (found == byBin.end())
                continue;
            const Candidate* pick = nullptr;
            for (const Candidate* c : found->second)
                if (std::abs(c->field - rise) < 1.0 && (!pick || c->depth > pick->depth))
                    pick = c;
            if (pick)
                layout.paint.push_back({pick->point, bandRadius, false});
        }
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
                bool placed = false;
                // From the end inwards, a bin at a time, until a spot clear of the holes turns up.
                for (int step = 0; step < bins && !placed; ++step) {
                    const int bin = endSide < 0 ? step : bins - 1 - step;
                    // Above an impaction band the cranial screws go above it: that bone is taken out.
                    const double wanted = sign * params.fixationOffsetMm +
                                          (sign > 0.0 ? bandAt(lowest + (bin + 0.5) * spacing) : 0.0);
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

    // Nothing painted may stand on its own: a sleeve's pad that touches no other is a ring floating over the
    // bone. This is also what makes the guide come out in one piece.
    const int connectors = connectPaint(field, layout.paint);

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
                // And as clear of the band's upper slit as of the Le Fort slit.
                const double rise = bandAt(s);
                if (rise > 0.0 && std::abs(pick->field - rise) < rowOffset)
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
            // The band's upper edge, inside the same piece (so it shares its bridges) and only where the band
            // reaches the threshold, to a quarter of a millimetre.
            if (!bandSamples.empty()) {
                const double sampleStep = 0.25;
                double runStart = 0.0;
                bool inRun = false;
                for (double u = pieceStart; u <= pieceEnd + 1e-9; u += sampleStep) {
                    const double at = std::min(u, pieceEnd);
                    const bool in = bandAt(at) > 0.0;
                    if (in && !inRun) {
                        runStart = at;
                        inRun = true;
                    }
                    const bool last = at >= pieceEnd - 1e-9;
                    if (inRun && (!in || last)) {
                        const double runEnd = in ? at : at - sampleStep;
                        if (runEnd - runStart >= 3.0) {
                            GuideSlot upper;
                            upper.path = impaction->upperCut;
                            upper.start = atLateral(runStart);
                            upper.end = atLateral(runEnd);
                            upper.hasExtent = true;
                            layout.slotPlan.push_back(upper);
                            ++layout.upperSlitPieces;
                        }
                        inRun = false;
                    }
                    if (last)
                        break;
                }
            }
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
    if (connectors > 0)
        layout.report += QStringLiteral(" Se añadieron %1 trazo(s) de unión para que ninguna camisa quede suelta.")
                             .arg(connectors);
    if (impaction && impaction->ok)
        layout.report += QStringLiteral(" ") + impaction->report +
                         (layout.upperSlitPieces > 0
                              ? QStringLiteral(" Segunda ranura por el borde superior de la franja: %1 tramo(s).")
                                    .arg(layout.upperSlitPieces)
                              : QString());
    layout.ok = true;
    return layout;
}
}
