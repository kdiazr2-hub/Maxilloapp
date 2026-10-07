// GuideResolutionProbe — the same Le Fort guide contoured two ways, to decide whether OpenVDB is worth adopting
// (user's question, 2026-10-07: "¿se puede usar una librería mejor que VTK para que me genere mejores guías?").
//
//   A. today: GuideDesignCore::Build — a dense grid at 0.25 mm, Flying Edges, 70 windowed-sinc passes, repair.
//   B. OpenVDB: the very same implicit solid (GuideDesignCore::SolidNode) sampled at 0.1 mm only in a narrow band
//      round its surface (a sparse VDB tree), signed-flood-filled, meshed with tools::volumeToMesh and lightly
//      smoothed.
//
// It reports time, voxels touched, triangles and the measured diameter of every drill sleeve's bore against the
// nominal 1.6 mm, and renders a close-up of a sleeve and of the engraved text side by side (A left, B right).
// Not a CTest: run it by hand (needs a display for the PNGs, e.g. Xvfb).
//
//   GuideResolutionProbe <out dir> [fine spacing mm, default 0.1]

#include "GuideBaseCore.h"
#include "GuideDesignCore.h"
#include "ImplicitCore.h"
#include "LeFortGuideCore.h"
#include "MeshRepairCore.h"
#include "OsteotomyCore.h"
#include "PlateCore.h"
#include "SplintTestGeometry.h"
#include "WrapCore.h"

#include <openvdb/openvdb.h>
#include <openvdb/tools/SignedFloodFill.h>
#include <openvdb/tools/VolumeToMesh.h>

#include <vtkActor.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkPNGWriter.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkPolyDataNormals.h>
#include <vtkProperty.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkSTLWriter.h>
#include <vtkTextActor.h>
#include <vtkTextProperty.h>
#include <vtkWindowToImageFilter.h>
#include <vtkWindowedSincPolyDataFilter.h>

#include <QDir>
#include <QString>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

