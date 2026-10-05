#include "Mesh3DView.h"
#include "CranioPalette.h"

#include <QEvent>
#include <QColor>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVTKOpenGLNativeWidget.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <vtkActor.h>
#include <vtkActor2D.h>
#include <vtkBillboardTextActor3D.h>
#include <vtkTextProperty.h>
#include <vtkArrowSource.h>
#include <vtkBoundingBox.h>
#include <vtkCallbackCommand.h>
#include <vtkMath.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkCellPicker.h>
#include <vtkCoordinate.h>
#include <vtkCubeSource.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkInteractorStyle.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkMatrix4x4.h>
#include <vtkObjectFactory.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkPolyDataMapper2D.h>
#include <vtkProperty.h>
#include <vtkProperty2D.h>
#include <vtkRegularPolygonSource.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkSmartPointer.h>
#include <vtkSphereSource.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>
#include <vtkTubeFilter.h>

// ─────────────────────────────────────────────────────────────────────────────
// Custom interactor style
//
// Extends TrackballCamera so that BOTH left-click AND right-click rotate the
// scene.  The middle button still pans; the scroll wheel still zooms.
// This gives medical users the natural "right-click = rotate" feel.
// ─────────────────────────────────────────────────────────────────────────────
class vtkInteractorStyleRotateRight : public vtkInteractorStyleTrackballCamera
{
public:
    static vtkInteractorStyleRotateRight* New();
    vtkTypeMacro(vtkInteractorStyleRotateRight, vtkInteractorStyleTrackballCamera);

    void OnRightButtonDown() override
    {
        this->FindPokedRenderer(
            this->Interactor->GetEventPosition()[0],
            this->Interactor->GetEventPosition()[1]);
        if (!this->CurrentRenderer) return;
        this->GrabFocus(static_cast<vtkCommand*>(this->EventCallbackCommand));
        this->StartRotate();
    }

    void OnRightButtonUp() override
    {
        if (this->State == VTKIS_ROTATE) {
            this->EndRotate();
            if (this->Interactor) this->ReleaseFocus();
        } else {
            Superclass::OnRightButtonUp();
        }
    }
};
vtkStandardNewMacro(vtkInteractorStyleRotateRight);

// ─────────────────────────────────────────────────────────────────────────────
// Default per-label colours
// ─────────────────────────────────────────────────────────────────────────────
static void meshColor(int label, double rgb[3])
{
    auto assign = [rgb](const QColor& color) {
        rgb[0] = color.redF();
        rgb[1] = color.greenF();
        rgb[2] = color.blueF();
    };

    switch (label) {
        case 1: assign(CranioPalette::bone()); return;
        case 2: assign(CranioPalette::softTissue()); return;
        case 3: assign(CranioPalette::dental()); return;
        case 4: assign(CranioPalette::guide()); return;
        case 5: assign(CranioPalette::maxilla()); return;
        case 6: assign(CranioPalette::mandible()); return;
        case 8: assign(CranioPalette::dental()); return;
        default: assign(CranioPalette::fallback()); return;
    }

    switch (label) {
        case 1: rgb[0] = 0.92; rgb[1] = 0.82; rgb[2] = 0.72; return; // hueso
        case 2: rgb[0] = 0.30; rgb[1] = 0.72; rgb[2] = 1.00; return; // blando
        case 3: rgb[0] = 1.00; rgb[1] = 0.95; rgb[2] = 0.35; return; // dientes superiores
        case 4: rgb[0] = 0.25; rgb[1] = 1.00; rgb[2] = 0.45; return; // via aerea
        case 5: rgb[0] = 0.98; rgb[1] = 0.72; rgb[2] = 0.72; return; // maxilar  — rosado
        case 6: rgb[0] = 0.62; rgb[1] = 0.80; rgb[2] = 0.98; return; // mandíbula — azul
        case 8: rgb[0] = 1.00; rgb[1] = 0.82; rgb[2] = 0.18; return; // dientes inferiores
        default: rgb[0] = 0.85; rgb[1] = 0.35; rgb[2] = 0.85; return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Ray-casting even-odd point-in-polygon test (2-D)
// ─────────────────────────────────────────────────────────────────────────────
static bool lassoContains(double px, double py, const QVector<QPointF>& poly)
{
    bool inside = false;
    const int n = poly.size();
    for (int i = 0, j = n - 1; i < n; j = i++) {
        const double xi = poly[i].x(), yi = poly[i].y();
        const double xj = poly[j].x(), yj = poly[j].y();
        if (((yi > py) != (yj > py)) &&
            (px < (xj - xi) * (py - yi) / (yj - yi) + xi))
            inside = !inside;
    }
    return inside;
}

// ─────────────────────────────────────────────────────────────────────────────
// LassoCanvas3D
//
// Transparent overlay that paints the in-progress lasso polygon in red.
// WA_TransparentForMouseEvents lets all events fall through to the eventFilter.
// ─────────────────────────────────────────────────────────────────────────────
class LassoCanvas3D : public QWidget
{
public:
    explicit LassoCanvas3D(QWidget* parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_TranslucentBackground);
        setStyleSheet("background: transparent;");
    }

    void setPoints(const QVector<QPointF>& pts)  { m_points = pts;  update(); }
    void clearPoints()                           { m_points.clear(); update(); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        if (m_points.size() < 2) return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QPen pen(QColor(230, 55, 55, 220), 1.5, Qt::DashLine);
        p.setPen(pen);
        for (int i = 0; i + 1 < m_points.size(); ++i)
            p.drawLine(m_points[i], m_points[i + 1]);
        if (m_points.size() > 2)
            p.drawLine(m_points.last(), m_points.first());
        p.setPen(QPen(QColor(230, 55, 55, 220), 4.0, Qt::SolidLine));
        p.drawPoint(m_points.first());
    }

private:
    QVector<QPointF> m_points;
};

// ─────────────────────────────────────────────────────────────────────────────
Mesh3DView::Mesh3DView(QWidget* parent)
    : QWidget(parent)
{
    buildLayout();
    initRenderer();
}

Mesh3DView::~Mesh3DView() = default;

void Mesh3DView::setTitle(const QString& title)
{
    if (m_titleLabel) m_titleLabel->setText(title);
}

// ─────────────────────────────────────────────────────────────────────────────
void Mesh3DView::buildLayout()
{
    setStyleSheet(
        "Mesh3DView { background-color:#1c1d20; border:1px solid #292b30; border-radius:4px; }"
        "QVTKOpenGLNativeWidget { border:0; }");

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    m_titleLabel = new QLabel("3D", this);
    m_titleLabel->setAlignment(Qt::AlignCenter);
    m_titleLabel->setFixedHeight(24);
    m_titleLabel->setStyleSheet(
        "color:#d1d1d6; font-weight:600; font-size:11px;"
        "background-color:#202226; border:0; border-bottom:1px solid #292b30;");
    outer->addWidget(m_titleLabel);

    m_vtkWidget = new QVTKOpenGLNativeWidget(this);
    m_vtkWidget->setMinimumSize(256, 256);
    m_vtkWidget->setFocusPolicy(Qt::StrongFocus);
    m_vtkWidget->setStyleSheet("background-color:#1c1d20;");
    m_vtkWidget->installEventFilter(this);
    outer->addWidget(m_vtkWidget, 1);

    // Transparent lasso overlay on top of the VTK widget
    m_lassoCanvas = new LassoCanvas3D(m_vtkWidget);
    m_lassoCanvas->setGeometry(m_vtkWidget->rect());
    m_lassoCanvas->hide();
    m_lassoCanvas->raise();

    m_clickFeedback = new QWidget(m_vtkWidget);
    m_clickFeedback->setObjectName("clickFeedback");
    m_clickFeedback->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_clickFeedback->setFixedSize(18, 18);
    m_clickFeedback->setStyleSheet(
        "background-color:rgba(10,132,255,70);"
        "border:2px solid #0a84ff; border-radius:9px;");
    m_clickFeedback->hide();

    m_gridToggleButton = new QToolButton(m_vtkWidget);
    m_gridToggleButton->setCheckable(true);
    m_gridToggleButton->setChecked(true);
    m_gridToggleButton->setText("#");
    m_gridToggleButton->setToolTip(tr("Mostrar/ocultar cuadricula 3D"));
    m_gridToggleButton->setFixedSize(28, 28);
    m_gridToggleButton->setStyleSheet(
        "QToolButton {"
        " background-color:rgba(32,34,38,225);"
        " color:#a7aab2;"
        " border:1px solid rgba(255,255,255,38);"
        " border-radius:6px;"
        " font-weight:bold;"
        "}"
        "QToolButton:checked {"
        " background-color:#0a84ff;"
        " color:#ffffff;"
        " border-color:#0a84ff;"
        "}"
        "QToolButton:hover { background-color:rgba(58,58,60,240); }");
    connect(m_gridToggleButton, &QToolButton::toggled,
            this, &Mesh3DView::setGridVisible);
    positionOverlayControls();
}

// ─────────────────────────────────────────────────────────────────────────────
void Mesh3DView::initRenderer()
{
    m_renderWindow = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
    m_renderWindow->SetNumberOfLayers(4);
    m_renderWindow->SetAlphaBitPlanes(1);
    m_renderWindow->SetMultiSamples(0);

    m_backgroundRenderer = vtkSmartPointer<vtkRenderer>::New();
    m_backgroundRenderer->SetLayer(0);
    m_backgroundRenderer->SetBackground(0.102, 0.106, 0.118);
    m_backgroundRenderer->InteractiveOff();

    m_renderer     = vtkSmartPointer<vtkRenderer>::New();
    m_renderer->SetLayer(1);
    m_renderer->SetBackground(0.12, 0.12, 0.14);
    m_renderer->SetBackgroundAlpha(0.0);

    m_foregroundRenderer = vtkSmartPointer<vtkRenderer>::New();
    m_foregroundRenderer->SetLayer(2);
    m_foregroundRenderer->SetActiveCamera(m_renderer->GetActiveCamera());
    m_foregroundRenderer->InteractiveOff();
    m_foregroundRenderer->PreserveDepthBufferOff();
    m_annotationRenderer = vtkSmartPointer<vtkRenderer>::New();
    m_annotationRenderer->SetLayer(3);
    m_annotationRenderer->SetActiveCamera(m_renderer->GetActiveCamera());
    m_annotationRenderer->InteractiveOff();
    for (auto* renderer : {m_renderer.GetPointer(), m_foregroundRenderer.GetPointer()}) {
        renderer->SetUseDepthPeeling(true);
        renderer->SetMaximumNumberOfPeels(100);
        renderer->SetOcclusionRatio(0.1);
    }

    m_vtkWidget->setRenderWindow(m_renderWindow);
    m_renderWindow->AddRenderer(m_backgroundRenderer);
    m_renderWindow->AddRenderer(m_renderer);
    m_renderWindow->AddRenderer(m_foregroundRenderer);
    m_renderWindow->AddRenderer(m_annotationRenderer);

    // Native camera interactions must also include foreground-only structures.
    auto clippingObserver = vtkSmartPointer<vtkCallbackCommand>::New();
    clippingObserver->SetClientData(this);
    clippingObserver->SetCallback([](vtkObject*, unsigned long, void* data, void*) {
        auto* view = static_cast<Mesh3DView*>(data);
        double bounds[6];
        view->sceneBounds(bounds);
        if (vtkBoundingBox::IsValid(bounds))
            view->m_renderer->ResetCameraClippingRange(bounds);
    });
    m_renderWindow->AddObserver(vtkCommand::StartEvent, clippingObserver);

    m_gridPolyData = vtkSmartPointer<vtkPolyData>::New();
    auto gridMapper = vtkSmartPointer<vtkPolyDataMapper2D>::New();
    auto gridCoord = vtkSmartPointer<vtkCoordinate>::New();
    gridCoord->SetCoordinateSystemToDisplay();
    gridMapper->SetTransformCoordinate(gridCoord);
    gridMapper->SetInputData(m_gridPolyData);

    m_gridActor = vtkSmartPointer<vtkActor2D>::New();
    m_gridActor->SetMapper(gridMapper);
    m_gridActor->GetProperty()->SetColor(0.38, 0.39, 0.43);
    m_gridActor->GetProperty()->SetOpacity(0.18);
    m_gridActor->GetProperty()->SetLineWidth(1.0);
    m_gridActor->SetVisibility(m_gridVisible ? 1 : 0);
    m_backgroundRenderer->AddActor2D(m_gridActor);
    updateGridGeometry();

    // Right-click = rotate (same as left-click); scroll = zoom; middle = pan
    auto style = vtkSmartPointer<vtkInteractorStyleRotateRight>::New();
    if (auto* interactor = m_renderWindow->GetInteractor())
        interactor->SetInteractorStyle(style);
}

// ─────────────────────────────────────────────────────────────────────────────
bool Mesh3DView::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != m_vtkWidget) return QWidget::eventFilter(watched, event);

    // Keep canvas sized to the vtk widget
    if (event->type() == QEvent::Resize) {
        positionOverlayControls();
        updateGridGeometry();
        render();
    }

    if (event->type() == QEvent::MouseButtonDblClick) {
        emit fullScreenToggleRequested(this);
        return true;
    }

    if (event->type() == QEvent::MouseButtonPress) {
        m_vtkWidget->setFocus(Qt::MouseFocusReason);
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::LeftButton &&
            (m_pointPickActive || m_pointEditActive || m_planeDragActive ||
             m_brushActive || m_lassoActive)) {
            showClickFeedback(mouseEvent->position());
        }
    }

    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Space && key->modifiers() == Qt::NoModifier) {
            emit fullScreenToggleRequested(this);
            return true;
        }
    }

    if (m_pointEditActive && handlePointEditEvent(event))
        return true;
    if (m_planeDragActive && handlePlaneDragEvent(event))
        return true;
    if (m_brushActive && handleBrushEvent(event))
        return true;

    if (m_pointPickActive && event->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton && m_renderer) {
            const double dpr = m_vtkWidget ? m_vtkWidget->devicePixelRatioF() : 1.0;
            auto picker = vtkSmartPointer<vtkCellPicker>::New();
            picker->SetTolerance(0.0025);
            const int px = static_cast<int>(std::lround(me->position().x() * dpr));
            const int py = static_cast<int>(std::lround((m_vtkWidget->height() - me->position().y()) * dpr));
            // Pick the visible foreground first, matching its display order.
            if (picker->Pick(px, py, 0.0, m_foregroundRenderer) ||
                picker->Pick(px, py, 0.0, m_renderer)) {
                vtkActor* actor = picker->GetActor();
                auto it = m_actorLabels.find(actor);
                if (it != m_actorLabels.end()) {
                    double p[3] = {};
                    picker->GetPickPosition(p);
                    emit pointPicked(it->second, p[0], p[1], p[2]);
                }
            }
            return true;
        }
    }

    if (!m_lassoActive) return QWidget::eventFilter(watched, event);

    // ── Lasso erase mouse events (LEFT button only) ───────────────────────
    // Right button always falls through to VTK so the camera can rotate.
    if (event->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
            m_lassoDrawing = true;
            m_lassoPoints.clear();
            m_lassoPoints.append(me->pos());
            if (m_lassoCanvas) {
                static_cast<LassoCanvas3D*>(m_lassoCanvas)->setPoints(m_lassoPoints);
                m_lassoCanvas->show();
                m_lassoCanvas->raise();
            }
            return true;   // prevent VTK from consuming the left press
        }
        // Right button → let VTK handle it (rotation via custom style)
    }

    if (event->type() == QEvent::MouseMove && m_lassoDrawing) {
        auto* me = static_cast<QMouseEvent*>(event);
        m_lassoPoints.append(me->pos());
        if (m_lassoCanvas)
            static_cast<LassoCanvas3D*>(m_lassoCanvas)->setPoints(m_lassoPoints);
        return true;
    }

    if (event->type() == QEvent::MouseButtonRelease && m_lassoDrawing) {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
            m_lassoDrawing = false;
            if (m_lassoCanvas) {
                static_cast<LassoCanvas3D*>(m_lassoCanvas)->clearPoints();
                m_lassoCanvas->hide();
            }
            if (m_lassoPoints.size() >= 3)
                applyLassoErase();
            m_lassoPoints.clear();
            return true;
        }
    }

    return QWidget::eventFilter(watched, event);
}

