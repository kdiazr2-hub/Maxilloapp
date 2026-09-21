#include "SplintDesignCore.h"

#include <QJsonValue>
#include <QUuid>

#include <vtkMath.h>
#include <vtkPolyData.h>

#include <algorithm>
#include <cmath>

namespace
{
QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool isBuiltInLabel(int label)
{
    return label == SplintDesignCore::kIntermediateLabel || label == SplintDesignCore::kFinalLabel;
}

QJsonArray vec3Json(const SplintPoint3& p)
{
    return QJsonArray{p[0], p[1], p[2]};
}

bool vec3From(const QJsonValue& value, SplintPoint3& out)
{
    const QJsonArray a = value.toArray();
    if (a.size() != 3)
        return false;
    for (int i = 0; i < 3; ++i) {
        if (!a[i].isDouble())
            return false;
        out[static_cast<size_t>(i)] = a[i].toDouble();
    }
    return true;
}

QJsonArray pointsJson(const std::vector<SplintPoint3>& points)
{
    QJsonArray array;
    for (const SplintPoint3& p : points)
        array.append(vec3Json(p));
    return array;
}

std::vector<SplintPoint3> pointsFrom(const QJsonValue& value)
{
    std::vector<SplintPoint3> points;
    for (const QJsonValue& item : value.toArray()) {
        SplintPoint3 p{};
        if (vec3From(item, p))
            points.push_back(p);
    }
    return points;
}

QJsonObject frameJson(const SplintOcclusalFrame& frame)
{
    QJsonObject o;
    o[QStringLiteral("origin")] = vec3Json(frame.origin);
    o[QStringLiteral("axisU")] = vec3Json(frame.axisU);
    o[QStringLiteral("axisV")] = vec3Json(frame.axisV);
    o[QStringLiteral("normal")] = vec3Json(frame.normal);
    o[QStringLiteral("anteriorResolved")] = frame.anteriorResolved;
    return o;
}

SplintOcclusalFrame frameFrom(const QJsonObject& o)
{
    SplintOcclusalFrame frame;
    vec3From(o.value(QStringLiteral("origin")), frame.origin);
    vec3From(o.value(QStringLiteral("axisU")), frame.axisU);
    vec3From(o.value(QStringLiteral("axisV")), frame.axisV);
    vec3From(o.value(QStringLiteral("normal")), frame.normal);
    frame.anteriorResolved = o.value(QStringLiteral("anteriorResolved")).toBool(false);
    return frame;
}

bool nameTaken(const std::vector<SplintDesign>& designs, const QString& name, int ignoreIndex, int limit)
{
    for (int i = 0; i < limit; ++i) {
        if (i != ignoreIndex && designs[static_cast<size_t>(i)].name.compare(name, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

QString uniqueAmong(const std::vector<SplintDesign>& designs, const QString& base, int ignoreIndex, int limit)
{
    QString trimmed = base.trimmed();
    if (trimmed.isEmpty())
        trimmed = QStringLiteral("Férula");
    if (!nameTaken(designs, trimmed, ignoreIndex, limit))
        return trimmed;
    for (int n = 2;; ++n) {
        const QString candidate = QStringLiteral("%1 %2").arg(trimmed).arg(n);
        if (!nameTaken(designs, candidate, ignoreIndex, limit))
            return candidate;
    }
}

bool validIndex(const std::vector<SplintDesign>& designs, int index, QString* error)
{
    if (index >= 0 && index < static_cast<int>(designs.size()))
        return true;
    if (error)
        *error = QStringLiteral("Diseño de férula inexistente.");
    return false;
}
} // namespace

namespace SplintDesignCore
{
namespace
{
double dot3(const SplintPoint3& a, const SplintPoint3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
SplintPoint3 sub3(const SplintPoint3& a, const SplintPoint3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
SplintPoint3 unit3(const SplintPoint3& a, const SplintPoint3& fallback)
{
    const double length = std::sqrt(dot3(a, a));
    return length > 1e-9 ? SplintPoint3{a[0] / length, a[1] / length, a[2] / length} : fallback;
}
} // namespace

std::vector<SplintPoint3> AutoGuidePoints(vtkPolyData* teeth, const SplintPoint3& occlusal, int count)
{
    std::vector<SplintPoint3> picked;
    if (!teeth || teeth->GetNumberOfPoints() < 50 || count < 3)
        return picked;
    const SplintPoint3 towardsBite = unit3(occlusal, {0.0, 0.0, -1.0});

    // The occlusal band: everything within 6 mm of the most occlusal point. That is the cusps, whatever else
    // the source mesh carries (the composite brings its bone with it).
    std::vector<vtkIdType> band;
    double highest = -1e30;
    for (vtkIdType id = 0; id < teeth->GetNumberOfPoints(); ++id) {
        double q[3] = {};
        teeth->GetPoint(id, q);
        highest = std::max(highest, dot3({q[0], q[1], q[2]}, towardsBite));
    }
    for (vtkIdType id = 0; id < teeth->GetNumberOfPoints(); ++id) {
        double q[3] = {};
        teeth->GetPoint(id, q);
        if (dot3({q[0], q[1], q[2]}, towardsBite) > highest - 6.0)
            band.push_back(id);
    }
    if (band.size() < static_cast<size_t>(count) * 3)
        return picked;

    // Its widest direction across the bite: the arch runs along it, right to left.
    SplintPoint3 centre{0.0, 0.0, 0.0};
    for (const vtkIdType id : band) {
        double q[3] = {};
        teeth->GetPoint(id, q);
        for (int a = 0; a < 3; ++a)
            centre[static_cast<size_t>(a)] += q[a] / static_cast<double>(band.size());
    }
    double covariance[3][3] = {};
    for (const vtkIdType id : band) {
        double q[3] = {};
        teeth->GetPoint(id, q);
        SplintPoint3 d = sub3({q[0], q[1], q[2]}, centre);
        const double along = dot3(d, towardsBite);
        for (int a = 0; a < 3; ++a)
            d[static_cast<size_t>(a)] -= along * towardsBite[static_cast<size_t>(a)]; // flattened onto the bite plane
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                covariance[r][c] += d[static_cast<size_t>(r)] * d[static_cast<size_t>(c)];
    }
    double* rows[3] = {covariance[0], covariance[1], covariance[2]};
    double eigenvalues[3] = {};
    double e0[3] = {}, e1[3] = {}, e2[3] = {};
    double* eigenvectors[3] = {e0, e1, e2};
    vtkMath::Jacobi(rows, eigenvalues, eigenvectors); // decreasing, vectors in columns
    const SplintPoint3 across =
        unit3({eigenvectors[0][0], eigenvectors[1][0], eigenvectors[2][0]}, {1.0, 0.0, 0.0});

    // One point per bin across the arch: the highest cusp in it.
    double lowestU = 1e30, highestU = -1e30;
    for (const vtkIdType id : band) {
        double q[3] = {};
        teeth->GetPoint(id, q);
        const double u = dot3(sub3({q[0], q[1], q[2]}, centre), across);
        lowestU = std::min(lowestU, u);
        highestU = std::max(highestU, u);
    }
    if (!(highestU - lowestU > 5.0))
        return picked;
    const double step = (highestU - lowestU) / count;
    std::vector<vtkIdType> best(static_cast<size_t>(count), -1);
    std::vector<double> bestHeight(static_cast<size_t>(count), -1e30);
    for (const vtkIdType id : band) {
        double q[3] = {};
        teeth->GetPoint(id, q);
        const SplintPoint3 p{q[0], q[1], q[2]};
        const int bin = std::clamp(static_cast<int>((dot3(sub3(p, centre), across) - lowestU) / step), 0, count - 1);
        const double height = dot3(p, towardsBite);
        if (height > bestHeight[static_cast<size_t>(bin)]) {
            bestHeight[static_cast<size_t>(bin)] = height;
            best[static_cast<size_t>(bin)] = id;
        }
    }
    for (const vtkIdType id : best) {
        if (id < 0)
            continue;
        double q[3] = {};
        teeth->GetPoint(id, q);
        picked.push_back({q[0], q[1], q[2]});
    }
    return picked;
}

std::vector<SplintDesign> DefaultDesigns(int upperSource, int intermediateLowerSource, int finalLowerSource)
{
    SplintDesign intermediate;
    intermediate.id = QStringLiteral("intermedia");
    intermediate.name = QStringLiteral("Intermedia");
    intermediate.label = kIntermediateLabel;
    intermediate.builtIn = true;
    intermediate.upperSource = upperSource;
    intermediate.lowerSource = intermediateLowerSource;

    SplintDesign finalDesign = intermediate;
    finalDesign.id = QStringLiteral("final");
    finalDesign.name = QStringLiteral("Final");
    finalDesign.label = kFinalLabel;
    finalDesign.lowerSource = finalLowerSource;
    return {intermediate, finalDesign};
}

int IndexOfLabel(const std::vector<SplintDesign>& designs, int label)
{
    for (size_t i = 0; i < designs.size(); ++i)
        if (designs[i].label == label)
            return static_cast<int>(i);
    return -1;
}

int NextFreeLabel(const std::vector<SplintDesign>& designs)
{
    for (int label = kFirstExtraLabel; label <= kLastExtraLabel; ++label)
        if (IndexOfLabel(designs, label) < 0)
            return label;
    return -1;
}

QString UniqueName(const std::vector<SplintDesign>& designs, const QString& base, int ignoreIndex)
{
    return uniqueAmong(designs, base, ignoreIndex, static_cast<int>(designs.size()));
}

bool AddDesign(std::vector<SplintDesign>& designs, const QString& name, int upperSource, int lowerSource,
               int* newIndex, QString* error)
{
    const int label = NextFreeLabel(designs);
    if (label < 0) {
        if (error)
            *error = QStringLiteral("Se alcanzó el máximo de %1 diseños adicionales.")
                         .arg(kLastExtraLabel - kFirstExtraLabel + 1);
        return false;
    }
    SplintDesign design;
    design.id = newId();
    design.name = UniqueName(designs, name);
    design.label = label;
    design.upperSource = upperSource;
    design.lowerSource = lowerSource;
    designs.push_back(design);
    if (newIndex)
        *newIndex = static_cast<int>(designs.size()) - 1;
    return true;
}

bool CopyDesign(std::vector<SplintDesign>& designs, int index, int* newIndex, QString* error)
{
    if (!validIndex(designs, index, error))
        return false;
    const int label = NextFreeLabel(designs);
    if (label < 0) {
        if (error)
            *error = QStringLiteral("Se alcanzó el máximo de %1 diseños adicionales.")
                         .arg(kLastExtraLabel - kFirstExtraLabel + 1);
        return false;
    }
    SplintDesign copy = designs[static_cast<size_t>(index)];
    copy.id = newId();
    copy.label = label;
    copy.builtIn = false;
    copy.name = UniqueName(designs, copy.name + QStringLiteral(" (copia)"));
    designs.push_back(copy);
    if (newIndex)
        *newIndex = static_cast<int>(designs.size()) - 1;
    return true;
}

bool RenameDesign(std::vector<SplintDesign>& designs, int index, const QString& name, QString* error)
{
    if (!validIndex(designs, index, error))
        return false;
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        if (error)
            *error = QStringLiteral("El nombre no puede estar vacío.");
        return false;
    }
    if (nameTaken(designs, trimmed, index, static_cast<int>(designs.size()))) {
        if (error)
            *error = QStringLiteral("Ya existe un diseño llamado «%1».").arg(trimmed);
        return false;
    }
    designs[static_cast<size_t>(index)].name = trimmed;
    return true;
}

bool RemoveDesign(std::vector<SplintDesign>& designs, int index, QString* error)
{
    if (!validIndex(designs, index, error))
        return false;
    if (designs[static_cast<size_t>(index)].builtIn) {
        if (error)
            *error = QStringLiteral("Los diseños Intermedia y Final no se pueden borrar.");
        return false;
    }
    designs.erase(designs.begin() + index);
    return true;
}

bool SameFrame(const SplintOcclusalFrame& a, const SplintOcclusalFrame& b,
               double originToleranceMm, double axisToleranceCos)
{
    const auto dot = [](const SplintPoint3& x, const SplintPoint3& y) {
        return x[0] * y[0] + x[1] * y[1] + x[2] * y[2];
    };
    const SplintPoint3 d{a.origin[0] - b.origin[0], a.origin[1] - b.origin[1], a.origin[2] - b.origin[2]};
    return std::sqrt(dot(d, d)) <= originToleranceMm &&
           dot(a.normal, b.normal) >= axisToleranceCos &&
           dot(a.axisU, b.axisU) >= axisToleranceCos &&
           dot(a.axisV, b.axisV) >= axisToleranceCos;
}

SplintMatrix IdentityMatrix()
{
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}

SplintMatrix Compose(const SplintMatrix& second, const SplintMatrix& first)
{
    SplintMatrix out{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k)
                sum += second[static_cast<size_t>(r * 4 + k)] * first[static_cast<size_t>(k * 4 + c)];
            out[static_cast<size_t>(r * 4 + c)] = sum;
        }
    return out;
}

SplintPoint3 TransformPoint(const SplintMatrix& m, const SplintPoint3& p)
{
    return {m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3],
            m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7],
            m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]};
}

SplintPoint3 TransformDirection(const SplintMatrix& m, const SplintPoint3& d)
{
    return {m[0] * d[0] + m[1] * d[1] + m[2] * d[2],
            m[4] * d[0] + m[5] * d[1] + m[6] * d[2],
            m[8] * d[0] + m[9] * d[1] + m[10] * d[2]};
}

std::optional<SplintMatrix> RigidMotion(vtkPolyData* from, vtkPolyData* to, double toleranceMm)
{
    if (!from || !to || from->GetNumberOfPoints() < 3 || from->GetNumberOfPoints() != to->GetNumberOfPoints())
        return std::nullopt;
    const vtkIdType n = from->GetNumberOfPoints();
    const vtkIdType step = std::max<vtkIdType>(1, n / 500);
    std::vector<std::array<double, 3>> ps, qs;
    double cp[3] = {0, 0, 0}, cq[3] = {0, 0, 0};
    for (vtkIdType i = 0; i < n; i += step) {
        std::array<double, 3> p{}, q{};
        from->GetPoint(i, p.data());
        to->GetPoint(i, q.data());
        ps.push_back(p);
        qs.push_back(q);
        for (int a = 0; a < 3; ++a) {
            cp[a] += p[static_cast<size_t>(a)];
            cq[a] += q[static_cast<size_t>(a)];
        }
    }
    const double count = static_cast<double>(ps.size());
    for (int a = 0; a < 3; ++a) {
        cp[a] /= count;
        cq[a] /= count;
    }
    double h[3][3] = {};
    for (size_t i = 0; i < ps.size(); ++i)
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                h[r][c] += (ps[i][static_cast<size_t>(r)] - cp[r]) * (qs[i][static_cast<size_t>(c)] - cq[c]);
    double u[3][3] = {}, w[3] = {}, vt[3][3] = {};
    vtkMath::SingularValueDecomposition3x3(h, u, w, vt);
    // R = V Uᵀ (Kabsch), with a reflection fix.
    const auto rotation = [&u, &vt](double sign) {
        std::array<std::array<double, 3>, 3> r{};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                double sum = 0.0;
                for (int k = 0; k < 3; ++k)
                    sum += vt[k][i] * (k == 2 ? sign : 1.0) * u[j][k];
                r[static_cast<size_t>(i)][static_cast<size_t>(j)] = sum;
            }
        return r;
    };
    auto r = rotation(1.0);
    const double det = r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) -
                       r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) +
                       r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]);
    if (det < 0.0)
        r = rotation(-1.0);
    SplintMatrix m = IdentityMatrix();
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j)
            m[static_cast<size_t>(i * 4 + j)] = r[static_cast<size_t>(i)][static_cast<size_t>(j)];
        m[static_cast<size_t>(i * 4 + 3)] = cq[i] - (r[static_cast<size_t>(i)][0] * cp[0] + r[static_cast<size_t>(i)][1] * cp[1] +
                                                     r[static_cast<size_t>(i)][2] * cp[2]);
    }
    for (size_t i = 0; i < ps.size(); ++i) {
        const SplintPoint3 mapped = TransformPoint(m, ps[i]);
        const double dx = mapped[0] - qs[i][0], dy = mapped[1] - qs[i][1], dz = mapped[2] - qs[i][2];
        if (std::sqrt(dx * dx + dy * dy + dz * dz) > toleranceMm)
            return std::nullopt;
    }
    return m;
}

