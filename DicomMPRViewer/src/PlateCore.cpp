#include "PlateCore.h"

#include "GuideBaseCore.h"

#include <QJsonArray>

#include <vtkLandmarkTransform.h>
#include <vtkIdTypeArray.h>
#include <vtkMatrix4x4.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataConnectivityFilter.h>
#include <vtkStaticCellLocator.h>

#include <algorithm>
#include <cmath>
#include <set>


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

int shellsOf(vtkPolyData* mesh)
{
    if (!mesh || mesh->GetNumberOfPolys() == 0)
        return 0;
    auto connectivity = vtkSmartPointer<vtkPolyDataConnectivityFilter>::New();
    connectivity->SetInputData(mesh);
    connectivity->SetExtractionModeToAllRegions();
    connectivity->Update();
    return connectivity->GetNumberOfExtractedRegions();
}

// Walks a point along the field's gradient until the field reads `level`: a point on the bone surface lifted
// to a given height above it (or dropped back onto it).
Vec3 projectToLevel(const ImplicitCore::BakedField& field, Vec3 p, double level)
{
    const double h = std::max(0.05, field.spacingMm);
    for (int iteration = 0; iteration < 8; ++iteration) {
        const double value = field.At(p) - level;
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

struct PathSample
{
    Vec3 point;  // on the bone surface (or on the straight bridge)
    Vec3 normal; // outward: the plate lies from `point` outwards along it
    bool bridge = false; // part of a bar over the gap, not seated on bone
};

// The surface normal averaged over about a millimetre. The envelope of segmented bone is rough at the voxel
// scale: its raw gradient swings by tens of degrees between neighbouring points, which reads as an edge
// everywhere and stops a plate arm from following the bone.
Vec3 smoothNormal(const ImplicitCore::BakedField& field, const Vec3& p, const Vec3& fallback)
{
    const double h = std::max(1.0, 2.0 * field.spacingMm);
    const Vec3 gradient{field.At({p[0] + h, p[1], p[2]}) - field.At({p[0] - h, p[1], p[2]}),
                        field.At({p[0], p[1] + h, p[2]}) - field.At({p[0], p[1] - h, p[2]}),
                        field.At({p[0], p[1], p[2] + h}) - field.At({p[0], p[1], p[2] - h})};
    Vec3 normal = unit(gradient, fallback);
    // Complex lateral buttresses can put the sample between two nearby surfaces. Keep the field gradient in the
    // same outward hemisphere as the picked surface normal; an inverted normal puts the ribbon inside the bone
    // and leaves a hooked or open-looking arm after the bone is subtracted.
    if (dot(normal, fallback) < 0.0)
        normal = scale(normal, -1.0);
    return normal;
}

// Walks on the bone from `start` towards `target`, a millimetre at a time in the tangent plane, each step
// dropped back onto the surface. It stops where the plate could not lie any more: the bone under it changes
// (the gap at the cut), the surface turns sharply (the cut face, the rim of the gap, a notch), the step falls
// into a hollow or stops making progress (a corner). Walking the surface — rather than dropping points of the
// straight chord onto it — keeps a long arm on a curved maxilla instead of cutting through it.
std::vector<PathSample> walkOnBone(const ImplicitCore::BakedField& field, const PathSample& start, const Vec3& target,
                                   PlateBone expected, double clearance, const PlateBoneQuery& boneAt, double step)
{
    std::vector<PathSample> walked{start};
    const Vec3 direct = sub(target, start.point);
    const double directLength = norm(direct);
    const Vec3 directAxis = unit(direct, {0.0, 0.0, 1.0});
    const double corridorMm = std::max(2.5, 0.15 * directLength);
    const double cosEdge = std::cos(40.0 * 3.14159265358979323846 / 180.0); // from one step to the next
    const double cosBend = std::cos(60.0 * 3.14159265358979323846 / 180.0); // from the hole the walk left
    const int maxSteps = static_cast<int>(3.0 * norm(sub(target, start.point)) / step) + 4;
    for (int i = 0; i < maxSteps; ++i) {
        const PathSample current = walked.back();
        const Vec3 toTarget = sub(target, current.point);
        const double remaining = norm(toTarget);
        if (remaining < 1.5 * step)
            break;
        const Vec3 tangent = sub(toTarget, scale(current.normal, dot(toTarget, current.normal)));
        if (norm(tangent) < 1e-6)
            break;
        const Vec3 guess = add(current.point, scale(unit(tangent), step));
        const Vec3 p = projectToLevel(field, guess, clearance);
        const Vec3 n = smoothNormal(field, p, current.normal);
        const Vec3 fromStart = sub(p, start.point);
        const double alongDirect = dot(fromStart, directAxis);
        const double offDirect = norm(sub(fromStart, scale(directAxis, alongDirect)));
        // Near the zygomatic buttress a geodesic walk may turn around the lateral rim and return towards the
        // target from behind. Stop at that point and let the bridge span the corner instead of making a loop.
        if (offDirect > corridorMm || alongDirect < -0.25 * step || alongDirect > directLength + 0.25 * step)
            break;
        if (dot(n, current.normal) < cosEdge || dot(n, start.normal) < cosBend)
            break;
        if (norm(sub(p, guess)) > step) // the surface fell away under the step: a hollow or the gap
            break;
        if (dot(sub(p, current.point), unit(toTarget)) < 0.3 * step) // no progress: stuck on a corner
            break;
        if (boneAt) {
            double distance = 0.0;
            const PlateBone bone = boneAt(p, &distance);
            if (distance > 1.0 || (expected != PlateBone::Unknown && bone != expected))
                break;
        }
        walked.push_back({p, n});
    }
    return walked;
}

// The bridge over the osteotomy, pulled taut over `obstacle` — the bones plus the space the movement vacated.
// Where the maxilla was advanced, that space is solid to the band, so it climbs the step in one smooth ramp,
// the shape a patient-specific implant really has, instead of diving into the cut and standing in the way of
// the maxilla. It used to be drawn as a right-angled dogleg, which no surgeon would bend and which showed as a
// zig-zag on the patient (user's report, 2026-09-20). The band bends along its length only: pushed out of the
// obstacle in the arm's own plane, never sideways, so it does not wander off round a buttress.
std::vector<Vec3> tautBand(const ImplicitCore::BakedField* obstacle, const Vec3& from, const Vec3& to,
                           double clearance, const Vec3& widthAxis, double* riseMm)
{
    const double length = norm(sub(to, from));
    const int count = std::clamp(static_cast<int>(std::ceil(length / 0.4)), 2, 400);
    std::vector<Vec3> band;
    band.reserve(static_cast<size_t>(count) + 1);
    for (int i = 0; i <= count; ++i)
        band.push_back(add(from, scale(sub(to, from), static_cast<double>(i) / count)));
    if (riseMm)
        *riseMm = 0.0;
    if (!obstacle)
        return band;
    const double h = std::max(0.2, obstacle->spacingMm);
    const auto pushOut = [&] {
        for (size_t i = 1; i + 1 < band.size(); ++i) {
            const Vec3 p = band[i];
            const double d = obstacle->At(p);
            if (d >= clearance)
                continue;
            Vec3 gradient{obstacle->At({p[0] + h, p[1], p[2]}) - obstacle->At({p[0] - h, p[1], p[2]}),
                          obstacle->At({p[0], p[1] + h, p[2]}) - obstacle->At({p[0], p[1] - h, p[2]}),
                          obstacle->At({p[0], p[1], p[2] + h}) - obstacle->At({p[0], p[1], p[2] - h})};
            gradient = sub(gradient, scale(widthAxis, dot(gradient, widthAxis)));
            band[i] = add(p, scale(unit(gradient, {0.0, 0.0, 0.0}), clearance - d));
        }
    };
    for (int iteration = 0; iteration < 80; ++iteration) {
        pushOut();
        // Taut: every point drawn back towards the middle of its neighbours. Alternated with the push, this
        // settles on the shortest band that stays outside the bone — the shape a strip of titanium takes.
        const std::vector<Vec3> previous = band;
        for (size_t i = 1; i + 1 < band.size(); ++i) {
            const Vec3 middle = scale(add(previous[i - 1], previous[i + 1]), 0.5);
            band[i] = add(previous[i], scale(sub(middle, previous[i]), 0.5));
        }
    }
    pushOut(); // the smoothing runs last above: nothing may be left inside the bone
    if (riseMm) {
        const Vec3 chord = unit(sub(to, from), {0.0, 0.0, 1.0});
        for (const Vec3& p : band) {
            const Vec3 r = sub(p, from);
            *riseMm = std::max(*riseMm, norm(sub(r, scale(chord, dot(r, chord)))));
        }
    }
    return band;
}

// The path of one plate arm from hole `a` to hole `b`: on each hole's bone as far as it goes, and a straight
// bar across whatever is left between the two walks — the gap the movement opened at the osteotomy, the cut
// face — lifted over any corner of bone. Without this the plate dives into the gap and falls apart.
std::vector<PathSample> strutPath(const ImplicitCore::BakedField& field, const PathSample& a, const PathSample& b,
                                   PlateBone boneA, PlateBone boneB, double clearance, const PlateBoneQuery& boneAt,
                                   const ImplicitCore::BakedField* boneAndGap, double* bridgedMm, bool* steppedBridge)
{
    const double step = 1.0;
    if (bridgedMm)
        *bridgedMm = 0.0;
    if (steppedBridge)
        *steppedBridge = false;
    std::vector<PathSample> fromA = walkOnBone(field, a, b.point, boneA, clearance, boneAt, step);
    if (norm(sub(b.point, fromA.back().point)) < 1.5 * step) {
        fromA.push_back(b); // one surface all the way
        return fromA;
    }
    std::vector<PathSample> fromB = walkOnBone(field, b, fromA.back().point, boneB, clearance, boneAt, step);
    const PathSample endA = fromA.back();
    const PathSample endB = fromB.back();
    const double gap = norm(sub(endB.point, endA.point));
    std::vector<PathSample> path = fromA;
    if (gap >= 1.5 * step) {
        // The bridge: a flat bar, exactly as thick as the plate. Its width stays where the plate's width is —
        // across the arm, in the bone's plane — and its normal is square to that and to the bar. (Taking the
        // holes' normal and squaring it to the bar degenerates when a large advancement makes the bar run
        // forward, along that very normal: the bar turned on edge and curled over the corner.)
        const Vec3 along = unit(sub(endB.point, endA.point), unit(sub(b.point, a.point)));
        const Vec3 meanNormal = unit(add(a.normal, b.normal), a.normal);
        Vec3 widthAxis = cross(meanNormal, unit(sub(b.point, a.point), along));
        if (norm(widthAxis) < 1e-3)
            widthAxis = cross(meanNormal, along);
        widthAxis = unit(widthAxis, {1.0, 0.0, 0.0});
        Vec3 barNormal = unit(cross(along, widthAxis), meanNormal);
        if (dot(barNormal, meanNormal) < 0.0)
            barNormal = scale(barNormal, -1.0);
        // The band across: taut over the bones and over the space the movement vacated. It leaves the cranial
        // bone, ramps across the step the advancement opened and lands on the moved maxilla, in one smooth
        // curve — a shape a surgeon could bend, which the right-angled dogleg this replaces was not. Telling an
        // advancement from a flat span is no longer needed either: with nothing in the way, the taut band is
        // simply the straight line between the two walks.
        double rise = 0.0;
        const std::vector<Vec3> band = tautBand(boneAndGap, endA.point, endB.point, clearance + 0.15, widthAxis, &rise);
        const bool crossesOsteotomy = boneA != boneB && boneA != PlateBone::Unknown && boneB != PlateBone::Unknown;
        if (steppedBridge)
            *steppedBridge = crossesOsteotomy && rise > 0.75;
        // One corner per `step` of band, so its pieces are as long as the walked ones.
        std::vector<Vec3> corners{band.front()};
        for (size_t i = 1; i + 1 < band.size(); ++i)
            if (norm(sub(band[i], corners.back())) >= step)
                corners.push_back(band[i]);
        corners.push_back(band.back());

        for (size_t c = 0; c + 1 < corners.size(); ++c) {
            const Vec3 from = corners[c], to = corners[c + 1];
            // Each straight piece is flat across the plate's width, square to its own direction.
            Vec3 pieceNormal = unit(cross(unit(sub(to, from), along), widthAxis), barNormal);
            if (dot(pieceNormal, barNormal) < 0.0)
                pieceNormal = scale(pieceNormal, -1.0);
            const int pieces = std::max(1, static_cast<int>(std::ceil(norm(sub(to, from)) / step)));
            for (int k = (c == 0 ? 1 : 0); k < pieces; ++k)
                path.push_back({add(from, scale(sub(to, from), static_cast<double>(k) / pieces)), pieceNormal, true});
        }
        if (bridgedMm)
            *bridgedMm = gap;
    }
    for (auto it = fromB.rbegin(); it != fromB.rend(); ++it)
        if (norm(sub(it->point, path.back().point)) > 0.05)
            path.push_back(*it);
    return path;
}

// The bone's distance over `bounds`, Gaussian-smoothed over `sigmaMm`. A plate's outer face is laid on it, so
// it comes out as smooth as a machined plate; its inner face still follows the real bone. Segmented bone is rough
// at the voxel scale, and a plate built on it alone copied every bump.
std::shared_ptr<const ImplicitCore::BakedField> smoothedField(const ImplicitCore::BakedField& field, const double bounds[6],
                                                              double sigmaMm)
{
    const double h = std::max(0.2, field.spacingMm);
    const auto sampled = ImplicitCore::BakeFunction([&field](const Vec3& p) { return field.At(p); }, bounds, h,
                                                    2.0 * sigmaMm + 1.0);
    if (!sampled)
        return nullptr;
    auto out = std::make_shared<ImplicitCore::BakedField>(*sampled);
    const int radius = std::max(1, static_cast<int>(std::ceil(2.5 * sigmaMm / h)));
    std::vector<double> kernel(static_cast<size_t>(2 * radius + 1));
    double total = 0.0;
    for (int k = -radius; k <= radius; ++k) {
        kernel[static_cast<size_t>(k + radius)] = std::exp(-0.5 * (k * h) * (k * h) / (sigmaMm * sigmaMm));
        total += kernel[static_cast<size_t>(k + radius)];
    }
    for (double& w : kernel)
        w /= total;
    const auto& dims = out->dims;
    std::vector<float> temp(out->values.size());
    const auto index = [&dims](int i, int j, int k) {
        return (static_cast<size_t>(k) * static_cast<size_t>(dims[1]) + static_cast<size_t>(j)) *
                   static_cast<size_t>(dims[0]) +
               static_cast<size_t>(i);
    };
    for (int axis = 0; axis < 3; ++axis) {
        for (int k = 0; k < dims[2]; ++k)
            for (int j = 0; j < dims[1]; ++j)
                for (int i = 0; i < dims[0]; ++i) {
                    double sum = 0.0;
                    for (int o = -radius; o <= radius; ++o) {
                        int c[3] = {i, j, k};
                        c[axis] = std::clamp(c[axis] + o, 0, dims[static_cast<size_t>(axis)] - 1);
                        sum += kernel[static_cast<size_t>(o + radius)] * out->values[index(c[0], c[1], c[2])];
                    }
                    temp[index(i, j, k)] = static_cast<float>(sum);
                }
        out->values.swap(temp);
    }
    return out;
}

QJsonArray vecToJson(const Vec3& v) { return QJsonArray{v[0], v[1], v[2]}; }
Vec3 vecFromJson(const QJsonValue& value, const Vec3& fallback)
{
    const QJsonArray array = value.toArray();
    if (array.size() != 3)
        return fallback;
    return {array[0].toDouble(), array[1].toDouble(), array[2].toDouble()};
}

QString boneName(PlateBone bone)
{
    switch (bone) {
    case PlateBone::Cranial: return QStringLiteral("cráneo");
    case PlateBone::Segment: return QStringLiteral("segmento Le Fort");
    default: return QStringLiteral("hueso sin asignar");
    }
}
} // namespace

namespace PlateCore
{
// ── Motion ────────────────────────────────────────────────────────────────────
bool RigidMotion(vtkPolyData* before, vtkPolyData* after, std::array<double, 16>& matrix, double* rmsMm,
                 QString* error, double toleranceMm)
{
    const auto fail = [&](const QString& message) {
        if (error)
            *error = message;
        return false;
    };
    if (!before || !after || before->GetNumberOfPoints() < 4)
        return fail(QStringLiteral("Falta el segmento Le Fort antes o después de la reposición."));
    const vtkIdType count = before->GetNumberOfPoints();
    if (after->GetNumberOfPoints() != count)
        return fail(QStringLiteral("El segmento Le Fort cambió de malla entre la osteotomía y la reposición: "
                                   "no se puede medir su movimiento."));
    // A few thousand vertices in correspondence are plenty for an exact rigid fit.
    const vtkIdType step = std::max<vtkIdType>(1, count / 4000);
    auto source = vtkSmartPointer<vtkPoints>::New();
    auto target = vtkSmartPointer<vtkPoints>::New();
    for (vtkIdType id = 0; id < count; id += step) {
        source->InsertNextPoint(before->GetPoint(id));
        target->InsertNextPoint(after->GetPoint(id));
    }
    auto landmark = vtkSmartPointer<vtkLandmarkTransform>::New();
    landmark->SetSourceLandmarks(source);
    landmark->SetTargetLandmarks(target);
    landmark->SetModeToRigidBody();
    landmark->Update();
    vtkMatrix4x4* fitted = landmark->GetMatrix();
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            matrix[static_cast<size_t>(4 * r + c)] = fitted->GetElement(r, c);

    double sum = 0.0;
    for (vtkIdType i = 0; i < source->GetNumberOfPoints(); ++i) {
        double p[3] = {};
        source->GetPoint(i, p);
        double q[3] = {};
        target->GetPoint(i, q);
        const Vec3 moved = TransformPoint(matrix, {p[0], p[1], p[2]});
        const Vec3 d = sub(moved, {q[0], q[1], q[2]});
        sum += dot(d, d);
    }
    const double rms = std::sqrt(sum / static_cast<double>(std::max<vtkIdType>(1, source->GetNumberOfPoints())));
    if (rmsMm)
        *rmsMm = rms;
    if (rms > toleranceMm)
        return fail(QStringLiteral("El segmento Le Fort no se movió como un cuerpo rígido (residuo %1 mm).")
                        .arg(rms, 0, 'f', 3));
    return true;
}

std::array<double, 16> Invert(const std::array<double, 16>& matrix)
{
    auto m = vtkSmartPointer<vtkMatrix4x4>::New();
    m->DeepCopy(matrix.data());
    m->Invert();
    std::array<double, 16> out{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[static_cast<size_t>(4 * r + c)] = m->GetElement(r, c);
    return out;
}

std::array<double, 3> TransformPoint(const std::array<double, 16>& m, const std::array<double, 3>& p)
{
    return {m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3], m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7],
            m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]};
}

