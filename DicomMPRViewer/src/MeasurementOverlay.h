#pragma once

#include <array>
#include <optional>
#include <vector>

#include <vtkSmartPointer.h>

#include "Measurement.h"

class vtkProp;
class vtkRenderer;

class MeasurementOverlay
{
public:
    MeasurementOverlay(vtkRenderer* renderer, MeasurementViewOrientation orientation);
    ~MeasurementOverlay();

    void setRenderer(vtkRenderer* renderer);
    void setCurrentSlice(int sliceIndex);
    void setMeasurements(const std::vector<Measurement>& measurements);
    void setPreviewMeasurement(const std::optional<Measurement>& measurement);
    void setGlobalVisible(bool visible);
    void refresh();
    void clear();

private:
    bool shouldDraw(const Measurement& measurement) const;
    void addMeasurement(const Measurement& measurement);
    void addPreviewMeasurement(const Measurement& measurement);
    void addPolyline(const std::vector<std::array<double, 3>>& points,
                     bool closed,
                     const double color[3],
                     double width);
    void addLine(const std::array<double, 3>& a,
                 const std::array<double, 3>& b,
                 const double color[3],
                 double width);
    void addRectangle(const Measurement& measurement, const double color[3]);
    void addCircle(const Measurement& measurement, const double color[3]);
    void addMarker(const std::array<double, 3>& point, const double color[3]);
    void addText(const std::array<double, 3>& point,
                 const std::string& text,
                 const double color[3]);
    bool worldToDisplay(const std::array<double, 3>& point, double display[3]) const;
    void planeAxes(int& horizontalAxis, int& verticalAxis, int& normalAxis) const;

    vtkRenderer* m_renderer = nullptr;
    MeasurementViewOrientation m_orientation = MeasurementViewOrientation::Axial;
    std::vector<Measurement> m_measurements;
    std::optional<Measurement> m_previewMeasurement;
    std::vector<vtkSmartPointer<vtkProp>> m_props;
    int m_currentSlice = 0;
    bool m_globalVisible = true;
};