namespace
{
using Vec3 = std::array<double, 3>;
using Clock = std::chrono::steady_clock;
using splinttest::boxMesh;

double seconds(Clock::time_point since) { return std::chrono::duration<double>(Clock::now() - since).count(); }

// LeFortGuideTests' synthetic anterior maxilla: face towards +y, piriform aperture |x| < 6, nasal spine, cut z = 9.
std::vector<vtkSmartPointer<vtkPolyData>> anatomy()
{
    return {boxMesh({-25.0, -6.0, -10.0, 0.0, 10.0, 30.0}, false, false),
            boxMesh({6.0, 25.0, -10.0, 0.0, 10.0, 30.0}, false, false),
            boxMesh({-6.0, 6.0, -10.0, 0.0, 20.0, 30.0}, false, false),
            boxMesh({-25.0, 25.0, -10.0, 0.0, -10.0, 8.0}, false, false),
            boxMesh({-1.5, 1.5, 0.0, 4.0, 5.0, 8.0}, false, false)};
}

OsteotomyPath leFortCut()
{
    return OsteotomyCore::LeFortPath({{{-10.0, 5.0, 9.0}, {10.0, 5.0, 9.0}, {-20.0, -5.0, 9.0}, {20.0, -5.0, 9.0}}});
}

// The surgeon's four pillars: two holes above and two below the cut on each side.
std::vector<PredictiveHole> markedHoles()
{
    std::vector<PredictiveHole> holes;
    for (const double side : {-1.0, 1.0})
        for (const auto& [x, z] : std::vector<std::pair<double, double>>{{9.0, 21.0}, {9.0, 3.0}, {18.0, 15.0}, {18.0, 3.0}}) {
            PredictiveHole hole;
            hole.bone = z > 9.0 ? PlateBone::Cranial : PlateBone::Segment;
            hole.preopCenter = {side * x, 0.0, z};
            hole.preopAxis = {0.0, 1.0, 0.0};
            hole.plannedCenter = hole.preopCenter;
            hole.plannedAxis = hole.preopAxis;
            holes.push_back(hole);
        }
    return holes;
}

struct VdbResult
{
    vtkSmartPointer<vtkPolyData> mesh;
    size_t activeVoxels = 0;
    size_t coarseSamples = 0;
};

// The solid sampled at `fine` only near its surface: a coarse pass finds the cells the surface crosses, and only
// those are refined. Signed flood fill gives the untouched interior its negative sign before meshing.
VdbResult sampleWithVdb(const ImplicitCore::NodePtr& solid, const double bounds[6], double fine)
{
    VdbResult out;
    const double coarse = 5.0 * fine;
    const int ratio = 5;
    std::array<int, 3> dims{};
    for (int a = 0; a < 3; ++a)
        dims[a] = static_cast<int>(std::ceil((bounds[2 * a + 1] - bounds[2 * a]) / coarse)) + 2;
    const Vec3 origin{bounds[0] - coarse, bounds[2] - coarse, bounds[4] - coarse};
    // Coarse values at the corners of the coarse cells.
    std::vector<float> coarseValues(static_cast<size_t>(dims[0]) * dims[1] * dims[2]);
    const auto cidx = [&](int i, int j, int k) { return (static_cast<size_t>(k) * dims[1] + j) * dims[0] + i; };
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    {
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < threads; ++t)
            pool.emplace_back([&, t] {
                for (int k = static_cast<int>(t); k < dims[2]; k += static_cast<int>(threads))
                    for (int j = 0; j < dims[1]; ++j)
                        for (int i = 0; i < dims[0]; ++i)
                            coarseValues[cidx(i, j, k)] = static_cast<float>(ImplicitCore::Value(
                                solid, {origin[0] + i * coarse, origin[1] + j * coarse, origin[2] + k * coarse}));
            });
        for (auto& th : pool)
            th.join();
    }
    out.coarseSamples = coarseValues.size();
    // Cells near the surface: some corner within a cell diagonal of it.
    const float near = static_cast<float>(coarse * 1.8);
    std::vector<std::array<int, 3>> cells;
    for (int k = 0; k + 1 < dims[2]; ++k)
        for (int j = 0; j + 1 < dims[1]; ++j)
            for (int i = 0; i + 1 < dims[0]; ++i) {
                float lo = 1e30f;
                for (int c = 0; c < 8; ++c)
                    lo = std::min(lo, std::abs(coarseValues[cidx(i + (c & 1), j + ((c >> 1) & 1), k + ((c >> 2) & 1))]));
                if (lo < near)
                    cells.push_back({i, j, k});
            }
    // Fine samples in those cells, per thread, then written into the VDB tree.
    const float background = static_cast<float>(3.0 * fine);
    auto grid = openvdb::FloatGrid::create(background);
    grid->setTransform(openvdb::math::Transform::createLinearTransform(fine));
    grid->setGridClass(openvdb::GRID_LEVEL_SET);
    struct Sample { openvdb::Coord ijk; float value; };
    std::vector<std::vector<Sample>> perThread(threads);
    {
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < threads; ++t)
            pool.emplace_back([&, t] {
                auto& mine = perThread[t];
                for (size_t c = t; c < cells.size(); c += threads) {
                    const auto& cell = cells[c];
                    for (int dk = 0; dk <= ratio; ++dk)
                        for (int dj = 0; dj <= ratio; ++dj)
                            for (int di = 0; di <= ratio; ++di) {
                                const Vec3 p{origin[0] + cell[0] * coarse + di * fine, origin[1] + cell[1] * coarse + dj * fine,
                                             origin[2] + cell[2] * coarse + dk * fine};
                                const double v = ImplicitCore::Value(solid, p);
                                const openvdb::Coord ijk(static_cast<int>(std::lround(p[0] / fine)),
                                                         static_cast<int>(std::lround(p[1] / fine)),
                                                         static_cast<int>(std::lround(p[2] / fine)));
                                mine.push_back({ijk, static_cast<float>(std::clamp(v, -3.0 * fine, 3.0 * fine))});
                            }
                }
            });
        for (auto& th : pool)
            th.join();
    }
    auto accessor = grid->getAccessor();
    for (const auto& samples : perThread)
        for (const Sample& s : samples)
            accessor.setValue(s.ijk, s.value);
    openvdb::tools::signedFloodFill(grid->tree());
    out.activeVoxels = grid->activeVoxelCount();

    std::vector<openvdb::Vec3s> points;
    std::vector<openvdb::Vec3I> triangles;
    std::vector<openvdb::Vec4I> quads;
    openvdb::tools::volumeToMesh(*grid, points, triangles, quads, 0.0, 0.0);
    auto vtkPts = vtkSmartPointer<vtkPoints>::New();
    for (const auto& p : points)
        vtkPts->InsertNextPoint(p[0], p[1], p[2]);
    auto polys = vtkSmartPointer<vtkCellArray>::New();
    for (const auto& t : triangles) {
        const vtkIdType ids[3] = {static_cast<vtkIdType>(t[0]), static_cast<vtkIdType>(t[1]), static_cast<vtkIdType>(t[2])};
        polys->InsertNextCell(3, ids);
    }
    for (const auto& q : quads) {
        const vtkIdType a[3] = {static_cast<vtkIdType>(q[0]), static_cast<vtkIdType>(q[1]), static_cast<vtkIdType>(q[2])};
        const vtkIdType b[3] = {static_cast<vtkIdType>(q[0]), static_cast<vtkIdType>(q[2]), static_cast<vtkIdType>(q[3])};
        polys->InsertNextCell(3, a);
        polys->InsertNextCell(3, b);
    }
    auto raw = vtkSmartPointer<vtkPolyData>::New();
    raw->SetPoints(vtkPts);
    raw->SetPolys(polys);
    // A light pass only: at 0.1 mm the voxel steps are a tenth of today's, so far less smoothing is needed and
    // edges keep their shape.
    auto smooth = vtkSmartPointer<vtkWindowedSincPolyDataFilter>::New();
    smooth->SetInputData(raw);
    smooth->SetNumberOfIterations(15);
    smooth->SetPassBand(0.1);
    smooth->NonManifoldSmoothingOn();
    smooth->NormalizeCoordinatesOn();
    smooth->Update();
    out.mesh = vtkSmartPointer<vtkPolyData>::New();
    out.mesh->DeepCopy(smooth->GetOutput());
    return out;
}