// ─────────────────────────────────────────────────────────────────────────────
// pushUndoState — deep-copies current polydata into the undo stack.
// Called at the start of applyLassoErase(), before any modification.
// ─────────────────────────────────────────────────────────────────────────────
void Mesh3DView::pushUndoState()
{
    MeshSnapshot snap;
    for (const auto& [label, pd] : m_meshPolyData) {
        if (!pd) continue;
        auto copy = vtkSmartPointer<vtkPolyData>::New();
        copy->DeepCopy(pd);
        snap[label] = copy;
    }
    if (m_undoStack.size() >= kMaxUndo)
        m_undoStack.removeFirst();
    m_undoStack.push(snap);
}

// ─────────────────────────────────────────────────────────────────────────────
// applyLassoErase — removes triangles whose centroid projects inside the lasso.
// ─────────────────────────────────────────────────────────────────────────────
void Mesh3DView::applyLassoErase()
{
    if (!m_renderer || m_lassoPoints.size() < 3) return;

    // Save undo state BEFORE any modification
    pushUndoState();

    const double dpr = m_vtkWidget->devicePixelRatioF();
    const double h   = static_cast<double>(m_vtkWidget->height());

    auto coord = vtkSmartPointer<vtkCoordinate>::New();
    coord->SetCoordinateSystemToWorld();

    bool anyModified = false;

    for (auto& [label, polyData] : m_meshPolyData) {
        if (!polyData || polyData->GetNumberOfCells() == 0) continue;

        auto newPolys = vtkSmartPointer<vtkCellArray>::New();
        bool modified = false;

        const vtkIdType nCells = polyData->GetNumberOfCells();
        for (vtkIdType cellId = 0; cellId < nCells; ++cellId) {
            vtkIdType        npts = 0;
            const vtkIdType* pts  = nullptr;
            polyData->GetCellPoints(cellId, npts, pts);

            // Centroid in world space
            double cx = 0.0, cy = 0.0, cz = 0.0;
            for (vtkIdType k = 0; k < npts; ++k) {
                double p[3];
                polyData->GetPoint(pts[k], p);
                cx += p[0]; cy += p[1]; cz += p[2];
            }
            if (npts > 0) { cx /= npts; cy /= npts; cz /= npts; }

            // Project world → VTK display (physical px, y=0 at bottom)
            coord->SetValue(cx, cy, cz);
            int* disp = coord->GetComputedDisplayValue(m_renderer);

            // Convert to Qt widget coords (logical px, y=0 at top)
            const double qt_x = disp[0] / dpr;
            const double qt_y = h - disp[1] / dpr;

            if (lassoContains(qt_x, qt_y, m_lassoPoints)) {
                modified = true;
                continue;   // erase this cell
            }
            newPolys->InsertNextCell(npts, pts);
        }

        if (!modified) continue;

        auto newPD = vtkSmartPointer<vtkPolyData>::New();
        newPD->SetPoints(polyData->GetPoints());
        newPD->SetPolys(newPolys);
        polyData = newPD;

        auto it = m_meshActors.find(label);
        if (it != m_meshActors.end()) {
            auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
            mapper->SetInputData(newPD);
            mapper->ScalarVisibilityOff();
            it->second->SetMapper(mapper);
        }
        emit meshEdited(label, newPD);
        anyModified = true;
    }

    if (anyModified)
        render();
    else
        m_undoStack.pop();   // nothing changed → discard the snapshot we just pushed
}

