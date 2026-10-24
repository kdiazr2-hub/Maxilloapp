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

    // The 3D Slicer extension surface: discrete flying edges on the label, one voxel of
    // padding, then windowed sinc. factor is 0..1. passband = 10^(-4*factor),
    // iterations = 20 + 40*factor. 0.5 is the extension's default (passband 0.01, 40 iterations).
    static double SlicerPassband(double factor);
    static int SlicerIterations(double factor);
    static vtkSmartPointer<vtkPolyData> SlicerSurface(vtkImageData* mask, int label, double factor,
                                                      QString* error = nullptr);
};