std::array<double, 3> TransformVector(const std::array<double, 16>& m, const std::array<double, 3>& v)
{
    return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[4] * v[0] + m[5] * v[1] + m[6] * v[2],
            m[8] * v[0] + m[9] * v[1] + m[10] * v[2]};
}

// ── Holes ─────────────────────────────────────────────────────────────────────
void AssignBones(std::vector<PlateDesign>& plates, vtkPolyData* cranialPlanned, vtkPolyData* segmentPlanned)
{
    const auto locatorFor = [](vtkPolyData* mesh) -> vtkSmartPointer<vtkStaticCellLocator> {
        if (!mesh || mesh->GetNumberOfCells() == 0)
            return nullptr;
        auto locator = vtkSmartPointer<vtkStaticCellLocator>::New();
        locator->SetDataSet(mesh);
        locator->BuildLocator();
        return locator;
    };
    const auto cranial = locatorFor(cranialPlanned);
    const auto segment = locatorFor(segmentPlanned);
    const auto distanceTo = [](vtkStaticCellLocator* locator, const Vec3& p) {
        if (!locator)
            return 1.0e30;
        double closest[3] = {};
        vtkIdType cell = -1;
        int subId = 0;
        double d2 = 0.0;
        locator->FindClosestPoint(p.data(), closest, cell, subId, d2);
        return std::sqrt(d2);
    };
    for (PlateDesign& plate : plates)
        for (PlateHole& hole : plate.holes) {
            const double toCranial = distanceTo(cranial, hole.center);
            const double toSegment = distanceTo(segment, hole.center);
            hole.bone = std::min(toCranial, toSegment) > 3.0 ? PlateBone::Unknown // not on either bone
                        : toSegment < toCranial              ? PlateBone::Segment
                                                             : PlateBone::Cranial;
        }
}

