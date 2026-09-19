#include "MPRView.h"
#include "SurfaceContourOverlay.h"
#include "CranioPalette.h"
#include "MeasurementOverlay.h"
#include "SegmentationOverlay.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QResizeEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QSlider>
#include <QLabel>
#include <QFrame>
#include <QVTKOpenGLNativeWidget.h>

#include <vtkCommand.h>
#include <vtkResliceImageViewer.h>
#include <vtkResliceCursor.h>
#include <vtkResliceCursorActor.h>
#include <vtkResliceCursorLineRepresentation.h>
#include <vtkResliceCursorPolyDataAlgorithm.h>
#include <vtkResliceCursorWidget.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkCamera.h>
#include <vtkActor.h>
#include <vtkCellArray.h>
#include <vtkCellPicker.h>
#include <vtkImageData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkCornerAnnotation.h>
#include <vtkProperty.h>
#include <vtkTextProperty.h>

static MeasurementViewOrientation toMeasurementView(MPROrientation orientation)
{
    switch (orientation) {
        case MPROrientation::Axial:    return MeasurementViewOrientation::Axial;
        case MPROrientation::Coronal:  return MeasurementViewOrientation::Coronal;
        case MPROrientation::Sagittal: return MeasurementViewOrientation::Sagittal;
    }
    return MeasurementViewOrientation::Axial;
}

class MPRViewInteractionCallback : public vtkCommand
{
public:
    static MPRViewInteractionCallback* New()
    {
        return new MPRViewInteractionCallback;
    }

    void SetOwner(MPRView* owner) { m_owner = owner; }

    void Execute(vtkObject*, unsigned long, void*) override
    {
        if (m_owner) m_owner->handleVTKInteraction();
    }

private:
    MPRView* m_owner = nullptr;
};

// ─────────────────────────────────────────────────────────────────────────────
// LassoCanvas
//
// Transparent overlay widget that sits on top of the VTK render widget and
// paints the in-progress lasso polygon as the user drags the mouse.
// WA_TransparentForMouseEvents keeps all mouse events flowing through to VTK.
// ─────────────────────────────────────────────────────────────────────────────
class LassoCanvas : public QWidget
{
public:
    explicit LassoCanvas(QWidget* parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_TranslucentBackground);
        setStyleSheet("background: transparent;");
    }

    void setPoints(const QVector<QPointF>& pts, bool addMode)
    {
        m_points  = pts;
        m_addMode = addMode;
        update();
    }

    void clearPoints()
    {
        m_points.clear();
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        if (m_points.size() < 2) return;

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        // Green = add, Red = erase
        const QColor lineColor = m_addMode
            ? QColor(50, 230, 80,  220)
            : QColor(230, 55, 55, 220);

        QPen pen(lineColor, 1.5, Qt::DashLine);
        p.setPen(pen);

        for (int i = 0; i + 1 < m_points.size(); ++i)
            p.drawLine(m_points[i], m_points[i + 1]);

        // Close the polygon
        if (m_points.size() > 2)
            p.drawLine(m_points.last(), m_points.first());

        // Draw vertices as small dots
        QPen dotPen(lineColor, 4.0, Qt::SolidLine);
        p.setPen(dotPen);
        p.drawPoint(m_points.first());
    }

private:
    QVector<QPointF> m_points;
    bool             m_addMode = true;
};

// ─────────────────────────────────────────────────────────────────────────────
MPRView::MPRView(MPROrientation orientation, QWidget* parent)
    : QWidget(parent)
    , m_orientation(orientation)
    , m_viewer(vtkSmartPointer<vtkResliceImageViewer>::New())
{
    buildLayout();
    auto* callback = MPRViewInteractionCallback::New();
    callback->SetOwner(this);
    m_interactionCallback.TakeReference(callback);
}

// ─────────────────────────────────────────────────────────────────────────────
MPRView::~MPRView() = default;

