#include "MeasurementOverlay.h"

#include <iomanip>
#include <cmath>
#include <sstream>

#include <vtkActor.h>
#include <vtkActor2D.h>
#include <vtkBillboardTextActor3D.h>
#include <vtkCellArray.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkPolyDataMapper2D.h>
#include <vtkProperty.h>
#include <vtkProperty2D.h>
#include <vtkRenderer.h>
#include <vtkTextActor.h>
#include <vtkTextProperty.h>

static void planeAxesForView(MeasurementViewOrientation view,
                             int& horizontalAxis,
                             int& verticalAxis,
                             int& normalAxis)
{
    switch (view) {
        case MeasurementViewOrientation::Axial:
            horizontalAxis = 0; verticalAxis = 1; normalAxis = 2; break;
        case MeasurementViewOrientation::Coronal:
            horizontalAxis = 0; verticalAxis = 2; normalAxis = 1; break;
        case MeasurementViewOrientation::Sagittal:
            horizontalAxis = 1; verticalAxis = 2; normalAxis = 0; break;
    }
}

static std::string fixedLabel(double value, const char* unit)
{
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1) << value << " " << unit;
    return ss.str();
}

static double roiPreviewAreaMm2(const Measurement& measurement)
{
    if (measurement.pointsPhysical.size() < 2) return 0.0;

    int h = 0;
    int v = 1;
    int n = 2;
    planeAxesForView(measurement.viewOrientation, h, v, n);
    (void)n;

    const auto& a = measurement.pointsPhysical[0];
    const auto& b = measurement.pointsPhysical[1];
    if (measurement.roiShape == MeasurementROIShape::Circle) {
        const double radius = std::sqrt((b[h] - a[h]) * (b[h] - a[h]) +
                                        (b[v] - a[v]) * (b[v] - a[v]));
        return 3.14159265358979323846 * radius * radius;
    }
    return std::abs((b[h] - a[h]) * (b[v] - a[v]));
}

MeasurementOverlay::MeasurementOverlay(vtkRenderer* renderer,
                                       MeasurementViewOrientation orientation)
    : m_renderer(renderer)
    , m_orientation(orientation)
{
}

MeasurementOverlay::~MeasurementOverlay()
{
    clear();
}

void MeasurementOverlay::setRenderer(vtkRenderer* renderer)
{
    if (m_renderer == renderer) return;
    clear();
    m_renderer = renderer;
    refresh();
}

void MeasurementOverlay::setCurrentSlice(int sliceIndex)
{
    if (m_currentSlice == sliceIndex) return;
    m_currentSlice = sliceIndex;
    refresh();
}

void MeasurementOverlay::setMeasurements(const std::vector<Measurement>& measurements)
{
    m_measurements = measurements;
    refresh();
}

void MeasurementOverlay::setPreviewMeasurement(const std::optional<Measurement>& measurement)
{
    m_previewMeasurement = measurement;
    refresh();
}

void MeasurementOverlay::setGlobalVisible(bool visible)
{
    if (m_globalVisible == visible) return;
    m_globalVisible = visible;
    refresh();
}

void MeasurementOverlay::refresh()
{
    clear();
    if (!m_renderer) return;

    if (m_globalVisible) {
        for (const Measurement& measurement : m_measurements) {
            if (shouldDraw(measurement)) addMeasurement(measurement);
        }
    }

    if (m_previewMeasurement.has_value() && shouldDraw(*m_previewMeasurement)) {
        addPreviewMeasurement(*m_previewMeasurement);
    }
}

void MeasurementOverlay::clear()
{
    if (m_renderer) {
        for (auto& prop : m_props) {
            m_renderer->RemoveViewProp(prop);
        }
    }
    m_props.clear();
}

bool MeasurementOverlay::shouldDraw(const Measurement& measurement) const
{
    return measurement.visible &&
           measurement.viewOrientation == m_orientation &&
           measurement.sliceIndex == m_currentSlice;
}

