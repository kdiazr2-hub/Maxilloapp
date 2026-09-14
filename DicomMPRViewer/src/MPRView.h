#pragma once

#include <set>
#include <array>
#include <memory>
#include <optional>
#include <vector>

#include <QVector>
#include <QPoint>
#include <QPointF>
#include <QWidget>
#include <vtkSmartPointer.h>

#include "WindowLevelPresets.h"

// Forward declarations – avoid pulling heavy VTK headers into every TU
// that includes this header.
class vtkResliceImageViewer;
class vtkResliceCursor;
class vtkImageData;
class vtkRenderer;
class vtkCornerAnnotation;
class vtkCommand;
class vtkActor;
class vtkPolyData;
class QVTKOpenGLNativeWidget;
class MPRViewInteractionCallback;
class MeasurementOverlay;
class SegmentationOverlay;
class SurfaceContourOverlay;
struct Measurement;

// A surface drawn as its intersection with the current slice.
struct MPRSurfaceContour
{
    vtkSmartPointer<vtkPolyData> mesh;
    std::array<double, 3> color{1.0, 0.82, 0.0};
};

QT_BEGIN_NAMESPACE
class QSlider;
class QLabel;
class QEvent;
class QPoint;
QT_END_NAMESPACE

// ─────────────────────────────────────────────────────────────────────────────
// MPROrientation
// Axis-aligned planes used for multi-planar reconstruction.
// ─────────────────────────────────────────────────────────────────────────────
enum class MPROrientation
{
    Axial,     // XY plane – superior → inferior
    Coronal,   // XZ plane – anterior → posterior
    Sagittal   // YZ plane – left     → right
};

// ─────────────────────────────────────────────────────────────────────────────
// MPRView
//
// Self-contained widget that owns:
//   • A QVTKOpenGLNativeWidget (the render surface)
//   • A vtkResliceImageViewer  (slice extraction + rendering)
//   • A QSlider                (slice navigation)
//   • Labels for title and current-slice info
//
// Designed for Phase 1 (display only).  Phase 2 will subclass or compose
// this widget to add segmentation overlays without modifying existing code.
// ─────────────────────────────────────────────────────────────────────────────
class MPRView : public QWidget
{
    Q_OBJECT

public:
    explicit MPRView(MPROrientation orientation, QWidget* parent = nullptr);
    ~MPRView() override;

    // Attach a loaded vtkImageData volume; call once after DicomVolumeLoader.
    void setVolume(vtkSmartPointer<vtkImageData> imageData);
    void setSharedResliceCursor(vtkResliceCursor* cursor);
    vtkResliceCursor* resliceCursor() const;

    // Update window / level on this view's colour-mapping pipeline.
    void applyWindowLevel(const WindowLevelPreset& preset);

    void resetCamera();
    void render();
    void syncFromCursor();
    void setFullScreenActive(bool active);
    void setMeasurementPickingEnabled(bool enabled);
    void setMeasurementFinishRequiresDoubleRightClick(bool enabled);
    void setMeasurements(const std::vector<Measurement>& measurements, bool visible);
    void setMeasurementPreview(const std::optional<Measurement>& measurement);
    void setSegmentationLabelmap(vtkSmartPointer<vtkImageData> labelmap);
    void setSegmentationHiddenLabels(const std::set<int>& hiddenLabels);
    void setSegmentationVisible(bool visible);
    void setSegmentationOpacity(double opacity);
    // Surfaces (e.g. registered intraoral scans) drawn as contours on the slice.
    void setSurfaceContours(const std::vector<MPRSurfaceContour>& contours);
    int surfaceContourCount() const;
    MPROrientation orientation() const { return m_orientation; }

    int currentSlice() const;
    int sliceMin()     const;
    int sliceMax()     const;

    // ── Lasso editing ─────────────────────────────────────────────────────
    // active=true enables freehand lasso drawing on mouse drag.
    // addMode=true paints the label; false erases it.
    void setLassoMode(bool active, bool addMode);

    // Expose the VTK renderer so SegmentationMaskEditor can convert coords.
    vtkRenderer* renderer() const;

signals:
    void reslicePositionChanged(MPRView* source);
    void fullScreenToggleRequested(MPRView* source);
    void physicalPointClicked(MPRView* source, double x, double y, double z);
    void measurementCompleteRequested(MPRView* source);
    void windowPresetMenuRequested(MPRView* source, const QPoint& globalPos);
    // Emitted when the user releases the mouse after drawing a lasso polygon.
    // vtkDisplayPoints are in VTK display coords (y=0 at render window bottom).
    void lassoEditCompleted(MPRView* source, QVector<QPointF> vtkDisplayPoints,
                            bool addMode);

private slots:
    void onSliderValueChanged(int value);

private:
    friend class MPRViewInteractionCallback;

    bool eventFilter(QObject* watched, QEvent* event) override;

    void buildLayout();
    void initViewer();
    void configureResliceCursorRepresentation();
    void installInteractionObservers();
    void handleVTKInteraction();
    void buildCrosshairActors();
    void updateCrosshair();
    void frameAnatomyOnCurrentSlice();
    void refreshSliceLabel();
    int sliceAxis() const;
    void planeAxes(int& horizontalAxis, int& verticalAxis, int& normalAxis) const;
    int sliceIndexFromCursor() const;
    double slicePosition() const;
    int clampSlice(int value) const;
    void setSliceIndex(int value, bool notify);
    void updateCursorCenterFromSlice(int index);
    bool pickPhysicalPoint(const QPoint& position, std::array<double, 3>& point) const;

    MPROrientation  m_orientation;

    vtkSmartPointer<vtkResliceImageViewer> m_viewer;
    vtkSmartPointer<vtkImageData>          m_imageData;
    vtkSmartPointer<vtkCommand>            m_interactionCallback;
    vtkSmartPointer<vtkActor>              m_crosshairHorizontalActor;
    vtkSmartPointer<vtkActor>              m_crosshairVerticalActor;
    vtkSmartPointer<vtkPolyData>           m_crosshairHorizontalData;
    vtkSmartPointer<vtkPolyData>           m_crosshairVerticalData;
    std::unique_ptr<MeasurementOverlay>    m_measurementOverlay;
    std::unique_ptr<SegmentationOverlay>   m_segmentationOverlay;
    std::unique_ptr<SurfaceContourOverlay> m_surfaceContourOverlay;
    std::vector<MPRSurfaceContour>         m_surfaceContours;

    QVTKOpenGLNativeWidget* m_vtkWidget  = nullptr;
    QSlider*                m_slider     = nullptr;
    QLabel*                 m_titleLabel = nullptr;
    QLabel*                 m_sliceLabel = nullptr;

    bool m_observersInstalled = false;
    bool m_suppressVtkEvents  = false;
    bool m_crosshairBuilt     = false;
    bool m_measurementPickingEnabled = false;
    bool m_measurementFinishRequiresDoubleRightClick = false;

    // ── Lasso state ────────────────────────────────────────────────────────
    bool             m_lassoActive  = false;
    bool             m_lassoAddMode = true;
    bool             m_lassoDrawing = false;
    QVector<QPointF> m_lassoPoints;
    QWidget*         m_lassoCanvas  = nullptr;
};
