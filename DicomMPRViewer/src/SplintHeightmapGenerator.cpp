#include "SplintHeightmapGenerator.h"

#include <vtkCellArray.h>
#include <vtkCellArrayIterator.h>
#include <vtkDecimatePro.h>
#include <vtkFeatureEdges.h>
#include <vtkFloatArray.h>
#include <vtkFlyingEdges2D.h>
#include <vtkFlyingEdges3D.h>
#include <vtkGenericCell.h>
#include <vtkImageData.h>
#include <vtkImageEuclideanDistance.h>
#include <vtkImageGaussianSmooth.h>
#include <vtkMath.h>
#include <vtkMatrix4x4.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataNormals.h>
#include <vtkStaticCellLocator.h>
#include <vtkStripper.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>
#include <vtkTriangleFilter.h>
#include <vtkUnsignedCharArray.h>

#include <QElapsedTimer>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <thread>

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kContourSampleMm = 0.2;
constexpr double kMaxHoleAreaMm2 = 25.0;
constexpr double kMinComponentFraction = 0.10;
constexpr double kGridMarginMm = 8.0;
constexpr double kMaxClearanceMm = 2.0;
// Geometry beyond the point surfaces by more than this is irrelevant to the
// splint (clamped to the surfaces); must exceed kMaxClearanceMm.
constexpr double kCropMarginMm = 3.0;
constexpr double kMaxUndercutAngleDeg = 60.0;
constexpr double kFarDistance = 1.0e9;
constexpr size_t kMaxGridCells = 6000000;
constexpr size_t kMaxVoxels = 80000000;
const float kNaN = std::numeric_limits<float>::quiet_NaN();

