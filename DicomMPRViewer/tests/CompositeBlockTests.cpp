#include "CompositeBlockCore.h"
#include "SplintTestGeometry.h"

#include <vtkAppendPolyData.h>
#include <vtkCellArray.h>
#include <vtkPoints.h>
#include <vtkCellData.h>
#include <vtkClipPolyData.h>
#include <vtkFeatureEdges.h>
#include <vtkImplicitPolyDataDistance.h>
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

// U-shaped contour around the synthetic arch: outer loop then inner loop back.
CompositeContour archContour(double z)
{
    CompositeContour contour;
    for (int i = 0; i <= 5; ++i) {
        const double t = kPi * i / 5.0;
        contour.push_back({(kArchRx + 5.0) * std::cos(t), (kArchRy + 5.0) * std::sin(t), z});
    }
    for (int i = 5; i >= 0; --i) {
        const double t = kPi * i / 5.0;
        contour.push_back({(kArchRx - 6.0) * std::cos(t), (kArchRy - 6.0) * std::sin(t), z});
    }
    return contour;
}

void testContourComposite()
{
    const auto dental = upperTeeth({});
    const CompositeCutBlock block = InitialBlock(dental, bone(), 15.0);
    double teethTop[6] = {};
    dental->GetBounds(teethTop);
    const double gingiva = teethTop[5];
    // Points on the scan border, just above the synthetic crowns so no vertex lies exactly on the line.
    const CompositeContour contour = archContour(gingiva + 0.3);
    QString error;
    require(ContourValid(block, contour, &error), "arch contour rejected: " + error.toStdString());
    const CompositeBlockResult result = CreateContourComposite(bone(), dental, block, contour);
    require(result.ok, "contour composite failed: " + result.error.toStdString());
    require(HasParts(result.composite), "contour composite has no part tags");

    const auto bonePart = ExtractPart(result.composite, BonePart);
    const auto dentalPart = ExtractPart(result.composite, DentalPart);
    require(bonePart && dentalPart, "contour parts cannot be extracted");
    // Edges near the contour are refined to 1 mm, so cut cells may straddle it by half a millimetre.
    double c[3] = {};
    for (vtkIdType i = 0; i < dentalPart->GetNumberOfCells(); ++i) {
        cellCentroid(dentalPart, i, c);
        require(ContourScanField(block, contour, c) <= 0.5, "dental scan kept outside the contour");
    }
    // Inside the region, bone may only remain away from the scan (it is replaced near the scan surface).
    auto keptScanDistance = vtkSmartPointer<vtkImplicitPolyDataDistance>::New();
    keptScanDistance->SetInput(dentalPart);
    for (vtkIdType i = 0; i < bonePart->GetNumberOfCells(); ++i) {
        cellCentroid(bonePart, i, c);
        require(!(ContourField(block, contour, c) < -0.5 &&
                  std::abs(keptScanDistance->EvaluateFunction(c)) < ContourNearScanMm - 0.5),
                "bone kept next to the scan inside the contour");
    }
    const auto dentalBounds = [](vtkPolyData* mesh) {
        std::array<double, 6> b{};
        mesh->GetBounds(b.data());
        return b;
    };
    const auto teethBounds = dentalBounds(upperTeeth({}));
    const auto keptBounds = dentalBounds(dentalPart);
    for (size_t k = 0; k < 6; ++k)
        require(std::abs(teethBounds[k] - keptBounds[k]) < 1e-6, "teeth inside the contour were lost");
    double b[6] = {};
    bonePart->GetBounds(b);
    require(std::abs(b[5] - 30.0) < 1e-6, "bone above the contour thickness was removed");
    double center[3] = {0.0, 0.0, gingiva + 10.0}; // palate vault above the line of the points
    require(ContourField(block, contour, center) > 0.0, "bone above the line of the points was replaced");

    // The whole scan inside the outline is used, also with a thin block (not only the occlusal part).
    const CompositeCutBlock thin = WithSize(block, block.sizeMm[0], block.sizeMm[1], 2.0);
    const CompositeBlockResult thinResult = CreateContourComposite(bone(), dental, thin, contour);
    require(thinResult.ok, "thin-block contour composite failed: " + thinResult.error.toStdString());
    const auto thinBounds = dentalBounds(ExtractPart(thinResult.composite, DentalPart));
    for (size_t k = 0; k < 6; ++k)
        require(std::abs(teethBounds[k] - thinBounds[k]) < 1e-6, "only part of the scan inside the outline was used");
    // Bone is replaced only up to the line of the points: CT crowns below go, alveolar bone above stays.
    const double belowLine[3] = {0.0, kArchRy, gingiva - 2.0}; // front of the arch
    const double aboveLine[3] = {0.0, kArchRy, gingiva + 3.0};
    require(ContourField(block, contour, belowLine) < 0.0 && ContourField(block, contour, aboveLine) > 0.0,
            "bone replacement does not stop at the line of the points");

    // Buccal and palatal points in any order give the same composite.
    CompositeContour mixed;
    for (size_t i = 0; i < contour.size() / 2; ++i) {
        mixed.push_back(contour[i]);
        mixed.push_back(contour[contour.size() - 1 - i]);
    }
    const CompositeBlockResult mixedResult = CreateContourComposite(bone(), dental, block, mixed);
    require(mixedResult.ok && mixedResult.dentalCells == result.dentalCells && mixedResult.boneCells == result.boneCells,
            "the order of the contour points changed the composite");
    // Mucosa above the line (vestibule) is not taken from the scan.
    auto withVestibule = vtkSmartPointer<vtkAppendPolyData>::New();
    withVestibule->AddInputData(dental);
    withVestibule->AddInputData(boxMesh({-30.0, 30.0, -6.0, 30.0, gingiva + 3.0, gingiva + 4.0}, false, false));
    withVestibule->Update();
    const CompositeBlockResult vestibuleResult = CreateContourComposite(bone(), withVestibule->GetOutput(), block, contour);
    require(vestibuleResult.ok, "contour composite with vestibule failed: " + vestibuleResult.error.toStdString());
    const auto vestibulePart = ExtractPart(vestibuleResult.composite, DentalPart);
    bool vestibuleKept = false;
    for (vtkIdType i = 0; i < vestibulePart->GetNumberOfCells(); ++i) {
        cellCentroid(vestibulePart, i, c);
        vestibuleKept = vestibuleKept || (c[1] < -4.0 && c[2] > gingiva + 2.0);
    }
    require(!vestibuleKept, "scan above the line of the points was kept");

    // An open upper border is blended toward the bone, past the old 2 mm lip, without crossing the palate.
    ArchOptions openOptions;
    openOptions.openScan = true;
    const CompositeBlockResult openResult = CreateContourComposite(bone(), upperTeeth(openOptions), block, contour);
    require(openResult.ok, "open-scan contour composite failed: " + openResult.error.toStdString());
    const auto openPart = ExtractPart(openResult.composite, DentalPart);
    const auto openBounds = dentalBounds(openPart);
    require(openBounds[5] > gingiva + 1.8 && openBounds[5] < gingiva + 3.2, "the open upper border does not rise toward the bone");
    vtkIdType blendBand = 0;
    for (vtkIdType i = 0; i < openPart->GetNumberOfPoints(); ++i) {
        double p[3] = {};
        openPart->GetPoint(i, p);
        if (p[2] > gingiva + 0.4 && p[2] < gingiva + 2.2)
            ++blendBand;
    }
    require(blendBand > 12, "the blend from the scan to the bone has no intermediate surface");

    // Proclined incisors (and brackets) reach several millimetres labial to the gingival points in the
    // occlusal view: that scan is kept and the CT crowns there are replaced.
    const Box proclined{-6.0, 6.0, kArchRy + 8.0, kArchRy + 12.0, gingiva - 5.0, gingiva - 1.0};
    auto proclinedScan = vtkSmartPointer<vtkAppendPolyData>::New();
    proclinedScan->AddInputData(dental);
    proclinedScan->AddInputData(boxMesh(proclined, false, false));
    proclinedScan->Update();
    auto proclinedBone = vtkSmartPointer<vtkAppendPolyData>::New();
    proclinedBone->AddInputData(bone());
    proclinedBone->AddInputData(boxMesh(proclined, false, false));
    proclinedBone->Update();
    const CompositeBlockResult proclinedResult =
        CreateContourComposite(proclinedBone->GetOutput(), proclinedScan->GetOutput(), block, contour);
    require(proclinedResult.ok, "contour composite with proclined crowns failed: " + proclinedResult.error.toStdString());
    const auto proclinedDental = dentalBounds(ExtractPart(proclinedResult.composite, DentalPart));
    require(proclinedDental[3] >= kArchRy + 12.0 - 1e-6, "proclined crowns labial to the points were cut from the scan");
    const auto proclinedBonePart = ExtractPart(proclinedResult.composite, BonePart);
    double pc[3] = {};
    for (vtkIdType i = 0; i < proclinedBonePart->GetNumberOfCells(); ++i) {
        cellCentroid(proclinedBonePart, i, pc);
        require(!(std::abs(pc[0]) < 6.0 && pc[1] > kArchRy + 7.0 && pc[1] < kArchRy + 13.0 && pc[2] < gingiva - 0.5),
                "CT crowns labial to the points were kept");
    }

    // Bone behind the last marked teeth (mandibular rami, tuberosity) and bone rising past the occlusal side of
    // the crowns stay, even inside the widened outline.
    const Box pastCusps{-6.0, 6.0, kArchRy - 2.0, kArchRy + 2.0, -8.0, -3.0};
    auto limitBoneInput = vtkSmartPointer<vtkAppendPolyData>::New();
    limitBoneInput->AddInputData(bone());
    limitBoneInput->AddInputData(boxMesh(pastCusps, false, false));
    limitBoneInput->Update();
    const CompositeBlockResult limitResult = CreateContourComposite(limitBoneInput->GetOutput(), dental, block, contour);
    require(limitResult.ok, "contour composite with rami-like bone failed: " + limitResult.error.toStdString());
    const auto limitBone = ExtractPart(limitResult.composite, BonePart);
    bool behindKept = false;
    bool pastCuspsKept = false;
    bool besideKept = false;
    double lc[3] = {};
    for (vtkIdType i = 0; i < limitBone->GetNumberOfCells(); ++i) {
        cellCentroid(limitBone, i, lc);
        behindKept = behindKept || (lc[0] > 22.0 && lc[0] < 30.0 && lc[1] < -8.5 && lc[2] < gingiva - 1.0);
        pastCuspsKept = pastCuspsKept || lc[2] < -2.5;
        // Beside the last tooth (external oblique ridge): inside the region but more than 3 mm from the scan.
        besideKept = besideKept || (lc[0] > 30.5 && lc[0] < 34.0 && lc[1] > 0.5 && lc[1] < 8.0 && lc[2] < gingiva - 1.0);
    }
    require(besideKept, "bone beside the teeth, away from the scan, was replaced");
    require(behindKept, "bone behind the last marked teeth was replaced");
    require(pastCuspsKept, "bone past the occlusal side of the crowns was replaced");

    require(!ContourValid(block, {contour[0], contour[1]}, &error) && !error.isEmpty(), "two points accepted");
    require(!CreateContourComposite(bone(), dental, block, {contour[0], contour[1]}).ok, "two-point contour built");
    require(ContourFromJson(ContourToJson(contour)) == contour, "contour JSON round trip failed");
    require(ContourWallMesh(block, contour)->GetNumberOfPolys() >= 3, "contour wall missing around the points");
    require(ContourPolyline(block, contour)->GetNumberOfLines() == 1, "contour polyline missing");
}