void MPRView::buildLayout()
{
    setStyleSheet(
        "MPRView { background-color:#1c1d20; border:1px solid #292b30; border-radius:4px; }"
        "QVTKOpenGLNativeWidget { border:0; }");

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // ── Title bar ─────────────────────────────────────────────────────────
    m_titleLabel = new QLabel(this);
    m_titleLabel->setAlignment(Qt::AlignCenter);
    m_titleLabel->setFixedHeight(24);
    m_titleLabel->setStyleSheet(
        "color:#d1d1d6; font-weight:600; font-size:11px;"
        "background-color:#202226; border:0; border-bottom:1px solid #292b30;");

    switch (m_orientation) {
        case MPROrientation::Axial:    m_titleLabel->setText("AXIAL");    break;
        case MPROrientation::Coronal:  m_titleLabel->setText("CORONAL");  break;
        case MPROrientation::Sagittal: m_titleLabel->setText("SAGITTAL"); break;
    }
    outer->addWidget(m_titleLabel);

    // ── VTK render surface ────────────────────────────────────────────────
    m_vtkWidget = new QVTKOpenGLNativeWidget(this);
    m_vtkWidget->setMinimumSize(256, 256);
    m_vtkWidget->installEventFilter(this);
    outer->addWidget(m_vtkWidget, 1 /*stretch*/);

    // ── Lasso canvas (transparent overlay on top of the VTK widget) ──────
    m_lassoCanvas = new LassoCanvas(m_vtkWidget);
    m_lassoCanvas->setGeometry(m_vtkWidget->rect());
    m_lassoCanvas->hide();
    m_lassoCanvas->raise();

    // ── Slice slider ──────────────────────────────────────────────────────
    m_slider = new QSlider(Qt::Horizontal, this);
    m_slider->setEnabled(false);
    m_slider->setStyleSheet(
        "QSlider::groove:horizontal { background:#3a3a3c; height:4px; border-radius:2px; }"
        "QSlider::handle:horizontal { background:#0a84ff; width:14px; height:14px;"
        "  margin:-5px 0; border-radius:7px; }"
        "QSlider::handle:horizontal:hover { background:#1d9bf0; }");
    m_slider->setContentsMargins(8, 0, 8, 0);
    outer->addWidget(m_slider);

    // ── Slice counter ─────────────────────────────────────────────────────
    m_sliceLabel = new QLabel("—", this);
    m_sliceLabel->setAlignment(Qt::AlignCenter);
    m_sliceLabel->setFixedHeight(18);
    m_sliceLabel->setStyleSheet("color:#98989d; font-size:10px; font-weight:500;");
    outer->addWidget(m_sliceLabel);

    connect(m_slider, &QSlider::valueChanged,
            this,     &MPRView::onSliderValueChanged);
}

bool MPRView::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_vtkWidget) {

        // Keep lasso canvas filling the vtk widget when it resizes
        if (event->type() == QEvent::Resize) {
            if (m_lassoCanvas) {
                m_lassoCanvas->setGeometry(m_vtkWidget->rect());
            }
            QTimer::singleShot(0, this, [this] {
                if (m_measurementOverlay) {
                    m_measurementOverlay->refresh();
                    render();
                }
            });
        }

        if (event->type() == QEvent::MouseButtonDblClick) {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (m_measurementPickingEnabled &&
                m_measurementFinishRequiresDoubleRightClick &&
                mouseEvent->button() == Qt::RightButton) {
                emit measurementCompleteRequested(this);
                return true;
            }
            emit fullScreenToggleRequested(this);
            return true;
        }

        // ── Lasso mode event handling ─────────────────────────────────────
        if (m_lassoActive) {
            if (event->type() == QEvent::MouseButtonPress) {
                auto* me = static_cast<QMouseEvent*>(event);
                if (me->button() == Qt::LeftButton) {
                    m_lassoDrawing = true;
                    m_lassoPoints.clear();
                    m_lassoPoints.append(me->pos());
                    if (m_lassoCanvas) {
                        static_cast<LassoCanvas*>(m_lassoCanvas)
                            ->setPoints(m_lassoPoints, m_lassoAddMode);
                        m_lassoCanvas->show();
                        m_lassoCanvas->raise();
                    }
                    return true;
                }
                if (me->button() == Qt::RightButton && m_lassoDrawing) {
                    // Right-click cancels the current stroke
                    m_lassoDrawing = false;
                    m_lassoPoints.clear();
                    if (m_lassoCanvas) {
                        static_cast<LassoCanvas*>(m_lassoCanvas)->clearPoints();
                        m_lassoCanvas->hide();
                    }
                    return true;
                }
                if (me->button() == Qt::RightButton) {
                    return true;
                }
            }

            if (event->type() == QEvent::MouseMove && m_lassoDrawing) {
                auto* me = static_cast<QMouseEvent*>(event);
                m_lassoPoints.append(me->pos());
                if (m_lassoCanvas) {
                    static_cast<LassoCanvas*>(m_lassoCanvas)
                        ->setPoints(m_lassoPoints, m_lassoAddMode);
                }
                return true;
            }

            if (event->type() == QEvent::MouseButtonRelease) {
                auto* me = static_cast<QMouseEvent*>(event);
                if (me->button() == Qt::LeftButton && m_lassoDrawing) {
                    m_lassoDrawing = false;
                    if (m_lassoCanvas) {
                        static_cast<LassoCanvas*>(m_lassoCanvas)->clearPoints();
                        m_lassoCanvas->hide();
                    }
                    if (m_lassoPoints.size() >= 3) {
                        // Convert from Qt logical pixels (y=0 at top) to
                        // VTK display pixels (y=0 at bottom of render window).
                        const double dpr = m_vtkWidget->devicePixelRatioF();
                        const double h   = static_cast<double>(m_vtkWidget->height());
                        QVector<QPointF> vtkPts;
                        vtkPts.reserve(m_lassoPoints.size());
                        for (const QPointF& p : m_lassoPoints) {
                            vtkPts.append(QPointF(p.x() * dpr,
                                                  (h - p.y()) * dpr));
                        }
                        emit lassoEditCompleted(this, vtkPts, m_lassoAddMode);
                    }
                    m_lassoPoints.clear();
                    return true;
                }
            }
        }

        // ── Measurement picking ───────────────────────────────────────────
        if (m_measurementPickingEnabled &&
            event->type() == QEvent::MouseButtonPress) {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::LeftButton) {
                std::array<double, 3> point = {};
                if (pickPhysicalPoint(mouseEvent->pos(), point)) {
                    emit physicalPointClicked(this, point[0], point[1], point[2]);
                }
                return true;
            }
            if (mouseEvent->button() == Qt::RightButton) {
                if (m_measurementFinishRequiresDoubleRightClick) {
                    return true;
                }
                emit measurementCompleteRequested(this);
                return true;
            }
        }

    }

    return QWidget::eventFilter(watched, event);
}