bool AddBracketMark(SplintExtras& extras, const SplintPoint3& center, double radiusMm)
{
    const double radius = std::clamp(radiusMm, 0.1, 10.0);
    for (const SplintBracketMark& mark : extras.bracketMarks) {
        const double dx = mark.center[0] - center[0];
        const double dy = mark.center[1] - center[1];
        const double dz = mark.center[2] - center[2];
        if (std::sqrt(dx * dx + dy * dy + dz * dz) < 0.5 * radius)
            return false;
    }
    extras.bracketMarks.push_back({center, radius});
    return true;
}

int RemoveBracketMarks(SplintExtras& extras, const SplintPoint3& center, double radiusMm)
{
    const auto before = extras.bracketMarks.size();
    extras.bracketMarks.erase(
        std::remove_if(extras.bracketMarks.begin(), extras.bracketMarks.end(),
                       [&](const SplintBracketMark& mark) {
                           const double dx = mark.center[0] - center[0];
                           const double dy = mark.center[1] - center[1];
                           const double dz = mark.center[2] - center[2];
                           return std::sqrt(dx * dx + dy * dy + dz * dz) <= radiusMm;
                       }),
        extras.bracketMarks.end());
    return static_cast<int>(before - extras.bracketMarks.size());
}

void ReorientWireHoles(SplintDesign& design, const SplintOcclusalFrame& frame)
{
    for (SplintWireHole& hole : design.extras.wireHoles)
        hole.axis = SplintHeightmapGenerator::WireHoleAxis(design.wireHoleOrientation, hole.surfaceNormal,
                                                           design.extras.bevel, frame);
}