// Mandible: body below the teeth (overlapping the CT crowns) and tall rami whose points lie above them.
vtkSmartPointer<vtkPolyData> mandible()
{
    auto append = vtkSmartPointer<vtkAppendPolyData>::New();
    append->AddInputData(boxMesh({-35, 35, -10, 35, -30, -4}, false, false));
    append->AddInputData(boxMesh({25, 35, -14, -8, -4, 90}, false, false));
    append->AddInputData(boxMesh({-35, -25, -14, -8, -4, 90}, false, false));
    append->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(append->GetOutput());
    return out;
}

void testMandibleContourComposite()
{
    const auto dental = lowerTeeth({});
    const auto bone = mandible();
    require(InitialBlock(dental, bone, 15.0).axisZ[2] > 0.0,
            "fixture: the rami do not pull the bone centroid above the teeth");
    const CompositeCutBlock block = InitialBlock(dental, bone, 15.0, nullptr, CompositeJaw::Lower);
    require(block.valid && block.axisZ[2] < -0.99, "mandibular block does not point down to the bone");
    require(BlockFacesJaw(block, CompositeJaw::Lower) && !BlockFacesJaw(block, CompositeJaw::Upper),
            "block jaw check failed");
    require(InitialBlock(upperTeeth({}), bone, 15.0, nullptr, CompositeJaw::Upper).axisZ[2] > 0.99,
            "maxillary block does not point up to the bone");

    double teeth[6] = {};
    dental->GetBounds(teeth);
    const double gingiva = teeth[4];
    const CompositeContour contour = archContour(gingiva - 0.3);
    const CompositeBlockResult result = CreateContourComposite(bone, dental, block, contour);
    require(result.ok, "mandibular contour composite failed: " + result.error.toStdString());
    // The scan is taken from the points up to the cusps.
    double kept[6] = {};
    ExtractPart(result.composite, DentalPart)->GetBounds(kept);
    for (int k = 0; k < 6; ++k)
        require(std::abs(teeth[k] - kept[k]) < 1e-6, "mandibular teeth above the points were lost");
    // CT crowns above the line are replaced; the body below the line and the rami stay.
    const auto c = toothCenter(kTeeth / 2);
    const double crown[3] = {c[0], c[1], gingiva + 4.0};
    const double body[3] = {c[0], c[1], gingiva - 3.0};
    require(ContourField(block, contour, crown) < 0.0, "CT crowns above the mandibular points are not replaced");
    require(ContourField(block, contour, body) > 0.0, "mandibular body below the points is replaced");
    double b[6] = {};
    ExtractPart(result.composite, BonePart)->GetBounds(b);
    require(b[4] < -30.0 + 1e-6 && b[5] > 90.0 - 1e-6, "mandibular body or rami were removed");

    // The open lower border is blended downward, toward the mandibular body.
    ArchOptions openOptions;
    openOptions.openScan = true;
    const CompositeBlockResult openResult = CreateContourComposite(bone, lowerTeeth(openOptions), block, contour);
    require(openResult.ok, "open lower contour composite failed: " + openResult.error.toStdString());
    double openLow[6] = {};
    ExtractPart(openResult.composite, DentalPart)->GetBounds(openLow);
    require(openLow[4] < gingiva - 1.8 && openLow[4] > gingiva - 3.2, "the open lower border does not drop toward the bone");
}