// ─────────────────────────────────────────────────────────────────────────────
// undo — restores the most recent mesh snapshot.
// ─────────────────────────────────────────────────────────────────────────────
void Mesh3DView::undo()
{
    if (m_undoStack.isEmpty()) return;

    const MeshSnapshot snap = m_undoStack.pop();
    for (const auto& [label, pd] : snap) {
        m_meshPolyData[label] = pd;
        auto it = m_meshActors.find(label);
        if (it != m_meshActors.end()) {
            auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
            mapper->SetInputData(pd);
            mapper->ScalarVisibilityOff();
            it->second->SetMapper(mapper);
        }
        emit meshEdited(label, pd);
    }
    updateGridGeometry();
    render();
}

// ─────────────────────────────────────────────────────────────────────────────
void Mesh3DView::setLassoEraseMode(bool active)
{
    m_lassoActive  = active;
    m_lassoDrawing = false;
    m_lassoPoints.clear();
    if (m_lassoCanvas) {
        static_cast<LassoCanvas3D*>(m_lassoCanvas)->clearPoints();
        m_lassoCanvas->setVisible(false);
    }
    updateInteractionCursor();
}

void Mesh3DView::setFullScreenActive(bool active)
{
    if (m_titleLabel) {
        m_titleLabel->setStyleSheet(
            active
                ? "color:#ffffff; font-weight:700; font-size:11px;"
                  "background-color:#0a84ff; border-radius:10px;"
                : "color:#f5f5f7; font-weight:700; font-size:11px;"
                  "background-color:#2c2c2e; border-radius:10px;");
    }
    updateInteractionCursor();
    if (active && m_vtkWidget)
        m_vtkWidget->setFocus(Qt::OtherFocusReason);
}

void Mesh3DView::updateInteractionCursor()
{
    Qt::CursorShape cursor = Qt::ArrowCursor;
    if (m_planeDragActive)
        cursor = Qt::SizeAllCursor;
    else if (m_brushActive)
        cursor = Qt::PointingHandCursor;
    else if (m_pointPickActive || m_pointEditActive || m_lassoActive)
        cursor = Qt::CrossCursor;

    setCursor(cursor);
    if (m_vtkWidget)
        m_vtkWidget->setCursor(cursor);
}

void Mesh3DView::showClickFeedback(const QPointF& position)
{
    if (!m_clickFeedback || !m_vtkWidget)
        return;

    const int x = qRound(position.x()) - m_clickFeedback->width() / 2;
    const int y = qRound(position.y()) - m_clickFeedback->height() / 2;
    m_clickFeedback->move(x, y);
    m_clickFeedback->show();
    m_clickFeedback->raise();

    const int generation = ++m_clickFeedbackGeneration;
    QTimer::singleShot(450, this, [this, generation] {
        if (m_clickFeedback && generation == m_clickFeedbackGeneration)
            m_clickFeedback->hide();
    });
}

// ─────────────────────────────────────────────────────────────────────────────
void Mesh3DView::setGridVisible(bool visible)
{
    m_gridVisible = visible;
    if (m_gridToggleButton && m_gridToggleButton->isChecked() != visible) {
        const QSignalBlocker blocker(m_gridToggleButton);
        m_gridToggleButton->setChecked(visible);
    }
    if (m_gridActor)
        m_gridActor->SetVisibility(visible ? 1 : 0);
    render();
}

void Mesh3DView::positionOverlayControls()
{
    if (!m_vtkWidget) return;

    if (m_lassoCanvas)
        m_lassoCanvas->setGeometry(m_vtkWidget->rect());

    if (m_lassoCanvas)
        m_lassoCanvas->raise();

    if (m_clickFeedback && m_clickFeedback->isVisible())
        m_clickFeedback->raise();

    if (m_gridToggleButton) {
        const int margin = 8;
        const int size = m_gridToggleButton->width() > 0 ? m_gridToggleButton->width() : 30;
        m_gridToggleButton->move(std::max(margin, m_vtkWidget->width() - size - margin), margin);
        m_gridToggleButton->raise();
    }
}

void Mesh3DView::updateGridGeometry()
{
    if (!m_gridPolyData)
        return;

    const double dpr = m_vtkWidget ? m_vtkWidget->devicePixelRatioF() : 1.0;
    int width = m_vtkWidget ? static_cast<int>(std::round(m_vtkWidget->width() * dpr)) : 640;
    int height = m_vtkWidget ? static_cast<int>(std::round(m_vtkWidget->height() * dpr)) : 480;

    width = std::max(width, 2);
    height = std::max(height, 2);
    const double step = width > 1300 || height > 900 ? 40.0 : 28.0;
    const double x0 = -step;
    const double y0 = -step;
    const double x1 = width + step;
    const double y1 = height + step;

    auto points = vtkSmartPointer<vtkPoints>::New();
    auto lines = vtkSmartPointer<vtkCellArray>::New();

    auto addLine = [&](double x0, double y0, double x1, double y1) {
        const vtkIdType start = points->GetNumberOfPoints();
        points->InsertNextPoint(x0, y0, 0.0);
        points->InsertNextPoint(x1, y1, 0.0);
        lines->InsertNextCell(2);
        lines->InsertCellPoint(start);
        lines->InsertCellPoint(start + 1);
    };

    for (double x = x0; x <= x1 + 1.0; x += step)
        addLine(x, y0, x, y1);
    for (double y = y0; y <= y1 + 1.0; y += step)
        addLine(x0, y, x1, y);

    m_gridPolyData->SetPoints(points);
    m_gridPolyData->SetLines(lines);
    m_gridPolyData->Modified();
}

void Mesh3DView::addMesh(int label, vtkSmartPointer<vtkPolyData> mesh,
                         const QString&)
{
    if (!mesh || !m_renderer) return;

    const bool hadMeshesBeforeAdd = !m_meshActors.empty();

    auto it = m_meshActors.find(label);
    if (it != m_meshActors.end()) {
        m_renderer->RemoveActor(it->second);
        m_foregroundRenderer->RemoveActor(it->second);
        m_actorLabels.erase(it->second);
        m_meshActors.erase(it);
    }
    m_meshPolyData[label] = mesh;

    auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    mapper->SetInputData(mesh);
    mapper->ScalarVisibilityOff();

    auto actor = vtkSmartPointer<vtkActor>::New();
    actor->SetMapper(mapper);

    double color[3] = {};
    meshColor(label, color);
    actor->GetProperty()->SetColor(color);
    actor->GetProperty()->SetSpecular(0.18);
    actor->GetProperty()->SetSpecularPower(16.0);
    actor->GetProperty()->SetInterpolationToPhong();

    const auto display = m_displayOptions.find(label);
    const bool onTop = display != m_displayOptions.end() && display->second.alwaysOnTop;
    if (display != m_displayOptions.end())
        actor->GetProperty()->SetOpacity(display->second.opacity);
    actor->SetPickable(actor->GetProperty()->GetOpacity() > 0.0);
    (onTop ? m_foregroundRenderer : m_renderer)->AddActor(actor);
    m_meshActors[label] = actor;
    m_actorLabels[actor] = label;
    updateGridGeometry();

    if (!hadMeshesBeforeAdd && !m_preserveCameraOnNextMesh) {
        // The first model is framed in the chosen standard view (frontal unless changed).
        if (m_standardViewIndex >= 0)
            applyStandardView(m_standardViewIndex);
        else
            resetCamera();
    } else {
        double bounds[6];
        sceneBounds(bounds);
        m_renderer->ResetCameraClippingRange(bounds);
        render();
    }
    m_preserveCameraOnNextMesh = false;
}

// ─────────────────────────────────────────────────────────────────────────────
void Mesh3DView::clearMeshes(bool preserveCamera)
{
    if (!m_renderer) return;
    m_preserveCameraOnNextMesh = preserveCamera && (!m_meshActors.empty() || m_preserveCameraOnNextMesh);
    for (auto& entry : m_meshActors) {
        m_renderer->RemoveActor(entry.second);
        m_foregroundRenderer->RemoveActor(entry.second);
    }
    m_meshActors.clear();
    m_meshPolyData.clear();
    m_actorLabels.clear();
    clearPointMarkers();
    m_undoStack.clear();
    updateGridGeometry();
    render();
}

// ─────────────────────────────────────────────────────────────────────────────
void Mesh3DView::setMeshColor(int label, const QColor& color)
{
    auto it = m_meshActors.find(label);
    if (it == m_meshActors.end()) return;
    it->second->GetProperty()->SetColor(color.redF(), color.greenF(), color.blueF());
    render();
}

