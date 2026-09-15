#include "CompositeBlockCore.h"
#include "OsteotomyCore.h"

#include <vtkAppendPolyData.h>
#include <vtkCleanPolyData.h>
#include <vtkFeatureEdges.h>
#include <vtkMatrix4x4.h>
#include <vtkPlaneSource.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>
#include <vtkTriangleFilter.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

// Finely tessellated box (so clipped cut lines are accurate).
vtkSmartPointer<vtkPolyData> gridBox(double x0, double x1, double y0, double y1, double z0, double z1, double step = 1.5)
{
    auto append = vtkSmartPointer<vtkAppendPolyData>::New();
    const auto face = [&](const OstPoint3& o, const OstPoint3& p1, const OstPoint3& p2) {
        auto plane = vtkSmartPointer<vtkPlaneSource>::New();
        plane->SetOrigin(o[0], o[1], o[2]);
        plane->SetPoint1(p1[0], p1[1], p1[2]);
        plane->SetPoint2(p2[0], p2[1], p2[2]);
        const auto length = [](const OstPoint3& a, const OstPoint3& b) {
            return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
        };
        plane->SetResolution(std::max(1, static_cast<int>(std::ceil(length(o, p1) / step))),
                             std::max(1, static_cast<int>(std::ceil(length(o, p2) / step))));
        plane->Update();
        append->AddInputData(plane->GetOutput());
    };
    face({x0, y0, z0}, {x1, y0, z0}, {x0, y1, z0});
    face({x0, y0, z1}, {x1, y0, z1}, {x0, y1, z1});
    face({x0, y0, z0}, {x1, y0, z0}, {x0, y0, z1});
    face({x0, y1, z0}, {x1, y1, z0}, {x0, y1, z1});
    face({x0, y0, z0}, {x0, y1, z0}, {x0, y0, z1});
    face({x1, y0, z0}, {x1, y1, z0}, {x1, y0, z1});
    append->Update();
    auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
    clean->SetInputConnection(append->GetOutputPort());
    auto tri = vtkSmartPointer<vtkTriangleFilter>::New();
    tri->SetInputConnection(clean->GetOutputPort());
    tri->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(tri->GetOutput());
    return out;
}

bool closedSurface(vtkPolyData* mesh)
{
    if (!mesh || mesh->GetNumberOfPolys() == 0)
        return false;
    auto edges = vtkSmartPointer<vtkFeatureEdges>::New();
    edges->SetInputData(mesh);
    edges->BoundaryEdgesOn();
    edges->NonManifoldEdgesOn();
    edges->FeatureEdgesOff();
    edges->ManifoldEdgesOff();
    edges->Update();
    return edges->GetOutput()->GetNumberOfCells() == 0;
}

void forEachPoint(vtkPolyData* mesh, const std::function<void(const OstPoint3&)>& visit)
{
    OstPoint3 p{};
    for (vtkIdType i = 0; i < mesh->GetNumberOfPoints(); ++i) {
        mesh->GetPoint(i, p.data());
        visit(p);
    }
}

void testLandmarks()
{
    require(OsteotomyCore::Landmarks(OsteotomyType::LeFortI).size() == 4, "Le Fort I needs 4 landmarks");
    require(OsteotomyCore::Landmarks(OsteotomyType::Bsso).size() == 6, "BSSO needs 6 landmarks (3 per side)");
    require(OsteotomyCore::Landmarks(OsteotomyType::Genioplasty).size() == 4, "genioplasty needs 4 landmarks");
    for (OsteotomyType type : {OsteotomyType::LeFortI, OsteotomyType::Bsso, OsteotomyType::Genioplasty}) {
        require(!OsteotomyCore::TypeName(type).isEmpty(), "missing type name");
        for (const OsteotomyLandmark& landmark : OsteotomyCore::Landmarks(type))
            require(!landmark.name.isEmpty() && !landmark.hint.isEmpty(), "landmark without name or hint");
    }
}

const std::array<OstPoint3, 4> kLeFort = {{{-10.0, 35.0, 20.0}, {10.0, 35.0, 21.0}, {-25.0, 10.0, 15.0}, {25.0, 10.0, 14.0}}};