void testGingivalCompositeDropsTheFlange()
{
    const auto c = toothCenter(kTeeth / 2);
    auto scan = vtkSmartPointer<vtkAppendPolyData>::New();
    scan->AddInputData(upperTeeth({}));
    scan->AddInputData(boxMesh({c[0] - 2.0, c[0] + 2.0, c[1] - 2.0, c[1] + 2.0, 9.0, 28.0}, false, false));
    // Vestibule in front of the tooth: its own top must not keep a sheet down the jaw.
    scan->AddInputData(boxMesh({c[0] - 3.0, c[0] + 3.0, c[1] + 10.0, c[1] + 14.0, 8.0, 26.0}, false, false));
    scan->Update();
    auto jawBone = vtkSmartPointer<vtkAppendPolyData>::New();
    jawBone->AddInputData(bone());
    // The alveolar crest, just on the jaw side of the band the scan covers.
    jawBone->AddInputData(boxMesh({c[0] - 3.0, c[0] + 3.0, c[1] - 3.0, c[1] + 3.0, 14.0, 17.0}, false, false));
    jawBone->Update();
    const CompositeCutBlock block = InitialBlock(upperTeeth({}), jawBone->GetOutput(), 15.0, nullptr, CompositeJaw::Upper);
    const CompositeBlockResult result = CreateGingivalComposite(jawBone->GetOutput(), scan->GetOutput(), block);
    require(result.ok, "gingival composite failed: " + result.error.toStdString());
    require(HasParts(result.composite), "gingival composite has no part tags");
    const auto dental = ExtractPart(result.composite, DentalPart);
    const auto jaw = ExtractPart(result.composite, BonePart);
    require(dental && jaw, "gingival parts cannot be extracted");
    bool crownKept = false;
    bool flangeKept = false;
    double centroid[3] = {};
    for (vtkIdType i = 0; i < dental->GetNumberOfCells(); ++i) {
        cellCentroid(dental, i, centroid);
        crownKept = crownKept || centroid[2] < 4.0;
        flangeKept = flangeKept || centroid[2] > ToothAndGingivaMm + 6.0;
    }
    require(crownKept, "the crowns were cut off with the gingiva");
    require(!flangeKept, "gingiva far from the teeth was kept");
    double b[6] = {};
    jaw->GetBounds(b);
    require(b[5] > 28.5, "bone away from the teeth was removed");
    bool crestKept = false;
    for (vtkIdType i = 0; i < jaw->GetNumberOfCells(); ++i) {
        cellCentroid(jaw, i, centroid);
        crestKept = crestKept || (std::abs(centroid[0] - c[0]) < 4.0 && centroid[2] > 14.0 && centroid[2] < 17.0);
    }
    require(crestKept, "the alveolar crest next to the scan was cut away");
    bool ctCrownKept = false;
    for (vtkIdType i = 0; i < jaw->GetNumberOfCells(); ++i) {
        cellCentroid(jaw, i, centroid);
        ctCrownKept = ctCrownKept || (std::abs(centroid[0] - c[0]) < 2.0 && std::abs(centroid[1] - c[1]) < 2.0
                                      && centroid[2] > 5.0 && centroid[2] < 12.0);
    }
    require(!ctCrownKept, "the CT crown under the scan was kept and mixes with the model");
    bool vestibuleKept = false;
    for (vtkIdType i = 0; i < dental->GetNumberOfPoints(); ++i) {
        double p[3] = {};
        dental->GetPoint(i, p);
        vestibuleKept = vestibuleKept || (p[1] > c[1] + 6.0 && p[2] > 16.0);
    }
    bool met = false;
    for (vtkIdType i = 0; i < result.composite->GetNumberOfPoints(); ++i) {
        double p[3] = {};
        result.composite->GetPoint(i, p);
        met = met || (std::abs(p[0] - c[0]) < 4.0 && std::abs(p[1] - c[1]) < 4.0 && p[2] > 13.2 && p[2] < 16.5);
    }
    require(!vestibuleKept, "the vestibule in front of the teeth was kept");
    require(met, "the open border does not meet the crest");
}