// ─────────────────────────────────────────────────────────────────────────────
void MPRView::setVolume(vtkSmartPointer<vtkImageData> imageData)
{
    m_imageData = imageData;
    initViewer();
}

// ─────────────────────────────────────────────────────────────────────────────
void MPRView::setSharedResliceCursor(vtkResliceCursor* cursor)
{
    if (!cursor) return;

    m_viewer->SetResliceCursor(cursor);
    configureResliceCursorRepresentation();
}

vtkResliceCursor* MPRView::resliceCursor() const
{
    return m_viewer ? m_viewer->GetResliceCursor() : nullptr;
}

void MPRView::initViewer()
{
    if (!m_imageData) return;

    // Wire the viewer to the Qt-managed render window.
    // Must be done before SetInputData so the renderer is available.
    m_viewer->SetRenderWindow(m_vtkWidget->renderWindow());
    m_viewer->SetupInteractor(
        m_vtkWidget->renderWindow()->GetInteractor());

    // Choose slice orientation.
    // vtkResliceImageViewer constants:
    //   SLICE_ORIENTATION_XY = 2  (axial    – Z axis varies)
    //   SLICE_ORIENTATION_XZ = 1  (coronal  – Y axis varies)
    //   SLICE_ORIENTATION_YZ = 0  (sagittal – X axis varies)
    switch (m_orientation) {
        case MPROrientation::Axial:
            m_viewer->SetSliceOrientationToXY();
            break;
        case MPROrientation::Coronal:
            m_viewer->SetSliceOrientationToXZ();
            break;
        case MPROrientation::Sagittal:
            m_viewer->SetSliceOrientationToYZ();
            break;
    }

    m_viewer->SetInputData(m_imageData);
    m_viewer->SetResliceModeToAxisAligned();
    installInteractionObservers();

    if (auto* cursor = m_viewer->GetResliceCursor()) {
        cursor->SetCenter(m_imageData->GetCenter());
    }

    // vtkResliceImageViewer calls InitializeCamera()+ResetCamera() on its
    // first Render(). This correctly fits the slice plane in the viewport
    // for all three orientations. Do NOT override SetParallelScale before
    // or after this call — it will either be ignored (before) or will
    // produce wrong zoom (after) because the internal value is already correct.
    m_viewer->Render();

    m_slider->setMinimum(sliceMin());
    m_slider->setMaximum(sliceMax());
    syncFromCursor();
    m_slider->setEnabled(true);
    buildCrosshairActors();
    updateCrosshair();
    if (!m_measurementOverlay) {
        m_measurementOverlay = std::make_unique<MeasurementOverlay>(
            m_viewer->GetRenderer(), toMeasurementView(m_orientation));
    } else {
        m_measurementOverlay->setRenderer(m_viewer->GetRenderer());
    }
    m_measurementOverlay->setCurrentSlice(currentSlice());
    if (!m_segmentationOverlay) {
        m_segmentationOverlay = std::make_unique<SegmentationOverlay>(
            m_viewer->GetRenderer(), m_orientation);
    } else {
        m_segmentationOverlay->setRenderer(m_viewer->GetRenderer());
    }
    m_segmentationOverlay->setCurrentSlice(currentSlice());
    if (!m_surfaceContourOverlay) {
        m_surfaceContourOverlay = std::make_unique<SurfaceContourOverlay>(
            m_viewer->GetRenderer(), m_orientation);
    } else {
        m_surfaceContourOverlay->setRenderer(m_viewer->GetRenderer());
    }
    m_surfaceContourOverlay->setSurfaces(m_surfaceContours);
    m_surfaceContourOverlay->setSlicePosition(slicePosition());

    if (m_viewer->GetRenderer()) {
        m_viewer->GetRenderer()->ResetCamera();
    }
    frameAnatomyOnCurrentSlice();
    m_viewer->Render();
}

