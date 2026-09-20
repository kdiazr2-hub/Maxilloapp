#include "ImplicitCore.h"

#include "MeshRepairCore.h"

#include <vtkCellArray.h>
#include <vtkCellArrayIterator.h>
#include <vtkFloatArray.h>
#include <vtkFlyingEdges3D.h>
#include <vtkImageData.h>
#include <vtkMath.h>
#include <vtkMatrix4x4.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>
#include <vtkReverseSense.h>
#include <vtkTriangleFilter.h>
#include <vtkWindowedSincPolyDataFilter.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <thread>
#include <vector>

namespace
{
using ImplicitCore::BakedField;
using ImplicitCore::ImplicitNode;
using ImplicitCore::NodePtr;
using ImplicitCore::NodeType;
using ImplicitCore::UV;
using ImplicitCore::Vec3;

constexpr double kInf = 1.0e30;
constexpr double kMaxVoxels = 40.0e6;

Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 mul(const Vec3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double length(const Vec3& v) { return std::sqrt(dot(v, v)); }
Vec3 normalized(const Vec3& v, const Vec3& fallback = {0.0, 0.0, 1.0})
{
    const double len = length(v);
    return len > 1e-9 ? mul(v, 1.0 / len) : fallback;
}

// Same shape as SplintHeightmapGenerator's: slices of the grid over the hardware threads.
void parallelFor(int count, const std::function<void(int, int)>& body)
{
    if (count <= 0)
        return;
    const int hardware = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    const int threads = std::min(hardware, std::max(1, count / 2));
    if (threads <= 1) {
        body(0, count);
        return;
    }
    const int chunk = (count + threads - 1) / threads;
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(threads));
    for (int t = 0; t < threads; ++t) {
        const int begin = t * chunk;
        const int end = std::min(count, begin + chunk);
        if (begin < end)
            pool.emplace_back(body, begin, end);
    }
    for (auto& thread : pool)
        thread.join();
}

// Orthonormal frame of an axis, chosen the same way wherever that axis is used.
void axisFrame(const Vec3& axis, Vec3& u, Vec3& v, Vec3& n)
{
    n = normalized(axis);
    const Vec3 helper = std::abs(n[2]) < 0.9 ? Vec3{0.0, 0.0, 1.0} : Vec3{1.0, 0.0, 0.0};
    u = normalized(cross(helper, n), {1.0, 0.0, 0.0});
    v = cross(n, u);
}

double segmentDistance2D(const UV& a, const UV& b, double x, double y)
{
    const double dx = b[0] - a[0], dy = b[1] - a[1];
    const double len2 = dx * dx + dy * dy;
    double t = len2 > 1e-18 ? ((x - a[0]) * dx + (y - a[1]) * dy) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    return std::hypot(x - (a[0] + t * dx), y - (a[1] + t * dy));
}

bool insidePolygon2D(const std::vector<UV>& polygon, double x, double y)
{
    bool inside = false;
    const size_t n = polygon.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const UV& a = polygon[i];
        const UV& b = polygon[j];
        if ((a[1] > y) != (b[1] > y)) {
            const double xCross = a[0] + (y - a[1]) * (b[0] - a[0]) / (b[1] - a[1]);
            if (x < xCross)
                inside = !inside;
        }
    }
    return inside;
}

// Signed distance to a closed polygon, negative inside. `skipAxisEdges` drops edges lying on
// x = 0: for a solid of revolution the axis is interior, not boundary.
double polygonDistance2D(const std::vector<UV>& polygon, double x, double y, bool skipAxisEdges)
{
    if (polygon.size() < 3)
        return kInf;
    double best = kInf;
    const size_t n = polygon.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        if (skipAxisEdges && std::abs(polygon[i][0]) < 1e-12 && std::abs(polygon[j][0]) < 1e-12)
            continue;
        best = std::min(best, segmentDistance2D(polygon[j], polygon[i], x, y));
    }
    return insidePolygon2D(polygon, x, y) ? -best : best;
}

// Convex 2D cross-section combined with a length along the axis.
double extrude(double planar, double axial)
{
    const double outside = std::hypot(std::max(planar, 0.0), std::max(axial, 0.0));
    return std::min(std::max(planar, axial), 0.0) + outside;
}

NodePtr makeNode(NodeType type)
{
    auto node = std::make_shared<ImplicitNode>();
    node->type = type;
    return node;
}

// Nodes are shared as const so trees can be reused; the factories fill them in on creation.
ImplicitNode& mutableNode(const NodePtr& node) { return const_cast<ImplicitNode&>(*node); }

// ── Exact squared euclidean distance transform (Felzenszwalb & Huttenlocher) ──
void edt1d(std::vector<double>& f, std::vector<double>& d, std::vector<int>& parabola, std::vector<double>& z, int n)
{
    int k = 0;
    parabola[0] = 0;
    z[0] = -kInf;
    z[1] = kInf;
    for (int q = 1; q < n; ++q) {
        double s = 0.0;
        while (true) {
            const int p = parabola[static_cast<size_t>(k)];
            s = ((f[static_cast<size_t>(q)] + static_cast<double>(q) * q) -
                 (f[static_cast<size_t>(p)] + static_cast<double>(p) * p)) /
                (2.0 * q - 2.0 * p);
            if (k > 0 && s <= z[static_cast<size_t>(k)])
                --k;
            else
                break;
        }
        ++k;
        parabola[static_cast<size_t>(k)] = q;
        z[static_cast<size_t>(k)] = s;
        z[static_cast<size_t>(k) + 1] = kInf;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[static_cast<size_t>(k) + 1] < q)
            ++k;
        const int p = parabola[static_cast<size_t>(k)];
        d[static_cast<size_t>(q)] = static_cast<double>(q - p) * (q - p) + f[static_cast<size_t>(p)];
    }
}

