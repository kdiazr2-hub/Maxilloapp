#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <array>
#include <map>

#include "ProjectSerializer.h"
#include "AppStateManager.h"
#include "SplintDesignCore.h"
#include "CompositeBlockCore.h"
#include "RegistrationResult.h"

#include <QList>
#include <QMainWindow>
#include <QMap>
#include <QPoint>
#include <QPointF>
#include <QStack>
#include <QThread>
#include <QVector>
#include <QVector3D>
#include <vtkSmartPointer.h>

#include "AsyncDicomLoader.h"      // AsyncDicomLoader; Q_DECLARE_METATYPE guards
#include "DicomSeriesIndexer.h"    // SeriesInfo
#include "DicomVolumeLoader.h"     // VolumeMetadata
#include "AISegmentationService.h"
#include "BoneSplitterService.h"
#include "MeasurementManager.h"
#include "WindowLevelPresets.h"

class vtkImageData;
class vtkMatrix4x4;
class vtkPolyData;
class MPRView;
class Mesh3DView;
class MeasurementTool;
class SplintDesignPanel;
class SplintPreviewScheduler;

QT_BEGIN_NAMESPACE
class QLabel;
class QDialog;
class QGroupBox;
class QAction;
class QComboBox;
class QCheckBox;
class QSlider;
class QListWidget;
class QProgressBar;
class QSplitter;
class QStackedWidget;
class QEvent;
class QKeyEvent;
class QPushButton;
class QDoubleSpinBox;
class QRadioButton;
class QTableWidget;
class QToolButton;
class QTimer;
QT_END_NAMESPACE

// ─────────────────────────────────────────────────────────────────────────────
// MainWindow
//
// Top-level window.  Owns three MPRView widgets (axial / coronal / sagittal),
// a lateral info panel, and the menu bar.
//
// Load pipeline (async):
//   onOpenDicomFolder()
//     → loadVolume()
//     → QMetaObject::invokeMethod → AsyncDicomLoader::startLoad()  [worker thread]
//         → DicomSeriesIndexer::indexFolder()
//         → VolumeCacheManager::hasCachedVolume()
//         → (if cache hit)  emit volumeReady()
//         → (if cache miss) emit previewReady() then emit volumeReady()
//     → onPreviewReady() / onVolumeReady()  [main thread, queued connection]
//         → distributeVolume() → MPRView::setVolume()
// ─────────────────────────────────────────────────────────────────────────────
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

signals:
    // Signals the worker thread to begin loading a user-selected series
    // (emitted from onSeriesFound() after the user picks from a dialog).
    void requestLoadSeries(SeriesInfo series, QString cacheDir);

private slots:
    void onOpenDicomFolder();
    void onSaveProject();
    void onOpenProject();
    void onBoneWindow();
    void onSoftTissueWindow();
    void onLungWindow();
    void onBrainWindow();

    // ── Async load pipeline callbacks (main thread, queued) ───────────────
    void onLoadStatusChanged(const QString& message);
    void onLoadProgressChanged(int percent);
    void onSeriesFound(const QVector<SeriesInfo>& series);
    void onPreviewReady(vtkSmartPointer<vtkImageData> preview,
                        DicomVolumeLoader::VolumeMetadata meta);
    void onVolumeReady(vtkSmartPointer<vtkImageData> volume,
                       DicomVolumeLoader::VolumeMetadata meta);
    void onLoadError(const QString& message);

