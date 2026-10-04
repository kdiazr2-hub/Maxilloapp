#include "LeFortMotionCore.h"

#include "OsteotomyCore.h"

#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
using Vec3 = std::array<double, 3>;
using Matrix = std::array<double, 16>;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

bool near(double value, double expected, double tolerance) { return std::abs(value - expected) <= tolerance; }

// The synthetic maxilla of LeFortGuideTests: face towards +Y, vertical +Z, patient's right towards −X. The
// cut is level at z = 9: pilar D (−20, −5), piriforme D (−10, 5), piriforme I (10, 5), pilar I (20, −5).
OsteotomyPath leFortCut()
{
    return OsteotomyCore::LeFortPath({{{-10.0, 5.0, 9.0}, {10.0, 5.0, 9.0}, {-20.0, -5.0, 9.0}, {20.0, -5.0, 9.0}}});
}

// Row-major rigid motion: rotate by `angle` about `axis` through `pivot`, then translate by `shift`.
Matrix motion(const Vec3& axis, double angle, const Vec3& pivot, const Vec3& shift)
{
    const double length = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    const double x = axis[0] / length, y = axis[1] / length, z = axis[2] / length;
    const double c = std::cos(angle), s = std::sin(angle), t = 1.0 - c;
    const double r[3][3] = {{t * x * x + c, t * x * y - s * z, t * x * z + s * y},
                            {t * x * y + s * z, t * y * y + c, t * y * z - s * x},
                            {t * x * z - s * y, t * y * z + s * x, t * z * z + c}};
    Matrix m{};
    for (int i = 0; i < 3; ++i) {
        double rc = 0.0;
        for (int j = 0; j < 3; ++j) {
            m[static_cast<size_t>(4 * i + j)] = r[i][j];
            rc += r[i][j] * pivot[static_cast<size_t>(j)];
        }
        m[static_cast<size_t>(4 * i + 3)] = pivot[static_cast<size_t>(i)] - rc + shift[static_cast<size_t>(i)];
    }
    m[15] = 1.0;
    return m;
}
Matrix translation(const Vec3& shift) { return motion({0.0, 0.0, 1.0}, 0.0, {0.0, 0.0, 0.0}, shift); }

Matrix compose(const Matrix& second, const Matrix& first)
{
    Matrix out{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k)
                out[static_cast<size_t>(4 * i + j)] +=
                    second[static_cast<size_t>(4 * i + k)] * first[static_cast<size_t>(4 * k + j)];
    return out;
}

// How far a point rises under a motion: the reference the band has to reproduce.
double rise(const Matrix& m, const Vec3& p) { return m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11] - p[2]; }

LeFortBandProfile band(const Matrix& m)
{
    const LeFortBandProfile profile = LeFortMotionCore::Band(leFortCut(), m);
    require(profile.ok, "the band was not computed: " + profile.error.toStdString());
    require(profile.heights.size() == 4, "one height per point of the cut was expected, got " +
                                             std::to_string(profile.heights.size()));
    return profile;
}

std::string heightsText(const LeFortBandProfile& profile)
{
    std::string text;
    for (const double h : profile.heights)
        text += std::to_string(h) + " ";
    return text;
}

// spec §Acceptance 1: a uniform 3 mm impaction is a 3 mm band above the whole cut.
void testAUniformImpactionIsABandOfItsHeightAboveTheCut()
{
    const LeFortBandProfile profile = band(translation({0.0, 0.0, 3.0}));
    for (const double h : profile.heights)
        require(near(h, 3.0, 0.5), "a uniform 3 mm impaction gave heights " + heightsText(profile));
    require(profile.kind == LeFortBandKind::Impaction, "a uniform rise is not classed as an impaction");
    require(profile.spans.size() == 1, "the band should run the whole cut in one stretch");
    const double length = LeFortMotionCore::CutLength(leFortCut());
    require(near(length, 2.0 * std::sqrt(200.0) + 20.0, 1e-6), "the cut length is wrong: " + std::to_string(length));
    require(near(profile.spans.front().first, 0.0, 1e-6) && near(profile.spans.front().second, length, 1e-6),
            "the band does not cover the cut from end to end");
    // The upper edge is the cut raised 3 mm, above it.
    const OsteotomyPath cut = leFortCut();
    require(profile.upperCut.points.size() == cut.points.size(), "the upper cut lost points");
    for (size_t i = 0; i < cut.points.size(); ++i) {
        require(near(profile.upperCut.points[i][2], cut.points[i][2] + 3.0, 1e-6) &&
                    near(profile.upperCut.points[i][0], cut.points[i][0], 1e-9) &&
                    near(profile.upperCut.points[i][1], cut.points[i][1], 1e-9),
                "the upper cut is not the cut raised by the band");
    }
    require(profile.pitch == LeFortPitch::None && profile.cant == LeFortCant::None,
            "a level rise reports a rotation");
    require(!profile.report.isEmpty() && profile.report.contains(QStringLiteral("mm")), "the report is empty");
}