// squared[] is 0 on the source set and infinite elsewhere; it becomes the squared distance in voxels.
void squaredDistanceTransform(std::vector<double>& squared, const std::array<int, 3>& dims)
{
    const size_t nx = static_cast<size_t>(dims[0]), ny = static_cast<size_t>(dims[1]), nz = static_cast<size_t>(dims[2]);
    const auto index = [nx, ny](size_t i, size_t j, size_t k) { return i + nx * (j + ny * k); };
    const size_t maxDim = static_cast<size_t>(std::max({dims[0], dims[1], dims[2]}));
    parallelFor(dims[2], [&](int kBegin, int kEnd) {
        std::vector<double> f(maxDim), d(maxDim), z(maxDim + 1);
        std::vector<int> parabola(maxDim);
        for (size_t k = static_cast<size_t>(kBegin); k < static_cast<size_t>(kEnd); ++k) {
            for (size_t j = 0; j < ny; ++j) { // along x
                for (size_t i = 0; i < nx; ++i)
                    f[i] = squared[index(i, j, k)];
                edt1d(f, d, parabola, z, dims[0]);
                for (size_t i = 0; i < nx; ++i)
                    squared[index(i, j, k)] = d[i];
            }
            for (size_t i = 0; i < nx; ++i) { // along y
                for (size_t j = 0; j < ny; ++j)
                    f[j] = squared[index(i, j, k)];
                edt1d(f, d, parabola, z, dims[1]);
                for (size_t j = 0; j < ny; ++j)
                    squared[index(i, j, k)] = d[j];
            }
        }
    });
    parallelFor(dims[0], [&](int iBegin, int iEnd) {
        std::vector<double> f(maxDim), d(maxDim), z(maxDim + 1);
        std::vector<int> parabola(maxDim);
        for (size_t i = static_cast<size_t>(iBegin); i < static_cast<size_t>(iEnd); ++i)
            for (size_t j = 0; j < ny; ++j) { // along z
                for (size_t k = 0; k < nz; ++k)
                    f[k] = squared[index(i, j, k)];
                edt1d(f, d, parabola, z, dims[2]);
                for (size_t k = 0; k < nz; ++k)
                    squared[index(i, j, k)] = d[k];
            }
    });
}

vtkSmartPointer<vtkPolyData> triangulated(vtkPolyData* mesh)
{
    auto filter = vtkSmartPointer<vtkTriangleFilter>::New();
    filter->SetInputData(mesh);
    filter->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(filter->GetOutput());
    return out;
}

bool cancelled(const std::atomic<bool>* cancel) { return cancel && cancel->load(std::memory_order_relaxed); }

void resetBounds(double bounds[6])
{
    for (int a = 0; a < 3; ++a) {
        bounds[2 * a] = std::numeric_limits<double>::max();
        bounds[2 * a + 1] = -std::numeric_limits<double>::max();
    }
}

void growBounds(double bounds[6], const Vec3& p)
{
    for (int a = 0; a < 3; ++a) {
        bounds[2 * a] = std::min(bounds[2 * a], p[static_cast<size_t>(a)]);
        bounds[2 * a + 1] = std::max(bounds[2 * a + 1], p[static_cast<size_t>(a)]);
    }
}

void sphereBounds(double bounds[6], const Vec3& center, double radius)
{
    resetBounds(bounds);
    growBounds(bounds, add(center, {radius, radius, radius}));
    growBounds(bounds, sub(center, {radius, radius, radius}));
}
} // namespace