std::vector<std::vector<int>> TemplateStruts(PlateTemplate kind, int firstArmHoles, int secondArmHoles)
{
    std::vector<std::vector<int>> struts;
    std::vector<int> first;
    for (int i = 0; i < firstArmHoles; ++i)
        first.push_back(i);
    if (first.size() >= 2)
        struts.push_back(first);
    if (kind != PlateTemplate::LShape || secondArmHoles < 1)
        return struts;
    std::vector<int> second;
    for (int i = 0; i < secondArmHoles; ++i)
        second.push_back(firstArmHoles + i);
    if (second.size() >= 2)
        struts.push_back(second);
    // The bar joins the lowest hole of each arm (they are clicked top to bottom), below the cut.
    if (firstArmHoles >= 1)
        struts.push_back({firstArmHoles - 1, firstArmHoles + secondArmHoles - 1});
    return struts;
}

std::vector<std::vector<int>> SplintlessStruts(const std::array<int, 4>& pillarHoles)
{
    std::vector<std::vector<int>> struts;
    std::array<int, 4> first{};
    int offset = 0;
    for (size_t pillar = 0; pillar < pillarHoles.size(); ++pillar) {
        first[pillar] = offset;
        std::vector<int> arm;
        for (int i = 0; i < std::max(0, pillarHoles[pillar]); ++i)
            arm.push_back(offset + i);
        if (arm.size() >= 2)
            struts.push_back(arm);
        offset += std::max(0, pillarHoles[pillar]);
    }
    if (std::all_of(pillarHoles.begin(), pillarHoles.end(), [](int count) { return count > 0; })) {
        // Along the lower Le Fort border: lateral right -> nasal right -> nasal left -> lateral left.
        struts.push_back({first[1] + pillarHoles[1] - 1, first[0] + pillarHoles[0] - 1,
                          first[2] + pillarHoles[2] - 1, first[3] + pillarHoles[3] - 1});
    }
    return struts;
}