vtkSmartPointer<vtkPolyData> horseshoe(double innerR, double outerR);

// The sheet is that same border, pushed along the jaw until it enters the bone above it.
// It is not fitted sideways onto the crest. The palate stays open.
void testUpperRimMeetsBoneBesideIt()
{
    const auto boxed = BoxScan(horseshoe(2.0, 26.0), CompositeJaw::Upper);
    require(boxed && boxed->GetNumberOfPolys() > 0, "the upper rim scan was not boxed");
    // Just above the anterior rim (r = 26, cut at z = 12), so the sheet has to enter it.
    const auto crest = boxMesh({-8, 8, 23, 32, 14, 18}, false, false);
    const CompositeCutBlock block = InitialBlock(boxed, crest, 15.0, nullptr, CompositeJaw::Upper);
    require(block.valid && block.axisZ[2] > 0.9, "upper beside-crest block does not point toward the bone");
    const CompositeBlockResult result = CreateGingivalComposite(crest, boxed, block);
    require(result.ok, "upper beside-crest composite failed: " + result.error.toStdString());
    const auto dental = ExtractPart(result.composite, DentalPart);
    const auto jaw = ExtractPart(result.composite, BonePart);
    require(dental && jaw, "upper beside-crest parts cannot be extracted");
    bool crestKept = false;
    double centroid[3] = {};
    for (vtkIdType i = 0; i < jaw->GetNumberOfCells(); ++i) {
        cellCentroid(jaw, i, centroid);
        crestKept = crestKept || (centroid[1] > 23.0 && centroid[1] < 32.0 && centroid[2] > 14.0 && centroid[2] < 18.0);
    }
    require(crestKept, "the crest beside the upper rim was cut away");
    bool rose = false;
    for (vtkIdType i = 0; i < result.composite->GetNumberOfPoints(); ++i) {
        double p[3] = {};
        result.composite->GetPoint(i, p);
        rose = rose || (std::abs(p[0]) < 8.0 && p[1] > 20.0 && p[1] < 27.5 && p[2] > 13.2 && p[2] < 22.0);
    }
    require(rose, "the upper border was not pushed into the bone");
    bool palateSheet = false;
    for (vtkIdType i = 0; i < result.composite->GetNumberOfCells(); ++i) {
        cellCentroid(result.composite, i, centroid);
        palateSheet = palateSheet || (std::abs(centroid[0]) < 8.0 && centroid[1] > 8.0 && centroid[1] < 22.0 && centroid[2] > 8.0);
    }
    require(!palateSheet, "the join filled the palate");
    double bounds[6] = {};
    result.composite->GetBounds(bounds);
    require(bounds[5] < 24.0, "the sheet climbed away from the border");
    require(bounds[3] < 36.0, "the blend ran past the crest");
}

