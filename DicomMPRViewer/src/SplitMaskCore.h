#pragma once

#include <atomic>
#include <cstdint>

#include <vtkSmartPointer.h>

class vtkImageData;

// One painted mark. `region` is 0 .. regionCount-1. The centre is patient LPS, millimetres.
struct SplitMarker
{
    int region = 0;
    double centerMm[3] = {0.0, 0.0, 0.0};
    double radiusMm = 2.0;
};

struct SplitRequest
{
    int regionCount = 2;
    // Voxels of the origin mask equal to this label are the bone to divide.
    int originLabel = 1;
    // A component nobody painted goes to the nearest region. Otherwise it stays unassigned.
    bool assignIslandsToNearest = false;
    const SplitMarker* markers = nullptr;
    int markerCount = 0;
};

// Divides a bone mask by continuity. The origin image is never written.
// Output: 0 outside the origin, 1..regionCount for the regions, regionCount+1 unassigned.
// Growth prefers dense bone, so the regions meet on the least dense connection
// (the joint space, the occlusion). A tie is the shorter path in millimetres.
class SplitMaskCore
{
public:
    static vtkSmartPointer<vtkImageData> split(
        vtkImageData* originMask,
        vtkImageData* intensities,
        const SplitRequest& request,
        std::atomic<bool>* cancel = nullptr,
        void (*progress)(double fraction, void* user) = nullptr,
        void* progressUser = nullptr);
};