std::vector<PredictiveHole> PredictHoles(const std::vector<PlateDesign>& plates,
                                         const std::array<double, 16>& segmentMotion, const OsteotomyPath& path,
                                         const std::array<double, 3>& segmentProbe)
{
    std::vector<PredictiveHole> out;
    const std::array<double, 16> back = Invert(segmentMotion); // planned → pre-operative
    const bool hasPath = path.valid;
    const double segmentSide = hasPath ? OsteotomyCore::PathField(path, segmentProbe) : 0.0;
    for (size_t p = 0; p < plates.size(); ++p)
        for (size_t h = 0; h < plates[p].holes.size(); ++h) {
            const PlateHole& hole = plates[p].holes[h];
            PredictiveHole predicted;
            predicted.plate = static_cast<int>(p);
            predicted.hole = static_cast<int>(h);
            predicted.bone = hole.bone;
            predicted.plannedCenter = hole.center;
            predicted.plannedAxis = unit(hole.axis);
            // The cranial base does not move; the segment goes back to where it is before the cut.
            if (hole.bone == PlateBone::Segment) {
                predicted.preopCenter = TransformPoint(back, hole.center);
                predicted.preopAxis = unit(TransformVector(back, hole.axis));
            } else {
                predicted.preopCenter = hole.center;
                predicted.preopAxis = predicted.plannedAxis;
            }
            predicted.cutDistanceMm = -1.0; // unknown until there is a path
            if (hasPath) {
                const double field = OsteotomyCore::PathField(path, predicted.preopCenter);
                predicted.cutDistanceMm = std::abs(field);
                if (hole.bone != PlateBone::Unknown && std::abs(segmentSide) > 1e-9 && std::abs(field) > 1e-9) {
                    const bool onSegmentSide = (field > 0.0) == (segmentSide > 0.0);
                    predicted.wrongSide = (hole.bone == PlateBone::Segment) != onSegmentSide;
                }
            }
            out.push_back(predicted);
        }
    return out;
}

PlateCheck Check(const std::vector<PlateDesign>& plates, const std::vector<PredictiveHole>& holes,
                 const PlateParams& params)
{
    PlateCheck check;
    for (size_t p = 0; p < plates.size(); ++p) {
        const PlateDesign& plate = plates[p];
        const QString name = plate.name.isEmpty() ? QStringLiteral("Placa %1").arg(p + 1) : plate.name;
        int cranial = 0, segment = 0;
        for (const PlateHole& hole : plate.holes) {
            cranial += hole.bone == PlateBone::Cranial ? 1 : 0;
            segment += hole.bone == PlateBone::Segment ? 1 : 0;
        }
        if (cranial < params.minScrewsPerBone)
            check.warnings << QStringLiteral("%1: %2 tornillo(s) en el cráneo (mínimo %3).")
                                  .arg(name).arg(cranial).arg(params.minScrewsPerBone);
        if (segment < params.minScrewsPerBone)
            check.warnings << QStringLiteral("%1: %2 tornillo(s) en el segmento Le Fort (mínimo %3).")
                                  .arg(name).arg(segment).arg(params.minScrewsPerBone);
        if (plate.struts.empty())
            check.warnings << QStringLiteral("%1: no tiene brazos que unan sus agujeros.").arg(name);
        // Holes closer than a ring would merge their rings and leave no bone bridge between the screws.
        for (size_t a = 0; a < plate.holes.size(); ++a)
            for (size_t b = a + 1; b < plate.holes.size(); ++b)
                if (norm(sub(plate.holes[a].center, plate.holes[b].center)) < params.ringDiameterMm)
                    check.warnings << QStringLiteral("%1: los agujeros %2 y %3 están a menos de %4 mm.")
                                          .arg(name).arg(a + 1).arg(b + 1).arg(params.ringDiameterMm, 0, 'f', 1);
    }
    if (std::any_of(holes.begin(), holes.end(), [](const PredictiveHole& hole) { return hole.cutDistanceMm < 0.0; }))
        check.warnings << QStringLiteral("Falta la trayectoria de la osteotomía Le Fort: no se puede medir la "
                                         "distancia de los agujeros al corte ni generar la guía.");
    for (const PredictiveHole& hole : holes) {
        const QString where = QStringLiteral("Placa %1, agujero %2").arg(hole.plate + 1).arg(hole.hole + 1);
        if (hole.bone == PlateBone::Unknown)
            check.warnings << QStringLiteral("%1: no está sobre el cráneo ni sobre el segmento.").arg(where);
        if (hole.bone != PlateBone::Unknown && hole.cutDistanceMm >= 0.0 && hole.cutDistanceMm < params.minCutDistanceMm)
            check.warnings << QStringLiteral("%1: a %2 mm de la osteotomía (mínimo %3 mm).")
                                  .arg(where)
                                  .arg(hole.cutDistanceMm, 0, 'f', 1)
                                  .arg(params.minCutDistanceMm, 0, 'f', 1);
        if (hole.wrongSide)
            check.warnings << QStringLiteral("%1: está sobre el %2 pero al otro lado de la osteotomía.")
                                  .arg(where, boneName(hole.bone));
    }
    return check;
}

std::vector<GuideFigure> SleeveFigures(const std::vector<PredictiveHole>& holes, const SleeveParams& params)
{
    std::vector<GuideFigure> figures;
    // The body starts half a millimetre above the bone: along the surface normal the bone under a 4 mm disc
    // sags a few hundredths, so it never enters the bone, and the guide base below it holds it.
    const double start = 0.5;
    const double height = std::max(start + 0.5, params.heightMm);
    for (const PredictiveHole& hole : holes) {
        const Vec3 axis = unit(hole.preopAxis);
        GuideFigure body;
        body.shape = GuideFigureShape::Cylinder;
        body.operation = GuideFigureOperation::Add;
        body.diameterMm = params.outerDiameterMm;
        body.lengthMm = height - start;
        body.matrix = GuideDesignCore::FrameAt(add(hole.preopCenter, scale(axis, 0.5 * (start + height))), axis);
        figures.push_back(body);

        GuideFigure bore;
        bore.shape = GuideFigureShape::Cylinder;
        bore.operation = GuideFigureOperation::Subtract;
        bore.diameterMm = params.boreDiameterMm;
        bore.lengthMm = 2.0 * height + 20.0; // through the sleeve and the whole guide wall
        bore.matrix = GuideDesignCore::FrameAt(hole.preopCenter, axis);
        figures.push_back(bore);
    }
    return figures;
}

