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

    ImplicitCore::PolygonizeOptions options;
    options.isoValue = -gap; // undoes the dilation
    options.smoothingIterations = params.smoothingIterations;
    options.repair = true;
    result.mesh = ImplicitCore::Polygonize(ImplicitCore::ToImage(*field), options);
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
