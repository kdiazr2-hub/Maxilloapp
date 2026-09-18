#include "GuideSculptCore.h"

#include "ImplicitCore.h"
#include "SplintTestGeometry.h"

#include <vtkMassProperties.h>
#include <vtkPolyData.h>
#include <vtkPolyDataConnectivityFilter.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using namespace splinttest;
using Vec3 = std::array<double, 3>;

constexpr double kSpacing = 0.5; // coarse on purpose: these are kernel tests, not print quality

vtkSmartPointer<vtkPolyData> meshOf(const ImplicitCore::NodePtr& node, double spacing = kSpacing)
{
    double bounds[6] = {};
    require(ImplicitCore::Bounds(node, bounds), "the test solid is not bounded");
    const ImplicitCore::BuildResult built = ImplicitCore::Build(node, bounds, spacing);
    require(built.ok, "the test solid could not be meshed");
    return built.mesh;
}

double volumeOf(vtkPolyData* mesh)
{
    auto mass = vtkSmartPointer<vtkMassProperties>::New();
    mass->SetInputData(mesh);
    mass->Update();
    return mass->GetVolume();
}

int shellsOf(vtkPolyData* mesh)
{
    auto connectivity = vtkSmartPointer<vtkPolyDataConnectivityFilter>::New();
    connectivity->SetInputData(mesh);
    connectivity->SetExtractionModeToAllRegions();
    connectivity->Update();
    return connectivity->GetNumberOfExtractedRegions();
}

// A slab with one sharp edge along y at (x = 0, z = 0): the corner the smoothing tools have to round.
ImplicitCore::NodePtr edgedBlock()
{
    return ImplicitCore::Box({-5.0, 0.0, -5.0}, {5.0, 6.0, 5.0});
}

SculptSession openOn(const ImplicitCore::NodePtr& node, double padding = 4.0)
{
    SculptSession session;
    QString error;
    require(session.Reset(meshOf(node), kSpacing, padding, &error), error.toStdString());
    return session;
}

SculptBrush brushAt(SculptTool tool, const Vec3& center, double radius, double level)
{
    SculptBrush brush;
    brush.tool = tool;
    brush.center = center;
    brush.radiusMm = radius;
    brush.level = level;
    return brush;
}

// Where the surface crosses the diagonal through the block's edge at (0, y, 0): 0 for the sharp corner
// itself, and further and further inside (negative) the more the edge is rounded off.
double cornerReach(const SculptSession& session)
{
    const SculptGrid& grid = session.Grid();
    const double s = 1.0 / std::sqrt(2.0);
    double low = -8.0, high = 8.0; // low is inside the block, high outside it
    for (int i = 0; i < 40; ++i) {
        const double mid = 0.5 * (low + high);
        (grid.At({mid * s, 0.0, mid * s}) < 0.0 ? low : high) = mid;
    }
    return 0.5 * (low + high);
}

// ── The grid ──────────────────────────────────────────────────────────────────
void testGridMatchesTheSolid()
{
    const auto sphere = ImplicitCore::Sphere({0.0, 0.0, 0.0}, 6.0);
    SculptSession session = openOn(sphere);
    require(session.Ready(), "the session has no grid");
    const SculptGrid& grid = session.Grid();
    // The baked field is the sphere's own distance, to within the voxel it was rasterised on.
    for (const Vec3 p : {Vec3{0.0, 0.0, 0.0}, Vec3{3.0, 0.0, 0.0}, Vec3{0.0, 0.0, 7.5}, Vec3{4.0, 4.0, 0.0}}) {
        const double exact = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) - 6.0;
        require(std::abs(grid.At(p) - exact) < 2.0 * kSpacing,
                "the grid does not measure the sphere at (" + std::to_string(p[0]) + ", " + std::to_string(p[2]) +
                    "): " + std::to_string(grid.At(p)) + " instead of " + std::to_string(exact));
    }
    // There is room around the guide for material to be added.
    const auto mesh = session.Contour(false);
    require(mesh && mesh->GetNumberOfPolys() > 0, "the grid did not contour back");
    double bounds[6] = {};
    mesh->GetBounds(bounds);
    require(bounds[1] > 5.5 && bounds[1] < 6.5, "the contour is not the sphere back again");
    require(grid.origin[0] < -9.0, "the grid leaves no padding for added material");
}

