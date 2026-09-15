#pragma once
#include <QString>
#include <vtkSmartPointer.h>
class vtkImageData;
class vtkPolyData;

class MaskToObjectCore
{
public:
    // Exact: voxel-faithful surface at voxel midpoints. Smooth: anti-aliased surface (binary label
    // blurred by SmoothSigmaVoxels, contoured at 0.5), so CT slice terraces disappear while filling and
    // thin plates stay; on real CT it lies within a few tenths of a millimetre of the exact surface.
    // Neither adds filling nor modifies the mask.
    enum class Surface { Exact, Smooth };
    static vtkSmartPointer<vtkPolyData> Convert(vtkImageData* mask, int label, QString* error = nullptr,
                                                Surface surface = Surface::Exact);
    static constexpr double SmoothSigmaVoxels = 0.6;
};