void MeasurementOverlay::addPreviewMeasurement(const Measurement& measurement)
{
    if (measurement.pointsPhysical.empty()) return;

    constexpr double preview[3] = {0.20, 0.75, 1.00};
    constexpr double white[3] = {1.00, 1.00, 1.00};

    const auto& points = measurement.pointsPhysical;
    for (size_t i = 0; i < points.size(); ++i) {
        addMarker(points[i], preview);
        addText(points[i], "P" + std::to_string(i + 1), white);
    }

    switch (measurement.type) {
        case MeasurementType::Distance:
            if (points.size() >= 2) {
                addLine(points[0], points[1], preview, 2.0);
                addText(points[1], fixedLabel(distanceMm(points[0], points[1]), "mm"), preview);
            }
            break;
        case MeasurementType::Angle:
            if (points.size() == 2) {
                addLine(points[0], points[1], preview, 2.0);
            } else if (points.size() >= 3) {
                addLine(points[1], points[0], preview, 2.0);
                addLine(points[1], points[2], preview, 2.0);
                addText(points[1], fixedLabel(angleDegrees(points[0], points[1], points[2]), "deg"), preview);
            }
            break;
        case MeasurementType::Area:
            addPolyline(points, false, preview, 2.0);
            if (points.size() >= 3) {
                addText(points.back(),
                        fixedLabel(polygonAreaMm2(points, measurement.viewOrientation), "mm2"),
                        preview);
            }
            break;
        case MeasurementType::ROIStatistics:
            if (points.size() >= 2) {
                if (measurement.roiShape == MeasurementROIShape::Circle) {
                    addCircle(measurement, preview);
                } else {
                    addRectangle(measurement, preview);
                }
                addText(points[1], fixedLabel(roiPreviewAreaMm2(measurement), "mm2"), preview);
            }
            break;
        case MeasurementType::PerpendicularDistance:
            if (points.size() >= 2) addLine(points[0], points[1], preview, 2.0);
            if (points.size() >= 3) {
                const auto projection = perpendicularProjection(points[0], points[1], points[2]);
                addLine(points[2], projection, preview, 2.0);
                addMarker(projection, preview);
                addText(points[2], fixedLabel(distanceMm(points[2], projection), "mm"), preview);
            }
            break;
        case MeasurementType::Annotation:
            break;
    }
}

void MeasurementOverlay::addMeasurement(const Measurement& measurement)
{
    constexpr double yellow[3] = {1.0, 0.88, 0.12};
    constexpr double cyan[3] = {0.15, 0.95, 1.0};
    constexpr double orange[3] = {1.0, 0.55, 0.18};
    constexpr double green[3] = {0.30, 1.00, 0.35};
    constexpr double magenta[3] = {1.00, 0.35, 0.90};
    constexpr double violet[3] = {0.75, 0.55, 1.00};

    if (measurement.type == MeasurementType::Distance &&
        measurement.pointsPhysical.size() >= 2) {
        const auto& a = measurement.pointsPhysical[0];
        const auto& b = measurement.pointsPhysical[1];
        addLine(a, b, yellow, 2.5);
        addMarker(a, yellow);
        addMarker(b, yellow);

        addText(b, fixedLabel(measurement.value, "mm"), yellow);
    } else if (measurement.type == MeasurementType::Angle &&
               measurement.pointsPhysical.size() >= 3) {
        const auto& p1 = measurement.pointsPhysical[0];
        const auto& vertex = measurement.pointsPhysical[1];
        const auto& p3 = measurement.pointsPhysical[2];
        addLine(vertex, p1, cyan, 2.5);
        addLine(vertex, p3, cyan, 2.5);
        addMarker(p1, cyan);
        addMarker(vertex, cyan);
        addMarker(p3, cyan);

        addText(vertex, fixedLabel(measurement.value, "deg"), cyan);
    } else if (measurement.type == MeasurementType::Annotation &&
               !measurement.pointsPhysical.empty()) {
        addMarker(measurement.pointsPhysical[0], orange);
        addText(measurement.pointsPhysical[0], measurement.text, orange);
    } else if (measurement.type == MeasurementType::Area &&
               measurement.pointsPhysical.size() >= 3) {
        addPolyline(measurement.pointsPhysical, true, green, 2.5);
        for (const auto& p : measurement.pointsPhysical) {
            addMarker(p, green);
        }
        addText(measurement.pointsPhysical.back(),
                fixedLabel(measurement.value, "mm2"),
                green);
    } else if (measurement.type == MeasurementType::ROIStatistics &&
               measurement.pointsPhysical.size() >= 2) {
        if (measurement.roiShape == MeasurementROIShape::Circle) {
            addCircle(measurement, magenta);
        } else {
            addRectangle(measurement, magenta);
        }
        addMarker(measurement.pointsPhysical[0], magenta);
        addMarker(measurement.pointsPhysical[1], magenta);
        addText(measurement.pointsPhysical[1], measurement.text, magenta);
    } else if (measurement.type == MeasurementType::PerpendicularDistance &&
               measurement.pointsPhysical.size() >= 4) {
        const auto& a = measurement.pointsPhysical[0];
        const auto& b = measurement.pointsPhysical[1];
        const auto& p = measurement.pointsPhysical[2];
        const auto& proj = measurement.pointsPhysical[3];
        addLine(a, b, violet, 2.5);
        addLine(p, proj, violet, 2.0);
        addMarker(a, violet);
        addMarker(b, violet);
        addMarker(p, violet);
        addMarker(proj, violet);

        addText(p, fixedLabel(measurement.value, "mm"), violet);
    }
}