// ─────────────────────────────────────────────────────────────────────────────
void Mesh3DView::setMeshOpacity(int label, double opacity)
{
    auto it = m_meshActors.find(label);
    if (it == m_meshActors.end()) return;
    opacity = std::clamp(opacity, 0.0, 1.0);
    if (const auto display = m_displayOptions.find(label); display != m_displayOptions.end())
        opacity = display->second.opacity;
    it->second->GetProperty()->SetOpacity(opacity);
    it->second->SetPickable(opacity > 0.0);
    render();
}

void Mesh3DView::setMeshDisplayOptions(int label, double opacity, bool alwaysOnTop)
{
    if (!std::isfinite(opacity)) return;
    if (opacity < 0.0) {
        m_displayOptions.erase(label);
        opacity = 1.0;
        alwaysOnTop = false;
    } else {
        opacity = std::clamp(opacity, 0.0, 1.0);
        m_displayOptions[label] = {opacity, alwaysOnTop};
    }
    const auto it = m_meshActors.find(label);
    if (it == m_meshActors.end()) return;
    auto* actor = it->second.GetPointer();
    auto* target = alwaysOnTop ? m_foregroundRenderer.GetPointer() : m_renderer.GetPointer();
    if (actor->GetProperty()->GetOpacity() == opacity && target->HasViewProp(actor)) return;
    actor->GetProperty()->SetOpacity(opacity);
    actor->SetPickable(opacity > 0.0);
    m_renderer->RemoveActor(actor);
    m_foregroundRenderer->RemoveActor(actor);
    target->AddActor(actor);
    render();
}

vtkSmartPointer<vtkPolyData> Mesh3DView::meshData(int label) const
{
    auto it = m_meshPolyData.find(label);
    return it == m_meshPolyData.end() ? nullptr : it->second;
}

void Mesh3DView::setPointPickMode(bool active)
{
    m_pointPickActive = active;
    updateInteractionCursor();
}

void Mesh3DView::addPointMarker(double x, double y, double z, const QColor& color)
{
    if (!m_renderer) return;

    auto sphere = vtkSmartPointer<vtkSphereSource>::New();
    sphere->SetCenter(x, y, z);
    sphere->SetRadius(1.4);
    sphere->SetThetaResolution(16);
    sphere->SetPhiResolution(16);
    sphere->Update();

    auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    mapper->SetInputConnection(sphere->GetOutputPort());

    auto actor = vtkSmartPointer<vtkActor>::New();
    actor->SetMapper(mapper);
    actor->GetProperty()->SetColor(color.redF(), color.greenF(), color.blueF());
    actor->GetProperty()->SetAmbient(0.35);
    actor->GetProperty()->SetDiffuse(0.9);
    actor->GetProperty()->SetSpecular(0.3);

    m_annotationRenderer->AddActor(actor);
    m_pointMarkerActors.push_back(actor);
    render();
}

void Mesh3DView::clearPointMarkers()
{
    if (!m_renderer) {
        m_pointMarkerActors.clear();
        return;
    }
    for (const auto& actor : m_pointMarkerActors)
        m_annotationRenderer->RemoveActor(actor);
    m_pointMarkerActors.clear();
    render();
}

void Mesh3DView::removeMesh(int label)
{
    auto it = m_meshActors.find(label);
    if (it != m_meshActors.end()) {
        if (m_renderer) m_renderer->RemoveActor(it->second);
        if (m_foregroundRenderer) m_foregroundRenderer->RemoveActor(it->second);
        m_actorLabels.erase(it->second);
        m_meshActors.erase(it);
    }
    m_meshPolyData.erase(label);
    updateGridGeometry();
    render();
}

// ─────────────────────────────────────────────────────────────────────────────
void Mesh3DView::setMeshVisible(int label, bool visible)
{
    auto it = m_meshActors.find(label);
    if (it == m_meshActors.end()) return;
    it->second->SetVisibility(visible ? 1 : 0);
    render();
}

void Mesh3DView::setMeshScalarColoring(int label, bool enabled)
{
    auto it = m_meshActors.find(label);
    if (it == m_meshActors.end()) return;
    if (auto* mapper = vtkPolyDataMapper::SafeDownCast(it->second->GetMapper())) {
        mapper->SetScalarVisibility(enabled ? 1 : 0);
        mapper->SetScalarModeToDefault(); // point scalars, else cell scalars (composite parts)
        mapper->SetColorModeToDirectScalars();
    }
    render();
}

void Mesh3DView::setMeshPickable(int label, bool pickable)
{
    auto it = m_meshActors.find(label);
    if (it == m_meshActors.end()) return;
    it->second->SetPickable(pickable ? 1 : 0);
}

// ─────────────────────────────────────────────────────────────────────────────
void Mesh3DView::resetCamera()
{
    if (!m_renderer) return;
    m_standardViewIndex = -1;
    double bounds[6];
    sceneBounds(bounds);
    m_renderer->ResetCamera(bounds);
    if (auto* camera = m_renderer->GetActiveCamera()) {
        camera->ParallelProjectionOff();
        camera->Elevation(15.0);
        camera->Azimuth(-35.0);
        camera->Dolly(1.2);
        m_renderer->ResetCameraClippingRange(bounds);
    }
    render();
}

void Mesh3DView::cycleStandardView()
{
    m_standardViewIndex = (m_standardViewIndex + 1) % 5;
    applyStandardView(m_standardViewIndex);
}

void Mesh3DView::setStandardView(int viewIndex)
{
    m_standardViewIndex = ((viewIndex % 5) + 5) % 5;
    applyStandardView(m_standardViewIndex);
}

void Mesh3DView::applyStandardView(int viewIndex)
{
    if (!m_renderer) return;
    auto* camera = m_renderer->GetActiveCamera();
    if (!camera) return;

    double bounds[6] = {};
    sceneBounds(bounds);
    if (!std::isfinite(bounds[0]) || bounds[0] > bounds[1] ||
        bounds[2] > bounds[3] || bounds[4] > bounds[5]) {
        render(); // nothing to frame yet: the first model added gets this view
        return;
    }

    const std::array<double, 3> center {
        (bounds[0] + bounds[1]) * 0.5,
        (bounds[2] + bounds[3]) * 0.5,
        (bounds[4] + bounds[5]) * 0.5
    };
    const double dx = std::max(1.0, bounds[1] - bounds[0]);
    const double dy = std::max(1.0, bounds[3] - bounds[2]);
    const double dz = std::max(1.0, bounds[5] - bounds[4]);
    const double diagonal = std::sqrt(dx * dx + dy * dy + dz * dz);
    const double distance = std::max(200.0, diagonal * 1.8);
    const double scale = std::max({dx, dy, dz}) * 0.62;

    std::array<double, 3> position = center;
    std::array<double, 3> viewUp {0.0, 0.0, 1.0};

    switch (viewIndex) {
        case 0: // frontal
            position[1] -= distance;
            break;
        case 1: // lateral izquierda
            position[0] -= distance;
            break;
        case 2: // lateral derecha
            position[0] += distance;
            break;
        case 3: // desde abajo
            position[2] -= distance;
            viewUp = {0.0, 1.0, 0.0};
            break;
        case 4: // desde arriba
            position[2] += distance;
            viewUp = {0.0, 1.0, 0.0};
            break;
        default:
            break;
    }

    camera->SetFocalPoint(center.data());
    camera->SetPosition(position.data());
    camera->SetViewUp(viewUp.data());
    camera->ParallelProjectionOn();
    camera->SetParallelScale(scale);
    m_renderer->ResetCameraClippingRange(bounds);
    render();
}

// ─────────────────────────────────────────────────────────────────────────────
void Mesh3DView::render()
{
    if (m_renderWindow) m_renderWindow->Render();
}

// ─────────────────────────────────────────────────────────────────────────────
// Editable points, overlays, plane drag
// ─────────────────────────────────────────────────────────────────────────────
namespace
{
constexpr double kClickTolerancePixels = 4.0;

// Qt widget position (logical, y down) → VTK display position (physical, y up).
void toDisplay(const QVTKOpenGLNativeWidget* widget, const QPointF& position, int& px, int& py)
{
    const double dpr = widget->devicePixelRatioF();
    px = static_cast<int>(std::lround(position.x() * dpr));
    py = static_cast<int>(std::lround((widget->height() - position.y()) * dpr));
}
} // namespace

bool Mesh3DView::RayPlaneIntersection(const std::array<double, 3>& rayStart, const std::array<double, 3>& rayEnd,
                                      const std::array<double, 3>& planeOrigin, const std::array<double, 3>& planeNormal,
                                      std::array<double, 3>& hit)
{
    const std::array<double, 3> d{rayEnd[0] - rayStart[0], rayEnd[1] - rayStart[1], rayEnd[2] - rayStart[2]};
    const double denom = planeNormal[0] * d[0] + planeNormal[1] * d[1] + planeNormal[2] * d[2];
    if (std::abs(denom) < 1e-12)
        return false;
    const double t = (planeNormal[0] * (planeOrigin[0] - rayStart[0]) +
                      planeNormal[1] * (planeOrigin[1] - rayStart[1]) +
                      planeNormal[2] * (planeOrigin[2] - rayStart[2])) / denom;
    hit = {rayStart[0] + t * d[0], rayStart[1] + t * d[1], rayStart[2] + t * d[2]};
    return true;
}

