#pragma once

#include <QString>
#include <vtkSmartPointer.h>

class vtkImageData;
class vtkPolyData;

class MeshGenerator
{
public:
    static vtkSmartPointer<vtkPolyData> generateMesh(vtkImageData* labelmap,
                                                     int labelValue,
                                                     bool smooth,
                                                     int smoothingIterations,
                                                     QString* errorMessage);
};
