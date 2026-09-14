#include "CompositeModelCore.h"

#include "GeometryValidation.h"
#include "TransformCore.h"

#include <vtkAppendPolyData.h>
#include <vtkCellArray.h>
#include <vtkCleanPolyData.h>
#include <vtkPolyDataConnectivityFilter.h>
#include <vtkIdList.h>
#include <vtkImplicitPolyDataDistance.h>
#include <vtkPolyDataNormals.h>
#include <vtkTriangleFilter.h>

#include <algorithm>
#include <cmath>

namespace
{
vtkSmartPointer<vtkPolyData> cleanForComposite(vtkPolyData* input)
{
    if (!input || input->GetNumberOfPoints() == 0) return nullptr;

    auto tri = vtkSmartPointer<vtkTriangleFilter>::New();
    tri->SetInputData(input);
    tri->Update();

    auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
    clean->SetInputConnection(tri->GetOutputPort());
    clean->Update();

    auto normals = vtkSmartPointer<vtkPolyDataNormals>::New();
    normals->SetInputConnection(clean->GetOutputPort());
    normals->ConsistencyOn();
    normals->SplittingOff();
    normals->AutoOrientNormalsOn();
    normals->Update();

    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(normals->GetOutput());
    return out->GetNumberOfPoints() > 0 ? out : nullptr;
}

// Keep the largest connected region PLUS any region with >= 5% of the largest
// region's cell count.  This preserves small-but-valid bone islands that are
// disconnected from the main jaw body by the dental surface trim, preventing
// the visible gap/void at the arch-bone junction.
vtkSmartPointer<vtkPolyData> filterConnectedComponents(vtkPolyData* input)
{
    if (!input || input->GetNumberOfPoints() == 0 || input->GetNumberOfCells() == 0)
        return nullptr;

    auto conn = vtkSmartPointer<vtkPolyDataConnectivityFilter>::New();
    conn->SetInputData(input);
    conn->SetExtractionModeToAllRegions();
    conn->Update();

    const int nRegions = conn->GetNumberOfExtractedRegions();
    if (nRegions <= 1) {
        auto out = vtkSmartPointer<vtkPolyData>::New();
        out->DeepCopy(conn->GetOutput());
        return out->GetNumberOfPoints() > 0 ? out : nullptr;
    }

    // Save sizes before changing extraction mode (pointer may be invalidated)
    auto* sizeArray = conn->GetRegionSizes();
    std::vector<vtkIdType> sizes(nRegions);
    vtkIdType maxSize = 0;
    for (int i = 0; i < nRegions; ++i) {
        sizes[i] = sizeArray->GetValue(i);
        maxSize = std::max(maxSize, sizes[i]);
    }

    conn->SetExtractionModeToSpecifiedRegions();
    conn->InitializeSpecifiedRegionList();
    for (int i = 0; i < nRegions; ++i) {
        if (sizes[i] >= maxSize * 0.05)
            conn->AddSpecifiedRegion(i);
    }
    conn->Update();

    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(conn->GetOutput());
    return out->GetNumberOfPoints() > 0 ? out : nullptr;
}

vtkSmartPointer<vtkPolyData> removeBoneCellsCoveredByDentalSurface(
    vtkPolyData* bone,
    vtkPolyData* dental,
    const QString& anatomyName,
    QString* report)
{
    if (!bone || !dental || bone->GetNumberOfCells() == 0 || dental->GetNumberOfPoints() == 0)
        return nullptr;

    double dentalBounds[6] = {};
    dental->GetBounds(dentalBounds);
    const double dx = dentalBounds[1] - dentalBounds[0];
    const double dy = dentalBounds[3] - dentalBounds[2];
    const double dz = dentalBounds[5] - dentalBounds[4];
    const double diag = std::sqrt(dx * dx + dy * dy + dz * dz);
    const bool mandible = anatomyName.toLower().contains(QStringLiteral("mand"));

    double boneCentroid[3] = {};
    double dentalCentroid[3] = {};
    double tmp[3] = {};
    const vtkIdType boneStep = std::max<vtkIdType>(1, bone->GetNumberOfPoints() / 20000);
    const vtkIdType dentalStep = std::max<vtkIdType>(1, dental->GetNumberOfPoints() / 20000);
    vtkIdType boneCount = 0;
    vtkIdType dentalCount = 0;
    for (vtkIdType i = 0; i < bone->GetNumberOfPoints(); i += boneStep) {
        bone->GetPoint(i, tmp);
        boneCentroid[0] += tmp[0];
        boneCentroid[1] += tmp[1];
        boneCentroid[2] += tmp[2];
        ++boneCount;
    }
    for (vtkIdType i = 0; i < dental->GetNumberOfPoints(); i += dentalStep) {
        dental->GetPoint(i, tmp);
        dentalCentroid[0] += tmp[0];
        dentalCentroid[1] += tmp[1];
        dentalCentroid[2] += tmp[2];
        ++dentalCount;
    }
    if (boneCount > 0) {
        boneCentroid[0] /= static_cast<double>(boneCount);
        boneCentroid[1] /= static_cast<double>(boneCount);
        boneCentroid[2] /= static_cast<double>(boneCount);
    }
    if (dentalCount > 0) {
        dentalCentroid[0] /= static_cast<double>(dentalCount);
        dentalCentroid[1] /= static_cast<double>(dentalCount);
        dentalCentroid[2] /= static_cast<double>(dentalCount);
    }

    const double dentalSpan[3] = {dx, dy, dz};
    int heightAxis = 0;
    if (dentalSpan[1] < dentalSpan[heightAxis]) heightAxis = 1;
    if (dentalSpan[2] < dentalSpan[heightAxis]) heightAxis = 2;
    const double boneSide =
        (boneCentroid[heightAxis] >= dentalCentroid[heightAxis]) ? 1.0 : -1.0;
    const double crownSide = -boneSide;

    // The composite should replace CT dental surface with the registered intraoral STL.
    // This trim must remove both the shell that touches the STL and any CT tooth/bone
    // fragments contained inside the STL footprint. A fragile global boolean is avoided
    // because dental scans are often open/noisy.
    const double trimDistance = mandible
        ? std::clamp(diag * 0.010, 0.45, 1.0)  // ultra-conservative: preserve mandibular TAC bone
        : std::clamp(diag * 0.022, 1.4, 2.8);
    const double deepDistance = mandible
        ? std::clamp(diag * 0.016, 1.0, 2.0)
        : std::clamp(diag * 0.045, 3.0, 5.2);
    const double boxPad = std::max(3.0, deepDistance * 1.1);
    const double roi[6] = {
        dentalBounds[0] - boxPad, dentalBounds[1] + boxPad,
        dentalBounds[2] - boxPad, dentalBounds[3] + boxPad,
        dentalBounds[4] - boxPad, dentalBounds[5] + boxPad
    };

    auto inRoi = [&](const double p[3]) {
        return p[0] >= roi[0] && p[0] <= roi[1] &&
               p[1] >= roi[2] && p[1] <= roi[3] &&
               p[2] >= roi[4] && p[2] <= roi[5];
    };

    const double footprintPad = std::max(2.5, trimDistance * 0.65);
    auto inDentalFootprint = [&](const double p[3]) {
        for (int axis = 0; axis < 3; ++axis) {
            const double low = dentalBounds[axis * 2] -
                (axis == heightAxis ? trimDistance : footprintPad);
            const double high = dentalBounds[axis * 2 + 1] +
                (axis == heightAxis ? trimDistance : footprintPad);
            if (p[axis] < low || p[axis] > high)
                return false;
        }
        return true;
    };

    auto onDentalCrownSide = [&](const double p[3]) {
        const double signedAxisDistance =
            (p[heightAxis] - dentalCentroid[heightAxis]) * crownSide;
        return signedAxisDistance >= (mandible ? 0.0 : -trimDistance * 0.6);
    };

    // gingivalMargin: bone is NOT trimmed within this distance of the gingival boundary
    // toward the crown.  For mandible a smaller value is needed so that the trim
    // starts early enough for the arch surface to fill the junction without a gap.
    const double gingivalMargin = mandible
        ? std::clamp(diag * 0.060, 4.0, 6.5)   // reduced: was 0.105+1.5 mm ≈ 9.5-14.5
        : std::clamp(diag * 0.075, 6.0, 9.5);
    const double effectiveGingivalMargin = mandible
        ? std::clamp(diag * 0.120, 8.5, 12.0)
        : gingivalMargin;
    const double gingivalBoundary = boneSide > 0.0
        ? dentalBounds[heightAxis * 2 + 1]
        : dentalBounds[heightAxis * 2];

    auto beyondGingivalMargin = [&](const double p[3]) {
        const double distanceTowardCrown =
            (p[heightAxis] - gingivalBoundary) * crownSide;
        return distanceTowardCrown >= effectiveGingivalMargin;
    };

    auto safelyAwayFromMandibularBody = [&](const double p[3]) {
        if (!mandible) return true;
        const double crownDistance =
            (p[heightAxis] - dentalCentroid[heightAxis]) * crownSide;
        return crownDistance >= trimDistance * 0.35;
    };

    auto distance = vtkSmartPointer<vtkImplicitPolyDataDistance>::New();
    distance->SetInput(dental);

    auto shouldRemovePoint = [&](const double p[3]) {
        if (!inRoi(p)) return false;
        if (!beyondGingivalMargin(p)) return false;
        double evalPoint[3] = {p[0], p[1], p[2]};
        const double signedDistance = distance->EvaluateFunction(evalPoint);

        if (std::abs(signedDistance) > trimDistance)
        {
            // ── maxilla only: allow a deeper interior trim ────────────────
            if (mandible) return false;
            // Some intraoral STL files are open or have inconsistent normals.
            // The deeper trim is only allowed on the crown side so that basal
            // maxilla / nasal floor are not cut.
            return inDentalFootprint(p) &&
                   onDentalCrownSide(p) &&
                   safelyAwayFromMandibularBody(p) &&
                   std::abs(signedDistance) <= deepDistance;
        }

        // ── within trimDistance of the dental surface ─────────────────────
        // For mandible, also require the point to lie within the lateral
        // footprint of the arch.  This prevents over-removal of the
        // mandibular body / ramus that happens to be close to the arch
        // surface but is clearly outside the dental arch envelope.
        if (mandible)
            return inDentalFootprint(p);

        return true;
    };

    auto keptPolys = vtkSmartPointer<vtkCellArray>::New();
    auto ids = vtkSmartPointer<vtkIdList>::New();
    double p[3] = {};
    double c[3] = {};
    vtkIdType kept = 0;
    vtkIdType removed = 0;

    for (vtkIdType cellId = 0; cellId < bone->GetNumberOfCells(); ++cellId) {
        bone->GetCellPoints(cellId, ids);
        const vtkIdType n = ids->GetNumberOfIds();
        if (n < 3) continue;

        c[0] = c[1] = c[2] = 0.0;
        for (vtkIdType i = 0; i < n; ++i) {
            bone->GetPoint(ids->GetId(i), p);
            c[0] += p[0];
            c[1] += p[1];
            c[2] += p[2];
        }
        c[0] /= static_cast<double>(n);
        c[1] /= static_cast<double>(n);
        c[2] /= static_cast<double>(n);

        int hits = 0;
        for (vtkIdType i = 0; i < n; ++i) {
            bone->GetPoint(ids->GetId(i), p);
            if (shouldRemovePoint(p)) {
                ++hits;
            }
        }

        // Mandible: require centroid + ALL vertices to qualify — prevents removing
        // cells that only partially overlap the dental STL footprint.
        // Maxilla: majority rule is fine (dental surface is more reliably closed).
        const bool coveredByDental = mandible
            ? (shouldRemovePoint(c) && hits >= n)
            : (shouldRemovePoint(c) || hits >= std::max<vtkIdType>(2, n / 2));

        if (coveredByDental) {
            ++removed;
        } else {
            keptPolys->InsertNextCell(ids);
            ++kept;
        }
    }

    auto trimmed = vtkSmartPointer<vtkPolyData>::New();
    trimmed->SetPoints(bone->GetPoints());
    trimmed->SetPolys(keptPolys);

    auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
    clean->SetInputData(trimmed);
    clean->Update();

    auto out = vtkSmartPointer<vtkPolyData>::New();
    if (auto filtered = filterConnectedComponents(clean->GetOutput()))
        out->DeepCopy(filtered);
    else
        out->DeepCopy(clean->GetOutput());

    if (report) {
        *report += QStringLiteral("\nComposite trim %1: surfaceDistance=%2 mm, deepDistance=%3 mm, gingivalMargin=%4 mm, heightAxis=%5, removedCells=%6, keptCells=%7")
            .arg(anatomyName)
            .arg(trimDistance, 0, 'f', 2)
            .arg(deepDistance, 0, 'f', 2)
            .arg(effectiveGingivalMargin, 0, 'f', 2)
            .arg(heightAxis)
            .arg(removed)
            .arg(kept);
    }
    return out->GetNumberOfPoints() > 0 ? out : nullptr;
}
}

