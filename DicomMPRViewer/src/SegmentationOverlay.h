#pragma once

#include <set>

#include <vtkSmartPointer.h>

#include "MPRView.h"

class vtkImageActor;
class vtkImageData;
class vtkImageMapToColors;
class vtkLookupTable;
class vtkRenderer;

class SegmentationOverlay
{
public:
    SegmentationOverlay(vtkRenderer* renderer, MPROrientation orientation);
    ~SegmentationOverlay();

    void setRenderer(vtkRenderer* renderer);
    void setLabelmap(vtkSmartPointer<vtkImageData> labelmap);
    void setCurrentSlice(int sliceIndex);
    void setHiddenLabels(const std::set<int>& hiddenLabels);
    void setVisible(bool visible);
    void setOpacity(double opacity);
    void refresh();
    void clear();

private:
    void buildLookupTable();
    void updateDisplayExtent();
    int normalAxis() const;

    vtkRenderer* m_renderer = nullptr;
    MPROrientation m_orientation = MPROrientation::Axial;
    vtkSmartPointer<vtkImageData> m_labelmap;
    vtkSmartPointer<vtkLookupTable> m_lookupTable;
    vtkSmartPointer<vtkImageMapToColors> m_colorMap;
    vtkSmartPointer<vtkImageActor> m_actor;
    int m_currentSlice = 0;
    bool m_visible = true;
    double m_opacity = 0.45;
    std::set<int> m_hiddenLabels;
};
