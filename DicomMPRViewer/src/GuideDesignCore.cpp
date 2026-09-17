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
    const double detail = std::clamp(params.base.smallestDetailMm, 0.05, 2.0);
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
    std::vector<ImplicitCore::NodePtr> cutters;
    int added = 0, subtracted = 0;
    for (const GuideFigure& figure : figures) {
        QString figureError;
        const auto node = FigureNode(figure, detail, &figureError);
        if (!node) {
            result.error = figureError.isEmpty() ? QStringLiteral("Una figura no tiene geometría.") : figureError;
            return result;
        }
        if (figure.operation == GuideFigureOperation::Add) {
            solids.push_back(node);
            ++added;
        } else {
            cutters.push_back(node);
            ++subtracted;
        }
    }
    const auto grown = ImplicitCore::Union(solids);
    double bounds[6] = {};
    if (!ImplicitCore::Bounds(grown, bounds)) {
        result.error = QStringLiteral("La base de la guía no está acotada.");
        return result;
    }

    // Slots and holes are carved into the same field as the base, and the whole guide is contoured once.
    // Every slot is clipped to the marked region shrunk by the edge margin, so material is always left
    // around it and the guide comes out in one piece.
    const double edgeMargin = std::clamp(params.edgeMarginMm, 0.0, 20.0);
    const auto insideRegion = ImplicitCore::Offset(GuideBaseCore::RegionPrism(region, prepared.spanMm), -edgeMargin);
    for (const GuideSlot& slot : slotPlan) {
        QString pathError;
        const auto slab = CutSlotCore::SlotNode(slot.path, bounds, params.slot, cancel, &pathError);
        if (!slab) {
            result.error = pathError.isEmpty() ? QStringLiteral("Trayectoria de corte no válida.") : pathError;
            return result;
        }
        std::vector<ImplicitCore::NodePtr> limits{slab, insideRegion};
        if (slot.hasExtent) {
            // Between the two ends the user placed: a half space at each, facing along the slot.
            const Vec3 along{slot.end[0] - slot.start[0], slot.end[1] - slot.start[1], slot.end[2] - slot.start[2]};
            const double length = std::sqrt(along[0] * along[0] + along[1] * along[1] + along[2] * along[2]);
            if (length < 1e-6) {
                result.error = QStringLiteral("Los dos extremos de la ranura están en el mismo punto.");
                return result;
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

    const auto solid = cutters.empty() ? grown : ImplicitCore::Subtract(grown, ImplicitCore::Union(cutters));
    ImplicitCore::PolygonizeOptions options;
    options.smoothingIterations = params.base.smoothingIterations;
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