void MeasurementOverlay::addPolyline(const std::vector<std::array<double, 3>>& points,
                                     bool closed,
                                     const double color[3],
                                     double width)
{
    if (points.size() < 2) return;

    for (size_t i = 1; i < points.size(); ++i) {
        addLine(points[i - 1], points[i], color, width);
    }
    if (closed && points.size() > 2) {
        addLine(points.back(), points.front(), color, width);
    }
}

void MeasurementOverlay::addLine(const std::array<double, 3>& a,
                                 const std::array<double, 3>& b,
                                 const double color[3],
                                 double width)
{
    if (!m_renderer) return;

    auto points = vtkSmartPointer<vtkPoints>::New();
    points->SetNumberOfPoints(2);
    points->SetPoint(0, a.data());
    points->SetPoint(1, b.data());

    auto lines = vtkSmartPointer<vtkCellArray>::New();
    lines->InsertNextCell(2);
    lines->InsertCellPoint(0);
    lines->InsertCellPoint(1);

    auto data = vtkSmartPointer<vtkPolyData>::New();
    data->SetPoints(points);
    data->SetLines(lines);

    auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    mapper->SetInputData(data);

    auto actor = vtkSmartPointer<vtkActor>::New();
    actor->SetMapper(mapper);
    actor->GetProperty()->SetColor(color[0], color[1], color[2]);
    actor->GetProperty()->SetLineWidth(width);
    actor->PickableOff();

    m_renderer->AddActor(actor);
    m_props.push_back(actor);
}

void MeasurementOverlay::addRectangle(const Measurement& measurement,
                                      const double color[3])
{
    if (measurement.pointsPhysical.size() < 2) return;

    int h = 0;
    int v = 1;
    int n = 2;
    planeAxes(h, v, n);

    const auto& a = measurement.pointsPhysical[0];
    const auto& b = measurement.pointsPhysical[1];
    std::vector<std::array<double, 3>> corners(4, a);
    corners[0][h] = a[h]; corners[0][v] = a[v]; corners[0][n] = a[n];
    corners[1][h] = b[h]; corners[1][v] = a[v]; corners[1][n] = a[n];
    corners[2][h] = b[h]; corners[2][v] = b[v]; corners[2][n] = a[n];
    corners[3][h] = a[h]; corners[3][v] = b[v]; corners[3][n] = a[n];
    addPolyline(corners, true, color, 2.5);
}

void MeasurementOverlay::addCircle(const Measurement& measurement,
                                   const double color[3])
{
    if (measurement.pointsPhysical.size() < 2) return;

    int h = 0;
    int v = 1;
    int n = 2;
    planeAxes(h, v, n);

    const auto& center = measurement.pointsPhysical[0];
    const auto& edge = measurement.pointsPhysical[1];
    const double radius = std::sqrt((edge[h] - center[h]) * (edge[h] - center[h]) +
                                    (edge[v] - center[v]) * (edge[v] - center[v]));
    if (radius <= 1e-6) return;

    std::vector<std::array<double, 3>> points;
    points.reserve(64);
    for (int i = 0; i < 64; ++i) {
        const double angle = 2.0 * 3.14159265358979323846 *
                             static_cast<double>(i) / 64.0;
        std::array<double, 3> p = center;
        p[h] = center[h] + radius * std::cos(angle);
        p[v] = center[v] + radius * std::sin(angle);
        p[n] = center[n];
        points.push_back(p);
    }
    addPolyline(points, true, color, 2.5);
}

