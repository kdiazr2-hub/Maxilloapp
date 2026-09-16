#include "GuideDesignCore.h"

#include <vtkPolyData.h>
#include <vtkPolyDataConnectivityFilter.h>

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
    prepared.wrapField = ImplicitCore::BakeMeshField(wrap, detail, clearance + thickness + 3.0, cancel, &error);
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

GuideDesignResult Build(const GuidePreparation& prepared, const GuideContour& contour,
                        const std::vector<OsteotomyPath>& paths, const std::vector<GuideFixationHole>& holes,
                        const GuideDesignParams& params, const std::atomic<bool>* cancel)
{
    GuideDesignResult result;
    if (!prepared.ok || !prepared.wrapField) {
        result.error = prepared.error.isEmpty() ? QStringLiteral("La envolvente no está preparada.") : prepared.error;
        return result;
    }
    if (!GuideBaseCore::ContourValid(contour, &result.error))
        return result;

    const double detail = std::clamp(params.base.smallestDetailMm, 0.05, 2.0);
    Vec3 axis{0.0, 0.0, 0.0};
    for (const auto& point : contour) {
        const auto normal = GuideBaseCore::NormalAt(*prepared.wrapField, point);
        axis = {axis[0] + normal[0], axis[1] + normal[1], axis[2] + normal[2]};
    }
    const double axisLength = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    result.projectionAxis = axisLength > 1e-9
                                ? Vec3{axis[0] / axisLength, axis[1] / axisLength, axis[2] / axisLength}
                                : Vec3{0.0, 0.0, 1.0};

    const auto base = GuideBaseCore::BaseNode(prepared.wrapField, contour, params.base, result.projectionAxis,
                                              prepared.spanMm);
    if (!base) {
        result.error = QStringLiteral("No se pudo construir la base de la guía.");
        return result;
    }
    double bounds[6] = {};
    if (!ImplicitCore::Bounds(base, bounds)) {
        result.error = QStringLiteral("La base de la guía no está acotada.");
        return result;
    }

    // Slots and holes are carved into the same field as the base, and the whole guide is contoured once.
    std::vector<ImplicitCore::NodePtr> cutters;
    for (const OsteotomyPath& path : paths) {
        QString pathError;
        const auto node = CutSlotCore::SlotNode(path, bounds, params.slot, cancel, &pathError);
        if (!node) {
            result.error = pathError.isEmpty() ? QStringLiteral("Trayectoria de corte no válida.") : pathError;
            return result;
        }
        cutters.push_back(node);
    }
    const double holeLength = std::clamp(params.holeLengthMm, 1.0, 100.0);
    for (const GuideFixationHole& hole : holes) {
        if (!(hole.diameterMm > 0.0))
            continue;
        cutters.push_back(ImplicitCore::Cylinder(hole.center, hole.axis, 0.5 * hole.diameterMm, 0.5 * holeLength));
    }

    const auto solid = cutters.empty() ? base : ImplicitCore::Subtract(base, ImplicitCore::Union(cutters));
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
                                   "%5 pieza(s), %6 triángulos.")
                        .arg(std::clamp(params.base.thicknessMm, 0.3, 20.0), 0, 'f', 2)
                        .arg(std::clamp(params.base.clearanceMm, 0.0, 5.0), 0, 'f', 2)
                        .arg(paths.size())
                        .arg(holes.size())
                        .arg(result.pieces)
                        .arg(result.mesh->GetNumberOfPolys());
    result.ok = true;
    return result;
}
}
