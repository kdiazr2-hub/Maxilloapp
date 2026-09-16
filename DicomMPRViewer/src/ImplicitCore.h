#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// ImplicitCore
//
// Signed distance kernel (negative inside) shared by the surgical guides and,
// in time, by every "solid" tool: wrap, hollow, offset, booleans, base plates,
// cut slots and tubes are all arithmetic on one field:
//
//   Union = min(a, b) · Subtract = max(a, -b) · Intersect = max(a, b)
//   Offset = f - t    · Hollow   = max(f - t, -f)
//
// A node tree is evaluated once into a grid and polygonised once, so the result
// is a single closed surface instead of a chain of mesh booleans (which fail on
// anatomy). Meshes enter the tree as fields baked to a grid (exact squared EDT)
// and read back trilinearly: never query a mesh locator per voxel.
//
// The grid resamples everything: the input triangulation is lost and sharp
// edges round off at voxel scale (irrelevant for guides at 0.15-0.2 mm).
// No Qt Widgets.
// ─────────────────────────────────────────────────────────────────────────────

#include <QString>
#include <vtkSmartPointer.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

class vtkImageData;
class vtkMatrix4x4;
class vtkPolyData;

namespace ImplicitCore
{
using Vec3 = std::array<double, 3>;
using UV = std::array<double, 2>;

enum class NodeType
{
    Sphere,
    Box,
    RoundedBox,
    Revolved,  // profile polygon in (radial, axial): cylinder, cone, frustum
    Torus,
    Capsule,
    HalfSpace,
    Prism,     // polygon swept along an axis
    MeshField, // baked signed distance grid
    Union,
    Intersect,
    Subtract,
    Negate,
    Offset,
    Transform
};

// Signed distance grid baked from a mesh, read back trilinearly.
struct BakedField
{
    std::vector<float> values;
    std::array<int, 3> dims{0, 0, 0};
    Vec3 origin{0.0, 0.0, 0.0};
    double spacingMm = 1.0;
    double At(const Vec3& p) const;
};

struct ImplicitNode;
using NodePtr = std::shared_ptr<const ImplicitNode>;

struct ImplicitNode
{
    NodeType type = NodeType::Sphere;
    std::vector<NodePtr> children;
    Vec3 center{0.0, 0.0, 0.0};   // sphere/box/revolved/torus/prism centre, half-space origin, capsule end A
    Vec3 axis{0.0, 0.0, 1.0};     // revolved/torus/prism axis, half-space normal, capsule end B
    Vec3 halfSize{1.0, 1.0, 1.0}; // box
    double radius = 1.0;          // sphere/torus major/capsule radius
    double minorRadius = 0.0;     // torus minor, rounded-box corner
    double halfHeight = 1.0;      // prism half length along the axis; <= 0 is infinite
    double value = 0.0;           // offset distance
    std::vector<UV> profile;      // revolved: (radial, axial); prism: polygon in the axis frame
    std::shared_ptr<const BakedField> field;
    vtkSmartPointer<vtkMatrix4x4> worldToChild; // transform nodes
};

// ── Primitives (negative inside) ──────────────────────────────────────────────
NodePtr Sphere(const Vec3& center, double radius);
NodePtr Box(const Vec3& center, const Vec3& halfSize);
NodePtr RoundedBox(const Vec3& center, const Vec3& halfSize, double cornerRadius);
NodePtr Cylinder(const Vec3& center, const Vec3& axis, double radius, double halfHeight);
// Frustum: radius at the -axis end and at the +axis end (a cone has one of them 0).
NodePtr Cone(const Vec3& center, const Vec3& axis, double radiusBase, double radiusTop, double halfHeight);
NodePtr Torus(const Vec3& center, const Vec3& axis, double majorRadius, double minorRadius);
NodePtr Capsule(const Vec3& a, const Vec3& b, double radius);
// Negative on the side the normal points away from.
NodePtr HalfSpace(const Vec3& origin, const Vec3& normal);
// Polygon (world points, projected along the axis through their centroid) swept along the axis.
NodePtr Prism(const std::vector<Vec3>& polygon, const Vec3& axis, double halfHeight);

// ── Operators ─────────────────────────────────────────────────────────────────
NodePtr Union(const std::vector<NodePtr>& nodes);
NodePtr Union(const NodePtr& a, const NodePtr& b);
NodePtr Intersect(const std::vector<NodePtr>& nodes);
NodePtr Intersect(const NodePtr& a, const NodePtr& b);
NodePtr Subtract(const NodePtr& from, const NodePtr& tool);
NodePtr Negate(const NodePtr& node);
// Grows the solid by t (t < 0 shrinks it).
NodePtr Offset(const NodePtr& node, double t);
// Shell of thickness t inside the surface.
NodePtr Hollow(const NodePtr& node, double thicknessMm);
// Layer of the field between two distances, e.g. a base plate on a wrap:
// clearance <= d <= clearance + thickness.
NodePtr Layer(const NodePtr& node, double fromMm, double toMm);
NodePtr Transformed(const NodePtr& node, vtkMatrix4x4* childToWorld);

// ── Mesh as a field ───────────────────────────────────────────────────────────
// Bakes the mesh once: triangles rasterised, inside flood-filled from outside,
// exact squared EDT both ways. `paddingMm` grows the grid around the mesh, so
// distances stay meaningful that far out; beyond it they keep growing but are
// only approximate.
std::shared_ptr<const BakedField> BakeMeshField(vtkPolyData* mesh, double spacingMm, double paddingMm,
                                                const std::atomic<bool>* cancel = nullptr, QString* error = nullptr);
NodePtr Field(const std::shared_ptr<const BakedField>& field);
NodePtr MeshField(vtkPolyData* mesh, double spacingMm, double paddingMm,
                  const std::atomic<bool>* cancel = nullptr, QString* error = nullptr);

// ── Voxel grid steps ──────────────────────────────────────────────────────────
// The stages a mesh goes through on its way to a field, exposed so the wrap can insert its own
// morphology between them: rasterise the shells, close gaps, fill the interior, measure distances.
struct VoxelMask
{
    std::vector<std::uint8_t> solid;
    std::array<int, 3> dims{0, 0, 0};
    Vec3 origin{0.0, 0.0, 0.0};
    double spacingMm = 1.0;
    bool Empty() const { return solid.empty(); }
};

// Every triangle sampled at half-voxel steps; the grid covers the meshes plus `paddingMm`.
VoxelMask RasterizeShells(const std::vector<vtkPolyData*>& meshes, double spacingMm, double paddingMm,
                          const std::atomic<bool>* cancel = nullptr, QString* error = nullptr);
// Grows the marked set by a real distance (exact EDT, not a count of voxel steps).
void DilateMask(VoxelMask& mask, double radiusMm);
// Marks everything the border cannot reach without crossing the mask.
void FillInteriorFromOutside(VoxelMask& mask);
// Distance to the boundary voxels of the mask, negative inside.
std::shared_ptr<const BakedField> SignedDistanceField(const VoxelMask& mask);
vtkSmartPointer<vtkImageData> ToImage(const BakedField& field);

// ── Evaluation ────────────────────────────────────────────────────────────────
double Value(const ImplicitNode& node, const Vec3& p);
inline double Value(const NodePtr& node, const Vec3& p) { return node ? Value(*node, p) : 1.0e30; }

// Samples the tree over `bounds` (xmin,xmax,ymin,ymax,zmin,zmax), padded by two
// voxels so a solid touching the bounds still closes. Returns nullptr when
// cancelled or when the node is empty.
vtkSmartPointer<vtkImageData> Evaluate(const NodePtr& node, const double bounds[6], double spacingMm,
                                       const std::atomic<bool>* cancel = nullptr);

struct PolygonizeOptions
{
    double isoValue = 0.0;
    int smoothingIterations = 15; // windowed sinc; 0 skips smoothing
    double passBand = 0.1;
    bool repair = true; // MeshRepairCore::Repair, so the result is closed and outward
};

vtkSmartPointer<vtkPolyData> Polygonize(vtkImageData* field, const PolygonizeOptions& options = {});

struct BuildResult
{
    bool ok = false;
    QString error;
    vtkSmartPointer<vtkPolyData> mesh;
    vtkSmartPointer<vtkImageData> field;
    double spacingMm = 0.0;
};

// Evaluate + Polygonize over the node's own bounds when `bounds` is null.
BuildResult Build(const NodePtr& node, const double bounds[6], double spacingMm,
                  const PolygonizeOptions& options = {}, const std::atomic<bool>* cancel = nullptr);

// Conservative bounds of the solid (primitives are exact; offsets grow them).
bool Bounds(const NodePtr& node, double bounds[6]);
}
