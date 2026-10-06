#include "LeFortGuideCore.h"
#include "GuideEngraveCore.h"

#include <vtkPolyData.h>
#include <vtkPointData.h>
#include <vtkDoubleArray.h>
#include <vtkClipPolyData.h>
#include <vtkAppendPolyData.h>
#include <vtkCleanPolyData.h>
#include <vtkPolyDataConnectivityFilter.h>

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

LeFortGuidePair SplitBySide(vtkPolyData* guides, const OsteotomyPath& path)
{
    LeFortGuidePair pair;
    if (!guides || guides->GetNumberOfPolys() == 0 || path.points.size() < 4)
        return pair;
    const Vec3 across = sub(path.points.back(), path.points.front());
    const Vec3 middle = scale(add(path.points[1], path.points[2]), 0.5);
    const double rightSide = dot(sub(path.points.front(), middle), across); // negative by construction
    auto connectivity = vtkSmartPointer<vtkPolyDataConnectivityFilter>::New();
    connectivity->SetInputData(guides);
    connectivity->SetExtractionModeToAllRegions();
    connectivity->ColorRegionsOn();
    connectivity->Update();
    const int regions = connectivity->GetNumberOfExtractedRegions();
    auto right = vtkSmartPointer<vtkAppendPolyData>::New();
    auto left = vtkSmartPointer<vtkAppendPolyData>::New();
    int rightCount = 0, leftCount = 0;
    for (int region = 0; region < regions; ++region) {
        auto one = vtkSmartPointer<vtkPolyDataConnectivityFilter>::New();
        one->SetInputData(guides);
        one->SetExtractionModeToSpecifiedRegions();
        one->AddSpecifiedRegion(region);
        auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
        clean->SetInputConnection(one->GetOutputPort());
        clean->Update();
        auto piece = vtkSmartPointer<vtkPolyData>::New();
        piece->DeepCopy(clean->GetOutput());
        if (piece->GetNumberOfPolys() == 0)
            continue;
        double b[6];
        piece->GetBounds(b);
        const Vec3 centre{0.5 * (b[0] + b[1]), 0.5 * (b[2] + b[3]), 0.5 * (b[4] + b[5])};
        const bool isRight = dot(sub(centre, middle), across) * rightSide > 0.0;
        (isRight ? right : left)->AddInputData(piece);
        (isRight ? rightCount : leftCount) += 1;
    }
    const auto finish = [](vtkAppendPolyData* append, int count) {
        auto out = vtkSmartPointer<vtkPolyData>::New();
        if (count > 0) {
            append->Update();
            out->DeepCopy(append->GetOutput());
        }
        return out;
    };
    pair.right = finish(right, rightCount);
    pair.left = finish(left, leftCount);
    return pair;
}