QJsonObject ExtrasToJson(const SplintExtras& e)
{
    QJsonObject o;
    if (e.bevel) {
        QJsonObject bevel;
        bevel[QStringLiteral("first")] = vec3Json(e.bevel->first);
        bevel[QStringLiteral("second")] = vec3Json(e.bevel->second);
        o[QStringLiteral("bevel")] = bevel;
    }
    QJsonArray holes;
    for (const SplintWireHole& hole : e.wireHoles) {
        QJsonObject h;
        h[QStringLiteral("center")] = vec3Json(hole.center);
        h[QStringLiteral("axis")] = vec3Json(hole.axis);
        h[QStringLiteral("surfaceNormal")] = vec3Json(hole.surfaceNormal);
        h[QStringLiteral("diameterMm")] = hole.diameterMm;
        holes.append(h);
    }
    o[QStringLiteral("wireHoles")] = holes;
    QJsonArray marks;
    for (const SplintBracketMark& mark : e.bracketMarks) {
        QJsonObject m;
        m[QStringLiteral("center")] = vec3Json(mark.center);
        m[QStringLiteral("radiusMm")] = mark.radiusMm;
        marks.append(m);
    }
    o[QStringLiteral("bracketMarks")] = marks;
    o[QStringLiteral("bracketOffsetMm")] = e.bracketOffsetMm;
    return o;
}

