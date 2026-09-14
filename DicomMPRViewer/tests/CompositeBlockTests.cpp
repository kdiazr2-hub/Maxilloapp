#include "CompositeBlockCore.h"
#include "SplintTestGeometry.h"

#include <vtkCellData.h>
#include <vtkClipPolyData.h>
#include <vtkFeatureEdges.h>
#include <vtkMassProperties.h>
#include <vtkMatrix4x4.h>
#include <vtkPlane.h>
#include <vtkPolyDataConnectivityFilter.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>

#include <functional>
#include <iostream>

namespace
{
using namespace splinttest;
using namespace CompositeBlockCore;

vtkIdType openEdges(vtkPolyData* mesh)
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

double volumeOf(vtkPolyData* mesh)
{
    auto mass = vtkSmartPointer<vtkMassProperties>::New();
    mass->SetInputData(mesh);
    mass->Update();
    return mass->GetVolume();
}

int regions(vtkPolyData* mesh)
{
    auto connectivity = vtkSmartPointer<vtkPolyDataConnectivityFilter>::New();
    connectivity->SetInputData(mesh);
    connectivity->SetExtractionModeToAllRegions();
    connectivity->Update();
    return connectivity->GetNumberOfExtractedRegions();
}

void cellCentroid(vtkPolyData* mesh, vtkIdType cellId, double c[3])
{
    vtkIdType npts = 0;
    const vtkIdType* ids = nullptr;
    mesh->GetCellPoints(cellId, npts, ids);
    c[0] = c[1] = c[2] = 0.0;
    double p[3] = {};
    for (vtkIdType k = 0; k < npts; ++k) {
        mesh->GetPoint(ids[k], p);
        for (int a = 0; a < 3; ++a)
            c[a] += p[a] / static_cast<double>(npts);
    }
}

vtkSmartPointer<vtkPolyData> transformed(vtkPolyData* mesh, vtkTransform* transform)
{
    auto filter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
    filter->SetInputData(mesh);
    filter->SetTransform(transform);
    filter->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(filter->GetOutput());
    return out;
}

// Maxilla-like bone slab overlapping the crowns (CT teeth inside the bone mesh).
vtkSmartPointer<vtkPolyData> bone()
{
    return boxMesh({-35, 35, -10, 35, 4, 30}, false, false);
}

void testInitialBlock()
{
    const auto dental = upperTeeth({});
    const CompositeCutBlock block = InitialBlock(dental, bone(), 15.0);
    require(block.valid, "initial block not created");
    require(block.axisZ[2] > 0.99, "block normal does not point from the teeth to the bone");
    require(std::abs(block.sizeMm[2] - 15.0) < 1e-9, "default thickness is not 15 mm");
    require(std::abs(block.center[2] - 7.5) < 1e-6, "block does not start 1 mm past the cusps");
    double p[3] = {};
    for (vtkIdType i = 0; i < dental->GetNumberOfPoints(); ++i) {
        dental->GetPoint(i, p);
        require(Contains(block, p, 1e-6), "initial block does not cover the dental scan");
    }
    QString error;
    require(!InitialBlock(nullptr, bone(), 15.0, &error).valid && !error.isEmpty(), "missing scan not reported");
}

void testCutKeepsRegions()
{
    const auto dental = upperTeeth({});
    const CompositeCutBlock block = InitialBlock(dental, bone(), 15.0);
    const CompositeBlockResult result = CreateBlockComposite(bone(), dental, block);
    require(result.ok, "composite failed: " + result.error.toStdString());
    require(HasParts(result.composite), "composite has no part tags");
    require(result.composite->GetNumberOfPolys() == result.boneCells + result.dentalCells, "cells lost when merging parts");

    const auto bonePart = ExtractPart(result.composite, BonePart);
    const auto dentalPart = ExtractPart(result.composite, DentalPart);
    require(bonePart && dentalPart, "parts cannot be extracted");
    double c[3] = {};
    for (vtkIdType i = 0; i < bonePart->GetNumberOfCells(); ++i) {
        cellCentroid(bonePart, i, c);
        require(!Contains(block, c, -1e-6), "bone kept inside the block");
    }
    for (vtkIdType i = 0; i < dentalPart->GetNumberOfCells(); ++i) {
        cellCentroid(dentalPart, i, c);
        require(Contains(block, c, 1e-6), "dental scan kept outside the block");
    }
    require(dentalPart->GetNumberOfCells() == upperTeeth({})->GetNumberOfPolys(), "teeth inside the block were cut");
    double b[6] = {};
    bonePart->GetBounds(b);
    require(std::abs(b[5] - 30.0) < 1e-6, "bone above the block was removed");

    CompositeCutBlock away = block;
    away.center[2] += 100.0;
    require(!CreateBlockComposite(bone(), dental, away).ok, "block without teeth accepted");
}

void testLinkFollowsTransformAndCuts()
{
    const auto dental = upperTeeth({});
    const auto composite = CreateBlockComposite(bone(), dental, InitialBlock(dental, bone(), 15.0)).composite;

    auto motion = vtkSmartPointer<vtkTransform>::New();
    motion->Translate(5.0, -3.0, 2.0);
    motion->RotateZ(20.0);
    const auto moved = transformed(composite, motion);
    const auto movedDental = ExtractPart(moved, DentalPart);
    const auto expected = transformed(ExtractPart(composite, DentalPart), motion);
    require(movedDental && movedDental->GetNumberOfCells() == expected->GetNumberOfCells(),
            "dental part lost after moving the composite");
    double b1[6] = {}, b2[6] = {};
    movedDental->GetBounds(b1);
    expected->GetBounds(b2);
    for (int i = 0; i < 6; ++i)
        require(std::abs(b1[i] - b2[i]) < 1e-6, "dental part did not follow the bone transform");

    // Le Fort-like plane cut: both pieces stay tagged; only the tooth-bearing one has scan.
    auto plane = vtkSmartPointer<vtkPlane>::New();
    plane->SetOrigin(0.0, 0.0, 12.0);
    plane->SetNormal(0.0, 0.0, -1.0);
    auto clip = vtkSmartPointer<vtkClipPolyData>::New();
    clip->SetInputData(composite);
    clip->SetClipFunction(plane);
    clip->GenerateClippedOutputOn();
    clip->Update();
    vtkPolyData* segment = clip->GetOutput();      // z < 12
    vtkPolyData* cranial = clip->GetClippedOutput(); // z > 12
    require(HasParts(segment) && HasParts(cranial), "plane cut dropped the part tags");
    require(ExtractPart(segment, DentalPart) != nullptr, "tooth-bearing segment lost its scan");
    require(ExtractPart(cranial, DentalPart) == nullptr, "cranial piece kept dental scan");
    require(ExtractPart(cranial, BonePart) != nullptr, "cranial piece lost its bone");
    require(ExtractPart(upperTeeth({}), DentalPart) == nullptr, "untagged mesh reported parts");
}

void testBlockTransformAndJson()
{
    CompositeCutBlock block;
    block.sizeMm = {10.0, 20.0, 5.0};
    block.valid = true;
    auto m = vtkSmartPointer<vtkTransform>::New();
    m->Translate(3.0, 4.0, 5.0);
    m->RotateZ(90.0);
    m->Scale(2.0, 1.0, 1.0);
    const CompositeCutBlock out = TransformBlock(block, m->GetMatrix());
    const auto same3 = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
        return std::abs(a[0] - b[0]) < 1e-9 && std::abs(a[1] - b[1]) < 1e-9 && std::abs(a[2] - b[2]) < 1e-9;
    };
    require(same3(out.center, {3, 4, 5}), "block center not transformed");
    require(same3(out.axisX, {0, 1, 0}) && same3(out.axisY, {-1, 0, 0}) && same3(out.axisZ, {0, 0, 1}), "block axes not rotated");
    require(same3(out.sizeMm, {20, 20, 5}), "block size not scaled along its axis");