// The posterior gingiva sits well below the highest rim. Skipping it leaves the slit under the molars.
void testLowPosteriorRimMeetsTheBoneBesideIt()
{
    auto scan = vtkSmartPointer<vtkAppendPolyData>::New();
    scan->AddInputData(horseshoe(2.0, 26.0));
    // Posterior gingiva whose cusps sit lower, so its rim is well below the rest of the arch.
    scan->AddInputData(boxMesh({24, 32, -3, 3, -10, 6}, false, false));
    scan->Update();
    const auto crest = boxMesh({34, 39, -3, 3, 0, 6}, false, false);
    CompositeCutBlock block;
    block.center = {0.0, 0.0, 4.0};
    block.axisX = {1.0, 0.0, 0.0};
    block.axisY = {0.0, 1.0, 0.0};
    block.axisZ = {0.0, 0.0, 1.0};
    block.sizeMm = {90.0, 90.0, 40.0};
    block.valid = true;
    const CompositeBlockResult result = CreateGingivalComposite(crest, scan->GetOutput(), block);
    require(result.ok, "posterior rim composite failed: " + result.error.toStdString());
    const auto dental = ExtractPart(result.composite, DentalPart);
    require(dental, "posterior rim dental part is missing");
    bool reached = false;
    for (vtkIdType i = 0; i < dental->GetNumberOfPoints(); ++i) {
        double p[3] = {};
        dental->GetPoint(i, p);
        reached = reached || (p[0] > 29.0 && p[0] < 36.0 && p[1] > -6.0 && p[1] < 6.0 && p[2] > 1.0 && p[2] < 9.0);
    }
    require(reached, "the low posterior rim stays separated from the bone beside it");
}

// The mandibular crest is in front of the gingiva, not straight down. The border has to
// reach that wall, and the wall has to stay.
void testLowerGingivaReachesBoneBesideTheRim()
{
    ArchOptions open;
    open.openScan = true;
    const auto scan = lowerTeeth(open);
    const auto c = toothCenter(kTeeth / 2);
    const double face = c[1] + 2.5;
    const auto jawBone = boxMesh({c[0] - 4.0, c[0] + 4.0, face + 3.0, face + 5.5, -11.0, -7.0}, false, false);
    const CompositeCutBlock block = InitialBlock(scan, jawBone, 15.0, nullptr, CompositeJaw::Lower);
    require(block.valid && block.axisZ[2] < -0.9, "lower gingival block does not point toward the bone");
    const CompositeBlockResult result = CreateGingivalComposite(jawBone, scan, block);
    require(result.ok, "lower gingival composite failed: " + result.error.toStdString());
    const auto dental = ExtractPart(result.composite, DentalPart);
    const auto jaw = ExtractPart(result.composite, BonePart);
    require(dental && jaw, "lower gingival parts cannot be extracted");
    bool wallKept = false;
    double centroid[3] = {};
    for (vtkIdType i = 0; i < jaw->GetNumberOfCells(); ++i) {
        cellCentroid(jaw, i, centroid);
        wallKept = wallKept || (centroid[1] > face + 3.0 && centroid[2] < -7.5 && centroid[2] > -11.0);
    }
    require(wallKept, "the bone beside the lower gingiva was cut away");
    bool reached = false;
    for (vtkIdType i = 0; i < result.composite->GetNumberOfPoints(); ++i) {
        double p[3] = {};
        result.composite->GetPoint(i, p);
        reached = reached || (std::abs(p[0] - c[0]) < 5.0 && p[1] > face + 2.2 && p[2] > -13.0 && p[2] < -6.0);
    }
    require(reached, "the lower border does not reach the bone beside the gingiva");
    double bounds[6] = {};
    dental->GetBounds(bounds);
    require(bounds[4] > -16.0, "the lower blend ran down the jaw");
}