PlateBoneQuery MakeBoneQuery(vtkPolyData* cranialPlanned, vtkPolyData* segmentPlanned,
                             const std::array<double, 16>& segmentMotion, const OsteotomyPath& path,
                             double cutMarginMm)
{
    std::vector<std::pair<PlateBone, vtkSmartPointer<vtkStaticCellLocator>>> locators;
    for (const auto& [bone, mesh] : {std::pair{PlateBone::Cranial, cranialPlanned}, std::pair{PlateBone::Segment, segmentPlanned}}) {
        if (!mesh || mesh->GetNumberOfCells() == 0)
            continue;
        auto locator = vtkSmartPointer<vtkStaticCellLocator>::New();
        locator->SetDataSet(mesh);
        locator->BuildLocator();
        locators.emplace_back(bone, locator);
    }
    const auto cut = path.valid ? OsteotomyCore::PreparePathField(path) : nullptr;
    const std::array<double, 16> back = Invert(segmentMotion);
    // Which side of the cut the segment is on: the field is negative on the caudal side by construction.
    return [locators, cut, back, cutMarginMm](const std::array<double, 3>& p, double* distanceMm) {
        double best = 1.0e30;
        PlateBone nearest = PlateBone::Unknown;
        for (const auto& [bone, locator] : locators) {
            double closest[3] = {};
            vtkIdType cell = -1;
            int subId = 0;
            double d2 = 0.0;
            locator->FindClosestPoint(p.data(), closest, cell, subId, d2);
            if (std::sqrt(d2) < best) {
                best = std::sqrt(d2);
                nearest = bone;
            }
        }
        if (distanceMm)
            *distanceMm = best;
        if (cut && nearest != PlateBone::Unknown) {
            const double field =
                OsteotomyCore::FieldAt(*cut, nearest == PlateBone::Segment ? TransformPoint(back, p) : p);
            const bool onItsSide = nearest == PlateBone::Segment ? field < 0.0 : field > 0.0;
            if (!onItsSide || std::abs(field) < cutMarginMm)
                return PlateBone::Unknown; // the cut face or its edge: no seat for a plate
        }
        return nearest;
    };
}

// ── Where a screw may go ──────────────────────────────────────────────────────
std::array<double, 3> BoneNormalAt(const ImplicitCore::BakedField& bone, const Vec3& point, const Vec3& hint)
{
    const double h = std::max(0.5, 2.0 * bone.spacingMm); // averaged over about a millimetre
    const Vec3 gradient{bone.At({point[0] + h, point[1], point[2]}) - bone.At({point[0] - h, point[1], point[2]}),
                        bone.At({point[0], point[1] + h, point[2]}) - bone.At({point[0], point[1] - h, point[2]}),
                        bone.At({point[0], point[1], point[2] + h}) - bone.At({point[0], point[1], point[2] - h})};
    Vec3 normal = unit(gradient, unit(hint, {0.0, 0.0, 1.0}));
    if (dot(normal, unit(hint, normal)) < 0.0)
        normal = scale(normal, -1.0); // outward, the side the hint came from
    return normal;
}


HoleSeat CheckHoleSeat(const Vec3& center, const Vec3& axis, const PlateBoneQuery& plannedBone,
                       const PlateBoneQuery& preopBone, const OsteotomyPath& path,
                       const std::array<double, 16>& segmentMotion, const PlateParams& params)
{
    HoleSeat seat;
    if (!plannedBone) {
        seat.ok = true; // nothing to check against
        return seat;
    }
    double distance = 0.0;
    const PlateBone bone = plannedBone(center, &distance);
    if (bone == PlateBone::Unknown || distance > 1.0) {
        seat.reason = QStringLiteral("Ahí no hay hueso: marque sobre la base craneal o sobre el Le Fort.");
        return seat;
    }
    // Everything else is judged where the drill actually goes: on the bone before the cut.
    const std::array<double, 16> back = Invert(segmentMotion);
    const Vec3 preopCenter = bone == PlateBone::Segment ? TransformPoint(back, center) : center;
    const Vec3 preopAxis = bone == PlateBone::Segment ? TransformVector(back, axis) : axis;

    // Bone all round the ring, in the surface's tangent plane. A hole on a free margin — the rim of the
    // piriform aperture, the lower border of the fragment, the edge of the segmentation — leaves the screw
    // with nothing to hold, and the guide's sleeve standing on air.
    if (preopBone) {
        const double ring = 0.5 * std::max(params.ringDiameterMm, params.holeDiameterMm + 1.0);
        const double reach = ring + std::max(0.0, params.minEdgeDistanceMm);
        constexpr double kOnBoneMm = 1.5; // the rim of a screw head this far off the surface is still carried
        // A normal we can trust: the direction in which the distance to the bone grows, read a millimetre off
        // the surface. The envelope's own gradient swings by tens of degrees where the wrap is coarse, and a
        // ring laid out in a plane tilted by that much reads a flat wall as an edge.
        const Vec3 hint = unit(preopAxis, {0.0, 0.0, 1.0});
        Vec3 gradient{0.0, 0.0, 0.0};
        {
            const Vec3 probe = add(preopCenter, scale(hint, 1.0));
            constexpr double h = 0.5;
            for (int a = 0; a < 3; ++a) {
                Vec3 up = probe, down = probe;
                up[static_cast<size_t>(a)] += h;
                down[static_cast<size_t>(a)] -= h;
                double far = 0.0, near = 0.0;
                preopBone(up, &far);
                preopBone(down, &near);
                gradient[static_cast<size_t>(a)] = far - near;
            }
        }
        const Vec3 n = unit(gradient, hint);
        const Vec3 helper = std::abs(n[2]) < 0.9 ? Vec3{0.0, 0.0, 1.0} : Vec3{1.0, 0.0, 0.0};
        const Vec3 u = unit(cross(helper, n), {1.0, 0.0, 0.0});
        const Vec3 v = cross(n, u);
        constexpr int kSamples = 16;
        for (int i = 0; i < kSamples; ++i) {
            const double angle = 2.0 * 3.14159265358979323846 * i / kSamples;
            const Vec3 around =
                add(preopCenter, add(scale(u, reach * std::cos(angle)), scale(v, reach * std::sin(angle))));
            double away = 0.0;
            preopBone(around, &away);
            if (away > kOnBoneMm) {
                seat.reason = QStringLiteral("El tornillo queda en la orilla del hueso: deje al menos %1 mm de "
                                             "hueso alrededor de la cabeza.")
                                  .arg(params.minEdgeDistanceMm, 0, 'f', 1);
                return seat;
            }
        }
    }
    // And clear of the osteotomy, measured before the cut.
    if (path.valid) {
        seat.cutDistanceMm = std::abs(OsteotomyCore::PathField(path, preopCenter));
        if (seat.cutDistanceMm < params.minCutDistanceMm) {
            seat.reason = QStringLiteral("El agujero queda a %1 mm de la osteotomía: deje al menos %2 mm.")
                              .arg(seat.cutDistanceMm, 0, 'f', 1)
                              .arg(params.minCutDistanceMm, 0, 'f', 1);
            return seat;
        }
    }
    seat.ok = true;
    return seat;
}

