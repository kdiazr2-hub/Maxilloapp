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

    // The teeth seen from above: the highest tooth point in each cell of a grid over the horizontal plane. A grid
    // along the cut alone stacked the molars behind one another, and the "first molar" took whichever posterior
    // tooth reached highest — an unerupted third molar above the cut (user's case, 2026-10-05).
    const Vec3 depth = unit(Vec3{up[1] * lateral[2] - up[2] * lateral[1], up[2] * lateral[0] - up[0] * lateral[2],
                                 up[0] * lateral[1] - up[1] * lateral[0]},
                            {0.0, 1.0, 0.0});
    const auto depthOf = [&](const Vec3& p) { return dot(p, depth); };
    const auto horizontal = [&](const Vec3& a, const Vec3& b) {
        const Vec3 d = sub(a, b);
        return norm(sub(d, scale(up, dot(d, up))));
    };
    // The surface, sampled at least every half cell: the vertices alone leave a coarse mesh's flat faces empty
    // and a flat root tip reads as several apices, one per corner.
    const double bin = std::max(0.25, params.binMm);
    std::vector<Vec3> pts;
    pts.reserve(static_cast<size_t>(teeth->GetNumberOfPoints()));
    for (vtkIdType i = 0; i < teeth->GetNumberOfPoints(); ++i) {
        double p[3];
        teeth->GetPoint(i, p);
        pts.push_back({p[0], p[1], p[2]});
    }
    for (vtkIdType c = 0; c < teeth->GetNumberOfCells(); ++c) {
        vtkIdType n = 0;
        const vtkIdType* ids = nullptr;
        teeth->GetCellPoints(c, n, ids);
        for (vtkIdType k = 1; k + 1 < n; ++k) { // a fan, for triangles and convex polygons alike
            Vec3 a, b, d;
            teeth->GetPoint(ids[0], a.data());
            teeth->GetPoint(ids[k], b.data());
            teeth->GetPoint(ids[k + 1], d.data());
            const double longest = std::max({norm(sub(b, a)), norm(sub(d, a)), norm(sub(d, b))});
            const int steps = static_cast<int>(std::ceil(longest / (0.5 * bin)));
            if (steps <= 1)
                continue;
            for (int i = 0; i <= steps; ++i)
                for (int j = 0; i + j <= steps; ++j)
                    pts.push_back(add(a, add(scale(sub(b, a), double(i) / steps), scale(sub(d, a), double(j) / steps))));
        }
    }
    const vtkIdType count = static_cast<vtkIdType>(pts.size());
    double minU = std::numeric_limits<double>::max(), maxU = std::numeric_limits<double>::lowest();
    double minV = minU, maxV = maxU;
    for (vtkIdType i = 0; i < count; ++i) {
        const Vec3& q = pts[static_cast<size_t>(i)];
        minU = std::min(minU, lateralOf(q));
        maxU = std::max(maxU, lateralOf(q));
        minV = std::min(minV, depthOf(q));
        maxV = std::max(maxV, depthOf(q));
    }
    const int nu = std::max(1, static_cast<int>(std::ceil((maxU - minU) / bin)) + 1);
    const int nv = std::max(1, static_cast<int>(std::ceil((maxV - minV) / bin)) + 1);
    const size_t cells = static_cast<size_t>(nu) * static_cast<size_t>(nv);
    std::vector<vtkIdType> top(cells, -1);
    std::vector<double> topHeight(cells, std::numeric_limits<double>::lowest());
    const auto cellOf = [&](const Vec3& q) {
        const int u = std::clamp(static_cast<int>(std::floor((lateralOf(q) - minU) / bin)), 0, nu - 1);
        const int v = std::clamp(static_cast<int>(std::floor((depthOf(q) - minV) / bin)), 0, nv - 1);
        return static_cast<size_t>(v) * static_cast<size_t>(nu) + static_cast<size_t>(u);
    };
    for (vtkIdType i = 0; i < count; ++i) {
        const Vec3& q = pts[static_cast<size_t>(i)];
        const size_t c = cellOf(q);
        if (heightOf(q) > topHeight[c]) {
            topHeight[c] = heightOf(q);
            top[c] = i;
        }
    }
    // Apices: cells highest within half the spacing all round (the first of a flat top, in grid order).
    const int half = std::max(1, static_cast<int>(std::round(0.5 * params.minApexSpacingMm / bin)));
    for (int v = 0; v < nv; ++v)
        for (int u = 0; u < nu; ++u) {
            const size_t c = static_cast<size_t>(v) * static_cast<size_t>(nu) + static_cast<size_t>(u);
            if (top[c] < 0)
                continue;
            bool peak = true;
            for (int dv = -half; dv <= half && peak; ++dv)
                for (int du = -half; du <= half && peak; ++du) {
                    const int nu2 = u + du, nv2 = v + dv;
                    if ((du == 0 && dv == 0) || nu2 < 0 || nu2 >= nu || nv2 < 0 || nv2 >= nv || du * du + dv * dv > half * half)
                        continue;
                    const size_t n = static_cast<size_t>(nv2) * static_cast<size_t>(nu) + static_cast<size_t>(nu2);
                    if (top[n] < 0)
                        continue;
                    peak = topHeight[c] >= topHeight[n] - 1e-6; // a flat top gives several: merged below
                }
            if (!peak)
                continue;
            RootApex apex;
            apex.apex = pts[static_cast<size_t>(top[c])];
            // The cusp: the lowest tooth point within the column round the apex.
            double cuspHeight = heightOf(apex.apex);
            apex.cusp = apex.apex;
            for (vtkIdType i = 0; i < count; ++i) {
                const Vec3& q = pts[static_cast<size_t>(i)];
                if (horizontal(q, apex.apex) <= params.columnRadiusMm && heightOf(q) < cuspHeight) {
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
    // A flat tip gives one peak per cell of it: each flat top (peaks of the same height, cell to cell) becomes
    // one apex at its middle. Then a forked tip or a molar's roots: of any within the spacing, the higher stays.
    const auto refresh = [&](RootApex& apex) {
        apex.cutDistanceMm = -OsteotomyCore::FieldAt(*field, apex.apex);
        apex.onCut = add(apex.apex, scale(up, apex.cutDistanceMm));
        apex.tooClose = apex.cutDistanceMm < params.minCutDistanceMm;
    };
    {
        std::vector<RootApex> flat;
        std::vector<bool> used(analysis.apices.size(), false);
        for (size_t i = 0; i < analysis.apices.size(); ++i) {
            if (used[i])
                continue;
            used[i] = true;
            std::vector<size_t> component{i};
            for (size_t k = 0; k < component.size(); ++k)
                for (size_t j = 0; j < analysis.apices.size(); ++j) {
                    const RootApex& a = analysis.apices[component[k]];
                    const RootApex& b = analysis.apices[j];
                    if (!used[j] && horizontal(a.apex, b.apex) <= 1.5 * bin &&
                        std::abs(heightOf(a.apex) - heightOf(b.apex)) <= 0.25) {
                        used[j] = true;
                        component.push_back(j);
                    }
                }
            RootApex apex = analysis.apices[i];
            Vec3 sum{0.0, 0.0, 0.0};
            for (const size_t k : component)
                sum = add(sum, analysis.apices[k].apex);
            apex.apex = scale(sum, 1.0 / static_cast<double>(component.size()));
            refresh(apex);
            flat.push_back(apex);
        }
        std::stable_sort(flat.begin(), flat.end(), [&](const RootApex& a, const RootApex& b) {
            return heightOf(a.apex) > heightOf(b.apex);
        });
        std::vector<RootApex> kept;
        for (const RootApex& candidate : flat) {
            const bool apart = std::none_of(kept.begin(), kept.end(), [&](const RootApex& other) {
                return horizontal(other.apex, candidate.apex) < params.minApexSpacingMm - 1e-9;
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
            const double away = horizontal(analysis.apices[i].apex, landmark);
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
    analysis.named[static_cast<size_t>(RootTooth::FirstMolarRight)] = nearestTo(cut.points[0], false, params.molarReachMm);
    analysis.named[static_cast<size_t>(RootTooth::CanineRight)] = nearestTo(cut.points[1], true, params.canineReachMm);
    analysis.named[static_cast<size_t>(RootTooth::CanineLeft)] = nearestTo(cut.points[2], true, params.canineReachMm);
    analysis.named[static_cast<size_t>(RootTooth::FirstMolarLeft)] = nearestTo(cut.points[3], false, params.molarReachMm);

    // What the surgeon measures (user's reference case): from the apex of each canine and first molar to the
    // osteotomy, below it or — when the cut runs through the root — above it.
    const auto toCut = [&](const RootApex& apex) {
        return apex.cutDistanceMm >= 0.0
                   ? QStringLiteral("ápice a %1 por debajo de la osteotomía").arg(mm(apex.cutDistanceMm))
                   : QStringLiteral("ápice a %1 por encima de la osteotomía: el corte cruza la raíz")
                         .arg(mm(-apex.cutDistanceMm));
    };
    QStringList lines;
    for (const RootTooth tooth : {RootTooth::FirstMolarRight, RootTooth::CanineRight, RootTooth::CanineLeft,
                                  RootTooth::FirstMolarLeft}) {
        const int index = analysis.named[static_cast<size_t>(tooth)];
        QString name = ToothName(tooth);
        name[0] = name[0].toUpper();
        if (index < 0) {
            lines << QStringLiteral("%1: no se identifica.").arg(name);
            continue;
        }
        const RootApex& apex = analysis.apices[static_cast<size_t>(index)];
        lines << QStringLiteral("%1: %2 (diente de %3).%4")
                     .arg(name, toCut(apex), mm(apex.lengthMm),
                          apex.tooClose ? QStringLiteral(" ⚠ menos de %1.").arg(mm(params.minCutDistanceMm))
                                        : QString());
        if (apex.tooClose)
            analysis.warnings << QStringLiteral("%1: %2.").arg(name, toCut(apex));
    }
    analysis.report = QStringLiteral("Ápice–osteotomía (mínimo %1):\n").arg(mm(params.minCutDistanceMm)) +
                      lines.join(QStringLiteral("\n"));
    analysis.ok = true;
    return analysis;
}
}
