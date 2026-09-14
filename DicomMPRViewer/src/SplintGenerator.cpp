#include "SplintGenerator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include <vtkAppendPolyData.h>
#include <vtkBooleanOperationPolyDataFilter.h>
#include <vtkCellArray.h>
#include <vtkCleanPolyData.h>
#include <vtkIdList.h>
#include <vtkImplicitPolyDataDistance.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataNormals.h>
#include <vtkSmartPointer.h>
#include <vtkTriangleFilter.h>
#include <vtkWindowedSincPolyDataFilter.h>

namespace {

constexpr double kPi = 3.14159265358979323846;

vtkSmartPointer<vtkPolyData> cleanTriangulate(vtkPolyData* mesh)
{
    if (!mesh || mesh->GetNumberOfPoints() <= 0)
        return nullptr;

    auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
    clean->SetInputData(mesh);
    clean->Update();

    auto tri = vtkSmartPointer<vtkTriangleFilter>::New();
    tri->SetInputConnection(clean->GetOutputPort());
    tri->Update();

    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(tri->GetOutput());
    return out;
}

double percentile(std::vector<double> values, double p)
{
    if (values.empty())
        return 0.0;

    std::sort(values.begin(), values.end());
    const double clamped = std::max(0.0, std::min(1.0, p));
    const double idx = clamped * static_cast<double>(values.size() - 1);
    const auto i0 = static_cast<size_t>(std::floor(idx));
    const auto i1 = static_cast<size_t>(std::ceil(idx));
    if (i0 == i1)
        return values[i0];
    const double t = idx - static_cast<double>(i0);
    return values[i0] * (1.0 - t) + values[i1] * t;
}

std::vector<double> zValues(vtkPolyData* mesh)
{
    std::vector<double> zs;
    if (!mesh) return zs;
    zs.reserve(static_cast<size_t>(mesh->GetNumberOfPoints()));
    double p[3] = {};
    for (vtkIdType i = 0; i < mesh->GetNumberOfPoints(); ++i) {
        mesh->GetPoint(i, p);
        zs.push_back(p[2]);
    }
    return zs;
}

struct XYBounds
{
    double xMin = 0.0;
    double xMax = 0.0;
    double yMin = 0.0;
    double yMax = 0.0;
    bool valid = false;
};

void includePoint(XYBounds& b, double x, double y)
{
    if (!b.valid) {
        b.xMin = b.xMax = x;
        b.yMin = b.yMax = y;
        b.valid = true;
        return;
    }
    b.xMin = std::min(b.xMin, x);
    b.xMax = std::max(b.xMax, x);
    b.yMin = std::min(b.yMin, y);
    b.yMax = std::max(b.yMax, y);
}

using GuidePointList = std::vector<std::array<double, 3>>;

void includeGuidePoints(XYBounds& b, const GuidePointList& points)
{
    for (const auto& p : points)
        includePoint(b, p[0], p[1]);
}

struct RailSample
{
    double angle = 0.0;
    double radius = 0.0;
};

struct LocalPoint
{
    double u = 0.0;
    double v = 0.0;
};

struct Vec3
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

Vec3 toVec3(const std::array<double, 3>& p)
{
    return {p[0], p[1], p[2]};
}

Vec3 operator+(const Vec3& a, const Vec3& b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 operator-(const Vec3& a, const Vec3& b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 operator*(const Vec3& a, double s)
{
    return {a.x * s, a.y * s, a.z * s};
}

double dot(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

double length(const Vec3& v)
{
    return std::sqrt(dot(v, v));
}

Vec3 normalized(const Vec3& v, const Vec3& fallback)
{
    const double len = length(v);
    if (len <= 1e-6)
        return fallback;
    return v * (1.0 / len);
}

Vec3 meanPoint(const GuidePointList& a, const GuidePointList& b = {})
{
    Vec3 mean;
    size_t count = 0;
    for (const auto& p : a) {
        mean = mean + toVec3(p);
        ++count;
    }
    for (const auto& p : b) {
        mean = mean + toVec3(p);
        ++count;
    }
    return count > 0 ? mean * (1.0 / static_cast<double>(count)) : Vec3{};
}

struct GuidedFootprint
{
    Vec3 origin;
    Vec3 axisU;
    Vec3 axisV;
    Vec3 normal;
    double startAngle = 0.0;
    double endAngle = 0.0;
    std::vector<RailSample> outerRail;
    std::vector<RailSample> innerRail;
    bool valid = false;
};

double normalizeAngle(double a)
{
    while (a < 0.0) a += 2.0 * kPi;
    while (a >= 2.0 * kPi) a -= 2.0 * kPi;
    return a;
}

double unwrapAngleFrom(double a, double start)
{
    a = normalizeAngle(a);
    while (a < start)
        a += 2.0 * kPi;
    return a;
}

void appendSamples(std::vector<RailSample>& out,
                   const GuidePointList& points,
                   const GuidedFootprint& guide,
                   double startAngle)
{
    for (const auto& p : points) {
        const Vec3 d = toVec3(p) - guide.origin;
        const double dx = dot(d, guide.axisU);
        const double dy = dot(d, guide.axisV);
        const double r = std::sqrt(dx * dx + dy * dy);
        if (r <= 1e-3)
            continue;
        out.push_back({unwrapAngleFrom(std::atan2(dy, dx), startAngle), r});
    }
}

double interpolatedRadius(const std::vector<RailSample>& samples, double angle)
{
    if (samples.empty())
        return 0.0;
    if (angle <= samples.front().angle)
        return samples.front().radius;
    if (angle >= samples.back().angle)
        return samples.back().radius;

    for (size_t i = 1; i < samples.size(); ++i) {
        const RailSample& a = samples[i - 1];
        const RailSample& b = samples[i];
        if (angle > b.angle)
            continue;
        const double span = std::max(1e-6, b.angle - a.angle);
        const double t = (angle - a.angle) / span;
        return a.radius * (1.0 - t) + b.radius * t;
    }
    return samples.back().radius;
}

LocalPoint projectLocal(const GuidedFootprint& guide, const Vec3& p)
{
    const Vec3 d = p - guide.origin;
    return {dot(d, guide.axisU), dot(d, guide.axisV)};
}

std::vector<LocalPoint> sortedLocalCurve(const GuidePointList& points,
                                         const GuidedFootprint& guide)
{
    std::vector<LocalPoint> curve;
    curve.reserve(points.size());
    for (const auto& p : points)
        curve.push_back(projectLocal(guide, toVec3(p)));

    std::sort(curve.begin(), curve.end(), [](const LocalPoint& a, const LocalPoint& b) {
        if (std::abs(a.u - b.u) > 1e-3)
            return a.u < b.u;
        return a.v < b.v;
    });
    return curve;
}

double localDistance(const LocalPoint& a, const LocalPoint& b)
{
    const double du = b.u - a.u;
    const double dv = b.v - a.v;
    return std::sqrt(du * du + dv * dv);
}

LocalPoint sampleLocalCurve(const std::vector<LocalPoint>& curve, double t)
{
    if (curve.empty())
        return {};
    if (curve.size() == 1)
        return curve.front();

    std::vector<double> cumulative(curve.size(), 0.0);
    for (size_t i = 1; i < curve.size(); ++i)
        cumulative[i] = cumulative[i - 1] + localDistance(curve[i - 1], curve[i]);

    const double total = cumulative.back();
    if (total <= 1e-6) {
        const double u = curve.front().u * (1.0 - t) + curve.back().u * t;
        const double v = curve.front().v * (1.0 - t) + curve.back().v * t;
        return {u, v};
    }

    const double target = std::clamp(t, 0.0, 1.0) * total;
    for (size_t i = 1; i < curve.size(); ++i) {
        if (target > cumulative[i])
            continue;
        const double span = std::max(1e-6, cumulative[i] - cumulative[i - 1]);
        const double localT = (target - cumulative[i - 1]) / span;
        return {
            curve[i - 1].u * (1.0 - localT) + curve[i].u * localT,
            curve[i - 1].v * (1.0 - localT) + curve[i].v * localT
        };
    }
    return curve.back();
}

std::vector<LocalPoint> extendCurveEnds(const std::vector<LocalPoint>& curve, double extensionMm)
{
    if (curve.size() < 2 || extensionMm <= 1e-3)
        return curve;

    std::vector<LocalPoint> extended;
    extended.reserve(curve.size() + 2);

    const LocalPoint first = curve.front();
    const LocalPoint second = curve[1];
    const double firstLen = std::max(1e-6, localDistance(first, second));
    const LocalPoint firstDir{
        (second.u - first.u) / firstLen,
        (second.v - first.v) / firstLen
    };
    extended.push_back({
        first.u - firstDir.u * extensionMm,
        first.v - firstDir.v * extensionMm
    });

    extended.insert(extended.end(), curve.begin(), curve.end());

    const LocalPoint last = curve.back();
    const LocalPoint prev = curve[curve.size() - 2];
    const double lastLen = std::max(1e-6, localDistance(prev, last));
    const LocalPoint lastDir{
        (last.u - prev.u) / lastLen,
        (last.v - prev.v) / lastLen
    };
    extended.push_back({
        last.u + lastDir.u * extensionMm,
        last.v + lastDir.v * extensionMm
    });

    return extended;
}

LocalPoint averagePoint(const LocalPoint& a, const LocalPoint& b)
{
    return {(a.u + b.u) * 0.5, (a.v + b.v) * 0.5};
}

void expandCurvePair(LocalPoint& outer, LocalPoint& inner, double paddingMm)
{
    if (paddingMm <= 1e-3)
        return;

    double du = outer.u - inner.u;
    double dv = outer.v - inner.v;
    double width = std::sqrt(du * du + dv * dv);
    if (width <= 1e-6) {
        du = 0.0;
        dv = 1.0;
        width = 1.0;
    }

    du /= width;
    dv /= width;
    outer.u += du * paddingMm;
    outer.v += dv * paddingMm;
    inner.u -= du * paddingMm;
    inner.v -= dv * paddingMm;
}

GuidedFootprint buildGuidedFootprint(const SplintGenerationInputs& inputs)
{
    GuidedFootprint guide;
    const size_t outerCount = inputs.upperVestibularPoints.size() + inputs.lowerVestibularPoints.size();
    const size_t innerCount = inputs.upperPalatalPoints.size() + inputs.lowerLingualPoints.size();
    if (outerCount < 3 || innerCount < 3)
        return guide;

    std::vector<std::array<double, 3>> all;
    all.reserve(outerCount + innerCount);
    all.insert(all.end(), inputs.upperVestibularPoints.begin(), inputs.upperVestibularPoints.end());
    all.insert(all.end(), inputs.lowerVestibularPoints.begin(), inputs.lowerVestibularPoints.end());
    all.insert(all.end(), inputs.upperPalatalPoints.begin(), inputs.upperPalatalPoints.end());
    all.insert(all.end(), inputs.lowerLingualPoints.begin(), inputs.lowerLingualPoints.end());

    guide.origin = meanPoint(all);

    const Vec3 upperCenter = meanPoint(inputs.upperVestibularPoints, inputs.upperPalatalPoints);
    const Vec3 lowerCenter = meanPoint(inputs.lowerVestibularPoints, inputs.lowerLingualPoints);
    guide.normal = normalized(upperCenter - lowerCenter, {0.0, 0.0, 1.0});

    Vec3 bestAxis;
    double bestLen2 = 0.0;
    for (size_t i = 0; i < all.size(); ++i) {
        const Vec3 pi = toVec3(all[i]);
        for (size_t j = i + 1; j < all.size(); ++j) {
            Vec3 d = toVec3(all[j]) - pi;
            d = d - guide.normal * dot(d, guide.normal);
            const double len2 = dot(d, d);
            if (len2 > bestLen2) {
                bestLen2 = len2;
                bestAxis = d;
            }
        }
    }
    guide.axisU = normalized(bestAxis, {1.0, 0.0, 0.0});
    guide.axisV = normalized(cross(guide.normal, guide.axisU), {0.0, 1.0, 0.0});

    const Vec3 outerCenter = meanPoint(inputs.upperVestibularPoints, inputs.lowerVestibularPoints);
    const Vec3 innerCenter = meanPoint(inputs.upperPalatalPoints, inputs.lowerLingualPoints);
    Vec3 vestibularDirection = outerCenter - innerCenter;
    vestibularDirection = vestibularDirection - guide.normal * dot(vestibularDirection, guide.normal);
    if (dot(guide.axisV, vestibularDirection) < 0.0)
        guide.axisV = guide.axisV * -1.0;

    std::vector<double> angles;
    angles.reserve(all.size());
    for (const auto& p : all) {
        const Vec3 d = toVec3(p) - guide.origin;
        angles.push_back(normalizeAngle(std::atan2(dot(d, guide.axisV), dot(d, guide.axisU))));
    }
    std::sort(angles.begin(), angles.end());
    if (angles.size() < 6)
        return guide;

    double largestGap = -1.0;
    size_t largestGapIndex = 0;
    for (size_t i = 0; i < angles.size(); ++i) {
        const double a = angles[i];
        const double b = (i + 1 < angles.size()) ? angles[i + 1] : angles.front() + 2.0 * kPi;
        const double gap = b - a;
        if (gap > largestGap) {
            largestGap = gap;
            largestGapIndex = i;
        }
    }

    // The largest empty angular gap normally corresponds to the posterior open
    // side of the arch. The generated splint follows the remaining guided arc.
    guide.startAngle = (largestGapIndex + 1 < angles.size())
        ? angles[largestGapIndex + 1]
        : angles.front();
    guide.endAngle = angles[largestGapIndex];
    if (guide.endAngle <= guide.startAngle)
        guide.endAngle += 2.0 * kPi;

    appendSamples(guide.outerRail, inputs.upperVestibularPoints, guide, guide.startAngle);
    appendSamples(guide.outerRail, inputs.lowerVestibularPoints, guide, guide.startAngle);
    appendSamples(guide.innerRail, inputs.upperPalatalPoints, guide, guide.startAngle);
    appendSamples(guide.innerRail, inputs.lowerLingualPoints, guide, guide.startAngle);
    auto byAngle = [](const RailSample& a, const RailSample& b) { return a.angle < b.angle; };
    std::sort(guide.outerRail.begin(), guide.outerRail.end(), byAngle);
    std::sort(guide.innerRail.begin(), guide.innerRail.end(), byAngle);

    guide.valid = guide.outerRail.size() >= 3 && guide.innerRail.size() >= 3 &&
                  (guide.endAngle - guide.startAngle) > kPi * 0.5;
    return guide;
}

struct GuidedRails
{
    std::vector<LocalPoint> upperOuter;
    std::vector<LocalPoint> upperInner;
    std::vector<LocalPoint> lowerOuter;
    std::vector<LocalPoint> lowerInner;
    bool valid = false;
};

GuidedRails buildGuidedRails(const SplintGenerationInputs& inputs,
                             const GuidedFootprint& guide)
{
    GuidedRails rails;
    if (!guide.valid)
        return rails;

    const double anteriorPosteriorExtensionMm = 2.8;
    rails.upperOuter = extendCurveEnds(sortedLocalCurve(inputs.upperVestibularPoints, guide),
                                       anteriorPosteriorExtensionMm);
    rails.upperInner = extendCurveEnds(sortedLocalCurve(inputs.upperPalatalPoints, guide),
                                       anteriorPosteriorExtensionMm);
    rails.lowerOuter = extendCurveEnds(sortedLocalCurve(inputs.lowerVestibularPoints, guide),
                                       anteriorPosteriorExtensionMm);
    rails.lowerInner = extendCurveEnds(sortedLocalCurve(inputs.lowerLingualPoints, guide),
                                       anteriorPosteriorExtensionMm);

    rails.valid =
        rails.upperOuter.size() >= 3 &&
        rails.upperInner.size() >= 3 &&
        rails.lowerOuter.size() >= 3 &&
        rails.lowerInner.size() >= 3;
    return rails;
}

std::vector<double> normalValues(vtkPolyData* mesh, const GuidedFootprint& guide)
{
    std::vector<double> values;
    if (!mesh || !guide.valid)
        return values;

    values.reserve(static_cast<size_t>(mesh->GetNumberOfPoints()));
    double p[3] = {};
    for (vtkIdType i = 0; i < mesh->GetNumberOfPoints(); ++i) {
        mesh->GetPoint(i, p);
        values.push_back(dot(Vec3{p[0], p[1], p[2]} - guide.origin, guide.normal));
    }
    return values;
}

Vec3 localToWorld(const GuidedFootprint& guide, double u, double v, double n)
{
    return guide.origin + guide.axisU * u + guide.axisV * v + guide.normal * n;
}

void copyPoint(const Vec3& p, double out[3])
{
    out[0] = p.x;
    out[1] = p.y;
    out[2] = p.z;
}

void shiftAlong(double p[3], const Vec3& axis, double amount)
{
    p[0] += axis.x * amount;
    p[1] += axis.y * amount;
    p[2] += axis.z * amount;
}

XYBounds occlusalBounds(vtkPolyData* upper, vtkPolyData* lower)
{
    XYBounds b;

    const auto upperZ = zValues(upper);
    const auto lowerZ = zValues(lower);
    const double upperCut = percentile(upperZ, 0.18);
    const double lowerCut = percentile(lowerZ, 0.82);

    double p[3] = {};
    if (upper) {
        for (vtkIdType i = 0; i < upper->GetNumberOfPoints(); ++i) {
            upper->GetPoint(i, p);
            if (p[2] <= upperCut)
                includePoint(b, p[0], p[1]);
        }
    }
    if (lower) {
        for (vtkIdType i = 0; i < lower->GetNumberOfPoints(); ++i) {
            lower->GetPoint(i, p);
            if (p[2] >= lowerCut)
                includePoint(b, p[0], p[1]);
        }
    }

    if (!b.valid && upper) {
        double bounds[6] = {};
        upper->GetBounds(bounds);
        includePoint(b, bounds[0], bounds[2]);
        includePoint(b, bounds[1], bounds[3]);
    }
    if (!b.valid && lower) {
        double bounds[6] = {};
        lower->GetBounds(bounds);
        includePoint(b, bounds[0], bounds[2]);
        includePoint(b, bounds[1], bounds[3]);
    }
    return b;
}

double indentationDelta(vtkImplicitPolyDataDistance* distance, const double p[3],
                        double depth, double influence)
{
    if (!distance)
        return 0.0;
    const double d = std::abs(distance->EvaluateFunction(const_cast<double*>(p)));
    if (d >= influence)
        return 0.0;
    const double t = 1.0 - (d / influence);
    return depth * t * t;
}

void addQuad(vtkCellArray* cells, vtkIdType a, vtkIdType b, vtkIdType c, vtkIdType d)
{
    vtkIdType ids[4] = {a, b, c, d};
    cells->InsertNextCell(4, ids);
}

vtkSmartPointer<vtkPolyData> smoothNormals(vtkPolyData* mesh, double filletMm = 0.0)
{
    if (!mesh || mesh->GetNumberOfPoints() <= 0)
        return nullptr;

    auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
    clean->SetInputData(mesh);
    clean->Update();

    auto smoother = vtkSmartPointer<vtkWindowedSincPolyDataFilter>::New();
    smoother->SetInputConnection(clean->GetOutputPort());
    smoother->SetNumberOfIterations(static_cast<int>(std::clamp(12.0 + filletMm * 5.0, 8.0, 36.0)));
    smoother->BoundarySmoothingOff();
    smoother->FeatureEdgeSmoothingOff();
    smoother->SetPassBand(std::clamp(0.12 - filletMm * 0.012, 0.06, 0.18));
    smoother->NormalizeCoordinatesOn();
    smoother->Update();

    auto normals = vtkSmartPointer<vtkPolyDataNormals>::New();
    normals->SetInputConnection(smoother->GetOutputPort());
    normals->ConsistencyOn();
    normals->AutoOrientNormalsOn();
    normals->SplittingOff();
    normals->Update();

    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(normals->GetOutput());
    return out;
}

vtkSmartPointer<vtkPolyData> tryBooleanDifference(vtkPolyData* splint,
                                                  vtkPolyData* upper,
                                                  vtkPolyData* lower,
                                                  double filletMm)
{
    if (!splint || (!upper && !lower))
        return nullptr;
    const vtkIdType upperPoints = upper ? upper->GetNumberOfPoints() : 0;
    const vtkIdType lowerPoints = lower ? lower->GetNumberOfPoints() : 0;
    if (upperPoints + lowerPoints > 180000)
        return nullptr;

    auto jawAppend = vtkSmartPointer<vtkAppendPolyData>::New();
    if (upper) jawAppend->AddInputData(upper);
    if (lower) jawAppend->AddInputData(lower);
    jawAppend->Update();

    auto splintTri = cleanTriangulate(splint);
    auto jawsTri = cleanTriangulate(jawAppend->GetOutput());
    if (!splintTri || !jawsTri)
        return nullptr;

    auto booleanOp = vtkSmartPointer<vtkBooleanOperationPolyDataFilter>::New();
    booleanOp->SetOperationToDifference();
    booleanOp->SetInputData(0, splintTri);
    booleanOp->SetInputData(1, jawsTri);
    booleanOp->Update();

    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(booleanOp->GetOutput());
    if (out->GetNumberOfPoints() <= 0 || out->GetNumberOfCells() <= 0)
        return nullptr;
    return smoothNormals(out, filletMm);
}

} // namespace

SplintGenerationResult SplintGenerator::Generate(const SplintGenerationInputs& inputs)
{
    SplintGenerationResult result;
    if (!inputs.upperOcclusion || !inputs.lowerOcclusion ||
        inputs.upperOcclusion->GetNumberOfPoints() <= 0 ||
        inputs.lowerOcclusion->GetNumberOfPoints() <= 0) {
        result.report = QStringLiteral("No hay mallas oclusales suficientes para generar la ferula.");
        return result;
    }

    auto upper = cleanTriangulate(inputs.upperOcclusion);
    auto lower = cleanTriangulate(inputs.lowerOcclusion);
    if (!upper || !lower) {
        result.report = QStringLiteral("No fue posible preparar las mallas para indentacion.");
        return result;
    }

    const GuidedFootprint guide = buildGuidedFootprint(inputs);
    const GuidedRails rails = buildGuidedRails(inputs, guide);

    double upperOcclusalZ = 0.0;
    double lowerOcclusalZ = 0.0;
    if (rails.valid) {
        const auto upperN = normalValues(upper, guide);
        const auto lowerN = normalValues(lower, guide);
        upperOcclusalZ = percentile(upperN, 0.06);
        lowerOcclusalZ = percentile(lowerN, 0.94);
    } else {
        const auto upperZ = zValues(upper);
        const auto lowerZ = zValues(lower);
        upperOcclusalZ = percentile(upperZ, 0.06);
        lowerOcclusalZ = percentile(lowerZ, 0.94);
    }
    const double gap = std::abs(upperOcclusalZ - lowerOcclusalZ);
    const double thickness = std::max(inputs.thicknessMm, std::min(7.0, gap + 0.8));
    const double zCenter = 0.5 * (upperOcclusalZ + lowerOcclusalZ);

    double cx = 0.0;
    double cy = 0.0;
    double rxOuter = 0.0;
    double ryOuter = 0.0;
    double rxInner = 0.0;
    double ryInner = 0.0;
    if (!rails.valid) {
        XYBounds b = occlusalBounds(upper, lower);
        includeGuidePoints(b, inputs.upperVestibularPoints);
        includeGuidePoints(b, inputs.upperPalatalPoints);
        includeGuidePoints(b, inputs.lowerVestibularPoints);
        includeGuidePoints(b, inputs.lowerLingualPoints);
        if (!b.valid) {
            result.report = QStringLiteral("No se pudo estimar el contorno oclusal.");
            return result;
        }

        cx = 0.5 * (b.xMin + b.xMax);
        cy = 0.5 * (b.yMin + b.yMax);
        rxOuter = std::max(22.0, 0.5 * (b.xMax - b.xMin) + inputs.borderPaddingMm);
        ryOuter = std::max(24.0, 0.5 * (b.yMax - b.yMin) + inputs.borderPaddingMm);
        rxInner = std::max(8.0, rxOuter - inputs.archWidthMm);
        ryInner = std::max(10.0, ryOuter - inputs.archWidthMm);
    }

    auto upperDistance = vtkSmartPointer<vtkImplicitPolyDataDistance>::New();
    upperDistance->SetInput(upper);
    auto lowerDistance = vtkSmartPointer<vtkImplicitPolyDataDistance>::New();
    lowerDistance->SetInput(lower);

    auto points = vtkSmartPointer<vtkPoints>::New();
    auto cells = vtkSmartPointer<vtkCellArray>::New();

    const int segments = 112;
    const double gapHalfDeg = 34.0;
    const double start = (90.0 + gapHalfDeg) * kPi / 180.0;
    const double end = (450.0 - gapHalfDeg) * kPi / 180.0;
    const double zTopBase = zCenter + 0.5 * thickness;
    const double zBottomBase = zCenter - 0.5 * thickness;
    const double influence = std::max(1.8, inputs.indentationDepthMm + 1.2);

    std::vector<std::array<vtkIdType, 4>> ids;
    ids.reserve(static_cast<size_t>(segments + 1));

    for (int i = 0; i <= segments; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(segments);
        const double a = rails.valid
            ? guide.startAngle + (guide.endAngle - guide.startAngle) * t
            : start + (end - start) * t;
        const double ca = std::cos(a);
        const double sa = std::sin(a);

        double topOuter[3] = {};
        double topInner[3] = {};
        double botOuter[3] = {};
        double botInner[3] = {};

        if (rails.valid) {
            LocalPoint upperOuter = sampleLocalCurve(rails.upperOuter, t);
            LocalPoint upperInner = sampleLocalCurve(rails.upperInner, t);
            LocalPoint lowerOuter = sampleLocalCurve(rails.lowerOuter, t);
            LocalPoint lowerInner = sampleLocalCurve(rails.lowerInner, t);

            expandCurvePair(upperOuter, upperInner, inputs.borderPaddingMm);
            expandCurvePair(lowerOuter, lowerInner, inputs.borderPaddingMm);

            copyPoint(localToWorld(guide, upperOuter.u, upperOuter.v, zTopBase), topOuter);
            copyPoint(localToWorld(guide, upperInner.u, upperInner.v, zTopBase), topInner);
            copyPoint(localToWorld(guide, lowerOuter.u, lowerOuter.v, zBottomBase), botOuter);
            copyPoint(localToWorld(guide, lowerInner.u, lowerInner.v, zBottomBase), botInner);
        } else {
            const double ox = cx + rxOuter * ca;
            const double oy = cy + ryOuter * sa;
            const double ix = cx + rxInner * ca;
            const double iy = cy + ryInner * sa;

            topOuter[0] = ox; topOuter[1] = oy; topOuter[2] = zTopBase;
            topInner[0] = ix; topInner[1] = iy; topInner[2] = zTopBase;
            botOuter[0] = ox; botOuter[1] = oy; botOuter[2] = zBottomBase;
            botInner[0] = ix; botInner[1] = iy; botInner[2] = zBottomBase;
        }

        const double topOuterDent = inputs.useUpperImpression
            ? indentationDelta(upperDistance, topOuter, inputs.indentationDepthMm, influence) : 0.0;
        const double topInnerDent = inputs.useUpperImpression
            ? indentationDelta(upperDistance, topInner, inputs.indentationDepthMm, influence) : 0.0;
        const double botOuterDent = inputs.useLowerImpression
            ? indentationDelta(lowerDistance, botOuter, inputs.indentationDepthMm, influence) : 0.0;
        const double botInnerDent = inputs.useLowerImpression
            ? indentationDelta(lowerDistance, botInner, inputs.indentationDepthMm, influence) : 0.0;
        if (rails.valid) {
            shiftAlong(topOuter, guide.normal, -topOuterDent);
            shiftAlong(topInner, guide.normal, -topInnerDent);
            shiftAlong(botOuter, guide.normal, botOuterDent);
            shiftAlong(botInner, guide.normal, botInnerDent);
        } else {
            topOuter[2] -= topOuterDent;
            topInner[2] -= topInnerDent;
            botOuter[2] += botOuterDent;
            botInner[2] += botInnerDent;
        }

        ids.push_back({
            points->InsertNextPoint(topOuter),
            points->InsertNextPoint(topInner),
            points->InsertNextPoint(botOuter),
            points->InsertNextPoint(botInner)
        });
    }

    for (int i = 0; i < segments; ++i) {
        const auto& a = ids[static_cast<size_t>(i)];
        const auto& b2 = ids[static_cast<size_t>(i + 1)];
        addQuad(cells, a[0], b2[0], b2[1], a[1]); // top
        addQuad(cells, a[2], a[3], b2[3], b2[2]); // bottom
        addQuad(cells, a[0], a[2], b2[2], b2[0]); // outer wall
        addQuad(cells, a[1], b2[1], b2[3], a[3]); // inner wall
    }
    addQuad(cells, ids.front()[0], ids.front()[1], ids.front()[3], ids.front()[2]);
    addQuad(cells, ids.back()[0], ids.back()[2], ids.back()[3], ids.back()[1]);

    auto splint = vtkSmartPointer<vtkPolyData>::New();
    splint->SetPoints(points);
    splint->SetPolys(cells);
    splint->Modified();

    auto finalMesh = smoothNormals(splint, inputs.filletMm);
    if (inputs.tryBooleanIndentation) {
        vtkPolyData* upperBoolean = inputs.useUpperImpression ? upper.Get() : nullptr;
        vtkPolyData* lowerBoolean = inputs.useLowerImpression ? lower.Get() : nullptr;
        if (auto booleanMesh = tryBooleanDifference(finalMesh, upperBoolean, lowerBoolean, inputs.filletMm)) {
            const vtkIdType baseCells = finalMesh ? finalMesh->GetNumberOfCells() : 0;
            const vtkIdType boolCells = booleanMesh->GetNumberOfCells();
            const bool plausible =
                baseCells <= 0 ||
                (boolCells > static_cast<vtkIdType>(baseCells * 0.25) &&
                 boolCells < static_cast<vtkIdType>(baseCells * 6.0));
            if (plausible) {
                finalMesh = booleanMesh;
                result.booleanApplied = true;
            }
        }
    }

    result.mesh = finalMesh;
    result.report = QStringLiteral("%1: grosor %2 mm, contorno %3, indentacion %4.")
                        .arg(inputs.name)
                        .arg(thickness, 0, 'f', 1)
                        .arg(rails.valid ? QStringLiteral("por puntos") : QStringLiteral("automatico"))
                        .arg(result.booleanApplied ? QStringLiteral("booleana") : QStringLiteral("por distancia"));
    return result;
}