// ── The plate ─────────────────────────────────────────────────────────────────
PlateBuildResult Build(const GuidePreparation& planned, const PlateDesign& plate, const PlateParams& params,
                       const PlateBoneQuery& boneAt, const std::atomic<bool>* cancel, const PlateKeepOut& keepOut)
{
    PlateBuildResult result;
    if (!planned.ok || !planned.wrapField) {
        result.error = QStringLiteral("Falta la envolvente del hueso en su posición planificada.");
        return result;
    }
    if (plate.holes.size() < 2 || plate.struts.empty()) {
        result.error = QStringLiteral("La placa necesita al menos dos agujeros unidos por un brazo.");
        return result;
    }
    const ImplicitCore::BakedField& wrap = *planned.wrapField;
    const double thickness = std::clamp(params.thicknessMm, 0.3, 5.0);
    const double clearance = std::clamp(params.clearanceMm, 0.0, 2.0);
    const double width = std::clamp(params.widthMm, 1.0, 15.0);
    const double ring = std::max(params.ringDiameterMm, params.holeDiameterMm + 1.0);
    const double detail = std::clamp(params.smallestDetailMm, 0.05, 0.5);
    // The edges are rounded by sweeping a shrunken ribbon and growing it back by this radius.
    const double round = std::clamp(params.edgeRoundMm, 0.0, 0.4 * thickness);
    const double middle = clearance + 0.5 * thickness;

    // The holes, dropped onto the bone surface, with the surface normal there as the screw axis.
    std::vector<PathSample> holes;
    for (const PlateHole& hole : plate.holes) {
        const Vec3 surface = projectToLevel(wrap, hole.center, clearance);
        holes.push_back({surface, smoothNormal(wrap, surface, unit(hole.axis))});
    }

    // Each strut's path: on each bone while the bone is there, straight across whatever lies between.
    struct RibbonPiece
    {
        Vec3 a, b;   // centreline on the bone surface
        Vec3 na, nb; // plate normal at each end, interpolated in between
        double halfWidth = 0.0;
        // Where the ribbon goes on past an end, the piece stops at the bisector plane (a mitre) so neighbours
        // meet flush; only the free ends of an arm are rounded.
        bool mitreA = false, mitreB = false;
        Vec3 mitreNormalA{0.0, 0.0, 0.0}, mitreNormalB{0.0, 0.0, 0.0};
    };
    std::vector<RibbonPiece> pieces;                 // the flat bars over the gap
    std::vector<ImplicitCore::NodePtr> seated;       // footprints of what lies on bone
    std::vector<Vec3> extent;
    std::vector<Vec3> holeNormals(holes.size());
    std::vector<bool> holeNormalSet(holes.size(), false);
    double bridged = 0.0;
    int steppedBridges = 0;
    std::set<std::pair<int, int>> builtEdges;
    for (const std::vector<int>& strut : plate.struts) {
        for (size_t k = 0; k + 1 < strut.size(); ++k) {
            const int a = strut[k], b = strut[k + 1];
            if (a < 0 || b < 0 || a >= static_cast<int>(holes.size()) || b >= static_cast<int>(holes.size()))
                continue;
            const std::pair<int, int> edge = std::minmax(a, b);
            if (!builtEdges.insert(edge).second)
                continue;
            double gap = 0.0;
            bool stepped = false;
            std::vector<PathSample> path =
                strutPath(wrap, holes[static_cast<size_t>(a)], holes[static_cast<size_t>(b)],
                          plate.holes[static_cast<size_t>(a)].bone, plate.holes[static_cast<size_t>(b)].bone,
                          clearance, boneAt, keepOut.boneAndGap.get(), &gap, &stepped);
            bridged += gap;
            steppedBridges += stepped ? 1 : 0;
            // The wrap's normal carries a few degrees of voxel noise: smoothed along the stretches seated on
            // bone. Not across into a bridge (each straight piece keeps its own, square to it) and not at the
            // holes, which keep the bone's normal: a hole tilted by the bar's slope put its ring and its screw
            // off the bone, and the bone carved the ring into a hook.
            for (int pass = 0; pass < 4; ++pass) {
                const std::vector<PathSample> previous = path;
                for (size_t i = 1; i + 1 < path.size(); ++i) {
                    if (previous[i].bridge || previous[i - 1].bridge || previous[i + 1].bridge)
                        continue;
                    path[i].normal = unit(add(add(previous[i - 1].normal, previous[i + 1].normal),
                                              scale(previous[i].normal, 2.0)),
                                          previous[i].normal);
                    // The centreline too: its voxel-scale jitter made the plate's edges wavy.
                    path[i].point = scale(add(add(previous[i - 1].point, previous[i + 1].point),
                                              scale(previous[i].point, 2.0)),
                                          0.25);
                }
            }
            // Coincident samples would make degenerate pieces.
            std::vector<PathSample> kept{path.front()};
            for (size_t i = 1; i < path.size(); ++i)
                if (norm(sub(path[i].point, kept.back().point)) > 0.05 || i + 1 == path.size())
                    kept.push_back(path[i]);
            // Seated on bone, the plate is bent to it: its footprint there is a capsule along the centreline at
            // mid-thickness, and the layer of the wrap's distance gives it the bone's exact shape. Only the
            // pieces that leave the bone (the bridge over the gap) are flat bars swept on their own.
            const size_t firstPiece = pieces.size();
            for (size_t i = 0; i + 1 < kept.size(); ++i) {
                if (!kept[i].bridge && !kept[i + 1].bridge) {
                    seated.push_back(ImplicitCore::Capsule(add(kept[i].point, scale(kept[i].normal, middle)),
                                                           add(kept[i + 1].point, scale(kept[i + 1].normal, middle)),
                                                           0.5 * width));
                    continue;
                }
                pieces.push_back({kept[i].point, kept[i + 1].point, kept[i].normal, kept[i + 1].normal, 0.5 * width});
            }
            // Mitred joints, the stepped bend included: its fillet turns a few degrees per piece, and pieces left
            // overlapping showed each one's rounded end as a ring on the bar.
            for (size_t i = firstPiece; i + 1 < pieces.size(); ++i) {
                if (norm(sub(pieces[i].b, pieces[i + 1].a)) > 1e-6)
                    continue; // not consecutive: a seated stretch lies between them
                const Vec3 bisector = unit(add(unit(sub(pieces[i].b, pieces[i].a)), unit(sub(pieces[i + 1].b, pieces[i + 1].a))),
                                           unit(sub(pieces[i].b, pieces[i].a)));
                pieces[i].mitreB = true;
                pieces[i].mitreNormalB = bisector;
                pieces[i + 1].mitreA = true;
                pieces[i + 1].mitreNormalA = bisector;
            }
            for (const PathSample& s : path)
                extent.push_back(s.point);
            // The screw axis — and so the ring, the bore and the countersink — is the bone's normal at the hole.
            for (const int index : {a, b}) {
                const size_t h = static_cast<size_t>(index);
                holeNormals[h] = holes[h].normal;
                holeNormalSet[h] = true;
            }
        }
    }
    for (size_t h = 0; h < holes.size(); ++h) {
        if (!holeNormalSet[h])
            holeNormals[h] = holes[h].normal;
        // The ring of material round every screw, seated on the bone like the rest: it takes the bone's shape,
        // so a hole on a curved rim is not a flat disc floating on one side and buried on the other.
        seated.push_back(ImplicitCore::Cylinder(add(holes[h].point, scale(holeNormals[h], middle)), holeNormals[h],
                                                0.5 * ring, thickness + 2.0));
        extent.push_back(holes[h].point);
    }

    double bounds[6] = {1e30, -1e30, 1e30, -1e30, 1e30, -1e30};
    for (const Vec3& p : extent)
        for (int axis = 0; axis < 3; ++axis) {
            bounds[2 * axis] = std::min(bounds[2 * axis], p[static_cast<size_t>(axis)]);
            bounds[2 * axis + 1] = std::max(bounds[2 * axis + 1], p[static_cast<size_t>(axis)]);
        }
    const double margin = 0.5 * std::max(width, ring) + clearance + thickness + 1.5;
    for (int axis = 0; axis < 3; ++axis) {
        bounds[2 * axis] -= margin;
        bounds[2 * axis + 1] += margin;
    }

    // The ribbon as one continuous sweep: at every point, the nearest piece of centreline, the plate normal
    // interpolated there, and a rounded rectangle (width × thickness) across it. No overlapping discs and
    // bars, so no seams; the thickness is exact everywhere and the edges are rounded by `round`.
    const double halfThickness = 0.5 * thickness;
    const auto sweep = [&pieces, middle, halfThickness, round](const Vec3& x) {
        double best = 1.0e30;
        for (const RibbonPiece& piece : pieces) {
            // Beyond a mitre the neighbouring piece takes over.
            if (piece.mitreA && dot(sub(x, piece.a), piece.mitreNormalA) < 0.0)
                continue;
            if (piece.mitreB && dot(sub(x, piece.b), piece.mitreNormalB) > 0.0)
                continue;
            const Vec3 ab = sub(piece.b, piece.a);
            const double len2 = dot(ab, ab);
            const double raw = len2 > 1e-12 ? dot(sub(x, piece.a), ab) / len2 : 0.0;
            const double t = std::clamp(raw, 0.0, 1.0);
            // Inside a mitred joint the edge runs straight on (no cap); only free ends are rounded.
            const double onLine = (raw < 0.0 && piece.mitreA) || (raw > 1.0 && piece.mitreB) ? raw : t;
            const Vec3 c = add(piece.a, scale(ab, onLine));
            const Vec3 r = sub(x, c);
            // Far pieces cannot beat the best one already found.
            if (norm(r) - piece.halfWidth - halfThickness - 1.0 > best)
                continue;
            const Vec3 n = unit(add(scale(piece.na, 1.0 - t), scale(piece.nb, t)), piece.na);
            const double along = dot(r, n);
            const double lateral = norm(sub(r, scale(n, along)));
            const double qx = lateral - (piece.halfWidth - round);
            const double qy = std::abs(along - middle) - (halfThickness - round);
            const double outside = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0));
            best = std::min(best, outside + std::min(std::max(qx, qy), 0.0) - round);
        }
        return best;
    };
    const auto wrapNode = ImplicitCore::Field(planned.wrapField);
    // Seated: from the real bone (inner face, never into it) out to `thickness` over a smoothed bone (outer face,
    // uniform like a machined plate), within the footprints, rims rounded by a smooth intersection. Where the real
    // bone bulges above the smoothed one the plate thins, but never below 70 % of its thickness.
    ImplicitCore::NodePtr body;
    if (!seated.empty()) {
        const auto smooth = smoothedField(wrap, bounds, 1.2);
        ImplicitCore::NodePtr outer = ImplicitCore::Offset(wrapNode, clearance + thickness);
        if (smooth)
            outer = ImplicitCore::Union(ImplicitCore::Offset(ImplicitCore::Field(smooth), clearance + thickness),
                                        ImplicitCore::Offset(wrapNode, clearance + 0.7 * thickness));
        body = ImplicitCore::SmoothIntersect({ImplicitCore::Union(seated), outer,
                                              ImplicitCore::Negate(ImplicitCore::Offset(wrapNode, clearance))},
                                             round);
    }
    // The bridge: the flat sweep, never inside the bone.
    if (!pieces.empty()) {
        const auto ribbonField = ImplicitCore::BakeFunction(sweep, bounds, detail, 0.0, cancel);
        if (!ribbonField) {
            result.error = QStringLiteral("Cálculo cancelado.");
            return result;
        }
        // The band is already taut on the outside of the bone and of the gap. Clipping it with the gap-closed
        // planning envelope would remove the middle of a ramp and leave two disconnected-looking stubs.
        const auto bridge = ImplicitCore::Field(ribbonField);
        // Filleted onto the seated part, not merely unioned with it. The seated part takes the bone's shape
        // and the bridge is flat, so where the arm leaves the bone their faces diverge at once and the plain
        // union showed the joint as one plate laid over another (user's report, 2026-09-20: "cuando se hace
        // el doblez se ve montado"). A smooth union is a smooth minimum: the negated smooth maximum.
        if (body) {
            const double blend = std::max(round, 0.5 * thickness);
            body = ImplicitCore::Negate(ImplicitCore::SmoothIntersect(
                {ImplicitCore::Negate(body), ImplicitCore::Negate(bridge)}, blend));
        } else {
            body = bridge;
        }
    }
    if (!body) {
        result.error = QStringLiteral("La placa quedó vacía.");
        return result;
    }
    // Never into the bone itself.
    if (keepOut.bone)
        body = ImplicitCore::Intersect(body,
                                       ImplicitCore::Negate(ImplicitCore::Offset(ImplicitCore::Field(keepOut.bone), clearance)));

    // The screw bores, and the countersink that seats each head flush with the outer face.
    std::vector<ImplicitCore::NodePtr> cutters;
    const double holeRadius = 0.5 * params.holeDiameterMm;
    const double sinkRadius = std::max(holeRadius, 0.5 * params.countersinkDiameterMm);
    const double sinkDepth = std::clamp(params.countersinkDepthMm, 0.0, thickness);
    for (size_t h = 0; h < holes.size(); ++h) {
        const PathSample hole{holes[h].point, holeNormals[h]};
        cutters.push_back(ImplicitCore::Cylinder(hole.point, hole.normal, holeRadius, 10.0));
        if (sinkDepth > 1e-3 && sinkRadius > holeRadius + 1e-3) {
            // A cone from the bore at `sinkDepth` below the outer face, widening past the face by 1 mm.
            const double slope = (sinkRadius - holeRadius) / sinkDepth;
            const double reach = 1.0;
            const double bottom = clearance + thickness - sinkDepth;
            const double top = clearance + thickness + reach;
            const Vec3 center = add(hole.point, scale(hole.normal, 0.5 * (bottom + top)));
            cutters.push_back(ImplicitCore::Cone(center, hole.normal, holeRadius, sinkRadius + slope * reach,
                                                 0.5 * (top - bottom)));
        }
    }
    const auto solid = ImplicitCore::Subtract(body, ImplicitCore::Union(cutters));

    ImplicitCore::PolygonizeOptions options;
    options.smoothingIterations = std::max(0, params.smoothingIterations);
    options.passBand = 0.05; // a finished surface: the seated part copies the bone, not its voxel roughness
    options.repair = true;
    const ImplicitCore::BuildResult built = ImplicitCore::Build(solid, bounds, detail, options, cancel);
    if (!built.ok) {
        result.error = built.error.isEmpty() ? QStringLiteral("La placa quedó vacía.") : built.error;
        return result;
    }
    result.mesh = built.mesh;
    // Where a footprint grazes a nearby fold or perforation of real bone, the layer leaves a sliver apart from
    // the plate. Slivers under 3 % of the plate are dust, not a piece: dropped. A real break is still reported.
    {
        auto regions = vtkSmartPointer<vtkPolyDataConnectivityFilter>::New();
        regions->SetInputData(result.mesh);
        regions->SetExtractionModeToAllRegions();
        regions->Update();
        vtkIdTypeArray* sizes = regions->GetRegionSizes();
        const int count = regions->GetNumberOfExtractedRegions();
        vtkIdType total = 0, largest = 0, others = 0;
        for (int r = 0; r < count; ++r) {
            total += sizes->GetValue(r);
            largest = std::max(largest, sizes->GetValue(r));
        }
        for (int r = 0; r < count; ++r)
            if (sizes->GetValue(r) != largest && sizes->GetValue(r) >= 0.03 * total)
                ++others;
        if (count > 1 && others == 0) {
            auto keep = vtkSmartPointer<vtkPolyDataConnectivityFilter>::New();
            keep->SetInputData(result.mesh);
            keep->SetExtractionModeToLargestRegion();
            keep->Update();
            auto cleaned = vtkSmartPointer<vtkPolyData>::New();
            cleaned->DeepCopy(keep->GetOutput());
            result.mesh = cleaned;
        }
    }
    result.pieces = shellsOf(result.mesh);
    result.bridgedMm = bridged;
    result.steppedBridges = steppedBridges;

    // Passive fit: how far the underside of the plate, under each hole, stands off the real bone.
    if (boneAt)
        for (const PathSample& hole : holes) {
            double distance = 0.0;
            boneAt(hole.point, &distance);
            result.maxFitGapMm = std::max(result.maxFitGapMm, std::max(0.0, distance));
        }

    int cranial = 0, segment = 0;
    for (const PlateHole& hole : plate.holes) {
        cranial += hole.bone == PlateBone::Cranial ? 1 : 0;
        segment += hole.bone == PlateBone::Segment ? 1 : 0;
    }
    result.report = QStringLiteral("%1: %2 agujero(s) (%3 en cráneo, %4 en segmento), espesor %5 mm, %6 pieza(s), "
                                   "%7 triángulos")
                        .arg(plate.name.isEmpty() ? QStringLiteral("Placa") : plate.name)
                        .arg(plate.holes.size())
                        .arg(cranial)
                        .arg(segment)
                        .arg(thickness, 0, 'f', 2)
                        .arg(result.pieces)
                        .arg(result.mesh->GetNumberOfPolys());
    if (bridged > 0)
        result.report += steppedBridges > 0
            ? QStringLiteral(", %1 puente(s) en rampa sobre %2 mm de espacio del corte")
                  .arg(steppedBridges).arg(bridged, 0, 'f', 1)
            : QStringLiteral(", puente de %1 mm sobre el espacio del corte").arg(bridged, 0, 'f', 1);
    if (boneAt)
        result.report += QStringLiteral(", holgura máxima bajo un agujero %1 mm").arg(result.maxFitGapMm, 0, 'f', 2);
    result.report += QStringLiteral(".");
    result.ok = true;
    return result;
}

