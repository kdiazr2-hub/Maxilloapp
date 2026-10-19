#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// CompositeBlockCore
//
// ProPlan-style composite model: an oriented cutting block replaces the CT
// dentition by the registered intraoral scan. Bone outside the block and scan
// inside it are merged into one mesh whose cells carry a "CompositePart" array
// (0 = bone, 1 = dental scan). Transforms, plane clips, cleaning and normals
// keep cell data, so the dental part stays linked to its bone through
// orientation, osteotomies and repositioning and can be extracted at any time.
// No Qt Widgets.
// ─────────────────────────────────────────────────────────────────────────────

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <vtkSmartPointer.h>

#include <array>
#include <atomic>
#include <vector>

class vtkMatrix4x4;
class vtkPolyData;

struct CompositeCutBlock
{
    std::array<double, 3> center{0.0, 0.0, 0.0};
    std::array<double, 3> axisX{1.0, 0.0, 0.0}; // along the arch width
    std::array<double, 3> axisY{0.0, 1.0, 0.0}; // antero-posterior
    std::array<double, 3> axisZ{0.0, 0.0, 1.0}; // occlusal normal, toward the bone
    std::array<double, 3> sizeMm{60.0, 50.0, 15.0}; // width, length, thickness
    bool valid = false;
};

// Jaw of a block: its normal (axisZ) points from the teeth to that jaw's bone, superior (+Z) for the upper
// jaw and inferior for the lower one. Auto guesses from the bone centroid, which the mandibular rami and
// condyles can pull above the lower teeth.
enum class CompositeJaw
{
    Auto,
    Upper,
    Lower
};

struct CompositeBlockResult
{
    bool ok = false;
    QString error;
    QString report;
    vtkSmartPointer<vtkPolyData> composite; // tagged with CompositePart
    vtkIdType boneCells = 0;
    vtkIdType dentalCells = 0;
};

struct VoxelUnionResult
{
    bool ok = false;
    QString error;
    QString report;
    vtkSmartPointer<vtkPolyData> mesh;
    double spacingMm = 0.0;
};