using Vec3 = SplintPoint3;
using UV = SplintPointUV;

Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 mul(const Vec3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
Vec3 normalized(const Vec3& v, const Vec3& fallback)
{
    const double len = std::sqrt(dot(v, v));
    return len > 1e-9 ? mul(v, 1.0 / len) : fallback;
}

double distUV(const UV& a, const UV& b) { return std::hypot(b[0] - a[0], b[1] - a[1]); }

bool isCancelled(const SplintHeightmapInputs& inputs)
{
    return inputs.cancel && inputs.cancel->load(std::memory_order_relaxed);
}

QString cancelledMessage() { return QStringLiteral("Cálculo cancelado."); }

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

// Gaussian elimination with partial pivoting on an n×n row-major system.
bool solveLinear(std::vector<double> a, std::vector<double> b, int n, std::vector<double>& x)
{
    double scale = 0.0;
    for (int i = 0; i < n; ++i)
        scale = std::max(scale, std::abs(a[static_cast<size_t>(i * n + i)]));
    const double eps = std::max(1e-12, scale * 1e-10);
    for (int col = 0; col < n; ++col) {
        int pivot = col;
        for (int r = col + 1; r < n; ++r) {
            if (std::abs(a[static_cast<size_t>(r * n + col)]) > std::abs(a[static_cast<size_t>(pivot * n + col)]))
                pivot = r;
        }
        if (std::abs(a[static_cast<size_t>(pivot * n + col)]) < eps)
            return false;
        if (pivot != col) {
            for (int c = 0; c < n; ++c)
                std::swap(a[static_cast<size_t>(col * n + c)], a[static_cast<size_t>(pivot * n + c)]);
            std::swap(b[static_cast<size_t>(col)], b[static_cast<size_t>(pivot)]);
        }
        for (int r = col + 1; r < n; ++r) {
            const double f = a[static_cast<size_t>(r * n + col)] / a[static_cast<size_t>(col * n + col)];
            for (int c = col; c < n; ++c)
                a[static_cast<size_t>(r * n + c)] -= f * a[static_cast<size_t>(col * n + c)];
            b[static_cast<size_t>(r)] -= f * b[static_cast<size_t>(col)];
        }
    }
    x.assign(static_cast<size_t>(n), 0.0);
    for (int r = n - 1; r >= 0; --r) {
        double s = b[static_cast<size_t>(r)];
        for (int c = r + 1; c < n; ++c)
            s -= a[static_cast<size_t>(r * n + c)] * x[static_cast<size_t>(c)];
        x[static_cast<size_t>(r)] = s / a[static_cast<size_t>(r * n + r)];
    }
    return true;
}

// Least squares with basis rows; ridge[i] is added to the diagonal.
bool leastSquares(const std::vector<std::vector<double>>& rows, const std::vector<double>& rhs,
                  const std::vector<double>& ridge, std::vector<double>& x)
{
    if (rows.empty())
        return false;
    const int n = static_cast<int>(rows.front().size());
    std::vector<double> ata(static_cast<size_t>(n * n), 0.0);
    std::vector<double> atb(static_cast<size_t>(n), 0.0);
    for (size_t k = 0; k < rows.size(); ++k) {
        for (int i = 0; i < n; ++i) {
            atb[static_cast<size_t>(i)] += rows[k][static_cast<size_t>(i)] * rhs[k];
            for (int j = 0; j < n; ++j)
                ata[static_cast<size_t>(i * n + j)] += rows[k][static_cast<size_t>(i)] * rows[k][static_cast<size_t>(j)];
        }
    }
    for (int i = 0; i < n && i < static_cast<int>(ridge.size()); ++i)
        ata[static_cast<size_t>(i * n + i)] += ridge[static_cast<size_t>(i)];
    return solveLinear(ata, atb, n, x);
}

bool fitBoundarySurface(const std::vector<Vec3>& local, SplintBoundarySurface& surface)
{
    if (local.size() < 3)
        return false;
    double minW = local.front()[2];
    double maxW = local.front()[2];
    for (const Vec3& p : local) {
        minW = std::min(minW, p[2]);
        maxW = std::max(maxW, p[2]);
    }
    surface = {};
    surface.minW = minW - 2.0;
    surface.maxW = maxW + 2.0;

    std::vector<double> rhs;
    for (const Vec3& p : local)
        rhs.push_back(p[2]);

    if (local.size() >= 6) {
        // Curve of Spee: quadratic with ridge regularisation on normalised
        // coordinates so six points do not produce an oscillating interpolant.
        double scale = 0.0;
        for (const Vec3& p : local)
            scale += p[0] * p[0] + p[1] * p[1];
        scale = std::max(1.0, std::sqrt(scale / static_cast<double>(local.size())));
        std::vector<std::vector<double>> rows;
        for (const Vec3& p : local) {
            const double u = p[0] / scale;
            const double v = p[1] / scale;
            rows.push_back({1.0, u, v, u * u, u * v, v * v});
        }
        const double lambda = 0.05 * static_cast<double>(local.size());
        std::vector<double> b;
        if (leastSquares(rows, rhs, {0.0, 0.0, 0.0, lambda, lambda, lambda}, b)) {
            surface.coefficients = {b[0], b[1] / scale, b[2] / scale,
                                    b[3] / (scale * scale), b[4] / (scale * scale), b[5] / (scale * scale)};
            surface.quadratic = true;
            return true;
        }
    }

    std::vector<std::vector<double>> rows;
    for (const Vec3& p : local)
        rows.push_back({1.0, p[0], p[1]});
    std::vector<double> b;
    if (!leastSquares(rows, rhs, {}, b))
        return false;
    surface.coefficients = {b[0], b[1], b[2], 0.0, 0.0, 0.0};
    return true;
}

bool computeInitialFrame(const SplintHeightmapInputs& inputs, SplintOcclusalFrame& frame, QString* error)
{
    std::vector<Vec3> all = inputs.upperPoints;
    all.insert(all.end(), inputs.lowerPoints.begin(), inputs.lowerPoints.end());

    auto mean = [](const std::vector<Vec3>& pts) {
        Vec3 m{};
        for (const Vec3& p : pts)
            m = add(m, p);
        return mul(m, 1.0 / static_cast<double>(pts.size()));
    };
    const Vec3 centroid = mean(all);
    const Vec3 upperMean = mean(inputs.upperPoints);
    const Vec3 lowerMean = mean(inputs.lowerPoints);

    double c0[3] = {}, c1[3] = {}, c2[3] = {};
    double* cov[3] = {c0, c1, c2};
    for (const Vec3& p : all) {
        const Vec3 d = sub(p, centroid);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                cov[r][c] += d[static_cast<size_t>(r)] * d[static_cast<size_t>(c)];
    }
    double eigenvalues[3] = {};
    double e0[3] = {}, e1[3] = {}, e2[3] = {};
    double* eigenvectors[3] = {e0, e1, e2};
    vtkMath::Jacobi(cov, eigenvalues, eigenvectors); // sorted decreasing, vectors in columns

    const Vec3 major{eigenvectors[0][0], eigenvectors[1][0], eigenvectors[2][0]};
    Vec3 n{eigenvectors[0][2], eigenvectors[1][2], eigenvectors[2][2]};
    const Vec3 upward = sub(upperMean, lowerMean);
    if (dot(n, upward) < 0.0)
        n = mul(n, -1.0);
    if (dot(n, upward) < 0.5) {
        if (error)
            *error = QStringLiteral("Los puntos superiores deben quedar por encima de los inferiores respecto al plano oclusal.");
        return false;
    }
    n = normalized(n, {0.0, 0.0, 1.0});
    const Vec3 u = normalized(sub(major, mul(n, dot(major, n))), {1.0, 0.0, 0.0});

    frame = {};
    frame.origin = centroid;
    frame.normal = n;
    frame.axisU = u;
    frame.axisV = cross(n, u);
    return true;
}

void setFrameAnterior(SplintOcclusalFrame& frame, const Vec3& anteriorWorld)
{
    const Vec3 v = normalized(sub(anteriorWorld, mul(frame.normal, dot(anteriorWorld, frame.normal))), frame.axisV);
    frame.axisV = v;
    frame.axisU = cross(v, frame.normal); // keeps (u, v, n) right-handed
}

bool fitParabola(const std::vector<double>& x, const std::vector<double>& y, double& c2, double& rms)
{
    std::vector<std::vector<double>> rows;
    rows.reserve(x.size());
    for (double xi : x)
        rows.push_back({1.0, xi, xi * xi});
    std::vector<double> c;
    if (!leastSquares(rows, y, {}, c))
        return false;
    double sum = 0.0;
    for (size_t i = 0; i < x.size(); ++i) {
        const double r = y[i] - (c[0] + c[1] * x[i] + c[2] * x[i] * x[i]);
        sum += r * r;
    }
    c2 = c[2];
    rms = std::sqrt(sum / static_cast<double>(x.size()));
    return true;
}

// The arch is U-shaped: its closed end is anterior.
void resolveAnterior(SplintOcclusalFrame& frame, const SplintHeightmapInputs& inputs,
                     const std::vector<UV>& samples)
{
    if (inputs.anteriorDirectionWorld) {
        const Vec3& a = *inputs.anteriorDirectionWorld;
        const Vec3 inPlane = sub(a, mul(frame.normal, dot(a, frame.normal)));
        if (dot(inPlane, inPlane) > 1e-12) {
            setFrameAnterior(frame, inPlane);
            frame.anteriorResolved = true;
            return;
        }
    }
    if (samples.size() < 20)
        return;

    std::vector<double> us, vs;
    for (const UV& s : samples) {
        us.push_back(s[0]);
        vs.push_back(s[1]);
    }
    double c2VofU = 0.0, rmsVofU = 0.0, c2UofV = 0.0, rmsUofV = 0.0;
    const bool okA = fitParabola(us, vs, c2VofU, rmsVofU);
    const bool okB = fitParabola(vs, us, c2UofV, rmsUofV);
    if (!okA && !okB)
        return;
    const bool useA = okA && (!okB || rmsVofU <= rmsUofV);
    const std::vector<double>& xs = useA ? us : vs;
    const double c2 = useA ? c2VofU : c2UofV;
    const auto [minIt, maxIt] = std::minmax_element(xs.begin(), xs.end());
    const double halfRange = 0.5 * (*maxIt - *minIt);
    if (std::abs(c2) * halfRange * halfRange < 2.0)
        return;

    const Vec3 axis = useA ? frame.axisV : frame.axisU;
    setFrameAnterior(frame, c2 > 0.0 ? mul(axis, -1.0) : axis);
    frame.anteriorResolved = true;
}

vtkSmartPointer<vtkPolyData> triangulated(vtkPolyData* mesh)
{
    auto tri = vtkSmartPointer<vtkTriangleFilter>::New();
    tri->SetInputData(mesh);
    tri->PassVertsOff();
    tri->PassLinesOff();
    tri->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(tri->GetOutput());
    return out;
}

struct BandScan
{
    double uMin = kFarDistance, uMax = -kFarDistance;
    double vMin = kFarDistance, vMax = -kFarDistance;
    double wMin = kFarDistance, wMax = -kFarDistance;
    vtkIdType bandCells = 0;
    std::vector<UV> samples;
};

// Finds the triangles whose vertical extent overlaps the band between the two
// boundary surfaces: their footprint bounds the height-map grid.
BandScan scanBand(vtkPolyData* mesh, const SplintOcclusalFrame& frame,
                  const SplintBoundarySurface& lower, const SplintBoundarySurface& upper)
{
    BandScan scan;
    const vtkIdType nPoints = mesh->GetNumberOfPoints();
    std::vector<Vec3> local(static_cast<size_t>(nPoints));
    double p[3] = {};
    for (vtkIdType i = 0; i < nPoints; ++i) {
        mesh->GetPoint(i, p);
        local[static_cast<size_t>(i)] = frame.ToLocal({p[0], p[1], p[2]});
        scan.wMin = std::min(scan.wMin, local[static_cast<size_t>(i)][2]);
        scan.wMax = std::max(scan.wMax, local[static_cast<size_t>(i)][2]);
    }

    const vtkIdType sampleStride = std::max<vtkIdType>(1, mesh->GetNumberOfPolys() / 4000);
    auto it = vtk::TakeSmartPointer(mesh->GetPolys()->NewIterator());
    vtkIdType cellIndex = 0;
    for (it->GoToFirstCell(); !it->IsDoneWithTraversal(); it->GoToNextCell(), ++cellIndex) {
        vtkIdType npts = 0;
        const vtkIdType* ids = nullptr;
        it->GetCurrentCell(npts, ids);
        if (npts < 3)
            continue;
        double minW = kFarDistance, maxW = -kFarDistance, cu = 0.0, cv = 0.0;
        for (vtkIdType k = 0; k < npts; ++k) {
            const Vec3& q = local[static_cast<size_t>(ids[k])];
            minW = std::min(minW, q[2]);
            maxW = std::max(maxW, q[2]);
            cu += q[0];
            cv += q[1];
        }
        cu /= static_cast<double>(npts);
        cv /= static_cast<double>(npts);
        if (maxW < lower.Evaluate(cu, cv) || minW > upper.Evaluate(cu, cv))
            continue;
        ++scan.bandCells;
        for (vtkIdType k = 0; k < npts; ++k) {
            const Vec3& q = local[static_cast<size_t>(ids[k])];
            scan.uMin = std::min(scan.uMin, q[0]);
            scan.uMax = std::max(scan.uMax, q[0]);
            scan.vMin = std::min(scan.vMin, q[1]);
            scan.vMax = std::max(scan.vMax, q[1]);
        }
        if (cellIndex % sampleStride == 0)
            scan.samples.push_back({cu, cv});
    }
    return scan;
}

// Keeps the triangles that can influence the height map: those reaching below
// (upper jaw) or above (lower jaw) wLimit whose footprint overlaps the rays.
vtkSmartPointer<vtkPolyData> cropForRays(vtkPolyData* mesh, const SplintOcclusalFrame& frame, bool upperJaw,
                                         double wLimit, double uMin, double uMax, double vMin, double vMax)
{
    const vtkIdType nPoints = mesh->GetNumberOfPoints();
    std::vector<Vec3> local(static_cast<size_t>(nPoints));
    double p[3] = {};
    for (vtkIdType i = 0; i < nPoints; ++i) {
        mesh->GetPoint(i, p);
        local[static_cast<size_t>(i)] = frame.ToLocal({p[0], p[1], p[2]});
    }

    auto polys = vtkSmartPointer<vtkCellArray>::New();
    auto it = vtk::TakeSmartPointer(mesh->GetPolys()->NewIterator());
    for (it->GoToFirstCell(); !it->IsDoneWithTraversal(); it->GoToNextCell()) {
        vtkIdType npts = 0;
        const vtkIdType* ids = nullptr;
        it->GetCurrentCell(npts, ids);
        double minW = kFarDistance, maxW = -kFarDistance;
        double bu0 = kFarDistance, bu1 = -kFarDistance, bv0 = kFarDistance, bv1 = -kFarDistance;
        for (vtkIdType k = 0; k < npts; ++k) {
            const Vec3& q = local[static_cast<size_t>(ids[k])];
            minW = std::min(minW, q[2]);
            maxW = std::max(maxW, q[2]);
            bu0 = std::min(bu0, q[0]);
            bu1 = std::max(bu1, q[0]);
            bv0 = std::min(bv0, q[1]);
            bv1 = std::max(bv1, q[1]);
        }
        const bool reaches = upperJaw ? minW <= wLimit : maxW >= wLimit;
        if (!reaches || bu1 < uMin || bu0 > uMax || bv1 < vMin || bv0 > vMax)
            continue;
        polys->InsertNextCell(npts, ids);
    }
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->SetPoints(mesh->GetPoints());
    out->SetPolys(polys);
    return out;
}

SplintHeightMap castHeightMap(vtkPolyData* mesh, const SplintOcclusalFrame& frame,
                              int nu, int nv, double u0, double v0, double spacing,
                              const UV& shear, bool upperJaw, double wStart, double wEnd,
                              const SplintHeightmapInputs& inputs)
{
    SplintHeightMap map;
    map.nu = nu;
    map.nv = nv;
    map.u0 = u0;
    map.v0 = v0;
    map.spacing = spacing;
    map.shear = shear;
    map.values.assign(static_cast<size_t>(nu) * static_cast<size_t>(nv), kNaN);
    if (mesh->GetNumberOfPolys() == 0)
        return map;

    auto locator = vtkSmartPointer<vtkStaticCellLocator>::New();
    locator->SetDataSet(mesh);
    locator->BuildLocator();

    const double sigma = upperJaw ? 1.0 : -1.0;
    parallelFor(nv, [&](int jBegin, int jEnd) {
        auto cell = vtkSmartPointer<vtkGenericCell>::New();
        for (int j = jBegin; j < jEnd; ++j) {
            if (isCancelled(inputs))
                return;
            for (int i = 0; i < nu; ++i) {
                const double cu = u0 + i * spacing;
                const double cv = v0 + j * spacing;
                const Vec3 p1 = frame.ToWorld(cu + sigma * wStart * shear[0], cv + sigma * wStart * shear[1], wStart);
                const Vec3 p2 = frame.ToWorld(cu + sigma * wEnd * shear[0], cv + sigma * wEnd * shear[1], wEnd);
                double t = 0.0;
                double x[3] = {};
                double pcoords[3] = {};
                int subId = 0;
                vtkIdType cellId = -1;
                if (locator->IntersectWithLine(p1.data(), p2.data(), 1e-7, t, x, pcoords, subId, cellId, cell)) {
                    map.values[static_cast<size_t>(j) * nu + i] =
                        static_cast<float>(dot(sub({x[0], x[1], x[2]}, frame.origin), frame.normal));
                }
            }
        }
    });
    return map;
}

// ── 2-D grid helpers ─────────────────────────────────────────────────────────

void edt1d(const std::vector<double>& f, int n, std::vector<double>& d, std::vector<int>& v, std::vector<double>& z)
{
    int k = 0;
    v[0] = 0;
    z[0] = -std::numeric_limits<double>::infinity();
    z[1] = std::numeric_limits<double>::infinity();
    for (int q = 1; q < n; ++q) {
        double s = ((f[static_cast<size_t>(q)] + q * q) - (f[static_cast<size_t>(v[static_cast<size_t>(k)])] + v[static_cast<size_t>(k)] * v[static_cast<size_t>(k)]))
                   / (2.0 * q - 2.0 * v[static_cast<size_t>(k)]);
        while (s <= z[static_cast<size_t>(k)]) {
            --k;
            s = ((f[static_cast<size_t>(q)] + q * q) - (f[static_cast<size_t>(v[static_cast<size_t>(k)])] + v[static_cast<size_t>(k)] * v[static_cast<size_t>(k)]))
                / (2.0 * q - 2.0 * v[static_cast<size_t>(k)]);
        }
        ++k;
        v[static_cast<size_t>(k)] = q;
        z[static_cast<size_t>(k)] = s;
        z[static_cast<size_t>(k + 1)] = std::numeric_limits<double>::infinity();
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[static_cast<size_t>(k + 1)] < q)
            ++k;
        const double dq = q - v[static_cast<size_t>(k)];
        d[static_cast<size_t>(q)] = dq * dq + f[static_cast<size_t>(v[static_cast<size_t>(k)])];
    }
}