// ── Persistence ───────────────────────────────────────────────────────────────
QJsonObject ToJson(const PlateDesign& plate)
{
    QJsonArray holes;
    for (const PlateHole& hole : plate.holes)
        holes.append(QJsonObject{{QStringLiteral("center"), vecToJson(hole.center)},
                                 {QStringLiteral("axis"), vecToJson(hole.axis)},
                                 {QStringLiteral("bone"), static_cast<int>(hole.bone)}});
    QJsonArray struts;
    for (const std::vector<int>& strut : plate.struts) {
        QJsonArray chain;
        for (int index : strut)
            chain.append(index);
        struts.append(chain);
    }
    const QString templateName = plate.kind == PlateTemplate::Splintless
                                     ? QStringLiteral("splintless")
                                     : plate.kind == PlateTemplate::LShape ? QStringLiteral("L")
                                                                          : QStringLiteral("paranasal");
    return QJsonObject{{QStringLiteral("name"), plate.name},
                       {QStringLiteral("side"), plate.side == PlateSide::Left ? QStringLiteral("left")
                                                : plate.side == PlateSide::Bilateral ? QStringLiteral("bilateral")
                                                                                     : QStringLiteral("right")},
                       {QStringLiteral("template"), templateName},
                       {QStringLiteral("holes"), holes},
                       {QStringLiteral("struts"), struts}};
}