SplintExtras ExtrasFromJson(const QJsonObject& o)
{
    SplintExtras e;
    const QJsonObject bevel = o.value(QStringLiteral("bevel")).toObject();
    SplintBevel b;
    if (vec3From(bevel.value(QStringLiteral("first")), b.first) && vec3From(bevel.value(QStringLiteral("second")), b.second))
        e.bevel = b;
    for (const QJsonValue& value : o.value(QStringLiteral("wireHoles")).toArray()) {
        const QJsonObject h = value.toObject();
        SplintWireHole hole;
        if (!vec3From(h.value(QStringLiteral("center")), hole.center) || !vec3From(h.value(QStringLiteral("axis")), hole.axis))
            continue;
        if (!vec3From(h.value(QStringLiteral("surfaceNormal")), hole.surfaceNormal))
            hole.surfaceNormal = hole.axis;
        hole.diameterMm = h.value(QStringLiteral("diameterMm")).toDouble(1.0);
        e.wireHoles.push_back(hole);
    }
    for (const QJsonValue& value : o.value(QStringLiteral("bracketMarks")).toArray()) {
        const QJsonObject m = value.toObject();
        SplintBracketMark mark;
        if (!vec3From(m.value(QStringLiteral("center")), mark.center))
            continue;
        mark.radiusMm = m.value(QStringLiteral("radiusMm")).toDouble(1.5);
        e.bracketMarks.push_back(mark);
    }
    e.bracketOffsetMm = o.value(QStringLiteral("bracketOffsetMm")).toDouble(e.bracketOffsetMm);
    return e;
}

