#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// PlateCore
//
// Patient-specific Le Fort I osteosynthesis: the plates and the "predictive
// hole" technique that ties them to the cutting guide (Qaisi 2021; Mohamed et
// al., Arch Craniofac Surg 2026; Abdelhamid et al., Cureus 2025).
//
//   1. The plate is designed on the bone in its PLANNED position: cranial base
//      plus the Le Fort segment after repositioning. Its screw holes sit on
//      either side of the osteotomy.
//   2. Each hole is assigned to the bone it lies on. Holes on the cranial base
//      do not move; holes on the segment are carried back to the PRE-OPERATIVE
//      position by the inverse of the segment's planned motion.
//   3. The cutting guide, built on the pre-operative bone, carries a drill
//      sleeve at every one of those pre-operative holes. They are drilled
//      before the cut; after the osteotomy the plate only meets its holes when
//      the maxilla sits exactly where it was planned.
//
// The plate is a field like the guide (never a chain of mesh booleans): a
// ribbon of uniform thickness swept along each arm, lying on the bone. Each arm
// follows its holes' bone while that bone is under it and turns gently. Where
// advancement separates the cut edges, it leaves a free margin at the superior
// Le Fort edge, turns inward to the cranial limit and then rises on the cranium —
// the stepped contour of a surgically bent plate. Edges are rounded by sweeping a shrunken ribbon and growing it back,
// the bone carves the seat where the flat ribbon meets a curve across its width,
// and the screw bores and countersinks are subtracted. Contoured once.
//
// The segment's motion is recovered from the meshes themselves: repositioning
// moves every vertex rigidly, so the pre-operative and planned copies of the
// segment correspond vertex by vertex and a least-squares rigid fit (Kabsch)
// gives the exact matrix without any bookkeeping in the repositioning step.
//
// No Qt Widgets.
// ─────────────────────────────────────────────────────────────────────────────

#include "GuideDesignCore.h"
#include "ImplicitCore.h"
#include "OsteotomyCore.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <vtkSmartPointer.h>

#include <array>
#include <atomic>
#include <functional>
#include <vector>

class vtkPolyData;

enum class PlateTemplate
{
    Paranasal, // minimally invasive: one strut along the piriform rim
    LShape,    // legacy: piriform strut + zygomaticomaxillary buttress strut, joined below the cut
    Splintless // four pillars joined across the repositioned Le Fort segment
};

enum class PlateSide
{
    Right,
    Left,
    Bilateral
};

// The bone a screw goes into.
enum class PlateBone
{
    Unknown,
    Cranial, // does not move
    Segment  // the Le Fort segment: moves with the plan
};

// A screw hole, placed on the bone in its planned position.
struct PlateHole
{
    std::array<double, 3> center{0.0, 0.0, 0.0}; // on the planned bone surface
    std::array<double, 3> axis{0.0, 0.0, 1.0};   // outward surface normal there: the screw goes in along -axis
    PlateBone bone = PlateBone::Unknown;
};

// One plate: its holes and the struts that join them. A strut is a chain of hole indices; the plate is the
// strip along every strut. The L plate has three: the piriform arm, the buttress arm and the bar that joins
// their lowest holes on the segment.
struct PlateDesign
{
    QString name;
    PlateSide side = PlateSide::Right;
    PlateTemplate kind = PlateTemplate::Paranasal;
    std::vector<PlateHole> holes;
    std::vector<std::vector<int>> struts;
};

struct PlateParams
{
    double thicknessMm = 1.0;            // titanium plate, selective laser melting
    double widthMm = 4.5;                // strip between the holes
    double ringDiameterMm = 5.6;         // material around each hole
    double holeDiameterMm = 2.1;         // 2.0 mm screws
    double countersinkDiameterMm = 3.6;  // screw head seat on the outer face
    double countersinkDepthMm = 0.5;
    double clearanceMm = 0.0;            // plate on bone
    double edgeRoundMm = 0.4;            // rim rounding (smooth intersection)
    double smallestDetailMm = 0.12;      // grid spacing of the plate
    int smoothingIterations = 30;
    double minCutDistanceMm = 4.0;       // a hole may not go closer than this to the osteotomy
    double minEdgeDistanceMm = 1.0;      // bone left all round the screw's ring, so it is not on a margin
    double cutEdgeMarginMm = 2.0;        // free plate edge to the osteotomy on both bones
    int minScrewsPerBone = 2;            // warn when a plate holds a bone with fewer screws
};

// Where a hole goes on the patient before the cut, and what was found checking it.
struct PredictiveHole
{
    int plate = -1;
    int hole = -1;
    PlateBone bone = PlateBone::Unknown;
    std::array<double, 3> plannedCenter{0.0, 0.0, 0.0};
    std::array<double, 3> plannedAxis{0.0, 0.0, 1.0};
    std::array<double, 3> preopCenter{0.0, 0.0, 0.0}; // where the guide's sleeve goes
    std::array<double, 3> preopAxis{0.0, 0.0, 1.0};
    double cutDistanceMm = 0.0; // to the planned osteotomy, measured before the cut; -1 without a path
    bool wrongSide = false;     // on the segment by distance, but on the cranial side of the cut (or the reverse)
};