// Exact Euclidean distance (mm) from every cell centre to the nearest feature cell.
std::vector<float> distanceField(const std::vector<uint8_t>& feature, int nu, int nv, double spacing)
{
    constexpr double kBig = 1.0e20;
    std::vector<double> g(feature.size());
    for (size_t i = 0; i < feature.size(); ++i)
        g[i] = feature[i] ? 0.0 : kBig;

    const int n = std::max(nu, nv);
    std::vector<double> f(static_cast<size_t>(n)), d(static_cast<size_t>(n)), z(static_cast<size_t>(n + 1));
    std::vector<int> v(static_cast<size_t>(n));
    for (int j = 0; j < nv; ++j) {
        for (int i = 0; i < nu; ++i)
            f[static_cast<size_t>(i)] = g[static_cast<size_t>(j) * nu + i];
        edt1d(f, nu, d, v, z);
        for (int i = 0; i < nu; ++i)
            g[static_cast<size_t>(j) * nu + i] = d[static_cast<size_t>(i)];
    }
    for (int i = 0; i < nu; ++i) {
        for (int j = 0; j < nv; ++j)
            f[static_cast<size_t>(j)] = g[static_cast<size_t>(j) * nu + i];
        edt1d(f, nv, d, v, z);
        for (int j = 0; j < nv; ++j)
            g[static_cast<size_t>(j) * nu + i] = d[static_cast<size_t>(j)];
    }

    std::vector<float> out(g.size());
    for (size_t i = 0; i < g.size(); ++i)
        out[i] = g[i] >= 1.0e19 ? static_cast<float>(kFarDistance) : static_cast<float>(std::sqrt(g[i]) * spacing);
    return out;
}

float sampleGrid(const std::vector<float>& grid, int nu, int nv, double u0, double v0, double spacing,
                 double u, double v, float outside)
{
    const double fi = (u - u0) / spacing;
    const double fj = (v - v0) / spacing;
    const int i0 = static_cast<int>(std::floor(fi));
    const int j0 = static_cast<int>(std::floor(fj));
    if (i0 < 0 || j0 < 0 || i0 + 1 >= nu || j0 + 1 >= nv)
        return outside;
    const double tu = fi - i0;
    const double tv = fj - j0;
    const auto at = [&](int i, int j) { return static_cast<double>(grid[static_cast<size_t>(j) * nu + i]); };
    return static_cast<float>((at(i0, j0) * (1 - tu) + at(i0 + 1, j0) * tu) * (1 - tv) +
                              (at(i0, j0 + 1) * (1 - tu) + at(i0 + 1, j0 + 1) * tu) * tv);
}

// Background components not connected to the grid border that are smaller
// than maxAreaMm2 are filled; region components under the size fraction are
// removed.
void cleanRegion(std::vector<uint8_t>& region, int nu, int nv, double spacing)
{
    const size_t count = region.size();
    const double cellArea = spacing * spacing;
    std::vector<int> label(count, -1);
    std::vector<int> stack;

    // Background components (4-connected).
    int next = 0;
    for (size_t start = 0; start < count; ++start) {
        if (region[start] || label[start] >= 0)
            continue;
        std::vector<int> members;
        bool touchesBorder = false;
        stack.assign(1, static_cast<int>(start));
        label[start] = next;
        while (!stack.empty()) {
            const int idx = stack.back();
            stack.pop_back();
            members.push_back(idx);
            const int i = idx % nu;
            const int j = idx / nu;
            if (i == 0 || j == 0 || i == nu - 1 || j == nv - 1)
                touchesBorder = true;
            const int nbrs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (const auto& o : nbrs) {
                const int ni = i + o[0];
                const int nj = j + o[1];
                if (ni < 0 || nj < 0 || ni >= nu || nj >= nv)
                    continue;
                const int nIdx = nj * nu + ni;
                if (!region[static_cast<size_t>(nIdx)] && label[static_cast<size_t>(nIdx)] < 0) {
                    label[static_cast<size_t>(nIdx)] = next;
                    stack.push_back(nIdx);
                }
            }
        }
        if (!touchesBorder && static_cast<double>(members.size()) * cellArea < kMaxHoleAreaMm2) {
            for (int idx : members)
                region[static_cast<size_t>(idx)] = 1;
        }
        ++next;
    }

    // Region components (8-connected).
    std::fill(label.begin(), label.end(), -1);
    std::vector<size_t> sizes;
    for (size_t start = 0; start < count; ++start) {
        if (!region[start] || label[start] >= 0)
            continue;
        const int id = static_cast<int>(sizes.size());
        size_t size = 0;
        stack.assign(1, static_cast<int>(start));
        label[start] = id;
        while (!stack.empty()) {
            const int idx = stack.back();
            stack.pop_back();
            ++size;
            const int i = idx % nu;
            const int j = idx / nu;
            for (int dj = -1; dj <= 1; ++dj) {
                for (int di = -1; di <= 1; ++di) {
                    const int ni = i + di;
                    const int nj = j + dj;
                    if ((di == 0 && dj == 0) || ni < 0 || nj < 0 || ni >= nu || nj >= nv)
                        continue;
                    const int nIdx = nj * nu + ni;
                    if (region[static_cast<size_t>(nIdx)] && label[static_cast<size_t>(nIdx)] < 0) {
                        label[static_cast<size_t>(nIdx)] = id;
                        stack.push_back(nIdx);
                    }
                }
            }
        }
        sizes.push_back(size);
    }
    if (sizes.empty())
        return;
    const size_t largest = *std::max_element(sizes.begin(), sizes.end());
    for (size_t idx = 0; idx < count; ++idx) {
        if (region[idx] && static_cast<double>(sizes[static_cast<size_t>(label[idx])]) <
                               kMinComponentFraction * static_cast<double>(largest))
            region[idx] = 0;
    }
}

constexpr double kGuideSpanMarginMm = 6.0; // about half a molar: the last marked tooth stays whole
constexpr double kGuidePi = 3.14159265358979323846;

// Keeps the tooth cells whose direction from the guide-point centroid lies within the angular span of the
// guide points (plus a few millimetres), so the splint ends where the marked teeth end: scan behind the
// last marked teeth (tuberosity, retromolar pad, palate) does not enlarge it.
void restrictToGuideSpan(std::vector<uint8_t>& teeth, size_t& teethCells, const SplintHeightmapInputs& inputs,
                         const SplintOcclusalFrame& frame, int nu, int nv, double u0, double v0, double h)
{
    std::vector<SplintPointUV> guide;
    for (const auto* list : {&inputs.upperPoints, &inputs.lowerPoints})
        for (const SplintPoint3& p : *list) {
            const SplintPoint3 q = frame.ToLocal(p);
            guide.push_back({q[0], q[1]});
        }
    if (guide.size() < 3)
        return;
    double cu = 0.0, cv = 0.0;
    for (const SplintPointUV& g : guide) {
        cu += g[0] / static_cast<double>(guide.size());
        cv += g[1] / static_cast<double>(guide.size());
    }
    std::vector<double> angles;
    for (const SplintPointUV& g : guide)
        angles.push_back(std::atan2(g[1] - cv, g[0] - cu));
    std::sort(angles.begin(), angles.end());
    // The arch opening is the largest angular gap between consecutive points.
    double gap = angles.front() + 2.0 * kGuidePi - angles.back();
    double start = angles.front();
    for (size_t i = 1; i < angles.size(); ++i)
        if (angles[i] - angles[i - 1] > gap) {
            gap = angles[i] - angles[i - 1];
            start = angles[i];
        }
    if (gap < kGuidePi / 3.0)
        return; // points all around: no open end to limit
    const double span = 2.0 * kGuidePi - gap;
    size_t kept = 0;
    for (int j = 0; j < nv; ++j)
        for (int i = 0; i < nu; ++i) {
            const size_t idx = static_cast<size_t>(j) * nu + i;
            if (!teeth[idx])
                continue;
            const double du = u0 + i * h - cu;
            const double dv = v0 + j * h - cv;
            double offset = std::atan2(dv, du) - start;
            while (offset < 0.0)
                offset += 2.0 * kGuidePi;
            while (offset >= 2.0 * kGuidePi)
                offset -= 2.0 * kGuidePi;
            const double margin = kGuideSpanMarginMm / std::max(1.0, std::hypot(du, dv));
            if (offset <= span + margin || offset >= 2.0 * kGuidePi - margin)
                ++kept;
            else
                teeth[idx] = 0;
        }
    teethCells = kept;
}

double signedArea(const SplintContourUV& c)
{
    double a = 0.0;
    for (size_t i = 0; i < c.size(); ++i) {
        const UV& p = c[i];
        const UV& q = c[(i + 1) % c.size()];
        a += p[0] * q[1] - q[0] * p[1];
    }
    return 0.5 * a;
}

std::vector<SplintContourUV> extractContours(const std::vector<uint8_t>& region, int nu, int nv,
                                             double u0, double v0, double spacing)
{
    constexpr int pad = 3;
    const int nx = nu + 2 * pad;
    const int ny = nv + 2 * pad;
    std::vector<float> field(static_cast<size_t>(nx) * ny, 0.0f);
    for (int j = 0; j < nv; ++j)
        for (int i = 0; i < nu; ++i)
            field[static_cast<size_t>(j + pad) * nx + (i + pad)] = region[static_cast<size_t>(j) * nu + i] ? 1.0f : 0.0f;

    // Light separable blur (sigma = 1 cell) for sub-cell contour placement.
    const float kernel[7] = {0.00443f, 0.05400f, 0.24204f, 0.39905f, 0.24204f, 0.05400f, 0.00443f};
    std::vector<float> tmp(field.size(), 0.0f);
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            float s = 0.0f;
            for (int k = -3; k <= 3; ++k) {
                const int ii = std::clamp(i + k, 0, nx - 1);
                s += kernel[k + 3] * field[static_cast<size_t>(j) * nx + ii];
            }
            tmp[static_cast<size_t>(j) * nx + i] = s;
        }
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            float s = 0.0f;
            for (int k = -3; k <= 3; ++k) {
                const int jj = std::clamp(j + k, 0, ny - 1);
                s += kernel[k + 3] * tmp[static_cast<size_t>(jj) * nx + i];
            }
            field[static_cast<size_t>(j) * nx + i] = s;
        }

    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(nx, ny, 1);
    image->SetOrigin(u0 - pad * spacing, v0 - pad * spacing, 0.0);
    image->SetSpacing(spacing, spacing, 1.0);
    image->AllocateScalars(VTK_FLOAT, 1);
    std::copy(field.begin(), field.end(), static_cast<float*>(image->GetScalarPointer()));

    auto contour = vtkSmartPointer<vtkFlyingEdges2D>::New();
    contour->SetInputData(image);
    contour->SetValue(0, 0.5);
    contour->ComputeScalarsOff();
    auto stripper = vtkSmartPointer<vtkStripper>::New();
    stripper->SetInputConnection(contour->GetOutputPort());
    stripper->JoinContiguousSegmentsOn();
    stripper->SetMaximumLength(100000);
    stripper->Update();

    std::vector<SplintContourUV> contours;
    vtkPolyData* lines = stripper->GetOutput();
    auto it = vtk::TakeSmartPointer(lines->GetLines()->NewIterator());
    for (it->GoToFirstCell(); !it->IsDoneWithTraversal(); it->GoToNextCell()) {
        vtkIdType npts = 0;
        const vtkIdType* ids = nullptr;
        it->GetCurrentCell(npts, ids);
        SplintContourUV poly;
        double p[3] = {};
        for (vtkIdType k = 0; k < npts; ++k) {
            lines->GetPoint(ids[k], p);
            poly.push_back({p[0], p[1]});
        }
        if (poly.size() > 1 && distUV(poly.front(), poly.back()) < 1e-6)
            poly.pop_back();
        if (poly.size() < 8 || std::abs(signedArea(poly)) < 4.0 * spacing * spacing)
            continue;
        if (signedArea(poly) < 0.0)
            std::reverse(poly.begin(), poly.end());
        contours.push_back(std::move(poly));
    }
    return contours;
}

