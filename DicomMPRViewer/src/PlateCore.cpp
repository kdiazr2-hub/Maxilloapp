#include "PlateCore.h"

#include "GuideBaseCore.h"

#include <QJsonArray>

#include <vtkLandmarkTransform.h>
#include <vtkMatrix4x4.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataConnectivityFilter.h>
#include <vtkStaticCellLocator.h>

#include <algorithm>
#include <cmath>

namespace
{
using Vec3 = std::array<double, 3>;

Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 scale(const Vec3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double norm(const Vec3& a) { return std::sqrt(dot(a, a)); }
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
    return unit(gradient, fallback);
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

// The path of one plate arm from hole `a` to hole `b`: on each hole's bone as far as it goes, and a straight
// bar across whatever is left between the two walks — the gap the movement opened at the osteotomy, the cut
// face — lifted over any corner of bone. Without this the plate dives into the gap and falls apart.
std::vector<PathSample> strutPath(const ImplicitCore::BakedField& field, const PathSample& a, const PathSample& b,
                                  PlateBone boneA, PlateBone boneB, double clearance, const PlateBoneQuery& boneAt,
                                  double* bridgedMm)
{
    const double step = 1.0;
    if (bridgedMm)
        *bridgedMm = 0.0;
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
        // The bridge: a flat bar with one normal — the holes' mean, square to the bar — so it is exactly as
        // thick as the plate; lifted along that normal where it would cut through a corner of bone.
        const Vec3 along = unit(sub(endB.point, endA.point), unit(sub(b.point, a.point)));
        Vec3 barNormal = unit(add(a.normal, b.normal), a.normal);
        barNormal = unit(sub(barNormal, scale(along, dot(barNormal, along))), a.normal);
        const int pieces = std::max(1, static_cast<int>(std::ceil(gap / step)));
        for (int k = 1; k < pieces; ++k) {
            PathSample s{add(endA.point, scale(sub(endB.point, endA.point), static_cast<double>(k) / pieces)), barNormal};
            for (int iteration = 0; iteration < 4; ++iteration) {
                const double below = clearance - field.At(s.point);
                if (below <= 1e-3)
                    break;
                s.point = add(s.point, scale(barNormal, below));
            }
            path.push_back(s);
        }
        if (bridgedMm)
            *bridgedMm = gap;
    }
    for (auto it = fromB.rbegin(); it != fromB.rend(); ++it)
        if (norm(sub(it->point, path.back().point)) > 0.05)
            path.push_back(*it);
    return path;
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

// ── The plate ─────────────────────────────────────────────────────────────────
PlateBuildResult Build(const GuidePreparation& planned, const PlateDesign& plate, const PlateParams& params,
                       const PlateBoneQuery& boneAt, const std::atomic<bool>* cancel)
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
    std::vector<RibbonPiece> pieces;
    std::vector<Vec3> extent;
    std::vector<Vec3> holeNormals(holes.size());
    std::vector<bool> holeNormalSet(holes.size(), false);
    double bridged = 0.0;
    for (const std::vector<int>& strut : plate.struts) {
        for (size_t k = 0; k + 1 < strut.size(); ++k) {
            const int a = strut[k], b = strut[k + 1];
            if (a < 0 || b < 0 || a >= static_cast<int>(holes.size()) || b >= static_cast<int>(holes.size()))
                continue;
            double gap = 0.0;
            std::vector<PathSample> path =
                strutPath(wrap, holes[static_cast<size_t>(a)], holes[static_cast<size_t>(b)],
                          plate.holes[static_cast<size_t>(a)].bone, plate.holes[static_cast<size_t>(b)].bone,
                          clearance, boneAt, &gap);
            bridged += gap;
            // The wrap's normal carries a few degrees of voxel noise, and the bar turns where it leaves the
            // bone: smoothed along the arm, the ribbon twists continuously instead of in steps.
            for (int pass = 0; pass < 4; ++pass) {
                const std::vector<PathSample> previous = path;
                for (size_t i = 0; i < path.size(); ++i) {
                    const Vec3& before = previous[i == 0 ? 0 : i - 1].normal;
                    const Vec3& after = previous[std::min(i + 1, path.size() - 1)].normal;
                    path[i].normal = unit(add(add(before, after), scale(previous[i].normal, 2.0)), previous[i].normal);
                }
            }
            // Coincident samples would make degenerate pieces.
            std::vector<PathSample> kept{path.front()};
            for (size_t i = 1; i < path.size(); ++i)
                if (norm(sub(path[i].point, kept.back().point)) > 0.05 || i + 1 == path.size())
                    kept.push_back(path[i]);
            const size_t firstPiece = pieces.size();
            for (size_t i = 0; i + 1 < kept.size(); ++i)
                pieces.push_back({kept[i].point, kept[i + 1].point, kept[i].normal, kept[i + 1].normal, 0.5 * width});
            for (size_t i = firstPiece; i + 1 < pieces.size(); ++i) {
                const Vec3 bisector = unit(add(unit(sub(pieces[i].b, pieces[i].a)), unit(sub(pieces[i + 1].b, pieces[i + 1].a))),
                                           unit(sub(pieces[i].b, pieces[i].a)));
                pieces[i].mitreB = true;
                pieces[i].mitreNormalB = bisector;
                pieces[i + 1].mitreA = true;
                pieces[i + 1].mitreNormalA = bisector;
            }
            for (const PathSample& s : path)
                extent.push_back(s.point);
            // The screw axis is the ribbon's own normal at the hole, so ring, bore and countersink line up.
            for (const auto& [index, sample] : {std::pair{a, path.front()}, std::pair{b, path.back()}}) {
                const size_t h = static_cast<size_t>(index);
                if (!holeNormalSet[h]) {
                    holeNormals[h] = sample.normal;
                    holeNormalSet[h] = true;
                }
            }
        }
    }
    for (size_t h = 0; h < holes.size(); ++h) {
        if (!holeNormalSet[h])
            holeNormals[h] = holes[h].normal;
        // The ring of material around every screw: a piece of no length, as wide as the ring.
        pieces.push_back({holes[h].point, holes[h].point, holeNormals[h], holeNormals[h], 0.5 * ring});
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
    const auto ribbonField = ImplicitCore::BakeFunction(sweep, bounds, detail, 0.0, cancel);
    if (!ribbonField) {
        result.error = QStringLiteral("Cálculo cancelado.");
        return result;
    }
    // Never inside the bone: where the flat ribbon meets a curve across its width, the bone carves its seat.
    const auto outsideBone = ImplicitCore::Negate(ImplicitCore::Offset(ImplicitCore::Field(planned.wrapField), clearance));
    const auto body = ImplicitCore::Intersect(ImplicitCore::Field(ribbonField), outsideBone);

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
    options.passBand = 0.08;
    options.repair = true;
    const ImplicitCore::BuildResult built = ImplicitCore::Build(solid, bounds, detail, options, cancel);
    if (!built.ok) {
        result.error = built.error.isEmpty() ? QStringLiteral("La placa quedó vacía.") : built.error;
        return result;
    }
    result.mesh = built.mesh;
    result.pieces = shellsOf(result.mesh);
    result.bridgedMm = bridged;

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
        result.report += QStringLiteral(", puente recto de %1 mm sobre el espacio del corte").arg(bridged, 0, 'f', 1);
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
    return QJsonObject{{QStringLiteral("name"), plate.name},
                       {QStringLiteral("side"), plate.side == PlateSide::Left ? QStringLiteral("left")
                                                                              : QStringLiteral("right")},
                       {QStringLiteral("template"), plate.kind == PlateTemplate::LShape ? QStringLiteral("L")
                                                                                        : QStringLiteral("paranasal")},
                       {QStringLiteral("holes"), holes},
                       {QStringLiteral("struts"), struts}};
}

PlateDesign FromJson(const QJsonObject& object)
{
    PlateDesign plate;
    plate.name = object.value(QStringLiteral("name")).toString();
    plate.side = object.value(QStringLiteral("side")).toString() == QStringLiteral("left") ? PlateSide::Left
                                                                                           : PlateSide::Right;
    plate.kind = object.value(QStringLiteral("template")).toString() == QStringLiteral("L") ? PlateTemplate::LShape
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
    p.minScrewsPerBone = o.value(QStringLiteral("minScrewsPerBone")).toInt(p.minScrewsPerBone);
    return p;
}
}