// ─────────────────────────────────────────────────────────────────────────────
void MPRView::onSliderValueChanged(int value)
{
    setSliceIndex(value, true);
}

// ─────────────────────────────────────────────────────────────────────────────
void MPRView::applyWindowLevel(const WindowLevelPreset& preset)
{
    if (!m_imageData) return;
    m_viewer->SetColorWindow(preset.window);
    m_viewer->SetColorLevel(preset.level);
    m_viewer->Render();
}

// ─────────────────────────────────────────────────────────────────────────────
void MPRView::resetCamera()
{
    if (m_viewer->GetRenderer())
        m_viewer->GetRenderer()->ResetCamera();
    frameAnatomyOnCurrentSlice();
    if (m_measurementOverlay) {
        m_measurementOverlay->refresh();
    }
    m_viewer->Render();
}

// ─────────────────────────────────────────────────────────────────────────────
void MPRView::render()
{
    if (m_viewer) m_viewer->Render();
}

void MPRView::syncFromCursor()
{
    if (!m_imageData || !m_slider) return;

    const int index = sliceIndexFromCursor();
    m_slider->blockSignals(true);
    m_slider->setMinimum(sliceMin());
    m_slider->setMaximum(sliceMax());
    m_slider->setValue(index);
    m_slider->blockSignals(false);
    m_viewer->SetSlice(index);
    updateCrosshair();
    if (m_measurementOverlay) {
        m_measurementOverlay->setCurrentSlice(index);
    }
    if (m_segmentationOverlay) {
        m_segmentationOverlay->setCurrentSlice(index);
    }
    if (m_surfaceContourOverlay) {
        m_surfaceContourOverlay->setSlicePosition(slicePosition());
    }
    refreshSliceLabel();
}

void MPRView::setFullScreenActive(bool active)
{
    m_titleLabel->setStyleSheet(
        active
            ? "color:#ffffff; font-weight:700; font-size:11px;"
              "background-color:#0a84ff; border-radius:10px;"
            : "color:#f5f5f7; font-weight:700; font-size:11px;"
              "background-color:#2c2c2e; border-radius:10px;");
}

void MPRView::setMeasurementPickingEnabled(bool enabled)
{
    m_measurementPickingEnabled = enabled;
    const auto cursor = enabled ? Qt::CrossCursor : Qt::ArrowCursor;
    setCursor(cursor);
    if (m_vtkWidget) m_vtkWidget->setCursor(cursor);
}

void MPRView::setMeasurementFinishRequiresDoubleRightClick(bool enabled)
{
    m_measurementFinishRequiresDoubleRightClick = enabled;
}

void MPRView::setMeasurements(const std::vector<Measurement>& measurements,
                              bool visible)
{
    if (!m_viewer || !m_viewer->GetRenderer()) return;
    if (!m_measurementOverlay) {
        m_measurementOverlay = std::make_unique<MeasurementOverlay>(
            m_viewer->GetRenderer(), toMeasurementView(m_orientation));
    }
    m_measurementOverlay->setCurrentSlice(currentSlice());
    m_measurementOverlay->setGlobalVisible(visible);
    m_measurementOverlay->setMeasurements(measurements);
    render();
}