SplintContourUV resampleClosed(const SplintContourUV& poly, double spacing)
{
    const size_t n = poly.size();
    double perimeter = 0.0;
    for (size_t i = 0; i < n; ++i)
        perimeter += distUV(poly[i], poly[(i + 1) % n]);
    const size_t samples = std::max<size_t>(8, static_cast<size_t>(std::lround(perimeter / spacing)));
    const double step = perimeter / static_cast<double>(samples);

    SplintContourUV out;
    out.reserve(samples);
    size_t seg = 0;
    double segStart = 0.0;
    double segLen = distUV(poly[0], poly[1 % n]);
    for (size_t s = 0; s < samples; ++s) {
        const double target = step * static_cast<double>(s);
        while (segStart + segLen < target && seg + 1 < n) {
            segStart += segLen;
            ++seg;
            segLen = distUV(poly[seg], poly[(seg + 1) % n]);
        }
        const double t = segLen > 1e-12 ? std::clamp((target - segStart) / segLen, 0.0, 1.0) : 0.0;
        const UV& a = poly[seg];
        const UV& b = poly[(seg + 1) % n];
        out.push_back({a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t});
    }
    return out;
}

SplintContourUV convexHull(SplintContourUV pts)
{
    std::sort(pts.begin(), pts.end());
    if (pts.size() < 3)
        return pts;
    auto turn = [](const UV& o, const UV& a, const UV& b) {
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
    };
    SplintContourUV hull(2 * pts.size());
    size_t k = 0;
    for (size_t i = 0; i < pts.size(); ++i) {
        while (k >= 2 && turn(hull[k - 2], hull[k - 1], pts[i]) <= 0.0)
            --k;
        hull[k++] = pts[i];
    }
    for (size_t i = pts.size() - 1, lower = k + 1; i > 0; --i) {
        while (k >= lower && turn(hull[k - 2], hull[k - 1], pts[i - 1]) <= 0.0)
            --k;
        hull[k++] = pts[i - 1];
    }
    hull.resize(k - 1);
    return hull;
}

double distanceToSegment(const UV& p, const UV& a, const UV& b)
{
    const double du = b[0] - a[0];
    const double dv = b[1] - a[1];
    const double len2 = du * du + dv * dv;
    double t = len2 > 1e-18 ? ((p[0] - a[0]) * du + (p[1] - a[1]) * dv) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    return std::hypot(p[0] - (a[0] + du * t), p[1] - (a[1] + dv * t));
}

double distanceToClosedPolyline(const UV& p, const SplintContourUV& poly)
{
    double best = kFarDistance;
    for (size_t i = 0; i < poly.size(); ++i)
        best = std::min(best, distanceToSegment(p, poly[i], poly[(i + 1) % poly.size()]));
    return best;
}

struct ContourSmoothing
{
    double innerSigma = 30.0;
    double hullSigma = 15.0;
    double hullDistance = 1.5;
    double reduceThreshold = 1.5;
    double smoothThreshold = 0.2;
    double minToothDistance = 0.5;
};

void reduceRecurse(const SplintContourUV& pts, size_t a, size_t b, double tol, double maxLen,
                   std::vector<char>& keep)
{
    if (b <= a + 1)
        return;
    double maxDev = -1.0;
    size_t split = a + 1;
    for (size_t i = a + 1; i < b; ++i) {
        const double d = distanceToSegment(pts[i], pts[a], pts[b]);
        if (d > maxDev) {
            maxDev = d;
            split = i;
        }
    }
    const bool tooLong = distUV(pts[a], pts[b]) > maxLen;
    if (maxDev <= tol && !tooLong)
        return;
    if (maxDev <= tol)
        split = (a + b) / 2;
    keep[split] = 1;
    reduceRecurse(pts, a, split, tol, maxLen, keep);
    reduceRecurse(pts, split, b, tol, maxLen, keep);
}

SplintContourUV smoothContour(const SplintContourUV& raw, const ContourSmoothing& options,
                              const std::vector<float>& toothDistance, int nu, int nv,
                              double u0, double v0, double spacing)
{
    const SplintContourUV pts = resampleClosed(raw, kContourSampleMm);
    const size_t n = pts.size();
    const SplintContourUV hull = convexHull(pts);

    SplintContourUV smoothed(n);
    for (size_t i = 0; i < n; ++i) {
        const bool hullSide = hull.size() >= 3 && distanceToClosedPolyline(pts[i], hull) <= options.hullDistance;
        const double sigma = hullSide ? options.hullSigma : options.innerSigma;
        if (sigma < 0.5) {
            smoothed[i] = pts[i];
            continue;
        }
        const long radius = std::min<long>(static_cast<long>(std::ceil(3.0 * sigma)), static_cast<long>(n - 1) / 2);
        double su = 0.0, sv = 0.0, sw = 0.0;
        for (long k = -radius; k <= radius; ++k) {
            const double w = std::exp(-0.5 * static_cast<double>(k * k) / (sigma * sigma));
            const UV& q = pts[static_cast<size_t>((static_cast<long>(i) + k + static_cast<long>(n)) % static_cast<long>(n))];
            su += w * q[0];
            sv += w * q[1];
            sw += w;
        }
        smoothed[i] = {su / sw, sv / sw};
    }

    // Smoothing must not pull the border onto the teeth: move offending vertices
    // back toward their original position until the minimum margin holds.
    const float far = static_cast<float>(kFarDistance);
    for (size_t i = 0; i < n; ++i) {
        const auto distAt = [&](const UV& p) {
            return sampleGrid(toothDistance, nu, nv, u0, v0, spacing, p[0], p[1], far);
        };
        if (distAt(smoothed[i]) >= options.minToothDistance || distAt(pts[i]) < options.minToothDistance)
            continue;
        UV bad = smoothed[i];
        UV good = pts[i];
        for (int iter = 0; iter < 10; ++iter) {
            const UV mid{0.5 * (bad[0] + good[0]), 0.5 * (bad[1] + good[1])};
            if (distAt(mid) >= options.minToothDistance)
                good = mid;
            else
                bad = mid;
        }
        smoothed[i] = good;
    }

    SplintContourUV unrolled = smoothed;
    unrolled.push_back(smoothed.front());
    std::vector<char> keep(unrolled.size(), 0);
    const size_t half = n / 2;
    keep[0] = keep[half] = keep[n] = 1;
    reduceRecurse(unrolled, 0, half, options.smoothThreshold, options.reduceThreshold, keep);
    reduceRecurse(unrolled, half, n, options.smoothThreshold, options.reduceThreshold, keep);

    SplintContourUV reduced;
    for (size_t i = 0; i < n; ++i)
        if (keep[i])
            reduced.push_back(smoothed[i]);
    return reduced;
}

vtkIdType openEdgeCount(vtkPolyData* mesh)
{
    auto edges = vtkSmartPointer<vtkFeatureEdges>::New();
    edges->SetInputData(mesh);
    edges->BoundaryEdgesOn();
    edges->NonManifoldEdgesOn();
    edges->FeatureEdgesOff();
    edges->ManifoldEdgesOff();
    edges->Update();
    return edges->GetOutput()->GetNumberOfCells();
}

double trilinear(const double* data, int nx, int ny, int nz, double x, double y, double z)
{
    x = std::clamp(x, 0.0, nx - 1.000001);
    y = std::clamp(y, 0.0, ny - 1.000001);
    z = std::clamp(z, 0.0, nz - 1.000001);
    const int i = static_cast<int>(x);
    const int j = static_cast<int>(y);
    const int k = static_cast<int>(z);
    const double tx = x - i, ty = y - j, tz = z - k;
    const auto at = [&](int a, int b, int c) {
        return data[static_cast<size_t>(a) + static_cast<size_t>(nx) * (static_cast<size_t>(b) + static_cast<size_t>(ny) * c)];
    };
    const double c00 = at(i, j, k) * (1 - tx) + at(i + 1, j, k) * tx;
    const double c10 = at(i, j + 1, k) * (1 - tx) + at(i + 1, j + 1, k) * tx;
    const double c01 = at(i, j, k + 1) * (1 - tx) + at(i + 1, j, k + 1) * tx;
    const double c11 = at(i, j + 1, k + 1) * (1 - tx) + at(i + 1, j + 1, k + 1) * tx;
    return (c00 * (1 - ty) + c10 * ty) * (1 - tz) + (c01 * (1 - ty) + c11 * ty) * tz;
}

double percentileOf(std::vector<float> values, double p)
{
    if (values.empty())
        return 0.0;
    const size_t idx = static_cast<size_t>(std::clamp(p, 0.0, 1.0) * static_cast<double>(values.size() - 1));
    std::nth_element(values.begin(), values.begin() + static_cast<long>(idx), values.end());
    return values[idx];
}

struct DiskOffset
{
    int di;
    int dj;
    float lift;
};

// Grid offsets inside a disk; with sphereLift each offset carries the height
// of a ball of that radius, so filtering with it offsets a height field by a sphere.
std::vector<DiskOffset> diskOffsets(double radiusMm, double spacing, bool sphereLift)
{
    std::vector<DiskOffset> offsets;
    const double radius = std::max(0.0, radiusMm);
    const int r = static_cast<int>(std::floor(radius / spacing + 1e-9));
    for (int dj = -r; dj <= r; ++dj)
        for (int di = -r; di <= r; ++di) {
            const double d = std::hypot(di, dj) * spacing;
            if (d > radius + 1e-9)
                continue;
            const float lift = sphereLift ? static_cast<float>(std::sqrt(std::max(0.0, radius * radius - d * d))) : 0.0f;
            offsets.push_back({di, dj, lift});
        }
    return offsets;
}

// takeMin: out(p) = min_q v(q) - lift; otherwise out(p) = max_q v(q) + lift.
std::vector<float> diskFilter(const std::vector<float>& in, int nu, int nv,
                              const std::vector<DiskOffset>& offsets, bool takeMin)
{
    std::vector<float> out(in.size());
    const float start = takeMin ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity();
    parallelFor(nv, [&](int jBegin, int jEnd) {
        for (int j = jBegin; j < jEnd; ++j)
            for (int i = 0; i < nu; ++i) {
                float best = start;
                for (const DiskOffset& o : offsets) {
                    const int ni = i + o.di;
                    const int nj = j + o.dj;
                    if (ni < 0 || nj < 0 || ni >= nu || nj >= nv)
                        continue;
                    const float value = in[static_cast<size_t>(nj) * nu + ni];
                    best = takeMin ? std::min(best, value - o.lift) : std::max(best, value + o.lift);
                }
                out[static_cast<size_t>(j) * nu + i] = best;
            }
    });
    return out;
}

