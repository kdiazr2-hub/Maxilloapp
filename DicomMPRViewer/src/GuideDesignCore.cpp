#include "GuideDesignCore.h"

#include <vtkCubeSource.h>
#include <vtkCylinderSource.h>
#include <vtkMatrix4x4.h>
#include <vtkPolyData.h>
#include <vtkPolyDataConnectivityFilter.h>
#include <vtkSphereSource.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>
#include <vtkTriangleFilter.h>
#include <vtkTubeFilter.h>
#include <vtkPoints.h>
#include <vtkCellArray.h>

#include <algorithm>
#include <cmath>

namespace
{
using ImplicitCore::Vec3;

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

vtkSmartPointer<vtkMatrix4x4> toMatrix(const std::array<double, 16>& m)
{
    auto matrix = vtkSmartPointer<vtkMatrix4x4>::New();
    matrix->DeepCopy(m.data());
    return matrix;
}

Vec3 normalized(const Vec3& v, const Vec3& fallback)
{
    const double len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return len > 1e-9 ? Vec3{v[0] / len, v[1] / len, v[2] / len} : fallback;
}

Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

std::vector<Vec3> curvedTubeSamples(const GuideFigure& figure)
{
    if (figure.controlPoints.size() < 3)
        return {};
    const Vec3& a = figure.controlPoints[0];
    const Vec3& b = figure.controlPoints[1];
    const Vec3& c = figure.controlPoints[2];
    // Convert the requested midpoint into a quadratic control point. A regular
    // Bézier only approaches its control point; this form passes through b at t=.5.
    const Vec3 q{2.0 * b[0] - 0.5 * (a[0] + c[0]),
                 2.0 * b[1] - 0.5 * (a[1] + c[1]),
                 2.0 * b[2] - 0.5 * (a[2] + c[2])};
    std::vector<Vec3> samples;
    samples.reserve(49);
    for (int i = 0; i <= 48; ++i) {
        const double t = i / 48.0;
        const double s = 1.0 - t;
        samples.push_back({s * s * a[0] + 2.0 * s * t * q[0] + t * t * c[0],
                           s * s * a[1] + 2.0 * s * t * q[1] + t * t * c[1],
                           s * s * a[2] + 2.0 * s * t * q[2] + t * t * c[2]});
    }
    return samples;
}

// Everything that must stay empty in the finished guide: the saw slots, the fixation holes and the figures
// the user subtracts. Both the build and the edit session's keep-out field come from here, so an edit can
// never fill in what the plan cut away. Returns an empty list and sets `error` when one of them is unusable.
std::vector<ImplicitCore::NodePtr> cutterNodes(const GuideRegion& region, double spanMm,
                                               const std::vector<GuideSlot>& slotPlan,
                                               const std::vector<GuideFixationHole>& holes,
                                               const std::vector<GuideFigure>& figures,
                                               const GuideDesignParams& params, const double bounds[6],
                                               const std::atomic<bool>* cancel, QString* error)
{
    std::vector<ImplicitCore::NodePtr> cutters;
    // Guides are inspected and printed at close range. Keep their final contour finer than the preview/wrap,
    // including projects saved with the older 0.30 mm default.
    const double detail = std::min(0.25, std::clamp(params.base.smallestDetailMm, 0.05, 2.0));
    for (const GuideFigure& figure : figures) {
        if (figure.operation != GuideFigureOperation::Subtract)
            continue;
        QString figureError;
        const auto node = GuideDesignCore::FigureNode(figure, detail, &figureError);
        if (!node) {
            if (error)
                *error = figureError.isEmpty() ? QStringLiteral("Una figura no tiene geometría.") : figureError;
            return {};
        }
        cutters.push_back(node);
    }
    // Every slot is clipped to the marked region shrunk by the edge margin, so material is always left
    // around it and the guide comes out in one piece.
    const double edgeMargin = std::clamp(params.edgeMarginMm, 0.0, 20.0);
    const auto insideRegion = ImplicitCore::Offset(GuideBaseCore::RegionPrism(region, spanMm), -edgeMargin);
    for (const GuideSlot& slot : slotPlan) {
        QString pathError;
        const auto slab = CutSlotCore::SlotNode(slot.path, bounds, params.slot, cancel, &pathError);
        if (!slab) {
            if (error)
                *error = pathError.isEmpty() ? QStringLiteral("Trayectoria de corte no válida.") : pathError;
            return {};
        }
        std::vector<ImplicitCore::NodePtr> limits{slab, insideRegion};
        if (slot.hasExtent) {
            // Between the two ends the user placed: a half space at each, facing along the slot.
            const Vec3 along{slot.end[0] - slot.start[0], slot.end[1] - slot.start[1], slot.end[2] - slot.start[2]};
            const double length = std::sqrt(along[0] * along[0] + along[1] * along[1] + along[2] * along[2]);
            if (length < 1e-6) {
                if (error)
                    *error = QStringLiteral("Los dos extremos de la ranura están en el mismo punto.");
                return {};
            }
            const Vec3 direction{along[0] / length, along[1] / length, along[2] / length};
            limits.push_back(ImplicitCore::HalfSpace(slot.start, {-direction[0], -direction[1], -direction[2]}));
            limits.push_back(ImplicitCore::HalfSpace(slot.end, direction));
        }
        cutters.push_back(ImplicitCore::Intersect(limits));
    }
    const double holeLength = std::clamp(params.holeLengthMm, 1.0, 100.0);
    for (const GuideFixationHole& hole : holes) {
        if (!(hole.diameterMm > 0.0))
            continue;
        cutters.push_back(ImplicitCore::Cylinder(hole.center, hole.axis, 0.5 * hole.diameterMm, 0.5 * holeLength));
    }
    return cutters;
}
} // namespace

