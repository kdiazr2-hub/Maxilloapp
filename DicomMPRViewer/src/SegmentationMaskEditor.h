#pragma once

#include <QVector>
#include <QPointF>

class vtkImageData;
class vtkRenderer;
enum class MPROrientation;

// ─────────────────────────────────────────────────────────────────────────────
// SegmentationMaskEditor
//
// Static utility for applying a freehand lasso polygon to a segmentation
// labelmap on a single MPR slice.
//
// The polygon is supplied in VTK display coordinates (physical pixels,
// y = 0 at the BOTTOM of the render window).  applyLasso() converts these
// vertices to world coordinates via vtkCoordinate, then iterates over every
// voxel on the current slice that falls inside the polygon and either paints
// (addMode = true) or erases (addMode = false) the requested label.
// ─────────────────────────────────────────────────────────────────────────────
class SegmentationMaskEditor
{
public:
    // Apply a lasso polygon to the labelmap on a single slice.
    //
    // Parameters:
    //   labelmap          – the vtkImageData labelmap to modify
    //   renderer          – the VTK renderer of the MPR view
    //   vtkDisplayPoints  – polygon vertices in VTK display coords (y=0 at bottom)
    //   orientation       – axial / coronal / sagittal
    //   sliceIndex        – current integer slice index along the normal axis
    //   label             – label value to paint or erase
    //   addMode           – true: set voxels to label;  false: clear voxels with label
    //
    // Returns true if at least one voxel was changed.
    static bool applyLasso(
        vtkImageData*           labelmap,
        vtkRenderer*            renderer,
        const QVector<QPointF>& vtkDisplayPoints,
        MPROrientation          orientation,
        int                     sliceIndex,
        int                     label,
        bool                    addMode);
};