// ── Smooth ────────────────────────────────────────────────────────────────────
void testSmoothRoundsAnEdgeAndKeepsTheVolume()
{
    SculptSession session = openOn(edgedBlock());
    const double sharp = cornerReach(session);
    const double before = volumeOf(session.Contour(false));

    session.BeginStroke();
    for (double y = -5.0; y <= 5.0; y += 0.5) {
        SculptBrush brush = brushAt(SculptTool::Smooth, {0.0, y, 0.0}, 3.0, 1.0);
        brush.hasPrevious = y > -5.0;
        brush.previous = {0.0, y - 0.5, 0.0};
        session.ApplyBrush(brush);
    }
    session.EndStroke();

    const double rounded = cornerReach(session);
    require(rounded < sharp - 0.2, "smoothing did not round the edge: " + std::to_string(sharp) + " → " +
                                       std::to_string(rounded));
    const double after = volumeOf(session.Contour(false));
    require(std::abs(after - before) / before < 0.05,
            "smoothing moved the volume by more than 5 %: " + std::to_string(before) + " → " + std::to_string(after));

    // The level slider runs 50-fold: a gentle pass rounds far less than a strong one.
    SculptSession gentle = openOn(edgedBlock());
    gentle.BeginStroke();
    for (double y = -5.0; y <= 5.0; y += 0.5)
        gentle.ApplyBrush(brushAt(SculptTool::Smooth, {0.0, y, 0.0}, 3.0, 0.0));
    gentle.EndStroke();
    require(cornerReach(gentle) > rounded, "the level slider does nothing");
    require(std::abs(GuideSculptCore::LevelScale(1.0, 0.02) / GuideSculptCore::LevelScale(0.0, 0.02) - 50.0) < 1e-9,
            "the level is not 50-fold from end to end");
}

void testHotWaxMelts()
{
    SculptSession session = openOn(edgedBlock());
    const double sharp = cornerReach(session);
    session.BeginStroke();
    SculptBrush brush = brushAt(SculptTool::HotWax, {0.0, 0.0, 0.0}, 3.0, 1.0);
    brush.wax = HotWaxMode::Melt;
    session.ApplyBrush(brush);
    session.EndStroke();
    require(cornerReach(session) < sharp - 0.2, "hot wax did not melt the edge");

    // Wax is laid on the surface itself: adding and taking it away move the surface the opposite ways.
    SculptSession warm = openOn(edgedBlock());
    const double plain = volumeOf(warm.Contour(false));
    warm.BeginStroke();
    SculptBrush add = brushAt(SculptTool::HotWax, {-5.0, 0.0, 0.0}, 3.0, 1.0);
    add.wax = HotWaxMode::Add;
    warm.ApplyBrush(add);
    warm.EndStroke();
    require(volumeOf(warm.Contour(false)) > plain, "adding wax removed material");
    warm.BeginStroke();
    SculptBrush remove = brushAt(SculptTool::HotWax, {-5.0, 0.0, 0.0}, 3.0, 1.0);
    remove.wax = HotWaxMode::Remove;
    warm.ApplyBrush(remove);
    warm.ApplyBrush(remove);
    warm.EndStroke();
    require(volumeOf(warm.Contour(false)) < plain, "taking wax away added material");
}

// ── Add and remove ────────────────────────────────────────────────────────────
void testAddAndRemoveMoveTheVolume()
{
    SculptSession session = openOn(edgedBlock());
    const double before = volumeOf(session.Contour(false));
    session.BeginStroke();
    session.ApplyBrush(brushAt(SculptTool::Add, {0.0, 0.0, 5.0}, 2.5, 0.5));
    session.EndStroke();
    const double grown = volumeOf(session.Contour(false));
    require(grown > before + 5.0, "adding material did not grow the guide");

    session.BeginStroke();
    session.ApplyBrush(brushAt(SculptTool::Remove, {0.0, 0.0, 5.0}, 2.5, 0.5));
    session.EndStroke();
    const double carved = volumeOf(session.Contour(false));
    require(carved < grown - 5.0, "removing material did not carve the guide");
    // The ball sat clear of the block, so taking it away again gives the guide back as it was.
    require(std::abs(carved - before) < 0.03 * before,
            "removing the same ball did not give the guide back: " + std::to_string(before) + " → " +
                std::to_string(carved));

    // A ball that bites into the block does take material away.
    session.BeginStroke();
    session.ApplyBrush(brushAt(SculptTool::Remove, {0.0, 0.0, 0.0}, 2.5, 0.5));
    session.EndStroke();
    require(volumeOf(session.Contour(false)) < before - 5.0, "the ball did not bite into the block");
    require(shellsOf(session.Contour(false)) == 1, "the edit broke the guide into pieces");
}

