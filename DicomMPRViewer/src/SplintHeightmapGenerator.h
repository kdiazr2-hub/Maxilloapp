#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// SplintHeightmapGenerator
//
// Occlusal splint generator based on height maps, modelled on ProPlan CMF.
// No Qt Widgets: usable from worker threads and from CTest executables.
//
//   Prepare()  – occlusal frame, boundary surfaces through the guide points and
//                ray-cast height maps of the upper / lower teeth (slow part).
//                Must be repeated when meshes, points, grid resolution or the
//                undercut settings change.
//   Build()    – contour, voxel solid, iso-surface and thickness (fast part).
//                Edge offset, fillet, clearance, impressions, thickness range,
//                contour smoothing and edited contours only need Build().
//
// Local frame: u = transverse, v = anterior, w = along the normal (toward the
// maxilla). Undercut removal casts the height map along an insertion vector
// tilted by the jaw angle toward undercutDirectionUV (standard vector).
// ─────────────────────────────────────────────────────────────────────────────

#include <QString>
#include <vtkSmartPointer.h>

#include <array>
#include <atomic>
#include <optional>
#include <vector>

class vtkMatrix4x4;
class vtkPolyData;

using SplintPoint3 = std::array<double, 3>;
using SplintPointUV = std::array<double, 2>;
using SplintContourUV = std::vector<SplintPointUV>;

struct SplintHeightmapParams
{
    // ── Build parameters ──────────────────────────────────────────────────
    double edgeOffsetMm = 1.0;       // 0.5–5, steps of 0.5
    double filletMm = 0.0;           // 0–1
    double clearanceMm = 0.1;        // spherical offset from the teeth, 0–2
    double minFeatureMm = 0.4;       // 0–1, splint fins narrower than this are removed
    bool impressionUpper = true;     // off: flat side following the points, touching the cusps
    bool impressionLower = true;
    double minThicknessMm = 1.5;
    double maxThicknessMm = 3.0;
    bool computeThickness = true;

    // Contour smoothing (ProPlan preference names). Sigmas are in contour
    // samples of 0.2 mm; distances and thresholds are in mm.
    double roundingFactorForSplintSide = 1.0; // multiplies convexHullSmoothSigma
    double convexHullFixDistance = 1.5;       // hull-side classification; closing radius = 2x
    double reduceThreshold = 1.5;             // maximum contour segment length
    double smoothThreshold = 0.2;             // maximum deviation after simplification
    double innerSmoothSigma = 30.0;
    double convexHullSmoothSigma = 15.0;

    // ── Prepare parameters ────────────────────────────────────────────────
    double gridResolutionMm = 0.2;
    bool undercutUpper = false;
    bool undercutLower = false;
    double undercutAngleUpperDeg = 5.0;
    double undercutAngleLowerDeg = 45.0;
    SplintPointUV undercutDirectionUV{0.0, 1.0}; // default: anterior
};

// ── Splint extras (ProPlan 4.11.4–4.11.6), Build stage, world coordinates ──
// Bevel: the plane through both points that follows the arch tangent; the
// material outside it (away from the arch centre) is removed.
struct SplintBevel
{
    SplintPoint3 first{};  // usually on the maxillary teeth
    SplintPoint3 second{}; // usually on the mandibular teeth
};

enum class SplintHoleOrientation
{
    SurfaceNormal, // perpendicular to the splint surface where it was placed
    Bevel          // parallel to the bevel face
};

// Cylinder subtracted from the splint (length 2 * kWireHoleHalfLengthMm).
struct SplintWireHole
{
    SplintPoint3 center{};
    SplintPoint3 axis{0.0, 0.0, 1.0};
    SplintPoint3 surfaceNormal{0.0, 0.0, 1.0}; // where it was placed; used to re-orient it
    double diameterMm = 1.0;
};

// Marked tooth / bracket surface: surface within radiusMm of the centre gets
// bracketOffsetMm of extra clearance. The jaw follows the side of the
// occlusal plane the centre lies on.
struct SplintBracketMark
{
    SplintPoint3 center{};
    double radiusMm = 1.5;
};

struct SplintExtras
{
    std::optional<SplintBevel> bevel;
    std::vector<SplintWireHole> wireHoles;
    std::vector<SplintBracketMark> bracketMarks;
    double bracketOffsetMm = 0.5; // 0–2
};

struct SplintHeightmapInputs
{
    vtkPolyData* upperTeeth = nullptr;   // world coordinates, planned position
    vtkPolyData* lowerTeeth = nullptr;
    std::vector<SplintPoint3> upperPoints; // >= 3
    std::vector<SplintPoint3> lowerPoints; // >= 3
    // Optional anterior direction in world coordinates. When absent it is
    // estimated from the arch shape.
    std::optional<SplintPoint3> anteriorDirectionWorld;
    SplintHeightmapParams params;
    // Closed polylines in the (u, v) occlusal frame. Empty = automatic contour.
    std::vector<SplintContourUV> contourOverrideUV;
    SplintExtras extras;
    const std::atomic<bool>* cancel = nullptr;
};