// The crest is beside the rim and a little toward the cusps. A lip straight down the jaw misses it.
void testLowerRimClosesOntoTheCrestBesideIt()
{
    ArchOptions open;
    open.openScan = true;
    const auto scan = lowerTeeth(open);
    const auto c = toothCenter(kTeeth / 2);
    const double face = c[1] + 2.5;
    const auto jawBone = boxMesh({c[0] - 4.0, c[0] + 4.0, face + 3.0, face + 5.0, -8.2, -6.2}, false, false);
    const CompositeCutBlock block = InitialBlock(scan, jawBone, 15.0, nullptr, CompositeJaw::Lower);
    require(block.valid && block.axisZ[2] < -0.9, "crest fixture block does not point toward the bone");
    const CompositeBlockResult result = CreateGingivalComposite(jawBone, scan, block);
    require(result.ok, "crest fixture composite failed: " + result.error.toStdString());
    const auto dental = ExtractPart(result.composite, DentalPart);
    const auto jaw = ExtractPart(result.composite, BonePart);
    require(dental && jaw, "crest fixture parts cannot be extracted");
    bool crestKept = false;
    double centroid[3] = {};
    for (vtkIdType i = 0; i < jaw->GetNumberOfCells(); ++i) {
        cellCentroid(jaw, i, centroid);
        crestKept = crestKept || (centroid[1] > face + 3.0 && centroid[2] > -8.2 && centroid[2] < -6.2);
    }
    require(crestKept, "the crest beside the lower gingiva was cut away");
    bool reached = false;
    for (vtkIdType i = 0; i < result.composite->GetNumberOfPoints(); ++i) {
        double p[3] = {};
        result.composite->GetPoint(i, p);
        reached = reached || (std::abs(p[0] - c[0]) < 5.0 && p[1] > face + 2.2 && p[2] > -9.2 && p[2] < -5.5);
    }
    require(reached, "the lower border stays more than a short blend from the crest beside it");
    double bounds[6] = {};
    dental->GetBounds(bounds);
    require(bounds[4] > -15.0, "the blend dropped away from the crest");
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

// U-shaped shell, open on the bone side. innerR == 0 is a sheet across the palate;
// a few millimetres between innerR and outerR is only the gingival wall.
vtkSmartPointer<vtkPolyData> horseshoe(double innerR, double outerR)
{
    constexpr int kSteps = 28;
    auto points = vtkSmartPointer<vtkPoints>::New();
    for (int i = 0; i <= kSteps; ++i) {
        const double t = kPi * i / kSteps;
        const double c = std::cos(t);
        const double s = std::sin(t);
        points->InsertNextPoint(outerR * c, outerR * s, 0.0);
        points->InsertNextPoint(outerR * c, outerR * s, 20.0);
        points->InsertNextPoint(innerR * c, innerR * s, 0.0);
        points->InsertNextPoint(innerR * c, innerR * s, 20.0);
    }
    auto polys = vtkSmartPointer<vtkCellArray>::New();
    auto tri = [&](vtkIdType a, vtkIdType b, vtkIdType c) {
        const vtkIdType ids[3] = {a, b, c};
        polys->InsertNextCell(3, ids);
    };
    auto quad = [&](vtkIdType a, vtkIdType b, vtkIdType c, vtkIdType d) {
        tri(a, b, c);
        tri(a, c, d);
    };
    for (int i = 0; i < kSteps; ++i) {
        const vtkIdType a = i * 4;
        const vtkIdType b = (i + 1) * 4;
        quad(a, b, b + 1, a + 1);         // outer wall
        quad(a + 2, a + 3, b + 3, b + 2); // inner wall
        quad(a, a + 2, b + 2, b);         // occlusal floor of the wall
    }
    quad(0, 1, 3, 2); // one posterior end
    const vtkIdType end = kSteps * 4;
    quad(end, end + 2, end + 3, end + 1);
    auto mesh = vtkSmartPointer<vtkPolyData>::New();
    mesh->SetPoints(points);
    mesh->SetPolys(polys);
    return mesh;
}

bool capCovers(vtkPolyData* mesh, double x, double y)
{
    double c[3] = {};
    for (vtkIdType i = 0; i < mesh->GetNumberOfCells(); ++i) {
        cellCentroid(mesh, i, c);
        if (c[2] > ToothAndGingivaMm - 0.8 && std::hypot(c[0] - x, c[1] - y) < 2.0)
            return true;
    }
    return false;
}

void testBoxScanClosesTheArch()
{
    auto upper = vtkSmartPointer<vtkAppendPolyData>::New();
    upper->AddInputData(boxMesh({-3, 3, -3, 3, 0, 20}, true, false));
    upper->AddInputData(boxMesh({18, 24, -2, 2, 16, 28}, false, false));
    upper->Update();
    const vtkIdType originalPoints = upper->GetOutput()->GetNumberOfPoints();
    const auto boxed = BoxScan(upper->GetOutput(), CompositeJaw::Upper);
    require(boxed && boxed->GetNumberOfPolys() > 0, "upper scan was not boxed");
    require(upper->GetOutput()->GetNumberOfPoints() == originalPoints, "boxing modified the original scan");
    double p[3] = {};
    bool tooth = false;
    for (vtkIdType i = 0; i < boxed->GetNumberOfPoints(); ++i) {
        boxed->GetPoint(i, p);
        require(p[2] <= ToothAndGingivaMm + 1e-3, "geometry past the upper plane was kept");
        tooth = tooth || p[2] < 1.0;
    }
    require(tooth, "occlusal teeth were removed");
    require(capCovers(boxed, 0.0, 0.0), "the upper opening was not filled");
    require(openEdges(boxed) == 0, "the boxed upper scan still has an open border");

    const auto wall = BoxScan(horseshoe(21.0, 25.0), CompositeJaw::Upper);
    require(wall && openEdges(wall) == 0, "the gingival wall was not closed on its rim");
    require(capCovers(wall, 0.0, 23.0), "the cap left the rim of the wall");
    require(!capCovers(wall, 0.0, 0.0), "the wall cap grew into the palate");

    const auto palate = BoxScan(horseshoe(2.0, 26.0), CompositeJaw::Upper);
    require(palate && palate->GetNumberOfPolys() > 0, "the horseshoe scan was dropped");
    require(!capCovers(palate, 0.0, 10.0), "a V was filled across the palatal opening");
    bool palateTooth = false;
    for (vtkIdType i = 0; i < palate->GetNumberOfPoints(); ++i) {
        palate->GetPoint(i, p);
        require(p[2] <= ToothAndGingivaMm + 1e-3, "geometry past the horseshoe plane was kept");
        palateTooth = palateTooth || p[2] < 1.0;
    }
    require(palateTooth, "horseshoe teeth were removed");

    auto lower = vtkSmartPointer<vtkAppendPolyData>::New();
    lower->AddInputData(boxMesh({-3, 3, -3, 3, -20, 0}, false, true));
    lower->AddInputData(boxMesh({18, 24, -2, 2, -28, -16}, false, false));
    lower->Update();
    const auto lowerBoxed = BoxScan(lower->GetOutput(), CompositeJaw::Lower);
    require(lowerBoxed && lowerBoxed->GetNumberOfPolys() > 0, "lower scan was not boxed");
    bool lowerTooth = false;
    bool lowerCap = false;
    for (vtkIdType i = 0; i < lowerBoxed->GetNumberOfPoints(); ++i) {
        lowerBoxed->GetPoint(i, p);
        require(p[2] >= -ToothAndGingivaMm - 1e-3, "geometry past the lower plane was kept");
        lowerTooth = lowerTooth || p[2] > -1.0;
    }
    double centroid[3] = {};
    for (vtkIdType i = 0; i < lowerBoxed->GetNumberOfCells(); ++i) {
        cellCentroid(lowerBoxed, i, centroid);
        lowerCap = lowerCap || (std::abs(centroid[2] + ToothAndGingivaMm) < 0.8 && std::hypot(centroid[0], centroid[1]) < 2.0);
    }
    require(lowerTooth, "lower occlusal teeth were removed");
    require(lowerCap, "the lower opening was not filled");
    require(openEdges(lowerBoxed) == 0, "the boxed lower scan still has an open border");
    // A tooth whose cusp sits further toward the bone. The cusp is the raised
    // nub; one plane from the other tooth used to slice this crown.
    auto uneven = vtkSmartPointer<vtkAppendPolyData>::New();
    uneven->AddInputData(boxMesh({-3, 3, -3, 3, -18, 0}, false, true));
    uneven->AddInputData(boxMesh({22, 28, -3, 3, -26, -8}, false, true));
    uneven->AddInputData(boxMesh({24, 26, -1, 1, -8, -6}, false, true));
    uneven->Update();
    const auto unevenBoxed = BoxScan(uneven->GetOutput(), CompositeJaw::Lower);
    require(unevenBoxed && unevenBoxed->GetNumberOfPolys() > 0, "the uneven lower scan was not boxed");
    bool fullIncisor = false;
    for (vtkIdType i = 0; i < unevenBoxed->GetNumberOfPoints(); ++i) {
        unevenBoxed->GetPoint(i, p);
        if (p[0] < 10.0)
            require(p[2] >= -ToothAndGingivaMm - 1e-3, "the higher lower cusp kept too much root");
        fullIncisor = fullIncisor || (p[0] > 20.0 && p[2] < -ToothAndGingivaMm - 2.0);
    }
    require(fullIncisor, "the lower tooth past the highest cusp was cut through the crown");
    auto unevenUpper = vtkSmartPointer<vtkAppendPolyData>::New();
    unevenUpper->AddInputData(boxMesh({-3, 3, -3, 3, 0, 20}, true, false));
    unevenUpper->AddInputData(boxMesh({22, 28, -3, 3, 8, 26}, false, false));
    unevenUpper->AddInputData(boxMesh({24, 26, -1, 1, 6, 8}, false, false));
    unevenUpper->Update();
    const auto unevenUpperBoxed = BoxScan(unevenUpper->GetOutput(), CompositeJaw::Upper);
    require(unevenUpperBoxed && unevenUpperBoxed->GetNumberOfPolys() > 0, "the uneven upper scan was not boxed");
    bool fullUpper = false;
    for (vtkIdType i = 0; i < unevenUpperBoxed->GetNumberOfPoints(); ++i) {
        unevenUpperBoxed->GetPoint(i, p);
        if (p[0] < 10.0)
            require(p[2] <= ToothAndGingivaMm + 1e-3, "the lower upper cusp kept too much root");
        fullUpper = fullUpper || (p[0] > 20.0 && p[2] > ToothAndGingivaMm + 2.0);
    }
    require(fullUpper, "the upper tooth past the lowest cusp was cut through the crown");
    require(!BoxScan(nullptr, CompositeJaw::Upper), "missing scan was boxed");
}

void testThreePointsReplaceTheCtTeeth()
{
    const auto jawBone = boxMesh({-30, 30, -10, 25, -18, 6}, false, false);
    const auto scan = boxMesh({-8, 8, -4, 10, 1, 12}, false, false);
    CompositeCutBlock block;
    block.center = {0.0, 2.0, 0.0};
    block.axisX = {1.0, 0.0, 0.0};
    block.axisY = {0.0, -1.0, 0.0}; // right-handed with axisZ pointing at the mandible
    block.axisZ = {0.0, 0.0, -1.0};
    block.sizeMm = {80.0, 60.0, 30.0};
    block.valid = true;
    const CompositeContour points = {{{-18.0, 12.0, 2.0}}, {{18.0, 12.0, 2.0}}, {{0.0, -6.0, 2.0}}};
    const CompositeBlockResult result = CreateArchComposite(jawBone, scan, block, points);
    require(result.ok, "three-point composite failed: " + result.error.toStdString());
    const auto dental = ExtractPart(result.composite, DentalPart);
    const auto bonePart = ExtractPart(result.composite, BonePart);
    require(dental && bonePart, "three-point parts cannot be extracted");
    bool crownKept = false;
    for (vtkIdType i = 0; i < dental->GetNumberOfPoints(); ++i) {
        double p[3] = {};
        dental->GetPoint(i, p);
        crownKept = crownKept || p[2] > 4.0;
    }
    require(crownKept, "the scan did not replace the teeth");
    bool ctCrownKept = false;
    bool bodyKept = false;
    bool ramusKept = false;
    double centroid[3] = {};
    for (vtkIdType i = 0; i < bonePart->GetNumberOfCells(); ++i) {
        cellCentroid(bonePart, i, centroid);
        ctCrownKept = ctCrownKept || (std::abs(centroid[0]) < 6.0 && centroid[1] > 0.0 && centroid[1] < 8.0 && centroid[2] > 3.0);
        ctCrownKept = ctCrownKept || (std::abs(centroid[0]) > 10.0 && std::abs(centroid[0]) < 16.0 && centroid[1] > -2.0 &&
                                     centroid[1] < 8.0 && centroid[2] > 3.0);
        bodyKept = bodyKept || centroid[2] < -2.0;
        ramusKept = ramusKept || (std::abs(centroid[0]) > 27.0 && centroid[1] < -4.0 && centroid[2] > 3.0);
    }
    require(!ctCrownKept, "the CT teeth on the tooth side of the line were kept");
    require(bodyKept, "the bone on the other side of the line was removed");
    require(ramusKept, "bone outside the arch was removed");
    const auto ribbon = ArchRibbon(jawBone, block, points);
    require(ribbon && ribbon->GetNumberOfPolys() > 0, "the red line was not drawn on the bone");
    bool ahead = false;
    bool reachesRight = false;
    bool reachesLeft = false;
    for (vtkIdType i = 0; i < ribbon->GetNumberOfPoints(); ++i) {
        double p[3] = {};
        ribbon->GetPoint(i, p);
        ahead = ahead || (p[1] < -9.0 && std::abs(p[0]) < 8.0);
        reachesRight = reachesRight || p[0] < -16.0;
        reachesLeft = reachesLeft || p[0] > 16.0;
    }
    require(ahead, "the red line does not pass in front of the anterior teeth");
    require(reachesRight && reachesLeft, "the red line does not wrap both sides of the arch");
    bool filled = false;
    for (vtkIdType i = 0; i < ribbon->GetNumberOfPoints(); ++i) {
        double p[3] = {};
        ribbon->GetPoint(i, p);
        filled = filled || (p[0] * p[0] + (p[1] - 0.2) * (p[1] - 0.2) < 15.0 * 15.0);
    }
    require(filled, "the horseshoe is not filled inside");
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
        {"mandible contour composite", testMandibleContourComposite},
        {"cut keeps regions", testCutKeepsRegions},
        {"contour composite", testContourComposite},
        {"gingival composite", testGingivalCompositeDropsTheFlange},
        {"upper rim meets the bone beside it", testUpperRimMeetsBoneBesideIt},
        {"low posterior rim meets the bone beside it", testLowPosteriorRimMeetsTheBoneBesideIt},
        {"boxed scan is a flat closed cast", testBoxScanClosesTheArch},
        {"lower gingiva meets the bone beside it", testLowerGingivaReachesBoneBesideTheRim},
        {"lower rim closes onto the crest beside it", testLowerRimClosesOntoTheCrestBesideIt},
        {"link follows transform and cuts", testLinkFollowsTransformAndCuts},
        {"block transform and json", testBlockTransformAndJson},
        {"three points replace the CT teeth", testThreePointsReplaceTheCtTeeth},
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