void Mesh3DView::setEditablePoints(int group, const std::vector<std::array<double, 3>>& points,
                                   const QColor& color, double radius)
{
    if (!m_annotationRenderer) return;
    m_editablePoints.erase(std::remove_if(m_editablePoints.begin(), m_editablePoints.end(),
                                          [this, group](const EditablePoint& p) {
                                              if (p.group != group) return false;
                                              m_annotationRenderer->RemoveActor(p.actor);
                                              return true;
                                          }),
                           m_editablePoints.end());
    for (size_t i = 0; i < points.size(); ++i) {
        auto source = vtkSmartPointer<vtkSphereSource>::New();
        source->SetCenter(points[i][0], points[i][1], points[i][2]);
        source->SetRadius(radius);
        source->SetThetaResolution(20);
        source->SetPhiResolution(20);
        auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
        mapper->SetInputConnection(source->GetOutputPort());
        auto actor = vtkSmartPointer<vtkActor>::New();
        actor->SetMapper(mapper);
        actor->GetProperty()->SetColor(color.redF(), color.greenF(), color.blueF());
        actor->GetProperty()->SetAmbient(0.35);
        actor->GetProperty()->SetDiffuse(0.9);
        actor->GetProperty()->SetSpecular(0.3);
        m_annotationRenderer->AddActor(actor);
        m_editablePoints.push_back({group, static_cast<int>(i), source, actor});
    }
    render();
}

void Mesh3DView::clearEditablePoints()
{
    if (m_annotationRenderer)
        for (const auto& p : m_editablePoints)
            m_annotationRenderer->RemoveActor(p.actor);
    m_editablePoints.clear();
    m_draggedPointGroup = m_draggedPointIndex = -1;
    render();
}

void Mesh3DView::setPointEditMode(bool active, int activeGroup)
{
    m_pointEditActive = active;
    m_pointEditGroup = active ? activeGroup : -1;
    m_draggedPointGroup = m_draggedPointIndex = -1;
    m_rightPressGroup = m_rightPressIndex = -1;
    updateInteractionCursor();
}

void Mesh3DView::setOverlayPolyline(int key, vtkSmartPointer<vtkPolyData> lines, const QColor& color, double lineWidth)
{
    if (!m_annotationRenderer) return;
    if (const auto it = m_overlayActors.find(key); it != m_overlayActors.end()) {
        m_annotationRenderer->RemoveActor(it->second);
        m_overlayActors.erase(it);
    }
    if (lines && lines->GetNumberOfPoints() > 0) {
        auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
        mapper->SetInputData(lines);
        mapper->ScalarVisibilityOff();
        auto actor = vtkSmartPointer<vtkActor>::New();
        actor->SetMapper(mapper);
        actor->GetProperty()->SetColor(color.redF(), color.greenF(), color.blueF());
        actor->GetProperty()->SetLineWidth(static_cast<float>(lineWidth));
        actor->GetProperty()->LightingOff();
        actor->SetPickable(0);
        m_annotationRenderer->AddActor(actor);
        m_overlayActors[key] = actor;
    }
    render();
}

void Mesh3DView::setOverlayLabels(int key, const std::vector<std::pair<std::array<double, 3>, QString>>& labels,
                                  const QColor& color)
{
    if (!m_annotationRenderer) return;
    if (const auto it = m_overlayLabels.find(key); it != m_overlayLabels.end()) {
        for (const auto& prop : it->second)
            m_annotationRenderer->RemoveViewProp(prop);
        m_overlayLabels.erase(it);
    }
    for (const auto& [position, text] : labels) {
        auto label = vtkSmartPointer<vtkBillboardTextActor3D>::New();
        label->SetInput(text.toUtf8().constData());
        label->SetPosition(position[0], position[1], position[2]);
        label->GetTextProperty()->SetFontSize(15);
        label->GetTextProperty()->SetBold(true);
        label->GetTextProperty()->SetColor(color.redF(), color.greenF(), color.blueF());
        label->GetTextProperty()->SetBackgroundColor(0.08, 0.09, 0.11);
        label->GetTextProperty()->SetBackgroundOpacity(0.75);
        label->SetPickable(0);
        m_annotationRenderer->AddViewProp(label);
        m_overlayLabels[key].push_back(label);
    }
    render();
}

void Mesh3DView::removeOverlay(int key)
{
    setOverlayLabels(key, {}, QColor());
    if (const auto it = m_overlayActors.find(key); it != m_overlayActors.end()) {
        if (m_annotationRenderer) m_annotationRenderer->RemoveActor(it->second);
        m_overlayActors.erase(it);
        render();
    }
}

void Mesh3DView::clearOverlays()
{
    if (m_annotationRenderer) {
        for (const auto& entry : m_overlayActors)
            m_annotationRenderer->RemoveActor(entry.second);
        for (const auto& entry : m_overlayLabels)
            for (const auto& prop : entry.second)
                m_annotationRenderer->RemoveViewProp(prop);
    }
    m_overlayActors.clear();
    m_overlayLabels.clear();
    render();
}

void Mesh3DView::setViewAlongDirection(const std::array<double, 3>& focal, const std::array<double, 3>& direction,
                                       const std::array<double, 3>& viewUp, double parallelScale)
{
    if (!m_renderer) return;
    auto* camera = m_renderer->GetActiveCamera();
    const double length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
    if (!camera || length < 1e-9) return;

    double bounds[6];
    sceneBounds(bounds);
    const bool validBounds = vtkBoundingBox::IsValid(bounds);
    double diagonal = 100.0;
    double fitScale = 50.0;
    if (validBounds) {
        const double dx = std::max(1.0, bounds[1] - bounds[0]);
        const double dy = std::max(1.0, bounds[3] - bounds[2]);
        const double dz = std::max(1.0, bounds[5] - bounds[4]);
        diagonal = std::sqrt(dx * dx + dy * dy + dz * dz);
        fitScale = std::max({dx, dy, dz}) * 0.62;
    }
    const double distance = std::max(200.0, diagonal * 1.8);
    camera->SetFocalPoint(focal[0], focal[1], focal[2]);
    camera->SetPosition(focal[0] - direction[0] / length * distance,
                        focal[1] - direction[1] / length * distance,
                        focal[2] - direction[2] / length * distance);
    camera->SetViewUp(viewUp[0], viewUp[1], viewUp[2]);
    camera->OrthogonalizeViewUp();
    camera->ParallelProjectionOn();
    camera->SetParallelScale(parallelScale > 0.0 ? parallelScale : fitScale);
    m_standardViewIndex = -1;
    if (validBounds)
        m_renderer->ResetCameraClippingRange(bounds);
    else
        m_renderer->ResetCameraClippingRange();
    render();
}

void Mesh3DView::setPlaneDragMode(bool active, const std::array<double, 3>& origin, const std::array<double, 3>& normal)
{
    m_planeDragActive = active;
    m_planeDragging = false;
    m_planeOrigin = origin;
    const double length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
    m_planeNormal = length > 1e-9 ? std::array<double, 3>{normal[0] / length, normal[1] / length, normal[2] / length}
                                  : std::array<double, 3>{0.0, 0.0, 1.0};
    updateInteractionCursor();
}

bool Mesh3DView::pickSurface(int px, int py, std::array<double, 3>& world) const
{
    if (!m_renderer) return false;
    auto picker = vtkSmartPointer<vtkCellPicker>::New();
    picker->SetTolerance(0.0025);
    // Markers and overlays live in the annotation layer and are never picked here.
    if (!picker->Pick(px, py, 0.0, m_foregroundRenderer) && !picker->Pick(px, py, 0.0, m_renderer))
        return false;
    if (m_actorLabels.find(picker->GetActor()) == m_actorLabels.end())
        return false;
    picker->GetPickPosition(world.data());
    return true;
}

bool Mesh3DView::pickEditablePoint(int px, int py, int& group, int& index) const
{
    if (m_editablePoints.empty() || !m_annotationRenderer) return false;
    auto picker = vtkSmartPointer<vtkCellPicker>::New();
    picker->SetTolerance(0.004);
    picker->PickFromListOn();
    for (const auto& p : m_editablePoints)
        picker->AddPickList(p.actor);
    if (!picker->Pick(px, py, 0.0, m_annotationRenderer))
        return false;
    for (const auto& p : m_editablePoints) {
        if (p.actor.GetPointer() == picker->GetActor()) {
            group = p.group;
            index = p.index;
            return true;
        }
    }
    return false;
}

bool Mesh3DView::planePointAt(int px, int py, std::array<double, 3>& world) const
{
    return rayPlanePoint(px, py, m_planeOrigin, m_planeNormal, world);
}

bool Mesh3DView::rayPlanePoint(int px, int py, const std::array<double, 3>& origin, const std::array<double, 3>& normal,
                               std::array<double, 3>& world) const
{
    if (!m_renderer) return false;
    double nearPoint[4] = {};
    double farPoint[4] = {};
    m_renderer->SetDisplayPoint(px, py, 0.0);
    m_renderer->DisplayToWorld();
    m_renderer->GetWorldPoint(nearPoint);
    m_renderer->SetDisplayPoint(px, py, 1.0);
    m_renderer->DisplayToWorld();
    m_renderer->GetWorldPoint(farPoint);
    if (std::abs(nearPoint[3]) < 1e-12 || std::abs(farPoint[3]) < 1e-12)
        return false;
    const std::array<double, 3> start{nearPoint[0] / nearPoint[3], nearPoint[1] / nearPoint[3], nearPoint[2] / nearPoint[3]};
    const std::array<double, 3> end{farPoint[0] / farPoint[3], farPoint[1] / farPoint[3], farPoint[2] / farPoint[3]};
    return RayPlaneIntersection(start, end, origin, normal, world);
}

