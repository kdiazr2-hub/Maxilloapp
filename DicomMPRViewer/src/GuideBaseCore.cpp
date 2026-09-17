#include "GuideBaseCore.h"

#include <vtkCellArray.h>
#include <vtkContourFilter.h>
#include <vtkFloatArray.h>
#include <vtkImageData.h>
#include <vtkMath.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkStripper.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <limits>
#include <thread>

namespace
{
using ImplicitCore::BakedPlanarField;
using ImplicitCore::Vec3;

Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 mul(const Vec3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double length(const Vec3& v) { return std::sqrt(dot(v, v)); }
Vec3 normalized(const Vec3& v, const Vec3& fallback)
{
    const double len = length(v);
    return len > 1e-9 ? mul(v, 1.0 / len) : fallback;
}

constexpr double kFar = 1.0e30;

void rows(int count, const std::function<void(int, int)>& body)
{
    const int threads = std::max(1, std::min(static_cast<int>(std::thread::hardware_concurrency()), count / 4));
    if (threads <= 1) {
        body(0, count);
        return;
    }
    const int chunk = (count + threads - 1) / threads;
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) {
        const int begin = t * chunk;
        const int end = std::min(count, begin + chunk);
        if (begin < end)
            pool.emplace_back(body, begin, end);
    }
    for (auto& thread : pool)
        thread.join();
}

bool insidePolygon(const std::vector<std::array<double, 2>>& polygon, double x, double y)
{
    bool inside = false;
    const size_t n = polygon.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const auto& a = polygon[i];
        const auto& b = polygon[j];
        if ((a[1] > y) != (b[1] > y) && x < a[0] + (y - a[1]) * (b[0] - a[0]) / (b[1] - a[1]))
            inside = !inside;
    }
    return inside;
}

// 2D morphology by an exact distance: grow a mask by `radiusCells`.
std::vector<uint8_t> dilate(const std::vector<uint8_t>& mask, int nu, int nv, double radiusCells)
{
    if (!(radiusCells > 0.0))
        return mask;
    std::vector<double> squared(mask.size());
    for (size_t i = 0; i < mask.size(); ++i)
        squared[i] = mask[i] ? 0.0 : kFar;
    ImplicitCore::SquaredDistanceTransform(squared, {nu, nv, 1});
    std::vector<uint8_t> out(mask.size());
    for (size_t i = 0; i < mask.size(); ++i)
        out[i] = squared[i] <= radiusCells * radiusCells ? 1 : 0;
    return out;
}

std::vector<uint8_t> invert(const std::vector<uint8_t>& mask)
{
    std::vector<uint8_t> out(mask.size());
    for (size_t i = 0; i < mask.size(); ++i)
        out[i] = mask[i] ? 0 : 1;
    return out;
}

std::vector<uint8_t> erode(const std::vector<uint8_t>& mask, int nu, int nv, double radiusCells)
{
    return invert(dilate(invert(mask), nu, nv, radiusCells));
}