// ── The protected fields ──────────────────────────────────────────────────────
void testTheBrushNeverEntersTheProtectedFields()
{
    // The guide is a slab standing at z >= 0; the "anatomy" fills everything below it.
    SculptSession session = openOn(ImplicitCore::Box({0.0, 0.0, 3.0}, {8.0, 8.0, 3.0}));
    const auto anatomy = ImplicitCore::BakeMeshField(meshOf(ImplicitCore::Box({0.0, 0.0, -6.0}, {12.0, 12.0, 6.0})),
                                                     kSpacing, 8.0);
    require(anatomy != nullptr, "the anatomy was not baked");
    // A saw slot across the middle of the slab.
    const auto slotNode = ImplicitCore::Box({0.0, 0.0, 3.0}, {0.4, 12.0, 6.0});
    double slotBounds[6] = {-14.0, 14.0, -14.0, 14.0, -2.0, 12.0};
    const auto keepOut = ImplicitCore::BakeFunction(
        [&](const Vec3& p) { return ImplicitCore::Value(slotNode, p); }, slotBounds, kSpacing);
    require(keepOut != nullptr, "the keep-out was not baked");

    SculptLimits limits;
    limits.wrapField = anatomy;
    limits.clearanceMm = 0.2;
    limits.keepOut = keepOut;
    session.SetLimits(limits);

    // A generous ball of material right over the anatomy and right over the slot.
    session.BeginStroke();
    session.ApplyBrush(brushAt(SculptTool::Add, {0.0, 0.0, -2.0}, 5.0, 1.0));
    session.ApplyBrush(brushAt(SculptTool::Add, {0.0, 0.0, 3.0}, 5.0, 1.0));
    session.EndStroke();

    const SculptGrid& grid = session.Grid();
    for (double z = -4.0; z < -0.4; z += 0.5)
        require(grid.At({0.0, 0.0, z}) > 0.0,
                "the brush filled the anatomy at z = " + std::to_string(z) + ": " + std::to_string(grid.At({0.0, 0.0, z})));
    for (double z = 1.0; z < 5.0; z += 0.5)
        require(grid.At({0.0, 0.0, z}) > 0.0,
                "the brush filled the saw slot at z = " + std::to_string(z) + ": " + std::to_string(grid.At({0.0, 0.0, z})));
    // Away from both, the wax did land.
    require(grid.At({4.0, 0.0, 6.5}) < grid.At({4.0, 0.0, 8.0}), "the brush did nothing where it was allowed");
}

// ── Flatten ───────────────────────────────────────────────────────────────────
void testFlattenLeavesAPlane()
{
    // A dome on a slab: flattening its top must bring the surface onto one plane.
    const auto solid = ImplicitCore::Union(ImplicitCore::Box({0.0, 0.0, -3.0}, {10.0, 10.0, 3.0}),
                                           ImplicitCore::Sphere({0.0, 0.0, 0.0}, 4.0));
    SculptSession session = openOn(solid);
    session.BeginStroke();
    for (int pass = 0; pass < 6; ++pass) {
        SculptBrush brush = brushAt(SculptTool::Flatten, {0.0, 0.0, 4.0}, 5.0, 1.0);
        brush.flatten = FlattenMode::Flatten;
        session.ApplyBrush(brush);
    }
    session.EndStroke();

    // The surface over the dome now sits at one height, to within a voxel.
    std::vector<double> heights;
    const SculptGrid& grid = session.Grid();
    for (double x = -2.0; x <= 2.0; x += 1.0)
        for (double y = -2.0; y <= 2.0; y += 1.0) {
            double low = 0.0, high = 8.0;
            for (int i = 0; i < 40; ++i) {
                const double mid = 0.5 * (low + high);
                (grid.At({x, y, mid}) < 0.0 ? low : high) = mid;
            }
            heights.push_back(0.5 * (low + high));
        }
    const auto [lowest, highest] = std::minmax_element(heights.begin(), heights.end());
    require(*highest - *lowest < kSpacing,
            "flattening did not leave a plane: " + std::to_string(*lowest) + " to " + std::to_string(*highest));

    // Scrape only takes material away, fill only adds it.
    SculptSession scraped = openOn(solid);
    const double plain = volumeOf(scraped.Contour(false));
    scraped.BeginStroke();
    SculptBrush scrape = brushAt(SculptTool::Flatten, {0.0, 0.0, 4.0}, 5.0, 1.0);
    scrape.flatten = FlattenMode::Scrape;
    scraped.ApplyBrush(scrape);
    scraped.EndStroke();
    require(volumeOf(scraped.Contour(false)) <= plain + 1e-6, "scraping added material");

    SculptSession filled = openOn(solid);
    filled.BeginStroke();
    SculptBrush fill = brushAt(SculptTool::Flatten, {0.0, 0.0, 4.0}, 5.0, 1.0);
    fill.flatten = FlattenMode::Fill;
    filled.ApplyBrush(fill);
    filled.EndStroke();
    require(volumeOf(filled.Contour(false)) >= plain - 1e-6, "filling removed material");
}