struct LocalMark
{
    Vec3 center;
    double radius;
};

// Height field used for the solid: fins narrower than minFeatureMm removed
// (opening of the upper map, closing of the lower map; both never move toward
// the teeth) and then offset by a clearance sphere. Surface within a bracket
// mark is offset by clearance + markOffsetMm instead.
std::vector<float> solidHeights(const SplintHeightMap& map, bool upperJaw, double minFeatureMm, double clearanceMm,
                                const std::vector<LocalMark>& marks, double markOffsetMm)
{
    const float missing = upperJaw ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity();
    std::vector<float> values(map.values.size());
    for (size_t i = 0; i < values.size(); ++i)
        values[i] = std::isfinite(map.values[i]) ? map.values[i] : missing;

    const double featureRadius = 0.5 * std::clamp(minFeatureMm, 0.0, 1.0);
    if (featureRadius >= 0.5 * map.spacing) {
        const auto disk = diskOffsets(featureRadius, map.spacing, false);
        values = diskFilter(diskFilter(values, map.nu, map.nv, disk, upperJaw), map.nu, map.nv, disk, !upperJaw);
    }
    std::vector<float> filtered = diskFilter(values, map.nu, map.nv, diskOffsets(clearanceMm, map.spacing, true), upperJaw);

    if (!marks.empty() && markOffsetMm > 0.0) {
        const double sigma = upperJaw ? 1.0 : -1.0;
        std::vector<float> marked(values.size(), missing);
        std::atomic<bool> any{false};
        parallelFor(map.nv, [&](int jBegin, int jEnd) {
            for (int j = jBegin; j < jEnd; ++j)
                for (int i = 0; i < map.nu; ++i) {
                    const size_t idx = static_cast<size_t>(j) * map.nu + i;
                    const float w = values[idx];
                    if (!std::isfinite(w))
                        continue;
                    const Vec3 p{map.u0 + i * map.spacing + sigma * w * map.shear[0],
                                 map.v0 + j * map.spacing + sigma * w * map.shear[1], w};
                    for (const LocalMark& mark : marks) {
                        const Vec3 d = sub(p, mark.center);
                        if (dot(d, d) <= mark.radius * mark.radius) {
                            marked[idx] = w;
                            any.store(true, std::memory_order_relaxed);
                            break;
                        }
                    }
                }
        });
        if (any.load()) {
            const std::vector<float> offsetValues = diskFilter(
                marked, map.nu, map.nv, diskOffsets(clearanceMm + markOffsetMm, map.spacing, true), upperJaw);
            for (size_t idx = 0; idx < filtered.size(); ++idx)
                filtered[idx] = upperJaw ? std::min(filtered[idx], offsetValues[idx])
                                         : std::max(filtered[idx], offsetValues[idx]);
        }
    }
    values = std::move(filtered);

    for (float& v : values)
        if (!std::isfinite(v))
            v = kNaN;
    return values;
}

UV shearFor(bool enabled, double angleDeg, const UV& direction)
{
    if (!enabled)
        return {0.0, 0.0};
    const double len = std::hypot(direction[0], direction[1]);
    const UV dir = len > 1e-9 ? UV{direction[0] / len, direction[1] / len} : UV{0.0, 1.0};
    const double t = std::tan(std::clamp(angleDeg, 0.0, kMaxUndercutAngleDeg) * kPi / 180.0);
    return {dir[0] * t, dir[1] * t};
}
} // namespace

// ─────────────────────────────────────────────────────────────────────────────

SplintPoint3 SplintOcclusalFrame::ToWorld(double u, double v, double w) const
{
    return add(origin, add(mul(axisU, u), add(mul(axisV, v), mul(normal, w))));
}

SplintPoint3 SplintOcclusalFrame::ToLocal(const SplintPoint3& world) const
{
    const Vec3 d = sub(world, origin);
    return {dot(d, axisU), dot(d, axisV), dot(d, normal)};
}

vtkSmartPointer<vtkMatrix4x4> SplintOcclusalFrame::LocalToWorldMatrix() const
{
    auto m = vtkSmartPointer<vtkMatrix4x4>::New();
    m->Identity();
    for (int r = 0; r < 3; ++r) {
        m->SetElement(r, 0, axisU[static_cast<size_t>(r)]);
        m->SetElement(r, 1, axisV[static_cast<size_t>(r)]);
        m->SetElement(r, 2, normal[static_cast<size_t>(r)]);
        m->SetElement(r, 3, origin[static_cast<size_t>(r)]);
    }
    return m;
}

double SplintBoundarySurface::Evaluate(double u, double v) const
{
    const auto& c = coefficients;
    double w = c[0] + c[1] * u + c[2] * v;
    if (quadratic)
        w += c[3] * u * u + c[4] * u * v + c[5] * v * v;
    return std::clamp(w, minW, maxW);
}

float SplintHeightMap::Sample(double cu, double cv) const
{
    if (values.empty() || spacing <= 0.0)
        return kNaN;
    const double fi = (cu - u0) / spacing;
    const double fj = (cv - v0) / spacing;
    const int i0 = static_cast<int>(std::floor(fi));
    const int j0 = static_cast<int>(std::floor(fj));
    if (i0 >= 0 && j0 >= 0 && i0 + 1 < nu && j0 + 1 < nv) {
        const float a = At(i0, j0), b = At(i0 + 1, j0), c = At(i0, j0 + 1), d = At(i0 + 1, j0 + 1);
        if (std::isfinite(a) && std::isfinite(b) && std::isfinite(c) && std::isfinite(d)) {
            const double tu = fi - i0;
            const double tv = fj - j0;
            return static_cast<float>((a * (1 - tu) + b * tu) * (1 - tv) + (c * (1 - tu) + d * tu) * tv);
        }
    }
    const long i = std::lround(fi);
    const long j = std::lround(fj);
    if (i < 0 || j < 0 || i >= nu || j >= nv)
        return kNaN;
    return At(static_cast<int>(i), static_cast<int>(j));
}

double SplintHeightmapGenerator::ContourArea(const SplintContourUV& contour)
{
    return std::abs(signedArea(contour));
}

namespace
{
// Horizontal direction from the frame origin toward `point` and the arch
// tangent there (normal × radial).
void archDirections(const SplintOcclusalFrame& frame, const Vec3& point, Vec3& radial, Vec3& tangent)
{
    Vec3 r = sub(point, frame.origin);
    r = sub(r, mul(frame.normal, dot(r, frame.normal)));
    radial = normalized(r, frame.axisV);
    tangent = normalized(cross(frame.normal, radial), frame.axisU);
}
} // namespace

bool SplintHeightmapGenerator::BevelPlane(const SplintBevel& bevel, const SplintOcclusalFrame& frame,
                                          SplintPoint3& origin, SplintPoint3& normal)
{
    const Vec3 line = sub(bevel.second, bevel.first);
    const double length = std::sqrt(dot(line, line));
    if (length < 0.5)
        return false;
    const Vec3 mid = mul(add(bevel.first, bevel.second), 0.5);
    Vec3 radial{}, tangent{};
    archDirections(frame, mid, radial, tangent);
    Vec3 n = cross(line, tangent);
    const double nLength = std::sqrt(dot(n, n));
    if (nLength < 0.2 * length) // the line runs along the arch: no plane
        return false;
    n = mul(n, 1.0 / nLength);
    if (dot(n, radial) < 0.0)
        n = mul(n, -1.0);
    origin = mid;
    normal = n;
    return true;
}

SplintPoint3 SplintHeightmapGenerator::WireHoleAxis(SplintHoleOrientation orientation, const SplintPoint3& surfaceNormal,
                                                    const std::optional<SplintBevel>& bevel,
                                                    const SplintOcclusalFrame& frame)
{
    Vec3 axis = normalized(surfaceNormal, frame.normal);
    Vec3 origin{}, bevelNormal{};
    if (orientation == SplintHoleOrientation::Bevel && bevel && BevelPlane(*bevel, frame, origin, bevelNormal)) {
        Vec3 radial{}, tangent{};
        archDirections(frame, origin, radial, tangent);
        axis = normalized(cross(tangent, bevelNormal), frame.normal);
    }
    if (dot(axis, frame.normal) < 0.0)
        axis = mul(axis, -1.0);
    return axis;
}

void SplintHeightmapGenerator::ApplyThicknessColors(vtkPolyData* mesh, double minMm, double maxMm)
{
    if (!mesh)
        return;
    auto* thickness = vtkFloatArray::SafeDownCast(mesh->GetPointData()->GetArray(ThicknessArrayName));
    if (!thickness)
        return;
    auto colors = vtkSmartPointer<vtkUnsignedCharArray>::New();
    colors->SetName(ThicknessColorArrayName);
    colors->SetNumberOfComponents(3);
    colors->SetNumberOfTuples(thickness->GetNumberOfTuples());
    for (vtkIdType i = 0; i < thickness->GetNumberOfTuples(); ++i) {
        const double t = thickness->GetValue(i);
        const unsigned char red[3] = {230, 40, 40};
        const unsigned char yellow[3] = {240, 210, 40};
        const unsigned char purple[3] = {140, 60, 200};
        const unsigned char* c = t < minMm ? red : (t > maxMm ? purple : yellow);
        colors->SetTypedTuple(i, c);
    }
    mesh->GetPointData()->SetScalars(colors);
}

