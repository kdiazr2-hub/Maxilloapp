#pragma once

#include <cstdint>
#include <vector>

#include <vtkSmartPointer.h>

class vtkImageData;

// A Hounsfield window, the same idea as a Mimics threshold: voxels with
// minHu <= value <= maxHu belong to the mask. The window is chosen per study.
struct ThresholdRequest
{
    double minHu = 226.0;
    double maxHu = 3071.0;
    bool fillHoles = false;
    // Kept for tests. The bone presets no longer turn it on: a click selects the piece.
    bool keepLargest = false;
    // 26-connected component of seedMm. A click outside the window yields an empty mask.
    bool keepConnectedToSeed = false;
    bool hasSeed = false;
    double seedMm[3] = {0.0, 0.0, 0.0};
    int label = 1;
};

// A starting window for a cone-beam study, whose grays are not Hounsfield units.
struct StudyWindow
{
    double minimum = 0.0;
    double maximum = 0.0;
};

struct HuHistogram
{
    double minimum = 0.0;
    double maximum = 0.0;
    double binWidth = 1.0;
    std::vector<std::int64_t> counts;
    std::int64_t voxels = 0;
};

// Threshold of a CT volume. No Qt widgets: the dialog only displays this.
class ThresholdMaskCore
{
public:
    // Up to `bins` buckets spanning the finite voxels of the volume.
    static HuHistogram histogram(vtkImageData* volume, int bins = 256);

    // Minimum is the 90th percentile of the voxels above the 20th of the study.
    // Maximum is the study maximum. The panel must say these are not Hounsfield units.
    static StudyWindow coneBeamBoneWindow(const HuHistogram& histogram);

    // Empty unsigned-char labelmap with the volume's geometry.
    static vtkSmartPointer<vtkImageData> blank(vtkImageData* volume);

    // The existing labelmap as one unsigned-char component, or the same image
    // when it already is. Labels above 255 are clipped.
    static vtkSmartPointer<vtkImageData> unsignedLabelmap(vtkImageData* source);

    // Zero `slice` (an extent index on axis 0/1/2) and paint `label` where the
    // volume sits in [minHu, maxHu]. What the three MPR views show while dragging.
    static void paintSlice(vtkImageData* volume, vtkImageData* labelmap,
                           int axis, int slice, double minHu, double maxHu, int label);

    // Full mask. `keepConnectedToSeed` keeps the 26-connected piece of the click
    // and ignores `keepLargest`. Otherwise `keepLargest` runs before `fillHoles`.
    // Null when nothing matches, or when the click misses the window.
    static vtkSmartPointer<vtkImageData> build(vtkImageData* volume, const ThresholdRequest& request,
                                               std::int64_t* voxels = nullptr);

    // Clear `clearLabels` in `destination`, then write `label` from the nonzero
    // voxels of `mask` onto what is now empty. Other labels stay. Returns the
    // number written, or -1 when the grids differ.
    static std::int64_t stamp(vtkImageData* destination, vtkImageData* mask, int label,
                              const int* clearLabels, int clearCount);

    // Same as stamp, but each written voxel keeps the value it already has in
    // `mask` (a jaw split stores maxilla and mandible in one mask).
    static std::int64_t stampMask(vtkImageData* destination, vtkImageData* mask,
                                  const int* clearLabels, int clearCount);

    // Split a bone mask whose voxels touch through the occlusion. The cranium
    // and maxilla become 5, the mandible 6. The cervical column, posterior and
    // below the skull, is removed. Bone that never meets either jaw keeps its
    // label. Returns false when no mandible can be told apart; the mask is
    // then left as it was.
    static bool separateMandible(vtkImageData* mask);

    // Close gaps of about a millimetre in each label. A new voxel has to read
    // at least `minHu`, so air in the nose, the sinuses and the orbits stays
    // open. Labels are closed separately, so the jaws are not glued back.
    static void sealThinBone(vtkImageData* mask, vtkImageData* volume,
                             double radiusMm = 1.2, double minHu = 120.0);
};
