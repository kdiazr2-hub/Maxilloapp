#pragma once

#include <array>
#include <QString>
#include <vtkSmartPointer.h>
#include <vtkType.h>

class vtkImageData;

// Air on a bone window is about -1000 HU. Marrow in the ramus and the cranial base is far above
// that, often only a little above water, so the cutoff is air rather than dense cortex.
inline constexpr double kMarrowMinimumHu = -500.0;
// At or above this the voxel is cancellous bone. Below it, an anterior opening is an orbit
// or the nasal airway and is left empty.
inline constexpr double kDenseMarrowHu = 150.0;

struct BoneCavityFillResult
{
    vtkSmartPointer<vtkImageData> labelmap;
    vtkIdType addedVoxels = 0;
    bool replacedSoftTissue = false;
    double addedVolumeMm3 = 0.0;
    QString error;
};

// Geometric repair of one user-selected, enclosed cavity, not tissue inference.
// Input is immutable. Only background (0) and generic soft tissue (2) may change.
BoneCavityFillResult fillEnclosedBoneCavity(
    vtkImageData* labelmap, int boneLabel, const std::array<double, 3>& physicalSeed);

// Marrow between the two cortices of Hueso, Maxilar or Mandíbula. A background or soft-tissue
// voxel becomes bone only when this label lies on both sides of it, at most maxGapMm apart,
// along one of the three axes. When `hounsfield` is the CT in the same space, the voxel also
// has to read at least `minimumHu` (not air). Dark marrow counts; an orbit or the nasal airway,
// open toward anterior, does not. The same rule fills the maxilla, the cranial base and the
// mandible, so the Le Fort piece, the cranial base and the sagittal segments all come from
// solid bone. Canal and teeth labels are not overwritten. The input is not modified.
// Returns null when this label is not bone or there is nothing to add.
vtkSmartPointer<vtkImageData> fillBetweenCortices(
    vtkImageData* labelmap, int boneLabel, double maxGapMm = 20.0,
    vtkImageData* hounsfield = nullptr, double minimumHu = kMarrowMinimumHu);
