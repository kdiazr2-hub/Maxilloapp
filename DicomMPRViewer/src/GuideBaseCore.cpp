#include "GuideBaseCore.h"

#include "ImplicitCore.h"

#include <vtkCellArray.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
using ImplicitCore::Vec3;

Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 mul(const Vec3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double length(const Vec3& v) { return std::sqrt(dot(v, v)); }
Vec3 normalized(const Vec3& v, const Vec3& fallback)
{
    const double len = length(v);
    return len > 1e-9 ? mul(v, 1.0 / len) : fallback;
}
} // namespace

namespace GuideBaseCore
{
bool ContourValid(const GuideContour& contour, QString* error)
{
    if (static_cast<int>(contour.size()) < MinContourPoints) {
        if (error)
            *error = QStringLiteral("Marque al menos %1 puntos alrededor de la zona de la guía.").arg(MinContourPoints);
        return false;
    }
    // The points must span an area, not a line.
    const Vec3 first = contour.front();
    double spread = 0.0;
    for (const auto& p : contour)
        spread = std::max(spread, length(sub(p, first)));
    if (spread < 1.0) {
        if (error)
            *error = QStringLiteral("Los puntos están demasiado juntos: rodee la zona de apoyo.");
        return false;
    }
    return true;
}

std::array<double, 3> ProjectionAxis(vtkPolyData* wrap, const GuideContour& contour, double detailMm)
{
    const Vec3 fallback{0.0, 0.0, 1.0};
    if (!wrap || contour.empty())
        return fallback;
    const auto field = ImplicitCore::BakeMeshField(wrap, std::max(0.1, detailMm), 4.0);
    if (!field)
        return fallback;
    // Gradient of the distance field: the outward normal of the wrap at each marked point.
    const double step = std::max(0.2, field->spacingMm);
    Vec3 sum{0.0, 0.0, 0.0};
    for (const auto& point : contour) {
        const Vec3 p{point[0], point[1], point[2]};
        Vec3 gradient{};
        for (int a = 0; a < 3; ++a) {
            Vec3 plus = p, minus = p;
            plus[static_cast<size_t>(a)] += step;
            minus[static_cast<size_t>(a)] -= step;
            gradient[static_cast<size_t>(a)] = field->At(plus) - field->At(minus);
        }
        sum = add(sum, normalized(gradient, fallback));
    }
    return normalized(sum, fallback);
}

vtkSmartPointer<vtkPolyData> ContourPolyline(const GuideContour& contour)
{
    auto points = vtkSmartPointer<vtkPoints>::New();
    auto lines = vtkSmartPointer<vtkCellArray>::New();
    std::vector<vtkIdType> ids;
    for (const auto& p : contour)
        ids.push_back(points->InsertNextPoint(p.data()));
    if (ids.size() >= 2) {
        if (ids.size() >= 3)
            ids.push_back(ids.front());
        lines->InsertNextCell(static_cast<vtkIdType>(ids.size()), ids.data());
    }
    auto polyline = vtkSmartPointer<vtkPolyData>::New();
    polyline->SetPoints(points);
    polyline->SetLines(lines);
    return polyline;
}

GuideBaseResult CreateBase(vtkPolyData* wrap, const GuideContour& contour, const GuideBaseParams& params,
                           const std::atomic<bool>* cancel)
{
    GuideBaseResult result;
    if (!ContourValid(contour, &result.error))
        return result;
    if (!wrap || wrap->GetNumberOfPolys() == 0) {
        result.error = QStringLiteral("Falta la malla envolvente (wrap).");
        return result;
    }
    const double thickness = std::clamp(params.thicknessMm, 0.3, 20.0);
    const double clearance = std::clamp(params.clearanceMm, 0.0, 5.0);
    const double detail = std::clamp(params.smallestDetailMm, 0.05, 2.0);

    QString error;
    // The field has to reach past the far face of the base, so it is padded accordingly.
    const auto field = ImplicitCore::BakeMeshField(wrap, detail, clearance + thickness + 3.0, cancel, &error);
    if (!field) {
        result.error = error.isEmpty() ? QStringLiteral("No se pudo medir la envolvente.") : error;
        return result;
    }
    const auto wrapField = ImplicitCore::Field(field);
    result.spacingMm = field->spacingMm;
    result.projectionAxis = ProjectionAxis(wrap, contour, std::max(detail, 0.4));

    // Region prism, long enough to cross the whole envelope.
    double wrapBounds[6] = {};
    wrap->GetBounds(wrapBounds);
    const double diagonal = std::sqrt(std::pow(wrapBounds[1] - wrapBounds[0], 2.0) +
                                      std::pow(wrapBounds[3] - wrapBounds[2], 2.0) +
                                      std::pow(wrapBounds[5] - wrapBounds[4], 2.0));
    std::vector<Vec3> polygon;
    polygon.reserve(contour.size());
    for (const auto& p : contour)
        polygon.push_back({p[0], p[1], p[2]});
    const auto prism = ImplicitCore::Prism(polygon, result.projectionAxis, diagonal);

    // The prism runs right through the anatomy and would pick up the far wall as well, so the region
    // is closed off just under the marked rim, deep enough to leave the wall its full thickness.
    double deepest = std::numeric_limits<double>::max();
    for (const Vec3& p : polygon)
        deepest = std::min(deepest, dot(p, result.projectionAxis));
    const Vec3 cutOrigin = mul(result.projectionAxis, deepest - (clearance + thickness + 0.5));
    const auto nearSide = ImplicitCore::HalfSpace(cutOrigin, mul(result.projectionAxis, -1.0));

    // The layer is measured as a real distance, so the wall keeps its thickness on oblique surfaces.
    const auto layer = ImplicitCore::Layer(wrapField, clearance, clearance + thickness);
    const auto solid = ImplicitCore::Intersect({prism, layer, nearSide});

    ImplicitCore::PolygonizeOptions options;
    options.smoothingIterations = params.smoothingIterations;
    options.repair = true;
    const ImplicitCore::BuildResult built = ImplicitCore::Build(solid, nullptr, detail, options, cancel);
    if (!built.ok) {
        result.error = built.error.isEmpty() ? QStringLiteral("La base quedó vacía.") : built.error;
        return result;
    }
    result.mesh = built.mesh;
    result.report = QStringLiteral("Base: espesor %1 mm, holgura %2 mm, detalle %3 mm, %4 triángulos.")
                        .arg(thickness, 0, 'f', 2)
                        .arg(clearance, 0, 'f', 2)
                        .arg(result.spacingMm, 0, 'f', 2)
                        .arg(result.mesh->GetNumberOfPolys());
    result.ok = true;
    return result;
}

QJsonArray ContourToJson(const GuideContour& contour)
{
    QJsonArray array;
    for (const auto& p : contour) {
        QJsonArray point;
        point.append(p[0]);
        point.append(p[1]);
        point.append(p[2]);
        array.append(point);
    }
    return array;
}

GuideContour ContourFromJson(const QJsonArray& array)
{
    GuideContour contour;
    for (const QJsonValue& value : array) {
        const QJsonArray point = value.toArray();
        if (point.size() == 3)
            contour.push_back({point.at(0).toDouble(), point.at(1).toDouble(), point.at(2).toDouble()});
    }
    return contour;
}
}
