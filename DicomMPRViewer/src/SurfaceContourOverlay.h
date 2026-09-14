#pragma once

// Intersection of surface meshes (e.g. the registered intraoral scan) with the
// current MPR slice, drawn as coloured polylines over the CT image.

#include <vtkSmartPointer.h>

#include <vector>

#include "MPRView.h"

class vtkActor;
class vtkRenderer;

class SurfaceContourOverlay
{
public:
    SurfaceContourOverlay(vtkRenderer* renderer, MPROrientation orientation);
    ~SurfaceContourOverlay();

    void setRenderer(vtkRenderer* renderer);
    void setSurfaces(const std::vector<MPRSurfaceContour>& surfaces);
    void setSlicePosition(double worldPosition);
    int surfaceCount() const { return static_cast<int>(m_surfaces.size()); }
    void refresh();
    void clear();

private:
    int normalAxis() const;

    vtkRenderer* m_renderer = nullptr;
    MPROrientation m_orientation = MPROrientation::Axial;
    std::vector<MPRSurfaceContour> m_surfaces;
    double m_position = 0.0;
    bool m_hasPosition = false;
    std::vector<vtkSmartPointer<vtkActor>> m_actors;
};