QJsonObject ParamsToJson(const SplintHeightmapParams& p)
{
    QJsonObject o;
    o[QStringLiteral("edgeOffsetMm")] = p.edgeOffsetMm;
    o[QStringLiteral("filletMm")] = p.filletMm;
    o[QStringLiteral("clearanceMm")] = p.clearanceMm;
    o[QStringLiteral("minFeatureMm")] = p.minFeatureMm;
    o[QStringLiteral("impressionUpper")] = p.impressionUpper;
    o[QStringLiteral("impressionLower")] = p.impressionLower;
    o[QStringLiteral("minThicknessMm")] = p.minThicknessMm;
    o[QStringLiteral("maxThicknessMm")] = p.maxThicknessMm;
    o[QStringLiteral("computeThickness")] = p.computeThickness;
    o[QStringLiteral("roundingFactorForSplintSide")] = p.roundingFactorForSplintSide;
    o[QStringLiteral("convexHullFixDistance")] = p.convexHullFixDistance;
    o[QStringLiteral("reduceThreshold")] = p.reduceThreshold;
    o[QStringLiteral("smoothThreshold")] = p.smoothThreshold;
    o[QStringLiteral("innerSmoothSigma")] = p.innerSmoothSigma;
    o[QStringLiteral("convexHullSmoothSigma")] = p.convexHullSmoothSigma;
    o[QStringLiteral("gridResolutionMm")] = p.gridResolutionMm;
    o[QStringLiteral("undercutUpper")] = p.undercutUpper;
    o[QStringLiteral("undercutLower")] = p.undercutLower;
    o[QStringLiteral("undercutAngleUpperDeg")] = p.undercutAngleUpperDeg;
    o[QStringLiteral("undercutAngleLowerDeg")] = p.undercutAngleLowerDeg;
    o[QStringLiteral("undercutDirectionUV")] = QJsonArray{p.undercutDirectionUV[0], p.undercutDirectionUV[1]};
    return o;
}