void MeasurementOverlay::addMarker(const std::array<double, 3>& point,
                                   const double color[3])
{
    if (!m_renderer) return;

    double display[3] = {};
    if (!worldToDisplay(point, display)) return;

    constexpr int sides = 24;
    constexpr double radiusPx = 5.5;

    auto points = vtkSmartPointer<vtkPoints>::New();
    points->SetNumberOfPoints(sides);

    for (int i = 0; i < sides; ++i) {
        const double angle = 2.0 * 3.14159265358979323846 *
                             static_cast<double>(i) / static_cast<double>(sides);
        points->SetPoint(i,
                         display[0] + radiusPx * std::cos(angle),
                         display[1] + radiusPx * std::sin(angle),
                         0.0);
    }

    auto polygon = vtkSmartPointer<vtkCellArray>::New();
    polygon->InsertNextCell(sides);
    for (int i = 0; i < sides; ++i) {
        polygon->InsertCellPoint(i);
    }

    auto data = vtkSmartPointer<vtkPolyData>::New();
    data->SetPoints(points);
    data->SetPolys(polygon);

    auto mapper = vtkSmartPointer<vtkPolyDataMapper2D>::New();
    mapper->SetInputData(data);

    auto actor = vtkSmartPointer<vtkActor2D>::New();
    actor->SetMapper(mapper);
    actor->GetProperty()->SetColor(color[0], color[1], color[2]);
    actor->GetProperty()->SetOpacity(1.0);
    actor->PickableOff();

    m_renderer->AddActor2D(actor);
    m_props.push_back(actor);
}

void MeasurementOverlay::addText(const std::array<double, 3>& point,
                                 const std::string& text,
                                 const double color[3])
{
    if (!m_renderer || text.empty()) return;

    double display[3] = {};
    if (!worldToDisplay(point, display)) return;

    auto actor = vtkSmartPointer<vtkTextActor>::New();
    actor->SetInput(text.c_str());
    actor->SetDisplayPosition(static_cast<int>(std::lround(display[0] + 10.0)),
                              static_cast<int>(std::lround(display[1] + 10.0)));
    actor->GetTextProperty()->SetColor(color[0], color[1], color[2]);
    actor->GetTextProperty()->SetFontSize(18);
    actor->GetTextProperty()->BoldOn();
    actor->GetTextProperty()->SetBackgroundColor(0.0, 0.0, 0.0);
    actor->GetTextProperty()->SetBackgroundOpacity(0.45);
    actor->PickableOff();

    m_renderer->AddActor2D(actor);
    m_props.push_back(actor);
}

bool MeasurementOverlay::worldToDisplay(const std::array<double, 3>& point,
                                        double display[3]) const
{
    if (!m_renderer || !display) return false;

    m_renderer->SetWorldPoint(point[0], point[1], point[2], 1.0);
    m_renderer->WorldToDisplay();
    m_renderer->GetDisplayPoint(display);

    return std::isfinite(display[0]) &&
           std::isfinite(display[1]) &&
           std::isfinite(display[2]);
}

void MeasurementOverlay::planeAxes(int& horizontalAxis,
                                   int& verticalAxis,
                                   int& normalAxis) const
{
    switch (m_orientation) {
        case MeasurementViewOrientation::Axial:
            horizontalAxis = 0;
            verticalAxis = 1;
            normalAxis = 2;
            break;
        case MeasurementViewOrientation::Coronal:
            horizontalAxis = 0;
            verticalAxis = 2;
            normalAxis = 1;
            break;
        case MeasurementViewOrientation::Sagittal:
            horizontalAxis = 1;
            verticalAxis = 2;
            normalAxis = 0;
            break;
    }
}
