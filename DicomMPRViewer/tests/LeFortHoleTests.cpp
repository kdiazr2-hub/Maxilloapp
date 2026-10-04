#include "LeFortHoleCore.h"

#include "ImplicitCore.h"
#include "LeFortMotionCore.h"
#include "OsteotomyCore.h"
#include "PlateCore.h"
#include "SplintTestGeometry.h"

#include <vtkAppendPolyData.h>
#include <vtkPolyData.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <string>

namespace
{
using namespace splinttest;
using Vec3 = std::array<double, 3>;
using Matrix = std::array<double, 16>;

// The anterior maxilla before the cut, face towards +Y, vertical +Z, patient's right towards −X. The cranial
// wall above the cut (z = 10..30) is 3 mm thick on the right and 1 mm thick on the left, with the sinus — air —
// behind it. The Le Fort segment below the cut (z = −10..8) is solid, 10 mm deep. The cut is level at z = 9.
std::vector<vtkSmartPointer<vtkPolyData>> cranialPieces()
{
    return {boxMesh({-25.0, 0.0, -3.0, 0.0, 10.0, 30.0}, false, false),
            boxMesh({0.0, 25.0, -1.0, 0.0, 10.0, 30.0}, false, false)};
}
vtkSmartPointer<vtkPolyData> segmentBeforeCut() { return boxMesh({-25.0, 25.0, -10.0, 0.0, -10.0, 8.0}, false, false); }

OsteotomyPath cutAt(double z)
{
    return OsteotomyCore::LeFortPath({{{-10.0, 5.0, z}, {10.0, 5.0, z}, {-20.0, -5.0, z}, {20.0, -5.0, z}}});
}

Matrix rise(double mm)
{
    return {1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, mm, 0.0, 0.0, 0.0, 1.0};
}

vtkSmartPointer<vtkPolyData> merged(const std::vector<vtkSmartPointer<vtkPolyData>>& meshes)
{
    auto append = vtkSmartPointer<vtkAppendPolyData>::New();
    for (const auto& mesh : meshes)
        append->AddInputData(mesh);
    append->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(append->GetOutput());
    return out;
}

vtkSmartPointer<vtkPolyData> moved(vtkPolyData* mesh, const Matrix& motion)
{
    auto transform = vtkSmartPointer<vtkTransform>::New();
    transform->SetMatrix(motion.data());
    auto filter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
    filter->SetInputData(mesh);
    filter->SetTransform(transform);
    filter->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(filter->GetOutput());
    return out;
}

// Everything a site is judged against, for a given cut and movement. Kept alive by the struct: the context
// only borrows the bone field.
struct Scene
{
    vtkSmartPointer<vtkPolyData> cranial;
    vtkSmartPointer<vtkPolyData> segment;
    vtkSmartPointer<vtkPolyData> segmentPlanned;
    std::shared_ptr<const ImplicitCore::BakedField> bone;
    LeFortHoleContext context;
};

Scene scene(const OsteotomyPath& cut, const Matrix& motion,
            const std::vector<vtkSmartPointer<vtkPolyData>>& cranialMeshes = cranialPieces())
{
    Scene s;
    s.cranial = merged(cranialMeshes);
    s.segment = segmentBeforeCut();
    s.segmentPlanned = moved(s.segment, motion);
    std::vector<vtkPolyData*> meshes{s.cranial, s.segment};
    s.bone = ImplicitCore::BakeMeshField(meshes, 0.2, 4.0);
    if (!s.bone)
        throw std::runtime_error("the bone field could not be baked");
    const Matrix still = rise(0.0);
    s.context.bone = s.bone.get();
    s.context.plannedBone = PlateCore::MakeBoneQuery(s.cranial, s.segmentPlanned, motion, {}, 0.0);
    s.context.preopBone = PlateCore::MakeBoneQuery(s.cranial, s.segment, still, {}, 0.0);
    s.context.cut = cut;
    s.context.motion = motion;
    const LeFortBandProfile band = LeFortMotionCore::Band(cut, motion);
    if (band.ok)
        s.context.band = band;
    s.context.anterior = {0.0, 1.0, 0.0};
    return s;
}

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

const Vec3 kFacing{0.0, 1.0, 0.0};

std::string describe(const LeFortHoleSupport& support)
{
    const char* verdict = support.verdict == LeFortSupportVerdict::Ok        ? "Ok"
                        : support.verdict == LeFortSupportVerdict::Warning ? "Warning"
                                                                             : "Rejected";
    return std::string(verdict) + " (" + std::to_string(support.thicknessMm) + " mm) " + support.reason.toStdString();
}

// spec §Acceptance 7: a site on a 3 mm wall, clear of everything, is sound and its bone is measured.
void testASiteOnAThreeMillimetreWallIsSound()
{
    const Scene s = scene(cutAt(9.0), rise(0.0001));
    const LeFortHoleSupport support = LeFortHoleCore::Support({-12.0, 0.0, 20.0}, kFacing, s.context);
    require(support.verdict == LeFortSupportVerdict::Ok, "a sound site was not accepted: " + describe(support));
    require(std::abs(support.thicknessMm - 3.0) <= 0.3, "the 3 mm wall was measured as " + describe(support));
    require(support.reason.isEmpty(), "a sound site carries a reason: " + describe(support));
}

// spec §Acceptance 9 and §Behaviour: on a 1 mm wall the hole is accepted with a warning that says why.
void testThinBoneIsAWarningNotARefusal()
{
    const Scene s = scene(cutAt(9.0), rise(0.0001));
    const LeFortHoleSupport support = LeFortHoleCore::Support({12.0, 0.0, 20.0}, kFacing, s.context);
    require(support.verdict == LeFortSupportVerdict::Warning, "thin bone was not a warning: " + describe(support));
    require(std::abs(support.thicknessMm - 1.0) <= 0.3, "the 1 mm wall was measured as " + describe(support));
    require(support.reason.contains(QStringLiteral("grosor")) && support.reason.contains(QStringLiteral("2.0")),
            "the warning does not give the thickness rule: " + describe(support));
}

// spec §Behaviour: a free bony margin is still refused, as the plates refuse it.
void testABonyMarginIsRefused()
{
    const Scene s = scene(cutAt(9.0), rise(0.0001));
    const LeFortHoleSupport support = LeFortHoleCore::Support({-23.0, 0.0, 20.0}, kFacing, s.context);
    require(support.verdict == LeFortSupportVerdict::Rejected && support.reason.contains(QStringLiteral("orilla")),
            "a hole on the lateral edge of the wall was allowed: " + describe(support));
}

// spec §Behaviour: closer than 4 mm to the osteotomy is refused. An imaginary cut across the middle of the
// wall, so the ring has bone all round and the cut is the only reason.
void testTooNearTheCutIsRefused()
{
    const Scene s = scene(cutAt(20.0), rise(0.0001));
    const LeFortHoleSupport support = LeFortHoleCore::Support({-12.0, 0.0, 17.0}, kFacing, s.context);
    require(support.verdict == LeFortSupportVerdict::Rejected && support.reason.contains(QStringLiteral("osteotom")),
            "a hole 3 mm from the osteotomy was allowed: " + describe(support));
}

// spec §Behaviour (user's decision, 2026-10-04): with a 4 mm impaction the bone from the cut up to 4 mm above
// it comes out. A cranial hole inside that band, or less than 4 mm above it, is refused for the band.
void testACranialHoleInOrNearTheBandIsRefused()
{
    const Scene s = scene(cutAt(9.0), rise(4.0)); // band from z = 9 to z = 13
    const LeFortHoleSupport inside = LeFortHoleCore::Support({-12.0, 0.0, 12.0}, kFacing, s.context);
    require(inside.verdict == LeFortSupportVerdict::Rejected && inside.reason.contains(QStringLiteral("franja")),
            "a hole inside the band was allowed or refused for another reason: " + describe(inside));
    const LeFortHoleSupport near = LeFortHoleCore::Support({-12.0, 0.0, 15.0}, kFacing, s.context);
    require(near.verdict == LeFortSupportVerdict::Rejected && near.reason.contains(QStringLiteral("franja")),
            "a hole 2 mm above the band was allowed: " + describe(near));
    const LeFortHoleSupport clear = LeFortHoleCore::Support({-12.0, 0.0, 19.0}, kFacing, s.context);
    require(clear.verdict == LeFortSupportVerdict::Ok, "a hole 6 mm above the band was refused: " + describe(clear));
}

// The band only takes cranial bone: a hole on the segment, judged where it is drilled before the cut, is
// sound even with an impaction, and its thickness is the segment's.
void testASegmentHoleIsNotTouchedByTheBand()
{
    const Scene s = scene(cutAt(9.0), rise(4.0));
    const LeFortHoleSupport support = LeFortHoleCore::Support({-12.0, 0.0, 0.0}, kFacing, s.context);
    require(support.verdict == LeFortSupportVerdict::Ok, "a sound hole on the segment was refused: " + describe(support));
    require(support.thicknessMm >= 9.5, "the segment's bone was not measured: " + describe(support));
}

// Off the bone altogether.
void testASiteOffTheBoneIsRefused()
{
    const Scene s = scene(cutAt(9.0), rise(0.0001));
    const LeFortHoleSupport support = LeFortHoleCore::Support({-12.0, 20.0, 20.0}, kFacing, s.context);
    require(support.verdict == LeFortSupportVerdict::Rejected, "a site in the air was allowed: " + describe(support));
}
// ── Proposing the holes ─────────────────────────────────────────────────────

// A maxilla with a piriform aperture and four pillars, face towards +Y. The cranial wall is 3 mm deep with the
// sinus behind it; the aperture is |x| < 6 from the cut up to z = 20. With `thinLeftPillar` the wall lateral to
// x = 12 on the left is only 1 mm deep. The cut is level at z = 9 through the pillars (x = ±20) and the
// piriform rims (x = ±10).
std::vector<vtkSmartPointer<vtkPolyData>> maxillaWithAperture(bool thinLeftPillar)
{
    std::vector<vtkSmartPointer<vtkPolyData>> pieces{boxMesh({-25.0, -6.0, -3.0, 0.0, 10.0, 30.0}, false, false),
                                                     boxMesh({-6.0, 6.0, -3.0, 0.0, 20.0, 30.0}, false, false)};
    if (thinLeftPillar) {
        pieces.push_back(boxMesh({6.0, 12.0, -3.0, 0.0, 10.0, 30.0}, false, false));
        pieces.push_back(boxMesh({12.0, 25.0, -1.0, 0.0, 10.0, 30.0}, false, false));
    } else {
        pieces.push_back(boxMesh({6.0, 25.0, -3.0, 0.0, 10.0, 30.0}, false, false));
    }
    return pieces;
}
OsteotomyPath pillarCut()
{
    return OsteotomyCore::LeFortPath({{{-10.0, 0.0, 9.0}, {10.0, 0.0, 9.0}, {-20.0, 0.0, 9.0}, {20.0, 0.0, 9.0}}});
}
double pillarX(LeFortPillar pillar)
{
    switch (pillar) {
    case LeFortPillar::PillarRight: return -20.0;
    case LeFortPillar::PiriformRight: return -10.0;
    case LeFortPillar::PiriformLeft: return 10.0;
    case LeFortPillar::PillarLeft: return 20.0;
    }
    return 0.0;
}
int count(const LeFortProposal& proposal, LeFortPillar pillar, LeFortCutSide side)
{
    int n = 0;
    for (const LeFortProposedHole& hole : proposal.holes)
        n += hole.pillar == pillar && hole.side == side ? 1 : 0;
    return n;
}
double distance(const Vec3& a, const Vec3& b)
{
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}
std::string where(const LeFortProposedHole& hole)
{
    return "(" + std::to_string(hole.center[0]) + ", " + std::to_string(hole.center[1]) + ", " +
           std::to_string(hole.center[2]) + ")";
}

// spec §Acceptance 7: with a 4 mm impaction and sound bone, 2 above and 2 below the cut at each of the four
// pillars — 16 — all on bone the support rules accept, at least 2 mm thick, apart, and the cranial ones clear
// of the band.
void testSixteenHolesAreProposedOnSoundBone()
{
    const Scene s = scene(pillarCut(), rise(4.0), maxillaWithAperture(false)); // band z = 9..13
    const LeFortProposal proposal = LeFortHoleCore::Propose(s.context);
    require(proposal.holes.size() == 16, "16 holes were expected, got " + std::to_string(proposal.holes.size()));
    require(proposal.missing.empty(), "sound bone reported missing holes");
    for (const LeFortPillar pillar : {LeFortPillar::PillarRight, LeFortPillar::PiriformRight, LeFortPillar::PiriformLeft,
                                      LeFortPillar::PillarLeft})
        for (const LeFortCutSide side : {LeFortCutSide::Cranial, LeFortCutSide::Segment})
            require(count(proposal, pillar, side) == 2,
                    "a pillar does not have 2 holes on one side: " + LeFortHoleCore::PillarName(pillar).toStdString() +
                        " " + LeFortHoleCore::SideName(side).toStdString());
    for (size_t i = 0; i < proposal.holes.size(); ++i) {
        const LeFortProposedHole& hole = proposal.holes[i];
        require(hole.origin == LeFortHoleOrigin::Auto, "a proposed hole is not marked automatic");
        require(hole.support.verdict == LeFortSupportVerdict::Ok && hole.support.thicknessMm >= 2.0,
                "a proposed hole is not on sound bone: " + where(hole));
        // The proposal agrees with the rules a surgeon's own click is judged by.
        const LeFortHoleSupport again = LeFortHoleCore::Support(hole.center, hole.axis, s.context);
        require(again.verdict == LeFortSupportVerdict::Ok, "Support refuses a proposed hole: " + where(hole));
        require(std::abs(hole.center[0] - pillarX(hole.pillar)) <= 8.0 + 1e-6, "a hole strayed from its pillar: " + where(hole));
        require(hole.axis[1] > 0.9, "a hole is not drilled into the anterior wall: " + where(hole));
        if (hole.side == LeFortCutSide::Cranial)
            require(hole.center[2] >= 17.0 - 1e-6 && hole.center[2] <= 25.0 + 1e-6,
                    "a cranial hole is not 4-12 mm above the band: " + where(hole));
        else
            require(hole.center[2] <= 5.0 + 1e-6 && hole.center[2] >= -3.0 - 1e-6,
                    "a segment hole is not 4-12 mm below the cut: " + where(hole));
        for (size_t j = i + 1; j < proposal.holes.size(); ++j)
            require(distance(hole.center, proposal.holes[j].center) >= 6.5 - 1e-6,
                    "two holes are closer than 6.5 mm: " + where(hole) + " " + where(proposal.holes[j]));
    }
}

// Without a band the cranial holes only keep 4 mm from the cut itself.
void testWithoutABandCranialHolesKeepFourMillimetresFromTheCut()
{
    const Scene s = scene(pillarCut(), rise(-3.0), maxillaWithAperture(false)); // a descent: no band
    const LeFortProposal proposal = LeFortHoleCore::Propose(s.context);
    require(proposal.holes.size() == 16, "16 holes were expected, got " + std::to_string(proposal.holes.size()));
    double lowest = 1e9;
    for (const LeFortProposedHole& hole : proposal.holes)
        if (hole.side == LeFortCutSide::Cranial)
            lowest = std::min(lowest, hole.center[2]);
    require(lowest >= 13.0 - 1e-6 && lowest < 17.0, "the cranial holes do not start 4 mm above the cut: " +
                                                        std::to_string(lowest));
}

// spec §Behaviour (no valid site): a pillar with only thin bone gets no hole on that side — never a weaker
// site — and the proposal says which and why.
void testAThinPillarIsReportedNotFilled()
{
    const Scene s = scene(pillarCut(), rise(4.0), maxillaWithAperture(true));
    const LeFortProposal proposal = LeFortHoleCore::Propose(s.context);
    require(count(proposal, LeFortPillar::PillarLeft, LeFortCutSide::Cranial) == 0,
            "holes were proposed on the thin left pillar");
    require(count(proposal, LeFortPillar::PiriformLeft, LeFortCutSide::Cranial) == 2 &&
                count(proposal, LeFortPillar::PillarRight, LeFortCutSide::Cranial) == 2 &&
                count(proposal, LeFortPillar::PillarLeft, LeFortCutSide::Segment) == 2,
            "the sound pillars lost holes");
    bool reported = false;
    for (const LeFortMissingHoles& gap : proposal.missing)
        reported = reported || (gap.pillar == LeFortPillar::PillarLeft && gap.side == LeFortCutSide::Cranial &&
                                gap.missing == 2 && !gap.reason.isEmpty());
    require(reported, "the thin pillar is not reported as missing its holes");
    for (const LeFortProposedHole& hole : proposal.holes)
        require(hole.support.verdict == LeFortSupportVerdict::Ok, "a weaker site was proposed: " + where(hole));
}

void testTheProposalIsDeterministic()
{
    const Scene s = scene(pillarCut(), rise(4.0), maxillaWithAperture(false));
    const LeFortProposal first = LeFortHoleCore::Propose(s.context);
    const LeFortProposal second = LeFortHoleCore::Propose(s.context);
    require(first.holes.size() == second.holes.size() && !first.holes.empty(), "two runs gave different counts");
    for (size_t i = 0; i < first.holes.size(); ++i)
        require(distance(first.holes[i].center, second.holes[i].center) < 1e-9 &&
                    first.holes[i].pillar == second.holes[i].pillar && first.holes[i].side == second.holes[i].side,
                "two runs gave different holes");
}
} // namespace

int main()
{
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"a site on a three millimetre wall is sound", testASiteOnAThreeMillimetreWallIsSound},
        {"thin bone is a warning, not a refusal", testThinBoneIsAWarningNotARefusal},
        {"a bony margin is refused", testABonyMarginIsRefused},
        {"too near the cut is refused", testTooNearTheCutIsRefused},
        {"a cranial hole in or near the band is refused", testACranialHoleInOrNearTheBandIsRefused},
        {"a segment hole is not touched by the band", testASegmentHoleIsNotTouchedByTheBand},
        {"a site off the bone is refused", testASiteOffTheBoneIsRefused},
        {"sixteen holes are proposed on sound bone", testSixteenHolesAreProposedOnSoundBone},
        {"without a band cranial holes keep four millimetres from the cut",
         testWithoutABandCranialHolesKeepFourMillimetresFromTheCut},
        {"a thin pillar is reported, not filled", testAThinPillarIsReportedNotFilled},
        {"the proposal is deterministic", testTheProposalIsDeterministic},
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