void MPRView::setMeasurementPreview(const std::optional<Measurement>& measurement)
{
    if (!m_viewer || !m_viewer->GetRenderer()) return;
    if (!m_measurementOverlay) {
        m_measurementOverlay = std::make_unique<MeasurementOverlay>(
            m_viewer->GetRenderer(), toMeasurementView(m_orientation));
    }
    m_measurementOverlay->setCurrentSlice(currentSlice());
    m_measurementOverlay->setPreviewMeasurement(measurement);
    render();
}

void MPRView::setSegmentationLabelmap(vtkSmartPointer<vtkImageData> labelmap)
{
    if (!m_viewer || !m_viewer->GetRenderer()) return;
    if (!m_segmentationOverlay) {
        m_segmentationOverlay = std::make_unique<SegmentationOverlay>(
            m_viewer->GetRenderer(), m_orientation);
    }
    m_segmentationOverlay->setCurrentSlice(currentSlice());
    m_segmentationOverlay->setLabelmap(labelmap);
    render();
}

void MPRView::setSegmentationHiddenLabels(const std::set<int>& hiddenLabels)
{
    if (!m_viewer || !m_viewer->GetRenderer()) return;
    if (!m_segmentationOverlay) {
        m_segmentationOverlay = std::make_unique<SegmentationOverlay>(
            m_viewer->GetRenderer(), m_orientation);
    }
    m_segmentationOverlay->setHiddenLabels(hiddenLabels);
    render();
}

void MPRView::setSegmentationVisible(bool visible)
{
    if (!m_viewer || !m_viewer->GetRenderer()) return;
    if (!m_segmentationOverlay) {
        m_segmentationOverlay = std::make_unique<SegmentationOverlay>(
            m_viewer->GetRenderer(), m_orientation);
    }
    m_segmentationOverlay->setVisible(visible);
    render();
}

void MPRView::setSegmentationOpacity(double opacity)
{
    if (!m_viewer || !m_viewer->GetRenderer()) return;
    if (!m_segmentationOverlay) {
        m_segmentationOverlay = std::make_unique<SegmentationOverlay>(
            m_viewer->GetRenderer(), m_orientation);
    }
    m_segmentationOverlay->setOpacity(opacity);
    render();
}

void MPRView::configureResliceCursorRepresentation()
{
    auto* widget = m_viewer->GetResliceCursorWidget();
    if (!widget) return;

    auto* rep = vtkResliceCursorLineRepresentation::SafeDownCast(widget->GetRepresentation());
    if (!rep) return;

    auto* actor = rep->GetResliceCursorActor();
    actor->GetCursorAlgorithm()->SetReslicePlaneNormal(sliceAxis());

    const double colors[3][3] = {
        {0.95, 0.25, 0.25},
        {0.35, 0.85, 0.35},
        {0.25, 0.55, 1.00}
    };
    for (int i = 0; i < 3; ++i) {
        if (auto* prop = actor->GetCenterlineProperty(i)) {
            prop->SetColor(colors[i][0], colors[i][1], colors[i][2]);
            prop->SetLineWidth(2.0);
            prop->SetOpacity(1.0);
        }
    }
}

void MPRView::installInteractionObservers()
{
    if (m_observersInstalled || !m_interactionCallback) return;

    if (auto* widget = m_viewer->GetResliceCursorWidget()) {
        widget->AddObserver(vtkResliceCursorWidget::ResliceAxesChangedEvent,
                            m_interactionCallback);
        widget->AddObserver(vtkResliceCursorWidget::ResetCursorEvent,
                            m_interactionCallback);
    }
    m_viewer->AddObserver(vtkResliceImageViewer::SliceChangedEvent,
                          m_interactionCallback);
    m_observersInstalled = true;
}

void MPRView::handleVTKInteraction()
{
    if (m_suppressVtkEvents) return;

    updateCursorCenterFromSlice(m_viewer->GetSlice());
    syncFromCursor();
    emit reslicePositionChanged(this);
}

static vtkSmartPointer<vtkPolyData> makeCrosshairLineData()
{
    auto points = vtkSmartPointer<vtkPoints>::New();
    points->SetNumberOfPoints(2);
    points->SetPoint(0, 0.0, 0.0, 0.0);
    points->SetPoint(1, 1.0, 1.0, 1.0);

    auto lines = vtkSmartPointer<vtkCellArray>::New();
    lines->InsertNextCell(2);
    lines->InsertCellPoint(0);
    lines->InsertCellPoint(1);

    auto data = vtkSmartPointer<vtkPolyData>::New();
    data->SetPoints(points);
    data->SetLines(lines);
    return data;
}