SplintHeightmapParams ParamsFromJson(const QJsonObject& o)
{
    SplintHeightmapParams p;
    const auto number = [&o](const QString& key, double& field) {
        const QJsonValue value = o.value(key);
        if (value.isDouble())
            field = value.toDouble();
    };
    const auto flag = [&o](const QString& key, bool& field) {
        const QJsonValue value = o.value(key);
        if (value.isBool())
            field = value.toBool();
    };
    number(QStringLiteral("edgeOffsetMm"), p.edgeOffsetMm);
    number(QStringLiteral("filletMm"), p.filletMm);
    number(QStringLiteral("clearanceMm"), p.clearanceMm);
    number(QStringLiteral("minFeatureMm"), p.minFeatureMm);
    flag(QStringLiteral("impressionUpper"), p.impressionUpper);
    flag(QStringLiteral("impressionLower"), p.impressionLower);
    number(QStringLiteral("minThicknessMm"), p.minThicknessMm);
    number(QStringLiteral("maxThicknessMm"), p.maxThicknessMm);
    flag(QStringLiteral("computeThickness"), p.computeThickness);
    number(QStringLiteral("roundingFactorForSplintSide"), p.roundingFactorForSplintSide);
    number(QStringLiteral("convexHullFixDistance"), p.convexHullFixDistance);
    number(QStringLiteral("reduceThreshold"), p.reduceThreshold);
    number(QStringLiteral("smoothThreshold"), p.smoothThreshold);
    number(QStringLiteral("innerSmoothSigma"), p.innerSmoothSigma);
    number(QStringLiteral("convexHullSmoothSigma"), p.convexHullSmoothSigma);
    number(QStringLiteral("gridResolutionMm"), p.gridResolutionMm);
    flag(QStringLiteral("undercutUpper"), p.undercutUpper);
    flag(QStringLiteral("undercutLower"), p.undercutLower);
    number(QStringLiteral("undercutAngleUpperDeg"), p.undercutAngleUpperDeg);
    number(QStringLiteral("undercutAngleLowerDeg"), p.undercutAngleLowerDeg);
    const QJsonArray direction = o.value(QStringLiteral("undercutDirectionUV")).toArray();
    if (direction.size() == 2 && direction[0].isDouble() && direction[1].isDouble())
        p.undercutDirectionUV = {direction[0].toDouble(), direction[1].toDouble()};
    return p;
}