struct BoreStats { double meanDiameter = 0.0, worstDeviation = 0.0, roundnessSd = 0.0; int sleeves = 0; };

// The bore wall of each sleeve: vertices within the sleeve's height and well inside its outer wall.
BoreStats measureBores(vtkPolyData* mesh, const std::vector<PredictiveHole>& holes, double nominal)
{
    BoreStats stats;
    double sumD = 0.0, sumSd = 0.0;
    for (const PredictiveHole& hole : holes) {
        std::vector<double> radii;
        for (vtkIdType id = 0; id < mesh->GetNumberOfPoints(); ++id) {
            double p[3];
            mesh->GetPoint(id, p);
            const Vec3 d{p[0] - hole.preopCenter[0], p[1] - hole.preopCenter[1], p[2] - hole.preopCenter[2]};
            const double t = d[0] * hole.preopAxis[0] + d[1] * hole.preopAxis[1] + d[2] * hole.preopAxis[2];
            if (t < 0.8 || t > 3.6) // the sleeve's own height, clear of both ends' rounding
                continue;
            const Vec3 r{d[0] - t * hole.preopAxis[0], d[1] - t * hole.preopAxis[1], d[2] - t * hole.preopAxis[2]};
            const double radius = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
            if (radius < 0.5 * nominal + 0.6)
                radii.push_back(radius);
        }
        if (radii.size() < 8)
            continue;
        double mean = 0.0;
        for (double r : radii)
            mean += r;
        mean /= static_cast<double>(radii.size());
        double var = 0.0;
        for (double r : radii)
            var += (r - mean) * (r - mean);
        const double sd = std::sqrt(var / static_cast<double>(radii.size()));
        sumD += 2.0 * mean;
        sumSd += 2.0 * sd;
        stats.worstDeviation = std::max(stats.worstDeviation, std::abs(2.0 * mean - nominal));
        ++stats.sleeves;
    }
    if (stats.sleeves > 0) {
        stats.meanDiameter = sumD / stats.sleeves;
        stats.roundnessSd = sumSd / stats.sleeves;
    }
    return stats;
}

void addMesh(vtkRenderer* renderer, vtkPolyData* mesh)
{
    auto normals = vtkSmartPointer<vtkPolyDataNormals>::New();
    normals->SetInputData(mesh);
    normals->SplittingOff();
    auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    mapper->SetInputConnection(normals->GetOutputPort());
    auto actor = vtkSmartPointer<vtkActor>::New();
    actor->SetMapper(mapper);
    actor->GetProperty()->SetColor(0.78, 0.80, 0.84);
    actor->GetProperty()->SetSpecular(0.3);
    actor->GetProperty()->SetSpecularPower(30.0);
    renderer->AddActor(actor);
}