PlateDesign FromJson(const QJsonObject& object)
{
    PlateDesign plate;
    plate.name = object.value(QStringLiteral("name")).toString();
    const QString side = object.value(QStringLiteral("side")).toString();
    plate.side = side == QStringLiteral("left") ? PlateSide::Left
                 : side == QStringLiteral("bilateral") ? PlateSide::Bilateral
                                                         : PlateSide::Right;
    const QString templateName = object.value(QStringLiteral("template")).toString();
    plate.kind = templateName == QStringLiteral("splintless") ? PlateTemplate::Splintless
                 : templateName == QStringLiteral("L")        ? PlateTemplate::LShape
                                                               : PlateTemplate::Paranasal;
    for (const QJsonValue& value : object.value(QStringLiteral("holes")).toArray()) {
        const QJsonObject hole = value.toObject();
        PlateHole parsed;
        parsed.center = vecFromJson(hole.value(QStringLiteral("center")), {0.0, 0.0, 0.0});
        parsed.axis = vecFromJson(hole.value(QStringLiteral("axis")), {0.0, 0.0, 1.0});
        const int bone = hole.value(QStringLiteral("bone")).toInt();
        parsed.bone = bone == 1 ? PlateBone::Cranial : bone == 2 ? PlateBone::Segment : PlateBone::Unknown;
        plate.holes.push_back(parsed);
    }
    for (const QJsonValue& value : object.value(QStringLiteral("struts")).toArray()) {
        std::vector<int> strut;
        for (const QJsonValue& index : value.toArray())
            strut.push_back(index.toInt());
        plate.struts.push_back(strut);
    }
    return plate;
}

QJsonObject ParamsToJson(const PlateParams& p)
{
    return QJsonObject{{QStringLiteral("thicknessMm"), p.thicknessMm},
                       {QStringLiteral("widthMm"), p.widthMm},
                       {QStringLiteral("ringDiameterMm"), p.ringDiameterMm},
                       {QStringLiteral("holeDiameterMm"), p.holeDiameterMm},
                       {QStringLiteral("countersinkDiameterMm"), p.countersinkDiameterMm},
                       {QStringLiteral("countersinkDepthMm"), p.countersinkDepthMm},
                       {QStringLiteral("clearanceMm"), p.clearanceMm},
                       {QStringLiteral("edgeRoundMm"), p.edgeRoundMm},
                       {QStringLiteral("smallestDetailMm"), p.smallestDetailMm},
                       {QStringLiteral("smoothingIterations"), p.smoothingIterations},
                       {QStringLiteral("minCutDistanceMm"), p.minCutDistanceMm},
                       {QStringLiteral("cutEdgeMarginMm"), p.cutEdgeMarginMm},
                       {QStringLiteral("minScrewsPerBone"), p.minScrewsPerBone}};
}

PlateParams ParamsFromJson(const QJsonObject& o)
{
    PlateParams p;
    const auto number = [&o](const char* key, double fallback) {
        return o.value(QLatin1String(key)).toDouble(fallback);
    };
    p.thicknessMm = number("thicknessMm", p.thicknessMm);
    p.widthMm = number("widthMm", p.widthMm);
    p.ringDiameterMm = number("ringDiameterMm", p.ringDiameterMm);
    p.holeDiameterMm = number("holeDiameterMm", p.holeDiameterMm);
    p.countersinkDiameterMm = number("countersinkDiameterMm", p.countersinkDiameterMm);
    p.countersinkDepthMm = number("countersinkDepthMm", p.countersinkDepthMm);
    p.clearanceMm = number("clearanceMm", p.clearanceMm);
    p.edgeRoundMm = number("edgeRoundMm", p.edgeRoundMm);
    p.smallestDetailMm = number("smallestDetailMm", p.smallestDetailMm);
    p.smoothingIterations = o.value(QStringLiteral("smoothingIterations")).toInt(p.smoothingIterations);
    p.minCutDistanceMm = number("minCutDistanceMm", p.minCutDistanceMm);
    p.cutEdgeMarginMm = std::max(3.5, number("cutEdgeMarginMm", p.cutEdgeMarginMm));
    p.minScrewsPerBone = o.value(QStringLiteral("minScrewsPerBone")).toInt(p.minScrewsPerBone);
    return p;
}
}