struct SplintOcclusalFrame
{
    SplintPoint3 origin{};
    SplintPoint3 axisU{1.0, 0.0, 0.0};
    SplintPoint3 axisV{0.0, 1.0, 0.0};
    SplintPoint3 normal{0.0, 0.0, 1.0};
    bool anteriorResolved = false;

    SplintPoint3 ToWorld(double u, double v, double w) const;
    SplintPoint3 ToLocal(const SplintPoint3& world) const;
    vtkSmartPointer<vtkMatrix4x4> LocalToWorldMatrix() const;
};

struct SplintBoundarySurface
{
    // w = c0 + c1 u + c2 v [+ c3 u² + c4 uv + c5 v²], clamped to [minW, maxW]
    std::array<double, 6> coefficients{};
    bool quadratic = false;
    double minW = 0.0;
    double maxW = 0.0;

    double Evaluate(double u, double v) const;
};

struct SplintHeightMap
{
    int nu = 0;
    int nv = 0;
    double u0 = 0.0;
    double v0 = 0.0;
    double spacing = 0.0;
    // A column (cu, cv) contains the local points (cu, cv) + sigma * w * shear,
    // sigma = +1 for the upper jaw and -1 for the lower jaw.
    SplintPointUV shear{0.0, 0.0};
    std::vector<float> values; // w of the first hit coming from the splint; NaN = no hit

    float At(int i, int j) const { return values[static_cast<size_t>(j) * nu + i]; }
    float Sample(double cu, double cv) const;
};

struct SplintHeightmapPrepared
{
    bool ok = false;
    QString error;
    QString report;
    SplintHeightmapParams params; // parameters used by Prepare()
    SplintOcclusalFrame frame;
    SplintBoundarySurface upperSurface;
    SplintBoundarySurface lowerSurface;
    SplintHeightMap upperMap;
    SplintHeightMap lowerMap;
    // Triangles kept for ray casting after cropping geometry that cannot
    // affect the splint (e.g. bone far from the occlusal band).
    vtkIdType upperTrianglesUsed = 0;
    vtkIdType upperTrianglesTotal = 0;
    vtkIdType lowerTrianglesUsed = 0;
    vtkIdType lowerTrianglesTotal = 0;
};

struct SplintHeightmapResult
{
    bool ok = false;
    QString error;
    QString report;
    // World coordinates, closed surface. Point array "Grosor" (mm) when
    // params.computeThickness is on.
    vtkSmartPointer<vtkPolyData> mesh;
    std::vector<SplintContourUV> contoursUV;
    vtkSmartPointer<vtkPolyData> contourWorld; // closed polylines on the occlusal plane
    SplintOcclusalFrame frame;
    double minThicknessMm = 0.0;
    double p05ThicknessMm = 0.0;
    double perforatedAreaMm2 = 0.0;
};

class SplintHeightmapGenerator
{
public:
    static constexpr const char* ThicknessArrayName = "Grosor";
    static constexpr const char* ThicknessColorArrayName = "ColorGrosor";
    static constexpr double WireHoleHalfLengthMm = 15.0;
    static constexpr double MaxBracketOffsetMm = 2.0;

    static SplintHeightmapPrepared Prepare(const SplintHeightmapInputs& inputs);
    static SplintHeightmapResult Build(const SplintHeightmapPrepared& prepared,
                                       const SplintHeightmapInputs& inputs);
    static SplintHeightmapResult Generate(const SplintHeightmapInputs& inputs);

    // Adds RGB point scalars: red below min, yellow in range, purple above max.
    static void ApplyThicknessColors(vtkPolyData* mesh, double minMm, double maxMm);
    static double ContourArea(const SplintContourUV& contour);

    // Bevel plane in world coordinates; normal points toward the removed side.
    // False when the points coincide or the line follows the arch tangent.
    static bool BevelPlane(const SplintBevel& bevel, const SplintOcclusalFrame& frame,
                           SplintPoint3& origin, SplintPoint3& normal);
    // Unit axis (toward the maxilla) of a wire hole placed on a surface with
    // `surfaceNormal`; Bevel keeps the hole parallel to the bevel face and
    // falls back to the surface normal without a valid bevel.
    static SplintPoint3 WireHoleAxis(SplintHoleOrientation orientation, const SplintPoint3& surfaceNormal,
                                     const std::optional<SplintBevel>& bevel, const SplintOcclusalFrame& frame);
};
