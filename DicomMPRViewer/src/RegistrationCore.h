#pragma once

#include <array>
#include <vector>

#include <vtkMatrix4x4.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>

// Rigid registration of the MODELOS step (intraoral scan onto the segmented arch) and of the bite step (bite scan
// onto the Le Fort segment, mandible onto the bite scan): paired points first, then ICP restricted to a region of
// interest. MainWindow only gathers the points, formats the reports and applies the result.
// Millimetres, DICOM LPS, Z+ superior.
namespace RegistrationCore {

using Point = std::array<double, 3>;

// Rigid least squares (vtkLandmarkTransform) of moving onto fixed. Uses the first min(moving, fixed) pairs; needs 3.
struct LandmarkFit {
    bool valid = false;
    int pairs = 0;
    double rmsMm = 0.0;
    double maxMm = 0.0;
    vtkSmartPointer<vtkMatrix4x4> matrix; // identity if !valid
};
LandmarkFit FitLandmarks(const std::vector<Point>& moving, const std::vector<Point>& fixed);

// vtkTransformPolyDataFilter of a deep copy; nullptr for an empty mesh.
vtkSmartPointer<vtkPolyData> Apply(vtkPolyData* mesh, vtkMatrix4x4* matrix);

// Building blocks of the ROI, public for the tests.
vtkSmartPointer<vtkPolyData> CleanTriangles(vtkPolyData* input);
// Cells whose centroid lies in box = {xmin, xmax, ymin, ymax, zmin, zmax}.
vtkSmartPointer<vtkPolyData> CropCellsByBox(vtkPolyData* input, const std::array<double, 6>& box);
// Cells with the centroid or a vertex within maxDistance of the target surface.
vtkSmartPointer<vtkPolyData> KeepCellsNearSurface(vtkPolyData* input, vtkPolyData* target, double maxDistance);
// Cells with the centroid or a vertex within radius of any of the points.
vtkSmartPointer<vtkPolyData> KeepCellsNearPoints(vtkPolyData* input, const std::vector<Point>& points, double radius);

// The arch ROI: the scan's bounds, 12–28 mm wider across, and towards the bone (up for the maxilla, down for the
// mandible) 14–30 mm against 5–10 mm towards the bite.
std::array<double, 6> ArchRoi(const double archBounds[6], bool upperArch);

// Distance from the registered moving surface to the target, sampled on at most 25 000 points.
struct FitMetrics {
    long long samples = 0;
    double rmseMm = 0.0;
    double meanMm = 0.0;
    double maxMm = 0.0;
    double within1MmPercent = 0.0;
    double within2MmPercent = 0.0;
};
FitMetrics MeasureFit(vtkPolyData* registered, vtkPolyData* target);

enum class IcpStatus {
    Ok,
    EmptyInput,  // no mesh: mesh is nullptr
    CleanFailed, // cleaning left nothing: mesh is nullptr
    RoiTooSmall, // fewer than 50 points to match: mesh is the cleaned input, unmoved, matrix identity
};

struct IcpResult {
    IcpStatus status = IcpStatus::EmptyInput;
    vtkSmartPointer<vtkPolyData> mesh;   // the whole moving mesh, cleaned and moved
    vtkSmartPointer<vtkMatrix4x4> matrix; // identity unless status == Ok
    FitMetrics fit;                       // on the ROI, after ICP
};

// Intraoral scan already placed by points onto its bone: 80 iterations, both surfaces cropped to ArchRoi.
IcpResult RefineArch(vtkPolyData* arch, vtkPolyData* bone, bool upperArch);

// Bite registration: the moving mesh within the bite scan's bounds (+4–12 mm) and 3–7 mm of it, or, when the
// surgeon's points give enough surface, only within 12 mm of them on both sides. 120 iterations, 5000 landmarks.
// movingLandmarks are the moving mesh's points AFTER the landmark fit.
IcpResult RefineBite(vtkPolyData* moving, vtkPolyData* biteScan, const std::vector<Point>& movingLandmarks,
                     const std::vector<Point>& targetLandmarks);

} // namespace RegistrationCore