void sideBySide(vtkPolyData* left, const QString& leftTitle, vtkPolyData* right, const QString& rightTitle,
                const Vec3& focus, const Vec3& from, double viewHeight, const QString& file)
{
    auto window = vtkSmartPointer<vtkRenderWindow>::New();
    window->SetOffScreenRendering(1);
    window->SetSize(1400, 700);
    for (int half = 0; half < 2; ++half) {
        auto renderer = vtkSmartPointer<vtkRenderer>::New();
        renderer->SetViewport(half * 0.5, 0.0, half * 0.5 + 0.5, 1.0);
        renderer->SetBackground(0.12, 0.13, 0.15);
        addMesh(renderer, half == 0 ? left : right);
        auto title = vtkSmartPointer<vtkTextActor>::New();
        title->SetInput((half == 0 ? leftTitle : rightTitle).toUtf8().constData());
        title->GetTextProperty()->SetFontSize(22);
        title->GetTextProperty()->SetColor(1.0, 1.0, 1.0);
        title->SetDisplayPosition(16 + half * 700, 660); // window pixels, not the viewport's
        renderer->AddActor2D(title);
        vtkCamera* camera = renderer->GetActiveCamera();
        camera->SetFocalPoint(focus[0], focus[1], focus[2]);
        camera->SetPosition(focus[0] + from[0], focus[1] + from[1], focus[2] + from[2]);
        camera->SetViewUp(0.0, 0.0, 1.0);
        camera->SetParallelProjection(1);
        camera->SetParallelScale(0.5 * viewHeight);
        renderer->ResetCameraClippingRange();
        window->AddRenderer(renderer);
    }
    window->Render();
    auto grab = vtkSmartPointer<vtkWindowToImageFilter>::New();
    grab->SetInput(window);
    grab->Update();
    auto png = vtkSmartPointer<vtkPNGWriter>::New();
    png->SetFileName(file.toUtf8().constData());
    png->SetInputConnection(grab->GetOutputPort());
    png->Write();
}

