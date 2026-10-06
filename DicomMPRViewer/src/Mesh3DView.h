#pragma once

#include <array>
#include <map>
#include <vector>

#include <QColor>
#include <QPointF>
#include <QStack>
#include <QVector>
#include <QWidget>
#include <vtkPolyData.h> // complete type: signals pass vtkSmartPointer<vtkPolyData> by value
#include <vtkSmartPointer.h>

class vtkActor;
class vtkProp;
class vtkActor2D;
class vtkCallbackCommand;
class vtkCellPicker;
class vtkGenericOpenGLRenderWindow;
class vtkMatrix4x4;
class vtkObject;
class vtkPolyData;
class vtkRenderer;
class vtkSphereSource;
class vtkTransform;
class QVTKOpenGLNativeWidget;

QT_BEGIN_NAMESPACE
class QLabel;
class QToolButton;
QT_END_NAMESPACE

class Mesh3DView : public QWidget
{
    Q_OBJECT

public:
    explicit Mesh3DView(QWidget* parent = nullptr);
    ~Mesh3DView() override;

    void setTitle(const QString& title);
    void addMesh(int label, vtkSmartPointer<vtkPolyData> mesh, const QString& name);
    void clearMeshes(bool preserveCamera = false);
    void resetCamera();
    void cycleStandardView();
    void setStandardView(int viewIndex);
    int standardViewIndex() const { return m_standardViewIndex; } // -1 after resetCamera
    void render();
    vtkSmartPointer<vtkPolyData> meshData(int label) const;
    void setGridVisible(bool visible);

    // Per-label controls (driven by the dynamic mask table)
    void setMeshColor(int label, const QColor& color);
    void setMeshOpacity(int label, double opacity);
    // User overrides survive workspace mesh rebuilds; negative opacity clears them.
    void setMeshDisplayOptions(int label, double opacity, bool alwaysOnTop);
    void setMeshVisible(int label, bool visible);
    bool isMeshVisible(int label) const; // false when there is no such mesh
    // Colors the mesh with its RGB point scalars instead of the actor color.
    void setMeshScalarColoring(int label, bool enabled);
    // Opacity changes make meshes pickable again; call after them.
    void setMeshPickable(int label, bool pickable);
    void removeMesh(int label);
    void setPointPickMode(bool active);
    void addPointMarker(double x, double y, double z, const QColor& color);
    void clearPointMarkers();

    // ── Lasso erase (3D surface trimming) ─────────────────────────────────
    // When active the user draws a freehand polygon on the 3D viewport;
    // mesh faces whose centroid projects inside the polygon are removed.
    void setLassoEraseMode(bool active);

    // ── Undo ──────────────────────────────────────────────────────────────
    bool canUndo() const { return !m_undoStack.isEmpty(); }
    void undo();

    // ── Gizmo (interactive 6-DOF axis handles) ────────────────────────────
    // After calling startGizmo(label), colored arrow/ring handles appear
    // around that mesh.  Arrows = axis-constrained translation (X red, Y
    // green, Z blue).  Rings = axis-constrained rotation.  Gold cube = free
    // translation in camera plane.  stopGizmo() bakes the final position
    // into the polydata and emits gizmoMeshUpdated.
    void startGizmo(int meshLabel);
    void stopGizmo();
    bool hasGizmo() const { return m_gizmoLabel != -1 && m_gizmoInteractorObs != nullptr; }
    vtkSmartPointer<vtkMatrix4x4> lastGizmoTransformMatrix() const;

    void setFullScreenActive(bool active);

    // ── Editable guide points ─────────────────────────────────────────────
    // While point editing is active: left click on a mesh adds a point to the
    // active group, left drag on a marker moves it over the surface, and a
    // right click without drag on a marker removes it (right drag still
    // rotates). Ctrl + drag moves a marker in free space (view plane through
    // it). Positions are owned by the caller: markers only follow the
    // mouse during a drag, so callers re-set the group from the signals.
    void setEditablePoints(int group, const std::vector<std::array<double, 3>>& points,
                           const QColor& color, double radius = 0.6);
    void clearEditablePoints();
    void setPointEditMode(bool active, int activeGroup = -1);

