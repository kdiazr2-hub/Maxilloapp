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
    for (const PredictiveHole& hole : holes) {
        const QString where = QStringLiteral("Placa %1, agujero %2").arg(hole.plate + 1).arg(hole.hole + 1);
        if (hole.bone == PlateBone::Unknown)
            check.warnings << QStringLiteral("%1: no está sobre el cráneo ni sobre el segmento.").arg(where);
        if (hole.bone != PlateBone::Unknown && hole.cutDistanceMm < params.minCutDistanceMm)
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
                       const std::function<double(const std::array<double, 3>&)>& boneDistance,
                       const std::atomic<bool>* cancel)
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
    const double middle = clearance + 0.5 * thickness;
    const double width = std::clamp(params.widthMm, 1.0, 15.0);
    const double ring = std::max(params.ringDiameterMm, params.holeDiameterMm + 1.0);
    const double detail = std::clamp(params.smallestDetailMm, 0.05, 0.5);

    // The holes, dropped onto the bone surface and lifted to the middle of the plate.
    std::vector<Vec3> onBone, lifted, normals;
    for (const PlateHole& hole : plate.holes) {
        const Vec3 surface = projectToLevel(wrap, hole.center, clearance);
        onBone.push_back(surface);
        lifted.push_back(projectToLevel(wrap, surface, middle));
        normals.push_back(unit(GuideBaseCore::NormalAt(wrap, surface), unit(hole.axis)));
    }

    // The strip: capsules along each strut, its centreline hugging the surface at mid-thickness so it follows
    // the bone and the bridge over the cut instead of cutting chords through them.
    std::vector<ImplicitCore::NodePtr> region;
    std::vector<Vec3> extent;
    for (const std::vector<int>& strut : plate.struts) {
        for (size_t k = 0; k + 1 < strut.size(); ++k) {
            const int a = strut[k], b = strut[k + 1];
            if (a < 0 || b < 0 || a >= static_cast<int>(lifted.size()) || b >= static_cast<int>(lifted.size()))
                continue;
            const Vec3 from = lifted[static_cast<size_t>(a)];
            const Vec3 to = lifted[static_cast<size_t>(b)];
            const int pieces = std::max(1, static_cast<int>(std::ceil(norm(sub(to, from)) / 1.2)));
            Vec3 previous = from;
            for (int i = 1; i <= pieces; ++i) {
                const double t = static_cast<double>(i) / pieces;
                Vec3 point = add(from, scale(sub(to, from), t));
                if (i < pieces)
                    point = projectToLevel(wrap, point, middle);
                region.push_back(ImplicitCore::Capsule(previous, point, 0.5 * width));
                extent.push_back(point);
                previous = point;
            }
        }
    }
    for (size_t h = 0; h < lifted.size(); ++h) {
        region.push_back(ImplicitCore::Cylinder(lifted[h], normals[h], 0.5 * ring, thickness + 2.0));
        extent.push_back(lifted[h]);
    }

    const auto wrapNode = ImplicitCore::Field(planned.wrapField);
    const auto outer = ImplicitCore::Offset(wrapNode, clearance + thickness);
    const auto inner = ImplicitCore::Negate(ImplicitCore::Offset(wrapNode, clearance));
    const auto body =
        ImplicitCore::SmoothIntersect({ImplicitCore::Union(region), outer, inner}, std::max(0.0, params.edgeRoundMm));

    // The screw bores, and the countersink that seats each head flush with the outer face.
    std::vector<ImplicitCore::NodePtr> cutters;
    const double holeRadius = 0.5 * params.holeDiameterMm;
    const double sinkRadius = std::max(holeRadius, 0.5 * params.countersinkDiameterMm);
    const double sinkDepth = std::clamp(params.countersinkDepthMm, 0.0, thickness);
    for (size_t h = 0; h < onBone.size(); ++h) {
        cutters.push_back(ImplicitCore::Cylinder(onBone[h], normals[h], holeRadius, 10.0));
        if (sinkDepth > 1e-3 && sinkRadius > holeRadius + 1e-3) {
            // A cone from the bore at `sinkDepth` below the outer face, widening past the face by 1 mm.
            const double slope = (sinkRadius - holeRadius) / sinkDepth;
            const double reach = 1.0;
            const double bottom = clearance + thickness - sinkDepth;
            const double top = clearance + thickness + reach;
            const Vec3 center = add(onBone[h], scale(normals[h], 0.5 * (bottom + top)));
            cutters.push_back(ImplicitCore::Cone(center, normals[h], holeRadius, sinkRadius + slope * reach,
                                                 0.5 * (top - bottom)));
        }
    }
    const auto solid = ImplicitCore::Subtract(body, ImplicitCore::Union(cutters));

    double bounds[6] = {1e30, -1e30, 1e30, -1e30, 1e30, -1e30};
    for (const Vec3& p : extent)
        for (int axis = 0; axis < 3; ++axis) {
            bounds[2 * axis] = std::min(bounds[2 * axis], p[static_cast<size_t>(axis)]);
            bounds[2 * axis + 1] = std::max(bounds[2 * axis + 1], p[static_cast<size_t>(axis)]);
        }
    const double margin = 0.5 * std::max(width, ring) + thickness + 1.5;
    for (int axis = 0; axis < 3; ++axis) {
        bounds[2 * axis] -= margin;
        bounds[2 * axis + 1] += margin;
    }

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

    // Passive fit: how far the underside of the plate, under each hole, stands off the real bone. The wrap
    // bridges the cut and fills narrow hollows, so this is where the plate would rock if it is not ~0.
    if (boneDistance)
        for (const Vec3& p : onBone)
            result.maxFitGapMm = std::max(result.maxFitGapMm, std::max(0.0, boneDistance(p)));

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
    if (boneDistance)
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
