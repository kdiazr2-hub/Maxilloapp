#include "SurfaceContourOverlay.h"

#include "CompositeBlockCore.h"

#include <vtkActor.h>
#include <vtkCamera.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderer.h>

SurfaceContourOverlay::SurfaceContourOverlay(vtkRenderer* renderer, MPROrientation orientation)
    : m_renderer(renderer)
    , m_orientation(orientation)
{
}

SurfaceContourOverlay::~SurfaceContourOverlay()
{
    clear();
}

void SurfaceContourOverlay::setRenderer(vtkRenderer* renderer)
{
    if (m_renderer == renderer) return;
    clear();
    m_renderer = renderer;
    refresh();
}

void SurfaceContourOverlay::setSurfaces(const std::vector<MPRSurfaceContour>& surfaces)
{
    m_surfaces = surfaces;
    refresh();
}

void SurfaceContourOverlay::setSlicePosition(double worldPosition)
{
    if (m_hasPosition && m_position == worldPosition) return;
    m_position = worldPosition;
    m_hasPosition = true;
    refresh();
}

void SurfaceContourOverlay::refresh()
{
    clear();
    if (!m_renderer || !m_hasPosition) return;

    const int axis = normalAxis();
    // Lift the lines slightly toward the camera so the CT slice never hides them.
    double lift = 0.05;
    if (auto* camera = m_renderer->GetActiveCamera()) {
        double position[3] = {};
        camera->GetPosition(position);
        lift = position[axis] >= m_position ? 0.05 : -0.05;
    }

    for (const MPRSurfaceContour& surface : m_surfaces) {
        if (!surface.mesh) continue;
        auto lines = CompositeBlockCore::SliceContour(surface.mesh, axis, m_position);
        if (!lines || lines->GetNumberOfLines() == 0) continue;
        if (auto* points = lines->GetPoints()) {
            double p[3] = {};
            for (vtkIdType i = 0; i < points->GetNumberOfPoints(); ++i) {
                points->GetPoint(i, p);
                p[axis] += lift;
                points->SetPoint(i, p);
            }
        }
        auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
        mapper->SetInputData(lines);
        mapper->ScalarVisibilityOff();
        auto actor = vtkSmartPointer<vtkActor>::New();
        actor->SetMapper(mapper);
        actor->GetProperty()->SetColor(surface.color[0], surface.color[1], surface.color[2]);
        actor->GetProperty()->SetLineWidth(2.0f);
        actor->GetProperty()->LightingOff();
        actor->PickableOff();
        m_renderer->AddActor(actor);
        m_actors.push_back(actor);
    }
}

void SurfaceContourOverlay::clear()
{
    if (m_renderer)
        for (const auto& actor : m_actors)
            m_renderer->RemoveActor(actor);
    m_actors.clear();
}

int SurfaceContourOverlay::normalAxis() const
{
    switch (m_orientation) {
        case MPROrientation::Sagittal: return 0;
        case MPROrientation::Coronal:  return 1;
        case MPROrientation::Axial:    return 2;
    }
    return 2;
}