bool Mesh3DView::handlePointEditEvent(QEvent* event)
{
    const QEvent::Type type = event->type();
    if (type != QEvent::MouseButtonPress && type != QEvent::MouseMove && type != QEvent::MouseButtonRelease)
        return false;
    auto* me = static_cast<QMouseEvent*>(event);
    int px = 0;
    int py = 0;
    toDisplay(m_vtkWidget, me->position(), px, py);

    if (type == QEvent::MouseButtonPress && me->button() == Qt::LeftButton) {
        int group = -1;
        int index = -1;
        if (pickEditablePoint(px, py, group, index)) {
            m_draggedPointGroup = group;
            m_draggedPointIndex = index;
            return true;
        }
        std::array<double, 3> hit{};
        if (m_pointEditGroup >= 0 && pickSurface(px, py, hit)) {
            emit editablePointAdded(m_pointEditGroup, hit[0], hit[1], hit[2]);
            return true;
        }
        return false; // empty space: the camera still rotates
    }

    if (type == QEvent::MouseMove && m_draggedPointIndex >= 0) {
        std::array<double, 3> hit{};
        EditablePoint* dragged = nullptr;
        for (auto& p : m_editablePoints) {
            if (p.group == m_draggedPointGroup && p.index == m_draggedPointIndex) {
                dragged = &p;
                break;
            }
        }
        bool found = false;
        if (me->modifiers().testFlag(Qt::ControlModifier) && dragged && m_renderer && m_renderer->GetActiveCamera()) {
            // Ctrl: free space, in the view plane through the marker.
            std::array<double, 3> origin{};
            std::array<double, 3> direction{};
            dragged->source->GetCenter(origin.data());
            m_renderer->GetActiveCamera()->GetDirectionOfProjection(direction.data());
            found = rayPlanePoint(px, py, origin, direction, hit);
        } else {
            found = pickSurface(px, py, hit);
        }
        if (found) {
            if (dragged)
                dragged->source->SetCenter(hit[0], hit[1], hit[2]);
            render();
            emit editablePointMoved(m_draggedPointGroup, m_draggedPointIndex, hit[0], hit[1], hit[2]);
        }
        return true;
    }

    if (type == QEvent::MouseButtonRelease && me->button() == Qt::LeftButton && m_draggedPointIndex >= 0) {
        const int group = m_draggedPointGroup;
        const int index = m_draggedPointIndex;
        m_draggedPointGroup = m_draggedPointIndex = -1;
        emit editablePointDragFinished(group, index);
        return true;
    }

    if (type == QEvent::MouseButtonPress && me->button() == Qt::RightButton) {
        m_rightPressGroup = m_rightPressIndex = -1;
        int group = -1;
        int index = -1;
        if (pickEditablePoint(px, py, group, index)) {
            m_rightPressGroup = group;
            m_rightPressIndex = index;
        }
        m_rightPressPosition = me->position();
        return false; // a right drag keeps rotating the camera
    }

    if (type == QEvent::MouseButtonRelease && me->button() == Qt::RightButton) {
        const QPointF delta = me->position() - m_rightPressPosition;
        const bool click = std::abs(delta.x()) + std::abs(delta.y()) <= kClickTolerancePixels;
        const int group = m_rightPressGroup;
        const int index = m_rightPressIndex;
        m_rightPressGroup = m_rightPressIndex = -1;
        if (click && index >= 0)
            emit editablePointRemoved(group, index);
        return false;
    }
    return false;
}

bool Mesh3DView::handlePlaneDragEvent(QEvent* event)
{
    const QEvent::Type type = event->type();
    if (type != QEvent::MouseButtonPress && type != QEvent::MouseMove && type != QEvent::MouseButtonRelease)
        return false;
    auto* me = static_cast<QMouseEvent*>(event);
    int px = 0;
    int py = 0;
    toDisplay(m_vtkWidget, me->position(), px, py);
    std::array<double, 3> hit{};

    if (type == QEvent::MouseButtonPress && me->button() == Qt::LeftButton) {
        if (!planePointAt(px, py, hit))
            return false;
        m_planeDragging = true;
        m_planeDragLastY = me->position().y();
        emit planeDragStarted(hit[0], hit[1], hit[2], me->modifiers());
        return true;
    }
    if (type == QEvent::MouseMove && m_planeDragging) {
        if (planePointAt(px, py, hit)) {
            const double deltaY = me->position().y() - m_planeDragLastY;
            m_planeDragLastY = me->position().y();
            emit planeDragMoved(hit[0], hit[1], hit[2], deltaY, me->modifiers());
        }
        return true;
    }
    if (type == QEvent::MouseButtonRelease && me->button() == Qt::LeftButton && m_planeDragging) {
        m_planeDragging = false;
        if (!planePointAt(px, py, hit))
            hit = m_planeOrigin;
        emit planeDragFinished(hit[0], hit[1], hit[2]);
        return true;
    }
    return false;
}

void Mesh3DView::setSurfaceBrushMode(bool active)
{
    m_brushActive = active;
    m_brushing = false;
    m_brushResizing = false;
    updateInteractionCursor();
}

std::array<double, 3> Mesh3DView::viewDirection() const
{
    if (!m_renderer || !m_renderer->GetActiveCamera())
        return {0.0, 1.0, 0.0};
    double position[3] = {}, focal[3] = {};
    m_renderer->GetActiveCamera()->GetPosition(position);
    m_renderer->GetActiveCamera()->GetFocalPoint(focal);
    std::array<double, 3> direction{focal[0] - position[0], focal[1] - position[1], focal[2] - position[2]};
    const double length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] +
                                    direction[2] * direction[2]);
    if (length < 1e-9)
        return {0.0, 1.0, 0.0};
    for (double& value : direction)
        value /= length;
    return direction;
}

bool Mesh3DView::handleBrushEvent(QEvent* event)
{
    const QEvent::Type type = event->type();
    if (type != QEvent::MouseButtonPress && type != QEvent::MouseMove && type != QEvent::MouseButtonRelease)
        return false;
    auto* me = static_cast<QMouseEvent*>(event);
    int px = 0;
    int py = 0;
    toDisplay(m_vtkWidget, me->position(), px, py);
    std::array<double, 3> hit{};

    if (type == QEvent::MouseButtonPress && me->button() == Qt::LeftButton) {
        m_brushing = true;
        m_brushResizing = me->modifiers().testFlag(Qt::AltModifier);
        m_brushLastY = me->position().y();
        if (!m_brushResizing && pickSurface(px, py, hit))
            emit surfaceBrushed(hit[0], hit[1], hit[2], me->modifiers());
        return true;
    }
    if (type == QEvent::MouseMove && m_brushing) {
        if (m_brushResizing || me->modifiers().testFlag(Qt::AltModifier)) {
            const double deltaY = me->position().y() - m_brushLastY;
            m_brushLastY = me->position().y();
            if (deltaY != 0.0)
                emit brushRadiusDragged(deltaY);
        } else if (pickSurface(px, py, hit)) {
            emit surfaceBrushed(hit[0], hit[1], hit[2], me->modifiers());
        }
        return true;
    }
    if (type == QEvent::MouseButtonRelease && me->button() == Qt::LeftButton && m_brushing) {
        m_brushing = false;
        m_brushResizing = false;
        emit surfaceBrushFinished();
        return true;
    }
    return false;
}

void Mesh3DView::sceneBounds(double bounds[6]) const
{
    vtkBoundingBox box;
    for (auto* renderer : {m_renderer.GetPointer(), m_foregroundRenderer.GetPointer()}) {
        double part[6];
        renderer->ComputeVisiblePropBounds(part);
        if (vtkBoundingBox::IsValid(part)) box.AddBounds(part);
    }
    box.GetBounds(bounds);
}

// ─────────────────────────────────────────────────────────────────────────────
// Gizmo  (interactive box widget for manual mesh repositioning)
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
void Mesh3DView::startGizmo(int label)
{
    // Stop any existing gizmo first
    if (hasGizmo()) stopGizmo();

    if (m_meshPolyData.find(label) == m_meshPolyData.end() ||
        m_meshActors.find(label)   == m_meshActors.end())
        return;

    m_gizmoLabel = label;

    // Snapshot the mesh so updateGizmoTransform() always transforms from rest
    m_gizmoOriginalMesh = vtkSmartPointer<vtkPolyData>::New();
    m_gizmoOriginalMesh->DeepCopy(m_meshPolyData[label]);
    m_gizmoTransformMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    m_gizmoTransformMatrix->Identity();

    // Build visual actors: X/Y/Z arrows + X/Y/Z rings + gold cube
    // All actors are now SetPickable(true) so they can be directly clicked
    double bounds[6];
    m_meshActors[label]->GetBounds(bounds);
    buildGizmoVisuals(bounds);

    // Store the gizmo center for rotation math and axis projection
    m_gizmoCenterWorld[0] = (bounds[0] + bounds[1]) * 0.5;
    m_gizmoCenterWorld[1] = (bounds[2] + bounds[3]) * 0.5;
    m_gizmoCenterWorld[2] = (bounds[4] + bounds[5]) * 0.5;

    // Create a cell picker restricted to the gizmo visual actors only
    m_gizmoPicker = vtkSmartPointer<vtkCellPicker>::New();
    m_gizmoPicker->SetTolerance(0.005);
    m_gizmoPicker->PickFromListOn();
    for (auto& actor : m_gizmoVisualActors)
        m_gizmoPicker->GetPickList()->AddItem(actor);

    // Register mouse event observer on the render-window interactor
    auto* iren = m_renderWindow->GetInteractor();
    if (iren) {
        m_gizmoInteractorObs = vtkSmartPointer<vtkCallbackCommand>::New();
        m_gizmoInteractorObs->SetCallback(GizmoInteractorCallback);
        m_gizmoInteractorObs->SetClientData(this);
        // Priority 1.0 fires before the default trackball-camera style
        iren->AddObserver(vtkCommand::LeftButtonPressEvent,   m_gizmoInteractorObs, 1.0);
        iren->AddObserver(vtkCommand::MouseMoveEvent,         m_gizmoInteractorObs, 1.0);
        iren->AddObserver(vtkCommand::LeftButtonReleaseEvent, m_gizmoInteractorObs, 1.0);
    }

    m_renderWindow->Render();
}