void testLeFortPath()
{
    const OsteotomyPath path = OsteotomyCore::LeFortPath(kLeFort);
    require(path.valid, "Le Fort path rejected: " + path.error.toStdString());
    require(path.widthMm == 120.0 && path.thicknessMm == 1.0 && path.extensionStartMm == 20.0 && path.extensionEndMm == 20.0,
            "Le Fort path does not use the ProPlan defaults");
    for (const OstPoint3& landmark : kLeFort)
        require(std::abs(OsteotomyCore::PathField(path, landmark)) < 1e-6, "the cut does not pass through a landmark");
    require(OsteotomyCore::PathField(path, {0.0, 20.0, 35.0}) > 0.0, "cranial side is not positive");
    require(OsteotomyCore::PathField(path, {0.0, 20.0, 5.0}) < 0.0, "segment side is not negative");
    // Beyond the extensions the cut continues, so the bone is always separated.
    require(OsteotomyCore::PathField(path, {-80.0, 10.0, 40.0}) > 0.0 && OsteotomyCore::PathField(path, {-80.0, 10.0, -10.0}) < 0.0,
            "field beyond the extension has the wrong sign");

    OsteotomyPath swapped = OsteotomyCore::LeFortPath({kLeFort[2], kLeFort[3], kLeFort[0], kLeFort[1]});
    require(swapped.valid, "landmark order must not invalidate the path");

    const auto same = OsteotomyCore::LeFortPath({kLeFort[0], kLeFort[0], kLeFort[0], kLeFort[0]});
    require(!same.valid && !same.error.isEmpty(), "coincident landmarks accepted");

    auto matrix = vtkSmartPointer<vtkMatrix4x4>::New();
    matrix->Identity();
    matrix->SetElement(2, 3, 5.0);
    const OsteotomyPath moved = OsteotomyCore::TransformPath(path, matrix);
    require(moved.valid && OsteotomyCore::PathField(moved, kLeFort[0]) < -4.0, "translated path did not move up");
}

void testLeFortSplitAndGuide()
{
    const OsteotomyPath path = OsteotomyCore::LeFortPath(kLeFort);
    const auto maxilla = CompositeBlockCore::TagPart(gridBox(-30.0, 30.0, 0.0, 40.0, 0.0, 40.0), CompositeBlockCore::BonePart);
    const OsteotomySplitResult split = OsteotomyCore::SplitByPath(maxilla, path);
    require(split.ok, "Le Fort split failed: " + split.error.toStdString());
    require(CompositeBlockCore::HasParts(split.negative) && CompositeBlockCore::HasParts(split.positive),
            "split lost the composite part link");
    double worstNegative = -1e9;
    double worstPositive = 1e9;
    forEachPoint(split.negative, [&](const OstPoint3& p) { worstNegative = std::max(worstNegative, OsteotomyCore::PathField(path, p)); });
    forEachPoint(split.positive, [&](const OstPoint3& p) { worstPositive = std::min(worstPositive, OsteotomyCore::PathField(path, p)); });
    // Cut points are linear interpolations along 1.5 mm edges: allow 0.2 mm at the path corners.
    require(worstNegative <= -0.3 && worstPositive >= 0.3,
            "the split does not leave the 1 mm kerf: " + std::to_string(worstNegative) + " / " + std::to_string(worstPositive));
    double bounds[6];
    split.negative->GetBounds(bounds);
    require(bounds[4] < 0.5 && bounds[5] < 26.0, "Le Fort segment is not the caudal part");
    split.positive->GetBounds(bounds);
    require(bounds[5] > 39.5 && bounds[4] > 8.0, "cranial part is not above the cut");

    const auto guide = OsteotomyCore::PathGuideMesh(path);
    require(closedSurface(guide), "Le Fort guide is not a closed slab");
    guide->GetBounds(bounds);
    require(bounds[1] - bounds[0] > 80.0, "guide does not include the lateral extensions");
    double worst = 0.0;
    forEachPoint(guide, [&](const OstPoint3& p) {
        worst = std::max(worst, std::abs(std::abs(OsteotomyCore::PathField(path, p)) - 0.5));
    });
    require(worst < 1.6, "guide surface is not the kerf of the cut");
}

// A composite that includes the skull base: bone hanging below the Le Fort
// path far behind the maxilla (mastoid-like) must not be cut.
void testLeFortSparesPosteriorBone()
{
    const OsteotomyPath path = OsteotomyCore::LeFortPath(kLeFort);
    for (bool tagged : {true, false}) {
        auto append = vtkSmartPointer<vtkAppendPolyData>::New();
        const auto maxilla = gridBox(-30.0, 30.0, 0.0, 40.0, 0.0, 40.0);
        const auto mastoid = gridBox(40.0, 50.0, -60.0, -50.0, 0.0, 40.0);
        append->AddInputData(tagged ? CompositeBlockCore::TagPart(maxilla, CompositeBlockCore::DentalPart).GetPointer()
                                    : maxilla.GetPointer());
        append->AddInputData(tagged ? CompositeBlockCore::TagPart(mastoid, CompositeBlockCore::BonePart).GetPointer()
                                    : mastoid.GetPointer());
        append->Update();
        const OsteotomySplitResult split = OsteotomyCore::SplitByPath(append->GetOutput(), path);
        const std::string label = tagged ? " (tagged)" : " (untagged)";
        require(split.ok, "split with posterior bone failed" + label + ": " + split.error.toStdString());
        double bounds[6];
        split.negative->GetBounds(bounds);
        require(bounds[1] < 35.0 && bounds[2] > -1.0, "posterior bone was cut into the Le Fort segment" + label);
        split.positive->GetBounds(bounds);
        require(bounds[1] > 49.5 && bounds[4] < 0.5, "posterior bone lost its lower part" + label);
    }
}

