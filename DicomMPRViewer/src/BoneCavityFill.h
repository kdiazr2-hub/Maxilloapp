#pragma once

#include <array>
#include <QString>
#include <vtkSmartPointer.h>
#include <vtkType.h>

class vtkImageData;

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