// ── Trim ──────────────────────────────────────────────────────────────────────
void testTrimCutsAndDropsTheIslands()
{
    // Two blocks joined by a thin neck: cutting the neck leaves an island, which must be dropped.
    const auto solid = ImplicitCore::Union({ImplicitCore::Box({-8.0, 0.0, 0.0}, {5.0, 5.0, 3.0}),
                                            ImplicitCore::Box({8.0, 0.0, 0.0}, {3.0, 3.0, 2.0}),
                                            ImplicitCore::Box({0.0, 0.0, 0.0}, {8.0, 1.0, 1.0})});
    SculptSession session = openOn(solid, 3.0);
    require(shellsOf(session.Contour(false)) == 1, "the test solid is not one piece to begin with");
    const double whole = volumeOf(session.Contour(false));

    // A window over the neck, seen along y, cut away.
    const std::vector<Vec3> window{{2.0, 0.0, -6.0}, {5.0, 0.0, -6.0}, {5.0, 0.0, 6.0}, {2.0, 0.0, 6.0}};
    QString error;
    require(session.Trim(window, {0.0, 1.0, 0.0}, false, &error), error.toStdString());
    const auto trimmed = session.Contour(false);
    require(trimmed && trimmed->GetNumberOfPolys() > 0, "the trim left nothing");
    require(shellsOf(trimmed) == 1, "the trim left islands behind");
    const double kept = volumeOf(trimmed);
    require(kept < whole, "the trim removed nothing");
    require(kept > 0.5 * whole, "the trim kept the small piece instead of the large one");

    // Inverted, the same window keeps only what is inside it.
    SculptSession inverted = openOn(solid, 3.0);
    require(inverted.Trim(window, {0.0, 1.0, 0.0}, true, &error), error.toStdString());
    const auto inside = inverted.Contour(false);
    require(inside && volumeOf(inside) < 0.5 * whole, "inverting the trim did not keep the inside");

    require(!session.Trim({{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}}, {0.0, 1.0, 0.0}, false, &error) && !error.isEmpty(),
            "a two-point trim was accepted");
}

// ── Undo ──────────────────────────────────────────────────────────────────────
void testUndoRestoresTheGridExactly()
{
    SculptSession session = openOn(edgedBlock());
    const std::vector<float> original = session.Grid().values;
    require(!session.CanUndo() && !session.CanRedo(), "a fresh session already has history");

    session.BeginStroke();
    session.ApplyBrush(brushAt(SculptTool::Add, {0.0, 0.0, 5.0}, 3.0, 1.0));
    session.EndStroke();
    const std::vector<float> edited = session.Grid().values;
    require(edited != original, "the stroke changed nothing");
    require(session.CanUndo(), "the stroke was not recorded");

    require(session.Undo(), "undo did nothing");
    require(session.Grid().values == original, "undo did not restore the grid byte for byte");
    require(session.CanRedo(), "undo left nothing to redo");
    require(session.Redo(), "redo did nothing");
    require(session.Grid().values == edited, "redo did not replay the stroke");
    require(session.Undo() && session.Grid().values == original, "the second undo did not restore the grid");
    require(!session.Undo(), "undo went past the first stroke");

    // Two strokes come back one at a time.
    session.BeginStroke();
    session.ApplyBrush(brushAt(SculptTool::Add, {4.0, 0.0, 5.0}, 2.0, 1.0));
    session.EndStroke();
    const std::vector<float> first = session.Grid().values;
    session.BeginStroke();
    session.ApplyBrush(brushAt(SculptTool::Remove, {-4.0, 0.0, 4.0}, 2.0, 1.0));
    session.EndStroke();
    require(session.Undo() && session.Grid().values == first, "undo did not stop at the stroke before");
    require(session.Undo() && session.Grid().values == original, "undo did not reach the untouched grid");
}

void testRejectsBadInput()
{
    SculptSession session;
    QString error;
    require(!session.Reset(nullptr, kSpacing, 3.0, &error) && !error.isEmpty(), "an empty guide was accepted");
    require(!session.Ready(), "an empty session says it is ready");
    require(session.Contour(false) == nullptr, "an empty session contoured something");
    require(!session.Undo() && !session.Redo(), "an empty session has history");
    session.ApplyBrush(brushAt(SculptTool::Add, {0.0, 0.0, 0.0}, 2.0, 1.0)); // must not crash
}
} // namespace

int main()
{
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"the grid measures the solid", testGridMatchesTheSolid},
        {"smooth rounds an edge and keeps the volume", testSmoothRoundsAnEdgeAndKeepsTheVolume},
        {"hot wax melts, adds and removes", testHotWaxMelts},
        {"add and remove move the volume", testAddAndRemoveMoveTheVolume},
        {"the brush never enters the protected fields", testTheBrushNeverEntersTheProtectedFields},
        {"flatten leaves a plane", testFlattenLeavesAPlane},
        {"trim cuts and drops the islands", testTrimCutsAndDropsTheIslands},
        {"undo restores the grid exactly", testUndoRestoresTheGridExactly},
        {"rejects bad input", testRejectsBadInput},
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
