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
// x = 11 on the left is only 1 mm deep, so the left pillar's whole window (x = 12..28) is thin. The cut is
// level at z = 9 through the pillars (x = ±20) and the piriform rims (x = ±10).
std::vector<vtkSmartPointer<vtkPolyData>> maxillaWithAperture(bool thinLeftPillar)
{
    std::vector<vtkSmartPointer<vtkPolyData>> pieces{boxMesh({-25.0, -6.0, -3.0, 0.0, 10.0, 30.0}, false, false),
                                                     boxMesh({-6.0, 6.0, -3.0, 0.0, 20.0, 30.0}, false, false)};
    if (thinLeftPillar) {
        pieces.push_back(boxMesh({6.0, 11.0, -3.0, 0.0, 10.0, 30.0}, false, false));
        pieces.push_back(boxMesh({11.0, 25.0, -1.0, 0.0, 10.0, 30.0}, false, false));
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
// The anterior wall of a real maxilla is often thinner than 2 mm, and refusing it left the surgeon with no holes
// at all (user's case, 2026-10-05: «Orificios: 0», nothing to accept). Thin bone is proposed with its warning,
// after any sound site, and the surgeon decides.
void testAThinPillarIsProposedWithAWarning()
{
    const Scene s = scene(pillarCut(), rise(4.0), maxillaWithAperture(true));
    const LeFortProposal proposal = LeFortHoleCore::Propose(s.context);
    require(count(proposal, LeFortPillar::PillarLeft, LeFortCutSide::Cranial) == 2,
            "the thin left pillar got no holes");
    require(count(proposal, LeFortPillar::PiriformLeft, LeFortCutSide::Cranial) == 2 &&
                count(proposal, LeFortPillar::PillarRight, LeFortCutSide::Cranial) == 2 &&
                count(proposal, LeFortPillar::PillarLeft, LeFortCutSide::Segment) == 2,
            "the sound pillars lost holes");
    for (const LeFortProposedHole& hole : proposal.holes) {
        const bool thin = hole.pillar == LeFortPillar::PillarLeft && hole.side == LeFortCutSide::Cranial &&
                          hole.center[0] > 11.0 + 2.8;
        if (thin)
            require(hole.support.verdict == LeFortSupportVerdict::Warning && hole.support.reason.contains(QStringLiteral("mm")),
                    "a site on thin bone is not marked as such: " + where(hole));
        if (hole.support.verdict == LeFortSupportVerdict::Rejected)
            require(false, "a refused site was proposed: " + where(hole));
    }
    for (const LeFortMissingHoles& gap : proposal.missing)
        require(!(gap.pillar == LeFortPillar::PillarLeft && gap.side == LeFortCutSide::Cranial),
                "the thin pillar is still reported as missing");
}

// When a pillar gets nothing, the report says what stopped it, not a generic sentence: here the wall beyond the
// left buttress faces sideways.
void testAnEmptyPillarSaysWhatBlockedIt()
{
    std::vector<vtkSmartPointer<vtkPolyData>> pieces{boxMesh({-25.0, -6.0, -3.0, 0.0, 10.0, 30.0}, false, false),
                                                     boxMesh({-6.0, 6.0, -3.0, 0.0, 20.0, 30.0}, false, false),
                                                     boxMesh({6.0, 14.0, -3.0, 0.0, 10.0, 30.0}, false, false)};
    auto turn = vtkSmartPointer<vtkTransform>::New();
    turn->Translate(14.0, 0.0, 0.0);
    turn->RotateZ(-70.0);
    auto filter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
    filter->SetInputData(boxMesh({0.0, 16.0, -3.0, 0.0, 10.0, 30.0}, false, false));
    filter->SetTransform(turn);
    filter->Update();
    auto side = vtkSmartPointer<vtkPolyData>::New();
    side->DeepCopy(filter->GetOutput());
    pieces.push_back(side);
    const Scene s = scene(pillarCut(), rise(4.0), pieces);
    const LeFortProposal proposal = LeFortHoleCore::Propose(s.context);
    bool named = false;
    for (const LeFortMissingHoles& gap : proposal.missing)
        if (gap.pillar == LeFortPillar::PillarLeft && gap.side == LeFortCutSide::Cranial)
            named = gap.reason.contains(QStringLiteral("hacia el lado"));
    require(named, "the left pillar's gap does not say the wall faces sideways");
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

// spec §Behaviour (edit) and §Revisions: a hole the surgeon moved stays where it was put, judged again; the
// app fills only what that pillar and side still lack, clear of it.
void testAMovedHoleIsKeptAndTheRestFillRoundIt()
{
    const Scene s = scene(pillarCut(), rise(4.0), maxillaWithAperture(false));
    const LeFortProposal first = LeFortHoleCore::Propose(s.context);
    require(first.holes.size() == 16, "the first proposal is incomplete");
    LeFortProposedHole manual;
    for (const LeFortProposedHole& hole : first.holes)
        if (hole.pillar == LeFortPillar::PillarRight && hole.side == LeFortCutSide::Cranial) {
            manual = hole;
            break;
        }
    manual.center = {manual.center[0] + 2.0, manual.center[1], manual.center[2] + 1.0};
    manual.origin = LeFortHoleOrigin::Manual;
    manual.support = {}; // stale: it must be judged again
    const LeFortProposal again = LeFortHoleCore::Propose(s.context, {manual});
    require(again.holes.size() == 16, "16 holes were expected around the moved one, got " +
                                          std::to_string(again.holes.size()));
    require(count(again, LeFortPillar::PillarRight, LeFortCutSide::Cranial) == 2,
            "the moved hole's pillar does not have exactly 2 holes above the cut");
    int kept = 0;
    for (const LeFortProposedHole& hole : again.holes) {
        if (hole.origin == LeFortHoleOrigin::Manual) {
            ++kept;
            require(distance(hole.center, manual.center) < 1e-9, "the moved hole was moved again: " + where(hole));
            require(hole.support.verdict == LeFortSupportVerdict::Ok && hole.support.thicknessMm > 2.0,
                    "the moved hole was not judged again: " + describe(hole.support));
        } else {
            require(distance(hole.center, manual.center) >= 6.5 - 1e-6,
                    "a proposed hole is closer than 6.5 mm to the moved one: " + where(hole));
        }
    }
    require(kept == 1, "the moved hole was not kept exactly once");
}

// A moved hole that the movement now puts inside the band is still the surgeon's: kept, refused, with why.
void testAMovedHoleInsideANewBandIsKeptAndRefused()
{
    const Scene s = scene(pillarCut(), rise(4.0), maxillaWithAperture(false)); // band z = 9..13
    LeFortProposedHole manual;
    manual.center = {-20.0, 0.0, 12.0};
    manual.axis = kFacing;
    manual.pillar = LeFortPillar::PillarRight;
    manual.side = LeFortCutSide::Cranial;
    manual.origin = LeFortHoleOrigin::Manual;
    const LeFortProposal proposal = LeFortHoleCore::Propose(s.context, {manual});
    const auto it = std::find_if(proposal.holes.begin(), proposal.holes.end(), [](const LeFortProposedHole& hole) {
        return hole.origin == LeFortHoleOrigin::Manual;
    });
    require(it != proposal.holes.end(), "the moved hole was dropped");
    require(it->support.verdict == LeFortSupportVerdict::Rejected && it->support.reason.contains(QStringLiteral("franja")),
            "the moved hole in the band is not refused with the band as reason: " + describe(it->support));
}

// The guide drills the holes as sleeves: each hole carries its centre, axis and bone over unchanged.
void testHolesBecomeTheGuidesDrillSites()
{
    LeFortProposedHole cranial;
    cranial.center = {1.0, 2.0, 3.0};
    cranial.axis = {0.0, 1.0, 0.0};
    cranial.side = LeFortCutSide::Cranial;
    LeFortProposedHole segment = cranial;
    segment.center = {4.0, 5.0, 6.0};
    segment.side = LeFortCutSide::Segment;
    const std::vector<PredictiveHole> sites = LeFortHoleCore::DrillSites({cranial, segment});
    require(sites.size() == 2, "every hole must become a drill site");
    require(sites[0].bone == PlateBone::Cranial && sites[1].bone == PlateBone::Segment, "the bone was not carried over");
    require(distance(sites[1].preopCenter, segment.center) < 1e-12 && distance(sites[1].preopAxis, segment.axis) < 1e-12,
            "the drill site is not the hole");
    require(sites[0].plate == -1, "a guide hole is not a plate hole");
}

// spec §Behaviour (earlier plates): plate holes with no guide hole within a millimetre are counted.
void testPlateHolesWithoutAGuideHoleAreCounted()
{
    LeFortProposedHole guide;
    guide.center = {0.0, 0.0, 20.0};
    PredictiveHole matching;
    matching.preopCenter = {0.5, 0.0, 20.0};
    PredictiveHole elsewhere;
    elsewhere.preopCenter = {8.0, 0.0, 20.0};
    require(LeFortHoleCore::UnmatchedPlateHoles({guide}, {matching, elsewhere}) == 1,
            "one plate hole has no guide hole and must be counted");
    require(LeFortHoleCore::UnmatchedPlateHoles({guide}, {}) == 0, "no plates, nothing to count");
}
// spec §Acceptance 8-9: a click near the bone (on the guide, which stands off it) moves the hole onto the
// bone itself, drilled along the bone's normal, judged where it landed, on whichever side of the cut that is.
void testAMovedHoleLandsOnTheBoneAndIsJudgedThere()
{
    const Scene s = scene(cutAt(9.0), rise(0.0001));
    LeFortProposedHole hole;
    hole.center = {-12.0, 0.0, 20.0};
    hole.axis = kFacing;
    hole.pillar = LeFortPillar::PiriformRight;
    hole.side = LeFortCutSide::Cranial;
    const LeFortProposedHole sound = LeFortHoleCore::MoveHole(hole, {-14.0, 2.5, 22.0}, s.context);
    require(std::abs(sound.center[1]) <= 0.15 && std::abs(sound.center[0] + 14.0) <= 0.15 &&
                std::abs(sound.center[2] - 22.0) <= 0.15,
            "the moved hole is not on the bone under the click: " + where(sound));
    require(sound.axis[1] > 0.95, "the moved hole is not drilled along the bone's normal");
    require(sound.origin == LeFortHoleOrigin::Manual && sound.pillar == LeFortPillar::PiriformRight,
            "a moved hole must be manual and keep its pillar");
    require(sound.support.verdict == LeFortSupportVerdict::Ok, "a sound site was not accepted: " + describe(sound.support));

    const LeFortProposedHole thin = LeFortHoleCore::MoveHole(hole, {12.0, 2.5, 20.0}, s.context);
    require(thin.support.verdict == LeFortSupportVerdict::Warning, "a 1 mm wall is not a warning: " + describe(thin.support));

    const LeFortProposedHole below = LeFortHoleCore::MoveHole(hole, {-12.0, 2.5, 0.0}, s.context);
    require(below.side == LeFortCutSide::Segment, "a hole moved below the cut is not on the segment");
}

// The piriform pillars look for bone outward from the rim, never towards the midline: below the aperture lie
// the incisor roots and the spine (user's case, 2026-10-05: holes proposed under the nose, by the incisors).
void testPiriformHolesStayLateralOfTheRim()
{
    const Scene s = scene(pillarCut(), rise(4.0), maxillaWithAperture(false));
    const LeFortProposal proposal = LeFortHoleCore::Propose(s.context);
    require(!proposal.holes.empty(), "nothing was proposed");
    for (const LeFortProposedHole& hole : proposal.holes) {
        if (hole.pillar == LeFortPillar::PiriformRight)
            require(hole.center[0] <= -10.0 + 0.5, "a right piriform hole went towards the midline: " + where(hole));
        if (hole.pillar == LeFortPillar::PiriformLeft)
            require(hole.center[0] >= 10.0 - 0.5, "a left piriform hole went towards the midline: " + where(hole));
    }
}

// A wall that faces sideways — the zygoma beyond the buttress — is no place for a guide's sleeve: the guide
// sits on the anterior wall (user's case, 2026-10-05: holes on the lateral zygoma).
void testAWallFacingSidewaysIsNotProposed()
{
    std::vector<vtkSmartPointer<vtkPolyData>> pieces{boxMesh({-25.0, -6.0, -3.0, 0.0, 10.0, 30.0}, false, false),
                                                     boxMesh({-6.0, 6.0, -3.0, 0.0, 20.0, 30.0}, false, false),
                                                     boxMesh({6.0, 14.0, -3.0, 0.0, 10.0, 30.0}, false, false)};
    // Beyond x = 14 on the left the wall turns to face 70° sideways.
    auto turn = vtkSmartPointer<vtkTransform>::New();
    turn->Translate(14.0, 0.0, 0.0);
    turn->RotateZ(-70.0);
    auto filter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
    filter->SetInputData(boxMesh({0.0, 16.0, -3.0, 0.0, 10.0, 30.0}, false, false));
    filter->SetTransform(turn);
    filter->Update();
    auto side = vtkSmartPointer<vtkPolyData>::New();
    side->DeepCopy(filter->GetOutput());
    pieces.push_back(side);
    const Scene s = scene(pillarCut(), rise(4.0), pieces);
    const LeFortProposal proposal = LeFortHoleCore::Propose(s.context);
    for (const LeFortProposedHole& hole : proposal.holes)
        require(hole.axis[1] >= 0.5, "a hole was proposed on a wall facing sideways: " + where(hole) + " axis (" +
                                         std::to_string(hole.axis[0]) + ", " + std::to_string(hole.axis[1]) + ")");
}

// A screw must not reach a root: the drill path, to the screw's depth, keeps a millimetre from the upper teeth
// (user's reference case measures the roots before placing anything).
void testADrillNearARootIsAWarningThatNamesIt()
{
    Scene s = scene(cutAt(9.0), rise(0.0001));
    auto root = boxMesh({-13.0, -11.0, -5.0, -3.0, -10.0, 6.0}, false, false); // a canine root, 3 mm behind the wall
    std::vector<vtkPolyData*> teethMeshes{root};
    const auto teeth = ImplicitCore::BakeMeshField(teethMeshes, 0.2, 4.0);
    require(teeth != nullptr, "the teeth field could not be baked");
    s.context.teeth = teeth.get();
    const LeFortHoleSupport onRoot = LeFortHoleCore::Support({-12.0, 0.0, 2.0}, kFacing, s.context);
    // The surgeon decides (user's request, 2026-10-05: two holes below the cut at every pillar, where the roots
    // are): a warning that says so, never a silent site.
    require(onRoot.verdict == LeFortSupportVerdict::Warning && onRoot.reason.contains(QStringLiteral("raíz")),
            "a screw into a root is not flagged: " + describe(onRoot));
    const LeFortHoleSupport clear = LeFortHoleCore::Support({-20.0, 0.0, 2.0}, kFacing, s.context);
    require(clear.verdict == LeFortSupportVerdict::Ok, "a screw 7 mm from the root was refused: " + describe(clear));
    const LeFortProposal proposal = LeFortHoleCore::Propose(s.context);
    for (const LeFortProposedHole& hole : proposal.holes)
        require(hole.center[0] <= -14.0 + 1e-6 || hole.center[0] >= -10.0 - 1e-6 || hole.side == LeFortCutSide::Cranial ||
                    hole.center[2] > 7.0,
                "a hole was proposed over the root: " + where(hole));
}

// spec, user's request 2026-10-05: at each pillar two holes above the cut and two below, on the pillar itself —
// one over the other along it — not spread over the wall wherever the bone is thickest.
void testTwoAboveAndTwoBelowInAColumnOnEachPillar()
{
    const Scene s = scene(pillarCut(), rise(4.0), maxillaWithAperture(false));
    const LeFortProposal proposal = LeFortHoleCore::Propose(s.context);
    require(proposal.holes.size() == 16, "16 holes were expected, got " + std::to_string(proposal.holes.size()));
    for (const LeFortProposedHole& hole : proposal.holes)
        require(std::abs(hole.center[0] - pillarX(hole.pillar)) <= 1.0 + 1e-6,
                "a hole is not on its pillar: " + where(hole));
}

// Roots under the whole wall below the cut: the pillars still get their two holes below, each flagged with the
// root it comes near, so the surgeon sees them and decides.
void testRootsDoNotLeaveAPillarWithoutHolesBelow()
{
    Scene s = scene(pillarCut(), rise(4.0), maxillaWithAperture(false));
    auto roots = boxMesh({-25.0, 25.0, -6.0, -2.0, -10.0, 7.0}, false, false);
    std::vector<vtkPolyData*> teethMeshes{roots};
    const auto teeth = ImplicitCore::BakeMeshField(teethMeshes, 0.2, 4.0);
    s.context.teeth = teeth.get();
    const LeFortProposal proposal = LeFortHoleCore::Propose(s.context);
    for (const LeFortPillar pillar : {LeFortPillar::PillarRight, LeFortPillar::PiriformRight, LeFortPillar::PiriformLeft,
                                      LeFortPillar::PillarLeft})
        require(count(proposal, pillar, LeFortCutSide::Segment) == 2,
                "a pillar lost its holes below the cut: " + LeFortHoleCore::PillarName(pillar).toStdString());
    for (const LeFortProposedHole& hole : proposal.holes)
        if (hole.side == LeFortCutSide::Segment)
            require(hole.support.verdict == LeFortSupportVerdict::Warning && hole.support.reason.contains(QStringLiteral("raíz")),
                    "a hole near the roots is not flagged: " + where(hole));
}

// The usable bone at a pillar is often short: a thin or perforated wall leaves a few millimetres. Taking the best
// site first and then looking for a second one 6.5 mm away left one hole per pillar on the surgeon's case
// (2026-10-06: "los puntos no salen donde es"). The pair is chosen together.
void testTwoHolesFitWhereTheBoneIsShort()
{
    std::vector<vtkSmartPointer<vtkPolyData>> pieces{boxMesh({-25.0, -6.0, -3.0, 0.0, 10.0, 30.0}, false, false),
                                                     boxMesh({-6.0, 6.0, -3.0, 0.0, 20.0, 30.0}, false, false),
                                                     boxMesh({6.0, 14.0, -3.0, 0.0, 10.0, 30.0}, false, false),
                                                     boxMesh({14.0, 25.0, -3.0, 0.0, 10.0, 25.0}, false, false)};
    const Scene s = scene(pillarCut(), rise(4.0), pieces); // band z 9..13: the window above it is z 17..25
    const LeFortProposal proposal = LeFortHoleCore::Propose(s.context);
    require(count(proposal, LeFortPillar::PillarLeft, LeFortCutSide::Cranial) == 2,
            "the short pillar got " + std::to_string(count(proposal, LeFortPillar::PillarLeft, LeFortCutSide::Cranial)) +
                " hole(s) above the cut instead of 2");
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
        {"a thin pillar is proposed with a warning", testAThinPillarIsProposedWithAWarning},
        {"an empty pillar says what blocked it", testAnEmptyPillarSaysWhatBlockedIt},
        {"the proposal is deterministic", testTheProposalIsDeterministic},
        {"a moved hole is kept and the rest fill round it", testAMovedHoleIsKeptAndTheRestFillRoundIt},
        {"a moved hole inside a new band is kept and refused", testAMovedHoleInsideANewBandIsKeptAndRefused},
        {"holes become the guide's drill sites", testHolesBecomeTheGuidesDrillSites},
        {"plate holes without a guide hole are counted", testPlateHolesWithoutAGuideHoleAreCounted},
        {"a moved hole lands on the bone and is judged there", testAMovedHoleLandsOnTheBoneAndIsJudgedThere},
        {"piriform holes stay lateral of the rim", testPiriformHolesStayLateralOfTheRim},
        {"a wall facing sideways is not proposed", testAWallFacingSidewaysIsNotProposed},
        {"a drill near a root is a warning that names it", testADrillNearARootIsAWarningThatNamesIt},
        {"two holes fit where the bone is short", testTwoHolesFitWhereTheBoneIsShort},
        {"two above and two below in a column on each pillar", testTwoAboveAndTwoBelowInAColumnOnEachPillar},
        {"roots do not leave a pillar without holes below", testRootsDoNotLeaveAPillarWithoutHolesBelow},
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