SplintHeightmapPrepared SplintHeightmapGenerator::Prepare(const SplintHeightmapInputs& inputs)
{
    QElapsedTimer timer;
    timer.start();
    SplintHeightmapPrepared prep;
    prep.params = inputs.params;

    if (!inputs.upperTeeth || !inputs.lowerTeeth ||
        inputs.upperTeeth->GetNumberOfPolys() <= 0 || inputs.lowerTeeth->GetNumberOfPolys() <= 0) {
        prep.error = QStringLiteral("Faltan las mallas de los dientes superiores o inferiores.");
        return prep;
    }
    if (inputs.upperPoints.size() < 3) {
        prep.error = QStringLiteral("Faltan puntos superiores: se necesitan al menos 3 (hay %1).")
                         .arg(inputs.upperPoints.size());
        return prep;
    }
    if (inputs.lowerPoints.size() < 3) {
        prep.error = QStringLiteral("Faltan puntos inferiores: se necesitan al menos 3 (hay %1).")
                         .arg(inputs.lowerPoints.size());
        return prep;
    }
    const double spacing = inputs.params.gridResolutionMm;
    if (!(spacing >= 0.05 && spacing <= 2.0)) {
        prep.error = QStringLiteral("La resolución de la rejilla debe estar entre 0.05 y 2 mm.");
        return prep;
    }

    SplintOcclusalFrame frame;
    if (!computeInitialFrame(inputs, frame, &prep.error))
        return prep;

    auto fitSurfaces = [&](const SplintOcclusalFrame& f) {
        std::vector<Vec3> upperLocal, lowerLocal;
        for (const Vec3& p : inputs.upperPoints)
            upperLocal.push_back(f.ToLocal(p));
        for (const Vec3& p : inputs.lowerPoints)
            lowerLocal.push_back(f.ToLocal(p));
        if (!fitBoundarySurface(upperLocal, prep.upperSurface)) {
            prep.error = QStringLiteral("Los puntos superiores están alineados; repártalos a lo largo de la arcada.");
            return false;
        }
        if (!fitBoundarySurface(lowerLocal, prep.lowerSurface)) {
            prep.error = QStringLiteral("Los puntos inferiores están alineados; repártalos a lo largo de la arcada.");
            return false;
        }
        return true;
    };
    if (!fitSurfaces(frame))
        return prep;

    const auto upperTri = triangulated(inputs.upperTeeth);
    const auto lowerTri = triangulated(inputs.lowerTeeth);

    BandScan upperScan = scanBand(upperTri, frame, prep.lowerSurface, prep.upperSurface);
    BandScan lowerScan = scanBand(lowerTri, frame, prep.lowerSurface, prep.upperSurface);
    if (upperScan.bandCells == 0 && lowerScan.bandCells == 0) {
        prep.error = QStringLiteral("Los dientes no cruzan la franja entre los puntos superiores e inferiores.");
        return prep;
    }
    if (isCancelled(inputs)) {
        prep.error = cancelledMessage();
        return prep;
    }

    std::vector<UV> samples = upperScan.samples;
    samples.insert(samples.end(), lowerScan.samples.begin(), lowerScan.samples.end());
    resolveAnterior(frame, inputs, samples);
    if (!fitSurfaces(frame))
        return prep;
    upperScan = scanBand(upperTri, frame, prep.lowerSurface, prep.upperSurface);
    lowerScan = scanBand(lowerTri, frame, prep.lowerSurface, prep.upperSurface);
    prep.frame = frame;

    const UV upperShear = shearFor(inputs.params.undercutUpper, inputs.params.undercutAngleUpperDeg,
                                   inputs.params.undercutDirectionUV);
    const UV lowerShear = shearFor(inputs.params.undercutLower, inputs.params.undercutAngleLowerDeg,
                                   inputs.params.undercutDirectionUV);

    double uMin = std::min(upperScan.uMin, lowerScan.uMin);
    double uMax = std::max(upperScan.uMax, lowerScan.uMax);
    double vMin = std::min(upperScan.vMin, lowerScan.vMin);
    double vMax = std::max(upperScan.vMax, lowerScan.vMax);
    for (const auto* list : {&inputs.upperPoints, &inputs.lowerPoints}) {
        for (const Vec3& p : *list) {
            const Vec3 q = frame.ToLocal(p);
            uMin = std::min(uMin, q[0]);
            uMax = std::max(uMax, q[0]);
            vMin = std::min(vMin, q[1]);
            vMax = std::max(vMax, q[1]);
        }
    }
    const double wExtent = std::max({10.0, std::abs(prep.upperSurface.maxW), std::abs(prep.lowerSurface.minW)});
    const double maxShear = std::max(std::hypot(upperShear[0], upperShear[1]), std::hypot(lowerShear[0], lowerShear[1]));
    const double margin = kGridMarginMm + maxShear * wExtent;
    uMin -= margin;
    uMax += margin;
    vMin -= margin;
    vMax += margin;

    const double u0 = std::floor(uMin / spacing) * spacing + 0.5 * spacing;
    const double v0 = std::floor(vMin / spacing) * spacing + 0.5 * spacing;
    const int nu = static_cast<int>(std::ceil((uMax - u0) / spacing)) + 1;
    const int nv = static_cast<int>(std::ceil((vMax - v0) / spacing)) + 1;
    if (static_cast<size_t>(nu) * static_cast<size_t>(nv) > kMaxGridCells) {
        prep.error = QStringLiteral("La rejilla del mapa de altura es demasiado grande (%1 × %2); aumente la resolución.")
                         .arg(nu).arg(nv);
        return prep;
    }

    // Rays only need to reach the clamp limit beyond the point surfaces; geometry
    // past it (bone of a composite) and outside the ray footprint is cropped.
    const double upperLimit = prep.upperSurface.maxW + kCropMarginMm;
    const double lowerLimit = prep.lowerSurface.minW - kCropMarginMm;
    const double upperStart = upperScan.wMin - 1.0;
    const double upperEnd = std::min(upperScan.wMax, upperLimit) + 1.0;
    const double lowerStart = lowerScan.wMax + 1.0;
    const double lowerEnd = std::max(lowerScan.wMin, lowerLimit) - 1.0;
    const double gridUMax = u0 + (nu - 1) * spacing;
    const double gridVMax = v0 + (nv - 1) * spacing;
    const auto cropJaw = [&](vtkPolyData* mesh, bool upperJaw, const UV& shear, double wLimit, double wA, double wB) {
        const double reach = std::max(std::abs(wA), std::abs(wB));
        const double du = std::abs(shear[0]) * reach + 1.0;
        const double dv = std::abs(shear[1]) * reach + 1.0;
        return cropForRays(mesh, frame, upperJaw, wLimit, u0 - du, gridUMax + du, v0 - dv, gridVMax + dv);
    };
    const auto upperRays = cropJaw(upperTri, true, upperShear, upperLimit, upperStart, upperEnd);
    const auto lowerRays = cropJaw(lowerTri, false, lowerShear, lowerLimit, lowerStart, lowerEnd);
    prep.upperTrianglesTotal = upperTri->GetNumberOfPolys();
    prep.upperTrianglesUsed = upperRays->GetNumberOfPolys();
    prep.lowerTrianglesTotal = lowerTri->GetNumberOfPolys();
    prep.lowerTrianglesUsed = lowerRays->GetNumberOfPolys();

    const qint64 setupMs = timer.elapsed();
    prep.upperMap = castHeightMap(upperRays, frame, nu, nv, u0, v0, spacing, upperShear, true,
                                  upperStart, upperEnd, inputs);
    if (isCancelled(inputs)) {
        prep.error = cancelledMessage();
        return prep;
    }
    prep.lowerMap = castHeightMap(lowerRays, frame, nu, nv, u0, v0, spacing, lowerShear, false,
                                  lowerStart, lowerEnd, inputs);
    if (isCancelled(inputs)) {
        prep.error = cancelledMessage();
        return prep;
    }

    prep.report = QStringLiteral("Mapas de altura: %1 × %2 celdas a %3 mm (preparación %4 ms, rayos %5 ms). "
                                 "Superficies: superior %6, inferior %7. Dirección anterior %8.")
                      .arg(nu).arg(nv).arg(spacing, 0, 'f', 2)
                      .arg(setupMs).arg(timer.elapsed() - setupMs)
                      .arg(prep.upperSurface.quadratic ? QStringLiteral("cuadrática") : QStringLiteral("plana"))
                      .arg(prep.lowerSurface.quadratic ? QStringLiteral("cuadrática") : QStringLiteral("plana"))
                      .arg(frame.anteriorResolved ? QStringLiteral("resuelta") : QStringLiteral("no resuelta (eje PCA)"));
    prep.report += QStringLiteral(" Triángulos usados: superior %1 de %2, inferior %3 de %4.")
                       .arg(prep.upperTrianglesUsed).arg(prep.upperTrianglesTotal)
                       .arg(prep.lowerTrianglesUsed).arg(prep.lowerTrianglesTotal);
    if (upperScan.bandCells == 0)
        prep.report += QStringLiteral(" Advertencia: los dientes superiores no cruzan la franja.");
    if (lowerScan.bandCells == 0)
        prep.report += QStringLiteral(" Advertencia: los dientes inferiores no cruzan la franja.");
    prep.ok = true;
    return prep;
}