QJsonObject DesignToJson(const SplintDesign& d)
{
    QJsonObject o;
    o[QStringLiteral("id")] = d.id;
    o[QStringLiteral("name")] = d.name;
    o[QStringLiteral("label")] = d.label;
    o[QStringLiteral("upperSource")] = d.upperSource;
    o[QStringLiteral("lowerSource")] = d.lowerSource;
    o[QStringLiteral("upperPoints")] = pointsJson(d.upperPoints);
    o[QStringLiteral("lowerPoints")] = pointsJson(d.lowerPoints);
    o[QStringLiteral("params")] = ParamsToJson(d.params);
    if (!d.editedContourUV.empty()) {
        QJsonArray contours;
        for (const SplintContourUV& contour : d.editedContourUV) {
            QJsonArray points;
            for (const SplintPointUV& p : contour)
                points.append(QJsonArray{p[0], p[1]});
            contours.append(points);
        }
        o[QStringLiteral("editedContourUV")] = contours;
        o[QStringLiteral("editedContourFrame")] = frameJson(d.editedContourFrame);
    }
    o[QStringLiteral("extras")] = ExtrasToJson(d.extras);
    o[QStringLiteral("wireHoleDiameterMm")] = d.wireHoleDiameterMm;
    o[QStringLiteral("wireHoleOrientation")] =
        d.wireHoleOrientation == SplintHoleOrientation::Bevel ? QStringLiteral("bevel") : QStringLiteral("normal");
    o[QStringLiteral("bracketBrushRadiusMm")] = d.bracketBrushRadiusMm;
    if (!d.createdSourceKey.isEmpty())
        o[QStringLiteral("createdSourceKey")] = d.createdSourceKey;
    return o;
}