// spec §Acceptance 2: 4 mm on the right and 1 mm on the left — the band follows the cant.
void testAnAsymmetricImpactionGivesEachSideItsHeight()
{
    // Rolled about the antero-posterior axis through the middle of the cut, plus a rise: x = −15 goes up
    // 4 mm and x = +15 goes up 1 mm.
    const double angle = std::asin(0.1);
    const Matrix m = motion({0.0, 1.0, 0.0}, angle, {0.0, 0.0, 9.0}, {0.0, 0.0, 2.5});
    const LeFortBandProfile profile = band(m);
    const OsteotomyPath cut = leFortCut();
    for (size_t i = 0; i < cut.points.size(); ++i)
        require(near(profile.heights[i], rise(m, cut.points[i]), 1e-6), "a height is not the point's rise");
    const double right = 0.5 * (profile.heights[0] + profile.heights[1]);
    const double left = 0.5 * (profile.heights[2] + profile.heights[3]);
    require(near(right, 4.0, 0.5) && near(left, 1.0, 0.5),
            "the right side should be 4 mm and the left 1 mm: " + heightsText(profile));
    require(profile.cant == LeFortCant::RightHigher, "the right side going up more is not reported");
    require(profile.kind == LeFortBandKind::Impaction, "a rise everywhere is not an impaction");
}

// spec §Acceptance 3: an anterior impaction (counter-clockwise rotation) is higher at the front.
void testAnAnteriorImpactionIsHigherAtTheFront()
{
    // Rotated about the lateral axis through the pillars at cut height: the piriform points, 10 mm in front,
    // rise 4 mm; the pillars do not move vertically.
    const Matrix m = motion({1.0, 0.0, 0.0}, std::asin(0.4), {0.0, -5.0, 9.0}, {0.0, 0.0, 0.0});
    const LeFortBandProfile profile = band(m);
    require(near(profile.heights[1], 4.0, 0.5) && near(profile.heights[2], 4.0, 0.5),
            "the piriform points should rise 4 mm: " + heightsText(profile));
    require(near(profile.heights[0], 0.0, 0.5) && near(profile.heights[3], 0.0, 0.5),
            "the pillars should not rise: " + heightsText(profile));
    require(profile.heights[1] > profile.heights[0] && profile.heights[2] > profile.heights[3],
            "the band is not higher at the front than at the back on each side");
    require(profile.pitch == LeFortPitch::CounterClockwise, "the anterior impaction is not counter-clockwise");
    // The band starts and ends where the rise crosses 0.5 mm, an eighth of the way up each oblique piece.
    require(profile.spans.size() == 1, "the band should be one stretch across the front");
    const double piece = std::sqrt(200.0);
    const double length = LeFortMotionCore::CutLength(leFortCut());
    require(near(profile.spans.front().first, piece / 8.0, 0.05) &&
                near(profile.spans.front().second, length - piece / 8.0, 0.05),
            "the band's ends are not where the rise crosses the threshold: " +
                std::to_string(profile.spans.front().first) + " .. " + std::to_string(profile.spans.front().second));
}

// spec §Acceptance 4: a pure 5 mm advancement changes no plane — no band.
void testAPureAdvancementGivesNoBand()
{
    const LeFortBandProfile profile = band(translation({0.0, 5.0, 0.0}));
    for (const double h : profile.heights)
        require(near(h, 0.0, 1e-9), "an advancement gave a height: " + heightsText(profile));
    require(profile.spans.empty(), "an advancement gave a band");
    require(profile.kind == LeFortBandKind::NoPlaneChange, "an advancement is not classed as no plane change");
}

// spec §Acceptance 5: advancing as well does not change a 3 mm impaction's band.
void testAdvancingDoesNotChangeTheImpactionBand()
{
    const LeFortBandProfile alone = band(translation({0.0, 0.0, 3.0}));
    const LeFortBandProfile advanced = band(translation({0.0, 5.0, 3.0}));
    for (size_t i = 0; i < alone.heights.size(); ++i)
        require(near(advanced.heights[i], alone.heights[i], 1e-9), "the advancement changed the band");
    require(advanced.spans == alone.spans, "the advancement changed where the band runs");
}

// Neither a lateral shift nor a rotation in the horizontal plane changes the band.
void testLateralShiftAndYawDoNotChangeTheBand()
{
    const Matrix impaction = translation({0.0, 0.0, 3.0});
    const Matrix yawed = compose(motion({0.0, 0.0, 1.0}, 10.0 * 3.14159265358979323846 / 180.0, {0.0, 0.0, 0.0},
                                        {2.0, 0.0, 0.0}),
                                 impaction);
    const LeFortBandProfile alone = band(impaction);
    const LeFortBandProfile moved = band(yawed);
    for (size_t i = 0; i < alone.heights.size(); ++i)
        require(near(moved.heights[i], alone.heights[i], 1e-9), "a lateral shift or a yaw changed the band");
}