namespace CompositeBlockCore
{
inline constexpr const char* PartArrayName = "CompositePart";
inline constexpr unsigned char BonePart = 0;
inline constexpr unsigned char DentalPart = 1;
inline constexpr double DefaultThicknessMm = 15.0;

// Block on the occlusal plane of the registered scan, covering all of it from
// just past the cusps toward the bone.
CompositeCutBlock InitialBlock(vtkPolyData* dentalScan, vtkPolyData* bone,
                               double thicknessMm = DefaultThicknessMm, QString* error = nullptr,
                               CompositeJaw jaw = CompositeJaw::Auto);
// Whether the block normal points to that jaw's bone (always true for Auto).
bool BlockFacesJaw(const CompositeCutBlock& block, CompositeJaw jaw);
vtkSmartPointer<vtkMatrix4x4> BlockLocalToWorld(const CompositeCutBlock& block);
vtkSmartPointer<vtkPolyData> BlockMesh(const CompositeCutBlock& block);
// Applies an affine matrix (e.g. a gizmo result) and re-orthonormalises the axes.
CompositeCutBlock TransformBlock(const CompositeCutBlock& block, vtkMatrix4x4* matrix);
CompositeCutBlock WithSize(const CompositeCutBlock& block, double widthMm, double lengthMm, double thicknessMm);
bool Contains(const CompositeCutBlock& block, const double point[3], double toleranceMm = 0.0);

CompositeBlockResult CreateBlockComposite(vtkPolyData* bone, vtkPolyData* dentalScan, const CompositeCutBlock& block);

// ── Contour drawn with points around the scan (instead of the rectangle) ──
// The polygon is the points, in placement order, projected on the block's
// X–Y plane; each point also keeps its height along axisZ.
using CompositeContour = std::vector<std::array<double, 3>>;
inline constexpr int MinContourPoints = 3;
// CT bone below the line of the points is only replaced within this distance of the kept scan.
// Bone past that line stays (external oblique ridge, ramus, palate). The scan's open border is
// blended onto it: upward on the maxilla, downward on the mandible.
inline constexpr double ContourNearScanMm = 3.0;
bool ContourValid(const CompositeCutBlock& block, const CompositeContour& contour, QString* error = nullptr);
// Points on the scan border (buccal and palatal gingiva, any order): everything below the line of the
// points (heights interpolated between them) within their convex outline, widened for the crown bulges,
// is taken from the scan; the CT bone there is replaced up to the line. Mucosa and palate above stay out.
// Bone replacement region: negative inside.
double ContourField(const CompositeCutBlock& block, const CompositeContour& contour, const double point[3]);
// Scan region kept: negative inside.
double ContourScanField(const CompositeCutBlock& block, const CompositeContour& contour, const double point[3]);
// Translucent wall around the outline, from the block's occlusal face up to the points (display).
vtkSmartPointer<vtkPolyData> ContourWallMesh(const CompositeCutBlock& block, const CompositeContour& contour);
// Closed polyline around the outline of the points (display).
vtkSmartPointer<vtkPolyData> ContourPolyline(const CompositeCutBlock& block, const CompositeContour& contour);
CompositeBlockResult CreateContourComposite(vtkPolyData* bone, vtkPolyData* dentalScan, const CompositeCutBlock& block,
                                            const CompositeContour& contour);
// No points: keeps the crowns plus a short band of gingiva (measured from the local occlusal surface toward
// the bone) and blends that border onto the jaw. Mucosa farther up the vestibule stays out.
inline constexpr double ToothAndGingivaMm = 12.0;
CompositeBlockResult CreateGingivalComposite(vtkPolyData* bone, vtkPolyData* dentalScan, const CompositeCutBlock& block);
// Three points — posterior right, posterior left and anterior — sit on the facial gingiva. The red
// band is the circle through them, just labial to the clicks and a little further forward at the
// incisors, continued past the molars. On its occlusal side, and only across that arch, the CT
// teeth are removed and the scan replaces them, overlapping the line by a couple of millimetres.
// The rami and the bone on the jaw side of the line stay.
vtkSmartPointer<vtkPolyData> ArchRibbon(vtkPolyData* bone, const CompositeCutBlock& block,
                                       const CompositeContour& points);
CompositeBlockResult CreateArchComposite(vtkPolyData* bone, vtkPolyData* dentalScan, const CompositeCutBlock& block,
                                        const CompositeContour& points);
// Boxed cast of an open intraoral scan, before the composite match. The cut is a plane
// normal to the jaw axis (upper +Z toward the maxilla, lower −Z toward the mandible),
// ToothAndGingivaMm past the cusps, so the teeth and a little gingiva stay and the ragged
// rim and anything past that plane are dropped. A narrow opening is capped on the rim.
// The palatal vault and the floor of the mouth are not covered. The input mesh is not modified.
vtkSmartPointer<vtkPolyData> BoxScan(vtkPolyData* scan, CompositeJaw jaw, QString* error = nullptr);
QJsonArray ContourToJson(const CompositeContour& contour);
CompositeContour ContourFromJson(const QJsonArray& array);

bool HasParts(vtkPolyData* mesh);
vtkSmartPointer<vtkPolyData> TagPart(vtkPolyData* mesh, unsigned char part);
// Cells of one part (keeps the tag); nullptr when the mesh has no parts or no such cells.
vtkSmartPointer<vtkPolyData> ExtractPart(vtkPolyData* mesh, unsigned char part);
// Copy with per-cell RGB scalars for display.
vtkSmartPointer<vtkPolyData> PartColoredCopy(vtkPolyData* mesh, const std::array<unsigned char, 3>& boneColor,
                                             const std::array<unsigned char, 3>& dentalColor);

// Intersection polylines of a mesh with the plane `axis` = position.
vtkSmartPointer<vtkPolyData> SliceContour(vtkPolyData* mesh, int axis, double position);

// Single closed mesh from several (possibly open) surfaces: voxelised shells,
// small gaps sealed by closing, interior filled from outside, iso-surface.
// `fillSolids` fills enclosed cavities. The gingival composite passes false: it only
// closes the gap between the scan and the bone, and leaves the sinus and the palate empty.
VoxelUnionResult VoxelUnion(const std::vector<vtkPolyData*>& meshes, double spacingMm = 0.3,
                            int closingVoxels = 3, const std::atomic<bool>* cancel = nullptr,
                            bool fillSolids = true);

QJsonObject BlockToJson(const CompositeCutBlock& block);
CompositeCutBlock BlockFromJson(const QJsonObject& object);
}