SplintHeightmapResult SplintHeightmapGenerator::Build(const SplintHeightmapPrepared& prep,
                                                      const SplintHeightmapInputs& inputs)
{
    QElapsedTimer timer;
    timer.start();
    SplintHeightmapResult result;
    result.frame = prep.frame;
    if (!prep.ok) {
        result.error = prep.error.isEmpty() ? QStringLiteral("Los mapas de altura no están preparados.") : prep.error;
        return result;
    }

    const SplintHeightmapParams& params = inputs.params;
    const SplintHeightMap& upperMap = prep.upperMap;
    const SplintHeightMap& lowerMap = prep.lowerMap;
    const int nu = upperMap.nu;
    const int nv = upperMap.nv;
    const double h = upperMap.spacing;
    const double u0 = upperMap.u0;
    const double v0 = upperMap.v0;
    const size_t cells = static_cast<size_t>(nu) * static_cast<size_t>(nv);
    const double edgeOffset = std::clamp(params.edgeOffsetMm, 0.0, 5.0);
    const double clearance = std::clamp(params.clearanceMm, 0.0, kMaxClearanceMm);

    // ── Extras in the local frame ─────────────────────────────────────────
    const SplintExtras& extras = inputs.extras;
    const SplintOcclusalFrame& frame = prep.frame;
    const auto toLocalDirection = [&frame](const Vec3& d) {
        return Vec3{dot(d, frame.axisU), dot(d, frame.axisV), dot(d, frame.normal)};
    };
    const double bracketOffset = std::clamp(extras.bracketOffsetMm, 0.0, MaxBracketOffsetMm);
    std::vector<LocalMark> upperMarks, lowerMarks;
    for (const SplintBracketMark& mark : extras.bracketMarks) {
        if (!(mark.radiusMm > 0.0))
            continue;
        const Vec3 local = frame.ToLocal(mark.center);
        (local[2] >= 0.0 ? upperMarks : lowerMarks).push_back({local, mark.radiusMm});
    }
    bool hasBevel = false;
    bool bevelIgnored = false;
    Vec3 bevelOrigin{}, bevelNormal{};
    if (extras.bevel) {
        Vec3 origin{}, normal{};
        if (BevelPlane(*extras.bevel, frame, origin, normal)) {
            bevelOrigin = frame.ToLocal(origin);
            bevelNormal = toLocalDirection(normal);
            hasBevel = true;
        } else {
            bevelIgnored = true;
        }
    }
    struct LocalHole
    {
        Vec3 center;
        Vec3 axis;
        double radius;
    };
    std::vector<LocalHole> holes;
    for (const SplintWireHole& hole : extras.wireHoles) {
        if (!(hole.diameterMm > 0.0))
            continue;
        holes.push_back({frame.ToLocal(hole.center), normalized(toLocalDirection(hole.axis), {0.0, 0.0, 1.0}),
                         0.5 * hole.diameterMm});
    }

    std::vector<float> surfSup(cells), surfInf(cells);
    for (int j = 0; j < nv; ++j)
        for (int i = 0; i < nu; ++i) {
            const size_t idx = static_cast<size_t>(j) * nu + i;
            surfSup[idx] = static_cast<float>(prep.upperSurface.Evaluate(u0 + i * h, v0 + j * h));
            surfInf[idx] = static_cast<float>(prep.lowerSurface.Evaluate(u0 + i * h, v0 + j * h));
        }

    // Upper-tooth surface in the column that passes through local (u, v, w).
    const auto upperHit = [&](double u, double v, double w) {
        return upperMap.Sample(u - w * upperMap.shear[0], v - w * upperMap.shear[1]);
    };
    const auto lowerHit = [&](double u, double v, double w) {
        return lowerMap.Sample(u + w * lowerMap.shear[0], v + w * lowerMap.shear[1]);
    };

    // ── Contour ───────────────────────────────────────────────────────────
    if (!inputs.contourOverrideUV.empty()) {
        for (const SplintContourUV& c : inputs.contourOverrideUV) {
            if (c.size() < 3) {
                result.error = QStringLiteral("El contorno editado tiene menos de 3 vértices.");
                return result;
            }
        }
        result.contoursUV = inputs.contourOverrideUV;
    } else {
        std::vector<uint8_t> teeth(cells, 0);
        size_t teethCells = 0;
        for (int j = 0; j < nv; ++j)
            for (int i = 0; i < nu; ++i) {
                const size_t idx = static_cast<size_t>(j) * nu + i;
                const double u = u0 + i * h;
                const double v = v0 + j * h;
                const float hs = upperHit(u, v, surfSup[idx]);
                const float hi = lowerHit(u, v, surfInf[idx]);
                if ((std::isfinite(hs) && hs < surfSup[idx]) || (std::isfinite(hi) && hi > surfInf[idx])) {
                    teeth[idx] = 1;
                    ++teethCells;
                }
            }
        restrictToGuideSpan(teeth, teethCells, inputs, frame, nu, nv, u0, v0, h);
        if (teethCells == 0) {
            result.error = QStringLiteral("Los dientes no cruzan la franja entre los puntos superiores e inferiores.");
            return result;
        }

        const std::vector<float> toothDistance = distanceField(teeth, nu, nv, h);
        std::vector<uint8_t> region(cells, 0);
        for (size_t idx = 0; idx < cells; ++idx)
            region[idx] = toothDistance[idx] <= edgeOffset + 1e-6 ? 1 : 0;

        const double closingRadius = 2.0 * std::max(0.0, params.convexHullFixDistance);
        if (closingRadius > 0.0) {
            const std::vector<float> toRegion = distanceField(region, nu, nv, h);
            std::vector<uint8_t> background(cells, 0);
            for (int j = 0; j < nv; ++j)
                for (int i = 0; i < nu; ++i) {
                    const size_t idx = static_cast<size_t>(j) * nu + i;
                    const bool border = i == 0 || j == 0 || i == nu - 1 || j == nv - 1;
                    background[idx] = (toRegion[idx] > closingRadius || border) ? 1 : 0;
                }
            const std::vector<float> toBackground = distanceField(background, nu, nv, h);
            for (size_t idx = 0; idx < cells; ++idx)
                if (toBackground[idx] > closingRadius)
                    region[idx] = 1;
        }
        cleanRegion(region, nu, nv, h);
        if (isCancelled(inputs)) {
            result.error = cancelledMessage();
            return result;
        }

        ContourSmoothing smoothing;
        smoothing.innerSigma = std::max(0.0, params.innerSmoothSigma);
        smoothing.hullSigma = std::max(0.0, params.convexHullSmoothSigma * params.roundingFactorForSplintSide);
        smoothing.hullDistance = std::max(0.0, params.convexHullFixDistance);
        smoothing.reduceThreshold = std::max(kContourSampleMm, params.reduceThreshold);
        smoothing.smoothThreshold = std::max(0.01, params.smoothThreshold);
        smoothing.minToothDistance = 0.5 * edgeOffset;
        for (const SplintContourUV& raw : extractContours(region, nu, nv, u0, v0, h)) {
            SplintContourUV smooth = smoothContour(raw, smoothing, toothDistance, nu, nv, u0, v0, h);
            if (smooth.size() >= 3)
                result.contoursUV.push_back(std::move(smooth));
        }
        if (result.contoursUV.empty()) {
            result.error = QStringLiteral("No se pudo extraer el contorno de la férula.");
            return result;
        }
    }
    const qint64 contourMs = timer.elapsed();

    // ── Signed distance to the contour (negative inside) ──────────────────
    double cuMin = kFarDistance, cuMax = -kFarDistance, cvMin = kFarDistance, cvMax = -kFarDistance;
    for (const auto& c : result.contoursUV)
        for (const UV& p : c) {
            cuMin = std::min(cuMin, p[0]);
            cuMax = std::max(cuMax, p[0]);
            cvMin = std::min(cvMin, p[1]);
            cvMax = std::max(cvMax, p[1]);
        }
    std::vector<float> signedDist(cells, static_cast<float>(kFarDistance));
    parallelFor(nv, [&](int jBegin, int jEnd) {
        for (int j = jBegin; j < jEnd; ++j) {
            const double v = v0 + j * h;
            if (v < cvMin - 3 * h || v > cvMax + 3 * h)
                continue;
            for (int i = 0; i < nu; ++i) {
                const double u = u0 + i * h;
                if (u < cuMin - 3 * h || u > cuMax + 3 * h)
                    continue;
                const UV p{u, v};
                double best = kFarDistance;
                bool inside = false;
                for (const auto& c : result.contoursUV) {
                    for (size_t k = 0, m = c.size() - 1; k < c.size(); m = k++) {
                        const UV& a = c[k];
                        const UV& b = c[m];
                        if ((a[1] > v) != (b[1] > v) && u < (b[0] - a[0]) * (v - a[1]) / (b[1] - a[1]) + a[0])
                            inside = !inside;
                        best = std::min(best, distanceToSegment(p, a, b));
                    }
                }
                signedDist[static_cast<size_t>(j) * nu + i] = static_cast<float>(inside ? -best : best);
            }
        }
    });

    SplintHeightMap upperSolid = upperMap;
    upperSolid.values = solidHeights(upperMap, true, params.minFeatureMm, clearance, upperMarks, bracketOffset);
    SplintHeightMap lowerSolid = lowerMap;
    lowerSolid.values = solidHeights(lowerMap, false, params.minFeatureMm, clearance, lowerMarks, bracketOffset);
    const auto upperSolidHit = [&](double u, double v, double w) {
        return upperSolid.Sample(u - w * upperSolid.shear[0], v - w * upperSolid.shear[1]);
    };
    const auto lowerSolidHit = [&](double u, double v, double w) {
        return lowerSolid.Sample(u + w * lowerSolid.shear[0], v + w * lowerSolid.shear[1]);
    };

    // Without impression a side is the point surface lowered (or raised) until
    // it touches the clearance-offset cusps inside the contour.
    double flatDropUpper = 0.0;
    double flatRiseLower = 0.0;
    for (int j = 0; j < nv; ++j)
        for (int i = 0; i < nu; ++i) {
            const size_t idx = static_cast<size_t>(j) * nu + i;
            if (signedDist[idx] > h)
                continue;
            const double u = u0 + i * h;
            const double v = v0 + j * h;
            const float hs = upperSolidHit(u, v, surfSup[idx]);
            if (std::isfinite(hs))
                flatDropUpper = std::max(flatDropUpper, static_cast<double>(surfSup[idx] - hs));
            const float hi = lowerSolidHit(u, v, surfInf[idx]);
            if (std::isfinite(hi))
                flatRiseLower = std::max(flatRiseLower, static_cast<double>(hi - surfInf[idx]));
        }

    // Top / bottom of the splint at local (u, v, w).
    const auto topAt = [&](size_t idx, double u, double v, double w) {
        if (!params.impressionUpper)
            return static_cast<double>(surfSup[idx]) - flatDropUpper;
        double top = surfSup[idx];
        const float hs = upperSolidHit(u, v, w);
        if (std::isfinite(hs))
            top = std::min(top, static_cast<double>(hs));
        return top;
    };
    const auto bottomAt = [&](size_t idx, double u, double v, double w) {
        if (!params.impressionLower)
            return static_cast<double>(surfInf[idx]) + flatRiseLower;
        double bottom = surfInf[idx];
        const float hi = lowerSolidHit(u, v, w);
        if (std::isfinite(hi))
            bottom = std::max(bottom, static_cast<double>(hi));
        return bottom;
    };

    double wLo = kFarDistance, wHi = -kFarDistance;
    size_t perforatedCells = 0;
    for (int j = 0; j < nv; ++j)
        for (int i = 0; i < nu; ++i) {
            const size_t idx = static_cast<size_t>(j) * nu + i;
            if (signedDist[idx] > h)
                continue;
            wLo = std::min(wLo, static_cast<double>(surfInf[idx]));
            wHi = std::max(wHi, static_cast<double>(surfSup[idx]));
            if (signedDist[idx] < 0.0f) {
                const double u = u0 + i * h;
                const double v = v0 + j * h;
                const double mid = 0.5 * (surfSup[idx] + surfInf[idx]);
                if (topAt(idx, u, v, mid) <= bottomAt(idx, u, v, mid))
                    ++perforatedCells;
            }
        }
    result.perforatedAreaMm2 = static_cast<double>(perforatedCells) * h * h;
    if (!(wHi - wLo > h)) {
        result.error = QStringLiteral("No hay espacio entre las superficies de los puntos dentro del contorno.");
        return result;
    }

    // ── Voxel solid ───────────────────────────────────────────────────────
    constexpr int pad = 2;
    const int nx = nu + 2 * pad;
    const int ny = nv + 2 * pad;
    const int nz = static_cast<int>(std::ceil((wHi - wLo) / h)) + 1 + 2 * pad;
    if (static_cast<size_t>(nx) * ny * nz > kMaxVoxels) {
        result.error = QStringLiteral("El volumen de la férula es demasiado grande para la resolución elegida.");
        return result;
    }
    const double ox = u0 - pad * h;
    const double oy = v0 - pad * h;
    const double oz = wLo - pad * h;

    auto volume = vtkSmartPointer<vtkImageData>::New();
    volume->SetDimensions(nx, ny, nz);
    volume->SetOrigin(ox, oy, oz);
    volume->SetSpacing(h, h, h);
    volume->AllocateScalars(VTK_FLOAT, 1);
    auto* occupancy = static_cast<float*>(volume->GetScalarPointer());
    std::fill(occupancy, occupancy + static_cast<size_t>(nx) * ny * nz, 0.0f);

    parallelFor(nz, [&](int kBegin, int kEnd) {
        for (int k = kBegin; k < kEnd; ++k) {
            if (isCancelled(inputs))
                return;
            const double w = oz + k * h;
            for (int j = pad; j < ny - pad; ++j) {
                const int gj = j - pad;
                const double v = oy + j * h;
                for (int i = pad; i < nx - pad; ++i) {
                    const int gi = i - pad;
                    const size_t idx = static_cast<size_t>(gj) * nu + gi;
                    const double inside2d = 0.5 - signedDist[idx] / h;
                    if (inside2d <= 0.0)
                        continue;
                    const double u = ox + i * h;
                    const double occTop = 0.5 + (topAt(idx, u, v, w) - w) / h;
                    const double occBottom = 0.5 + (w - bottomAt(idx, u, v, w)) / h;
                    double occ = std::min({inside2d, occTop, occBottom});
                    if (occ > 0.0) {
                        if (hasBevel) {
                            const Vec3 d{u - bevelOrigin[0], v - bevelOrigin[1], w - bevelOrigin[2]};
                            occ = std::min(occ, 0.5 - dot(d, bevelNormal) / h);
                        }
                        for (const LocalHole& hole : holes) {
                            const Vec3 d{u - hole.center[0], v - hole.center[1], w - hole.center[2]};
                            const double t = dot(d, hole.axis);
                            if (std::abs(t) > WireHoleHalfLengthMm)
                                continue;
                            const double radial = std::sqrt(std::max(0.0, dot(d, d) - t * t));
                            occ = std::min(occ, 0.5 + (radial - hole.radius) / h);
                        }
                    }
                    occ = std::clamp(occ, 0.0, 1.0);
                    occupancy[static_cast<size_t>(i) + static_cast<size_t>(nx) * (static_cast<size_t>(j) + static_cast<size_t>(ny) * k)] =
                        static_cast<float>(occ);
                }
            }
        }
    });
    if (isCancelled(inputs)) {
        result.error = cancelledMessage();
        return result;
    }

    auto gaussian = vtkSmartPointer<vtkImageGaussianSmooth>::New();
    gaussian->SetInputData(volume);
    gaussian->SetDimensionality(3);
    const double sigmaVoxels = std::max(0.5, std::clamp(params.filletMm, 0.0, 1.0) / h);
    gaussian->SetStandardDeviations(sigmaVoxels, sigmaVoxels, sigmaVoxels);
    gaussian->SetRadiusFactors(2.5, 2.5, 2.5);
    gaussian->Update();
    vtkImageData* smoothed = gaussian->GetOutput();

    auto surface = vtkSmartPointer<vtkFlyingEdges3D>::New();
    surface->SetInputData(smoothed);
    surface->SetValue(0, 0.5);
    surface->ComputeNormalsOff();
    surface->ComputeGradientsOff();
    surface->ComputeScalarsOff();
    surface->Update();
    vtkSmartPointer<vtkPolyData> localMesh = surface->GetOutput();
    if (!localMesh || localMesh->GetNumberOfPolys() == 0) {
        result.error = QStringLiteral("No quedó material de férula dentro del contorno; revise los puntos.");
        return result;
    }
    const qint64 solidMs = timer.elapsed() - contourMs;

    QString decimationNote = QStringLiteral("sin decimado");
    if (localMesh->GetNumberOfPolys() > 20000) {
        auto decimate = vtkSmartPointer<vtkDecimatePro>::New();
        decimate->SetInputData(localMesh);
        decimate->SetTargetReduction(0.5);
        decimate->PreserveTopologyOn();
        decimate->SplittingOff();
        decimate->BoundaryVertexDeletionOff();
        decimate->SetFeatureAngle(30.0);
        decimate->Update();
        if (decimate->GetOutput()->GetNumberOfPolys() > 0 && openEdgeCount(decimate->GetOutput()) == 0) {
            decimationNote = QStringLiteral("decimado %1 → %2").arg(localMesh->GetNumberOfPolys())
                                 .arg(decimate->GetOutput()->GetNumberOfPolys());
            localMesh = decimate->GetOutput();
        } else {
            decimationNote = QStringLiteral("decimado descartado (abría la malla)");
        }
    }

    auto normals = vtkSmartPointer<vtkPolyDataNormals>::New();
    normals->SetInputData(localMesh);
    normals->ComputePointNormalsOn();
    normals->ComputeCellNormalsOff();
    normals->SplittingOff();
    normals->ConsistencyOn();
    normals->AutoOrientNormalsOn();
    normals->Update();
    auto meshLocal = vtkSmartPointer<vtkPolyData>::New();
    meshLocal->DeepCopy(normals->GetOutput());

    // ── Thickness ─────────────────────────────────────────────────────────
    if (params.computeThickness) {
        auto binary = vtkSmartPointer<vtkImageData>::New();
        binary->SetDimensions(nx, ny, nz);
        binary->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
        auto* bin = static_cast<unsigned char*>(binary->GetScalarPointer());
        const auto* smooth = static_cast<float*>(smoothed->GetScalarPointer());
        const size_t voxels = static_cast<size_t>(nx) * ny * nz;
        for (size_t i = 0; i < voxels; ++i)
            bin[i] = smooth[i] >= 0.5f ? 1 : 0;

        const double marchMm = 0.75 * std::max(params.maxThicknessMm, params.minThicknessMm) + 2.0 * h;
        auto edt = vtkSmartPointer<vtkImageEuclideanDistance>::New();
        edt->SetInputData(binary);
        edt->InitializeOn();
        edt->ConsiderAnisotropyOff();
        edt->SetAlgorithmToSaitoCached();
        edt->SetMaximumDistance(std::pow(marchMm / h + 2.0, 2.0));
        edt->Update();
        const auto* dist2 = static_cast<double*>(edt->GetOutput()->GetScalarPointer());

        auto* normalArray = meshLocal->GetPointData()->GetNormals();
        auto thickness = vtkSmartPointer<vtkFloatArray>::New();
        thickness->SetName(ThicknessArrayName);
        thickness->SetNumberOfTuples(meshLocal->GetNumberOfPoints());
        const int nPoints = static_cast<int>(meshLocal->GetNumberOfPoints());
        parallelFor(nPoints, [&](int begin, int end) {
            double p[3] = {};
            double n[3] = {};
            for (int id = begin; id < end; ++id) {
                meshLocal->GetPoint(id, p);
                normalArray->GetTuple(id, n);
                double best = 0.0;
                for (double s = 0.5 * h; s <= marchMm; s += 0.5 * h) {
                    const double x = (p[0] - n[0] * s - ox) / h;
                    const double y = (p[1] - n[1] * s - oy) / h;
                    const double z = (p[2] - n[2] * s - oz) / h;
                    const double d = std::sqrt(std::max(0.0, trilinear(dist2, nx, ny, nz, x, y, z))) * h;
                    if (d > best)
                        best = d;
                    else if (d < best - h)
                        break;
                }
                thickness->SetValue(id, static_cast<float>(2.0 * best));
            }
        });
        meshLocal->GetPointData()->AddArray(thickness);

        std::vector<float> values(static_cast<size_t>(nPoints));
        for (int id = 0; id < nPoints; ++id)
            values[static_cast<size_t>(id)] = thickness->GetValue(id);
        result.minThicknessMm = values.empty() ? 0.0 : *std::min_element(values.begin(), values.end());
        result.p05ThicknessMm = percentileOf(values, 0.05);
    }

    auto transform = vtkSmartPointer<vtkTransform>::New();
    transform->SetMatrix(prep.frame.LocalToWorldMatrix());
    auto toWorld = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
    toWorld->SetInputData(meshLocal);
    toWorld->SetTransform(transform);
    toWorld->Update();
    result.mesh = vtkSmartPointer<vtkPolyData>::New();
    result.mesh->DeepCopy(toWorld->GetOutput());

    auto contourPoints = vtkSmartPointer<vtkPoints>::New();
    auto contourLines = vtkSmartPointer<vtkCellArray>::New();
    for (const auto& c : result.contoursUV) {
        std::vector<vtkIdType> ids;
        for (const UV& p : c) {
            const Vec3 world = prep.frame.ToWorld(p[0], p[1], 0.0);
            ids.push_back(contourPoints->InsertNextPoint(world.data()));
        }
        ids.push_back(ids.front());
        contourLines->InsertNextCell(static_cast<vtkIdType>(ids.size()), ids.data());
    }
    result.contourWorld = vtkSmartPointer<vtkPolyData>::New();
    result.contourWorld->SetPoints(contourPoints);
    result.contourWorld->SetLines(contourLines);

    double contourArea = 0.0;
    for (const auto& c : result.contoursUV)
        contourArea += ContourArea(c);
    result.report = QStringLiteral("Férula: contorno %1 mm² (%2), %3 triángulos (%4). Contorno %5 ms, sólido %6 ms, total %7 ms.")
                        .arg(contourArea, 0, 'f', 0)
                        .arg(inputs.contourOverrideUV.empty() ? QStringLiteral("automático") : QStringLiteral("editado"))
                        .arg(result.mesh->GetNumberOfPolys())
                        .arg(decimationNote)
                        .arg(contourMs).arg(solidMs).arg(timer.elapsed());
    if (params.computeThickness)
        result.report += QStringLiteral(" Grosor mínimo %1 mm (P5 %2 mm).")
                             .arg(result.minThicknessMm, 0, 'f', 2).arg(result.p05ThicknessMm, 0, 'f', 2);
    if (!params.impressionUpper)
        result.report += QStringLiteral(" Cara superior plana a %1 mm bajo la superficie de los puntos.").arg(flatDropUpper, 0, 'f', 2);
    if (!params.impressionLower)
        result.report += QStringLiteral(" Cara inferior plana a %1 mm sobre la superficie de los puntos.").arg(flatRiseLower, 0, 'f', 2);
    if (result.perforatedAreaMm2 > 0.0)
        result.report += QStringLiteral(" Área perforada por contacto oclusal: %1 mm².").arg(result.perforatedAreaMm2, 0, 'f', 1);
    if (hasBevel)
        result.report += QStringLiteral(" Bisel aplicado.");
    if (bevelIgnored)
        result.report += QStringLiteral(" Bisel ignorado: los dos puntos coinciden o siguen la arcada.");
    if (!holes.empty())
        result.report += QStringLiteral(" %1 agujero(s) para alambre.").arg(holes.size());
    if (!upperMarks.empty() || !lowerMarks.empty())
        result.report += QStringLiteral(" Margen de brackets %1 mm en %2 marca(s).")
                             .arg(bracketOffset, 0, 'f', 2).arg(upperMarks.size() + lowerMarks.size());
    if (!prep.report.isEmpty())
        result.report = prep.report + QStringLiteral("\n") + result.report;
    result.ok = true;
    return result;
}

SplintHeightmapResult SplintHeightmapGenerator::Generate(const SplintHeightmapInputs& inputs)
{
    const SplintHeightmapPrepared prep = Prepare(inputs);
    if (!prep.ok) {
        SplintHeightmapResult result;
        result.error = prep.error;
        result.frame = prep.frame;
        return result;
    }
    return Build(prep, inputs);
}
