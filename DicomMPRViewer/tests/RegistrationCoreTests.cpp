#include "RegistrationCore.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

#include <vtkAppendPolyData.h>
#include <vtkCleanPolyData.h>
#include <vtkMatrix4x4.h>
#include <vtkPoints.h>
#include <vtkTessellatedBoxSource.h>
#include <vtkTransform.h>
#include <vtkTriangleFilter.h>

namespace {

using RegistrationCore::Point;

constexpr double kPi = 3.14159265358979323846;

int fail(const std::string& message)
{
    std::cerr << "FAIL: " << message << "\n";
    return 1;
}

// Twelve box teeth on a U opening towards -y, crowns from z = 1 up to a height that grows along the arch so the
// arch has no symmetry ICP could slide along. Tessellated so every face has points for ICP to match.
vtkSmartPointer<vtkPolyData> syntheticArch()
{
    auto append = vtkSmartPointer<vtkAppendPolyData>::New();
    for (int i = 0; i < 12; ++i) {
        const double angle = (-75.0 + 150.0 * i / 11.0) * kPi / 180.0;
        const double x = 24.0 * std::sin(angle);
        const double y = 20.0 * std::cos(angle);
        auto box = vtkSmartPointer<vtkTessellatedBoxSource>::New();
        box->SetBounds(x - 3.0, x + 3.0, y - 3.0, y + 3.0, 1.0, 7.0 + 0.4 * i);
        box->SetLevel(4);
        box->QuadsOff();
        box->Update();
        append->AddInputData(box->GetOutput());
    }
    auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
    clean->SetInputConnection(append->GetOutputPort());
    auto tri = vtkSmartPointer<vtkTriangleFilter>::New();
    tri->SetInputConnection(clean->GetOutputPort());
    tri->Update();
    auto mesh = vtkSmartPointer<vtkPolyData>::New();
    mesh->DeepCopy(tri->GetOutput());
    return mesh;
}

vtkSmartPointer<vtkMatrix4x4> smallMotion()
{
    auto transform = vtkSmartPointer<vtkTransform>::New();
    transform->PostMultiply();
    transform->RotateZ(2.0);
    transform->RotateX(1.0);
    transform->Translate(0.8, -0.6, 0.5);
    auto matrix = vtkSmartPointer<vtkMatrix4x4>::New();
    matrix->DeepCopy(transform->GetMatrix());
    return matrix;
}

Point apply(vtkMatrix4x4* matrix, const Point& p)
{
    const double in[4] = {p[0], p[1], p[2], 1.0};
    double out[4] = {};
    matrix->MultiplyPoint(in, out);
    return {out[0], out[1], out[2]};
}

double distance(const Point& a, const Point& b)
{
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

// How far the motion `undo · motion` takes a point of the arch from where it started: zero when undo inverts motion.
double residualMm(vtkMatrix4x4* undo, vtkMatrix4x4* motion)
{
    auto both = vtkSmartPointer<vtkMatrix4x4>::New();
    vtkMatrix4x4::Multiply4x4(undo, motion, both);
    double worst = 0.0;
    for (const Point& p : {Point{-24.0, 5.0, 4.0}, Point{24.0, 5.0, 4.0}, Point{0.0, 20.0, 9.0}, Point{0.0, 0.0, 0.0}})
        worst = std::max(worst, distance(apply(both, p), p));
    return worst;
}

bool isIdentity(vtkMatrix4x4* matrix)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            if (std::abs(matrix->GetElement(r, c) - (r == c ? 1.0 : 0.0)) > 1e-12)
                return false;
    return true;
}

} // namespace

