#pragma once

#include <QString>
#include <vtkSmartPointer.h>

class vtkImageData;
class vtkPolyData;

class MeshGenerator
{
public:
    // closeCortices fills the marrow of a bone label. A tooth mask that reuses label 1
    // passes false so the gaps between crowns stay open. `hounsfield` is the CT: marrow is
    // kept only where the density is bone, so air and the orbits stay empty.
    static vtkSmartPointer<vtkPolyData> generateMesh(vtkImageData* labelmap,
                                                     int labelValue,
                                                     bool smooth,
                                                     int smoothingIterations,
                                                     QString* errorMessage,
                                                     bool closeCortices = true,
                                                     vtkImageData* hounsfield = nullptr);

    // Modest mesh smoothing for a tooth surface. Stronger than the raw marching-cubes
    // facets, short of the bone pass that rounds cusps away. The input is not modified.
    static vtkSmartPointer<vtkPolyData> smoothToothMesh(vtkPolyData* mesh);
};