// A drill sleeve on the guide, built as two figures: a solid cylinder added and its bore subtracted.
struct SleeveParams
{
    double screwDiameterMm = 2.0; // the screw the surgeon chose (user's request, 2026-10-06)
    double boreDiameterMm = 1.6;  // its pilot drill (`PlateCore::PilotDrillFor`), or a metal sleeve's outer diameter
    double outerDiameterMm = 4.2;
    double heightMm = 4.0;        // above the bone surface: longer sleeves steer the drill better
};

struct PlateCheck
{
    QStringList warnings; // in Spanish, for the report
    bool Ok() const { return warnings.isEmpty(); }
};

struct PlateBuildResult
{
    bool ok = false;
    QString error;
    QString report;
    vtkSmartPointer<vtkPolyData> mesh;
    int pieces = 0;
    double maxFitGapMm = 0.0; // largest gap under a hole between the plate and the real bone
    double bridgedMm = 0.0;   // gap bridged at the cut, summed over the arms
    int steppedBridges = 0;   // arms that ramp across the step the movement opened at the cut
};

// Where a plate may not go. The two fields answer two different questions and are built differently.
struct PlateKeepOut
{
    // The bone MATERIAL, from `ImplicitCore::BakeMeshField` of the planned meshes themselves: exact on the
    // surface, and "inside" means inside the wall rather than inside the sinus. No part of a plate may enter
    // it. The wrap the plate is laid on is quantised to voxel centres and can stand a voxel inside the bone,
    // which is what put the plate half a millimetre into the maxilla.
    std::shared_ptr<const ImplicitCore::BakedField> bone;
    // A CLOSED envelope (a wrap with the planning gap closing) of the same two bones plus the Le Fort segment
    // in its PRE-OPERATIVE position, so their union also fills the space the movement vacated: the osteotomy
    // gap. It has to be closed, not the bone's own field: segmented maxilla is a perforated shell round an
    // open sinus, and an arm routed against the bare shell walks straight through the sinus. An arm crossing the cut is pulled taut over this, which is
    // how a real patient-specific implant is shaped — it ramps across the step the advancement makes, instead
    // of falling into the cut and standing in the way of the maxilla (user's report, 2026-09-20).
    std::shared_ptr<const ImplicitCore::BakedField> boneAndGap;
};

// Whether a screw may go where it was clicked, and why not.
struct HoleSeat
{
    bool ok = false;
    QString reason; // in Spanish, ready to show
    double cutDistanceMm = -1.0;
};

// Which planned bone is nearest a point, and how far it is: lets each plate arm follow its own bone and tells
// where it has to bridge. Without it the arms still bridge where the surface turns sharply or falls away.
using PlateBoneQuery = std::function<PlateBone(const std::array<double, 3>&, double* distanceMm)>;

