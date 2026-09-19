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
// advancement separates the cut edges, it reaches the cranial edge, bends
// outward by the advancement and then turns onto the repositioned Le Fort —
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
    LShape     // conventional: piriform strut + zygomaticomaxillary buttress strut, joined below the cut
};

enum class PlateSide
{
    Right,
    Left
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
    double minCutDistanceMm = 4.0;       // warn when a hole is closer than this to the osteotomy
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
    double boreDiameterMm = 1.6;  // pilot drill for 2.0 mm screws (or a metal sleeve's outer diameter)
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
    int steppedBridges = 0;   // arms bent outward at the cranial edge before entering the moved segment
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

// ── The plate ────────────────────────────────────────────────────────────────
// `planned` is the wrap of the bone in its planned position (`GuideDesignCore::Prepare` on the cranial base
// and the moved segment, with a small gap closing so it follows the bone). `boneAt`, when given, names the
// real bone under a point: the arms follow it and bridge between bones, and the gap under each hole is reported.
PlateBuildResult Build(const GuidePreparation& planned, const PlateDesign& plate, const PlateParams& params = {},
                       const PlateBoneQuery& boneAt = {}, const std::atomic<bool>* cancel = nullptr);

// ── Persistence ─────────────────────────────────────────────────────────────
QJsonObject ToJson(const PlateDesign& plate);
PlateDesign FromJson(const QJsonObject& object);
QJsonObject ParamsToJson(const PlateParams& params);
PlateParams ParamsFromJson(const QJsonObject& object);
}