    // ── Overlays (e.g. the splint contour), drawn on top of the scene ─────
    void setOverlayPolyline(int key, vtkSmartPointer<vtkPolyData> lines, const QColor& color,
                            double lineWidth = 2.5);
    void removeOverlay(int key);
    void clearOverlays();
    bool hasOverlay(int key) const { return m_overlayActors.count(key) > 0; }
    // Text that faces the camera at points of the scene (a measurement's value beside its line), drawn on top.
    // An empty list removes the key's labels.
    void setOverlayLabels(int key, const std::vector<std::pair<std::array<double, 3>, QString>>& labels,
                          const QColor& color);
    int overlayLabelCount(int key) const
    {
        const auto it = m_overlayLabels.find(key);
        return it == m_overlayLabels.end() ? 0 : static_cast<int>(it->second.size());
    }

    // Parallel-projection camera looking along `direction` at `focal`;
    // parallelScale <= 0 fits the scene.
    void setViewAlongDirection(const std::array<double, 3>& focal, const std::array<double, 3>& direction,
                               const std::array<double, 3>& viewUp, double parallelScale = -1.0);

    // ── Plane drag (contour editing) ──────────────────────────────────────
    // Left drag reports the mouse ray intersected with the plane.
    void setPlaneDragMode(bool active, const std::array<double, 3>& origin = {0.0, 0.0, 0.0},
                          const std::array<double, 3>& normal = {0.0, 0.0, 1.0});

    // ── Surface brush (bracket marking) ───────────────────────────────────
    // Left press / drag reports the surface point under the mouse with the
    // keyboard modifiers (Ctrl = unmark); Alt + vertical drag reports pixel
    // deltas to resize the brush. Right drag still rotates.
    void setSurfaceBrushMode(bool active);

    // Where the camera looks (focal - position, normalised): the axis a trim sweeps along.
    std::array<double, 3> viewDirection() const;

    static bool RayPlaneIntersection(const std::array<double, 3>& rayStart, const std::array<double, 3>& rayEnd,
                                     const std::array<double, 3>& planeOrigin, const std::array<double, 3>& planeNormal,
                                     std::array<double, 3>& hit);

signals:
    void fullScreenToggleRequested(Mesh3DView* source);
    void pointPicked(int actorLabel, double x, double y, double z);
    void meshEdited(int meshLabel, vtkSmartPointer<vtkPolyData> newMesh);
    // Fired by stopGizmo() with the freshly-baked polydata
    void gizmoMeshUpdated(int meshLabel, vtkSmartPointer<vtkPolyData> newMesh);
    void editablePointAdded(int group, double x, double y, double z);
    void editablePointMoved(int group, int index, double x, double y, double z);
    void editablePointDragFinished(int group, int index);
    void editablePointRemoved(int group, int index);
    void planeDragStarted(double x, double y, double z, Qt::KeyboardModifiers modifiers);
    void planeDragMoved(double x, double y, double z, double deltaYPixels, Qt::KeyboardModifiers modifiers);
    void planeDragFinished(double x, double y, double z);
    void surfaceBrushed(double x, double y, double z, Qt::KeyboardModifiers modifiers);
    void brushRadiusDragged(double deltaYPixels);
    void surfaceBrushFinished();

private:
    bool eventFilter(QObject* watched, QEvent* event) override;

    void buildLayout();
    void initRenderer();
    void applyStandardView(int viewIndex);
    void applyLassoErase();   // removes triangles inside the lasso polygon
    void pushUndoState();     // captures current mesh state before an edit
    void buildGizmoVisuals(const double bounds[6]);
    void clearGizmoVisuals();
    void updateGizmoVisuals(vtkTransform* transform);
    void updateGridGeometry();
    void positionOverlayControls();
    void updateInteractionCursor();
    void showClickFeedback(const QPointF& position);
    void sceneBounds(double bounds[6]) const;

    QVTKOpenGLNativeWidget* m_vtkWidget  = nullptr;
    QLabel*                 m_titleLabel = nullptr;
    QToolButton*            m_gridToggleButton = nullptr;
    QWidget*                m_clickFeedback = nullptr;
    int                     m_clickFeedbackGeneration = 0;

    vtkSmartPointer<vtkGenericOpenGLRenderWindow> m_renderWindow;
    vtkSmartPointer<vtkRenderer>                  m_renderer;
    vtkSmartPointer<vtkRenderer>                  m_backgroundRenderer;
    vtkSmartPointer<vtkRenderer>                  m_foregroundRenderer;
    vtkSmartPointer<vtkRenderer>                  m_annotationRenderer;
    vtkSmartPointer<vtkActor2D>                   m_gridActor;
    vtkSmartPointer<vtkPolyData>                  m_gridPolyData;
    bool                                          m_gridVisible = true;