namespace CompositeModelCore
{
vtkSmartPointer<vtkPolyData> CreateNonDestructiveComposite(
    vtkPolyData* boneMesh,
    vtkMatrix4x4* boneToWorld,
    vtkPolyData* dentalMesh,
    vtkMatrix4x4* dentalToWorld,
    const QString& anatomyName,
    QString* report,
    QString* error)
{
    auto boneWorld = TransformCore::ApplyTransformToPolyData(boneMesh, boneToWorld);
    auto dentalWorld = TransformCore::ApplyTransformToPolyData(dentalMesh, dentalToWorld);
    if (!boneWorld || !dentalWorld) {
        if (error) *error = QStringLiteral("Faltan mallas validas para el compuesto.");
        return nullptr;
    }

    QString validation;
    const bool scaleOk = GeometryValidation::ValidateCompositeInputs(
        boneWorld, dentalWorld, anatomyName, &validation);
    if (report) *report = validation;
    if (!scaleOk && error) {
        *error = QStringLiteral("Advertencia de escala en modelo compuesto:\n") + validation;
        // Keep going: imported STL can be partial; caller decides whether this is blocking.
    }

    auto boneClean = cleanForComposite(boneWorld);
    auto dentalClean = cleanForComposite(dentalWorld);
    if (!boneClean || !dentalClean) {
        if (error) *error = QStringLiteral("No se pudieron limpiar las mallas para el compuesto.");
        return nullptr;
    }

    QString trimReport;
    auto trimmedBone = removeBoneCellsCoveredByDentalSurface(
        boneClean, dentalClean, anatomyName, &trimReport);
    if (report && !trimReport.isEmpty())
        *report += trimReport;
    if (!trimmedBone)
        trimmedBone = boneClean;

    auto append = vtkSmartPointer<vtkAppendPolyData>::New();
    append->AddInputData(trimmedBone);
    append->AddInputData(dentalClean);
    append->Update();

    auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
    clean->SetInputConnection(append->GetOutputPort());
    clean->Update();

    // Recompute normals across the junction for smooth rendering and no dark seam
    auto finalNormals = vtkSmartPointer<vtkPolyDataNormals>::New();
    finalNormals->SetInputConnection(clean->GetOutputPort());
    finalNormals->ConsistencyOn();
    finalNormals->SplittingOff();
    finalNormals->AutoOrientNormalsOn();
    finalNormals->Update();

    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(finalNormals->GetOutput());
    return out->GetNumberOfPoints() > 0 ? out : nullptr;
}
}
