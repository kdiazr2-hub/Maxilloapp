#include "RegistrationCore.h"

#include <algorithm>
#include <cmath>

#include <vtkCellArray.h>
#include <vtkCleanPolyData.h>
#include <vtkIdList.h>
#include <vtkImplicitPolyDataDistance.h>
#include <vtkIterativeClosestPointTransform.h>
#include <vtkLandmarkTransform.h>
#include <vtkPoints.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>
#include <vtkTriangleFilter.h>

namespace RegistrationCore {

namespace {

constexpr vtkIdType kMinRoiPoints = 50;
constexpr vtkIdType kMaxFitSamples = 25000;

vtkSmartPointer<vtkMatrix4x4> identity()
{
    auto matrix = vtkSmartPointer<vtkMatrix4x4>::New();
    matrix->Identity();
    return matrix;
}

vtkSmartPointer<vtkPolyData> copyOf(vtkPolyData* input)
{
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(input);
    return out;
}

// The cells of input that keep(cellPoints, centroid) accepts, on input's own points, cleaned.
template <typename Keep>
vtkSmartPointer<vtkPolyData> selectCells(vtkPolyData* input, Keep keep)
{
    auto polys = vtkSmartPointer<vtkCellArray>::New();
    auto ids = vtkSmartPointer<vtkIdList>::New();
    double c[3] = {};
    for (vtkIdType cellId = 0; cellId < input->GetNumberOfCells(); ++cellId) {
        input->GetCellPoints(cellId, ids);
        const vtkIdType n = ids->GetNumberOfIds();
        if (n < 3)
            continue;
        double p[3] = {};
        c[0] = c[1] = c[2] = 0.0;
        for (vtkIdType i = 0; i < n; ++i) {
            input->GetPoint(ids->GetId(i), p);
            c[0] += p[0];
            c[1] += p[1];
            c[2] += p[2];
        }
        c[0] /= static_cast<double>(n);
        c[1] /= static_cast<double>(n);
        c[2] /= static_cast<double>(n);
        if (keep(ids.GetPointer(), c))
            polys->InsertNextCell(ids);
    }

    auto selected = vtkSmartPointer<vtkPolyData>::New();
    selected->SetPoints(input->GetPoints());
    selected->SetPolys(polys);
    auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
    clean->SetInputData(selected);
    clean->Update();
    return copyOf(clean->GetOutput());
}

bool enough(vtkPolyData* mesh)
{
    return mesh && mesh->GetNumberOfPoints() >= kMinRoiPoints;
}

// ICP of roi onto target, then the whole moving mesh carried by it and the fit measured on the ROI.
IcpResult runIcp(vtkPolyData* whole, vtkPolyData* roi, vtkPolyData* target, int iterations, double meanDistance,
                 int maxLandmarks)
{
    auto icp = vtkSmartPointer<vtkIterativeClosestPointTransform>::New();
    icp->SetSource(roi);
    icp->SetTarget(target);
    icp->GetLandmarkTransform()->SetModeToRigidBody();
    icp->StartByMatchingCentroidsOff();
    icp->CheckMeanDistanceOn();
    if (maxLandmarks > 0)
        icp->SetMaximumNumberOfLandmarks(maxLandmarks);
    icp->SetMaximumNumberOfIterations(iterations);
    icp->SetMaximumMeanDistance(meanDistance);
    icp->Update();

    IcpResult result;
    result.status = IcpStatus::Ok;
    result.matrix = vtkSmartPointer<vtkMatrix4x4>::New();
    result.matrix->DeepCopy(icp->GetMatrix());
    result.mesh = Apply(whole, result.matrix);
    result.fit = MeasureFit(Apply(roi, result.matrix), target);
    return result;
}

} // namespace

LandmarkFit FitLandmarks(const std::vector<Point>& moving, const std::vector<Point>& fixed)
{
    LandmarkFit fit;
    fit.matrix = identity();
    const size_t pairs = std::min(moving.size(), fixed.size());
    if (pairs < 3)
        return fit;

    auto source = vtkSmartPointer<vtkPoints>::New();
    auto target = vtkSmartPointer<vtkPoints>::New();
    for (size_t i = 0; i < pairs; ++i) {
        source->InsertNextPoint(moving[i].data());
        target->InsertNextPoint(fixed[i].data());
    }
    auto landmark = vtkSmartPointer<vtkLandmarkTransform>::New();
    landmark->SetSourceLandmarks(source);
    landmark->SetTargetLandmarks(target);
    landmark->SetModeToRigidBody();
    landmark->Update();

    double sumSq = 0.0;
    for (size_t i = 0; i < pairs; ++i) {
        double out[3] = {};
        landmark->TransformPoint(moving[i].data(), out);
        const double dx = out[0] - fixed[i][0];
        const double dy = out[1] - fixed[i][1];
        const double dz = out[2] - fixed[i][2];
        const double e = std::sqrt(dx * dx + dy * dy + dz * dz);
        sumSq += e * e;
        fit.maxMm = std::max(fit.maxMm, e);
    }
    fit.valid = true;
    fit.pairs = static_cast<int>(pairs);
    fit.rmsMm = std::sqrt(sumSq / static_cast<double>(pairs));
    fit.matrix->DeepCopy(landmark->GetMatrix());
    return fit;
}

vtkSmartPointer<vtkPolyData> Apply(vtkPolyData* mesh, vtkMatrix4x4* matrix)
{
    if (!mesh || mesh->GetNumberOfPoints() == 0)
        return nullptr;
    auto transform = vtkSmartPointer<vtkTransform>::New();
    if (matrix)
        transform->SetMatrix(matrix);
    auto filter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
    filter->SetInputData(mesh);
    filter->SetTransform(transform);
    filter->Update();
    return copyOf(filter->GetOutput());
}

vtkSmartPointer<vtkPolyData> CleanTriangles(vtkPolyData* input)
{
    auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
    clean->SetInputData(input);
    clean->Update();
    auto tri = vtkSmartPointer<vtkTriangleFilter>::New();
    tri->SetInputConnection(clean->GetOutputPort());
    tri->Update();
    return copyOf(tri->GetOutput());
}

vtkSmartPointer<vtkPolyData> CropCellsByBox(vtkPolyData* input, const std::array<double, 6>& box)
{
    if (!input || input->GetNumberOfCells() == 0)
        return nullptr;
    return selectCells(input, [&](vtkIdList*, const double c[3]) {
        return c[0] >= box[0] && c[0] <= box[1] && c[1] >= box[2] && c[1] <= box[3] && c[2] >= box[4]
               && c[2] <= box[5];
    });
}

vtkSmartPointer<vtkPolyData> KeepCellsNearSurface(vtkPolyData* input, vtkPolyData* target, double maxDistance)
{
    if (!input || !target || input->GetNumberOfCells() == 0 || target->GetNumberOfPoints() == 0)
        return nullptr;
    auto distance = vtkSmartPointer<vtkImplicitPolyDataDistance>::New();
    distance->SetInput(target);
    return selectCells(input, [&](vtkIdList* ids, const double c[3]) {
        double centroid[3] = {c[0], c[1], c[2]};
        if (std::abs(distance->EvaluateFunction(centroid)) <= maxDistance)
            return true;
        double p[3] = {};
        for (vtkIdType i = 0; i < ids->GetNumberOfIds(); ++i) {
            input->GetPoint(ids->GetId(i), p);
            if (std::abs(distance->EvaluateFunction(p)) <= maxDistance)
                return true;
        }
        return false;
    });
}

vtkSmartPointer<vtkPolyData> KeepCellsNearPoints(vtkPolyData* input, const std::vector<Point>& points, double radius)
{
    if (!input || points.empty() || input->GetNumberOfCells() == 0)
        return nullptr;
    const double radiusSquared = radius * radius;
    const auto closeToPoint = [&](const double p[3]) {
        for (const Point& hint : points) {
            const double dx = p[0] - hint[0];
            const double dy = p[1] - hint[1];
            const double dz = p[2] - hint[2];
            if (dx * dx + dy * dy + dz * dz <= radiusSquared)
                return true;
        }
        return false;
    };
    return selectCells(input, [&](vtkIdList* ids, const double c[3]) {
        double p[3] = {};
        for (vtkIdType i = 0; i < ids->GetNumberOfIds(); ++i) {
            input->GetPoint(ids->GetId(i), p);
            if (closeToPoint(p))
                return true;
        }
        return closeToPoint(c);
    });
}

std::array<double, 6> ArchRoi(const double archBounds[6], bool upperArch)
{
    const double dx = std::max(1.0, archBounds[1] - archBounds[0]);
    const double dy = std::max(1.0, archBounds[3] - archBounds[2]);
    const double dz = std::max(1.0, archBounds[5] - archBounds[4]);
    const double maxRange = std::max({dx, dy, dz});

    const double xyPad = std::clamp(maxRange * 0.18, 12.0, 28.0);
    const double towardsBite = std::clamp(maxRange * 0.08, 5.0, 10.0);
    const double towardsBone = std::clamp(maxRange * 0.22, 14.0, 30.0);
    const double zBelow = upperArch ? towardsBite : towardsBone;
    const double zAbove = upperArch ? towardsBone : towardsBite;
    return {archBounds[0] - xyPad, archBounds[1] + xyPad, archBounds[2] - xyPad,
            archBounds[3] + xyPad, archBounds[4] - zBelow, archBounds[5] + zAbove};
}

FitMetrics MeasureFit(vtkPolyData* registered, vtkPolyData* target)
{
    FitMetrics fit;
    if (!registered || !target || target->GetNumberOfPoints() == 0)
        return fit;
    auto distance = vtkSmartPointer<vtkImplicitPolyDataDistance>::New();
    distance->SetInput(target);

    const vtkIdType total = registered->GetNumberOfPoints();
    const vtkIdType step = std::max<vtkIdType>(1, total / kMaxFitSamples);
    double p[3] = {};
    double sum = 0.0;
    double sumSq = 0.0;
    long long within1 = 0;
    long long within2 = 0;
    for (vtkIdType i = 0; i < total; i += step) {
        registered->GetPoint(i, p);
        const double d = std::abs(distance->EvaluateFunction(p));
        sum += d;
        sumSq += d * d;
        fit.maxMm = std::max(fit.maxMm, d);
        if (d <= 1.0)
            ++within1;
        if (d <= 2.0)
            ++within2;
        ++fit.samples;
    }
    if (fit.samples > 0) {
        const double n = static_cast<double>(fit.samples);
        fit.meanMm = sum / n;
        fit.rmseMm = std::sqrt(sumSq / n);
        fit.within1MmPercent = 100.0 * static_cast<double>(within1) / n;
        fit.within2MmPercent = 100.0 * static_cast<double>(within2) / n;
    }
    return fit;
}

IcpResult RefineArch(vtkPolyData* arch, vtkPolyData* bone, bool upperArch)
{
    IcpResult result;
    result.matrix = identity();
    if (!arch || !bone || arch->GetNumberOfPoints() == 0 || bone->GetNumberOfPoints() == 0)
        return result;

    auto archTri = CleanTriangles(arch);
    auto boneTri = CleanTriangles(bone);
    if (!archTri || !boneTri) {
        result.status = IcpStatus::CleanFailed;
        return result;
    }

    double archBounds[6] = {};
    archTri->GetBounds(archBounds);
    const auto roi = ArchRoi(archBounds, upperArch);
    auto archRoi = CropCellsByBox(archTri, roi);
    auto boneRoi = CropCellsByBox(boneTri, roi);
    if (!enough(archRoi) || !enough(boneRoi)) {
        result.status = IcpStatus::RoiTooSmall;
        result.mesh = archTri;
        return result;
    }
    return runIcp(archTri, archRoi, boneRoi, 80, 0.0005, 0);
}

IcpResult RefineBite(vtkPolyData* moving, vtkPolyData* biteScan, const std::vector<Point>& movingLandmarks,
                     const std::vector<Point>& targetLandmarks)
{
    IcpResult result;
    result.matrix = identity();
    if (!moving || !biteScan || moving->GetNumberOfPoints() == 0 || biteScan->GetNumberOfPoints() == 0)
        return result;

    auto movingTri = CleanTriangles(moving);
    auto biteTri = CleanTriangles(biteScan);
    if (!movingTri || !biteTri || movingTri->GetNumberOfPoints() == 0 || biteTri->GetNumberOfPoints() == 0) {
        result.status = IcpStatus::CleanFailed;
        return result;
    }

    double biteBounds[6] = {};
    biteTri->GetBounds(biteBounds);
    const double sx = std::max(1.0, biteBounds[1] - biteBounds[0]);
    const double sy = std::max(1.0, biteBounds[3] - biteBounds[2]);
    const double sz = std::max(1.0, biteBounds[5] - biteBounds[4]);
    const double biteDiag = std::sqrt(sx * sx + sy * sy + sz * sz);
    const double pad = std::clamp(biteDiag * 0.08, 4.0, 12.0);
    const std::array<double, 6> roiBox = {biteBounds[0] - pad, biteBounds[1] + pad, biteBounds[2] - pad,
                                          biteBounds[3] + pad, biteBounds[4] - pad, biteBounds[5] + pad};

    constexpr double kLandmarkRadiusMm = 12.0;
    auto movingRoi = CropCellsByBox(movingTri, roiBox);
    auto movingLocal = KeepCellsNearSurface(movingRoi, biteTri, std::clamp(biteDiag * 0.05, 3.0, 7.0));
    auto hintedMoving = KeepCellsNearPoints(movingTri, movingLandmarks, kLandmarkRadiusMm);
    auto targetLocal = KeepCellsNearPoints(biteTri, targetLandmarks, kLandmarkRadiusMm);
    if (enough(hintedMoving))
        movingLocal = hintedMoving;
    if (!enough(targetLocal))
        targetLocal = biteTri;
    if (!enough(movingLocal))
        movingLocal = movingRoi;
    if (!enough(movingLocal) || !enough(targetLocal)) {
        result.status = IcpStatus::RoiTooSmall;
        result.mesh = copyOf(movingTri);
        return result;
    }
    return runIcp(movingTri, movingLocal, targetLocal, 120, 0.001, 5000);
}

} // namespace RegistrationCore