void testGenioPath()
{
    const std::array<OstPoint3, 4> points = {{{-15.0, 40.0, 20.0}, {-15.0, 30.0, 0.0}, {15.0, 40.0, 20.0}, {15.0, 30.0, 0.0}}};
    const OsteotomyPath path = OsteotomyCore::GenioPath(points);
    require(path.valid, "genioplasty path rejected: " + path.error.toStdString());
    require(path.widthMm == 50.0, "genioplasty path does not use the ProPlan width");
    require(OsteotomyCore::PathField(path, {0.0, 45.0, 5.0}) < 0.0, "chin is not the segment side");
    require(OsteotomyCore::PathField(path, {0.0, 10.0, 10.0}) > 0.0, "mandibular body is not the remaining side");
    for (const OstPoint3& p : points)
        require(std::abs(OsteotomyCore::PathField(path, p)) < 1e-6, "the genioplasty cut misses a landmark");
    require(closedSurface(OsteotomyCore::PathGuideMesh(path)), "genioplasty guide is not closed");
}

BssoPlan bssoPlan()
{
    BssoPlan plan;
    plan.right = {{20.0, -25.0, 30.0}, {25.0, -8.0, 20.0}, {30.0, 10.0, 5.0}};
    plan.left = {{-20.0, -25.0, 30.0}, {-25.0, -8.0, 20.0}, {-30.0, 10.0, 5.0}};
    return plan;
}

vtkSmartPointer<vtkPolyData> mandible()
{
    auto append = vtkSmartPointer<vtkAppendPolyData>::New();
    for (double sign : {1.0, -1.0}) {
        const double a = 20.0 * sign;
        const double b = 30.0 * sign;
        append->AddInputData(gridBox(std::min(a, b), std::max(a, b), -10.0, 40.0, 0.0, 20.0));  // body
        append->AddInputData(gridBox(std::min(a, b), std::max(a, b), -40.0, -10.0, 0.0, 60.0)); // ramus
    }
    append->AddInputData(gridBox(-30.0, 30.0, 30.0, 40.0, 0.0, 20.0)); // chin
    append->Update();
    return CompositeBlockCore::TagPart(append->GetOutput(), CompositeBlockCore::BonePart);
}

void testBssoField()
{
    const BssoPlan plan = bssoPlan();
    const BssoSidePlanes right = OsteotomyCore::BssoPlanes(plan, false);
    const BssoSidePlanes left = OsteotomyCore::BssoPlanes(plan, true);
    require(right.valid && left.valid, "BSSO planes rejected: " + right.error.toStdString() + left.error.toStdString());
    const auto field = [&](const BssoSidePlanes& side, double x, double y, double z) {
        return OsteotomyCore::BssoField(side, {x, y, z});
    };
    require(field(right, 25.0, -35.0, 55.0) < 0.0, "right condyle is not in the right proximal segment");
    require(field(right, 29.5, 0.0, 12.0) < 0.0, "right buccal plate behind the vertical cut is not proximal");
    require(field(right, 21.0, 0.0, 10.0) > 0.0, "right lingual cortex below the lingula must stay distal");
    require(field(right, 21.0, -30.0, 45.0) < 0.0, "right ramus above the lingula is not proximal");
    require(field(right, 25.0, 25.0, 10.0) > 0.0, "body in front of the vertical cut must stay distal");
    require(field(right, 0.0, 35.0, 10.0) > 0.0 && field(left, 0.0, 35.0, 10.0) > 0.0, "chin is not distal");
    require(field(right, -25.0, -35.0, 55.0) > 0.0, "left condyle leaked into the right proximal segment");
    require(field(left, -25.0, -35.0, 55.0) < 0.0, "left condyle is not in the left proximal segment");

    BssoPlan collapsed = plan;
    collapsed.left = collapsed.right;
    require(!OsteotomyCore::BssoPlanes(collapsed, false).valid, "identical sides accepted");
}

