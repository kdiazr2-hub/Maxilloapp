#include "RootAnalysisCore.h"

#include <vtkPolyData.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
using Vec3 = std::array<double, 3>;
Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 scale(const Vec3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double norm(const Vec3& a) { return std::sqrt(dot(a, a)); }
Vec3 unit(const Vec3& a, const Vec3& fallback)
{
    const double n = norm(a);
    return n > 1e-12 ? scale(a, 1.0 / n) : fallback;
}
QString mm(double value) { return QString::number(value, 'f', 1) + QStringLiteral(" mm"); }
} // namespace

namespace RootAnalysisCore
{
QString ToothName(RootTooth tooth)
{
    switch (tooth) {
    case RootTooth::FirstMolarRight: return QStringLiteral("primer molar derecho");
    case RootTooth::CanineRight: return QStringLiteral("canino derecho");
    case RootTooth::CanineLeft: return QStringLiteral("canino izquierdo");
    case RootTooth::FirstMolarLeft: return QStringLiteral("primer molar izquierdo");
    }
    return {};
}

RootAnalysis Analyze(vtkPolyData* teeth, const OsteotomyPath& cut, const RootAnalysisParams& params)
{
    RootAnalysis analysis;
    if (!teeth || teeth->GetNumberOfPoints() == 0) {
        analysis.error = QStringLiteral("No hay dientes superiores separados del hueso: vuelva a segmentar el TAC "
                                        "para medir las raíces.");
        return analysis;
    }
    if (!cut.valid || cut.points.size() != 4) {
        analysis.error = QStringLiteral("Falta la trayectoria de la osteotomía Le Fort.");
        return analysis;
    }
    const auto field = OsteotomyCore::PreparePathField(cut);
    if (!field) {
        analysis.error = QStringLiteral("La trayectoria de la osteotomía Le Fort no es válida.");
        return analysis;
    }
    // The frame: up from the cut, lateral from pilar D to pilar I, square to it.
    const Vec3 up = unit(cut.upAxis, {0.0, 0.0, 1.0});
    const Vec3 across = sub(cut.points.back(), cut.points.front());
    const Vec3 lateral = unit(sub(across, scale(up, dot(across, up))), {1.0, 0.0, 0.0});
    const auto lateralOf = [&](const Vec3& p) { return dot(p, lateral); };
    const auto heightOf = [&](const Vec3& p) { return dot(p, up); };

    // The highest tooth point in each bin along the cut.
    const vtkIdType count = teeth->GetNumberOfPoints();
    double lowest = std::numeric_limits<double>::max(), highest = std::numeric_limits<double>::lowest();
    for (vtkIdType i = 0; i < count; ++i) {
        double p[3];
        teeth->GetPoint(i, p);
        const double s = lateralOf({p[0], p[1], p[2]});
        lowest = std::min(lowest, s);
        highest = std::max(highest, s);
    }
    const double bin = std::max(0.25, params.binMm);
    const int bins = std::max(1, static_cast<int>(std::ceil((highest - lowest) / bin)) + 1);
    std::vector<vtkIdType> top(static_cast<size_t>(bins), -1);
    std::vector<double> topHeight(static_cast<size_t>(bins), std::numeric_limits<double>::lowest());
    for (vtkIdType i = 0; i < count; ++i) {
        double p[3];
        teeth->GetPoint(i, p);
        const Vec3 q{p[0], p[1], p[2]};
        const int b = std::clamp(static_cast<int>(std::floor((lateralOf(q) - lowest) / bin)), 0, bins - 1);
        if (heightOf(q) > topHeight[static_cast<size_t>(b)]) {
            topHeight[static_cast<size_t>(b)] = heightOf(q);
            top[static_cast<size_t>(b)] = i;
        }
    }
    // Apices: bins highest within half the spacing either side (the first of a flat run).
    const int half = std::max(1, static_cast<int>(std::round(0.5 * params.minApexSpacingMm / bin)));
    for (int b = 0; b < bins; ++b) {
        if (top[static_cast<size_t>(b)] < 0)
            continue;
        bool peak = true;
        for (int o = -half; o <= half && peak; ++o) {
            const int n = b + o;
            if (o == 0 || n < 0 || n >= bins || top[static_cast<size_t>(n)] < 0)
                continue;
            const double other = topHeight[static_cast<size_t>(n)], here = topHeight[static_cast<size_t>(b)];
            peak = here > other + 1e-6 || (std::abs(here - other) <= 1e-6 && o > 0);
        }
        if (!peak)
            continue;
        RootApex apex;
        double p[3];
        teeth->GetPoint(top[static_cast<size_t>(b)], p);
        apex.apex = {p[0], p[1], p[2]};
        // The cusp: the lowest tooth point within the column round the apex.
        double cuspHeight = heightOf(apex.apex);
        apex.cusp = apex.apex;
        for (vtkIdType i = 0; i < count; ++i) {
            double r[3];
            teeth->GetPoint(i, r);
            const Vec3 q{r[0], r[1], r[2]};
            const Vec3 d = sub(q, apex.apex);
            const Vec3 flat = sub(d, scale(up, dot(d, up)));
            if (norm(flat) <= params.columnRadiusMm && heightOf(q) < cuspHeight) {
                cuspHeight = heightOf(q);
                apex.cusp = q;
            }
        }
        apex.lengthMm = heightOf(apex.apex) - cuspHeight;
        // The cut's field is a distance across the cut surface, negative below it.
        apex.cutDistanceMm = -OsteotomyCore::FieldAt(*field, apex.apex);
        apex.onCut = add(apex.apex, scale(up, apex.cutDistanceMm));
        apex.tooClose = apex.cutDistanceMm < params.minCutDistanceMm;
        analysis.apices.push_back(apex);
    }
    // A flat or forked root tip gives more than one peak: of any two within the spacing, the higher stays.
    {
        std::vector<RootApex> byHeight = analysis.apices;
        std::stable_sort(byHeight.begin(), byHeight.end(), [&](const RootApex& a, const RootApex& b) {
            return heightOf(a.apex) > heightOf(b.apex);
        });
        std::vector<RootApex> kept;
        for (const RootApex& candidate : byHeight) {
            const bool apart = std::none_of(kept.begin(), kept.end(), [&](const RootApex& other) {
                return std::abs(lateralOf(other.apex) - lateralOf(candidate.apex)) < params.minApexSpacingMm - 1e-9;
            });
            if (apart)
                kept.push_back(candidate);
        }
        std::sort(kept.begin(), kept.end(), [&](const RootApex& a, const RootApex& b) {
            return lateralOf(a.apex) < lateralOf(b.apex);
        });
        analysis.apices = kept;
    }
    if (analysis.apices.empty()) {
        analysis.error = QStringLiteral("No se encontraron raíces bajo el corte.");
        return analysis;
    }

    // Naming from the cut's own landmarks: canine = longest root near the piriform point of its side, first
    // molar = root nearest the pillar point.
    const auto nearestTo = [&](const Vec3& landmark, bool longest, double reach) {
        int best = -1;
        double bestScore = std::numeric_limits<double>::max();
        for (size_t i = 0; i < analysis.apices.size(); ++i) {
            const double away = std::abs(lateralOf(analysis.apices[i].apex) - lateralOf(landmark));
            if (away > reach)
                continue;
            const double score = longest ? -analysis.apices[i].lengthMm : away;
            if (score < bestScore) {
                bestScore = score;
                best = static_cast<int>(i);
            }
        }
        return best;
    };
    analysis.named[static_cast<size_t>(RootTooth::FirstMolarRight)] = nearestTo(cut.points[0], false, 8.0);
    analysis.named[static_cast<size_t>(RootTooth::CanineRight)] = nearestTo(cut.points[1], true, params.canineReachMm);
    analysis.named[static_cast<size_t>(RootTooth::CanineLeft)] = nearestTo(cut.points[2], true, params.canineReachMm);
    analysis.named[static_cast<size_t>(RootTooth::FirstMolarLeft)] = nearestTo(cut.points[3], false, 8.0);

    QStringList lines;
    for (const RootTooth tooth : {RootTooth::FirstMolarRight, RootTooth::CanineRight, RootTooth::CanineLeft,
                                  RootTooth::FirstMolarLeft}) {
        const int index = analysis.named[static_cast<size_t>(tooth)];
        if (index < 0) {
            lines << QStringLiteral("%1: no se identifica.").arg(ToothName(tooth));
            continue;
        }
        const RootApex& apex = analysis.apices[static_cast<size_t>(index)];
        lines << QStringLiteral("%1: raíz %2, ápice a %3 del corte.")
                     .arg(ToothName(tooth), mm(apex.lengthMm), mm(apex.cutDistanceMm));
    }
    for (const RootApex& apex : analysis.apices)
        if (apex.tooClose)
            analysis.warnings << QStringLiteral("Un ápice queda a %1 del corte (mínimo %2).")
                                     .arg(mm(apex.cutDistanceMm), mm(params.minCutDistanceMm));
    analysis.report = QStringLiteral("Raíces: ") + lines.join(QStringLiteral(" ")) +
                      (analysis.warnings.isEmpty()
                           ? QStringLiteral(" Ningún ápice a menos de %1 del corte.").arg(mm(params.minCutDistanceMm))
                           : QStringLiteral(" ⚠ ") + analysis.warnings.join(QStringLiteral(" ")));
    analysis.ok = true;
    return analysis;
}
}
