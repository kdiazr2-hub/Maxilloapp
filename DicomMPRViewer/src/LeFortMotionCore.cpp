#include "LeFortMotionCore.h"

#include <algorithm>
#include <cmath>

namespace
{
using Vec3 = std::array<double, 3>;

Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 scale(const Vec3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double norm(const Vec3& a) { return std::sqrt(dot(a, a)); }

Vec3 moved(const std::array<double, 16>& m, const Vec3& p)
{
    return {m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3], m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7],
            m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]};
}

QString mm(double value) { return QString::number(value, 'f', 1) + QStringLiteral(" mm"); }

// Names of the four Le Fort points, in the cut's order.
QString pointName(size_t index, size_t count)
{
    if (count == 4) {
        static const char* names[] = {"pilar D", "piriforme D", "piriforme I", "pilar I"};
        return QString::fromUtf8(names[index]);
    }
    return QStringLiteral("P%1").arg(index + 1);
}
} // namespace

namespace LeFortMotionCore
{
double CutLength(const OsteotomyPath& cut)
{
    double length = 0.0;
    for (size_t i = 1; i < cut.points.size(); ++i)
        length += norm(sub(cut.points[i], cut.points[i - 1]));
    return length;
}

std::array<double, 3> PointAlongCut(const OsteotomyPath& cut, double arcLengthMm)
{
    if (cut.points.empty())
        return {0.0, 0.0, 0.0};
    double travelled = 0.0;
    for (size_t i = 1; i < cut.points.size(); ++i) {
        const Vec3 piece = sub(cut.points[i], cut.points[i - 1]);
        const double length = norm(piece);
        if (length > 0.0 && arcLengthMm <= travelled + length) {
            const double t = std::clamp((arcLengthMm - travelled) / length, 0.0, 1.0);
            return add(cut.points[i - 1], scale(piece, t));
        }
        travelled += length;
    }
    return arcLengthMm <= 0.0 ? cut.points.front() : cut.points.back();
}

LeFortBandProfile Band(const OsteotomyPath& cut, const std::array<double, 16>& motion, const LeFortBandParams& params)
{
    LeFortBandProfile profile;
    if (!cut.valid || cut.points.size() < 2) {
        profile.error = cut.error.isEmpty() ? QStringLiteral("Falta la trayectoria de la osteotomía Le Fort.") : cut.error;
        return profile;
    }
    const double verticalLength = norm(params.vertical);
    const Vec3 up = verticalLength > 1e-12 ? scale(params.vertical, 1.0 / verticalLength) : Vec3{0.0, 0.0, 1.0};

    // No movement: the segment has not been repositioned. Measured where it matters, at the cut's centre,
    // with the rotation angle from the trace, as `guideMotionSummary` does.
    Vec3 centre{0.0, 0.0, 0.0};
    for (const Vec3& p : cut.points)
        centre = add(centre, scale(p, 1.0 / static_cast<double>(cut.points.size())));
    const double shift = norm(sub(moved(motion, centre), centre));
    const double trace = motion[0] + motion[5] + motion[10];
    const double angleDeg = std::acos(std::clamp(0.5 * (trace - 1.0), -1.0, 1.0)) * 180.0 / 3.14159265358979323846;
    if (shift < params.noMotionMm && angleDeg < params.noMotionDeg) {
        profile.noMotion = true;
        profile.error = QStringLiteral("El Le Fort no tiene movimiento planificado: realice el movimiento en "
                                       "REPOSICIÓN. La guía se genera con un solo corte, sin franja.");
        return profile;
    }

    // The rise of each point: the vertical component of its displacement. A horizontal translation and a
    // rotation about the vertical leave it unchanged, so only the changes of plane count.
    std::vector<double> heights;
    heights.reserve(cut.points.size());
    for (const Vec3& p : cut.points)
        heights.push_back(dot(sub(moved(motion, p), p), up));
    return BandFromHeights(cut, heights, params);
}

LeFortBandProfile BandFromHeights(const OsteotomyPath& cut, const std::vector<double>& heights,
                                  const LeFortBandParams& params)
{
    LeFortBandProfile profile;
    if (!cut.valid || cut.points.size() < 2) {
        profile.error = cut.error.isEmpty() ? QStringLiteral("Falta la trayectoria de la osteotomía Le Fort.") : cut.error;
        return profile;
    }
    if (heights.size() != cut.points.size()) {
        profile.error = QStringLiteral("Hace falta una altura por cada punto del corte.");
        return profile;
    }
    const double verticalLength = norm(params.vertical);
    const Vec3 up = verticalLength > 1e-12 ? scale(params.vertical, 1.0 / verticalLength) : Vec3{0.0, 0.0, 1.0};
    const size_t count = cut.points.size();
    profile.heights = heights;

    // Where the rise reaches the threshold, as arc length. The rise is linear along each straight piece, so
    // each piece is either all in, all out, or in up to the exact crossing.
    const double threshold = params.thresholdMm;
    double travelled = 0.0;
    const auto addSpan = [&profile](double from, double to) {
        if (to - from <= 1e-9)
            return;
        if (!profile.spans.empty() && std::abs(profile.spans.back().second - from) <= 1e-9)
            profile.spans.back().second = to;
        else
            profile.spans.push_back({from, to});
    };
    for (size_t i = 1; i < count; ++i) {
        const double length = norm(sub(cut.points[i], cut.points[i - 1]));
        const double h0 = profile.heights[i - 1], h1 = profile.heights[i];
        const bool in0 = h0 >= threshold, in1 = h1 >= threshold;
        if (in0 && in1) {
            addSpan(travelled, travelled + length);
        } else if (in0 != in1) {
            const double t = std::clamp((threshold - h0) / (h1 - h0), 0.0, 1.0);
            if (in0)
                addSpan(travelled, travelled + t * length);
            else
                addSpan(travelled + t * length, travelled + length);
        }
        travelled += length;
    }

    // The band's upper edge: every point of the cut raised by its own rise.
    profile.upperCut = cut;
    for (size_t i = 0; i < count; ++i)
        profile.upperCut.points[i] = add(cut.points[i], scale(up, profile.heights[i]));

    const auto [lowest, highest] = std::minmax_element(profile.heights.begin(), profile.heights.end());
    const bool rises = *highest >= threshold, drops = *lowest <= -threshold;
    profile.kind = rises && drops ? LeFortBandKind::Mixed
                 : rises          ? LeFortBandKind::Impaction
                 : drops          ? LeFortBandKind::Descent
                                  : LeFortBandKind::NoPlaneChange;

    // Changes of plane, from the points themselves: right half against left half, and for the four Le Fort
    // points the piriform pair (front) against the pillars (back).
    const auto mean = [&profile](size_t from, size_t to) {
        double sum = 0.0;
        for (size_t i = from; i < to; ++i)
            sum += profile.heights[i];
        return sum / static_cast<double>(to - from);
    };
    const double sideDifference = mean(0, count / 2) - mean(count - count / 2, count);
    if (sideDifference >= params.planeDifferenceMm)
        profile.cant = LeFortCant::RightHigher;
    else if (sideDifference <= -params.planeDifferenceMm)
        profile.cant = LeFortCant::LeftHigher;
    if (count == 4) {
        const double frontDifference =
            0.5 * (profile.heights[1] + profile.heights[2]) - 0.5 * (profile.heights[0] + profile.heights[3]);
        if (frontDifference >= params.planeDifferenceMm)
            profile.pitch = LeFortPitch::CounterClockwise;
        else if (frontDifference <= -params.planeDifferenceMm)
            profile.pitch = LeFortPitch::Clockwise;
    }

    QStringList heightText;
    for (size_t i = 0; i < count; ++i)
        heightText << pointName(i, count) + QStringLiteral(" ") + (profile.heights[i] >= 0.0 ? QStringLiteral("+") : QString()) +
                       mm(profile.heights[i]);
    QString report;
    switch (profile.kind) {
    case LeFortBandKind::Impaction:
        report = QStringLiteral("Impactación: franja de hueso a quitar por encima del corte, hasta %1.").arg(mm(*highest));
        break;
    case LeFortBandKind::Mixed:
        report = QStringLiteral("Movimiento mixto: franja a quitar solo donde el Le Fort sube, hasta %1.").arg(mm(*highest));
        break;
    case LeFortBandKind::Descent:
        report = QStringLiteral("Descenso: sin franja, un solo corte (hasta %1 de descenso).").arg(mm(-*lowest));
        break;
    case LeFortBandKind::NoPlaneChange:
        report = QStringLiteral("Sin cambio de plano por encima de %1: un solo corte. El avance o retroceso lo "
                                "determina la placa.")
                     .arg(mm(threshold));
        break;
    }
    report += QStringLiteral(" Altura en el corte: ") + heightText.join(QStringLiteral(", ")) + QStringLiteral(".");
    if (profile.pitch == LeFortPitch::CounterClockwise)
        report += QStringLiteral(" Rotación antihoraria (sube más adelante que atrás).");
    else if (profile.pitch == LeFortPitch::Clockwise)
        report += QStringLiteral(" Rotación horaria (sube más atrás que adelante).");
    if (profile.cant == LeFortCant::RightHigher)
        report += QStringLiteral(" El lado derecho sube más.");
    else if (profile.cant == LeFortCant::LeftHigher)
        report += QStringLiteral(" El lado izquierdo sube más.");
    profile.report = report;
    profile.ok = true;
    return profile;
}
}