    const CompositeCutBlock resized = WithSize(out, 30.0, 25.0, 12.0);
    require(same3(resized.sizeMm, {30, 25, 12}) && same3(resized.center, out.center), "resize changed more than the size");

    const CompositeCutBlock restored = BlockFromJson(BlockToJson(resized));
    require(restored.valid && same3(restored.center, resized.center) && same3(restored.axisX, resized.axisX) &&
                same3(restored.axisZ, resized.axisZ) && same3(restored.sizeMm, resized.sizeMm),
            "block JSON round trip failed");
    require(!BlockFromJson(QJsonObject{}).valid, "empty JSON produced a valid block");
}

void testSliceContour()
{
    const auto cube = boxMesh({-5, 5, -5, 5, -5, 5}, false, false);
    const auto lines = SliceContour(cube, 2, 0.0);
    require(lines->GetNumberOfLines() > 0, "slice through the cube has no contour");
    double b[6] = {};
    lines->GetBounds(b);
    require(std::abs(b[0] + 5) < 1e-6 && std::abs(b[1] - 5) < 1e-6 && std::abs(b[4]) < 1e-9 && std::abs(b[5]) < 1e-9,
            "slice contour not on the cube section");
    require(SliceContour(cube, 2, 10.0)->GetNumberOfLines() == 0, "slice outside the mesh produced lines");
}

void testVoxelUnion()
{
    const auto a = boxMesh({0, 10, 0, 10, 0, 10}, false, false);
    const auto b = boxMesh({5, 15, 0, 10, 0, 10}, false, false);
    const VoxelUnionResult overlap = VoxelUnion({a, b}, 0.25, 2);
    require(overlap.ok, "voxel union failed: " + overlap.error.toStdString());
    require(openEdges(overlap.mesh) == 0, "voxel union is not closed");
    const double volume = volumeOf(overlap.mesh);
    std::cout << "  union volume " << volume << " mm3 (expected 1500)\n";
    require(std::abs(volume / 1500.0 - 1.0) < 0.12, "voxel union volume is wrong");
    require(regions(overlap.mesh) == 1, "overlapping parts not fused");

    const auto c = boxMesh({10.8, 20, 0, 10, 0, 10}, false, false); // 0.8 mm gap
    const VoxelUnionResult gap = VoxelUnion({a, c}, 0.25, 2);
    require(gap.ok && openEdges(gap.mesh) == 0 && regions(gap.mesh) == 1, "small gap was not sealed");
}
} // namespace

int main()
{
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"initial block", testInitialBlock},
        {"cut keeps regions", testCutKeepsRegions},
        {"link follows transform and cuts", testLinkFollowsTransformAndCuts},
        {"block transform and json", testBlockTransformAndJson},
        {"slice contour", testSliceContour},
        {"voxel union", testVoxelUnion},
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