void MPRView::buildCrosshairActors()
{
    if (m_crosshairBuilt || !m_viewer->GetRenderer()) return;

    m_crosshairHorizontalData = makeCrosshairLineData();
    m_crosshairVerticalData = makeCrosshairLineData();

    auto horizontalMapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    horizontalMapper->SetInputData(m_crosshairHorizontalData);

    auto verticalMapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    verticalMapper->SetInputData(m_crosshairVerticalData);

    m_crosshairHorizontalActor = vtkSmartPointer<vtkActor>::New();
    m_crosshairHorizontalActor->SetMapper(horizontalMapper);
    m_crosshairHorizontalActor->GetProperty()->SetColor(0.95, 0.25, 0.25);
    m_crosshairHorizontalActor->GetProperty()->SetLineWidth(1.5);
    m_crosshairHorizontalActor->PickableOff();

    m_crosshairVerticalActor = vtkSmartPointer<vtkActor>::New();
    m_crosshairVerticalActor->SetMapper(verticalMapper);
    m_crosshairVerticalActor->GetProperty()->SetColor(0.35, 0.85, 0.35);
    m_crosshairVerticalActor->GetProperty()->SetLineWidth(1.5);
    m_crosshairVerticalActor->PickableOff();

    m_viewer->GetRenderer()->AddActor(m_crosshairHorizontalActor);
    m_viewer->GetRenderer()->AddActor(m_crosshairVerticalActor);
    m_crosshairBuilt = true;
}

void MPRView::updateCrosshair()
{
    if (!m_imageData || !m_viewer->GetResliceCursor() ||
        !m_crosshairHorizontalData || !m_crosshairVerticalData) {
        return;
    }

    double center[3] = {};
    double bounds[6] = {};
    m_viewer->GetResliceCursor()->GetCenter(center);
    m_imageData->GetBounds(bounds);

    int horizontalAxis = 0;
    int verticalAxis = 1;
    int normalAxis = 2;
    planeAxes(horizontalAxis, verticalAxis, normalAxis);

    double p0[3] = {center[0], center[1], center[2]};
    double p1[3] = {center[0], center[1], center[2]};
    p0[horizontalAxis] = bounds[horizontalAxis * 2];
    p1[horizontalAxis] = bounds[horizontalAxis * 2 + 1];
    p0[verticalAxis] = center[verticalAxis];
    p1[verticalAxis] = center[verticalAxis];
    p0[normalAxis] = center[normalAxis];
    p1[normalAxis] = center[normalAxis];
    m_crosshairHorizontalData->GetPoints()->SetPoint(0, p0);
    m_crosshairHorizontalData->GetPoints()->SetPoint(1, p1);
    m_crosshairHorizontalData->GetPoints()->Modified();
    m_crosshairHorizontalData->Modified();

    p0[horizontalAxis] = center[horizontalAxis];
    p1[horizontalAxis] = center[horizontalAxis];
    p0[verticalAxis] = bounds[verticalAxis * 2];
    p1[verticalAxis] = bounds[verticalAxis * 2 + 1];
    p0[normalAxis] = center[normalAxis];
    p1[normalAxis] = center[normalAxis];
    m_crosshairVerticalData->GetPoints()->SetPoint(0, p0);
    m_crosshairVerticalData->GetPoints()->SetPoint(1, p1);
    m_crosshairVerticalData->GetPoints()->Modified();
    m_crosshairVerticalData->Modified();
}

int MPRView::sliceAxis() const
{
    switch (m_orientation) {
        case MPROrientation::Sagittal: return 0;
        case MPROrientation::Coronal:  return 1;
        case MPROrientation::Axial:    return 2;
    }
    return 2;
}

void MPRView::planeAxes(int& horizontalAxis, int& verticalAxis, int& normalAxis) const
{
    switch (m_orientation) {
        case MPROrientation::Axial:
            horizontalAxis = 0;
            verticalAxis = 1;
            normalAxis = 2;
            break;
        case MPROrientation::Coronal:
            horizontalAxis = 0;
            verticalAxis = 2;
            normalAxis = 1;
            break;
        case MPROrientation::Sagittal:
            horizontalAxis = 1;
            verticalAxis = 2;
            normalAxis = 0;
            break;
    }
}

int MPRView::sliceMin() const
{
    if (!m_imageData) return 0;
    int extent[6] = {};
    m_imageData->GetExtent(extent);
    return extent[sliceAxis() * 2];
}