namespace ImplicitCore
{
double BakedField::At(const Vec3& p) const
{
    if (values.empty())
        return kInf;
    const double gx = std::clamp((p[0] - origin[0]) / spacingMm, 0.0, dims[0] - 1.000001);
    const double gy = std::clamp((p[1] - origin[1]) / spacingMm, 0.0, dims[1] - 1.000001);
    const double gz = std::clamp((p[2] - origin[2]) / spacingMm, 0.0, dims[2] - 1.000001);
    const int i = static_cast<int>(gx), j = static_cast<int>(gy), k = static_cast<int>(gz);
    const double tx = gx - i, ty = gy - j, tz = gz - k;
    const size_t nx = static_cast<size_t>(dims[0]), ny = static_cast<size_t>(dims[1]);
    const auto at = [&](int a, int b, int c) {
        return static_cast<double>(
            values[static_cast<size_t>(a) + nx * (static_cast<size_t>(b) + ny * static_cast<size_t>(c))]);
    };
    const double c00 = at(i, j, k) * (1 - tx) + at(i + 1, j, k) * tx;
    const double c10 = at(i, j + 1, k) * (1 - tx) + at(i + 1, j + 1, k) * tx;
    const double c01 = at(i, j, k + 1) * (1 - tx) + at(i + 1, j, k + 1) * tx;
    const double c11 = at(i, j + 1, k + 1) * (1 - tx) + at(i + 1, j + 1, k + 1) * tx;
    return (c00 * (1 - ty) + c10 * ty) * (1 - tz) + (c01 * (1 - ty) + c11 * ty) * tz;
}

// ── Primitives ────────────────────────────────────────────────────────────────
NodePtr Sphere(const Vec3& center, double radius)
{
    auto node = makeNode(NodeType::Sphere);
    mutableNode(node).center = center;
    mutableNode(node).radius = std::max(0.0, radius);
    return node;
}

NodePtr Box(const Vec3& center, const Vec3& halfSize)
{
    auto node = makeNode(NodeType::Box);
    mutableNode(node).center = center;
    mutableNode(node).halfSize = {std::max(0.0, halfSize[0]), std::max(0.0, halfSize[1]), std::max(0.0, halfSize[2])};
    return node;
}

NodePtr RoundedBox(const Vec3& center, const Vec3& halfSize, double cornerRadius)
{
    auto node = makeNode(NodeType::RoundedBox);
    const double r = std::clamp(cornerRadius, 0.0, std::min({halfSize[0], halfSize[1], halfSize[2]}));
    mutableNode(node).center = center;
    mutableNode(node).halfSize = {halfSize[0] - r, halfSize[1] - r, halfSize[2] - r};
    mutableNode(node).minorRadius = r;
    return node;
}

NodePtr Cone(const Vec3& center, const Vec3& axis, double radiusBase, double radiusTop, double halfHeight)
{
    auto node = makeNode(NodeType::Revolved);
    ImplicitNode& n = mutableNode(node);
    n.center = center;
    n.axis = normalized(axis);
    const double hh = std::max(1e-9, halfHeight);
    const double rb = std::max(0.0, radiusBase);
    const double rt = std::max(0.0, radiusTop);
    // Profile in (radial, axial); the edge on the axis is skipped when measuring.
    n.profile = {{0.0, -hh}, {rb, -hh}, {rt, hh}, {0.0, hh}};
    n.radius = std::max(rb, rt);
    n.halfHeight = hh;
    return node;
}

NodePtr Cylinder(const Vec3& center, const Vec3& axis, double radius, double halfHeight)
{
    return Cone(center, axis, radius, radius, halfHeight);
}

NodePtr Torus(const Vec3& center, const Vec3& axis, double majorRadius, double minorRadius)
{
    auto node = makeNode(NodeType::Torus);
    ImplicitNode& n = mutableNode(node);
    n.center = center;
    n.axis = normalized(axis);
    n.radius = std::max(0.0, majorRadius);
    n.minorRadius = std::max(0.0, minorRadius);
    return node;
}

NodePtr Capsule(const Vec3& a, const Vec3& b, double radius)
{
    auto node = makeNode(NodeType::Capsule);
    ImplicitNode& n = mutableNode(node);
    n.center = a;
    n.axis = b; // the other end, not a direction
    n.radius = std::max(0.0, radius);
    return node;
}

NodePtr HalfSpace(const Vec3& origin, const Vec3& normal)
{
    auto node = makeNode(NodeType::HalfSpace);
    mutableNode(node).center = origin;
    mutableNode(node).axis = normalized(normal);
    return node;
}

NodePtr Prism(const std::vector<Vec3>& polygon, const Vec3& axis, double halfHeight)
{
    auto node = makeNode(NodeType::Prism);
    ImplicitNode& n = mutableNode(node);
    Vec3 u{}, v{}, w{};
    axisFrame(axis, u, v, w);
    n.axis = w;
    n.halfHeight = halfHeight;
    if (polygon.empty())
        return node;
    Vec3 centroid{0.0, 0.0, 0.0};
    for (const Vec3& p : polygon)
        centroid = add(centroid, mul(p, 1.0 / static_cast<double>(polygon.size())));
    n.center = centroid;
    double radius = 0.0;
    for (const Vec3& p : polygon) {
        const Vec3 d = sub(p, centroid);
        const UV uv{dot(d, u), dot(d, v)};
        n.profile.push_back(uv);
        radius = std::max(radius, std::hypot(uv[0], uv[1]));
    }
    n.radius = radius;
    return node;
}

// ── Operators ─────────────────────────────────────────────────────────────────
namespace
{
NodePtr combine(NodeType type, const std::vector<NodePtr>& nodes)
{
    std::vector<NodePtr> kept;
    for (const NodePtr& child : nodes)
        if (child)
            kept.push_back(child);
    if (kept.empty())
        return nullptr;
    if (kept.size() == 1)
        return kept.front();
    auto node = makeNode(type);
    mutableNode(node).children = kept;
    return node;
}
} // namespace

NodePtr Union(const std::vector<NodePtr>& nodes) { return combine(NodeType::Union, nodes); }
NodePtr Union(const NodePtr& a, const NodePtr& b) { return combine(NodeType::Union, {a, b}); }
NodePtr Intersect(const std::vector<NodePtr>& nodes) { return combine(NodeType::Intersect, nodes); }
NodePtr Intersect(const NodePtr& a, const NodePtr& b) { return combine(NodeType::Intersect, {a, b}); }

NodePtr SmoothIntersect(const std::vector<NodePtr>& nodes, double radiusMm)
{
    if (!(radiusMm > 0.0))
        return Intersect(nodes);
    auto node = combine(NodeType::SmoothIntersect, nodes);
    if (node && node->type == NodeType::SmoothIntersect)
        mutableNode(node).value = radiusMm;
    return node;
}

NodePtr Subtract(const NodePtr& from, const NodePtr& tool)
{
    if (!from)
        return nullptr;
    if (!tool)
        return from;
    auto node = makeNode(NodeType::Subtract);
    mutableNode(node).children = {from, tool};
    return node;
}

NodePtr Negate(const NodePtr& child)
{
    if (!child)
        return nullptr;
    auto node = makeNode(NodeType::Negate);
    mutableNode(node).children = {child};
    return node;
}

NodePtr Offset(const NodePtr& child, double t)
{
    if (!child)
        return nullptr;
    auto node = makeNode(NodeType::Offset);
    mutableNode(node).children = {child};
    mutableNode(node).value = t;
    return node;
}

// max(f - t, -f): the shell of thickness t inside the surface.
NodePtr Hollow(const NodePtr& child, double thicknessMm)
{
    return Intersect(child, Negate(Offset(child, -std::abs(thicknessMm))));
}

// fromMm <= d <= toMm, e.g. a base plate standing off a wrap.
NodePtr Layer(const NodePtr& child, double fromMm, double toMm)
{
    if (!child)
        return nullptr;
    return Intersect(Offset(child, std::max(fromMm, toMm)), Negate(Offset(child, std::min(fromMm, toMm))));
}

NodePtr Transformed(const NodePtr& child, vtkMatrix4x4* childToWorld)
{
    if (!child || !childToWorld)
        return child;
    auto node = makeNode(NodeType::Transform);
    mutableNode(node).children = {child};
    auto inverse = vtkSmartPointer<vtkMatrix4x4>::New();
    vtkMatrix4x4::Invert(childToWorld, inverse);
    mutableNode(node).worldToChild = inverse;
    return node;
}

double BakedPlanarField::At(double u, double v) const
{
    if (values.empty())
        return kInf;
    const double gu = std::clamp((u - u0) / spacingMm, 0.0, nu - 1.000001);
    const double gv = std::clamp((v - v0) / spacingMm, 0.0, nv - 1.000001);
    const int i = static_cast<int>(gu), j = static_cast<int>(gv);
    const double tu = gu - i, tv = gv - j;
    const auto at = [&](int a, int b) {
        return static_cast<double>(values[static_cast<size_t>(a) + static_cast<size_t>(nu) * static_cast<size_t>(b)]);
    };
    if (nu == 1 || nv == 1)
        return at(std::min(i, nu - 1), std::min(j, nv - 1));
    return (at(i, j) * (1 - tu) + at(i + 1, j) * tu) * (1 - tv) + (at(i, j + 1) * (1 - tu) + at(i + 1, j + 1) * tu) * tv;
}

NodePtr PlanarPrism(const std::shared_ptr<const BakedPlanarField>& profile, double halfHeightMm)
{
    if (!profile || profile->values.empty())
        return nullptr;
    auto node = makeNode(NodeType::PlanarPrism);
    mutableNode(node).planar = profile;
    mutableNode(node).halfHeight = halfHeightMm;
    return node;
}

NodePtr PlanarHeight(const std::shared_ptr<const BakedPlanarField>& heights, double offsetMm)
{
    if (!heights || heights->values.empty())
        return nullptr;
    auto node = makeNode(NodeType::PlanarHeight);
    mutableNode(node).planar = heights;
    mutableNode(node).value = offsetMm;
    return node;
}

void SquaredDistanceTransform(std::vector<double>& squared, const std::array<int, 3>& dims)
{
    squaredDistanceTransform(squared, dims);
}

NodePtr Field(const std::shared_ptr<const BakedField>& field)
{
    if (!field || field->values.empty())
        return nullptr;
    auto node = makeNode(NodeType::MeshField);
    mutableNode(node).field = field;
    return node;
}

// ── One point ─────────────────────────────────────────────────────────────────
double Value(const ImplicitNode& node, const Vec3& p)
{
    switch (node.type) {
    case NodeType::Sphere:
        return length(sub(p, node.center)) - node.radius;
    case NodeType::Box:
    case NodeType::RoundedBox: {
        const Vec3 d = sub(p, node.center);
        const Vec3 q{std::abs(d[0]) - node.halfSize[0], std::abs(d[1]) - node.halfSize[1],
                     std::abs(d[2]) - node.halfSize[2]};
        const double outside = length({std::max(q[0], 0.0), std::max(q[1], 0.0), std::max(q[2], 0.0)});
        const double inside = std::min(std::max({q[0], q[1], q[2]}), 0.0);
        return outside + inside - node.minorRadius;
    }
    case NodeType::Revolved: {
        const Vec3 d = sub(p, node.center);
        const double axial = dot(d, node.axis);
        const double radial = length(sub(d, mul(node.axis, axial)));
        return polygonDistance2D(node.profile, radial, axial, true);
    }
    case NodeType::Torus: {
        const Vec3 d = sub(p, node.center);
        const double axial = dot(d, node.axis);
        const double radial = length(sub(d, mul(node.axis, axial)));
        return std::hypot(radial - node.radius, axial) - node.minorRadius;
    }
    case NodeType::Capsule: {
        const Vec3 ab = sub(node.axis, node.center);
        const Vec3 ap = sub(p, node.center);
        const double len2 = dot(ab, ab);
        const double t = len2 > 1e-18 ? std::clamp(dot(ap, ab) / len2, 0.0, 1.0) : 0.0;
        return length(sub(ap, mul(ab, t))) - node.radius;
    }
    case NodeType::HalfSpace:
        return dot(sub(p, node.center), node.axis);
    case NodeType::Prism: {
        const Vec3 d = sub(p, node.center);
        Vec3 u{}, v{}, w{};
        axisFrame(node.axis, u, v, w);
        const double planar = polygonDistance2D(node.profile, dot(d, u), dot(d, v), false);
        if (!(node.halfHeight > 0.0))
            return planar; // infinite prism
        return extrude(planar, std::abs(dot(d, w)) - node.halfHeight);
    }
    case NodeType::MeshField:
        return node.field ? node.field->At(p) : kInf;
    case NodeType::PlanarPrism: {
        if (!node.planar)
            return kInf;
        const Vec3 d = sub(p, node.planar->origin);
        const double planar = node.planar->At(dot(d, node.planar->uAxis), dot(d, node.planar->vAxis));
        if (!(node.halfHeight > 0.0))
            return planar;
        return extrude(planar, std::abs(dot(d, node.planar->axis)) - node.halfHeight);
    }
    case NodeType::PlanarHeight: {
        if (!node.planar)
            return kInf;
        const Vec3 d = sub(p, node.planar->origin);
        const double height = node.planar->At(dot(d, node.planar->uAxis), dot(d, node.planar->vAxis));
        return (height - node.value) - dot(d, node.planar->axis);
    }
    case NodeType::Union: {
        double best = kInf;
        for (const NodePtr& child : node.children)
            best = std::min(best, Value(*child, p));
        return best;
    }
    case NodeType::Intersect: {
        double best = -kInf;
        for (const NodePtr& child : node.children)
            best = std::max(best, Value(*child, p));
        return best;
    }
    case NodeType::SmoothIntersect: {
        // Smooth maximum: max(a, b) plus a bump where the two are within `value` of each other.
        const double k = node.value;
        double result = Value(*node.children[0], p);
        for (size_t i = 1; i < node.children.size(); ++i) {
            const double b = Value(*node.children[i], p);
            const double h = std::clamp(0.5 - 0.5 * (b - result) / k, 0.0, 1.0);
            result = b + (result - b) * h + k * h * (1.0 - h);
        }
        return result;
    }
    case NodeType::Subtract:
        return std::max(Value(*node.children[0], p), -Value(*node.children[1], p));
    case NodeType::Negate:
        return -Value(*node.children[0], p);
    case NodeType::Offset:
        return Value(*node.children[0], p) - node.value;
    case NodeType::Transform: {
        const double in[4] = {p[0], p[1], p[2], 1.0};
        double out[4] = {};
        node.worldToChild->MultiplyPoint(in, out);
        return Value(*node.children[0], Vec3{out[0], out[1], out[2]});
    }
    }
    return kInf;
}

// ── Bounds ────────────────────────────────────────────────────────────────────
bool Bounds(const NodePtr& node, double bounds[6])
{
    if (!node)
        return false;
    const ImplicitNode& n = *node;
    switch (n.type) {
    case NodeType::Sphere:
        sphereBounds(bounds, n.center, n.radius);
        return true;
    case NodeType::Box:
    case NodeType::RoundedBox: {
        const Vec3 half = add(n.halfSize, {n.minorRadius, n.minorRadius, n.minorRadius});
        resetBounds(bounds);
        growBounds(bounds, add(n.center, half));
        growBounds(bounds, sub(n.center, half));
        return true;
    }
    case NodeType::Revolved:
        sphereBounds(bounds, n.center, std::hypot(n.radius, n.halfHeight));
        return true;
    case NodeType::Torus:
        sphereBounds(bounds, n.center, n.radius + n.minorRadius);
        return true;
    case NodeType::Capsule:
        resetBounds(bounds);
        growBounds(bounds, add(n.center, {n.radius, n.radius, n.radius}));
        growBounds(bounds, sub(n.center, {n.radius, n.radius, n.radius}));
        growBounds(bounds, add(n.axis, {n.radius, n.radius, n.radius}));
        growBounds(bounds, sub(n.axis, {n.radius, n.radius, n.radius}));
        return true;
    case NodeType::Prism:
        if (!(n.halfHeight > 0.0))
            return false;
        sphereBounds(bounds, n.center, std::hypot(n.radius, n.halfHeight));
        return true;
    case NodeType::PlanarPrism: {
        if (!n.planar || !(n.halfHeight > 0.0))
            return false;
        const BakedPlanarField& f = *n.planar;
        resetBounds(bounds);
        for (double u : {f.u0, f.u0 + (f.nu - 1) * f.spacingMm})
            for (double v : {f.v0, f.v0 + (f.nv - 1) * f.spacingMm})
                for (double a : {-n.halfHeight, n.halfHeight})
                    growBounds(bounds, add(f.origin, add(mul(f.uAxis, u), add(mul(f.vAxis, v), mul(f.axis, a)))));
        return true;
    }
    case NodeType::PlanarHeight:
        return false;
    case NodeType::MeshField: {
        if (!n.field)
            return false;
        resetBounds(bounds);
        growBounds(bounds, n.field->origin);
        growBounds(bounds, add(n.field->origin, {(n.field->dims[0] - 1) * n.field->spacingMm,
                                                 (n.field->dims[1] - 1) * n.field->spacingMm,
                                                 (n.field->dims[2] - 1) * n.field->spacingMm}));
        return true;
    }
    case NodeType::Union: {
        resetBounds(bounds);
        for (const NodePtr& child : n.children) {
            double childBounds[6] = {};
            if (!Bounds(child, childBounds))
                return false; // an unbounded part makes the union unbounded
            growBounds(bounds, {childBounds[0], childBounds[2], childBounds[4]});
            growBounds(bounds, {childBounds[1], childBounds[3], childBounds[5]});
        }
        return !n.children.empty();
    }
    case NodeType::Intersect:
    case NodeType::SmoothIntersect: {
        bool any = false;
        for (const NodePtr& child : n.children) {
            double childBounds[6] = {};
            if (!Bounds(child, childBounds))
                continue; // half-spaces and negations only cut the bounded parts
            if (!any) {
                std::copy(childBounds, childBounds + 6, bounds);
                any = true;
            } else {
                for (int a = 0; a < 3; ++a) {
                    bounds[2 * a] = std::max(bounds[2 * a], childBounds[2 * a]);
                    bounds[2 * a + 1] = std::min(bounds[2 * a + 1], childBounds[2 * a + 1]);
                }
            }
        }
        return any;
    }
    case NodeType::Subtract:
        return Bounds(n.children[0], bounds);
    case NodeType::Negate:
    case NodeType::HalfSpace:
        return false;
    case NodeType::Offset: {
        if (!Bounds(n.children[0], bounds))
            return false;
        for (int a = 0; a < 3; ++a) {
            bounds[2 * a] -= n.value;
            bounds[2 * a + 1] += n.value;
        }
        return true;
    }
    case NodeType::Transform: {
        double childBounds[6] = {};
        if (!Bounds(n.children[0], childBounds))
            return false;
        auto childToWorld = vtkSmartPointer<vtkMatrix4x4>::New();
        vtkMatrix4x4::Invert(n.worldToChild, childToWorld);
        resetBounds(bounds);
        for (int corner = 0; corner < 8; ++corner) {
            const double in[4] = {childBounds[corner & 1], childBounds[2 + ((corner >> 1) & 1)],
                                  childBounds[4 + ((corner >> 2) & 1)], 1.0};
            double out[4] = {};
            childToWorld->MultiplyPoint(in, out);
            growBounds(bounds, {out[0], out[1], out[2]});
        }
        return true;
    }
    }
    return false;
}

// ── Voxel grid steps ──────────────────────────────────────────────────────────
VoxelMask RasterizeShells(const std::vector<vtkPolyData*>& meshes, double spacingMm, double paddingMm,
                          const std::atomic<bool>* cancel, QString* error)
{
    const auto fail = [error](const QString& message) {
        if (error)
            *error = message;
        return VoxelMask{};
    };
    double meshBounds[6] = {};
    resetBounds(meshBounds);
    bool any = false;
    for (vtkPolyData* mesh : meshes) {
        if (!mesh || mesh->GetNumberOfPolys() == 0)
            continue;
        double b[6] = {};
        mesh->GetBounds(b);
        growBounds(meshBounds, {b[0], b[2], b[4]});
        growBounds(meshBounds, {b[1], b[3], b[5]});
        any = true;
    }
    if (!any)
        return fail(QStringLiteral("La malla está vacía."));

    double h = std::max(0.05, spacingMm);
    double pad = 0.0;
    std::array<int, 3> dims{};
    const auto computeGrid = [&] {
        pad = std::max(0.0, paddingMm) + 3.0 * h;
        for (int a = 0; a < 3; ++a)
            dims[static_cast<size_t>(a)] =
                static_cast<int>(std::ceil((meshBounds[2 * a + 1] - meshBounds[2 * a] + 2.0 * pad) / h)) + 1;
    };
    computeGrid();
    const double requested = static_cast<double>(dims[0]) * dims[1] * dims[2];
    if (requested > kMaxVoxels) {
        h *= std::cbrt(requested / kMaxVoxels) * 1.01;
        computeGrid();
    }

    VoxelMask mask;
    mask.dims = dims;
    mask.spacingMm = h;
    mask.origin = {meshBounds[0] - pad, meshBounds[2] - pad, meshBounds[4] - pad};
    const size_t nx = static_cast<size_t>(dims[0]), ny = static_cast<size_t>(dims[1]);
    mask.solid.assign(nx * ny * static_cast<size_t>(dims[2]), 0);
    const auto index = [nx, ny](size_t i, size_t j, size_t k) { return i + nx * (j + ny * k); };

    for (vtkPolyData* mesh : meshes) {
        if (!mesh || mesh->GetNumberOfPolys() == 0)
            continue;
        const auto tri = triangulated(mesh);
        auto it = vtk::TakeSmartPointer(tri->GetPolys()->NewIterator());
        vtkIdType counter = 0;
        double a[3] = {}, b[3] = {}, c[3] = {};
        for (it->GoToFirstCell(); !it->IsDoneWithTraversal(); it->GoToNextCell()) {
            if ((++counter % 20000) == 0 && cancelled(cancel))
                return fail(QStringLiteral("Cálculo cancelado."));
            vtkIdType npts = 0;
            const vtkIdType* ids = nullptr;
            it->GetCurrentCell(npts, ids);
            if (npts != 3)
                continue;
            tri->GetPoint(ids[0], a);
            tri->GetPoint(ids[1], b);
            tri->GetPoint(ids[2], c);
            const double edge = std::max({std::sqrt(vtkMath::Distance2BetweenPoints(a, b)),
                                          std::sqrt(vtkMath::Distance2BetweenPoints(b, c)),
                                          std::sqrt(vtkMath::Distance2BetweenPoints(c, a))});
            const int steps = std::max(1, static_cast<int>(std::ceil(edge / (0.5 * h))));
            for (int s = 0; s <= steps; ++s)
                for (int t = 0; s + t <= steps; ++t) {
                    const double u = static_cast<double>(s) / steps;
                    const double v = static_cast<double>(t) / steps;
                    const long i = std::lround((a[0] + (b[0] - a[0]) * u + (c[0] - a[0]) * v - mask.origin[0]) / h);
                    const long j = std::lround((a[1] + (b[1] - a[1]) * u + (c[1] - a[1]) * v - mask.origin[1]) / h);
                    const long k = std::lround((a[2] + (b[2] - a[2]) * u + (c[2] - a[2]) * v - mask.origin[2]) / h);
                    if (i >= 0 && j >= 0 && k >= 0 && i < dims[0] && j < dims[1] && k < dims[2])
                        mask.solid[index(static_cast<size_t>(i), static_cast<size_t>(j), static_cast<size_t>(k))] = 1;
                }
        }
    }
    return mask;
}

void DilateMask(VoxelMask& mask, double radiusMm)
{
    if (mask.Empty() || !(radiusMm > 0.0))
        return;
    std::vector<double> distance(mask.solid.size());
    for (size_t id = 0; id < mask.solid.size(); ++id)
        distance[id] = mask.solid[id] ? 0.0 : kInf;
    squaredDistanceTransform(distance, mask.dims);
    const double limit = (radiusMm / mask.spacingMm) * (radiusMm / mask.spacingMm);
    for (size_t id = 0; id < mask.solid.size(); ++id)
        if (distance[id] <= limit)
            mask.solid[id] = 1;
}

void FillInteriorFromOutside(VoxelMask& mask)
{
    if (mask.Empty())
        return;
    const size_t nx = static_cast<size_t>(mask.dims[0]), ny = static_cast<size_t>(mask.dims[1]),
                 nz = static_cast<size_t>(mask.dims[2]);
    const auto index = [nx, ny](size_t i, size_t j, size_t k) { return i + nx * (j + ny * k); };
    std::vector<uint8_t> outside(mask.solid.size(), 0);
    std::vector<size_t> stack;
    stack.reserve(mask.solid.size() / 8);
    const auto push = [&](size_t i, size_t j, size_t k) {
        const size_t id = index(i, j, k);
        if (!outside[id] && !mask.solid[id]) {
            outside[id] = 1;
            stack.push_back(id);
        }
    };
    for (size_t j = 0; j < ny; ++j)
        for (size_t i = 0; i < nx; ++i) {
            push(i, j, 0);
            push(i, j, nz - 1);
        }
    for (size_t k = 0; k < nz; ++k) {
        for (size_t i = 0; i < nx; ++i) {
            push(i, 0, k);
            push(i, ny - 1, k);
        }
        for (size_t j = 0; j < ny; ++j) {
            push(0, j, k);
            push(nx - 1, j, k);
        }
    }
    while (!stack.empty()) {
        const size_t id = stack.back();
        stack.pop_back();
        const size_t i = id % nx;
        const size_t j = (id / nx) % ny;
        const size_t k = id / (nx * ny);
        if (i > 0) push(i - 1, j, k);
        if (i + 1 < nx) push(i + 1, j, k);
        if (j > 0) push(i, j - 1, k);
        if (j + 1 < ny) push(i, j + 1, k);
        if (k > 0) push(i, j, k - 1);
        if (k + 1 < nz) push(i, j, k + 1);
    }
    for (size_t id = 0; id < mask.solid.size(); ++id)
        mask.solid[id] = outside[id] ? 0 : 1;
}

std::shared_ptr<const BakedField> SignedDistanceField(const VoxelMask& mask)
{
    if (mask.Empty())
        return nullptr;
    const size_t nx = static_cast<size_t>(mask.dims[0]), ny = static_cast<size_t>(mask.dims[1]),
                 nz = static_cast<size_t>(mask.dims[2]);
    const size_t total = mask.solid.size();
    const auto index = [nx, ny](size_t i, size_t j, size_t k) { return i + nx * (j + ny * k); };

    // Distance is measured from the boundary voxels, whose centres sit on the surface. Measuring
    // between the inside and outside sets instead would put the zero crossing half a voxel out.
    std::vector<double> distance(total, kInf);
    for (size_t k = 0; k < nz; ++k)
        for (size_t j = 0; j < ny; ++j)
            for (size_t i = 0; i < nx; ++i) {
                const size_t id = index(i, j, k);
                if (!mask.solid[id])
                    continue;
                const bool boundary = i == 0 || j == 0 || k == 0 || i + 1 == nx || j + 1 == ny || k + 1 == nz ||
                                      !mask.solid[index(i - 1, j, k)] || !mask.solid[index(i + 1, j, k)] ||
                                      !mask.solid[index(i, j - 1, k)] || !mask.solid[index(i, j + 1, k)] ||
                                      !mask.solid[index(i, j, k - 1)] || !mask.solid[index(i, j, k + 1)];
                if (boundary)
                    distance[id] = 0.0;
            }
    squaredDistanceTransform(distance, mask.dims);

    auto baked = std::make_shared<BakedField>();
    baked->dims = mask.dims;
    baked->origin = mask.origin;
    baked->spacingMm = mask.spacingMm;
    baked->values.resize(total);
    for (size_t id = 0; id < total; ++id) {
        const double d = std::sqrt(distance[id]) * mask.spacingMm;
        baked->values[id] = static_cast<float>(mask.solid[id] ? -d : d);
    }
    return baked;
}

vtkSmartPointer<vtkImageData> ToImage(const BakedField& field)
{
    if (field.values.empty())
        return nullptr;
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(field.dims[0], field.dims[1], field.dims[2]);
    image->SetSpacing(field.spacingMm, field.spacingMm, field.spacingMm);
    image->SetOrigin(field.origin[0], field.origin[1], field.origin[2]);
    auto values = vtkSmartPointer<vtkFloatArray>::New();
    values->SetName("ImplicitField");
    values->SetNumberOfComponents(1);
    values->SetNumberOfTuples(static_cast<vtkIdType>(field.values.size()));
    std::copy(field.values.begin(), field.values.end(), values->GetPointer(0));
    image->GetPointData()->SetScalars(values);
    return image;
}

// ── Mesh baked to a field ─────────────────────────────────────────────────────
std::shared_ptr<const BakedField> BakeMeshField(vtkPolyData* mesh, double spacingMm, double paddingMm,
                                                const std::atomic<bool>* cancel, QString* error)
{
    return BakeMeshField(std::vector<vtkPolyData*>{mesh}, spacingMm, paddingMm, cancel, error);
}

std::shared_ptr<const BakedField> BakeMeshField(const std::vector<vtkPolyData*>& meshes, double spacingMm,
                                                double paddingMm, const std::atomic<bool>* cancel, QString* error)
{
    VoxelMask mask = RasterizeShells(meshes, spacingMm, paddingMm, cancel, error);
    if (mask.Empty())
        return nullptr;
    FillInteriorFromOutside(mask);
    if (cancelled(cancel)) {
        if (error)
            *error = QStringLiteral("Cálculo cancelado.");
        return nullptr;
    }
    return SignedDistanceField(mask);
}

std::shared_ptr<const BakedField> BakeFunction(const std::function<double(const Vec3&)>& function,
                                               const double bounds[6], double spacingMm, double paddingMm,
                                               const std::atomic<bool>* cancel)
{
    if (!function || !bounds)
        return nullptr;
    double h = std::max(0.02, spacingMm);
    const double pad = std::max(0.0, paddingMm) + 2.0 * h;
    std::array<int, 3> dims{};
    const auto computeGrid = [&] {
        for (int a = 0; a < 3; ++a)
            dims[static_cast<size_t>(a)] =
                static_cast<int>(std::ceil((bounds[2 * a + 1] - bounds[2 * a] + 2.0 * pad) / h)) + 1;
    };
    computeGrid();
    const double requested = static_cast<double>(dims[0]) * dims[1] * dims[2];
    if (requested > kMaxVoxels) {
        h *= std::cbrt(requested / kMaxVoxels) * 1.01;
        computeGrid();
    }
    auto baked = std::make_shared<BakedField>();
    baked->dims = dims;
    baked->spacingMm = h;
    baked->origin = {bounds[0] - pad, bounds[2] - pad, bounds[4] - pad};
    baked->values.resize(static_cast<size_t>(dims[0]) * dims[1] * dims[2]);
    std::atomic<bool> aborted{false};
    parallelFor(dims[2], [&](int kBegin, int kEnd) {
        for (int k = kBegin; k < kEnd; ++k) {
            if ((k % 8) == 0 && (cancelled(cancel) || aborted.load(std::memory_order_relaxed))) {
                aborted.store(true, std::memory_order_relaxed);
                return;
            }
            for (int j = 0; j < dims[1]; ++j) {
                const size_t row = static_cast<size_t>(j) * static_cast<size_t>(dims[0]) +
                                   static_cast<size_t>(k) * static_cast<size_t>(dims[0]) * static_cast<size_t>(dims[1]);
                for (int i = 0; i < dims[0]; ++i)
                    baked->values[row + static_cast<size_t>(i)] =
                        static_cast<float>(function(Vec3{baked->origin[0] + i * h, baked->origin[1] + j * h,
                                                         baked->origin[2] + k * h}));
            }
        }
    });
    if (aborted.load() || cancelled(cancel))
        return nullptr;
    return baked;
}

NodePtr MeshField(vtkPolyData* mesh, double spacingMm, double paddingMm, const std::atomic<bool>* cancel,
                  QString* error)
{
    return Field(BakeMeshField(mesh, spacingMm, paddingMm, cancel, error));
}

// ── Grid evaluation and polygonisation ────────────────────────────────────────
vtkSmartPointer<vtkImageData> Evaluate(const NodePtr& node, const double bounds[6], double spacingMm,
                                       const std::atomic<bool>* cancel)
{
    if (!node || !bounds)
        return nullptr;
    double h = std::max(0.02, spacingMm);
    std::array<int, 3> dims{};
    const auto computeGrid = [&] {
        for (int a = 0; a < 3; ++a)
            dims[static_cast<size_t>(a)] =
                static_cast<int>(std::ceil((bounds[2 * a + 1] - bounds[2 * a]) / h)) + 5; // two voxels of margin
    };
    computeGrid();
    const double requested = static_cast<double>(dims[0]) * dims[1] * dims[2];
    if (requested > kMaxVoxels) {
        h *= std::cbrt(requested / kMaxVoxels) * 1.01;
        computeGrid();
    }
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(dims[0], dims[1], dims[2]);
    image->SetSpacing(h, h, h);
    image->SetOrigin(bounds[0] - 2.0 * h, bounds[2] - 2.0 * h, bounds[4] - 2.0 * h);
    auto values = vtkSmartPointer<vtkFloatArray>::New();
    values->SetName("ImplicitField");
    values->SetNumberOfComponents(1);
    values->SetNumberOfTuples(static_cast<vtkIdType>(dims[0]) * dims[1] * dims[2]);
    image->GetPointData()->SetScalars(values);
    double origin[3] = {};
    image->GetOrigin(origin);
    float* data = values->GetPointer(0);
    std::atomic<bool> aborted{false};
    parallelFor(dims[2], [&](int kBegin, int kEnd) {
        for (int k = kBegin; k < kEnd; ++k) {
            if ((k % 8) == 0 && (cancelled(cancel) || aborted.load(std::memory_order_relaxed))) {
                aborted.store(true, std::memory_order_relaxed);
                return;
            }
            for (int j = 0; j < dims[1]; ++j) {
                const size_t row = static_cast<size_t>(j) * static_cast<size_t>(dims[0]) +
                                   static_cast<size_t>(k) * static_cast<size_t>(dims[0]) * static_cast<size_t>(dims[1]);
                for (int i = 0; i < dims[0]; ++i)
                    data[row + static_cast<size_t>(i)] = static_cast<float>(
                        Value(*node, Vec3{origin[0] + i * h, origin[1] + j * h, origin[2] + k * h}));
            }
        }
    });
    if (aborted.load() || cancelled(cancel))
        return nullptr;
    return image;
}

vtkSmartPointer<vtkPolyData> Polygonize(vtkImageData* field, const PolygonizeOptions& options)
{
    if (!field)
        return nullptr;
    auto surface = vtkSmartPointer<vtkFlyingEdges3D>::New();
    surface->SetInputData(field);
    surface->SetValue(0, options.isoValue);
    surface->ComputeNormalsOff();
    surface->ComputeGradientsOff();
    surface->ComputeScalarsOff();
    surface->Update();
    vtkSmartPointer<vtkPolyData> mesh = surface->GetOutput();
    if (!mesh || mesh->GetNumberOfPolys() == 0)
        return nullptr;
    // The field is negative inside, so the contour comes out facing inward: flip it once here and the
    // result is outward whether or not it is repaired afterwards.
    auto reverse = vtkSmartPointer<vtkReverseSense>::New();
    reverse->SetInputData(mesh);
    reverse->ReverseCellsOn();
    reverse->ReverseNormalsOn();
    reverse->Update();
    mesh = reverse->GetOutput();
    if (options.smoothingIterations > 0) {
        auto smooth = vtkSmartPointer<vtkWindowedSincPolyDataFilter>::New();
        smooth->SetInputData(mesh);
        smooth->SetNumberOfIterations(options.smoothingIterations);
        smooth->SetPassBand(options.passBand);
        smooth->BoundarySmoothingOff();
        smooth->FeatureEdgeSmoothingOff();
        smooth->NonManifoldSmoothingOn();
        smooth->NormalizeCoordinatesOn();
        smooth->Update();
        mesh = smooth->GetOutput();
    }
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(mesh);
    if (!options.repair)
        return out;
    const MeshRepairResult repaired = MeshRepairCore::Repair(out);
    return repaired.mesh ? repaired.mesh : out;
}

BuildResult Build(const NodePtr& node, const double bounds[6], double spacingMm, const PolygonizeOptions& options,
                  const std::atomic<bool>* cancel)
{
    BuildResult result;
    result.spacingMm = spacingMm;
    if (!node) {
        result.error = QStringLiteral("No hay geometría que construir.");
        return result;
    }
    double own[6] = {};
    if (!bounds) {
        if (!Bounds(node, own)) {
            result.error = QStringLiteral("La geometría no está acotada: indique los límites.");
            return result;
        }
        bounds = own;
    }
    result.field = Evaluate(node, bounds, spacingMm, cancel);
    if (!result.field) {
        result.error = QStringLiteral("Cálculo cancelado.");
        return result;
    }
    result.mesh = Polygonize(result.field, options);
    if (!result.mesh || result.mesh->GetNumberOfPolys() == 0) {
        result.error = QStringLiteral("La geometría quedó vacía con este espaciado.");
        return result;
    }
    result.ok = true;
    return result;
}
}
