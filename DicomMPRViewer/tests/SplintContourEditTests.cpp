#include "SplintContourEditCore.h"

#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using namespace SplintContourEditCore;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

bool approx(double a, double b, double tol = 1e-9)
{
    return std::abs(a - b) <= tol;
}

// 40 × 40 mm square, 0.5 mm vertex spacing (320 vertices), starting at the origin.
SplintContourUV square()
{
    return Densify({{0, 0}, {40, 0}, {40, 40}, {0, 40}}, 0.5);
}

double displacement(const SplintContourUV& a, const SplintContourUV& b, size_t i)
{
    return std::hypot(b[i][0] - a[i][0], b[i][1] - a[i][1]);
}

void testDensify()
{
    const SplintContourUV base{{0, 0}, {40, 0}, {40, 40}, {0, 40}};
    const auto dense = Densify(base, 0.5);
    require(dense.size() == 320, "unexpected vertex count: " + std::to_string(dense.size()));
    require(approx(Perimeter(dense), 160.0, 1e-6), "densify changed the perimeter");
    for (size_t i = 0; i < dense.size(); ++i) {
        const auto& a = dense[i];
        const auto& b = dense[(i + 1) % dense.size()];
        require(std::hypot(b[0] - a[0], b[1] - a[1]) <= 0.5 + 1e-9, "segment longer than the limit");
    }
    require(dense[0] == base[0] && dense[80] == base[1] && dense[160] == base[2] && dense[240] == base[3],
            "original vertices not preserved");
}

void testInfluence()
{
    require(approx(ClampInfluence(0.0), 1.0) && approx(ClampInfluence(80.0), 50.0) && approx(ClampInfluence(20.0), 20.0),
            "influence clamp wrong");
    require(approx(InfluenceAfterDrag(20.0, -100.0), 30.0), "dragging up does not grow the radius");
    require(approx(InfluenceAfterDrag(20.0, 500.0), 1.0), "dragging down does not clamp at 1 %");
    require(approx(InfluenceWeight(0.0, 10.0), 1.0) && approx(InfluenceWeight(5.0, 10.0), 0.5) &&
                approx(InfluenceWeight(10.0, 10.0), 0.0) && approx(InfluenceWeight(15.0, 10.0), 0.0),
            "cosine falloff wrong");
}

void testDragFalloff()
{
    const auto contour = square();
    const size_t n = contour.size();
    const int grabbed = 40; // (20, 0), middle of the bottom edge
    const auto moved = Drag(contour, grabbed, {0.0, -3.0}, 20.0); // radius 32 mm = 64 vertices
    require(approx(moved[40][0], 20.0) && approx(moved[40][1], -3.0), "grabbed vertex did not follow the delta");

    for (size_t i = 0; i < n; ++i) {
        const double arc = std::min(std::abs(static_cast<double>(i) - grabbed), n - std::abs(static_cast<double>(i) - grabbed)) * 0.5;
        if (arc >= 32.0)
            require(displacement(contour, moved, i) == 0.0, "vertex outside the radius moved");
    }
    const auto wrap = [n](int i) { return static_cast<size_t>((i % static_cast<int>(n) + static_cast<int>(n)) % static_cast<int>(n)); };
    for (int k = 1; k < 64; ++k) {
        const double previous = displacement(contour, moved, wrap(grabbed + k - 1));
        const double current = displacement(contour, moved, wrap(grabbed + k));
        require(current <= previous + 1e-12, "falloff is not monotonic");
        require(approx(current, displacement(contour, moved, wrap(grabbed - k)), 1e-12),
                "falloff is not symmetric");
    }
}

void testDragWrapsAroundStart()
{
    const auto contour = square();
    const auto moved = Drag(contour, 0, {-2.0, -2.0}, 10.0); // radius 16 mm = 32 vertices
    require(approx(moved[0][0], -2.0) && approx(moved[0][1], -2.0), "vertex 0 did not follow the delta");
    const size_t n = contour.size();
    require(displacement(contour, moved, 10) > 0.0 && approx(displacement(contour, moved, 10), displacement(contour, moved, n - 10), 1e-12),
            "influence does not wrap across vertex 0");
    require(displacement(contour, moved, 40) == 0.0 && displacement(contour, moved, n - 40) == 0.0,
            "influence leaked past the radius across vertex 0");
}

void testFindHandle()
{
    const std::vector<SplintContourUV> contours = {square(), Densify({{100, 0}, {110, 0}, {110, 10}}, 1.0)};
    auto handle = FindHandle(contours, {20.2, 0.3}, 1.0);
    require(handle && handle->contour == 0 && handle->vertex == 40, "nearest vertex not found");
    handle = FindHandle(contours, {109.8, 0.1}, 1.0);
    require(handle && handle->contour == 1, "second contour not searched");
    require(!FindHandle(contours, {60.0, 60.0}, 1.0), "far point returned a handle");
    const auto moved = Drag(contours[0], 999, {1.0, 1.0}, 20.0);
    require(moved == contours[0], "invalid vertex modified the contour");
}
} // namespace

int main()
{
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"densify", testDensify},
        {"influence", testInfluence},
        {"drag falloff", testDragFalloff},
        {"drag wraps around start", testDragWrapsAroundStart},
        {"find handle", testFindHandle},
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