void Mesh3DView::buildGizmoVisuals(const double bounds[6])
{
    clearGizmoVisuals();
    if (!m_renderer) return;

    const double cx = (bounds[0] + bounds[1]) * 0.5;
    const double cy = (bounds[2] + bounds[3]) * 0.5;
    const double cz = (bounds[4] + bounds[5]) * 0.5;
    const double dx = std::max(1.0, bounds[1] - bounds[0]);
    const double dy = std::max(1.0, bounds[3] - bounds[2]);
    const double dz = std::max(1.0, bounds[5] - bounds[4]);
    const double size = std::max({dx, dy, dz});
    const double arrowLength = std::clamp(size * 0.52, 8.0, 38.0);
    const double ringRadius = std::clamp(size * 0.40, 6.0, 30.0);
    const double tubeRadius = std::clamp(size * 0.0045, 0.28, 0.85);

    auto makeActor = [&](vtkPolyData* pd, double r, double g, double b, double opacity = 0.92) {
        auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
        mapper->SetInputData(pd);
        mapper->ScalarVisibilityOff();
        auto actor = vtkSmartPointer<vtkActor>::New();
        actor->SetMapper(mapper);
        actor->GetProperty()->SetColor(r, g, b);
        actor->GetProperty()->SetOpacity(opacity);
        actor->GetProperty()->SetAmbient(0.35);
        actor->GetProperty()->SetDiffuse(0.75);
        actor->SetPickable(true);  // gizmo handles are directly clickable
        m_annotationRenderer->AddActor(actor);
        m_gizmoVisualActors.push_back(actor);
    };

    auto makeArrow = [&](double rx, double ry, double rz, double r, double g, double b) {
        auto source = vtkSmartPointer<vtkArrowSource>::New();
        source->SetShaftRadius(0.035);
        source->SetTipRadius(0.095);
        source->SetTipLength(0.28);
        source->Update();

        auto t = vtkSmartPointer<vtkTransform>::New();
        t->Translate(cx, cy, cz);
        t->RotateX(rx);
        t->RotateY(ry);
        t->RotateZ(rz);
        t->Scale(arrowLength, arrowLength, arrowLength);

        auto filter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
        filter->SetInputConnection(source->GetOutputPort());
        filter->SetTransform(t);
        filter->Update();
        makeActor(filter->GetOutput(), r, g, b);
    };

    makeArrow(0.0, 0.0, 0.0, 1.0, 0.05, 0.05);      // X red
    makeArrow(0.0, 0.0, 90.0, 0.05, 0.95, 0.10);    // Y green
    makeArrow(0.0, -90.0, 0.0, 0.15, 0.35, 1.0);    // Z blue

    auto makeRing = [&](double nx, double ny, double nz, double r, double g, double b) {
        auto ring = vtkSmartPointer<vtkRegularPolygonSource>::New();
        ring->SetNumberOfSides(160);
        ring->SetRadius(ringRadius);
        ring->SetCenter(cx, cy, cz);
        ring->SetNormal(nx, ny, nz);
        ring->GeneratePolygonOff();
        ring->Update();

        auto tube = vtkSmartPointer<vtkTubeFilter>::New();
        tube->SetInputConnection(ring->GetOutputPort());
        tube->SetRadius(tubeRadius);
        tube->SetNumberOfSides(12);
        tube->Update();
        makeActor(tube->GetOutput(), r, g, b, 0.78);
    };

    makeRing(1.0, 0.0, 0.0, 1.0, 0.05, 0.05);
    makeRing(0.0, 1.0, 0.0, 0.05, 0.95, 0.10);
    makeRing(0.0, 0.0, 1.0, 0.15, 0.35, 1.0);

    auto cube = vtkSmartPointer<vtkCubeSource>::New();
    const double cubeSize = std::clamp(size * 0.055, 2.5, 5.0);
    cube->SetCenter(cx, cy, cz);
    cube->SetXLength(cubeSize);
    cube->SetYLength(cubeSize);
    cube->SetZLength(cubeSize);
    cube->Update();
    makeActor(cube->GetOutput(), 1.0, 0.86, 0.05, 0.95);

    auto makeScaleHandle = [&](double ax, double ay, double az, double r, double g, double b) {
        auto handle = vtkSmartPointer<vtkCubeSource>::New();
        const double handleSize = std::clamp(size * 0.060, 3.0, 5.0);
        handle->SetCenter(cx + ax * arrowLength * 1.08,
                          cy + ay * arrowLength * 1.08,
                          cz + az * arrowLength * 1.08);
        handle->SetXLength(handleSize);
        handle->SetYLength(handleSize);
        handle->SetZLength(handleSize);
        handle->Update();
        makeActor(handle->GetOutput(), r, g, b, 0.98);
    };

    // Scale handles: draggable colored cubes at the end of each axis.
    // Actor indices must stay in sync with roleForActorIndex().
    makeScaleHandle(1.0, 0.0, 0.0, 1.0, 0.18, 0.18);   // Scale X
    makeScaleHandle(0.0, 1.0, 0.0, 0.18, 1.0, 0.22);   // Scale Y
    makeScaleHandle(0.0, 0.0, 1.0, 0.25, 0.45, 1.0);   // Scale Z
}

void Mesh3DView::clearGizmoVisuals()
{
    if (m_renderer) {
        for (auto& actor : m_gizmoVisualActors) {
            if (actor) m_annotationRenderer->RemoveActor(actor);
        }
    }
    m_gizmoVisualActors.clear();
}