namespace PlateCore
{
// ── The planned motion of the segment ───────────────────────────────────────
// Rigid matrix (row-major, before → after) that carries `before` onto `after`, from their vertices taken in
// correspondence. Fails when the two do not have the same vertices or the fit leaves more than
// `toleranceMm` of residual (the mesh was re-cut or deformed, not moved).
bool RigidMotion(vtkPolyData* before, vtkPolyData* after, std::array<double, 16>& matrix, double* rmsMm = nullptr,
                 QString* error = nullptr, double toleranceMm = 0.05);
std::array<double, 16> Invert(const std::array<double, 16>& matrix);
std::array<double, 3> TransformPoint(const std::array<double, 16>& matrix, const std::array<double, 3>& point);
std::array<double, 3> TransformVector(const std::array<double, 16>& matrix, const std::array<double, 3>& vector);

// ── Placing holes ───────────────────────────────────────────────────────────
// The bone under each hole, by the nearest surface among the planned cranial base and the planned segment.
void AssignBones(std::vector<PlateDesign>& plates, vtkPolyData* cranialPlanned, vtkPolyData* segmentPlanned);
// The struts a template joins the holes with, from how many holes each arm has (clicked top to bottom).
std::vector<std::vector<int>> TemplateStruts(PlateTemplate kind, int firstArmHoles, int secondArmHoles);
// Four groups in clinical marking order: nasomaxillary right, zygomaticomaxillary right,
// nasomaxillary left, zygomaticomaxillary left. Each group is top-to-bottom.
std::vector<std::vector<int>> SplintlessStruts(const std::array<int, 4>& pillarHoles);

// ── Predictive holes ─────────────────────────────────────────────────────────
// Every hole carried to its pre-operative position (`segmentMotion` is pre-op → planned), with its distance
// to the osteotomy measured there. `path` is the planned cut in pre-operative coordinates; `segmentProbe` is
// any point of the segment before the cut (its centroid), which tells which side of the path it lies on.
std::vector<PredictiveHole> PredictHoles(const std::vector<PlateDesign>& plates,
                                         const std::array<double, 16>& segmentMotion, const OsteotomyPath& path,
                                         const std::array<double, 3>& segmentProbe);
// What a surgeon would want to hear about before the plates are made.
PlateCheck Check(const std::vector<PlateDesign>& plates, const std::vector<PredictiveHole>& holes,
                 const PlateParams& params = {});
// One sleeve per predictive hole, as guide figures (added body, subtracted bore) at the pre-operative
// position, so `GuideDesignCore::Build` carves them into the guide with everything else.
std::vector<GuideFigure> SleeveFigures(const std::vector<PredictiveHole>& holes, const SleeveParams& params = {});

// The bone a plate can seat on, as the arms walk it: the nearest of the two planned bones — but not within
// `cutMarginMm` of the osteotomy, nor on the wrong side of it. Near the cut the surface turns into the cut face,
// and an arm that followed it would go down into the gap and meet the other bone there; stopping short keeps
// the plate on the anterior faces and the bridge in front of the gap. The cut is `path` in pre-operative
// coordinates, so a point on the segment is taken back through the motion first. Without a path, nearest only.
PlateBoneQuery MakeBoneQuery(vtkPolyData* cranialPlanned, vtkPolyData* segmentPlanned,
                             const std::array<double, 16>& segmentMotion, const OsteotomyPath& path,
                             double cutMarginMm = 1.5);

// ── Where a screw may go ─────────────────────────────────────────────────────
// The outward normal of the bone at `point`, averaged over about a millimetre. This is the screw's axis, the
// drill's vector and the axis of the guide's sleeve, so it has to be the bone's own normal: the envelope the
// plate is laid on is quantised to voxel centres and its gradient swings tens of degrees on a flat wall, which
// left sleeves visibly tilted on the guide (user's report, 2026-09-21: "mal orientada"). `bone` is
// `ImplicitCore::BakeMeshField` of the meshes themselves.
std::array<double, 3> BoneNormalAt(const ImplicitCore::BakedField& bone, const std::array<double, 3>& point,
                                   const std::array<double, 3>& hint);

// A screw needs bone all round its ring and a margin to the osteotomy; one at a bony margin has nothing to
// hold it, and one at the cut sits in the few tenths of cortex the saw is about to take (user's rule,
// 2026-09-20: "no dejes que los orificios se coloquen en la orilla").
//   · `plannedBone` says which bone was clicked, on the planned anatomy the user is looking at;
//   · `preopBone` is the SAME bones before the movement, and the ring is judged on it, because the hole is
//     drilled through the guide before the cut is made. Judging it on the planned anatomy made the osteotomy
//     itself read as a free margin, and no screw could be placed near the cut at all.
// Both must be queries without a cut margin (`MakeBoneQuery` with an invalid path): the distance to the cut is
// measured separately. `segmentMotion` is pre-op → planned, and `path` the cut in pre-operative coordinates.
HoleSeat CheckHoleSeat(const std::array<double, 3>& center, const std::array<double, 3>& axis,
                       const PlateBoneQuery& plannedBone, const PlateBoneQuery& preopBone,
                       const OsteotomyPath& path, const std::array<double, 16>& segmentMotion,
                       const PlateParams& params = {});

// ── The plate ────────────────────────────────────────────────────────────────
// `planned` is the wrap of the bone in its planned position (`GuideDesignCore::Prepare` on the cranial base
// and the moved segment, with a small gap closing so it follows the bone). `boneAt`, when given, names the
// real bone under a point: the arms follow it and bridge between bones, and the gap under each hole is reported.
// `keepOut` carries the real bone and the gap; without it an arm crossing the osteotomy has nothing to ramp
// over and runs straight, which is only right when the movement opened no step. The planning wrap cannot do
// either job — it fills the corner of the step at the cut.
PlateBuildResult Build(const GuidePreparation& planned, const PlateDesign& plate, const PlateParams& params = {},
                       const PlateBoneQuery& boneAt = {}, const std::atomic<bool>* cancel = nullptr,
                       const PlateKeepOut& keepOut = {});

// ── Persistence ─────────────────────────────────────────────────────────────
QJsonObject ToJson(const PlateDesign& plate);
PlateDesign FromJson(const QJsonObject& object);
QJsonObject ParamsToJson(const PlateParams& params);
// The pilot drill for a screw: 1.5 → 1.1, 1.7 → 1.3, 2.0 → 1.6, 2.3 → 1.8, 2.5 → 2.0, 2.7 → 2.0 mm, linear
// between them and 0.4 mm under the screw outside the table.
double PilotDrillFor(double screwDiameterMm);
PlateParams ParamsFromJson(const QJsonObject& object);
}
