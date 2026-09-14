#include "RegistrationResult.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <QJsonArray>

#include <vtkCellLocator.h>
#include <vtkGenericCell.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>

// ─────────────────────────────────────────────────────────────────────────────
// summaryLine
// ─────────────────────────────────────────────────────────────────────────────

QString RegistrationResult::summaryLine() const
{
    return QStringLiteral("LM-RMS: %1 mm  |  Media: %2 mm  |  P95: %3 mm")
        .arg(landmarkRms,  0, 'f', 2)
        .arg(meanDistance, 0, 'f', 2)
        .arg(p95Distance,  0, 'f', 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// JSON
// ─────────────────────────────────────────────────────────────────────────────

QJsonObject RegistrationResult::toJson() const
{
    QJsonObject o;
    o[QStringLiteral("landmarkRms")]  = landmarkRms;
    o[QStringLiteral("meanDistance")] = meanDistance;
    o[QStringLiteral("maxDistance")]  = maxDistance;
    o[QStringLiteral("p95Distance")]  = p95Distance;
    o[QStringLiteral("accepted")]     = accepted;
    o[QStringLiteral("report")]       = report;

    QJsonArray arr;
    for (double v : matrix) arr.append(v);
    o[QStringLiteral("matrix")] = arr;

    return o;
}

RegistrationResult RegistrationResult::fromJson(const QJsonObject& obj)
{
    RegistrationResult r;
    r.landmarkRms  = obj[QStringLiteral("landmarkRms")].toDouble(0.0);
    r.meanDistance = obj[QStringLiteral("meanDistance")].toDouble(0.0);
    r.maxDistance  = obj[QStringLiteral("maxDistance")].toDouble(0.0);
    r.p95Distance  = obj[QStringLiteral("p95Distance")].toDouble(0.0);
    r.accepted     = obj[QStringLiteral("accepted")].toBool(false);
    r.report       = obj[QStringLiteral("report")].toString();

    const QJsonArray arr = obj[QStringLiteral("matrix")].toArray();
    r.matrix.reserve(arr.size());
    for (const auto& v : arr)
        r.matrix.append(v.toDouble());

    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// computeMetrics
// ─────────────────────────────────────────────────────────────────────────────

bool RegistrationResult::computeMetrics(vtkPolyData* source,
                                         vtkPolyData* target,
                                         double& outMean,
                                         double& outMax,
                                         double& outP95)
{
    outMean = outMax = outP95 = 0.0;

    if (!source || source->GetNumberOfPoints() == 0 ||
        !target || target->GetNumberOfCells()  == 0)
        return false;

    auto locator = vtkSmartPointer<vtkCellLocator>::New();
    locator->SetDataSet(target);
    locator->BuildLocator();

    const vtkIdType nPts = source->GetNumberOfPoints();
    // Sample at most 10 000 points to keep it fast
    const vtkIdType step = std::max<vtkIdType>(1, nPts / 10000);

    std::vector<double> dists;
    dists.reserve(static_cast<size_t>(nPts / step + 1));

    auto      cell   = vtkSmartPointer<vtkGenericCell>::New();
    double    closest[3] = {};
    vtkIdType cellId = -1;
    int       subId  = 0;
    double    dist2  = 0.0;

    double pt[3] = {};
    for (vtkIdType i = 0; i < nPts; i += step) {
        source->GetPoint(i, pt);
        locator->FindClosestPoint(pt, closest, cell.Get(), cellId, subId, dist2);
        dists.push_back(std::sqrt(dist2 < 0.0 ? 0.0 : dist2));
    }

    if (dists.empty()) return false;

    // Mean
    double sum = 0.0;
    for (double d : dists) sum += d;
    outMean = sum / static_cast<double>(dists.size());

    // Max
    outMax = *std::max_element(dists.begin(), dists.end());

    // P95
    std::vector<double> sorted = dists;
    std::sort(sorted.begin(), sorted.end());
    const size_t idx95 = static_cast<size_t>(
        std::ceil(0.95 * static_cast<double>(sorted.size())) - 1);
    outP95 = sorted[std::min(idx95, sorted.size() - 1)];

    return true;
}