void testBssoSplit()
{
    const BssoPlan plan = bssoPlan();
    const BssoSplitResult split = OsteotomyCore::SplitBsso(mandible(), plan);
    require(split.ok, "BSSO split failed: " + split.error.toStdString());
    double bounds[6];
    split.proximalRight->GetBounds(bounds);
    require(bounds[0] > 15.0 && bounds[2] < -39.0 && bounds[5] > 59.0, "right proximal segment is wrong");
    split.proximalLeft->GetBounds(bounds);
    require(bounds[1] < -15.0 && bounds[2] < -39.0 && bounds[5] > 59.0, "left proximal segment is wrong");
    split.distal->GetBounds(bounds);
    require(bounds[3] > 39.0 && bounds[0] < -20.0 && bounds[1] > 20.0 && bounds[5] < 32.0,
            "distal segment does not keep the body and chin only");
    for (vtkPolyData* piece : {split.proximalRight.GetPointer(), split.proximalLeft.GetPointer(), split.distal.GetPointer()})
        require(CompositeBlockCore::HasParts(piece), "BSSO split lost the composite part link");

    const BssoSidePlanes right = OsteotomyCore::BssoPlanes(plan, false);
    double worst = -1e9;
    forEachPoint(split.proximalRight, [&](const OstPoint3& p) { worst = std::max(worst, OsteotomyCore::BssoField(right, p)); });
    // Linear interpolation along 1.5 mm edges at the plane intersections: allow 0.25 mm.
    require(worst <= -0.25, "right proximal segment crosses the kerf: " + std::to_string(worst));

    for (bool leftSide : {false, true})
        require(closedSurface(OsteotomyCore::BssoGuideMesh(plan, leftSide)), "BSSO guide is not a closed slab");

    auto matrix = vtkSmartPointer<vtkMatrix4x4>::New();
    matrix->Identity();
    matrix->SetElement(1, 3, -3.0);
    const BssoPlan moved = OsteotomyCore::TransformBsso(plan, matrix);
    require(std::abs(moved.right.body[1] - (plan.right.body[1] - 3.0)) < 1e-9, "BSSO landmarks did not follow the gizmo");
    const BssoPlan rightOnly = OsteotomyCore::TransformBsso(plan, matrix, true, false);
    require(std::abs(rightOnly.right.body[1] - (plan.right.body[1] - 3.0)) < 1e-9 && rightOnly.left.body == plan.left.body,
            "moving the right BSSO cut moved the left side");
}
} // namespace

void testLandmarkMovement()
{
    const auto segment = gridBox(-20, 20, 0, 30, -10, 0);
    const std::vector<OstPoint3> landmarks = {{-15.0, 25.0, 0.0}, {15.0, 25.0, 0.0}};
    const SegmentReference reference = OsteotomyCore::CaptureSegmentReference(segment, landmarks);
    require(reference.ids.size() == 64 && reference.points.size() == 64, "segment reference not sampled");

    auto transform = vtkSmartPointer<vtkTransform>::New();
    transform->Translate(0.0, 4.0, 3.0);
    transform->RotateY(5.0);
    auto filter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
    filter->SetInputData(segment);
    filter->SetTransform(transform);
    filter->Update();
    const LandmarkMovement movement = OsteotomyCore::MeasureLandmarkMovement(reference, filter->GetOutput());
    require(movement.valid && movement.rmsMm < 1e-3 && movement.displacements.size() == 2, "rigid movement not measured");
    for (size_t i = 0; i < landmarks.size(); ++i) {
        double expected[3];
        transform->TransformPoint(landmarks[i].data(), expected);
        for (int a = 0; a < 3; ++a)
            require(std::abs(movement.displacements[i][a] - (expected[a] - landmarks[i][a])) < 1e-3,
                    "landmark displacement is wrong");
    }
    const auto identity = OsteotomyCore::MeasureLandmarkMovement(reference, segment);
    require(identity.valid && std::abs(identity.displacements[0][2]) < 1e-6, "unmoved segment reports movement");
    require(!OsteotomyCore::MeasureLandmarkMovement(reference, gridBox(-30, 30, 0, 30, -10, 0)).valid,
            "a different mesh was accepted as the moved segment");
}

int main()
{
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"landmark movement", testLandmarkMovement},
        {"landmarks", testLandmarks},
        {"Le Fort path", testLeFortPath},
        {"Le Fort split and guide", testLeFortSplitAndGuide},
        {"Le Fort spares posterior bone", testLeFortSparesPosteriorBone},
        {"genioplasty path", testGenioPath},
        {"BSSO field", testBssoField},
        {"BSSO split", testBssoSplit},
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