    // Keyed by label so we can address individual meshes after creation
    std::map<int, vtkSmartPointer<vtkActor>>    m_meshActors;
    std::map<int, vtkSmartPointer<vtkPolyData>> m_meshPolyData; // mutable surface
    std::map<vtkActor*, int>                    m_actorLabels;
    struct DisplayOptions { double opacity; bool alwaysOnTop; };
    std::map<int, DisplayOptions>               m_displayOptions;
    std::vector<vtkSmartPointer<vtkActor>>      m_pointMarkerActors;
    bool                                        m_pointPickActive = false;

    // ── Lasso state ────────────────────────────────────────────────────────
    bool             m_lassoActive  = false;
    bool             m_lassoDrawing = false;
    QVector<QPointF> m_lassoPoints;          // Qt widget coords, y=0 at top
    QWidget*         m_lassoCanvas  = nullptr;

    // ── Undo stack (surface edits only) ───────────────────────────────────
    using MeshSnapshot = std::map<int, vtkSmartPointer<vtkPolyData>>;
    QStack<MeshSnapshot> m_undoStack;
    static constexpr int kMaxUndo = 20;
    int m_standardViewIndex = 0; // frontal; -1 after resetCamera (oblique perspective)
    bool m_preserveCameraOnNextMesh = false;

    // ── Gizmo state ────────────────────────────────────────────────────────
    enum class GizmoRole {
        None,
        TransX, TransY, TransZ,
        RotX, RotY, RotZ,
        FreeMove,
        ScaleX, ScaleY, ScaleZ
    };

    void updateGizmoTransform();   // applies m_gizmoTransformMatrix to mesh + visuals
    void handleGizmoPress(int x, int y);
    void handleGizmoMove(int x, int y);
    void handleGizmoRelease();
    GizmoRole roleForActorIndex(int idx) const;
    static void GizmoInteractorCallback(vtkObject*, unsigned long, void* clientData, void*);

    vtkSmartPointer<vtkPolyData>           m_gizmoOriginalMesh; // snapshot before gizmo
    vtkSmartPointer<vtkMatrix4x4>          m_gizmoTransformMatrix;
    vtkSmartPointer<vtkCellPicker>         m_gizmoPicker;
    vtkSmartPointer<vtkCallbackCommand>    m_gizmoInteractorObs;
    std::vector<vtkSmartPointer<vtkActor>> m_gizmoVisualActors;
    GizmoRole m_gizmoDragRole   = GizmoRole::None;
    int       m_gizmoDragLastX  = 0;
    int       m_gizmoDragLastY  = 0;
    double    m_gizmoCenterWorld[3] = {0.0, 0.0, 0.0};
    int       m_gizmoLabel      = -1;

    // ── Editable points, overlays and plane drag ──────────────────────────
    bool handlePointEditEvent(QEvent* event);
    bool handlePlaneDragEvent(QEvent* event);
    bool pickSurface(int px, int py, std::array<double, 3>& world) const;
    bool pickEditablePoint(int px, int py, int& group, int& index) const;
    bool planePointAt(int px, int py, std::array<double, 3>& world) const;
    bool rayPlanePoint(int px, int py, const std::array<double, 3>& origin, const std::array<double, 3>& normal,
                       std::array<double, 3>& world) const;
    bool handleBrushEvent(QEvent* event);

    struct EditablePoint
    {
        int group = -1;
        int index = -1;
        vtkSmartPointer<vtkSphereSource> source;
        vtkSmartPointer<vtkActor> actor;
    };
    std::vector<EditablePoint> m_editablePoints;
    bool m_pointEditActive = false;
    int m_pointEditGroup = -1;
    int m_draggedPointGroup = -1;
    int m_draggedPointIndex = -1;
    int m_rightPressGroup = -1;
    int m_rightPressIndex = -1;
    QPointF m_rightPressPosition;

    std::map<int, vtkSmartPointer<vtkActor>> m_overlayActors;
    std::map<int, std::vector<vtkSmartPointer<vtkProp>>> m_overlayLabels;

    bool m_planeDragActive = false;
    bool m_planeDragging = false;
    std::array<double, 3> m_planeOrigin{0.0, 0.0, 0.0};
    std::array<double, 3> m_planeNormal{0.0, 0.0, 1.0};
    double m_planeDragLastY = 0.0;

    bool m_brushActive = false;
    bool m_brushing = false;
    bool m_brushResizing = false;
    double m_brushLastY = 0.0;
};
