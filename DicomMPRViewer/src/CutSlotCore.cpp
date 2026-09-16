#include "CutSlotCore.h"

#include "ImplicitCore.h"

#include <vtkPolyData.h>
#include <vtkPolyDataConnectivityFilter.h>

#include <algorithm>

namespace
{
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

namespace CutSlotCore
{
CutSlotResult CutSlots(vtkPolyData* base, const std::vector<OsteotomyPath>& paths, const CutSlotParams& params,
                       const std::atomic<bool>* cancel)
{
    CutSlotResult result;
    if (!base || base->GetNumberOfPolys() == 0) {
        result.error = QStringLiteral("Falta la base de la guía.");
        return result;
    }
    if (paths.empty()) {
        result.error = QStringLiteral("No hay trayectorias de osteotomía para las ranuras.");
        return result;
    }
    const double blade = std::clamp(params.bladeThicknessMm, 0.2, 3.0);
    const double detail = std::clamp(params.smallestDetailMm, 0.05, 2.0);
    const double extension = std::clamp(params.extensionMm, 0.0, 20.0);

    QString error;
    const auto baseField = ImplicitCore::MeshField(base, detail, blade + extension + 2.0, cancel, &error);
    if (!baseField) {
        result.error = error.isEmpty() ? QStringLiteral("No se pudo medir la base.") : error;
        return result;
    }
    result.spacingMm = detail;

    double bounds[6] = {};
    base->GetBounds(bounds);
    std::vector<ImplicitCore::NodePtr> slotNodes; // not "slots": Qt defines that as a keyword macro
    for (const OsteotomyPath& path : paths) {
        QString pathError;
        const auto prepared = OsteotomyCore::PreparePathField(path, &pathError);
        if (!prepared) {
            result.error = pathError.isEmpty() ? QStringLiteral("Trayectoria de corte no válida.") : pathError;
            return result;
        }
        // The osteotomy field baked once over the base: the same field that cuts the bone.
        const auto field = ImplicitCore::BakeFunction(
            [prepared](const ImplicitCore::Vec3& p) { return OsteotomyCore::FieldAt(*prepared, {p[0], p[1], p[2]}); },
            bounds, detail, extension + 2.0, cancel);
        if (!field) {
            result.error = QStringLiteral("Cálculo cancelado.");
            return result;
        }
        slotNodes.push_back(ImplicitCore::Layer(ImplicitCore::Field(field), -0.5 * blade, 0.5 * blade));
    }

    const auto solid = ImplicitCore::Subtract(baseField, ImplicitCore::Union(slotNodes));
    ImplicitCore::PolygonizeOptions options;
    options.smoothingIterations = params.smoothingIterations;
    options.repair = true;
    const ImplicitCore::BuildResult built = ImplicitCore::Build(solid, nullptr, detail, options, cancel);
    if (!built.ok) {
        result.error = built.error.isEmpty() ? QStringLiteral("La guía quedó vacía tras las ranuras.") : built.error;
        return result;
    }
    result.mesh = built.mesh;
    result.pieces = shellsOf(result.mesh);
    result.report = QStringLiteral("Ranuras: %1 corte(s) de %2 mm, detalle %3 mm, %4 pieza(s), %5 triángulos.")
                        .arg(paths.size())
                        .arg(blade, 0, 'f', 2)
                        .arg(detail, 0, 'f', 2)
                        .arg(result.pieces)
                        .arg(result.mesh->GetNumberOfPolys());
    result.ok = true;
    return result;
}
}
