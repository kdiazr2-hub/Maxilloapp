#include "SegmentationOverlay.h"

#include <algorithm>

#include <vtkImageActor.h>
#include <vtkImageData.h>
#include <vtkImageMapToColors.h>
#include <vtkImageMapper3D.h>
#include <vtkImageProperty.h>
#include <vtkLookupTable.h>
#include <vtkRenderer.h>

SegmentationOverlay::SegmentationOverlay(vtkRenderer* renderer,
                                         MPROrientation orientation)
    : m_renderer(renderer)
    , m_orientation(orientation)
{
    buildLookupTable();
}

SegmentationOverlay::~SegmentationOverlay()
{
    clear();
}

void SegmentationOverlay::setRenderer(vtkRenderer* renderer)
{
    if (m_renderer == renderer) return;
    clear();
    m_renderer = renderer;
    refresh();
}

void SegmentationOverlay::setLabelmap(vtkSmartPointer<vtkImageData> labelmap)
{
    m_labelmap = labelmap;
    refresh();
}

void SegmentationOverlay::setCurrentSlice(int sliceIndex)
{
    if (m_currentSlice == sliceIndex) return;
    m_currentSlice = sliceIndex;
    updateDisplayExtent();
}

void SegmentationOverlay::setHiddenLabels(const std::set<int>& hiddenLabels)
{
    if (m_hiddenLabels == hiddenLabels) return;
    m_hiddenLabels = hiddenLabels;
    buildLookupTable();
    if (m_colorMap) m_colorMap->Modified();
    refresh();
}

void SegmentationOverlay::setVisible(bool visible)
{
    if (m_visible == visible) return;
    m_visible = visible;
    refresh();
}

void SegmentationOverlay::setOpacity(double opacity)
{
    m_opacity = std::clamp(opacity, 0.0, 1.0);
    buildLookupTable();
    if (m_colorMap) m_colorMap->Modified();
    refresh();
}

void SegmentationOverlay::refresh()
{
    clear();
    if (!m_renderer || !m_labelmap || !m_visible) return;

    m_colorMap = vtkSmartPointer<vtkImageMapToColors>::New();
    m_colorMap->SetInputData(m_labelmap);
    m_colorMap->SetLookupTable(m_lookupTable);
    m_colorMap->PassAlphaToOutputOn();
    m_colorMap->Update();

    m_actor = vtkSmartPointer<vtkImageActor>::New();
    m_actor->GetMapper()->SetInputConnection(m_colorMap->GetOutputPort());
    m_actor->InterpolateOff();
    m_actor->PickableOff();
    m_actor->GetProperty()->SetOpacity(1.0);
    updateDisplayExtent();

    m_renderer->AddActor(m_actor);
}

void SegmentationOverlay::clear()
{
    if (m_renderer && m_actor) {
        m_renderer->RemoveActor(m_actor);
    }
    m_actor = nullptr;
    m_colorMap = nullptr;
}

void SegmentationOverlay::buildLookupTable()
{
    m_lookupTable = vtkSmartPointer<vtkLookupTable>::New();
    m_lookupTable->SetNumberOfTableValues(256);
    m_lookupTable->SetRange(0.0, 255.0);
    m_lookupTable->SetTableValue(0, 0.0, 0.0, 0.0, 0.0);

    auto alphaFor = [this](int label) {
        return m_hiddenLabels.count(label) ? 0.0 : m_opacity;
    };
    m_lookupTable->SetTableValue(1, 0.90, 0.25, 0.22, alphaFor(1)); // hueso
    m_lookupTable->SetTableValue(2, 0.20, 0.75, 1.00, alphaFor(2)); // tejidos blandos
    m_lookupTable->SetTableValue(3, 1.00, 0.92, 0.25, alphaFor(3)); // dientes superiores
    m_lookupTable->SetTableValue(4, 0.20, 1.00, 0.35, alphaFor(4)); // via aerea
    m_lookupTable->SetTableValue(5, 1.00, 0.62, 0.62, alphaFor(5)); // maxilar/craneo
    m_lookupTable->SetTableValue(6, 0.55, 0.75, 1.00, alphaFor(6)); // mandibula
    m_lookupTable->SetTableValue(7, 0.86, 0.37, 0.31, alphaFor(7)); // canal mandibular
    m_lookupTable->SetTableValue(8, 1.00, 0.72, 0.20, alphaFor(8)); // dientes inferiores

    for (int i = 9; i < 256; ++i) {
        const double r = ((i * 37) % 255) / 255.0;
        const double g = ((i * 73) % 255) / 255.0;
        const double b = ((i * 19) % 255) / 255.0;
        m_lookupTable->SetTableValue(i, r, g, b, alphaFor(i));
    }
    m_lookupTable->Build();
}

void SegmentationOverlay::updateDisplayExtent()
{
    if (!m_actor || !m_labelmap) return;

    int extent[6] = {};
    m_labelmap->GetExtent(extent);
    const int axis = normalAxis();
    const int slice = std::clamp(m_currentSlice, extent[axis * 2], extent[axis * 2 + 1]);

    int displayExtent[6] = {
        extent[0], extent[1],
        extent[2], extent[3],
        extent[4], extent[5]
    };
    displayExtent[axis * 2] = slice;
    displayExtent[axis * 2 + 1] = slice;
    m_actor->SetDisplayExtent(displayExtent);
}

int SegmentationOverlay::normalAxis() const
{
    switch (m_orientation) {
        case MPROrientation::Sagittal: return 0;
        case MPROrientation::Coronal:  return 1;
        case MPROrientation::Axial:    return 2;
    }
    return 2;
}