void Mesh3DView::updateGizmoVisuals(vtkTransform* transform)
{
    for (auto& actor : m_gizmoVisualActors) {
        if (actor) actor->SetUserTransform(transform);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Applies the accumulated m_gizmoTransformMatrix to the original mesh and
// to all visual actors, giving the user real-time drag feedback.
void Mesh3DView::updateGizmoTransform()
{
    if (m_gizmoLabel == -1 || !m_gizmoOriginalMesh || !m_gizmoTransformMatrix) return;

    auto t = vtkSmartPointer<vtkTransform>::New();
    t->SetMatrix(m_gizmoTransformMatrix);

    updateGizmoVisuals(t);

    auto filter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
    filter->SetTransform(t);
    filter->SetInputData(m_gizmoOriginalMesh);  // always transform from rest position
    filter->Update();

    // In-place update: same vtkPolyData pointer the mapper already watches
    m_meshPolyData[m_gizmoLabel]->DeepCopy(filter->GetOutput());
    m_meshPolyData[m_gizmoLabel]->Modified();
    m_renderWindow->Render();
}

// ─────────────────────────────────────────────────────────────────────────────
// Static VTK interactor callback — routes press/move/release to handlers.
// Uses AbortFlagOn() to absorb events that are consumed by the gizmo, so the
// default interactor style (camera rotate/pan) never sees them.  This is
// preferable to SetEnabled(0/1) which can leave the style in a confused state.
void Mesh3DView::GizmoInteractorCallback(vtkObject* caller, unsigned long event,
                                          void* clientData, void*)
{
    auto* self = static_cast<Mesh3DView*>(clientData);
    auto* iren = static_cast<vtkRenderWindowInteractor*>(caller);
    const int x = iren->GetEventPosition()[0];
    const int y = iren->GetEventPosition()[1];

    if (event == vtkCommand::LeftButtonPressEvent) {
        self->handleGizmoPress(x, y);
        // Absorb the press only if a handle was actually hit
        if (self->m_gizmoDragRole != GizmoRole::None && self->m_gizmoInteractorObs)
            self->m_gizmoInteractorObs->AbortFlagOn();

    } else if (event == vtkCommand::MouseMoveEvent) {
        if (self->m_gizmoDragRole != GizmoRole::None) {
            self->handleGizmoMove(x, y);
            // Absorb move events while dragging so the camera doesn't pan
            if (self->m_gizmoInteractorObs)
                self->m_gizmoInteractorObs->AbortFlagOn();
        }

    } else if (event == vtkCommand::LeftButtonReleaseEvent) {
        if (self->m_gizmoDragRole != GizmoRole::None) {
            self->handleGizmoRelease();
            // Absorb the release that ends the drag
            if (self->m_gizmoInteractorObs)
                self->m_gizmoInteractorObs->AbortFlagOn();
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
Mesh3DView::GizmoRole Mesh3DView::roleForActorIndex(int idx) const
{
    switch (idx) {
        case 0: return GizmoRole::TransX;
        case 1: return GizmoRole::TransY;
        case 2: return GizmoRole::TransZ;
        case 3: return GizmoRole::RotX;
        case 4: return GizmoRole::RotY;
        case 5: return GizmoRole::RotZ;
        case 6: return GizmoRole::FreeMove;
        case 7: return GizmoRole::ScaleX;
        case 8: return GizmoRole::ScaleY;
        case 9: return GizmoRole::ScaleZ;
        default: return GizmoRole::None;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void Mesh3DView::handleGizmoPress(int x, int y)
{
    if (!m_gizmoPicker || m_gizmoVisualActors.empty()) return;

    m_gizmoPicker->Pick(x, y, 0.0, m_annotationRenderer);
    vtkActor* picked = m_gizmoPicker->GetActor();
    if (!picked) {
        // Clicked outside all handles — let the default camera style handle it
        m_gizmoDragRole = GizmoRole::None;
        return;
    }

    // Find the index of the picked actor
    int idx = -1;
    for (int i = 0; i < static_cast<int>(m_gizmoVisualActors.size()); ++i) {
        if (m_gizmoVisualActors[i].Get() == picked) { idx = i; break; }
    }
    if (idx < 0) { m_gizmoDragRole = GizmoRole::None; return; }

    m_gizmoDragRole  = roleForActorIndex(idx);
    m_gizmoDragLastX = x;
    m_gizmoDragLastY = y;
    // Event absorption is handled in GizmoInteractorCallback via AbortFlagOn()
}

// ─────────────────────────────────────────────────────────────────────────────
void Mesh3DView::handleGizmoMove(int x, int y)
{
    if (m_gizmoDragRole == GizmoRole::None) return;

    const int dx = x - m_gizmoDragLastX;
    const int dy = y - m_gizmoDragLastY;
    m_gizmoDragLastX = x;
    m_gizmoDragLastY = y;
    if (dx == 0 && dy == 0) return;

    // Compute the current center of the mesh in world space
    // (original center transformed by the accumulated matrix)
    double oc[4] = { m_gizmoCenterWorld[0], m_gizmoCenterWorld[1],
                     m_gizmoCenterWorld[2], 1.0 };
    double cc[4];
    m_gizmoTransformMatrix->MultiplyPoint(oc, cc);

    // Build the incremental world-space delta transform
    auto delta = vtkSmartPointer<vtkTransform>::New();
    delta->Identity();

    if (m_gizmoDragRole == GizmoRole::FreeMove) {
        // Translate in the camera plane (right + up vectors)
        auto* cam = m_renderer->GetActiveCamera();
        double vUp[3], pos[3], foc[3];
        cam->GetViewUp(vUp);
        cam->GetPosition(pos);
        cam->GetFocalPoint(foc);
        double fwd[3] = { foc[0]-pos[0], foc[1]-pos[1], foc[2]-pos[2] };
        double rgt[3];
        vtkMath::Cross(fwd, vUp, rgt);
        vtkMath::Normalize(rgt);
        vtkMath::Normalize(vUp);
        const double wpp = cam->GetDistance() / m_renderer->GetSize()[1] * 2.0;
        delta->Translate( rgt[0]*dx*wpp + vUp[0]*dy*wpp,
                          rgt[1]*dx*wpp + vUp[1]*dy*wpp,
                          rgt[2]*dx*wpp + vUp[2]*dy*wpp );
    } else if (m_gizmoDragRole == GizmoRole::TransX ||
               m_gizmoDragRole == GizmoRole::TransY ||
               m_gizmoDragRole == GizmoRole::TransZ) {
        const double ax = (m_gizmoDragRole == GizmoRole::TransX) ? 1.0 : 0.0;
        const double ay = (m_gizmoDragRole == GizmoRole::TransY) ? 1.0 : 0.0;
        const double az = (m_gizmoDragRole == GizmoRole::TransZ) ? 1.0 : 0.0;

        // Project the world axis onto display space at the current mesh center
        double d0[2], d1[2];
        m_renderer->SetWorldPoint(cc[0], cc[1], cc[2], 1.0);
        m_renderer->WorldToDisplay();
        d0[0] = m_renderer->GetDisplayPoint()[0];
        d0[1] = m_renderer->GetDisplayPoint()[1];
        m_renderer->SetWorldPoint(cc[0]+ax, cc[1]+ay, cc[2]+az, 1.0);
        m_renderer->WorldToDisplay();
        d1[0] = m_renderer->GetDisplayPoint()[0];
        d1[1] = m_renderer->GetDisplayPoint()[1];

        const double sX = d1[0] - d0[0];
        const double sY = d1[1] - d0[1];
        const double len2 = sX*sX + sY*sY;
        if (len2 < 1e-10) return;
        const double proj = (dx*sX + dy*sY) / len2;  // world units along axis
        delta->Translate(ax*proj, ay*proj, az*proj);
    } else if (m_gizmoDragRole == GizmoRole::ScaleX ||
               m_gizmoDragRole == GizmoRole::ScaleY ||
               m_gizmoDragRole == GizmoRole::ScaleZ) {
        const double ax = (m_gizmoDragRole == GizmoRole::ScaleX) ? 1.0 : 0.0;
        const double ay = (m_gizmoDragRole == GizmoRole::ScaleY) ? 1.0 : 0.0;
        const double az = (m_gizmoDragRole == GizmoRole::ScaleZ) ? 1.0 : 0.0;

        double d0[2], d1[2];
        m_renderer->SetWorldPoint(cc[0], cc[1], cc[2], 1.0);
        m_renderer->WorldToDisplay();
        d0[0] = m_renderer->GetDisplayPoint()[0];
        d0[1] = m_renderer->GetDisplayPoint()[1];
        m_renderer->SetWorldPoint(cc[0]+ax, cc[1]+ay, cc[2]+az, 1.0);
        m_renderer->WorldToDisplay();
        d1[0] = m_renderer->GetDisplayPoint()[0];
        d1[1] = m_renderer->GetDisplayPoint()[1];

        const double sX = d1[0] - d0[0];
        const double sY = d1[1] - d0[1];
        const double len = std::sqrt(sX*sX + sY*sY);
        if (len < 1e-6) return;

        const double pixelsAlongAxis = (dx*sX + dy*sY) / len;
        const double factor = std::clamp(std::exp(pixelsAlongAxis * 0.008), 0.92, 1.08);
        const double sx = (m_gizmoDragRole == GizmoRole::ScaleX) ? factor : 1.0;
        const double sy = (m_gizmoDragRole == GizmoRole::ScaleY) ? factor : 1.0;
        const double sz = (m_gizmoDragRole == GizmoRole::ScaleZ) ? factor : 1.0;

        delta->Translate( cc[0],  cc[1],  cc[2]);
        delta->Scale(sx, sy, sz);
        delta->Translate(-cc[0], -cc[1], -cc[2]);
    } else {
        // Rotation: dx → angle in degrees (0.5°/pixel)
        const double angle = dx * 0.5;
        delta->Translate( cc[0],  cc[1],  cc[2]);
        if      (m_gizmoDragRole == GizmoRole::RotX) delta->RotateX(angle);
        else if (m_gizmoDragRole == GizmoRole::RotY) delta->RotateY(angle);
        else if (m_gizmoDragRole == GizmoRole::RotZ) delta->RotateZ(angle);
        delta->Translate(-cc[0], -cc[1], -cc[2]);
    }

    // Compose: new_total = delta * old_total  (world-space increment applied last)
    auto composed = vtkSmartPointer<vtkTransform>::New();
    composed->PreMultiply();
    composed->SetMatrix(m_gizmoTransformMatrix);
    composed->Concatenate(delta->GetMatrix());
    m_gizmoTransformMatrix->DeepCopy(composed->GetMatrix());

    updateGizmoTransform();
}

// ─────────────────────────────────────────────────────────────────────────────
void Mesh3DView::handleGizmoRelease()
{
    m_gizmoDragRole = GizmoRole::None;
    // No style re-enable needed — we use AbortFlagOn() instead of SetEnabled(0)
}

// ─────────────────────────────────────────────────────────────────────────────
// Removes the gizmo handles, releases the interactor observer, and emits
// gizmoMeshUpdated() with the baked polydata.
// The polydata was already updated in real-time, so no extra baking is needed.
void Mesh3DView::stopGizmo()
{
    if (!hasGizmo()) return;

    // Restore the default interactor style before removing the observer
    if (m_gizmoDragRole != GizmoRole::None) {
        if (m_renderWindow && m_renderWindow->GetInteractor())
            if (auto* style = m_renderWindow->GetInteractor()->GetInteractorStyle())
                style->SetEnabled(1);
    }
    m_gizmoDragRole = GizmoRole::None;

    if (m_gizmoInteractorObs && m_renderWindow && m_renderWindow->GetInteractor())
        m_renderWindow->GetInteractor()->RemoveObserver(m_gizmoInteractorObs);
    m_gizmoInteractorObs = nullptr;
    m_gizmoPicker = nullptr;

    const int label = m_gizmoLabel;
    m_gizmoLabel = -1;
    m_gizmoOriginalMesh = nullptr;
    clearGizmoVisuals();

    if (m_meshPolyData.count(label))
        emit gizmoMeshUpdated(label, m_meshPolyData[label]);

    m_renderWindow->Render();
}

// ─────────────────────────────────────────────────────────────────────────────
vtkSmartPointer<vtkMatrix4x4> Mesh3DView::lastGizmoTransformMatrix() const
{
    auto matrix = vtkSmartPointer<vtkMatrix4x4>::New();
    matrix->Identity();
    if (m_gizmoTransformMatrix)
        matrix->DeepCopy(m_gizmoTransformMatrix);
    return matrix;
}