void writeStl(vtkPolyData* mesh, const QString& file)
{
    auto writer = vtkSmartPointer<vtkSTLWriter>::New();
    writer->SetFileName(file.toUtf8().constData());
    writer->SetInputData(mesh);
    writer->SetFileTypeToBinary();
    writer->Write();
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: GuideResolutionProbe <out dir> [fine spacing mm]\n");
        return 2;
    }
    const QString outDir = QString::fromLocal8Bit(argv[1]);
    QDir().mkpath(outDir);
    const double fine = argc > 2 ? std::atof(argv[2]) : 0.1;
    openvdb::initialize();

    // The anatomy, its envelope and the guide plan, as the assistant lays it out.
    const auto meshes = anatomy();
    std::vector<vtkPolyData*> raw;
    for (const auto& m : meshes)
        raw.push_back(m);
    WrapParams wrapParams;
    wrapParams.gapClosingMm = 1.5;
    wrapParams.smallestDetailMm = 0.4;
    const WrapResult wrap = WrapCore::Wrap(raw, wrapParams);
    GuideDesignParams design;
    design.base.smallestDetailMm = 0.4;
    design.slot.smallestDetailMm = 0.4;
    const GuidePreparation prepared = GuideDesignCore::Prepare(wrap.mesh, design);
    if (!wrap.ok || !prepared.ok) {
        std::fprintf(stderr, "the anatomy could not be wrapped\n");
        return 1;
    }
    const std::vector<PredictiveHole> holes = markedHoles();
    LeFortGuideParams params;
    params.separateSides = true;
    params.hullOutline = true;
    params.extentFromHoles = true;
    params.caseLabel = QStringLiteral("20406");
    const LeFortGuideLayout layout = LeFortGuideCore::Layout(prepared, wrap.mesh, leFortCut(), holes, params);
    if (!layout.ok) {
        std::fprintf(stderr, "layout failed: %s\n", layout.error.toUtf8().constData());
        return 1;
    }
    const GuideRegion region = GuideBaseCore::MakeBrushRegion(prepared.wrapField, layout.paint, design.base);
    SleeveParams sleeve;
    std::vector<GuideFigure> figures = layout.figures;
    for (const GuideFigure& f : PlateCore::SleeveFigures(holes, sleeve))
        figures.push_back(f);

    // A. Today.
    auto t0 = Clock::now();
    const GuideDesignResult today = GuideDesignCore::Build(prepared, region, layout.slotPlan, layout.fixation, figures, design);
    const double timeA = seconds(t0);
    if (!today.ok) {
        std::fprintf(stderr, "today's build failed: %s\n", today.error.toUtf8().constData());
        return 1;
    }
    double boundsA[6];
    today.mesh->GetBounds(boundsA);
    const double denseA = std::ceil((boundsA[1] - boundsA[0]) / 0.25 + 8) * std::ceil((boundsA[3] - boundsA[2]) / 0.25 + 8) *
                          std::ceil((boundsA[5] - boundsA[4]) / 0.25 + 8);

    // B. OpenVDB at `fine`, on the very same solid.
    t0 = Clock::now();
    double bounds[6];
    QString error;
    const auto solid = GuideDesignCore::SolidNode(prepared, region, layout.slotPlan, layout.fixation, figures, design, fine,
                                                  bounds, nullptr, nullptr, &error);
    if (!solid) {
        std::fprintf(stderr, "no solid: %s\n", error.toUtf8().constData());
        return 1;
    }
    VdbResult vdb = sampleWithVdb(solid, bounds, fine);
    const MeshCheck rawCheck = MeshRepairCore::Analyze(vdb.mesh);
    std::printf("OpenVDB raw mesh: %lld boundary edges, %lld non-manifold, %lld inconsistent, %d shells\n",
                static_cast<long long>(rawCheck.boundaryEdges), static_cast<long long>(rawCheck.nonManifoldEdges),
                static_cast<long long>(rawCheck.inconsistentEdges), rawCheck.shells);
    // The same repair today's build ends with.
    const MeshRepairResult repaired = MeshRepairCore::Repair(vdb.mesh);
    if (repaired.ok && repaired.mesh)
        vdb.mesh = repaired.mesh;
    const double timeB = seconds(t0);
    const double denseB = std::ceil((bounds[1] - bounds[0]) / fine) * std::ceil((bounds[3] - bounds[2]) / fine) *
                          std::ceil((bounds[5] - bounds[4]) / fine);

    const BoreStats boreA = measureBores(today.mesh, holes, sleeve.boreDiameterMm);
    const BoreStats boreB = measureBores(vdb.mesh, holes, sleeve.boreDiameterMm);
    const MeshCheck checkB = MeshRepairCore::Analyze(vdb.mesh);

    std::printf("A today   : %.1f s, dense grid %.1f M voxels at 0.25 mm, %lld triangles\n", timeA, denseA / 1e6,
                static_cast<long long>(today.mesh->GetNumberOfPolys()));
    std::printf("B OpenVDB : %.1f s, %.1f M voxels touched at %.2f mm (a dense grid would be %.1f M), %lld triangles, "
                "closed %s\n",
                timeB, (vdb.activeVoxels + vdb.coarseSamples) / 1e6, fine, denseB / 1e6,
                static_cast<long long>(vdb.mesh->GetNumberOfPolys()), checkB.Valid() ? "yes" : "no");
    std::printf("Bore %.1f mm nominal — A: mean %.2f mm (worst off %.2f, roundness sd %.3f) over %d sleeves\n",
                sleeve.boreDiameterMm, boreA.meanDiameter, boreA.worstDeviation, boreA.roundnessSd, boreA.sleeves);
    std::printf("                    — B: mean %.2f mm (worst off %.2f, roundness sd %.3f) over %d sleeves\n",
                boreB.meanDiameter, boreB.worstDeviation, boreB.roundnessSd, boreB.sleeves);

    writeStl(today.mesh, QDir(outDir).filePath(QStringLiteral("guia_actual_025.stl")));
    writeStl(vdb.mesh, QDir(outDir).filePath(QStringLiteral("guia_openvdb_%1.stl").arg(fine, 0, 'f', 2)));
    const QString leftTitle = QStringLiteral("Actual: 0.25 mm + 70 suavizados");
    const QString rightTitle = QStringLiteral("OpenVDB: %1 mm").arg(fine, 0, 'f', 2);
    // A sleeve from in front and from the side, the engraving, and the whole right guide.
    const PredictiveHole& sleeveHole = holes.front();
    sideBySide(today.mesh, leftTitle, vdb.mesh, rightTitle, sleeveHole.preopCenter, {0.0, 40.0, 0.0}, 9.0,
               QDir(outDir).filePath(QStringLiteral("1_camisa_frente.png")));
    sideBySide(today.mesh, leftTitle, vdb.mesh, rightTitle, sleeveHole.preopCenter, {30.0, 25.0, 12.0}, 10.0,
               QDir(outDir).filePath(QStringLiteral("2_camisa_lado.png")));
    for (const LeFortGuideLabel& label : layout.labels)
        if (label.text == params.caseLabel) {
            sideBySide(today.mesh, leftTitle, vdb.mesh, rightTitle, label.center, {0.0, 40.0, 0.0}, 0.9 * label.widthMm,
                       QDir(outDir).filePath(QStringLiteral("3_texto.png")));
            break;
        }
    sideBySide(today.mesh, leftTitle, vdb.mesh, rightTitle, {-14.0, 2.0, 10.0}, {12.0, 40.0, 8.0}, 34.0,
               QDir(outDir).filePath(QStringLiteral("4_guia_derecha.png")));
    return 0;
}
