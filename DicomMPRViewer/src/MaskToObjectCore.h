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
    // Bone labels also close the gap between cortices (see fillBetweenCortices) unless closeCortices is
    // false: a label the segmentation script already delivered solid gets nothing but the chords' slabs.
    // With the CT, only marrow-density voxels are added. The mask is not modified.
    enum class Surface { Exact, Smooth };
    static vtkSmartPointer<vtkPolyData> Convert(vtkImageData* mask, int label, QString* error = nullptr,
                                                Surface surface = Surface::Exact,
                                                vtkImageData* hounsfield = nullptr,
                                                bool closeCortices = true);
    static constexpr double SmoothSigmaVoxels = 0.6;
};
