#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// OsteotomyCore
//
// ProPlan-style osteotomy planning geometry (manual 4.10.3). No Qt Widgets.
//
//  • Cutting path ("line segments"): a polyline through the landmarks as seen
//    in the frontal view, extended at both ends and swept along a depth axis.
//    Used by Le Fort I (4 landmarks) and genioplasty (4 landmarks). The cut
//    follows the path exactly (a stepped Le Fort I stays stepped).
//  • BSSO: bilateral sagittal split from 3 landmarks per side (6 in total),
//    cut on both sides at once. The proximal segment of a side is the region
//    posterior to the vertical buccal cut and lateral to the sagittal cut, plus
//    the ramus above the medial horizontal cut.
//
// Every cut is a signed distance-like field (negative = segment side). Meshes
// are split with a kerf of the cut thickness; cell data such as the composite
// "CompositePart" link is kept. Guide meshes are the closed kerf slab of the
// same field inside the guide extents, so preview, slice contour and cut match.
// ─────────────────────────────────────────────────────────────────────────────

#include <QJsonObject>
#include <QString>
#include <vtkSmartPointer.h>
#include <vtkType.h>

#include <array>
#include <memory>
#include <vector>

class vtkMatrix4x4;
class vtkPolyData;

using OstPoint3 = std::array<double, 3>;

enum class OsteotomyType
{
    LeFortI = 0,
    Bsso = 1,
    Genioplasty = 2
};

struct OsteotomyLandmark
{
    QString name;
    QString hint;
};

// Line-segment cutting path.
struct OsteotomyPath
{
    std::vector<OstPoint3> points;   // any order; sorted right → left along the path
    OstPoint3 depthAxis{0.0, 1.0, 0.0}; // sweep direction (antero-posterior)
    OstPoint3 upAxis{0.0, 0.0, 1.0};    // side kept by the cranial / body piece
    double widthMm = 120.0;           // guide extent along depthAxis
    double thicknessMm = 1.0;         // kerf
    double extensionStartMm = 20.0;   // first (right) end
    double extensionEndMm = 20.0;     // last (left) end
    bool valid = false;
    QString error;
};

struct BssoSideLandmarks
{
    OstPoint3 ramus{};   // medial ramus above the lingula (horizontal cut)
    OstPoint3 oblique{}; // external oblique ridge behind the last molar (top of the sagittal cut)
    OstPoint3 body{};    // buccal cortex at the vertical cut (bottom of the sagittal cut)
};

struct BssoPlan
{
    BssoSideLandmarks right;
    BssoSideLandmarks left;
    OstPoint3 upAxis{0.0, 0.0, 1.0};
    double thicknessMm = 1.0;
    // Guide extents only (the cut always separates the bone).
    double posteriorExtensionMm = 20.0;
    double inferiorExtensionMm = 30.0;
    double mediolateralExtensionMm = 15.0;
};

struct OsteotomyPlane
{
    OstPoint3 origin{};
    OstPoint3 normal{0.0, 0.0, 1.0};
};

struct BssoSidePlanes
{
    OsteotomyPlane horizontal; // normal down: above is negative
    OsteotomyPlane sagittal;   // normal medial: lateral is negative
    OsteotomyPlane vertical;   // normal anterior, through the body point: posterior is negative
    OsteotomyPlane ramusFront; // normal anterior, through the oblique point
    OsteotomyPlane midline;    // normal toward the other side: this side is negative
    OstPoint3 anterior{0.0, 1.0, 0.0};
    OstPoint3 medial{1.0, 0.0, 0.0};
    bool valid = false;
    QString error;
};

struct OsteotomySplitResult
{
    bool ok = false;
    QString error;
    vtkSmartPointer<vtkPolyData> negative; // segment side
    vtkSmartPointer<vtkPolyData> positive; // remaining side
};

// Segment position at cut time, to measure its later movement at the landmarks
// (impaction / advancement). Point order survives rigid transforms.
struct SegmentReference
{
    std::vector<vtkIdType> ids;      // sampled mesh vertices
    std::vector<OstPoint3> points;   // their positions at cut time
    std::vector<OstPoint3> landmarks;
};

struct LandmarkMovement
{
    bool valid = false;
    double rmsMm = 0.0;                   // rigid fit residual
    std::vector<OstPoint3> displacements; // current − cut position, per landmark
};

