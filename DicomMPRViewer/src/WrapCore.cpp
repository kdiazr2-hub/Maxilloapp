#include "WrapCore.h"

#include "ImplicitCore.h"

#include <vtkImageData.h>
#include <vtkPolyData.h>

#include <algorithm>

namespace WrapCore
{
WrapResult Wrap(const std::vector<vtkPolyData*>& meshes, const WrapParams& params, const std::atomic<bool>* cancel)
{
    WrapResult result;
    const double detail = std::clamp(params.smallestDetailMm, 0.05, 5.0);
    const double gap = std::clamp(params.gapClosingMm, 0.0, 20.0);
    const auto cancelled = [cancel] { return cancel && cancel->load(std::memory_order_relaxed); };

    QString error;
    // The grid has to hold the dilation as well, hence the padding.
    ImplicitCore::VoxelMask mask = ImplicitCore::RasterizeShells(meshes, detail, gap + 2.0, cancel, &error);
    if (mask.Empty()) {
        result.error = error.isEmpty() ? QStringLiteral("No hay mallas que envolver.") : error;
        return result;
    }
    result.spacingMm = mask.spacingMm;

    // The solid itself, filled before any closing: when the meshes enclose a volume (bone from the CT, cut
    // segments, composites), its own signed distance restores sub-voxel accuracy below.
    ImplicitCore::VoxelMask solid = mask;
    const auto shellVoxels = std::count(mask.solid.begin(), mask.solid.end(), std::uint8_t(1));
    ImplicitCore::FillInteriorFromOutside(solid);
    const auto solidVoxels = std::count(solid.solid.begin(), solid.solid.end(), std::uint8_t(1));
    const bool enclosed = solidVoxels > 2 * shellVoxels;

    // Morphological closing in millimetres: grow by the gap, fill what is enclosed once the gaps are
    // bridged, then take the iso surface back in by the same distance instead of eroding the voxels.
    ImplicitCore::DilateMask(mask, gap);
    ImplicitCore::FillInteriorFromOutside(mask);
    if (cancelled()) {
        result.error = QStringLiteral("Cálculo cancelado.");
        return result;
    }
    const auto field = ImplicitCore::SignedDistanceField(mask);
    if (!field) {
        result.error = QStringLiteral("No se pudo medir el campo del wrap.");
        return result;
    }
    if (cancelled()) {
        result.error = QStringLiteral("Cálculo cancelado.");
        return result;
    }

    // The closing contains the solid, and wherever it bridges nothing it IS the solid's surface. But the
    // dilation and the distance to its boundary are both quantised to voxel centres, which leaves the wrap up
    // to one voxel INSIDE the surface it wraps (0.3 mm at 0.3 mm detail) — and every guide and plate built on
    // it sinks into the bone by as much. The solid's own field (shell raster + exact EDT, read back
    // trilinearly) has no such error, so the wrap is the union of the two: the solid's surface wherever it is
    // bone, the closing only where it bridges a gap.
    std::shared_ptr<const ImplicitCore::BakedField> contoured = field;
    double iso = -gap; // undoes the dilation
    if (enclosed) {
        const auto exact = ImplicitCore::SignedDistanceField(solid);
        if (exact && exact->values.size() == field->values.size() && exact->dims == field->dims) {
            auto merged = std::make_shared<ImplicitCore::BakedField>(*field);
            for (size_t i = 0; i < merged->values.size(); ++i)
                merged->values[i] = std::min(merged->values[i] + static_cast<float>(gap), exact->values[i]);
            contoured = merged;
            // Even the solid's field rounds to the voxel, a third of one either way: contouring half a voxel
            // out keeps the wrap from ever sinking into the bone (a slightly loose seat, never an interference).
            iso = 0.5 * mask.spacingMm;
        }
    }

    ImplicitCore::PolygonizeOptions options;
    options.isoValue = iso;
    options.smoothingIterations = params.smoothingIterations;
    options.repair = true;
    result.mesh = ImplicitCore::Polygonize(ImplicitCore::ToImage(*contoured), options);
    if (!result.mesh || result.mesh->GetNumberOfPolys() == 0) {
        result.error = QStringLiteral("El wrap quedó vacío: baje el cierre de huecos o el detalle.");
        return result;
    }
    result.report = QStringLiteral("Wrap: detalle %1 mm, cierre de huecos %2 mm, %3 triángulos.")
                        .arg(result.spacingMm, 0, 'f', 2)
                        .arg(gap, 0, 'f', 2)
                        .arg(result.mesh->GetNumberOfPolys());
    result.ok = true;
    return result;
}
}