private:
    friend class RepositionWorkspaceTests;
    friend class SplintWorkspaceTests;
    enum class MeasurementToolMode
    {
        Cursor,
        Distance,
        Angle,
        Annotation,
        Area,
        ROICircle,
        ROIRectangle,
        PerpendicularDistance
    };

    enum class DentalPointSet
    {
        None,
        MaxillaBone,
        UpperArch,
        MandibleBone,
        LowerArch
    };

    enum class BitePointSet
    {
        None,
        LeFortSegment,
        UpperBiteScan,
        MandibleDistal,
        LowerBiteScan
    };

    enum class SplintPointSet
    {
        None,
        UpperVestibular,
        UpperPalatal,
        LowerVestibular,
        LowerLingual
    };

    enum class MeshSmoothingPreset
    {
        Light,
        Moderate,
        Optimal
    };

    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

    void buildMenuBar();
    void buildToolBar();
    void buildCentralWidget();
    QWidget* buildLeftMenuPanel();
    QWidget* buildInfoPanel();
    void setModelsWorkspace(bool enabled);
    void setOrientationWorkspace(bool enabled);

    void loadVolume(const QString& folderPath);
    void distributeVolume();
    void broadcastPreset(const WindowLevelPreset& preset);
    void refreshInfoPanel();
    void syncResliceViews(MPRView* source);
    bool toggleViewUnderCursor();
    QWidget* viewAtCursor() const;
    void toggleViewFullScreen(QWidget* view);
    void exitViewFullScreen();
    void deactivateLassoTools();
    void setMeasurementTool(MeasurementToolMode mode);
    void handleMeasurementPoint(MPRView* source, double x, double y, double z);
    void finishMeasurementTool(MPRView* source);
    std::optional<Measurement> activeMeasurementPreview() const;
    void refreshMeasurementTable();
    void refreshMeasurementOverlays();
    void deleteSelectedMeasurement();
    void saveMeasurements();
    void loadMeasurements();
    void syncModelViews();
    void syncOrientationView();
    void updateModelWorkflowUi();
    void goBackModelWorkflow();
    QColor maskColorForLabel(int label) const;
    QColor objectColorForLabel(int label) const;
    void applyMaskColorToAllViews(int label, const QColor& color);
    void applyObjectColorToAllViews(int label, const QColor& color);
    void applyObjectDisplayOptionsToAllViews(int label, double opacity, bool alwaysOnTop);
    void setObjectDisplayOptions(int label, double opacity, bool alwaysOnTop);
    void updateObjectAppearanceControls();
    void applyProjectState(const ProjectState& state);
    ProjectState collectProjectState() const;
    void saveProjectTo(const QString& path);
    void setLoadingUiEnabled(bool enabled);
    void showSegmentationBackendInfo();
    void startAISegmentation(SegmentationTarget target);
    void onSegmentationFinished(const QString& outputSegmentationPath);
    void onSegmentationError(const QString& error);
    void refreshSegmentationOverlays();
    void generateSelectedMesh();
    int selectedMaskLabel() const;
    void beginBoneCavityFill();
    void cancelBoneCavityFill();
    void fillBoneCavityAt(const std::array<double, 3>& point);
    bool hasDerivedBonePlanning() const;
    QString refreshEditedSegmentationMesh(int label);
    void publishSegmentationMesh(int label, vtkSmartPointer<vtkPolyData> mesh);
    void calculateObjectFromMask(int label, MeshSmoothingPreset smoothing = MeshSmoothingPreset::Moderate);
    int smoothingIterationsForPreset(MeshSmoothingPreset smoothing) const;
    QString smoothingNameForPreset(MeshSmoothingPreset smoothing) const;
    bool chooseMeshSmoothingPreset(MeshSmoothingPreset* smoothing) const;
    bool objectEntryExists(int label) const;
    bool objectEntryVisible(int label) const;
    void syncVisibilityPanelToAllViews();
    void publishCompositeMeshesToSceneViews();
    void ensureOsteotomyMeshesPresent();
    void onSurfaceMeshEdited(int meshLabel, vtkSmartPointer<vtkPolyData> newMesh);
    void setMaskVisible(int label, bool visible);
    void addMaskEntry(const QString& name, const QColor& color, int label);
    void addObjectEntry(const QString& name, const QColor& color, int label);
    void removeObjectEntry(int label);
    void deleteSelectedMask();
    // Called when the user finishes drawing a lasso on any MPR view.
    void onLassoEdit(MPRView* source, QVector<QPointF> vtkDisplayPoints,
                     bool addMode);
    void showMprPresetContextMenu(MPRView* source, const QPoint& globalPos);
    void undoLastEdit();
    void onBoneSplitFinished(const QString& outputPath);
    void onBoneSplitError(const QString& error);
    void importUpperArchStl();
    void importLowerArchStl();
    void setDentalPointCapture(DentalPointSet set);
    void onDentalPointPicked(int actorLabel, double x, double y, double z);
    void alignUpperArchToMaxilla();
    void alignLowerArchToMandible();
    void alignBothDentalArches();
    void startDentalAdjustmentGizmo(int actorKey);
    void commitActiveDentalGizmos(bool showReview = true);
    void onDentalGizmoMeshUpdated(int actorKey, vtkSmartPointer<vtkPolyData> newMesh);
    void resetDentalArchTransforms();
    void continueToOrientationWithoutMatch();
    void createDentalCompositeModels();
    void onDentalCompositeFinished(int step, vtkSmartPointer<vtkPolyData> mesh);
    void setObjectEntryVisible(int label, bool visible);
    void exportDentalCompositeStl();
    void exportDentalRegistrationPackage();
    vtkSmartPointer<vtkPolyData> resolvedUpperArchWorldMesh(QString* error = nullptr) const;
    vtkSmartPointer<vtkPolyData> resolvedLowerArchWorldMesh(QString* error = nullptr) const;
    vtkSmartPointer<vtkPolyData> loadStlMesh(const QString& filePath, QString* error) const;
    vtkSmartPointer<vtkPolyData> transformMesh(vtkPolyData* mesh, const QVector<QVector3D>& moving,
                                               const QVector<QVector3D>& fixed, QString* error,
                                               vtkMatrix4x4* outputMatrix = nullptr,
                                               double* landmarkRms = nullptr) const;
    vtkSmartPointer<vtkPolyData> refineArchWithIcp(vtkPolyData* arch, vtkPolyData* bone,
                                                   bool upperArch, QString* report,
                                                   QString* error,
                                                   vtkMatrix4x4* outputMatrix = nullptr) const;
    void logRegistrationDiagnostics(const QString& name, vtkPolyData* bone, vtkPolyData* arch) const;
    vtkSmartPointer<vtkPolyData> appendMeshes(const QVector<vtkSmartPointer<vtkPolyData>>& meshes) const;
    vtkSmartPointer<vtkPolyData> booleanComposite(vtkPolyData* bone, vtkPolyData* arch, bool upperArch) const;
    vtkSmartPointer<vtkPolyData> meshForAnatomicLabel(int label) const;
    void updateDentalPointStatus();
    void clearDentalRegistrationPoints();
    void rebuildDentalPointMarkers();
    void showFinalCompositeView(bool advanceToOrientation = false);
    void updateButtonStates();    // Phase 1: syncs all toolbar QAction* enable states
    void setGuidedActionState(QAction* action, const QString& state);
    void clearGuidedActionStates(const QVector<QAction*>& actions);
    void setGuidedNext(QAction* action, const QString& message = {});
    void setGuidedDone(QAction* action);
    void exportClinicalLog();     // Phase 4: prompts for path, saves LoggerCore log

    // ── Frankfurt plane (PLAN/Orientación workspace) ───────────────────────
    void onOrientationPointPicked(int actorLabel, double x, double y, double z);
    void onFrankfurtPointPicked(int actorLabel, double x, double y, double z);
    void onOrientationGizmoUpdated(int meshLabel, vtkSmartPointer<vtkPolyData> newMesh);
    void alignFrankfurtPlane();
    void updateFrankfurtPointStatus();
    void exportOrientedCompositeStl();

    // ── Le Fort I osteotomy workspace ─────────────────────────────────────
    void setOsteotomyWorkspace(bool enabled);
    void setBiteRegistrationWorkspace(bool enabled);
    void setRepositionWorkspace(bool enabled);
    void setSplintWorkspace(bool enabled);
    // step=0: upper reg review → "Continuar con Mandíbula"
    // step=1: lower reg review → "Continuar con Orientación" (auto-creates composites)
    void showRegistrationReview(int step);
    void onLeFortPointPicked(int actorLabel, double x, double y, double z);
    QWidget* buildOsteotomyWizardPanel(QWidget* parent);
    void setLeFortWizardStep(int step);
    void updateLeFortWizardUi();
    void refreshLeFortFinalizeTable();
    void startLeFortPointCapture(int index);
    void applyLeFortGuideSpinValues();
    void updateLeFortPointStatus();
    void updateLeFortPlaneDisc();          // redraws the cut-plane indicator from landmarks
    void rebuildLeFortPlaneFromState();    // redraws using stored m_leFortPlaneNormal/Center
    vtkSmartPointer<vtkPolyData> buildLeFortCutGuideMesh() const;
    void showLeFortGuidePropertiesDialog();
    void executeOsteotomySplit();
    void executeLeFortSplit();
    void executeBssoSplit();
    void executeGenioSplit();
    void exportLeFortSegments();
    void onOsteotomyGizmoUpdated(int meshLabel, vtkSmartPointer<vtkPolyData> newMesh);
    void onBssoPointPicked(int actorLabel, double x, double y, double z);
    void updateBssoPointStatus();
    void setBssoActiveSide(bool leftSide);
    void updateOsteotomyWorkflowUi();
    void updateBssoGuide();
    vtkSmartPointer<vtkPolyData> buildBssoGuideMesh() const;
    void showBssoGuidePropertiesDialog();
    void onGenioPointPicked(int actorLabel, double x, double y, double z);
    void updateGenioPointStatus();
    void updateGenioPlaneGuide();
    vtkSmartPointer<vtkPolyData> buildGenioGuideMesh() const;
    void rebuildGenioPlaneFromState();
    void importBiteScanStl();
    void setBitePointCapture(BitePointSet set);
    void onBitePointPicked(int actorLabel, double x, double y, double z);
    void alignLeFortToBiteScan();
    void alignMandibleDistalToBiteScan();
    void clearBiteRegistrationPoints();
    void rebuildBitePointMarkers();
    void syncBiteRegistrationView();
    void updateBiteRegistrationUi();
    int currentBiteMandibleTargetLabel() const;
    void startBiteAdjustmentGizmo(int label);
    void acceptBiteAdjustmentGizmo();
    void onBiteGizmoMeshUpdated(int meshLabel, vtkSmartPointer<vtkPolyData> newMesh);
    void applyBiteMandibleBlockTransform(int mandibleLabel, vtkMatrix4x4* matrix);
    void transformBiteScanPointLists(vtkMatrix4x4* matrix);
    vtkSmartPointer<vtkPolyData> refineBiteRegistrationWithIcp(
        vtkPolyData* moving, vtkPolyData* biteScan, QString* report, QString* error,
        vtkMatrix4x4* outputMatrix = nullptr) const;
    QWidget* buildRepositionControlPanel(QWidget* parent);
    void syncRepositionView();
    void syncRepositionSelectionVisibility();
    void updateRepositionTargetList();
    void updateRepositionControls();
    void captureMandibleMovementReference(int label, vtkPolyData* mesh);
    void recordMandibleMovement(int label, vtkMatrix4x4* delta, bool biteStage = false);
    void updateMandibleMovementSummary();
    int currentRepositionTargetLabel() const;
    QList<int> selectedRepositionTargetLabels() const;
    vtkSmartPointer<vtkPolyData> repositionMeshForLabel(int label) const;
    void setRepositionMeshForLabel(int label, vtkSmartPointer<vtkPolyData> mesh);
    void applyRepositionTransform(vtkMatrix4x4* matrix, const QString& description);
    void beginDefineRepositionPivot();
    void onRepositionPivotPicked(int actorLabel, double x, double y, double z);
    void refreshRepositionPivotMarker();
    void translateRepositionTarget(double dx, double dy, double dz);
    void rotateRepositionTarget(double axisX, double axisY, double axisZ, double degrees);
    void resetRepositionTarget();
    void sendRepositionTargetHome();
    void startRepositionGizmo();
    void acceptRepositionGizmo();
    void onRepositionGizmoUpdated(int meshLabel, vtkSmartPointer<vtkPolyData> newMesh);
    void syncSplintView();
    void updateSplintControls();
    void createIntermediateSplint();
    void createFinalSplint();
    void createSplint(bool finalSplint);
    void createSelectedSplint();
    void startSplintGizmo();
    void acceptSplintGizmo();
    void onSplintGizmoUpdated(int meshLabel, vtkSmartPointer<vtkPolyData> newMesh);
    void onSplintGenerated(int label, vtkSmartPointer<vtkPolyData> mesh,
                           const QString& report, bool booleanApplied);
    void exportSplintStl();
    int selectedSplintUpperLabel() const;
    int selectedSplintLowerLabel(bool finalSplint) const;
    vtkSmartPointer<vtkPolyData> meshForSplintSourceLabel(int label) const;
    vtkSmartPointer<vtkPolyData> initialMandibleMeshForSplint() const;
    vtkSmartPointer<vtkPolyData> finalMandibleMeshForSplint() const;
    void setSplintPointCapture(SplintPointSet set);
    void onSplintPointPicked(int actorLabel, double x, double y, double z);
    void rebuildSplintPointMarkers();
    void clearSplintPoints();
    int splintPointCount(SplintPointSet set) const;
    bool hasEnoughSplintGuidePoints() const;

    // ── MPR views ──────────────────────────────────────────────────────────
    MPRView*   m_axialView    = nullptr;
    MPRView*   m_coronalView  = nullptr;
    MPRView*   m_sagittalView = nullptr;
    Mesh3DView* m_mesh3DView  = nullptr;
    // ── MODELOS workspace ─────────────────────────────────────────────────
    // Layout per step:
    //   Top-left : bone view  (maxilar or mandíbula)
    //   Top-right: arch view  (arco superior or inferior)
    //   Bottom   : match preview (both meshes overlaid, full width)
    //   Below    : "Crear Modelo Compuesto" button
    // Step 0 = upper pair, Step 1 = lower pair (QStackedWidget switches)
    Mesh3DView* m_modelMaxillaView   = nullptr;  // top-left  step 0
    Mesh3DView* m_modelUpperArchView = nullptr;  // top-right step 0
    Mesh3DView* m_modelMandibleView  = nullptr;  // top-left  step 1
    Mesh3DView* m_modelLowerArchView = nullptr;  // top-right step 1
    Mesh3DView* m_modelMatchView     = nullptr;  // bottom match preview (shared)
    Mesh3DView* m_orientationView     = nullptr;  // cranial orientation / Frankfort plane prep
    Mesh3DView* m_osteotomyView       = nullptr;  // Le Fort I osteotomy workspace
    Mesh3DView* m_biteSegmentView      = nullptr; // bite registration: Le Fort + mandible
    Mesh3DView* m_biteScanView         = nullptr; // bite registration: bite scan reference
    Mesh3DView* m_biteRegistrationView = nullptr; // bite registration: match preview
    Mesh3DView* m_repositionView      = nullptr;  // postoperative reposition workspace
    Mesh3DView* m_splintUpperView     = nullptr;  // occlusal splint: upper source
    Mesh3DView* m_splintLowerView     = nullptr;  // occlusal splint: lower source
    Mesh3DView* m_splintView          = nullptr;  // occlusal splint: combined preview
    QWidget*    m_osteotomyWizardPanel = nullptr;
    QStackedWidget* m_leFortWizardStack = nullptr;
    QStackedWidget* m_modelStepStack   = nullptr;  // switches the top pair
    QPushButton*    m_compositeButton  = nullptr;  // "Crear Modelo Compuesto"
    QStackedWidget* m_viewModeStack    = nullptr;
    QSplitter*      m_mprSplitter      = nullptr;
    QSplitter*      m_modelSplitter    = nullptr;  // vertical: step stack / match view
    QSplitter*      m_repositionSplitter = nullptr;
    QWidget*   m_leftMenuPanel = nullptr;
    QStackedWidget* m_leftModuleStack = nullptr;
    QWidget*   m_infoPanel    = nullptr;

    // ── Info panel labels ──────────────────────────────────────────────────
    QLabel* m_lblPatientName = nullptr;
    QLabel* m_lblPatientId   = nullptr;
    QLabel* m_lblStudyDate   = nullptr;
    QLabel* m_lblDimensions  = nullptr;
    QLabel* m_lblSpacing     = nullptr;
    QLabel* m_lblHuRange     = nullptr;
    QLabel* m_lblNumSlices   = nullptr;
    QTableWidget* m_measurementTable       = nullptr;
    QPushButton*  m_deleteMeasurementButton = nullptr;
    QComboBox*    m_structureCombo          = nullptr;
    QTableWidget* m_maskTable               = nullptr;   // dynamic mask list
    QTableWidget* m_objectTable             = nullptr;   // calculated mesh objects
    QSlider*      m_objectOpacitySlider     = nullptr;
    QLabel*       m_objectOpacityValue      = nullptr;
    QCheckBox*    m_objectOnTopCheck         = nullptr;

    // ── Async loader infrastructure ────────────────────────────────────────
    QThread*          m_loaderThread   = nullptr;
    AsyncDicomLoader* m_asyncLoader    = nullptr;
    QProgressBar*     m_progressBar    = nullptr;
    QAction*          m_openAction     = nullptr;   // disabled while loading
    QString           m_cacheDir;
    QString           m_dicomFolder;          // last successfully loaded DICOM folder
    QString           m_projectFilePath;      // path of the open .maxilloproject (empty if unsaved)
    std::optional<ProjectState> m_pendingProjectState; // set before async DICOM load for project open
    bool              m_loadInProgress = false;
    bool              m_previewActive  = false;
    WindowLevelPreset m_currentPreset  = WindowLevelPresets::Bone;

    // ── Data ───────────────────────────────────────────────────────────────
    vtkSmartPointer<vtkImageData>     m_volume;
    vtkSmartPointer<vtkImageData>     m_segmentationLabelmap;
    DicomVolumeLoader::VolumeMetadata m_volumeMeta;
    QString                           m_segmentationOutputDir;
    int                               m_pendingAutoMeshLabel = -1;  // -1 = none
    std::set<int>                     m_hiddenMaskLabels;
    QMap<int, int>                    m_maskSmoothingIterations;

    // ── Labelmap undo stack (2-D lasso edits) ─────────────────────────────
    QStack<vtkSmartPointer<vtkImageData>> m_labelmapUndoStack;
    static constexpr int kMaxUndo2D = 20;
    void pushLabelmapUndo();

    MeasurementManager                m_measurementManager;
    AISegmentationService*            m_aiSegmentationService = nullptr;
    BoneSplitterService*              m_boneSplitter          = nullptr;
    std::unique_ptr<MeasurementTool>  m_activeMeasurementTool;
    MeasurementToolMode               m_measurementToolMode = MeasurementToolMode::Cursor;
    QAction*                          m_toggleMeasurementsAct = nullptr;
    QAction*                          m_autoSegmentationAct = nullptr;
    QAction*                          m_toggleSegmentationOverlayAct = nullptr;
    QToolButton*                      m_lassoAddButton = nullptr;
    QToolButton*                      m_lassoSubButton = nullptr;
    QAction*                          m_fillBoneCavityAct = nullptr;
    int                               m_fillBoneCavityLabel = -1;

    DentalPointSet                    m_dentalPointSet = DentalPointSet::None;
    BitePointSet                      m_bitePointSet = BitePointSet::None;
    vtkSmartPointer<vtkPolyData>      m_upperArchOriginalMesh;
    vtkSmartPointer<vtkPolyData>      m_lowerArchOriginalMesh;
    vtkSmartPointer<vtkPolyData>      m_upperArchMesh;
    vtkSmartPointer<vtkPolyData>      m_lowerArchMesh;
    vtkSmartPointer<vtkPolyData>      m_upperCompositeMesh;
    vtkSmartPointer<vtkPolyData>      m_lowerCompositeMesh;
    vtkSmartPointer<vtkMatrix4x4>     m_upperArchRegistrationMatrix;
    vtkSmartPointer<vtkMatrix4x4>     m_lowerArchRegistrationMatrix;
    QString                           m_upperRegistrationReport;
    QString                           m_lowerRegistrationReport;
    bool                              m_upperRegistrationCalculated = false;
    bool                              m_lowerRegistrationCalculated = false;
    QVector<QVector3D>                m_maxillaBonePoints;
    QVector<QVector3D>                m_upperArchPoints;
    QVector<QVector3D>                m_mandibleBonePoints;
    QVector<QVector3D>                m_lowerArchPoints;
    vtkSmartPointer<vtkPolyData>      m_biteScanMesh;
    vtkSmartPointer<vtkPolyData>      m_preBiteMandibleMeshForSplint;
    vtkSmartPointer<vtkPolyData>      m_orientedInitialMandibleMeshForSplint;
    vtkSmartPointer<vtkMatrix4x4>     m_biteLeFortRegistrationMatrix;
    vtkSmartPointer<vtkMatrix4x4>     m_biteMandibleRegistrationMatrix;
    QString                           m_biteLeFortRegistrationReport;
    QString                           m_biteMandibleRegistrationReport;
    bool                              m_biteLeFortRegistered = false;
    bool                              m_biteMandibleRegistered = false;
    QVector<QVector3D>                m_biteLeFortSegmentPoints;
    QVector<QVector3D>                m_biteUpperScanPoints;
    QVector<QVector3D>                m_biteMandibleSegmentPoints;
    QVector<QVector3D>                m_biteLowerScanPoints;

    bool             m_viewFullScreen = false;
    bool             m_compositeInProgress        = false;
    bool             m_autoCreateBothComposites   = false; // chains upper→lower→orientation
    QWidget*         m_fullScreenView = nullptr;
    QList<int>       m_splitterSizesBeforeFullScreen;
    Qt::WindowStates m_windowStateBeforeFullScreen = Qt::WindowNoState;

    // ── Phase 1: AppStateManager + promoted QAction* members ──────────────
    AppStateManager m_appState;
    QAction* m_importUpperAct  = nullptr;  // STL Sup
    QAction* m_importLowerAct  = nullptr;  // STL Inf
    QAction* m_modelBackAct    = nullptr;  // Volver
    QAction* m_maxPtsAct       = nullptr;  // Pts Max
    QAction* m_upperPtsAct     = nullptr;  // Pts Sup
    QAction* m_matchUpperAct   = nullptr;  // Reg Max
    QAction* m_mandPtsAct      = nullptr;  // Pts Mand
    QAction* m_lowerPtsAct     = nullptr;  // Pts Inf
    QAction* m_matchLowerAct   = nullptr;  // Reg Mand
    QAction* m_matchBothAct    = nullptr;  // Reg Ambos
    QAction* m_adjustArchAct   = nullptr;  // Ajustar STL
    QAction* m_acceptGizmoAct  = nullptr;  // Aceptar Ajuste
    QAction* m_compositeAct    = nullptr;  // Crear Comp.
    QAction* m_continueNoMatchAct = nullptr; // Continuar sin match
    QAction* m_exportAct       = nullptr;  // Export STL
    QAction* m_exportPackAct   = nullptr;  // Export Reg
    QAction* m_clearPtsAct     = nullptr;  // Limpiar Pts
    QAction* m_resetArchAct    = nullptr;  // Reset STL
    QAction* m_splitAct        = nullptr;  // Max / Mand.
    QAction* m_saveProjectAct  = nullptr;  // Guardar Proyecto
    QAction* m_exportLogAct    = nullptr;  // Exportar Log
    bool     m_dentalGizmoActive = false;
    int      m_airwayPointCaptureStep = 0;
    QVector3D m_airwayPoint1;
    QVector3D m_airwayPoint2;
    QAction* m_manualAirwayAct = nullptr;
    QDialog* m_airwayDialog = nullptr;
    QLabel*  m_airwayDialogLabel = nullptr;
    void showAirwayDialog(int step);
    void closeAirwayDialog();

    // ── Frankfurt plane (PLAN workspace) ─────────────────────────────────
    QVector<QVector3D>  m_frankfurtPoints;          // [0]=PorionD [1]=PorionI [2]=OrbitalD [3]=OrbitalI
    int                 m_frankfurtCapturingIdx = -1;
    QAction*            m_frankfortPorionDAct  = nullptr;   // index 0
    QAction*            m_frankfortPorionIAct  = nullptr;   // index 1
    QAction*            m_frankfortOrbitalDAct = nullptr;   // index 2
    QAction*            m_frankfortOrbitalIAct = nullptr;   // index 3
    QAction*            m_alignFrankfurtAct    = nullptr;
    QAction*            m_midlineGizmoAct      = nullptr;   // "Ajustar Media" — starts gizmo
    QAction*            m_midlineAcceptGizmoAct= nullptr;   // "Aceptar Gizmo" — stops gizmo
    QAction*            m_saveOrientationAct   = nullptr;   // "Guardar Orient." → navigate to osteotomy
    QAction*            m_exportOrientedAct    = nullptr;

    // ── Le Fort I osteotomy (OSTEOTOMIA workspace) ────────────────────────
    // Cephalometric landmarks: [0]=PirD [1]=PirI [2]=PilaxD [3]=PilaxI
    QVector<QVector3D>                 m_leFortPoints;
    std::array<bool, 4>                m_leFortPointSet {};
    int                                m_leFortCapturingIdx  = -1;
    int                                m_leFortWizardStep    = 0;
    int                                m_leFortTargetLabel   = 203; // kUpperCompositeLabel — mesh to cut
    QLabel*                            m_leFortCutLabel      = nullptr;  // status display
    QLabel*                            m_leFortWizardStatus  = nullptr;
    QLabel*                            m_leFortPointPrompt   = nullptr;
    std::array<QLabel*, 5>             m_leFortStepDots {};
    std::array<QLabel*, 5>             m_leFortStepTexts {};
    std::array<QPushButton*, 4>        m_leFortPointButtons {};
    QPushButton*                       m_leFortBackBtn       = nullptr;
    QPushButton*                       m_leFortNextBtn       = nullptr;
    QPushButton*                       m_leFortSelectTypeBtn = nullptr;
    QPushButton*                       m_leFortSelectBoneBtn = nullptr;
    QPushButton*                       m_leFortTranslateBtn  = nullptr;
    QPushButton*                       m_leFortRotateBtn     = nullptr;
    QPushButton*                       m_leFortResizeBtn     = nullptr;
    QDoubleSpinBox*                    m_leFortWidthSpin     = nullptr;
    QDoubleSpinBox*                    m_leFortThicknessSpin = nullptr;
    QDoubleSpinBox*                    m_leFortExtRightSpin  = nullptr;
    QDoubleSpinBox*                    m_leFortExtLeftSpin   = nullptr;
    QTableWidget*                      m_leFortFinalizeTable = nullptr;
    QRadioButton*                      m_leFortNextAnotherOsteotomy = nullptr;
    QRadioButton*                      m_leFortNextOcclusion = nullptr;
    QRadioButton*                      m_leFortNextReposition = nullptr;
    vtkSmartPointer<vtkPolyData>       m_leFortCranialMesh;   // cranial base after split
    vtkSmartPointer<vtkPolyData>       m_leFortSegmentMesh;   // Le Fort segment after split
    vtkSmartPointer<vtkPolyData>       m_leFortPlaneVisualMesh;
    // Current cut-plane state (initialized from landmarks; overridden by gizmo adjustment)
    QVector3D                          m_leFortPlaneNormal;
    QVector3D                          m_leFortPlaneCenter;
    QVector3D                          m_leFortGuideAxis;
    double                             m_leFortPlaneRadius = 0.0;
    double                             m_leFortGuideCoreLengthMm = 80.0;
    double                             m_leFortGuideWidthMm = 120.0;
    double                             m_leFortCutThicknessMm = 1.0;
    double                             m_leFortGuideExtensionRightMm = 20.0;
    double                             m_leFortGuideExtensionLeftMm = 20.0;
    QAction*                           m_leFortPirDACt       = nullptr;  // Piriforme Der
    QAction*                           m_leFortPirIAct       = nullptr;  // Piriforme Izq
    QAction*                           m_leFortPilaxDAct     = nullptr;  // Pilax Der
    QAction*                           m_leFortPilaxIAct     = nullptr;  // Pilax Izq
    QAction*                           m_leFortTargMaxAct    = nullptr;  // seleccionar maxilar
    QAction*                           m_leFortTargMandAct   = nullptr;  // seleccionar mandibula
    QAction*                           m_leFortAdjustPlaneAct= nullptr;  // Ajustar plano con gizmo
    QAction*                           m_leFortGuidePropsAct = nullptr;  // propiedades de guia
    QAction*                           m_leFortAcceptPlaneAct= nullptr;  // Aceptar ajuste del plano
    QAction*                           m_leFortSplitAct      = nullptr;  // Dividir
    QAction*                           m_leFortExportAct     = nullptr;  // Exportar

    // Bilateral sagittal split osteotomy planning guide (BSSO)
    QVector<QVector3D>                 m_bssoPoints;        // [0..2]=BSSO derecha, [3..5]=BSSO izquierda
    std::array<bool, 6>                m_bssoPointSet {};
    int                                m_bssoCapturingIdx = -1;
    QLabel*                            m_bssoStatusLabel = nullptr;
    vtkSmartPointer<vtkPolyData>       m_bssoGuideVisualMesh;
    vtkSmartPointer<vtkPolyData>       m_bssoDistalMesh;
    vtkSmartPointer<vtkPolyData>       m_bssoProximalMesh;
    vtkSmartPointer<vtkPolyData>       m_bssoRightProximalMesh;
    vtkSmartPointer<vtkPolyData>       m_bssoLeftProximalMesh;
    double                             m_bssoGuideLengthMm = 58.0;
    double                             m_bssoGuideHeightMm = 70.0;
    double                             m_bssoGuideWingMm = 32.0;
    double                             m_bssoCutThicknessMm = 1.0;
    QAction*                           m_bssoAutoGuideAct = nullptr;
    QAction*                           m_bssoLeftGuideAct = nullptr;
    QAction*                           m_bssoRamusRightAct = nullptr;
    QAction*                           m_bssoBodyRightAct = nullptr;
    QAction*                           m_bssoPlaneRightAct = nullptr;
    QAction*                           m_bssoRamusLeftAct = nullptr;
    QAction*                           m_bssoBodyLeftAct = nullptr;
    QAction*                           m_bssoPlaneLeftAct = nullptr;
    QAction*                           m_bssoAdjustGuideAct = nullptr;
    QAction*                           m_bssoGuidePropsAct = nullptr;
    QAction*                           m_bssoAcceptGuideAct = nullptr;
    bool                               m_bssoGuideReady = false;
    bool                               m_bssoActiveLeftSide = false;
    std::array<bool, 2>                m_bssoSideSplitDone {};

    // Chin osteotomy / genioplasty planning guide
    QVector<QVector3D>                 m_genioPoints;       // [0]=apical R, [1]=basal R, [2]=apical L, [3]=basal L
    std::array<bool, 4>                m_genioPointSet {};
    int                                m_genioCapturingIdx = -1;
    QLabel*                            m_genioStatusLabel = nullptr;
    vtkSmartPointer<vtkPolyData>       m_genioPlaneVisualMesh;
    vtkSmartPointer<vtkPolyData>       m_genioBodyMesh;
    vtkSmartPointer<vtkPolyData>       m_genioSegmentMesh;
    vtkSmartPointer<vtkPolyData>       m_intermediateSplintMesh;
    vtkSmartPointer<vtkPolyData>       m_finalSplintMesh;
    QVector3D                          m_genioPlaneNormal;
    QVector3D                          m_genioPlaneCenter;
    QVector3D                          m_genioGuideAxis;
    QVector3D                          m_genioGuideDepthAxis;
    double                             m_genioGuideLengthMm = 70.0;
    double                             m_genioGuideWidthMm = 24.0;
    double                             m_genioCutThicknessMm = 1.0;
    QAction*                           m_genioApicalRightAct = nullptr;
    QAction*                           m_genioBasalRightAct = nullptr;
    QAction*                           m_genioApicalLeftAct = nullptr;
    QAction*                           m_genioBasalLeftAct = nullptr;
    QAction*                           m_genioAdjustPlaneAct = nullptr;
    QAction*                           m_genioAcceptPlaneAct = nullptr;

    // Bite registration workspace
    QAction*                           m_importBiteScanAct = nullptr;
    QAction*                           m_biteLeFortPtsAct = nullptr;
    QAction*                           m_biteUpperScanPtsAct = nullptr;
    QAction*                           m_biteRegisterLeFortAct = nullptr;
    QAction*                           m_biteMandiblePtsAct = nullptr;
    QAction*                           m_biteLowerScanPtsAct = nullptr;
    QAction*                           m_biteRegisterMandibleAct = nullptr;
    QAction*                           m_biteAdjustGizmoAct = nullptr;
    QAction*                           m_biteAcceptGizmoAct = nullptr;
    QAction*                           m_biteClearPtsAct = nullptr;
    int                                m_biteGizmoTargetLabel = -1;
    bool                               m_biteGizmoActive = false;

    // Reposition workspace
    QListWidget*                       m_repositionObjectList = nullptr;
    QDoubleSpinBox*                    m_repositionStepSpin = nullptr;
    QDoubleSpinBox*                    m_repositionRotStepSpin = nullptr;
    QLabel*                            m_repositionLateralValue = nullptr;
    QLabel*                            m_repositionAntPostValue = nullptr;
    QLabel*                            m_repositionVerticalValue = nullptr;
    QLabel*                            m_repositionPivotValue = nullptr;
    QPushButton*                       m_repositionPivotButton = nullptr;
    QLabel*                            m_repositionRotXValue = nullptr;
    QLabel*                            m_repositionRotYValue = nullptr;
    QLabel*                            m_repositionRotZValue = nullptr;
    QAction*                           m_repositionGizmoAct = nullptr;
    QAction*                           m_repositionAcceptGizmoAct = nullptr;
    QAction*                           m_repositionResetAct = nullptr;
    QAction*                           m_repositionHomeAct = nullptr;
    QAction*                           m_repositionFixedViewAct = nullptr;
    int                                m_repositionTargetLabel = -1;
    QList<int>                         m_repositionGizmoGroupLabels;
    std::map<int, vtkSmartPointer<vtkPolyData>> m_repositionGizmoGroupMeshes;
    bool                               m_repositionPickingPivot = false;
    std::map<int, vtkSmartPointer<vtkPolyData>> m_repositionOriginalMeshes;
    QMap<int, QVector3D>               m_repositionTranslationMm;
    QMap<int, QVector3D>               m_repositionRotationDeg;
    QMap<int, QVector3D>               m_repositionPivotWorld;
    ProjMandibleMovement              m_mandibleMovement;
    QVector<double>                   m_mandibleMovementResetMatrix;
    QWidget*                          m_mandibleMovementPanel = nullptr;
    QLabel*                           m_mandibleMovementReference = nullptr;
    QLabel*                           m_mandibleMovementStatus = nullptr;
    QTableWidget*                     m_mandibleMovementTable = nullptr;

    // ── Phase 5: RegistrationResult metrics ───────────────────────────────
    QAction*                           m_splintIntermediateAct = nullptr;
    QAction*                           m_splintFinalAct = nullptr;
    QAction*                           m_splintExportAct = nullptr;
    QComboBox*                         m_splintDesignCombo = nullptr;
    QComboBox*                         m_splintUpperPartCombo = nullptr;
    QComboBox*                         m_splintLowerPartCombo = nullptr;
    QPushButton*                       m_splintUpperVestibularButton = nullptr;
    QPushButton*                       m_splintUpperPalatalButton = nullptr;
    QPushButton*                       m_splintLowerVestibularButton = nullptr;
    QPushButton*                       m_splintLowerLingualButton = nullptr;
    QPushButton*                       m_splintClearPointsButton = nullptr;
    QLabel*                            m_splintPointStatusLabel = nullptr;
    QCheckBox*                         m_splintShowThicknessCheck = nullptr;
    QCheckBox*                         m_splintMaxillaImpressionCheck = nullptr;
    QCheckBox*                         m_splintMandibleImpressionCheck = nullptr;
    QCheckBox*                         m_splintMaxillaUndercutCheck = nullptr;
    QCheckBox*                         m_splintMandibleUndercutCheck = nullptr;
    QDoubleSpinBox*                    m_splintMinThicknessSpin = nullptr;
    QDoubleSpinBox*                    m_splintMaxThicknessSpin = nullptr;
    QDoubleSpinBox*                    m_splintOffsetSpin = nullptr;
    QDoubleSpinBox*                    m_splintFilletSpin = nullptr;
    QDoubleSpinBox*                    m_splintBracketsOffsetSpin = nullptr;
    QDoubleSpinBox*                    m_splintHoleDiameterSpin = nullptr;
    QDoubleSpinBox*                    m_splintHoleTopHeightSpin = nullptr;
    QDoubleSpinBox*                    m_splintHoleBottomHeightSpin = nullptr;
    QDoubleSpinBox*                    m_splintThicknessSpin = nullptr;
    QPushButton*                       m_splintAdjustButton = nullptr;
    QPushButton*                       m_splintAcceptAdjustButton = nullptr;
    QPushButton*                       m_splintCreateButton = nullptr;
    bool                               m_splintInProgress = false;
    int                                m_splintGizmoTargetLabel = -1;
    bool                               m_splintGizmoActive = false;
    SplintPointSet                     m_splintPointSet = SplintPointSet::None;
    QVector<QVector3D>                 m_splintUpperVestibularPoints;
    QVector<QVector3D>                 m_splintUpperPalatalPoints;
    QVector<QVector3D>                 m_splintLowerVestibularPoints;
    QVector<QVector3D>                 m_splintLowerLingualPoints;

    RegistrationResult m_upperRegResult;
    RegistrationResult m_lowerRegResult;

    // ── Splint workspace: named height-map designs ────────────────────────
    std::vector<SplintDesign> m_splintDesigns;
    int m_activeSplintDesign = 0;

    // Height-map method (implemented in MainWindowSplint.cpp)
    QWidget* buildSplintMethodPanel(QWidget* parent, QWidget* classicPanel);
    void connectSplintHeightmapViews();
    bool splintHeightmapMethodActive() const;
    SplintDesign* activeSplintDesign();
    const SplintDesign* activeSplintDesign() const;
    void ensureSplintDesigns();
    void refreshSplintDesignPanel();
    void updateSplintPanelState();
    void selectSplintDesign(int index);
    void selectSplintDesignByLabel(int label);
    void addSplintDesign();
    void copySplintDesign();
    void renameSplintDesign();
    void deleteSplintDesign();
    void onSplintSourcesChanged();
    void onSplintParamsChanged();
    void setSplintPointGroup(int group);
    void clearSplintDesignPoints(int group);
    void onSplintEditablePointAdded(int group, double x, double y, double z);
    void onSplintEditablePointMoved(int group, int index, double x, double y, double z);
    void onSplintEditablePointDragFinished(int group, int index);
    void onSplintEditablePointRemoved(int group, int index);
    void rebuildSplintEditablePoints();
    bool confirmSplintContourLoss();
    void discardSplintEditedContour(const QString& reason);
    void requestSplintPreview();
    void requestRefinedSplintPreview();
    void onSplintPreviewReady(quint64 generation, const SplintHeightmapResult& result);
    void onSplintPreviewFailed(quint64 generation, const QString& error);
    void clearSplintPreviewDisplay();
    void updateSplintPreviewMesh();
    void updateSplintContourOverlay();
    void applySplintOcclusalCameras(bool force);
    void setSplintContourEditing(bool editing);
    void resetSplintContour();
    void onSplintPlaneDragStarted(double x, double y, double z, Qt::KeyboardModifiers modifiers);
    void onSplintPlaneDragMoved(double x, double y, double z, double deltaY, Qt::KeyboardModifiers modifiers);
    void onSplintPlaneDragFinished(double x, double y, double z);
    vtkSmartPointer<vtkPolyData> splintSourceMesh(int choice) const;
    SplintHeightmapInputs splintInputsForDesign(const SplintDesign& design) const;
    void syncSplintHeightmapView();
    void createHeightmapSplint();
    void onHeightmapSplintCreated(int label, const QString& designName, const SplintHeightmapResult& result);
    void exportHeightmapSplintStl();
    void loadSplintTestStl();
    void setSplintTestSources(vtkSmartPointer<vtkPolyData> upper, vtkSmartPointer<vtkPolyData> lower);
    void exportSplintDesignPoints();
    void exportSplintReport();
    void restoreSplintDesigns(const ProjectState& state);

    QComboBox*              m_splintMethodCombo = nullptr;
    QStackedWidget*         m_splintMethodStack = nullptr;
    SplintDesignPanel*      m_splintDesignPanel = nullptr;
    SplintPreviewScheduler* m_splintPreview = nullptr;
    QTimer*                 m_splintRefineTimer = nullptr;
    quint64                 m_splintCoarseGeneration = 0;
    SplintHeightmapResult   m_splintPreviewResult;
    bool                    m_splintPreviewValid = false;
    bool                    m_splintPreviewRefined = false;
    SplintOcclusalFrame     m_splintCameraFrame;
    bool                    m_splintCameraFrameSet = false;
    int                     m_splintPointGroup = -1;     // 0 maxilla, 1 mandible
    bool                    m_splintContourEditing = false;
    double                  m_splintInfluencePercent = 20.0;
    int                     m_splintDragContour = -1;
    int                     m_splintDragVertex = -1;
    SplintContourUV         m_splintDragStartContour;
    SplintPointUV           m_splintDragStartUV{};
    bool                    m_splintInfluenceDrag = false;
    bool                    m_splintPointDragStarted = false;
    bool                    m_splintPointDragRejected = false;
    SplintDesign            m_splintPointDragSnapshot;
    bool                    m_splintHeightmapBuildInProgress = false;
    QString                 m_splintLastReport;
    vtkSmartPointer<vtkPolyData> m_splintTestUpperMesh;
    vtkSmartPointer<vtkPolyData> m_splintTestLowerMesh;
    struct SplintSourceCacheEntry
    {
        QString fingerprint;
        vtkSmartPointer<vtkPolyData> mesh;
    };
    // Private copies of the source meshes, stable while unchanged so the
    // preview cache is reused and the worker never sees in-place edits.
    mutable std::map<int, SplintSourceCacheEntry> m_splintSourceCache;
    // Dialog hooks, replaced by the workspace tests.
    std::function<bool(const QString&)> m_splintConfirm;
    std::function<std::optional<QString>(const QString&, const QString&)> m_splintAskName;

    // ── Composite with cutting block (implemented in MainWindowComposite.cpp) ──
    // Registrar → Ajuste fino → Bloque → Revisar: a composite is only stored
    // after the user accepts its review.
    enum class CompositeStage { None, Block, Computing, Review };
    QWidget* buildCompositeBlockPanel(QWidget* parent);
    void startCompositeBlockStage(int step);
    void showCompositeStage();
    void updateCompositeStagePanel();
    void updateCompositeBlockDisplay();
    void updateCompositeReviewDisplay();
    void onCompositeBlockSizeChanged();
    void startCompositeBlockGizmo();
    void acceptCompositeBlockGizmo();
    void onCompositeBlockGizmoUpdated(int meshLabel, vtkSmartPointer<vtkPolyData> newMesh);
    void resetCompositeBlock();
    void calculateBlockComposite();
    void onBlockCompositeCalculated(int step, const CompositeBlockResult& result);
    void backToCompositeBlockStage();
    void acceptCompositeReview();
    void cancelCompositeStage();
    void showCompositeSlicesInCt();
    bool compositeBlockMethodActive() const;
    CompositeCutBlock& compositeBlockForStep(int step);
    vtkSmartPointer<vtkPolyData> compositeDentalForStep(int step, QString* error) const;
    void refreshRegisteredArchContours();
    void writeCompositeStl(const QVector<vtkSmartPointer<vtkPolyData>>& parts, vtkSmartPointer<vtkPolyData> mergedMesh,
                           const QString& path);
    QJsonObject compositeBlocksJson() const;
    void restoreCompositeBlocks(const ProjectState& state);

    CompositeStage               m_compositeStage = CompositeStage::None;
    int                          m_compositeStageStep = 0;
    CompositeCutBlock            m_upperCompositeBlock;
    CompositeCutBlock            m_lowerCompositeBlock;
    CompositeCutBlock            m_compositeBlockAtGizmoStart;
    bool                         m_compositeBlockGizmoActive = false;
    vtkSmartPointer<vtkPolyData> m_compositeReviewMesh;
    QString                      m_compositeReviewReport;
    QWidget*                     m_compositeBlockPanel = nullptr;
    QWidget*                     m_compositeBlockControls = nullptr;
    QWidget*                     m_compositeReviewControls = nullptr;
    QLabel*                      m_compositeStageLabel = nullptr;
    QComboBox*                   m_compositeMethodCombo = nullptr;
    QDoubleSpinBox*              m_compositeWidthSpin = nullptr;
    QDoubleSpinBox*              m_compositeLengthSpin = nullptr;
    QDoubleSpinBox*              m_compositeThicknessSpin = nullptr;
    QPushButton*                 m_compositeGizmoButton = nullptr;
    QPushButton*                 m_compositeAcceptGizmoButton = nullptr;
    QPushButton*                 m_compositeResetButton = nullptr;
    QPushButton*                 m_compositeCalculateButton = nullptr;
    vtkSmartPointer<vtkPolyData> m_contourUpperArchMesh;
    vtkSmartPointer<vtkPolyData> m_contourLowerArchMesh;
    vtkMTimeType                 m_contourUpperArchTime = 0;
    vtkMTimeType                 m_contourLowerArchTime = 0;
};
