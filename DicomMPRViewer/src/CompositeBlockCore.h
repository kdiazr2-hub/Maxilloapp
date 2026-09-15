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
                               double thicknessMm = DefaultThicknessMm, QString* error = nullptr);
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
// CT bone is only replaced within this distance of the kept scan (crowns, skirt): bone beside or behind the
// teeth inside the region (external oblique ridge, ramus) stays when no scan covers it.
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
VoxelUnionResult VoxelUnion(const std::vector<vtkPolyData*>& meshes, double spacingMm = 0.3,
                            int closingVoxels = 3, const std::atomic<bool>* cancel = nullptr);

QJsonObject BlockToJson(const CompositeCutBlock& block);
CompositeCutBlock BlockFromJson(const QJsonObject& object);
}