int MPRView::sliceMax() const
{
    if (!m_imageData) return 0;
    int extent[6] = {};
    m_imageData->GetExtent(extent);
    return extent[sliceAxis() * 2 + 1];
}

int MPRView::clampSlice(int value) const
{
    return std::clamp(value, sliceMin(), sliceMax());
}

int MPRView::sliceIndexFromCursor() const
{
    if (!m_imageData || !m_viewer->GetResliceCursor()) return 0;

    double center[3] = {};
    double origin[3] = {};
    double spacing[3] = {1.0, 1.0, 1.0};
    m_viewer->GetResliceCursor()->GetCenter(center);
    m_imageData->GetOrigin(origin);
    m_imageData->GetSpacing(spacing);

    const int axis = sliceAxis();
    const double step = spacing[axis] == 0.0 ? 1.0 : spacing[axis];
    const auto index = static_cast<int>(std::lround((center[axis] - origin[axis]) / step));
    return clampSlice(index);
}

void MPRView::setSliceIndex(int value, bool notify)
{
    if (!m_imageData || !m_viewer->GetResliceCursor()) return;

    const int index = clampSlice(value);
    if (index == currentSlice()) {
        syncFromCursor();
        return;
    }

    m_suppressVtkEvents = true;
    updateCursorCenterFromSlice(index);
    syncFromCursor();
    m_viewer->Render();
    m_suppressVtkEvents = false;

    if (notify) emit reslicePositionChanged(this);
}

void MPRView::updateCursorCenterFromSlice(int index)
{
    if (!m_imageData || !m_viewer->GetResliceCursor()) return;

    const int clamped = clampSlice(index);
    double center[3] = {};
    double origin[3] = {};
    double spacing[3] = {1.0, 1.0, 1.0};
    m_viewer->GetResliceCursor()->GetCenter(center);
    m_imageData->GetOrigin(origin);
    m_imageData->GetSpacing(spacing);

    const int axis = sliceAxis();
    center[axis] = origin[axis] + clamped * spacing[axis];
    m_viewer->GetResliceCursor()->SetCenter(center);
}

int MPRView::currentSlice() const { return sliceIndexFromCursor(); }

double MPRView::slicePosition() const
{
    if (!m_imageData) return 0.0;
    double origin[3] = {};
    double spacing[3] = {1.0, 1.0, 1.0};
    m_imageData->GetOrigin(origin);
    m_imageData->GetSpacing(spacing);
    const int axis = sliceAxis();
    return origin[axis] + currentSlice() * spacing[axis];
}

void MPRView::setSurfaceContours(const std::vector<MPRSurfaceContour>& contours)
{
    m_surfaceContours = contours;
    if (m_surfaceContourOverlay) {
        m_surfaceContourOverlay->setSurfaces(m_surfaceContours);
        if (m_imageData) m_viewer->Render();
    }
}

int MPRView::surfaceContourCount() const
{
    return static_cast<int>(m_surfaceContours.size());
}

