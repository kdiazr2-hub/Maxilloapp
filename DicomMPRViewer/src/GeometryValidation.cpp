#include "GeometryValidation.h"

#include <algorithm>
#include <cmath>

namespace GeometryValidation
{
QString MeshBoundsText(vtkPolyData* mesh)
{
    if (!mesh || mesh->GetNumberOfPoints() == 0) return QStringLiteral("<empty>");
    double b[6] = {};
    mesh->GetBounds(b);
    return QStringLiteral("[%1, %2] x [%3, %4] x [%5, %6]")
        .arg(b[0], 0, 'f', 3).arg(b[1], 0, 'f', 3)
        .arg(b[2], 0, 'f', 3).arg(b[3], 0, 'f', 3)
        .arg(b[4], 0, 'f', 3).arg(b[5], 0, 'f', 3);
}

QString MeshCentroidText(vtkPolyData* mesh)
{
    if (!mesh || mesh->GetNumberOfPoints() == 0) return QStringLiteral("<empty>");
    double p[3] = {};
    double c[3] = {};
    const vtkIdType step = std::max<vtkIdType>(1, mesh->GetNumberOfPoints() / 20000);
    vtkIdType count = 0;
    for (vtkIdType i = 0; i < mesh->GetNumberOfPoints(); i += step) {
        mesh->GetPoint(i, p);
        c[0] += p[0];
        c[1] += p[1];
        c[2] += p[2];
        ++count;
    }
    if (count > 0) {
        c[0] /= static_cast<double>(count);
        c[1] /= static_cast<double>(count);
        c[2] /= static_cast<double>(count);
    }
    return QStringLiteral("(%1, %2, %3)")
        .arg(c[0], 0, 'f', 3)
        .arg(c[1], 0, 'f', 3)
        .arg(c[2], 0, 'f', 3);
}

double MeshDiagonalLength(vtkPolyData* mesh)
{
    if (!mesh || mesh->GetNumberOfPoints() == 0) return 0.0;
    double b[6] = {};
    mesh->GetBounds(b);
    const double dx = b[1] - b[0];
    const double dy = b[3] - b[2];
    const double dz = b[5] - b[4];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool ValidateDicomGeometry(vtkImageData* image, QString* report)
{
    if (!image || image->GetNumberOfPoints() == 0) {
        if (report) *report = QStringLiteral("Volumen DICOM vacio.");
        return false;
    }
    int dims[3] = {};
    double spacing[3] = {};
    double origin[3] = {};
    image->GetDimensions(dims);
    image->GetSpacing(spacing);
    image->GetOrigin(origin);

    const bool dimsOk = dims[0] > 0 && dims[1] > 0 && dims[2] > 0;
    const bool spacingOk = spacing[0] > 0.0 && spacing[0] < 5.0 &&
                           spacing[1] > 0.0 && spacing[1] < 5.0 &&
                           spacing[2] > 0.0 && spacing[2] < 10.0;
    if (report) {
        *report = QStringLiteral("dims=%1x%2x%3 spacing=%4,%5,%6 origin=%7,%8,%9")
            .arg(dims[0]).arg(dims[1]).arg(dims[2])
            .arg(spacing[0], 0, 'f', 4).arg(spacing[1], 0, 'f', 4).arg(spacing[2], 0, 'f', 4)
            .arg(origin[0], 0, 'f', 3).arg(origin[1], 0, 'f', 3).arg(origin[2], 0, 'f', 3);
    }
    return dimsOk && spacingOk;
}

bool ValidateMeshScale(vtkPolyData* mesh, const QString& anatomy, QString* report)
{
    if (!mesh || mesh->GetNumberOfPoints() == 0) {
        if (report) *report = anatomy + QStringLiteral(": malla vacia.");
        return false;
    }
    const double diag = MeshDiagonalLength(mesh);
    double minDiag = 10.0;
    double maxDiag = 350.0;
    const QString lower = anatomy.toLower();
    if (lower.contains(QStringLiteral("mandib"))) {
        minDiag = 45.0;
        maxDiag = 180.0;
    } else if (lower.contains(QStringLiteral("arco")) || lower.contains(QStringLiteral("stl"))) {
        minDiag = 20.0;
        maxDiag = 140.0;
    } else if (lower.contains(QStringLiteral("maxil"))) {
        minDiag = 40.0;
        maxDiag = 260.0;
    }

    const bool ok = diag >= minDiag && diag <= maxDiag;
    if (report) {
        *report = QStringLiteral("%1 diag=%2 mm bounds=%3 (%4)")
            .arg(anatomy)
            .arg(diag, 0, 'f', 2)
            .arg(MeshBoundsText(mesh))
            .arg(ok ? QStringLiteral("ok") : QStringLiteral("escala atipica"));
    }
    return ok;
}

bool ValidateCompositeInputs(vtkPolyData* boneWorld, vtkPolyData* archWorld,
                             const QString& anatomy, QString* report)
{
    QString boneReport;
    QString archReport;
    const bool boneOk = ValidateMeshScale(boneWorld, anatomy, &boneReport);
    const bool archOk = ValidateMeshScale(archWorld, QStringLiteral("STL ") + anatomy, &archReport);

    if (report) {
        *report = boneReport + QLatin1Char('\n') + archReport;
    }
    return boneOk && archOk;
}

// ─────────────────────────────────────────────────────────────────────────────
// Structured ValidationResult wrappers  (Phase 3)
// ─────────────────────────────────────────────────────────────────────────────

ValidationResult validateDicom(vtkImageData* vol)
{
    if (!vol || vol->GetNumberOfPoints() == 0) {
        return { ValidationSeverity::Error,
                 QStringLiteral("Volumen DICOM vacío o nulo.") };
    }

    int dims[3] = {};
    double spacing[3] = {};
    vol->GetDimensions(dims);
    vol->GetSpacing(spacing);

    const bool dimsOk    = dims[0] > 0 && dims[1] > 0 && dims[2] > 0;
    const bool spacingOk = spacing[0] > 0.0 && spacing[0] < 5.0 &&
                           spacing[1] > 0.0 && spacing[1] < 5.0 &&
                           spacing[2] > 0.0 && spacing[2] < 10.0;

    if (!dimsOk) {
        return { ValidationSeverity::Error,
                 QStringLiteral("Dimensiones DICOM inválidas: %1x%2x%3")
                     .arg(dims[0]).arg(dims[1]).arg(dims[2]) };
    }

    if (!spacingOk) {
        // Unusual spacing is a warning (data is still usable)
        return { ValidationSeverity::Warning,
                 QStringLiteral("Espaciado DICOM inusual: %1 x %2 x %3 mm "
                                "(esperado <5mm XY, <10mm Z)")
                     .arg(spacing[0], 0, 'f', 3)
                     .arg(spacing[1], 0, 'f', 3)
                     .arg(spacing[2], 0, 'f', 3) };
    }

    return { ValidationSeverity::Ok, {} };
}

ValidationResult validateMeshScale(vtkPolyData* mesh, const QString& anatomyType)
{
    if (!mesh || mesh->GetNumberOfPoints() == 0) {
        return { ValidationSeverity::Error,
                 QStringLiteral("%1: malla vacía o nula.").arg(anatomyType) };
    }

    QString report;
    const bool ok = ValidateMeshScale(mesh, anatomyType, &report);

    if (!ok) {
        // Determine severity: if diagonal is extremely off it is an error,
        // otherwise a warning that the user should double-check.
        const double diag = MeshDiagonalLength(mesh);
        const ValidationSeverity sev =
            (diag < 5.0 || diag > 400.0)
                ? ValidationSeverity::Error
                : ValidationSeverity::Warning;
        return { sev, report };
    }

    return { ValidationSeverity::Ok, report };
}

} // namespace GeometryValidation