// spec §Acceptance 6: a 3 mm descent leaves a single cut.
void testADescentGivesNoBand()
{
    const LeFortBandProfile profile = band(translation({0.0, 0.0, -3.0}));
    for (const double h : profile.heights)
        require(near(h, -3.0, 0.5), "a 3 mm descent gave heights " + heightsText(profile));
    require(profile.spans.empty(), "a descent gave a band");
    require(profile.kind == LeFortBandKind::Descent, "a drop everywhere is not classed as a descent");
}

// spec §Behaviour (mixed): up on one side, down on the other — the band is only where it goes up, and it
// ends exactly where the rise falls to the threshold.
void testAMixedMovementHasABandOnlyWhereItRises()
{
    // Rolled about the antero-posterior axis at cut height: the rise is −0.1·x.
    const Matrix m = motion({0.0, 1.0, 0.0}, std::asin(0.1), {0.0, 0.0, 9.0}, {0.0, 0.0, 0.0});
    const LeFortBandProfile profile = band(m);
    require(profile.kind == LeFortBandKind::Mixed, "up on one side and down on the other is not mixed");
    require(profile.spans.size() == 1, "the band should be one stretch on the right");
    // 0.5 mm of rise at x = −5, which is 5 mm past the right piriform point along the cut.
    const double expectedEnd = std::sqrt(200.0) + 5.0;
    require(near(profile.spans.front().first, 0.0, 1e-6) && near(profile.spans.front().second, expectedEnd, 0.05),
            "the band does not end where the rise crosses 0.5 mm: " + std::to_string(profile.spans.front().second));
    require(profile.cant == LeFortCant::RightHigher, "the right side going up is not reported");
}

// Under 0.5 mm there is nothing to take out.
void testARiseBelowTheThresholdGivesNoBand()
{
    const LeFortBandProfile profile = band(translation({0.0, 0.0, 0.4}));
    require(profile.spans.empty(), "a 0.4 mm rise gave a band");
    require(profile.kind == LeFortBandKind::NoPlaneChange, "a rise under the threshold is not 'no plane change'");
}

// spec §Acceptance 11: without REPOSICIÓN there is no movement, and the band says so instead of inventing one.
void testNoMovementIsReportedAndGivesNoBand()
{
    const LeFortBandProfile profile =
        LeFortMotionCore::Band(leFortCut(), translation({0.0, 0.0, 0.0}));
    require(!profile.ok && profile.noMotion, "a segment that did not move was not reported as such");
    require(!profile.error.isEmpty(), "no message for the missing movement");
    require(profile.spans.empty(), "a band was made without movement");
}

void testAnInvalidCutIsRefused()
{
    OsteotomyPath broken;
    const LeFortBandProfile profile = LeFortMotionCore::Band(broken, translation({0.0, 0.0, 3.0}));
    require(!profile.ok && !profile.noMotion && !profile.error.isEmpty(), "an invalid cut was not refused");
}

void testPointsAlongTheCut()
{
    const OsteotomyPath cut = leFortCut();
    const double piece = std::sqrt(200.0);
    const Vec3 start = LeFortMotionCore::PointAlongCut(cut, 0.0);
    const Vec3 piriform = LeFortMotionCore::PointAlongCut(cut, piece);
    const Vec3 middle = LeFortMotionCore::PointAlongCut(cut, piece + 10.0);
    const Vec3 past = LeFortMotionCore::PointAlongCut(cut, 1000.0);
    require(near(start[0], -20.0, 1e-9) && near(start[1], -5.0, 1e-9), "the cut does not start at pilar D");
    require(near(piriform[0], -10.0, 1e-9) && near(piriform[1], 5.0, 1e-9), "piriforme D is not one piece along");
    require(near(middle[0], 0.0, 1e-9) && near(middle[1], 5.0, 1e-9), "the midline is not half way");
    require(near(past[0], 20.0, 1e-9) && near(past[1], -5.0, 1e-9), "past the end is not clamped to pilar I");
}
} // namespace

int main()
{
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"a uniform impaction is a band of its height above the cut", testAUniformImpactionIsABandOfItsHeightAboveTheCut},
        {"an asymmetric impaction gives each side its height", testAnAsymmetricImpactionGivesEachSideItsHeight},
        {"an anterior impaction is higher at the front", testAnAnteriorImpactionIsHigherAtTheFront},
        {"a pure advancement gives no band", testAPureAdvancementGivesNoBand},
        {"advancing does not change the impaction band", testAdvancingDoesNotChangeTheImpactionBand},
        {"lateral shift and yaw do not change the band", testLateralShiftAndYawDoNotChangeTheBand},
        {"a descent gives no band", testADescentGivesNoBand},
        {"a mixed movement has a band only where it rises", testAMixedMovementHasABandOnlyWhereItRises},
        {"a rise below the threshold gives no band", testARiseBelowTheThresholdGivesNoBand},
        {"no movement is reported and gives no band", testNoMovementIsReportedAndGivesNoBand},
        {"an invalid cut is refused", testAnInvalidCutIsRefused},
        {"points along the cut", testPointsAlongTheCut},
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