// The direction the marked patch faces: the normal of the plane that best fits the marked points, turned
// outward by the field. Averaging the surface normals at the points instead would tilt the axis whenever a
// point sits on an edge (its normal is the side wall's), and the side would then look like the front.
std::array<double, 3> axisFromField(const ImplicitCore::BakedField& field, const GuideContour& contour)
{
    Vec3 outward{0.0, 0.0, 0.0};
    Vec3 centroid{0.0, 0.0, 0.0};
    for (const auto& point : contour) {
        outward = add(outward, GuideBaseCore::NormalAt(field, point));
        centroid = add(centroid, mul(point, 1.0 / static_cast<double>(contour.size())));
    }
    outward = normalized(outward, {0.0, 0.0, 1.0});

    double c0[3] = {}, c1[3] = {}, c2[3] = {};
    double* covariance[3] = {c0, c1, c2};
    for (const auto& point : contour) {
        const Vec3 d = sub(point, centroid);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                covariance[r][c] += d[static_cast<size_t>(r)] * d[static_cast<size_t>(c)];
    }
    double eigenvalues[3] = {};
    double e0[3] = {}, e1[3] = {}, e2[3] = {};
    double* eigenvectors[3] = {e0, e1, e2};
    vtkMath::Jacobi(covariance, eigenvalues, eigenvectors); // decreasing, vectors in columns
    // Points spread along a line give no plane: keep the averaged normal.
    if (!(eigenvalues[1] > 1e-6))
        return outward;
    Vec3 normal = normalized({eigenvectors[0][2], eigenvectors[1][2], eigenvectors[2][2]}, outward);
    if (dot(normal, outward) < 0.0)
        normal = mul(normal, -1.0);
    return normal;
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

std::array<double, 3> NormalAt(const ImplicitCore::BakedField& field, const std::array<double, 3>& point)
{
    // Gradient of the distance field: the outward normal of the wrap there.
    const Vec3 fallback{0.0, 0.0, 1.0};
    const Vec3 p{point[0], point[1], point[2]};
    const double step = std::max(0.2, field.spacingMm);
    Vec3 gradient{};
    for (int a = 0; a < 3; ++a) {
        Vec3 plus = p, minus = p;
        plus[static_cast<size_t>(a)] += step;
        minus[static_cast<size_t>(a)] -= step;
        gradient[static_cast<size_t>(a)] = field.At(plus) - field.At(minus);
    }
    return normalized(gradient, fallback);
}

std::array<double, 3> ProjectionAxis(vtkPolyData* wrap, const GuideContour& contour, double detailMm)
{
    if (!wrap || contour.empty())
        return {0.0, 0.0, 1.0};
    const auto field = ImplicitCore::BakeMeshField(wrap, std::max(0.1, detailMm), 4.0);
    return field ? axisFromField(*field, contour) : std::array<double, 3>{0.0, 0.0, 1.0};
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

GuideRegion MakeRegion(const std::shared_ptr<const ImplicitCore::BakedField>& wrapField, const GuideContour& contour,
                       const GuideBaseParams& params)
{
    GuideRegion region;
    if (!wrapField) {
        region.error = QStringLiteral("La envolvente no está medida.");
        return region;
    }
    if (!ContourValid(contour, &region.error))
        return region;
    const ImplicitCore::BakedField& field = *wrapField;
    const double h = std::max(0.2, field.spacingMm);
    const double radius = std::clamp(params.cornerRadiusMm, 0.0, 20.0);
    const double thickness = std::clamp(params.thicknessMm, 0.3, 20.0);
    const double clearance = std::clamp(params.clearanceMm, 0.0, 5.0);

    // Frame of the projection axis, centred on the marked points.
    region.axis = axisFromField(field, contour);
    const Vec3 n = region.axis;
    const Vec3 helper = std::abs(n[2]) < 0.9 ? Vec3{0.0, 0.0, 1.0} : Vec3{1.0, 0.0, 0.0};
    const Vec3 uAxis = normalized(cross(helper, n), {1.0, 0.0, 0.0});
    const Vec3 vAxis = cross(n, uAxis);
    Vec3 origin{0.0, 0.0, 0.0};
    for (const auto& p : contour)
        origin = add(origin, mul(p, 1.0 / static_cast<double>(contour.size())));

    std::vector<std::array<double, 2>> polygon;
    double uMin = kFar, uMax = -kFar, vMin = kFar, vMax = -kFar;
    for (const auto& p : contour) {
        const Vec3 d = sub(p, origin);
        polygon.push_back({dot(d, uAxis), dot(d, vAxis)});
        uMin = std::min(uMin, polygon.back()[0]);
        uMax = std::max(uMax, polygon.back()[0]);
        vMin = std::min(vMin, polygon.back()[1]);
        vMax = std::max(vMax, polygon.back()[1]);
    }
    // Room for the rounding and for the wall to wrap past the rim.
    const double margin = radius + clearance + thickness + 4.0 * h;
    const int nu = static_cast<int>(std::ceil((uMax - uMin + 2.0 * margin) / h)) + 1;
    const int nv = static_cast<int>(std::ceil((vMax - vMin + 2.0 * margin) / h)) + 1;
    const double u0 = uMin - margin, v0 = vMin - margin;
    const size_t cells = static_cast<size_t>(nu) * static_cast<size_t>(nv);
    const auto index = [nu](int i, int j) { return static_cast<size_t>(i) + static_cast<size_t>(nu) * j; };

    // Rounded outline: opening then closing by the corner radius, so convex and concave corners both round.
    std::vector<uint8_t> inside(cells);
    for (int j = 0; j < nv; ++j)
        for (int i = 0; i < nu; ++i)
            inside[index(i, j)] = insidePolygon(polygon, u0 + i * h, v0 + j * h) ? 1 : 0;
    const double radiusCells = radius / h;
    std::vector<uint8_t> rounded = inside;
    if (radiusCells > 0.0) {
        rounded = dilate(erode(rounded, nu, nv, radiusCells), nu, nv, radiusCells);
        rounded = erode(dilate(rounded, nu, nv, radiusCells), nu, nv, radiusCells);
    }
    if (std::none_of(rounded.begin(), rounded.end(), [](uint8_t c) { return c != 0; })) {
        region.error = QStringLiteral("La zona marcada es más pequeña que el redondeo: amplíela o reduzca el redondeo.");
        return region;
    }
    std::vector<double> toInside(cells), toOutside(cells);
    for (size_t c = 0; c < cells; ++c) {
        toInside[c] = rounded[c] ? 0.0 : kFar;
        toOutside[c] = rounded[c] ? kFar : 0.0;
    }
    ImplicitCore::SquaredDistanceTransform(toInside, {nu, nv, 1});
    ImplicitCore::SquaredDistanceTransform(toOutside, {nu, nv, 1});
    auto outline = std::make_shared<BakedPlanarField>();
    outline->nu = nu;
    outline->nv = nv;
    outline->u0 = u0;
    outline->v0 = v0;
    outline->spacingMm = h;
    outline->origin = origin;
    outline->uAxis = uAxis;
    outline->vAxis = vAxis;
    outline->axis = n;
    outline->values.resize(cells);
    for (size_t c = 0; c < cells; ++c)
        outline->values[c] = static_cast<float>(
            (rounded[c] ? -(std::sqrt(toOutside[c]) - 0.5) : (std::sqrt(toInside[c]) - 0.5)) * h);

    // Height of the first surface met coming in along -axis, over the whole grid.
    double aTop = -kFar, aBottom = kFar;
    for (int corner = 0; corner < 8; ++corner) {
        const Vec3 p{field.origin[0] + ((corner & 1) ? (field.dims[0] - 1) * field.spacingMm : 0.0),
                     field.origin[1] + ((corner & 2) ? (field.dims[1] - 1) * field.spacingMm : 0.0),
                     field.origin[2] + ((corner & 4) ? (field.dims[2] - 1) * field.spacingMm : 0.0)};
        const double a = dot(sub(p, origin), n);
        aTop = std::max(aTop, a);
        aBottom = std::min(aBottom, a);
    }
    std::vector<double> surface(cells, kFar); // kFar: nothing hit
    rows(nv, [&](int jBegin, int jEnd) {
        for (int j = jBegin; j < jEnd; ++j)
            for (int i = 0; i < nu; ++i) {
                const Vec3 base = add(origin, add(mul(uAxis, u0 + i * h), mul(vAxis, v0 + j * h)));
                double previous = field.At(add(base, mul(n, aTop)));
                for (double a = aTop - h; a >= aBottom; a -= h) {
                    const double value = field.At(add(base, mul(n, a)));
                    if (value <= 0.0) {
                        const double t = previous > value ? previous / (previous - value) : 0.0;
                        surface[index(i, j)] = (a + h) - t * h;
                        break;
                    }
                    previous = value;
                }
            }
    });
    // Where the surface is seen, the wall must stay in front of it (a little tolerance for the sampling).
    // Past the silhouette nothing is seen: those cells take the height of the nearest surface lowered by the
    // wall's thickness, so the rim wraps the edge by no more than that and never runs down the back.
    const double tolerance = 1.5 * h;
    const double wrapPast = clearance + thickness + h;
    auto front = std::make_shared<BakedPlanarField>(*outline);
    std::vector<double> heights(cells, kFar);
    std::deque<std::pair<int, int>> queue;
    for (int j = 0; j < nv; ++j)
        for (int i = 0; i < nu; ++i)
            if (surface[index(i, j)] < kFar) {
                heights[index(i, j)] = surface[index(i, j)] - tolerance;
                queue.emplace_back(i, j);
            }
    if (queue.empty()) {
        region.error = QStringLiteral("La zona marcada no toca la envolvente.");
        return region;
    }
    std::vector<double> nearest = surface; // height of the nearest seen surface, spread outward
    while (!queue.empty()) {
        const auto [i, j] = queue.front();
        queue.pop_front();
        const double source = nearest[index(i, j)];
        for (const auto& [di, dj] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
            const int ni = i + di, nj = j + dj;
            if (ni < 0 || nj < 0 || ni >= nu || nj >= nv || nearest[index(ni, nj)] < kFar)
                continue;
            nearest[index(ni, nj)] = source;
            heights[index(ni, nj)] = source - wrapPast;
            queue.emplace_back(ni, nj);
        }
    }
    for (size_t c = 0; c < cells; ++c)
        front->values[c] = static_cast<float>(heights[c]);

    region.outline = outline;
    region.front = front;
    region.valid = true;
    return region;
}

vtkSmartPointer<vtkPolyData> RegionOutline(const GuideRegion& region, double liftMm)
{
    auto result = vtkSmartPointer<vtkPolyData>::New();
    if (!region.valid || !region.outline)
        return result;
    const BakedPlanarField& outline = *region.outline;
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(outline.nu, outline.nv, 1);
    image->SetSpacing(outline.spacingMm, outline.spacingMm, 1.0);
    image->SetOrigin(outline.u0, outline.v0, 0.0);
    auto values = vtkSmartPointer<vtkFloatArray>::New();
    values->SetNumberOfTuples(static_cast<vtkIdType>(outline.values.size()));
    std::copy(outline.values.begin(), outline.values.end(), values->GetPointer(0));
    image->GetPointData()->SetScalars(values);

    auto contour = vtkSmartPointer<vtkContourFilter>::New();
    contour->SetInputData(image);
    contour->SetValue(0, 0.0);
    auto stripper = vtkSmartPointer<vtkStripper>::New();
    stripper->SetInputConnection(contour->GetOutputPort());
    stripper->JoinContiguousSegmentsOn();
    stripper->Update();
    result->DeepCopy(stripper->GetOutput());

    // Lay the outline on the surface: each (u, v) goes to the height of the surface seen there.
    vtkPoints* points = result->GetPoints();
    if (!points)
        return result;
    double uvw[3] = {};
    for (vtkIdType id = 0; id < points->GetNumberOfPoints(); ++id) {
        points->GetPoint(id, uvw);
        double height = region.front ? region.front->At(uvw[0], uvw[1]) : 0.0;
        if (!(height < kFar * 0.5))
            height = 0.0;
        const Vec3 p = add(outline.origin, add(mul(outline.uAxis, uvw[0]),
                                               add(mul(outline.vAxis, uvw[1]), mul(outline.axis, height + liftMm))));
        points->SetPoint(id, p.data());
    }
    return result;
}

ImplicitCore::NodePtr RegionPrism(const GuideRegion& region, double spanMm)
{
    return region.valid ? ImplicitCore::PlanarPrism(region.outline, spanMm) : nullptr;
}

ImplicitCore::NodePtr BaseNode(const std::shared_ptr<const ImplicitCore::BakedField>& wrapField,
                               const GuideRegion& region, const GuideBaseParams& params, double spanMm)
{
    const auto field = ImplicitCore::Field(wrapField);
    if (!field || !region.valid)
        return nullptr;
    const double thickness = std::clamp(params.thicknessMm, 0.3, 20.0);
    const double clearance = std::clamp(params.clearanceMm, 0.0, 5.0);
    // The layer is measured as a real distance, so the wall keeps its thickness on oblique surfaces; the
    // height limiter keeps it on the surface the user marked.
    return ImplicitCore::Intersect({RegionPrism(region, spanMm), ImplicitCore::Layer(field, clearance, clearance + thickness),
                                    ImplicitCore::PlanarHeight(region.front, 0.0)});
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
    result.spacingMm = field->spacingMm;
    const GuideRegion region = MakeRegion(field, contour, params);
    if (!region.valid) {
        result.error = region.error;
        return result;
    }
    result.projectionAxis = region.axis;

    // Region prism, long enough to cross the whole envelope.
    double wrapBounds[6] = {};
    wrap->GetBounds(wrapBounds);
    const double diagonal = std::sqrt(std::pow(wrapBounds[1] - wrapBounds[0], 2.0) +
                                      std::pow(wrapBounds[3] - wrapBounds[2], 2.0) +
                                      std::pow(wrapBounds[5] - wrapBounds[4], 2.0));
    const auto solid = BaseNode(field, region, params, diagonal);
    double bounds[6] = {};
    if (!ImplicitCore::Bounds(solid, bounds)) {
        result.error = QStringLiteral("La base no está acotada.");
        return result;
    }

    ImplicitCore::PolygonizeOptions options;
    options.smoothingIterations = params.smoothingIterations;
    options.repair = true;
    const ImplicitCore::BuildResult built = ImplicitCore::Build(solid, bounds, detail, options, cancel);
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
    for (const auto& p : contour)
        array.append(QJsonArray{p[0], p[1], p[2]});
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