void MPRView::frameAnatomyOnCurrentSlice()
{
    if (!m_imageData || !m_viewer || !m_viewer->GetRenderer())
        return;

    auto* camera = m_viewer->GetRenderer()->GetActiveCamera();
    if (!camera)
        return;

    int extent[6] = {};
    double origin[3] = {};
    double spacing[3] = {1.0, 1.0, 1.0};
    m_imageData->GetExtent(extent);
    m_imageData->GetOrigin(origin);
    m_imageData->GetSpacing(spacing);

    int horizontalAxis = 0;
    int verticalAxis = 1;
    int normalAxis = 2;
    planeAxes(horizontalAxis, verticalAxis, normalAxis);

    const int fixed = clampSlice(currentSlice());
    const int h0 = extent[horizontalAxis * 2];
    const int h1 = extent[horizontalAxis * 2 + 1];
    const int v0 = extent[verticalAxis * 2];
    const int v1 = extent[verticalAxis * 2 + 1];

    constexpr double kAirThresholdHu = -700.0;
    double minH = std::numeric_limits<double>::max();
    double maxH = std::numeric_limits<double>::lowest();
    double minV = std::numeric_limits<double>::max();
    double maxV = std::numeric_limits<double>::lowest();
    int hits = 0;

    int ijk[3] = {};
    ijk[normalAxis] = fixed;
    for (int v = v0; v <= v1; ++v) {
        ijk[verticalAxis] = v;
        for (int h = h0; h <= h1; ++h) {
            ijk[horizontalAxis] = h;
            const double hu = m_imageData->GetScalarComponentAsDouble(ijk[0], ijk[1], ijk[2], 0);
            if (!std::isfinite(hu) || hu <= kAirThresholdHu)
                continue;

            const double px = origin[horizontalAxis] + h * spacing[horizontalAxis];
            const double py = origin[verticalAxis] + v * spacing[verticalAxis];
            minH = std::min(minH, px);
            maxH = std::max(maxH, px);
            minV = std::min(minV, py);
            maxV = std::max(maxV, py);
            ++hits;
        }
    }

    if (hits < 20 || minH >= maxH || minV >= maxV)
        return;

    const double width = std::max(40.0, maxH - minH);
    const double height = std::max(40.0, maxV - minV);
    const double aspect = m_vtkWidget && m_vtkWidget->height() > 0
        ? static_cast<double>(m_vtkWidget->width()) / static_cast<double>(m_vtkWidget->height())
        : 1.0;
    const double parallelScale =
        std::max(height * 0.5, width / (2.0 * std::max(0.25, aspect))) * 1.16;

    double oldFocal[3] = {};
    double oldPosition[3] = {};
    double direction[3] = {};
    camera->GetFocalPoint(oldFocal);
    camera->GetPosition(oldPosition);
    camera->GetDirectionOfProjection(direction);

    const double dx = oldPosition[0] - oldFocal[0];
    const double dy = oldPosition[1] - oldFocal[1];
    const double dz = oldPosition[2] - oldFocal[2];
    const double distance = std::max(1.0, std::sqrt(dx * dx + dy * dy + dz * dz));

    double focal[3] = {oldFocal[0], oldFocal[1], oldFocal[2]};
    focal[horizontalAxis] = (minH + maxH) * 0.5;
    focal[verticalAxis] = (minV + maxV) * 0.5;
    focal[normalAxis] = origin[normalAxis] + fixed * spacing[normalAxis];

    const double position[3] = {
        focal[0] - direction[0] * distance,
        focal[1] - direction[1] * distance,
        focal[2] - direction[2] * distance
    };
    camera->SetFocalPoint(focal);
    camera->SetPosition(position);
    camera->SetParallelScale(parallelScale);
    m_viewer->GetRenderer()->ResetCameraClippingRange();
}

// ─────────────────────────────────────────────────────────────────────────────
bool MPRView::pickPhysicalPoint(const QPoint& position,
                                std::array<double, 3>& point) const
{
    if (!m_imageData || !m_viewer || !m_viewer->GetRenderer() || !m_vtkWidget) {
        return false;
    }

    auto picker = vtkSmartPointer<vtkCellPicker>::New();
    picker->SetTolerance(0.003);

    const double dpr = m_vtkWidget->devicePixelRatioF();
    const int x = static_cast<int>(std::lround(position.x() * dpr));
    const int y = static_cast<int>(std::lround((m_vtkWidget->height() - position.y()) * dpr));
    if (picker->Pick(x, y, 0.0, m_viewer->GetRenderer()) == 0) {
        return false;
    }

    double picked[3] = {};
    picker->GetPickPosition(picked);
    point = {picked[0], picked[1], picked[2]};

    if (auto* cursor = m_viewer->GetResliceCursor()) {
        double center[3] = {};
        cursor->GetCenter(center);
        point[sliceAxis()] = center[sliceAxis()];
    }
    return true;
}

void MPRView::refreshSliceLabel()
{
    m_sliceLabel->setText(
        QString("Slice %1 / %2")
            .arg(currentSlice())
            .arg(sliceMax()));
}

// ─────────────────────────────────────────────────────────────────────────────
void MPRView::setLassoMode(bool active, bool addMode)
{
    m_lassoActive  = active;
    m_lassoAddMode = addMode;

    // Cancel any in-progress stroke when the mode changes
    m_lassoDrawing = false;
    m_lassoPoints.clear();
    if (m_lassoCanvas) {
        static_cast<LassoCanvas*>(m_lassoCanvas)->clearPoints();
        m_lassoCanvas->setVisible(false);
    }

    const Qt::CursorShape cur = active ? Qt::CrossCursor : Qt::ArrowCursor;
    setCursor(cur);
    if (m_vtkWidget) m_vtkWidget->setCursor(cur);
}

// ─────────────────────────────────────────────────────────────────────────────
vtkRenderer* MPRView::renderer() const
{
    return m_viewer ? m_viewer->GetRenderer() : nullptr;
}