namespace GuideDesignCore
{
GuidePreparation Prepare(vtkPolyData* wrap, const GuideDesignParams& params, const std::atomic<bool>* cancel)
{
    GuidePreparation prepared;
    if (!wrap || wrap->GetNumberOfPolys() == 0) {
        prepared.error = QStringLiteral("Falta la malla envolvente (wrap).");
        return prepared;
    }
    const double thickness = std::clamp(params.base.thicknessMm, 0.3, 20.0);
    const double clearance = std::clamp(params.base.clearanceMm, 0.0, 5.0);
    const double detail = std::clamp(params.base.smallestDetailMm, 0.05, 2.0);

    QString error;
    // The field must reach past the far face of the wall and past the slots.
    prepared.wrapField =
        ImplicitCore::BakeMeshField(wrap, detail, std::max(6.0, clearance + thickness + 3.0), cancel, &error);
    if (!prepared.wrapField) {
        prepared.error = error.isEmpty() ? QStringLiteral("No se pudo medir la envolvente.") : error;
        return prepared;
    }
    double bounds[6] = {};
    wrap->GetBounds(bounds);
    prepared.spanMm = std::sqrt(std::pow(bounds[1] - bounds[0], 2.0) + std::pow(bounds[3] - bounds[2], 2.0) +
                                std::pow(bounds[5] - bounds[4], 2.0));
    prepared.spacingMm = prepared.wrapField->spacingMm;
    prepared.ok = true;
    return prepared;
}

std::array<double, 3> SurfaceNormalAt(const GuidePreparation& prepared, const std::array<double, 3>& point)
{
    if (!prepared.wrapField)
        return {0.0, 0.0, 1.0};
    return GuideBaseCore::NormalAt(*prepared.wrapField, point);
}

std::array<double, 16> FrameAt(const std::array<double, 3>& center, const std::array<double, 3>& zAxis)
{
    const Vec3 z = normalized(zAxis, {0.0, 0.0, 1.0});
    const Vec3 helper = std::abs(z[2]) < 0.9 ? Vec3{0.0, 0.0, 1.0} : Vec3{1.0, 0.0, 0.0};
    const Vec3 x = normalized(cross(helper, z), {1.0, 0.0, 0.0});
    const Vec3 y = cross(z, x);
    return {x[0], y[0], z[0], center[0], x[1], y[1], z[1], center[1], x[2], y[2], z[2], center[2], 0.0, 0.0, 0.0, 1.0};
}

std::vector<std::array<double, 3>> OutwardTubeControlPoints(
    const std::vector<std::array<double, 3>>& points,
    const std::array<double, 3>& outwardNormal,
    double minimumBulgeMm)
{
    if (points.size() < 3)
        return points;

    std::vector<std::array<double, 3>> corrected = points;
    const Vec3 normal = normalized(outwardNormal, {0.0, 1.0, 0.0});
    const Vec3 midpoint{0.5 * (points[0][0] + points[2][0]),
                        0.5 * (points[0][1] + points[2][1]),
                        0.5 * (points[0][2] + points[2][2])};
    const Vec3 offset{points[1][0] - midpoint[0], points[1][1] - midpoint[1],
                      points[1][2] - midpoint[2]};
    const double signedBulge = offset[0] * normal[0] + offset[1] * normal[1] + offset[2] * normal[2];
    const double outwardBulge = std::max(std::abs(signedBulge), std::max(0.0, minimumBulgeMm));
    const double correction = outwardBulge - signedBulge;
    for (int axis = 0; axis < 3; ++axis)
        corrected[1][static_cast<size_t>(axis)] += correction * normal[static_cast<size_t>(axis)];
    return corrected;
}

std::vector<std::array<double, 3>> CurvedTubeCenterline(const GuideFigure& figure)
{
    return curvedTubeSamples(figure);
}

ImplicitCore::NodePtr FigureNode(const GuideFigure& figure, double detailMm, QString* error)
{
    ImplicitCore::NodePtr local;
    switch (figure.shape) {
    case GuideFigureShape::Cylinder:
        local = ImplicitCore::Cylinder({0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, 0.5 * std::max(0.05, figure.diameterMm),
                                       0.5 * std::max(0.05, figure.lengthMm));
        break;
    case GuideFigureShape::Box:
        local = ImplicitCore::Box({0.0, 0.0, 0.0}, {0.5 * std::max(0.05, figure.widthMm),
                                                   0.5 * std::max(0.05, figure.heightMm),
                                                   0.5 * std::max(0.05, figure.depthMm)});
        break;
    case GuideFigureShape::Sphere:
        local = ImplicitCore::Sphere({0.0, 0.0, 0.0}, 0.5 * std::max(0.05, figure.diameterMm));
        break;
    case GuideFigureShape::Mesh:
        if (!figure.mesh || figure.mesh->GetNumberOfPolys() == 0) {
            if (error)
                *error = QStringLiteral("La figura importada no tiene geometría.");
            return nullptr;
        }
        local = ImplicitCore::MeshField(figure.mesh, std::max(0.1, detailMm), 2.0, nullptr, error);
        break;
    case GuideFigureShape::CurvedTube: {
        const auto samples = curvedTubeSamples(figure);
        if (samples.size() < 2) {
            if (error)
                *error = QStringLiteral("El tubo curvo necesita tres puntos.");
            return nullptr;
        }
        std::vector<ImplicitCore::NodePtr> pieces;
        pieces.reserve(samples.size() - 1);
        const double radius = 0.5 * std::clamp(figure.diameterMm, 0.5, 10.0);
        for (size_t i = 1; i < samples.size(); ++i)
            pieces.push_back(ImplicitCore::Capsule(samples[i - 1], samples[i], radius));
        local = ImplicitCore::Union(pieces);
        break;
    }
    }
    if (!local)
        return nullptr;
    return ImplicitCore::Transformed(local, toMatrix(figure.matrix));
}

vtkSmartPointer<vtkPolyData> FigurePreview(const GuideFigure& figure)
{
    vtkSmartPointer<vtkPolyData> local;
    switch (figure.shape) {
    case GuideFigureShape::Cylinder: {
        // vtkCylinderSource runs along y; the figure's length is along local z.
        auto source = vtkSmartPointer<vtkCylinderSource>::New();
        source->SetRadius(0.5 * figure.diameterMm);
        source->SetHeight(figure.lengthMm);
        source->SetResolution(40);
        source->Update();
        auto toZ = vtkSmartPointer<vtkTransform>::New();
        toZ->RotateX(90.0);
        auto filter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
        filter->SetInputConnection(source->GetOutputPort());
        filter->SetTransform(toZ);
        filter->Update();
        local = filter->GetOutput();
        break;
    }
    case GuideFigureShape::Box: {
        auto source = vtkSmartPointer<vtkCubeSource>::New();
        source->SetXLength(figure.widthMm);
        source->SetYLength(figure.heightMm);
        source->SetZLength(figure.depthMm);
        source->Update();
        local = source->GetOutput();
        break;
    }
    case GuideFigureShape::Sphere: {
        auto source = vtkSmartPointer<vtkSphereSource>::New();
        source->SetRadius(0.5 * figure.diameterMm);
        source->SetThetaResolution(32);
        source->SetPhiResolution(24);
        source->Update();
        local = source->GetOutput();
        break;
    }
    case GuideFigureShape::Mesh:
        local = figure.mesh;
        break;
    case GuideFigureShape::CurvedTube: {
        const auto samples = curvedTubeSamples(figure);
        if (samples.size() < 2)
            return nullptr;
        auto points = vtkSmartPointer<vtkPoints>::New();
        for (const auto& point : samples)
            points->InsertNextPoint(point.data());
        auto lines = vtkSmartPointer<vtkCellArray>::New();
        lines->InsertNextCell(static_cast<vtkIdType>(samples.size()));
        for (vtkIdType i = 0; i < static_cast<vtkIdType>(samples.size()); ++i)
            lines->InsertCellPoint(i);
        auto curve = vtkSmartPointer<vtkPolyData>::New();
        curve->SetPoints(points);
        curve->SetLines(lines);
        auto tube = vtkSmartPointer<vtkTubeFilter>::New();
        tube->SetInputData(curve);
        tube->SetRadius(0.5 * std::clamp(figure.diameterMm, 0.5, 10.0));
        tube->SetNumberOfSides(24);
        tube->CappingOn();
        tube->Update();
        local = tube->GetOutput();
        break;
    }
    }
    if (!local)
        return nullptr;
    auto triangles = vtkSmartPointer<vtkTriangleFilter>::New();
    triangles->SetInputData(local);
    auto place = vtkSmartPointer<vtkTransform>::New();
    place->SetMatrix(toMatrix(figure.matrix));
    auto filter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
    filter->SetInputConnection(triangles->GetOutputPort());
    filter->SetTransform(place);
    filter->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(filter->GetOutput());
    return out;
}

GuideDesignResult Build(const GuidePreparation& prepared, const GuideContour& contour,
                        const std::vector<GuideSlot>& slotPlan, const std::vector<GuideFixationHole>& holes,
                        const GuideDesignParams& params, const std::atomic<bool>* cancel)
{
    return Build(prepared, contour, slotPlan, holes, {}, params, cancel);
}

ImplicitCore::NodePtr KeepOutNode(const GuidePreparation& prepared, const GuideRegion& region,
                                  const std::vector<GuideSlot>& slotPlan, const std::vector<GuideFixationHole>& holes,
                                  const std::vector<GuideFigure>& figures, const GuideDesignParams& params,
                                  const double bounds[6], const std::atomic<bool>* cancel, QString* error)
{
    if (!bounds || !region.valid) {
        if (error)
            *error = QStringLiteral("No hay zona marcada para calcular el volumen protegido.");
        return nullptr;
    }
    QString cutterError;
    const auto cutters =
        cutterNodes(region, prepared.spanMm, slotPlan, holes, figures, params, bounds, cancel, &cutterError);
    if (!cutterError.isEmpty()) {
        if (error)
            *error = cutterError;
        return nullptr;
    }
    return cutters.empty() ? nullptr : ImplicitCore::Union(cutters);
}

GuideDesignResult Build(const GuidePreparation& prepared, const GuideContour& contour,
                        const std::vector<GuideSlot>& slotPlan, const std::vector<GuideFixationHole>& holes,
                        const std::vector<GuideFigure>& figures, const GuideDesignParams& params,
                        const std::atomic<bool>* cancel)
{
    GuideDesignResult result;
    if (!prepared.ok || !prepared.wrapField) {
        result.error = prepared.error.isEmpty() ? QStringLiteral("La envolvente no está preparada.") : prepared.error;
        return result;
    }
    if (!GuideBaseCore::ContourValid(contour, &result.error))
        return result;
    return Build(prepared, GuideBaseCore::MakeRegion(prepared.wrapField, contour, params.base), slotPlan, holes,
                 figures, params, cancel);
}

GuideDesignResult Build(const GuidePreparation& prepared, const GuideRegion& region,
                        const std::vector<GuideSlot>& slotPlan, const std::vector<GuideFixationHole>& holes,
                        const std::vector<GuideFigure>& figures, const GuideDesignParams& params,
                        const std::atomic<bool>* cancel)
{
    GuideDesignResult result;
    if (!prepared.ok || !prepared.wrapField) {
        result.error = prepared.error.isEmpty() ? QStringLiteral("La envolvente no está preparada.") : prepared.error;
        return result;
    }
    // Guides are inspected and printed at close range. Keep their final contour finer than the preview/wrap,
    // including projects saved with the older 0.30 mm default.
    const double detail = std::min(0.25, std::clamp(params.base.smallestDetailMm, 0.05, 2.0));
    if (!region.valid) {
        result.error = region.error.isEmpty() ? QStringLiteral("Marque la zona de apoyo de la guía.") : region.error;
        return result;
    }
    result.projectionAxis = region.axis;
    const auto base = GuideBaseCore::BaseNode(prepared.wrapField, region, params.base, prepared.spanMm);
    if (!base) {
        result.error = QStringLiteral("No se pudo construir la base de la guía.");
        return result;
    }

    // Added figures grow the guide before anything is cut from it.
    std::vector<ImplicitCore::NodePtr> solids{base};
    int added = 0, subtracted = 0;
    for (const GuideFigure& figure : figures) {
        if (figure.operation == GuideFigureOperation::Subtract) {
            ++subtracted;
            continue;
        }
        QString figureError;
        const auto node = FigureNode(figure, detail, &figureError);
        if (!node) {
            result.error = figureError.isEmpty() ? QStringLiteral("Una figura no tiene geometría.") : figureError;
            return result;
        }
        solids.push_back(node);
        ++added;
    }
    // Add a short annular boss around every fixation hole. The through-hole is
    // subtracted later, leaving a positive stop/collar like a surgical drill guide.
    const double wallOffset = std::clamp(params.base.clearanceMm, 0.0, 5.0) +
                              std::clamp(params.base.thicknessMm, 0.3, 20.0);
    const double collarHeight = std::clamp(params.holeCollarHeightMm, 0.0, 5.0);
    const double collarWidth = std::clamp(params.holeCollarWidthMm, 0.0, 5.0);
    if (collarHeight > 0.0 && collarWidth > 0.0) {
        for (const GuideFixationHole& hole : holes) {
            if (!(hole.diameterMm > 0.0))
                continue;
            const Vec3 axis = normalized(hole.axis, {0.0, 0.0, 1.0});
            // Sink half the boss into the guide wall so polygonization cannot
            // leave a numerically tangent, detached collar.
            const Vec3 center{hole.center[0] + axis[0] * (wallOffset + 0.25 * collarHeight),
                              hole.center[1] + axis[1] * (wallOffset + 0.25 * collarHeight),
                              hole.center[2] + axis[2] * (wallOffset + 0.25 * collarHeight)};
            solids.push_back(ImplicitCore::Cylinder(center, axis,
                0.5 * hole.diameterMm + collarWidth, 0.75 * collarHeight));
        }
    }
    const auto grown = ImplicitCore::Union(solids);
    double bounds[6] = {};
    if (!ImplicitCore::Bounds(grown, bounds)) {
        result.error = QStringLiteral("La base de la guía no está acotada.");
        return result;
    }

    // Slots, holes and subtracted figures are carved into the same field as the base, and the whole guide
    // is contoured once.
    QString cutterError;
    const auto cutters =
        cutterNodes(region, prepared.spanMm, slotPlan, holes, figures, params, bounds, cancel, &cutterError);
    if (!cutterError.isEmpty()) {
        result.error = cutterError;
        return result;
    }

    const auto solid = cutters.empty() ? grown : ImplicitCore::Subtract(grown, ImplicitCore::Union(cutters));
    ImplicitCore::PolygonizeOptions options;
    options.smoothingIterations = std::max(params.base.smoothingIterations, 70);
    options.passBand = 0.02; // smoother support and rim while the implicit cutters retain slots and collars
    options.repair = true;
    const ImplicitCore::BuildResult built = ImplicitCore::Build(solid, bounds, detail, options, cancel);
    if (!built.ok) {
        result.error = built.error.isEmpty() ? QStringLiteral("La guía quedó vacía.") : built.error;
        return result;
    }
    result.mesh = built.mesh;
    result.spacingMm = detail;
    result.pieces = shellsOf(result.mesh);
    result.report = QStringLiteral("Guía: espesor %1 mm, holgura %2 mm, %3 ranura(s), %4 agujero(s), "
                                   "%5 figura(s) sumada(s), %6 restada(s), %7 pieza(s), %8 triángulos.")
                        .arg(std::clamp(params.base.thicknessMm, 0.3, 20.0), 0, 'f', 2)
                        .arg(std::clamp(params.base.clearanceMm, 0.0, 5.0), 0, 'f', 2)
                        .arg(slotPlan.size())
                        .arg(holes.size())
                        .arg(added)
                        .arg(subtracted)
                        .arg(result.pieces)
                        .arg(result.mesh->GetNumberOfPolys());
    result.ok = true;
    return result;
}
}