vtkSmartPointer<vtkPolyData> BandOnBone(vtkPolyData* bone, const OsteotomyPath& cut, const LeFortBandProfile& band,
                                        double thresholdMm)
{
    auto out = vtkSmartPointer<vtkPolyData>::New();
    if (!bone || bone->GetNumberOfPoints() == 0 || band.spans.empty())
        return out;
    const auto lower = OsteotomyCore::PreparePathField(cut);
    const auto upper = OsteotomyCore::PreparePathField(band.upperCut);
    if (!lower || !upper)
        return out;
    // Three half-spaces, each clipped on its own scalar: above the cut, below the upper edge, and where the two
    // are at least the threshold apart (the band's local height).
    auto current = vtkSmartPointer<vtkPolyData>::New();
    current->ShallowCopy(bone);
    const auto clipBy = [&](const char* name, const std::function<double(const Vec3&)>& value) {
        auto copy = vtkSmartPointer<vtkPolyData>::New();
        copy->ShallowCopy(current);
        auto scalars = vtkSmartPointer<vtkDoubleArray>::New();
        scalars->SetName(name);
        scalars->SetNumberOfTuples(copy->GetNumberOfPoints());
        for (vtkIdType i = 0; i < copy->GetNumberOfPoints(); ++i) {
            double p[3];
            copy->GetPoint(i, p);
            scalars->SetValue(i, value({p[0], p[1], p[2]}));
        }
        copy->GetPointData()->SetScalars(scalars);
        auto clip = vtkSmartPointer<vtkClipPolyData>::New();
        clip->SetInputData(copy);
        clip->SetValue(0.0); // keeps value >= 0
        clip->Update();
        current = vtkSmartPointer<vtkPolyData>::New();
        current->DeepCopy(clip->GetOutput());
    };
    clipBy("AboveCut", [&](const Vec3& p) { return OsteotomyCore::FieldAt(*lower, p); });
    if (current->GetNumberOfPolys() > 0)
        clipBy("BelowUpperEdge", [&](const Vec3& p) { return -OsteotomyCore::FieldAt(*upper, p); });
    if (current->GetNumberOfPolys() > 0)
        clipBy("BandHeight", [&](const Vec3& p) {
            return OsteotomyCore::FieldAt(*lower, p) - OsteotomyCore::FieldAt(*upper, p) - thresholdMm;
        });
    // Only by the cut: on each side's wall from the pillar to the piriform rim, within `reachMm` across the
    // vertical. The slabs extend past the cut and through the whole skull, so without this the band covered
    // whatever bone lay between them (user's case, 2026-10-05: red over the palate, the posterior maxilla and
    // up by the orbits). With the four Le Fort points the piece across the aperture is left out, and nothing
    // is drawn medial to a piriform rim.
    if (current->GetNumberOfPolys() > 0 && cut.points.size() >= 2) {
        const Vec3 up = unit({cut.upAxis[0], cut.upAxis[1], cut.upAxis[2]});
        const auto flat = [&](const Vec3& v) { return sub(v, scale(up, dot(v, up))); };
        struct Piece { Vec3 a, b; bool medialEndB; };
        std::vector<Piece> pieces;
        const auto at = [&](size_t i) { return Vec3{cut.points[i][0], cut.points[i][1], cut.points[i][2]}; };
        if (cut.points.size() == 4) {
            pieces.push_back({at(0), at(1), true});
            pieces.push_back({at(3), at(2), true});
        } else {
            for (size_t i = 0; i + 1 < cut.points.size(); ++i)
                pieces.push_back({at(i), at(i + 1), false});
        }
        constexpr double reachMm = 6.0;
        constexpr double medialSlackMm = 0.5;
        clipBy("NearCut", [&](const Vec3& p) {
            double best = -1e9;
            for (const Piece& piece : pieces) {
                const Vec3 a = flat(piece.a), b = flat(piece.b), q = flat(p);
                const Vec3 ab = sub(b, a);
                const double len = std::sqrt(dot(ab, ab));
                if (len < 1e-9)
                    continue;
                const double along = dot(sub(q, a), ab) / len; // mm from a towards b
                if (piece.medialEndB && along > len + medialSlackMm)
                    continue; // medial to the piriform rim: the aperture
                const double t = std::clamp(along / len, 0.0, 1.0);
                const Vec3 closest = add(a, scale(ab, t));
                const Vec3 d = sub(q, closest);
                best = std::max(best, reachMm - std::sqrt(dot(d, d)));
            }
            return best;
        });
    }
    current->GetPointData()->SetScalars(nullptr);
    return current;
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

    // The band runs along the whole cut, pillar to pillar, and further out where a hole lies beyond it. It used
    // to span only the holes, which was the cut while the holes were the plates' (at the pillars); with the few
    // sound sites a real maxilla gives, the guide shrank to a block round one hole (user's report, 2026-10-05).
    double lowest = 1e30, highest = -1e30;
    for (const Vec3& point : path.points) {
        lowest = std::min(lowest, lateralOf(point));
        highest = std::max(highest, lateralOf(point));
    }
    for (const PredictiveHole& hole : holes) {
        lowest = std::min(lowest, lateralOf(hole.preopCenter));
        highest = std::max(highest, lateralOf(hole.preopCenter));
    }
    lowest -= params.lateralMarginMm;
    highest += params.lateralMarginMm;

    // The anterior surface near the cut: envelope vertices facing forward, within the band's reach.
    // Far enough down for the bridge that crosses below the aperture and the nasal spine.
    const double reach = std::max({12.0, tallestBand + std::max(params.fixationOffsetMm, params.bandRadiusMm) + 2.0,
                                   params.spineClearanceMm + 2.0 * params.bandRadiusMm + 6.0});
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
    const double strapRadius = 0.75 * bandRadius;
    // The piriform aperture: the stretch between the last bins on the cut either side of the bins with no wall
    // on the cut. Nothing of the guide may reach into it (user's rule, 2026-10-05: "sin meterse en la nariz").
    bool hasAperture = false;
    double rimLow = 0.0, rimHigh = 0.0;
    // Only the gaps between the piriform points: a perforation in a lateral wall is also a stretch of the cut
    // with no wall, and taking every such bin made it part of the "nose" and cut the guide back to beyond it
    // (user's case, 2026-10-05: the left guide never reached the zygomatic pillar). A nasal spine can split the
    // aperture in two; both halves count.
    {
        std::vector<double> walls;
        for (const BandPoint& b : band)
            if (b.onCut)
                walls.push_back(b.lateral);
        std::sort(walls.begin(), walls.end());
        double innerLow = -1e30, innerHigh = 1e30;
        if (path.points.size() == 4) {
            innerLow = std::min(lateralOf(path.points[1]), lateralOf(path.points[2]));
            innerHigh = std::max(lateralOf(path.points[1]), lateralOf(path.points[2]));
        }
        for (size_t k = 1; k < walls.size(); ++k) {
            const double a = walls[k - 1], b = walls[k];
            const double mid = 0.5 * (a + b);
            // Inside the rims: its middle at least a millimetre in, and neither end more than 3 mm past a
            // piriform point (a missing bin right at a rim is not the nose).
            if (b - a <= 1.5 * spacing || mid < innerLow + 1.0 || mid > innerHigh - 1.0 || a < innerLow - 3.0 ||
                b > innerHigh + 3.0)
                continue;
            rimLow = hasAperture ? std::min(rimLow, a) : a;
            rimHigh = hasAperture ? std::max(rimHigh, b) : b;
            hasAperture = true;
        }
    }
    // Two guides without an aperture between them (the cut does not cross one): they part at the midline.
    if (params.separateSides && !hasAperture && path.points.size() == 4) {
        const double middle = 0.5 * (lateralOf(path.points[1]) + lateralOf(path.points[2]));
        hasAperture = true;
        rimLow = middle - 2.0;
        rimHigh = middle + 2.0;
    }
    // A dab beside the aperture is shrunk so it stops at the rim; one that would be a crumb is left out.
    const auto clearOfNose = [&](double lateral, double radius) {
        if (!hasAperture || lateral <= rimLow - radius || lateral >= rimHigh + radius)
            return radius;
        if (lateral > rimLow && lateral < rimHigh)
            return 0.0;
        return std::min(radius, (lateral <= rimLow ? rimLow - lateral : lateral - rimHigh) + 0.25);
    };
    for (const BandPoint& b : band) {
        if (!b.onCut)
            continue; // the aperture is crossed by the bridge below
        const double radius = clearOfNose(b.lateral, bandRadius);
        if (radius >= 1.5)
            layout.paint.push_back({b.point, radius, false});
    }
    // The bridge: across the aperture on the alveolar wall, its upper edge `spineClearanceMm` below the anterior
    // nasal spine — the forward spur at the aperture's floor — and carried a few bins past each rim so it
    // overlaps the band there. Without a spine it keeps that clearance below the floor itself.
    if (hasAperture && !params.separateSides) {
        // The wall below the aperture, slice by slice downwards from the cut: the most forward point of each.
        const double sliceMm = 0.5;
        std::map<int, double> forwardBySlice; // slice index (downwards) → most forward depth
        double floorField = -1e30;
        for (const Candidate& c : candidates) {
            if (c.lateral <= rimLow || c.lateral >= rimHigh || c.field >= -0.5)
                continue;
            floorField = std::max(floorField, c.field);
            const int slice = static_cast<int>(std::floor(-c.field / sliceMm));
            const auto it = forwardBySlice.find(slice);
            if (it == forwardBySlice.end() || c.depth > it->second)
                forwardBySlice[slice] = c.depth;
        }
        if (floorField > -1e29) {
            // The alveolar wall's own depth: the median of the slices' forward-most points. The spine is the run
            // of slices from the floor down that stand more than 1.5 mm in front of it.
            std::vector<double> depths;
            for (const auto& [slice, depth] : forwardBySlice)
                depths.push_back(depth);
            std::nth_element(depths.begin(), depths.begin() + depths.size() / 2, depths.end());
            const double wall = depths[depths.size() / 2];
            double spineBottom = floorField;
            for (const auto& [slice, depth] : forwardBySlice) {
                const double level = -slice * sliceMm;
                if (level > floorField + 1e-9)
                    continue;
                if (depth > wall + 1.5)
                    spineBottom = std::min(spineBottom, level - sliceMm);
                else if (level < spineBottom - sliceMm)
                    break; // past the spine: the plain wall
            }
            const double target = spineBottom - params.spineClearanceMm - strapRadius;
            const int reachBins = static_cast<int>(std::ceil(strapRadius / spacing)) + 1;
            for (int bin = 0; bin < bins; ++bin) {
                const double binLateral = lowest + (bin + 0.5) * spacing;
                if (binLateral < rimLow - reachBins * spacing || binLateral > rimHigh + reachBins * spacing)
                    continue;
                const auto found = byBin.find(bin);
                if (found == byBin.end())
                    continue;
                // Under the aperture the bridge runs at its level; beside it the bridge ramps up towards the band
                // so the two overlap, never reaching above the aperture's floor over the opening.
                const auto levelAt = [&](double lateral) {
                    const double outside = lateral < rimLow ? rimLow - lateral : lateral > rimHigh ? lateral - rimHigh : 0.0;
                    if (outside <= 0.0)
                        return target;
                    double level = -0.5 * strapRadius;
                    if (outside < strapRadius)
                        level = std::min(level, floorField - std::sqrt(strapRadius * strapRadius - outside * outside));
                    return std::max(level, target);
                };
                const Candidate* pick = nullptr;
                double pickLevel = 0.0;
                for (const Candidate* c : found->second) {
                    const double level = levelAt(c->lateral);
                    if (c->field > level + 0.25)
                        continue;
                    // The highest under its own level, then the most forward.
                    const double slack = level - c->field;
                    if (!pick || slack < pickLevel - 0.25 || (std::abs(slack - pickLevel) <= 0.25 && c->depth > pick->depth)) {
                        pick = c;
                        pickLevel = slack;
                    }
                }
                if (pick)
                    layout.paint.push_back({pick->point, strapRadius, false});
            }
        }
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
            const double radius = pick ? clearOfNose(b.lateral, bandRadius) : 0.0;
            if (radius >= 1.5)
                layout.paint.push_back({pick->point, radius, false});
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
        // Two guides: each also needs a screw at its inner end, by the piriform rim, clear of the opening.
        if (params.separateSides && hasAperture) {
            constexpr double kScrewPad = 3.5;
            for (const int inner : {-1, 1}) { // −1: the right guide's inner end, +1: the left guide's
                for (const double sign : sides) {
                    bool placed = false;
                    for (int step = 0; step < bins && !placed; ++step) {
                        const double s = inner < 0 ? rimLow - (kScrewPad + 0.5) - step * spacing
                                                   : rimHigh + (kScrewPad + 0.5) + step * spacing;
                        if (s < lowest || s > highest)
                            break;
                        const int bin = std::clamp(static_cast<int>(std::floor((s - lowest) / spacing)), 0, bins - 1);
                        const double wanted = sign * params.fixationOffsetMm + (sign > 0.0 ? bandAt(s) : 0.0);
                        const auto found = byBin.find(bin);
                        if (found == byBin.end())
                            continue;
                        const Candidate* pick = nullptr;
                        for (const Candidate* c : found->second)
                            if (std::abs(c->field - wanted) < 1.0 && (inner < 0 ? c->lateral <= s + 0.5 : c->lateral >= s - 0.5) &&
                                (!pick || c->depth > pick->depth))
                                pick = c;
                        if (!pick || !clearOfHoles(pick->point))
                            continue;
                        GuideFixationHole screw;
                        screw.center = pick->point;
                        screw.axis = GuideBaseCore::NormalAt(field, pick->point);
                        screw.diameterMm = params.fixationDiameterMm;
                        layout.fixation.push_back(screw);
                        layout.paint.push_back({pick->point, kScrewPad, false});
                        placed = true;
                    }
                }
            }
        }
    }

    // Nothing painted may stand on its own: a sleeve's pad that touches no other is a ring floating over the
    // bone. This is also what makes the guide come out in one piece.
    int connectors = 0;
    if (params.separateSides && hasAperture) {
        // Each guide on its own: patches are joined within a side, never across the midline.
        const double middle = 0.5 * (rimLow + rimHigh);
        GuideBrushPaint right, left;
        for (const GuideBrushStroke& dab : layout.paint)
            (lateralOf(dab.center) < middle ? right : left).push_back(dab);
        connectors = connectPaint(field, right) + connectPaint(field, left);
        layout.paint = right;
        layout.paint.insert(layout.paint.end(), left.begin(), left.end());
    } else {
        connectors = connectPaint(field, layout.paint);
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

    // The engraving (spec asistente-guia-lefort, after the user's printed guides): on each guide the case number
    // above its cranial positioning screws and DER / IZQ below the caudal ones, at the middle of the side, clear
    // of every screw hole and sleeve. Each label brings its own material — a strip of dabs under it, joined to
    // the band — as the printed guides are taller where the number is; the openwork cells under it go.
    if (params.separateSides && hasAperture && !params.caseLabel.trimmed().isEmpty()) {
        const double textHeight = std::max(1.0, params.labelHeightMm);
        const double pad = 0.5 * textHeight + 1.5;
        const Vec3 upAxis = unit(path.upAxis, {0.0, 0.0, 1.0});
        const Vec3 towardLeft = path.points.size() >= 2 ? sub(path.points.back(), path.points.front()) : lateral;
        const double middle = 0.5 * (rimLow + rimHigh);
        const bool lowIsRight = path.points.empty() || lateralOf(path.points.front()) < middle;
        const double sleeveBore = 0.5 * params.sleeveOuterDiameterMm;
        // How far a point is from the text's footprint (a segment along the reading direction).
        const auto offText = [](const Vec3& p, const Vec3& center, const Vec3& reading, double half) {
            const Vec3 d = sub(p, center);
            const double t = std::clamp(dot(d, reading), -half, half);
            return norm(sub(d, scale(reading, t)));
        };
        for (const bool lowSide : {true, false}) {
            const double low = lowSide ? lowest : rimHigh, high = lowSide ? rimLow : highest;
            const QString sideName = lowSide == lowIsRight ? QStringLiteral("DER") : QStringLiteral("IZQ");
            const std::pair<QString, bool> lines[] = {{params.caseLabel.trimmed(), true}, {sideName, false}};
            for (const auto& [text, cranial] : lines) {
                const double half = 0.5 * GuideEngraveCore::TextWidth(text, textHeight);
                const double mid = 0.5 * (low + high);
                bool placed = false;
                // Row by row away from the cut, and along each row from the middle of the side outwards.
                for (int row = 0; row <= 8 && !placed; ++row)
                for (int k = 0; k <= static_cast<int>(std::ceil(high - low)) && !placed; ++k) {
                    const double s = mid + ((k % 2) ? 1.0 : -1.0) * std::ceil(0.5 * k);
                    if (s - half < low || s + half > high)
                        continue;
                    const double beyond = params.fixationOffsetMm + pad + 0.5 + row;
                    const double wanted = cranial ? bandAt(s) + beyond : -beyond;
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
                    const Vec3 outward = GuideBaseCore::NormalAt(field, pick->point);
                    // Read left to right from in front of the face, upright.
                    Vec3 reading = unit(sub(towardLeft, scale(outward, dot(towardLeft, outward))), lateral);
                    const Vec3 upright{outward[1] * reading[2] - outward[2] * reading[1],
                                       outward[2] * reading[0] - outward[0] * reading[2],
                                       outward[0] * reading[1] - outward[1] * reading[0]};
                    if (dot(upright, upAxis) < 0.0)
                        reading = scale(reading, -1.0);
                    bool clear = true;
                    for (const GuideFixationHole& screw : layout.fixation)
                        clear = clear && offText(screw.center, pick->point, reading, half) >=
                                             0.5 * textHeight + 0.5 * screw.diameterMm + 1.0;
                    for (const PredictiveHole& hole : holes)
                        clear = clear && offText(hole.preopCenter, pick->point, reading, half) >= 0.5 * textHeight + sleeveBore + 1.0;
                    for (const LeFortGuideLabel& other : layout.labels)
                        clear = clear && norm(sub(other.center, pick->point)) >= half + 0.5 * other.widthMm + 1.0;
                    if (!clear)
                        continue;
                    const double cellClear = pad + cellRadius + 0.6;
                    layout.figures.erase(std::remove_if(layout.figures.begin(), layout.figures.end(),
                                                        [&](const GuideFigure& cell) {
                                                            if (cell.operation != GuideFigureOperation::Subtract)
                                                                return false;
                                                            const Vec3 c{cell.matrix[3], cell.matrix[7], cell.matrix[11]};
                                                            return offText(c, pick->point, reading, half) < cellClear;
                                                        }),
                                         layout.figures.end());
                    for (double t = -half - 1.0; t <= half + 1.0 + 1e-9; t += 1.5)
                        layout.paint.push_back({ontoSurface(field, add(pick->point, scale(reading, t))), pad, false});
                    // On the guide's outer face, not on the bone: inside the wall the letters would not show.
                    const Vec3 onFace = add(pick->point, scale(outward, std::max(0.0, params.labelWallMm)));
                    layout.figures.push_back(GuideEngraveCore::TextFigure(text, onFace, reading, outward, textHeight,
                                                                          params.labelReliefMm));
                    layout.labels.push_back({text, onFace, pick->point, reading, 2.0 * half});
                    placed = true;
                }
                if (!placed)
                    layout.report += QStringLiteral(" No cabe el texto «%1» en la guía %2.").arg(text, sideName);
            }
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
    // Only over the anterior and middle part of each guide: from the medial end (the piriform rim, or the
    // midline) the slits cover `slitLateralFraction` of the way to the lateral end, and the rest of the guide is
    // left whole, so it cannot come apart along its two slits (user's report, 2026-10-06). The surgeon finishes
    // the cut laterally along the same line.
    double slitLow = lowest, slitHigh = highest;
    {
        const double fraction = std::clamp(params.slitLateralFraction, 0.1, 1.0);
        const double medialLow = hasAperture ? rimLow : 0.0;
        const double medialHigh = hasAperture ? rimHigh : 0.0;
        if (lowest < medialLow)
            slitLow = medialLow - fraction * (medialLow - lowest);
        if (highest > medialHigh)
            slitHigh = medialHigh + fraction * (highest - medialHigh);
    }
    const int firstBridge = static_cast<int>(std::floor((slitLow + 0.5 * period) / period));
    const int lastBridge = static_cast<int>(std::ceil((slitHigh + 0.5 * period) / period));
    double pieceStart = slitLow;
    for (int index = firstBridge; index <= lastBridge + 1; ++index) {
        // Each bridge is a `bridgeWidthMm` band of uncut guide centred on `index * period`; the midline
        // (lateral 0, index 0) always has one.
        const double bridgeAt = period * index;
        const double pieceEnd = std::min(slitHigh, bridgeAt - halfBridge);
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
        if (pieceStart >= slitHigh)
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