bool DesignFromJson(const QJsonObject& o, SplintDesign& design, QString* error)
{
    const QString name = o.value(QStringLiteral("name")).toString().trimmed();
    if (name.isEmpty()) {
        if (error)
            *error = QStringLiteral("Diseño de férula sin nombre.");
        return false;
    }
    if (!o.value(QStringLiteral("label")).isDouble()) {
        if (error)
            *error = QStringLiteral("El diseño de férula «%1» no tiene label.").arg(name);
        return false;
    }

    SplintDesign d;
    d.id = o.value(QStringLiteral("id")).toString();
    if (d.id.isEmpty())
        d.id = newId();
    d.name = name;
    d.label = o.value(QStringLiteral("label")).toInt();
    d.builtIn = isBuiltInLabel(d.label);
    d.upperSource = o.value(QStringLiteral("upperSource")).toInt(0);
    d.lowerSource = o.value(QStringLiteral("lowerSource")).toInt(0);
    d.upperPoints = pointsFrom(o.value(QStringLiteral("upperPoints")));
    d.lowerPoints = pointsFrom(o.value(QStringLiteral("lowerPoints")));
    d.params = ParamsFromJson(o.value(QStringLiteral("params")).toObject());
    for (const QJsonValue& contourValue : o.value(QStringLiteral("editedContourUV")).toArray()) {
        SplintContourUV contour;
        for (const QJsonValue& pointValue : contourValue.toArray()) {
            const QJsonArray p = pointValue.toArray();
            if (p.size() == 2 && p[0].isDouble() && p[1].isDouble())
                contour.push_back({p[0].toDouble(), p[1].toDouble()});
        }
        if (contour.size() >= 3)
            d.editedContourUV.push_back(std::move(contour));
    }
    if (!d.editedContourUV.empty())
        d.editedContourFrame = frameFrom(o.value(QStringLiteral("editedContourFrame")).toObject());
    d.extras = ExtrasFromJson(o.value(QStringLiteral("extras")).toObject());
    d.wireHoleDiameterMm = o.value(QStringLiteral("wireHoleDiameterMm")).toDouble(d.wireHoleDiameterMm);
    d.wireHoleOrientation = o.value(QStringLiteral("wireHoleOrientation")).toString() == QStringLiteral("bevel")
        ? SplintHoleOrientation::Bevel
        : SplintHoleOrientation::SurfaceNormal;
    d.bracketBrushRadiusMm = o.value(QStringLiteral("bracketBrushRadiusMm")).toDouble(d.bracketBrushRadiusMm);
    d.createdSourceKey = o.value(QStringLiteral("createdSourceKey")).toString();
    design = std::move(d);
    return true;
}

QJsonArray DesignsToJson(const std::vector<SplintDesign>& designs)
{
    QJsonArray array;
    for (const SplintDesign& d : designs)
        array.append(DesignToJson(d));
    return array;
}

std::vector<SplintDesign> DesignsFromJson(const QJsonArray& array, const std::vector<SplintDesign>& defaults)
{
    std::vector<SplintDesign> builtIns;
    std::vector<SplintDesign> extras;
    for (const QJsonValue& value : array) {
        SplintDesign design;
        if (!DesignFromJson(value.toObject(), design, nullptr))
            continue;
        if (design.builtIn) {
            if (IndexOfLabel(builtIns, design.label) < 0)
                builtIns.push_back(std::move(design));
        } else {
            extras.push_back(std::move(design));
        }
    }
    for (const SplintDesign& d : defaults)
        if (d.builtIn && IndexOfLabel(builtIns, d.label) < 0)
            builtIns.push_back(d);
    std::sort(builtIns.begin(), builtIns.end(),
              [](const SplintDesign& a, const SplintDesign& b) { return a.label < b.label; });

    std::vector<SplintDesign> result = builtIns;
    for (SplintDesign& d : extras) {
        const bool labelOk = d.label >= kFirstExtraLabel && d.label <= kLastExtraLabel && IndexOfLabel(result, d.label) < 0;
        if (!labelOk) {
            d.label = NextFreeLabel(result);
            if (d.label < 0)
                continue;
        }
        result.push_back(std::move(d));
    }

    for (size_t i = 0; i < result.size(); ++i) {
        result[i].name = uniqueAmong(result, result[i].name, static_cast<int>(i), static_cast<int>(i));
        for (size_t j = 0; j < i; ++j) {
            if (result[j].id == result[i].id) {
                result[i].id = newId();
                break;
            }
        }
    }
    return result;
}
} // namespace SplintDesignCore