int main()
{
    const auto motion = smallMotion();

    // Paired points: the fit recovers a known motion exactly and needs three pairs.
    {
        const std::vector<Point> fixed = {{-20, 8, 5}, {20, 8, 5}, {0, 21, 9}, {-10, 17, 2}};
        std::vector<Point> moving;
        auto inverse = vtkSmartPointer<vtkMatrix4x4>::New();
        vtkMatrix4x4::Invert(motion, inverse);
        for (const Point& p : fixed)
            moving.push_back(apply(inverse, p));
        const auto fit = RegistrationCore::FitLandmarks(moving, fixed);
        if (!fit.valid || fit.pairs != 4)
            return fail("four pairs must give a valid fit of four pairs");
        if (fit.rmsMm > 1e-6 || fit.maxMm > 1e-6)
            return fail("exact pairs must fit with no residual");
        if (residualMm(fit.matrix, inverse) > 1e-6)
            return fail("the fit must recover the motion the pairs were made with");

        const auto twoPairs = RegistrationCore::FitLandmarks({moving[0], moving[1]}, {fixed[0], fixed[1]});
        if (twoPairs.valid || !isIdentity(twoPairs.matrix))
            return fail("two pairs must be refused with an identity matrix");

        const auto unequal = RegistrationCore::FitLandmarks(moving, {fixed[0], fixed[1], fixed[2]});
        if (!unequal.valid || unequal.pairs != 3)
            return fail("unequal lists must use the shorter one");
        std::cout << "PASS landmark fit\n";
    }

    const auto arch = syntheticArch();
    if (arch->GetNumberOfPoints() < 800)
        return fail("the synthetic arch is too coarse for ICP");

    // Apply moves the mesh and leaves the input alone.
    {
        double before[3] = {};
        arch->GetPoint(0, before);
        const auto moved = RegistrationCore::Apply(arch, motion);
        double after[3] = {};
        moved->GetPoint(0, after);
        const Point expected = apply(motion, {before[0], before[1], before[2]});
        if (distance({after[0], after[1], after[2]}, expected) > 1e-9)
            return fail("Apply must move every point by the matrix");
        double still[3] = {};
        arch->GetPoint(0, still);
        if (still[0] != before[0] || still[1] != before[1] || still[2] != before[2])
            return fail("Apply must not modify its input");
        if (RegistrationCore::Apply(nullptr, motion))
            return fail("Apply of no mesh must be nullptr");
        std::cout << "PASS apply\n";
    }

    // ROI building blocks.
    {
        const auto front = RegistrationCore::CropCellsByBox(arch, {-6, 6, 14, 30, -10, 30});
        if (!front || front->GetNumberOfCells() == 0 || front->GetNumberOfCells() >= arch->GetNumberOfCells())
            return fail("a box round the incisors must keep some cells, not all");
        double b[6] = {};
        front->GetBounds(b);
        // A centroid inside the box puts the triangle's vertices at most one tessellation cell (1.5 mm) outside it.
        if (b[0] < -6.0 - 2.0 || b[1] > 6.0 + 2.0 || b[2] < 14.0 - 2.0)
            return fail("cropped cells must have their centroid inside the box");

        const auto nearIncisor = RegistrationCore::KeepCellsNearPoints(arch, {{0.0, 23.0, 4.0}}, 3.0);
        if (!nearIncisor || nearIncisor->GetNumberOfCells() == 0
            || nearIncisor->GetNumberOfCells() >= front->GetNumberOfCells())
            return fail("cells near one point must be fewer than the front of the arch");
        if (RegistrationCore::KeepCellsNearPoints(arch, {}, 3.0))
            return fail("no points must select nothing");

        const auto nearFront = RegistrationCore::KeepCellsNearSurface(arch, front, 0.5);
        if (!nearFront || nearFront->GetNumberOfCells() < front->GetNumberOfCells()
            || nearFront->GetNumberOfCells() >= arch->GetNumberOfCells())
            return fail("cells near the front must include the front and not the molars");
        std::cout << "PASS roi building blocks\n";
    }

    // Arch ROI: wide across, deep towards the bone, shallow towards the bite.
    {
        const double bounds[6] = {-25, 25, -5, 35, 0, 10}; // largest range 50 mm
        const auto upper = RegistrationCore::ArchRoi(bounds, true);
        const auto lower = RegistrationCore::ArchRoi(bounds, false);
        if (std::abs(upper[0] - (-37.0)) > 1e-9 || std::abs(upper[1] - 37.0) > 1e-9)
            return fail("the arch ROI must be 12 mm wider across (lower clamp of 18 %)");
        if (std::abs(upper[4] - (-5.0)) > 1e-9 || std::abs(upper[5] - 24.0) > 1e-9)
            return fail("the upper arch ROI must go 14 mm up to the maxilla and 5 mm down to the bite");
        if (std::abs(lower[4] - (-14.0)) > 1e-9 || std::abs(lower[5] - 15.0) > 1e-9)
            return fail("the lower arch ROI must go 14 mm down to the mandible and 5 mm up to the bite");
        std::cout << "PASS arch roi\n";
    }

    const auto displaced = RegistrationCore::Apply(arch, motion);
    auto identity = vtkSmartPointer<vtkMatrix4x4>::New();
    identity->Identity();
    const double startMm = residualMm(identity, motion); // ~1.8 mm at the molars

    // MODELOS: an intraoral scan a millimetre and two degrees off its bone comes back onto it.
    {
        const auto result = RegistrationCore::RefineArch(displaced, arch, true);
        if (result.status != RegistrationCore::IcpStatus::Ok || !result.mesh)
            return fail("arch ICP on overlapping surfaces must succeed");
        if (result.fit.rmseMm > 0.2)
            return fail("arch ICP must leave the scan within 0.2 mm RMSE of the bone, got "
                        + std::to_string(result.fit.rmseMm));
        if (residualMm(result.matrix, motion) > 0.25 * startMm)
            return fail("arch ICP must undo at least three quarters of the displacement, residual "
                        + std::to_string(residualMm(result.matrix, motion)) + " mm");
        if (result.fit.within1MmPercent < 99.0)
            return fail("almost every ROI point must end within 1 mm of the bone");

        auto farAway = vtkSmartPointer<vtkMatrix4x4>::New();
        farAway->Identity();
        farAway->SetElement(0, 3, 300.0);
        const auto lost = RegistrationCore::RefineArch(displaced, RegistrationCore::Apply(arch, farAway), true);
        if (lost.status != RegistrationCore::IcpStatus::RoiTooSmall || !lost.mesh || !isIdentity(lost.matrix))
            return fail("bone outside the ROI must skip ICP, keep the scan and give identity");

        if (RegistrationCore::RefineArch(nullptr, arch, true).status != RegistrationCore::IcpStatus::EmptyInput)
            return fail("no scan must be reported as empty input");
        std::cout << "PASS arch icp\n";
    }

    // Bite: the same, restricted to the surgeon's points.
    {
        std::vector<Point> targetPoints = {{-20, 10, 8}, {0, 23, 9}, {20, 10, 9}};
        std::vector<Point> movingPoints;
        for (const Point& p : targetPoints)
            movingPoints.push_back(apply(motion, p));
        const auto result = RegistrationCore::RefineBite(displaced, arch, movingPoints, targetPoints);
        if (result.status != RegistrationCore::IcpStatus::Ok || !result.mesh)
            return fail("bite ICP on overlapping surfaces must succeed");
        if (result.fit.rmseMm > 0.2)
            return fail("bite ICP must leave the scan within 0.2 mm RMSE, got " + std::to_string(result.fit.rmseMm));
        if (residualMm(result.matrix, motion) > 0.25 * startMm)
            return fail("bite ICP must undo at least three quarters of the displacement, residual "
                        + std::to_string(residualMm(result.matrix, motion)) + " mm");
        if (result.mesh->GetNumberOfPoints() < displaced->GetNumberOfPoints() / 2)
            return fail("bite ICP must return the whole moving mesh, not its ROI");

        if (RegistrationCore::RefineBite(displaced, nullptr, movingPoints, targetPoints).status
            != RegistrationCore::IcpStatus::EmptyInput)
            return fail("no bite scan must be reported as empty input");
        std::cout << "PASS bite icp\n";
    }

    std::cout << "RegistrationCoreTests OK\n";
    return 0;
}