struct BssoSplitResult
{
    bool ok = false;
    QString error;
    vtkSmartPointer<vtkPolyData> proximalRight;
    vtkSmartPointer<vtkPolyData> proximalLeft;
    vtkSmartPointer<vtkPolyData> distal;
};

namespace OsteotomyCore
{
inline constexpr const char* FieldArrayName = "OsteotomyField";

// ProPlan CMF defaults (Properties.User.xml): LeFort W120 T1 Ext 20/20,
// Geo W50 T1 Ext 20/20.
inline constexpr double LeFortWidthMm = 120.0;
inline constexpr double LeFortExtensionMm = 20.0;
inline constexpr double GenioWidthMm = 50.0;
inline constexpr double GenioExtensionMm = 20.0;
inline constexpr double DefaultThicknessMm = 1.0;

QString TypeName(OsteotomyType type);
// Le Fort I: piriform R, piriform L, zygomatic buttress R, buttress L.
// BSSO: ramus R, oblique R, body R, ramus L, oblique L, body L.
// Genioplasty: basal exit R, under canine R, under canine L, basal exit L.
// Le Fort I and genioplasty paths are drawn in the frontal view (+Z superior) and swept antero-posteriorly.
std::vector<OsteotomyLandmark> Landmarks(OsteotomyType type);

OsteotomyPath LeFortPath(const std::array<OstPoint3, 4>& landmarks, double widthMm = LeFortWidthMm,
                         double thicknessMm = DefaultThicknessMm, double extensionRightMm = LeFortExtensionMm,
                         double extensionLeftMm = LeFortExtensionMm);
OsteotomyPath GenioPath(const std::array<OstPoint3, 4>& landmarks, double widthMm = GenioWidthMm,
                        double thicknessMm = DefaultThicknessMm, double extensionRightMm = GenioExtensionMm,
                        double extensionLeftMm = GenioExtensionMm);
// A path as JSON, so plans that reference a cut (the surgical guides) can be saved and reloaded.
QJsonObject PathToJson(const OsteotomyPath& path);
OsteotomyPath PathFromJson(const QJsonObject& object);
// Validates and completes a path (valid / error).
OsteotomyPath CheckedPath(OsteotomyPath path);
// Rigid (or scaled) gizmo transform: points move, axes rotate, width scales.
OsteotomyPath TransformPath(const OsteotomyPath& path, vtkMatrix4x4* matrix);
BssoPlan TransformBsso(const BssoPlan& plan, vtkMatrix4x4* matrix, bool rightSide = true, bool leftSide = true);

// Signed field (negative = caudal / segment side). Slow per call (it rebuilds the frame); for tests.
double PathField(const OsteotomyPath& path, const OstPoint3& point);
// The same field with the frame built once, for sweeping a grid (the guide slots): prepare it and
// call FieldAt per point. Returns nullptr on an invalid path.
struct PreparedPathField;
std::shared_ptr<const PreparedPathField> PreparePathField(const OsteotomyPath& path, QString* error = nullptr);
double FieldAt(const PreparedPathField& prepared, const OstPoint3& point);
BssoSidePlanes BssoPlanes(const BssoPlan& plan, bool leftSide);
double BssoField(const BssoSidePlanes& planes, const OstPoint3& point);

OsteotomySplitResult SplitByPath(vtkPolyData* mesh, const OsteotomyPath& path);
OsteotomySplitResult SplitBssoSide(vtkPolyData* mesh, const BssoPlan& plan, bool leftSide);
BssoSplitResult SplitBsso(vtkPolyData* mesh, const BssoPlan& plan);

// Closed kerf slabs inside the guide extents, in world coordinates.
vtkSmartPointer<vtkPolyData> PathGuideMesh(const OsteotomyPath& path);
vtkSmartPointer<vtkPolyData> BssoGuideMesh(const BssoPlan& plan, bool leftSide);

SegmentReference CaptureSegmentReference(vtkPolyData* mesh, const std::vector<OstPoint3>& landmarks, int samples = 64);
// Invalid when the mesh no longer matches the reference (point order changed or non-rigid).
LandmarkMovement MeasureLandmarkMovement(const SegmentReference& reference, vtkPolyData* current, double maxRmsMm = 0.5);
}
