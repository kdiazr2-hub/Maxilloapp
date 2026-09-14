#pragma once
#include <QString>
#include <vtkSmartPointer.h>
class vtkImageData;
class vtkPolyData;

class MaskToObjectCore
{
public:
    // Extract the current label at voxel midpoints, without smoothing or new filling.
    static vtkSmartPointer<vtkPolyData> Convert(vtkImageData* mask, int label, QString* error = nullptr);
};
