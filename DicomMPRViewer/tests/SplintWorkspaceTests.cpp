#include "MainWindow.h"
#include "MPRView.h"
#include "Mesh3DView.h"
#include "MeshRepairCore.h"
#include "ObjectLabels.h"
#include "ProjectSerializer.h"
#include "SplintContourEditCore.h"
#include "SplintDesignPanel.h"
#include "SplintPreviewScheduler.h"
#include "SplintTestGeometry.h"
#include "OsteotomyWizardPanel.h"

#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QListWidget>
#include <QScrollArea>
#include <QToolButton>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFont>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QVTKOpenGLNativeWidget.h>

#include <QDoubleSpinBox>
#include <QMessageBox>
#include <QStackedWidget>
#include <QTabWidget>
#include <QStatusBar>

#include <vtkAppendPolyData.h>
#include <vtkClipPolyData.h>
#include <vtkPlaneSource.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>
#include <vtkTriangleFilter.h>
#include <vtkFeatureEdges.h>
#include <vtkPolyDataConnectivityFilter.h>
#include <vtkImageData.h>
#include <vtkImplicitPolyDataDistance.h>
#include <vtkMatrix4x4.h>
#include <vtkPlane.h>
#include <vtkPointData.h>

#include <functional>
#include <iostream>
#include <optional>

using namespace splinttest;

namespace
{
class HideDialogs : public QObject
{
    bool eventFilter(QObject* object, QEvent* event) override
    {
        if (event->type() == QEvent::Polish)
            if (auto* dialog = qobject_cast<QDialog*>(object))
                dialog->setAttribute(Qt::WA_DontShowOnScreen);
        return false;
    }
};

void settle()
{
    for (int i = 0; i < 5; ++i)
        QApplication::processEvents();
}

bool waitFor(const std::function<bool()>& condition, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        if (timer.elapsed() > timeoutMs)
            return false;
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return true;
}

bool isClosed(vtkPolyData* mesh)
{
    if (!mesh || mesh->GetNumberOfPolys() == 0)
        return false;
    auto edges = vtkSmartPointer<vtkFeatureEdges>::New();
    edges->SetInputData(mesh);
    edges->BoundaryEdgesOn();
    edges->NonManifoldEdgesOn();
    edges->FeatureEdgesOff();
    edges->ManifoldEdgesOff();
    edges->Update();
    return edges->GetOutput()->GetNumberOfCells() == 0;
}
} // namespace

class SplintWorkspaceTests
{
public:
    static void run(const QString& artifactsDir)
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1700, 950);
        window.show();
        settle();

        // Ribbon: MEDIDAS now lives inside ARCHIVO; planning remains in ORTOGNÁTICA.
        QStringList visibleTabs;
        for (auto* tab : window.findChildren<QToolButton*>(QStringLiteral("MT")))
            if (tab->isVisibleTo(&window))
                visibleTabs << tab->text();
        require(visibleTabs == QStringList({QStringLiteral("ARCHIVO"), QString::fromUtf8("ORTOGN\xc3\x81TICA")}),
                "ribbon tabs are not ARCHIVO, ORTOGNATICA: " + visibleTabs.join(", ").toStdString());
        require(!window.m_orthoStepPanel->isVisibleTo(&window), "step rail shown outside ORTOGNATICA");
        window.m_orthoTab->click();
        settle();
        require(window.m_orthoStepPanel->isVisibleTo(&window) && window.m_orthoTab->isChecked() &&
                    window.m_orthoStep == 0 && window.m_orthoStepButtons[0]->isChecked(),
                "ORTOGNATICA did not open the step rail on segmentation");
        window.m_orthoNextButton->click();
        settle();
        require(window.m_orthoStep == 1 && window.m_viewModeStack->currentIndex() == 1 &&
                    window.m_orthoStepButtons[1]->isChecked() && window.m_orthoPrevButton->isEnabled(),
                "Siguiente did not open MODELOS");
        require(window.m_modelMatchView->standardViewIndex() == 0, "MODELOS does not open in the frontal view");
        window.m_orthoStepButtons[6]->click();
        settle();
        require(window.m_orthoStep == 6 && window.m_viewModeStack->currentIndex() == 6 &&
                    window.m_orthoNextButton->isEnabled(),
                "the step rail did not jump to FERULA");
        require(window.m_splintView->standardViewIndex() == 0, "FERULA does not open in the frontal view");
        // GUIAS is the last step of the bar.
        window.m_orthoStepButtons[7]->click();
        settle();
        require(window.m_orthoStep == 7 && window.m_viewModeStack->currentIndex() == 7 &&
                    !window.m_orthoNextButton->isEnabled(),
                "GUIAS is not the last step of the bar");
        window.m_orthoStepButtons[6]->click();
        settle();
        for (auto* tab : window.findChildren<QToolButton*>(QStringLiteral("MT")))
            if (tab->text() == QStringLiteral("ARCHIVO"))
                tab->click();
        settle();
        require(!window.m_orthoStepPanel->isVisibleTo(&window) && window.m_viewModeStack->currentIndex() == 0 &&
                    !window.m_orthoTab->isChecked(),
                "ARCHIVO did not leave the ORTOGNATICA steps");
        window.m_orthoTab->click();
        settle();
        require(window.m_orthoStep == 6 && window.m_viewModeStack->currentIndex() == 6,
                "ORTOGNATICA did not return to the last step");

        int confirmations = 0;
        bool acceptConfirmations = true;
        window.m_splintConfirm = [&](const QString&) {
            ++confirmations;
            return acceptConfirmations;
        };
        QStringList names = {QStringLiteral("Prueba"), QStringLiteral("Renombrada")};
        window.m_splintAskName = [&](const QString&, const QString&) -> std::optional<QString> {
            if (names.isEmpty())
                return std::nullopt;
            return names.takeFirst();
        };

        require(window.splintHeightmapMethodActive(), "the height-map method is not the default");
        window.setSplintWorkspace(true);
        settle();
        // Only the height-map method, and its panel fits the column (nothing cut off, no horizontal scroll).
        require(!window.m_splintMethodCombo->isVisibleTo(&window), "the classic splint method is still offered");
        auto* splintScroll = window.findChild<QScrollArea*>(QStringLiteral("SplintDesignScroll"));
        require(splintScroll != nullptr, "splint panel scroll area missing");
        require(window.m_objectTable->contextMenuPolicy() == Qt::CustomContextMenu,
                "the object list has no right-click actions");
        require(window.m_splintDesignPanel->minimumSizeHint().width() <= splintScroll->viewport()->width(),
                "the splint panel is wider than its column: " +
                    std::to_string(window.m_splintDesignPanel->minimumSizeHint().width()) + " > " +
                    std::to_string(splintScroll->viewport()->width()));

        // ── Test sources and guide points ─────────────────────────────────
        Scene scene = makeScene();
        window.setSplintTestSources(scene.upper, scene.lower);
        require(window.activeSplintDesign()->upperSource == kSplintTestUpperChoice &&
                    window.activeSplintDesign()->lowerSource == kSplintTestLowerChoice,
                "test STL were not selected as sources");
        for (const SplintPoint3& p : guidePoints(5.0))
            window.onSplintEditablePointAdded(0, p[0], p[1], p[2]);
        for (const SplintPoint3& p : guidePoints(-5.0))
            window.onSplintEditablePointAdded(1, p[0], p[1], p[2]);
        require(window.activeSplintDesign()->upperPoints.size() == 3 &&
                    window.activeSplintDesign()->lowerPoints.size() == 3,
                "guide points were not stored in the design");

        const auto settled = [&] {
            return window.m_splintPreviewValid && window.m_splintPreviewRefined && !window.m_splintPreview->isBusy() &&
                   !window.m_splintRefineTimer->isActive();
        };
        require(waitFor(settled, 90000), "refined preview never arrived");
        const SplintHeightmapResult preview = window.m_splintPreviewResult;
        require(preview.ok && preview.mesh && preview.mesh->GetNumberOfPolys() > 0 && preview.contoursUV.size() == 1,
                "preview has no splint or contour");
        require(preview.report.contains(QStringLiteral("Grosor mínimo")), "refined preview has no thickness");
        require(window.m_splintUpperView->hasOverlay(kSplintContourOverlayKey) &&
                    window.m_splintLowerView->hasOverlay(kSplintContourOverlayKey),
                "contour is not shown in both occlusal views");
        require(window.m_splintView->meshData(kSplintPreviewActorKey) != nullptr, "preview mesh not shown in 3D");
        std::cout << "  preview: " << preview.mesh->GetNumberOfPolys() << " triangles, min thickness "
                  << preview.minThicknessMm << " mm\n";

        // ── Contour editing ───────────────────────────────────────────────
        window.setSplintContourEditing(true);
        require(window.m_splintContourEditing, "contour editing did not start");
        const SplintOcclusalFrame frame = preview.frame;
        const SplintContourUV& automatic = preview.contoursUV.front();
        SplintPointUV grab = automatic.front();
        for (const SplintPointUV& p : automatic)
            if (p[1] > grab[1])
                grab = p;
        const SplintPoint3 from = frame.ToWorld(grab[0], grab[1], 0.0);
        const SplintPoint3 to = frame.ToWorld(grab[0], grab[1] + 2.0, 0.0);
        window.onSplintPlaneDragStarted(from[0], from[1], from[2], Qt::NoModifier);
        require(window.m_splintDragContour >= 0, "contour drag did not grab the contour");
        window.onSplintPlaneDragMoved(to[0], to[1], to[2], 0.0, Qt::NoModifier);
        window.onSplintPlaneDragFinished(to[0], to[1], to[2]);
        const auto& edited = window.activeSplintDesign()->editedContourUV;
        require(!edited.empty(), "drag did not create an edited contour");
        bool movedVertex = false;
        for (const SplintPointUV& p : edited.front())
            movedVertex = movedVertex || (std::abs(p[0] - grab[0]) < 0.05 && std::abs(p[1] - grab[1] - 2.0) < 0.05);
        require(movedVertex, "grabbed contour vertex did not follow the drag");
        require(SplintHeightmapGenerator::ContourArea(edited.front()) > SplintHeightmapGenerator::ContourArea(automatic),
                "pulling the contour outward did not enlarge it");
        require(waitFor([&] {
                    return settled() && window.m_splintPreviewResult.contoursUV == window.activeSplintDesign()->editedContourUV;
                }, 90000),
                "preview did not use the edited contour");

        const double influence = window.m_splintInfluencePercent;
        window.onSplintPlaneDragStarted(from[0], from[1], from[2], Qt::AltModifier);
        window.onSplintPlaneDragMoved(from[0], from[1], from[2], -50.0, Qt::AltModifier);
        window.onSplintPlaneDragFinished(from[0], from[1], from[2]);
        require(window.m_splintInfluencePercent > influence + 4.0, "Alt-drag did not grow the influence radius");

        // ── Moving a point: confirmed discards the contour, rejected restores the point ─
        SplintPoint3 p0 = window.activeSplintDesign()->upperPoints[0];
        confirmations = 0;
        window.onSplintEditablePointMoved(0, 0, p0[0], p0[1], p0[2] + 0.5);
        window.onSplintEditablePointDragFinished(0, 0);
        require(confirmations == 1 && window.activeSplintDesign()->editedContourUV.empty() && !window.m_splintContourEditing,
                "confirmed point move did not discard the edited contour");
        require(waitFor(settled, 90000), "preview after moving a point never arrived");

        window.activeSplintDesign()->editedContourUV = window.m_splintPreviewResult.contoursUV;
        window.activeSplintDesign()->editedContourFrame = window.m_splintPreviewResult.frame;
        const auto pointsBefore = window.activeSplintDesign()->upperPoints;
        acceptConfirmations = false;
        p0 = pointsBefore[0];
        window.onSplintEditablePointMoved(0, 0, p0[0], p0[1], p0[2] + 3.0);
        window.onSplintEditablePointDragFinished(0, 0);
        require(window.activeSplintDesign()->upperPoints == pointsBefore && !window.activeSplintDesign()->editedContourUV.empty(),
                "rejected point move changed the design");
        acceptConfirmations = true;
        window.resetSplintContour();
        require(waitFor(settled, 90000), "preview after resetting the contour never arrived");

        // ── Create the Intermedia splint ──────────────────────────────────
        require(window.activeSplintDesign()->label == kIntermediateSplintLabel, "Intermedia is not the active design");
        window.createHeightmapSplint();
        require(waitFor([&] { return !window.m_splintHeightmapBuildInProgress; }, 120000), "splint creation never finished");
        require(window.objectEntryExists(kIntermediateSplintLabel), "created splint is not in the object table");
        require(isClosed(window.repositionMeshForLabel(kIntermediateSplintLabel)), "created splint is not a closed mesh");
        require(window.m_intermediateSplintMesh != nullptr, "Intermedia splint mesh not stored");

        // ── Extras: bevel, wire hole, bracket margins, repair, outdated splint ─
        {
            SplintDesign* design = window.activeSplintDesign();
            const auto c5 = toothCenter(5);
            const double len5 = std::hypot(c5[0], c5[1]);
            const auto radial = [&](double offset, double z) {
                return SplintPoint3{c5[0] + c5[0] / len5 * offset, c5[1] + c5[1] / len5 * offset, z};
            };
            window.setSplintTool(MainWindow::SplintToolBevel);
            require(window.m_splintTool == MainWindow::SplintToolBevel && window.m_splintPointGroup < 0,
                    "bevel tool not active");
            SplintPoint3 b0 = radial(0.5, 5.0);
            SplintPoint3 b1 = radial(2.0, -5.0);
            window.onSplintEditablePointAdded(2, b0[0], b0[1], b0[2]);
            require(!design->extras.bevel && window.m_splintBevelPendingPoint.has_value(), "first bevel point not pending");
            window.onSplintEditablePointAdded(2, b1[0], b1[1], b1[2]);
            require(design->extras.bevel.has_value() && !window.m_splintBevelPendingPoint, "bevel not stored");
            b1 = radial(2.5, -5.0);
            window.onSplintEditablePointMoved(2, 1, b1[0], b1[1], b1[2]);
            require(design->extras.bevel->second == b1, "dragging a bevel point did not move it");
            require(window.m_splintView->hasOverlay(kSplintBevelOverlayKey), "bevel line not shown");

            window.setSplintTool(MainWindow::SplintToolHoles);
            require(window.m_splintTool == MainWindow::SplintToolHoles, "hole tool not active");
            const auto c3 = toothCenter(3);
            window.onSplintEditablePointAdded(3, c3[0], c3[1], 0.9);
            require(design->extras.wireHoles.size() == 1 && design->extras.wireHoles[0].axis[2] > 0.9,
                    "wire hole not placed perpendicular to the occlusal surface");
            require(window.m_splintView->meshData(kSplintWireHolesActorKey) != nullptr, "wire hole cylinder not shown");

            window.setSplintTool(MainWindow::SplintToolBrackets);
            const auto c8 = toothCenter(8);
            window.onSplintSurfaceBrushed(c8[0], c8[1], 1.0, Qt::NoModifier);
            window.onSplintSurfaceBrushed(c8[0] + 0.2, c8[1], 1.0, Qt::NoModifier);
            require(design->extras.bracketMarks.size() == 1, "brush stroke did not mark exactly once");
            window.onSplintSurfaceBrushed(c8[0], c8[1], 1.0, Qt::ControlModifier);
            require(design->extras.bracketMarks.empty(), "Ctrl brush did not unmark");
            window.onSplintSurfaceBrushed(c8[0], c8[1], 1.0, Qt::NoModifier);
            window.onSplintSurfaceBrushFinished();
            require(window.m_splintView->meshData(kSplintBracketMarksActorKey) != nullptr, "bracket marks not shown");
            window.setSplintTool(MainWindow::SplintToolNone);

            require(waitFor([&] {
                        return settled() && window.m_splintPreviewResult.report.contains(QStringLiteral("Bisel aplicado")) &&
                               window.m_splintPreviewResult.report.contains(QStringLiteral("agujero")) &&
                               window.m_splintPreviewResult.report.contains(QStringLiteral("brackets"));
                    }, 90000),
                    "preview did not apply the extras");

            window.createHeightmapSplint();
            require(waitFor([&] { return !window.m_splintHeightmapBuildInProgress; }, 120000), "splint with extras never finished");
            require(isClosed(window.repositionMeshForLabel(kIntermediateSplintLabel)), "splint with extras is not closed");
            require(window.m_splintLastReport.contains(QStringLiteral("Reparación STL")), "created splint was not validated");
            require(!design->createdSourceKey.isEmpty() && design->createdSourceKey == window.splintSourceKey(*design),
                    "created splint has no source signature");

            const auto staleShown = [&] {
                for (QLabel* label : window.m_splintDesignPanel->findChildren<QLabel*>())
                    if (label->text().contains(QStringLiteral("desactualizada")) && label->isVisibleTo(window.m_splintDesignPanel))
                        return true;
                return false;
            };
            require(!staleShown(), "fresh splint marked outdated");
            auto movedLower = vtkSmartPointer<vtkPolyData>::New();
            movedLower->DeepCopy(scene.lower);
            for (vtkIdType i = 0; i < movedLower->GetNumberOfPoints(); ++i) {
                double p[3];
                movedLower->GetPoint(i, p);
                movedLower->GetPoints()->SetPoint(i, p[0], p[1] - 1.0, p[2]);
            }
            window.setSplintTestSources(scene.upper, movedLower);
            require(staleShown(), "splint not marked outdated after the sources moved");
            window.setSplintTestSources(scene.upper, scene.lower);
            require(!staleShown(), "outdated warning stayed after restoring the sources");
            require(waitFor(settled, 90000), "preview after restoring sources never arrived");
        }

        // ── Named designs ─────────────────────────────────────────────────
        window.addSplintDesign();
        require(window.m_splintDesigns.size() == 3 && window.activeSplintDesign()->name == QStringLiteral("Prueba") &&
                    window.activeSplintDesign()->label == 230,
                "new design not created");
        window.copySplintDesign();
        require(window.m_splintDesigns.size() == 4 && window.activeSplintDesign()->name == QStringLiteral("Prueba (copia)"),
                "design copy not created");
        window.renameSplintDesign();
        require(window.activeSplintDesign()->name == QStringLiteral("Renombrada"), "design not renamed");
        confirmations = 0;
        window.deleteSplintDesign();
        require(confirmations == 1 && window.m_splintDesigns.size() == 3 &&
                    window.activeSplintDesign()->name == QStringLiteral("Prueba"),
                "design not deleted");
        window.selectSplintDesign(0);
        require(window.activeSplintDesign()->upperPoints.size() == 3, "switching designs lost the Intermedia points");
        require(waitFor(settled, 90000), "preview after switching designs never arrived");

        // ── Screenshot with the thickness map ─────────────────────────────
        const auto checks = window.m_splintDesignPanel->findChildren<QCheckBox*>();
        for (QCheckBox* check : checks)
            if (check->text() == QStringLiteral("Mostrar grosor"))
                check->setChecked(true);
        settle();
        const QString screenshot = QDir(artifactsDir).filePath(QStringLiteral("splint-workspace.png"));
        QDir().mkpath(artifactsDir);
        require(composeWorkspace(window).save(screenshot), "workspace screenshot not written");
        std::cout << "  screenshot: " << screenshot.toStdString() << '\n';

        // ── Save and reopen ───────────────────────────────────────────────
        QTemporaryDir dir;
        require(dir.isValid(), "no temporary directory");
        const QString path = dir.filePath(QStringLiteral("ferula.maxilloproject"));
        QString error;
        require(ProjectSerializer::save(path, window.collectProjectState(), &error), "save failed: " + error.toStdString());
        ProjectState loaded;
        require(ProjectSerializer::load(path, loaded, &error), "load failed: " + error.toStdString());

        MainWindow reopened;
        reopened.setAttribute(Qt::WA_DontShowOnScreen);
        reopened.resize(1700, 950);
        reopened.show();
        settle();
        reopened.applyProjectState(loaded);
        settle();
        require(reopened.m_splintDesigns.size() == window.m_splintDesigns.size(), "designs lost after reopening");
        for (size_t i = 0; i < window.m_splintDesigns.size(); ++i) {
            const SplintDesign& a = window.m_splintDesigns[i];
            const SplintDesign& b = reopened.m_splintDesigns[i];
            require(a.id == b.id && a.name == b.name && a.label == b.label && a.upperPoints == b.upperPoints &&
                        a.lowerPoints == b.lowerPoints && a.params.edgeOffsetMm == b.params.edgeOffsetMm &&
                        a.extras.bevel.has_value() == b.extras.bevel.has_value() &&
                        a.extras.wireHoles.size() == b.extras.wireHoles.size() &&
                        a.extras.bracketMarks.size() == b.extras.bracketMarks.size() &&
                        a.createdSourceKey == b.createdSourceKey,
                    "design changed after reopening: " + a.name.toStdString());
        }
        require(reopened.objectEntryExists(kIntermediateSplintLabel) && reopened.m_intermediateSplintMesh != nullptr,
                "created splint lost after reopening");
        std::cout << "Splint workspace: preview, contour editing, creation, designs and project OK\n";
    }

    // MODELOS composite: block → review (mandatory), link through cuts, splint source, project.
    static void runModelGuide(const QString& artifactsDir)
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1366, 768);
        window.show();
        window.m_mesh3DView->addMesh(5, boxMesh({-35, 35, -10, 35, 4, 30}, false, false), "Maxilar");
        window.setModelsWorkspace(true);
        window.updateModelWorkflowUi();
        require(window.m_modelGuideSteps[0]->property("current").toBool(), "guide did not start at STL import");
        require(!window.m_matchUpperAct->isEnabled(), "registration enabled before STL import");
        window.m_upperArchMesh = upperTeeth({});
        window.syncModelViews();
        window.updateModelWorkflowUi();
        window.setDentalPointCapture(MainWindow::DentalPointSet::MaxillaBone);
        window.onDentalPointPicked(5, 0.0, 0, 0);
        require(!window.m_matchUpperAct->isEnabled(), "unpaired points allowed registration");
        require(window.m_dentalPointSet == MainWindow::DentalPointSet::UpperArch,
                "point workflow did not alternate to the upper STL");
        window.onDentalPointPicked(objectActorKey(kUpperArchLabel), 0.0, 0, 0);
        for (int i = 1; i < 3; ++i) {
            window.onDentalPointPicked(5, i * 10.0, 0, 0);
            window.onDentalPointPicked(objectActorKey(kUpperArchLabel), i * 10.0, 0, 0);
        }
        require(window.m_matchUpperAct->isEnabled(), "paired points did not unlock registration");
        require(window.m_modelGuidePoints->text().contains("3"), "point counts not displayed");
        require(window.m_modelGuideSteps[2]->property("current").toBool(), "guide did not advance to registration");
        window.m_upperRegistrationCalculated = true;
        window.m_dentalGizmoActive = true;
        window.updateModelWorkflowUi();
        require(window.m_compositeAct->isEnabled() && window.m_compositeButton->isEnabled(),
                "active registration gizmo disabled direct composite creation");
        require(window.m_modelGuideMessage->text().contains("Crear modelo compuesto"),
                "model guide still requests a separate gizmo acceptance");
        window.m_upperRegistrationCalculated = false;
        window.m_dentalGizmoActive = false;
        window.updateModelWorkflowUi();
        window.onDentalPointPicked(5, 0, 10, 0);
        require(!window.m_matchUpperAct->isEnabled(), "unpaired extra point allowed registration");
        // Point tools are automatic; only the registration action remains in the side panel.
        settle();
        bool pointButton = false;
        bool registerButton = false;
        for (auto* button : window.findChildren<QToolButton*>())
            if (button->isVisibleTo(&window)) {
                pointButton = pointButton || button->defaultAction() == window.m_upperPtsAct;
                registerButton = registerButton || button->defaultAction() == window.m_matchUpperAct;
            }
        require(window.m_modelControlPopulated && !pointButton && registerButton,
                "MODELOS did not simplify the automatic point workflow");
        // Atrás undoes the last step of the jaw: first the capture, then the points; the STL stays.
        window.goBackModelWorkflow();
        window.goBackModelWorkflow();
        require(window.m_maxillaBonePoints.isEmpty() && window.m_upperArchPoints.isEmpty() && window.m_upperArchMesh,
                "Atrás did not reset the points of the current step");
        require(window.m_modelGuideSteps[1]->property("current").toBool(), "guide did not return to the points step");
        for (QSize size : {QSize(1366, 768), QSize(1700, 950)}) {
            window.resize(size);
            settle();
            require(window.m_modelGuide->isVisibleTo(&window), "model guide is not visible");
            for (auto* label : window.m_modelGuideSteps)
                require(window.m_modelGuide->rect().contains(label->geometry()), "guide label overflows");
            require(window.m_modelGuideMessage->geometry().bottom() < window.m_modelGuidePoints->geometry().top(),
                    "instructions overlap point counts");
            QDir().mkpath(artifactsDir);
            window.grab().save(QDir(artifactsDir).filePath(QString("model-guide-%1.png").arg(size.width())));
        }
    }

    static void runCompositeWorkflow(const QString& artifactsDir)
    {
        const auto capture = [&artifactsDir](MainWindow& w, const QString& name) {
            w.m_modelMatchView->render();
            settle();
            const QImage view = w.m_modelMatchView->findChild<QVTKOpenGLNativeWidget*>()->grabFramebuffer();
            const QImage panel = w.m_compositeBlockPanel->grab().toImage();
            QImage image(std::max(view.width(), panel.width()), view.height() + panel.height(), QImage::Format_RGB32);
            image.fill(QColor(30, 30, 32));
            QPainter painter(&image);
            painter.drawImage(0, 0, view);
            painter.drawImage(0, view.height(), panel);
            painter.end();
            QDir().mkpath(artifactsDir);
            require(image.save(QDir(artifactsDir).filePath(name)), "composite screenshot not written");
        };
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1700, 950);
        window.show();
        settle();

        auto volume = vtkSmartPointer<vtkImageData>::New();
        volume->SetDimensions(16, 16, 16);
        volume->SetSpacing(0.5, 0.5, 0.5);
        volume->AllocateScalars(VTK_SHORT, 1);
        window.m_volume = volume;

        const auto setJaw = [&window](bool upper) {
            const auto bone = upper ? boxMesh({-35, 35, -10, 35, 4, 30}, false, false)
                                    : boxMesh({-35, 35, -10, 35, -30, -4}, false, false);
            window.m_mesh3DView->addMesh(upper ? 5 : 6, bone, upper ? "Maxilar" : "Mandibula");
            const auto arch = upper ? upperTeeth({}) : lowerTeeth({});
            auto registered = vtkSmartPointer<vtkPolyData>::New();
            registered->DeepCopy(arch);
            auto identity = vtkSmartPointer<vtkMatrix4x4>::New();
            identity->Identity();
            if (upper) {
                window.m_upperArchOriginalMesh = arch;
                window.m_upperArchMesh = registered;
                window.m_upperArchRegistrationMatrix = identity;
                window.m_upperRegistrationCalculated = true;
            } else {
                window.m_lowerArchOriginalMesh = arch;
                window.m_lowerArchMesh = registered;
                window.m_lowerArchRegistrationMatrix = identity;
                window.m_lowerRegistrationCalculated = true;
            }
        };
        // ORIENTACION without composites explains what is missing instead of an empty view.
        window.setOrientationWorkspace(true);
        require(window.statusBar()->currentMessage().contains(QStringLiteral("modelos compuestos")),
                "empty ORIENTACION does not explain that the composites are missing");

        setJaw(true);
        window.setModelsWorkspace(true);
        settle();

        // Fine adjustment: the registered scan is drawn on the CT slices.
        window.updateModelWorkflowUi();
        require(window.m_axialView && window.m_axialView->surfaceContourCount() == 1,
                "registered scan contour not sent to the CT slices");

        // Block stage.
        window.m_modelStepStack->setCurrentIndex(0);
        window.showRegistrationReview(0);
        window.m_compositeButton->click();
        require(window.m_compositeStage == MainWindow::CompositeStage::Block, "composite did not open the block stage");
        require(window.m_upperCompositeBlock.valid && std::abs(window.m_upperCompositeBlock.sizeMm[2] - 15.0) < 1e-9,
                "initial cutting block is not 15 mm thick");
        require(window.m_compositeBlockPanel->isVisibleTo(&window), "block panel not shown");

        // Default method: contour points placed around the scan.
        require(window.compositePointsMethodActive(), "contour points are not the default composite method");
        require(!window.m_compositeCalculateButton->isEnabled(), "composite can be calculated without contour points");
        {
            const double kPiLocal = 3.14159265358979323846;
            for (int i = 0; i <= 5; ++i) {
                const double t = kPiLocal * i / 5.0;
                window.onCompositeContourPointAdded(0, (kArchRx + 5.0) * std::cos(t), (kArchRy + 5.0) * std::sin(t), 9.0);
            }
            for (int i = 5; i >= 0; --i) {
                const double t = kPiLocal * i / 5.0;
                window.onCompositeContourPointAdded(0, (kArchRx - 6.0) * std::cos(t), (kArchRy - 6.0) * std::sin(t), 9.0);
            }
        }
        require(window.m_compositeContours[0].size() == 12 && window.m_compositeCalculateButton->isEnabled(),
                "contour points not stored");
        require(window.m_modelMatchView->meshData(kCompositeContourActorKey) != nullptr &&
                    window.m_modelMatchView->hasOverlay(5),
                "contour not shown around the scan");
        window.onCompositeContourPointRemoved(0, 11);
        window.onCompositeContourPointAdded(0, (kArchRx - 6.0), 0.0, 9.0);
        require(window.m_compositeContours[0].size() == 12, "removing/re-adding a contour point failed");
        window.m_modelMatchView->setStandardView(0);
        capture(window, QStringLiteral("composite-contour.png"));
        window.calculateBlockComposite();
        require(waitFor([&] {
                    return !window.m_compositeInProgress && window.m_compositeStage == MainWindow::CompositeStage::Review;
                }, 60000),
                "contour composite review never opened");
        require(CompositeBlockCore::HasParts(window.m_compositeReviewMesh) &&
                    CompositeBlockCore::ExtractPart(window.m_compositeReviewMesh, CompositeBlockCore::DentalPart),
                "contour composite has no dental part");
        window.backToCompositeBlockStage();

        // The cutting block stays available.
        window.m_compositeMethodCombo->setCurrentIndex(window.m_compositeMethodCombo->findData(0));
        require(!window.compositePointsMethodActive() && window.compositeBlockMethodActive(), "block method not selectable");
        require(window.m_modelMatchView->meshData(kCompositeBlockActorKey) != nullptr, "cutting block not shown");
        require(!window.m_modelMatchView->meshData(kCompositeContourActorKey), "contour still shown with the block method");
        require(window.m_modelGuide->isVisibleTo(&window) && window.m_modelGuideSteps[3]->property("current").toBool(),
                "guide disappeared during block adjustment");
        window.m_compositeThicknessSpin->setValue(12.0);
        require(std::abs(window.m_upperCompositeBlock.sizeMm[2] - 12.0) < 1e-9, "thickness control did not resize the block");
        window.m_compositeThicknessSpin->setValue(15.0);
        window.m_modelMatchView->setStandardView(0);
        capture(window, QStringLiteral("composite-block.png"));

        // Review is mandatory; Atrás returns to the block without storing anything.
        const auto waitReview = [&] {
            return waitFor([&] {
                return !window.m_compositeInProgress && window.m_compositeStage == MainWindow::CompositeStage::Review;
            }, 60000);
        };
        window.calculateBlockComposite();
        require(waitReview(), "composite review never opened");
        require(!window.m_upperCompositeMesh, "composite stored before review");
        require(window.m_modelGuideMessage->text().contains(QStringLiteral("Revise")) &&
                !window.m_modelGuideSteps[3]->property("complete").toBool(), "guide marked review as accepted");
        require(CompositeBlockCore::HasParts(window.m_compositeReviewMesh), "reviewed composite has no linked parts");
        window.m_modelMatchView->setStandardView(0);
        capture(window, QStringLiteral("composite-review.png"));
        window.backToCompositeBlockStage();
        require(window.m_compositeStage == MainWindow::CompositeStage::Block && !window.m_upperCompositeMesh,
                "Atrás did not return to the block stage");
        window.calculateBlockComposite();
        require(waitReview(), "composite review never reopened");
        window.acceptCompositeReview();
        require(window.m_compositeStage == MainWindow::CompositeStage::None &&
                    CompositeBlockCore::HasParts(window.m_upperCompositeMesh) &&
                    window.objectEntryExists(kUpperCompositeLabel),
                "accepted composite not stored");

        // A chained creation cannot skip the review.
        setJaw(false);
        window.m_autoCreateBothComposites = true;
        window.m_modelStepStack->setCurrentIndex(1);
        window.createDentalCompositeModels();
        require(window.m_compositeStage == MainWindow::CompositeStage::Block && !window.m_lowerCompositeMesh,
                "chained composite skipped the block and review stages");
        window.cancelCompositeStage();
        require(window.m_compositeStage == MainWindow::CompositeStage::None && !window.m_autoCreateBothComposites,
                "cancel did not leave the composite stage");

        // Link: a Le Fort-like cut keeps the scan with the tooth-bearing segment; the splint uses it.
        auto plane = vtkSmartPointer<vtkPlane>::New();
        plane->SetOrigin(0.0, 0.0, 12.0);
        plane->SetNormal(0.0, 0.0, -1.0);
        auto clip = vtkSmartPointer<vtkClipPolyData>::New();
        clip->SetInputData(window.m_upperCompositeMesh);
        clip->SetClipFunction(plane);
        clip->Update();
        auto segment = vtkSmartPointer<vtkPolyData>::New();
        segment->DeepCopy(clip->GetOutput());
        window.m_leFortSegmentMesh = segment;
        const auto dental = CompositeBlockCore::ExtractPart(segment, CompositeBlockCore::DentalPart);
        const auto source = window.splintSourceMesh(kLeFortSegLabel);
        require(dental && source && source->GetNumberOfPolys() == dental->GetNumberOfPolys(),
                "splint source is not the dental part of the Le Fort segment");

        // Export union of the composite is a single closed mesh.
        const VoxelUnionResult united = CompositeBlockCore::VoxelUnion({window.m_upperCompositeMesh.Get()}, 0.5, 3);
        require(united.ok && isClosed(united.mesh), "voxel union of the composite is not closed");

        // ORIENTACION shows the accepted composite and the Frankfort alignment levels it.
        window.setOrientationWorkspace(true);
        settle();
        require(window.m_orientationView->meshData(objectActorKey(kUpperCompositeLabel)) != nullptr,
                "ORIENTACION did not receive the accepted composite");
        require(window.m_frankfurtCapturingIdx == 0, "ORIENTACION did not start Porion derecho automatically");
        window.onFrankfurtPointPicked(0, -30.0, -5.0, 20.0);
        settle();
        require(window.m_frankfurtCapturingIdx == 1,
                "ORIENTACION did not activate Porion izquierdo after the first point");
        window.m_frankfurtPoints = {QVector3D(-30.0f, -5.0f, 20.0f), QVector3D(30.0f, -5.0f, 20.0f),
                                    QVector3D(-25.0f, 30.0f, 26.0f), QVector3D(25.0f, 30.0f, 26.0f)};
        window.alignFrankfurtPlane();
        // ORIENTACION guided side panel: its actions are buttons there and the guide explains the step.
        window.updateOrientationGuide();
        settle();
        bool orientationButton = false;
        for (auto* button : window.findChildren<QToolButton*>())
            orientationButton = orientationButton ||
                (button->defaultAction() == window.m_saveOrientationAct && button->isVisibleTo(&window));
        require(window.m_orientationControlPopulated && orientationButton, "ORIENTACION actions are not in the side panel");
        require(window.m_orientationGuideSteps[0]->text().startsWith(QStringLiteral("✓")) &&
                    !window.m_orientationGuideMessage->text().isEmpty(),
                "ORIENTACION side panel does not guide the Frankfort steps");
        require(window.m_orientationView->meshData(objectActorKey(kUpperCompositeLabel)) != nullptr &&
                    window.m_orientationView->meshData(-100) != nullptr,
                "Frankfort alignment left the orientation view without the composite or the plane");
        require(std::abs(window.m_frankfurtPoints[0].z() - window.m_frankfurtPoints[2].z()) < 1e-3 &&
                    std::abs(window.m_frankfurtPoints[1].z() - window.m_frankfurtPoints[3].z()) < 1e-3,
                "Frankfort plane is not horizontal after alignment");
        require(CompositeBlockCore::HasParts(window.m_upperCompositeMesh), "Frankfort alignment dropped the dental link");
        // Hidden in the object list: ORIENTACION still shows the model it orients.
        window.setObjectEntryVisible(kUpperCompositeLabel, false);
        window.setOrientationWorkspace(true);
        require(window.objectEntryVisible(kUpperCompositeLabel), "ORIENTACION left the composite hidden");
        window.m_orientationView->setStandardView(1);
        window.m_orientationView->render();
        settle();
        QDir().mkpath(artifactsDir);
        window.m_orientationView->findChild<QVTKOpenGLNativeWidget*>()->grabFramebuffer().save(
            QDir(artifactsDir).filePath(QStringLiteral("orientation-frankfort.png")));

        // Save and reopen keeps the block and the link.
        QTemporaryDir dir;
        require(dir.isValid(), "no temporary directory");
        const QString path = dir.filePath(QStringLiteral("compuesto.maxilloproject"));
        QString error;
        require(ProjectSerializer::save(path, window.collectProjectState(), &error), "save failed: " + error.toStdString());
        ProjectState loaded;
        require(ProjectSerializer::load(path, loaded, &error), "load failed: " + error.toStdString());
        MainWindow reopened;
        reopened.setAttribute(Qt::WA_DontShowOnScreen);
        reopened.show();
        settle();
        reopened.applyProjectState(loaded);
        settle();
        require(CompositeBlockCore::HasParts(reopened.m_upperCompositeMesh), "composite link lost after reopening");
        require(reopened.m_upperCompositeBlock.valid &&
                    reopened.m_upperCompositeBlock.center == window.m_upperCompositeBlock.center &&
                    reopened.m_upperCompositeBlock.sizeMm == window.m_upperCompositeBlock.sizeMm,
                "cutting block lost after reopening");
        require(reopened.m_compositeContours[0] == window.m_compositeContours[0], "composite contour lost after reopening");
        std::cout << "Composite workspace: block, mandatory review, link, splint source, union and project OK\n";
    }

    // REPOSICIÓN analysis: intersection volume, highlight, restriction and pre-op ghost.
    static void runRepositionAnalysis()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1400, 900);
        window.show();
        settle();
        window.m_leFortSegmentMesh = boxMesh({-10, 10, -10, 10, 0, 10}, false, false);
        window.m_leFortCranialMesh = boxMesh({-20, 20, -20, 20, 8, 30}, false, false); // 2 mm impaction
        window.addObjectEntry(QStringLiteral("Segmento Le Fort I"), QColor(200, 180, 160), kLeFortSegLabel);
        window.addObjectEntry(QStringLiteral("Base craneal"), QColor(220, 210, 190), kLeFortCranialLabel);
        window.setRepositionWorkspace(true);
        settle();
        for (int row = 0; row < window.m_repositionObjectList->count(); ++row) {
            auto* item = window.m_repositionObjectList->item(row);
            if (item->data(Qt::UserRole).toInt() == kLeFortSegLabel) {
                item->setCheckState(Qt::Checked);
                window.m_repositionObjectList->setCurrentItem(item);
            }
        }
        require(window.selectedRepositionTargetLabels() == QList<int>{kLeFortSegLabel}, "Le Fort segment not selected");
        window.m_segmentReferences[kLeFortSegLabel] =
            OsteotomyCore::CaptureSegmentReference(window.m_leFortSegmentMesh, {{0.0, 10.0, 0.0}});

        window.analyzeRepositionIntersection(false);
        require(std::abs(window.m_repositionLastIntersectionMm3 - 800.0) < 60.0,
                "intersection volume is not the 2 mm impaction: " + std::to_string(window.m_repositionLastIntersectionMm3));
        require(window.m_repositionIntersectionLabel->text().contains(QStringLiteral("mm³")), "intersection not reported");
        window.toggleRepositionHighlight();
        require(window.m_repositionHighlightActive &&
                    window.m_repositionView->meshData(kRepositionHighlightBaseKey - kLeFortSegLabel) != nullptr,
                "intersection not highlighted");
        window.toggleRepositionHighlight();
        require(!window.m_repositionHighlightActive &&
                    !window.m_repositionView->meshData(kRepositionHighlightBaseKey - kLeFortSegLabel),
                "highlight not removed");

        double before[6];
        window.m_leFortSegmentMesh->GetBounds(before);
        window.setRepositionRestriction(2);
        window.translateRepositionTarget(0.0, 0.0, -3.0);
        double after[6];
        window.m_leFortSegmentMesh->GetBounds(after);
        require(after[4] == before[4], "translation ignored the rotation-only restriction");
        window.setRepositionRestriction(0);
        window.translateRepositionTarget(0.0, 0.0, -3.0);
        window.m_leFortSegmentMesh->GetBounds(after);
        require(std::abs(after[4] - (before[4] - 3.0)) < 1e-6, "translation not applied without restriction");
        require(window.m_repositionMeasureLabel->text().contains(QStringLiteral("descenso")),
                "Le Fort descent not measured: " + window.m_repositionMeasureLabel->text().toStdString());
        window.analyzeRepositionIntersection(false);
        require(window.m_repositionLastIntersectionMm3 == 0.0, "moved segment still reports an intersection");

        window.toggleRepositionPreOp();
        require(window.m_repositionView->meshData(kRepositionPreOpBaseKey - kLeFortSegLabel) != nullptr,
                "pre-op position not shown");
        window.toggleRepositionPreOp();
        require(!window.m_repositionView->meshData(kRepositionPreOpBaseKey - kLeFortSegLabel), "pre-op ghost not removed");
        std::cout << "Reposition analysis OK\n";
    }

    // FÉRULA follows REPOSICIÓN: the moved Le Fort segment is the source and its guide points follow it.
    static void runSplintFollowsReposition()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1400, 900);
        window.show();
        settle();
        window.m_leFortSegmentMesh = CompositeBlockCore::TagPart(upperTeeth({}), CompositeBlockCore::DentalPart);
        window.m_lowerCompositeMesh = lowerTeeth({});
        window.setSplintWorkspace(true);
        settle();
        require(window.activeSplintDesign()->upperSource == kLeFortSegLabel, "Intermedia does not use the Le Fort segment");
        for (const SplintPoint3& p : guidePoints(5.0))
            window.onSplintEditablePointAdded(0, p[0], p[1], p[2]);
        for (const SplintPoint3& p : guidePoints(-5.0))
            window.onSplintEditablePointAdded(1, p[0], p[1], p[2]);
        const auto upperBefore = window.activeSplintDesign()->upperPoints;
        const auto lowerBefore = window.activeSplintDesign()->lowerPoints;
        require(upperBefore.size() == 3 && lowerBefore.size() == 3, "guide points not placed");

        auto transform = vtkSmartPointer<vtkTransform>::New();
        transform->Translate(0.0, 3.0, 1.0);
        auto filter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
        filter->SetInputData(window.m_leFortSegmentMesh);
        filter->SetTransform(transform);
        filter->Update();
        window.setRepositionMeshForLabel(kLeFortSegLabel, filter->GetOutput()); // as REPOSICIÓN does
        window.syncSplintView();

        const auto& upper = window.activeSplintDesign()->upperPoints;
        for (size_t i = 0; i < upper.size(); ++i)
            require(std::abs(upper[i][0] - upperBefore[i][0]) < 1e-6 && std::abs(upper[i][1] - upperBefore[i][1] - 3.0) < 1e-6 &&
                        std::abs(upper[i][2] - upperBefore[i][2] - 1.0) < 1e-6,
                    "upper guide points did not follow the repositioned Le Fort segment");
        require(window.activeSplintDesign()->lowerPoints == lowerBefore, "lower guide points moved with the maxilla");
        double bounds[6];
        window.splintSourceMesh(kLeFortSegLabel)->GetBounds(bounds);
        require(std::abs(bounds[4] - 2.0) < 1e-6, "the splint source is not the repositioned segment");
        waitFor([&] { return !window.m_splintPreview->isBusy(); }, 60000);
        std::cout << "Splint follows reposition OK\n";
    }

    // ORIENTACION with segmented bones only: the bones are oriented (as «Continuar sin match»).
    static void runOrientationWithBonesOnly()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1400, 900);
        window.show();
        settle();
        window.m_mesh3DView->addMesh(5, boxMesh({-35, 35, -10, 35, 4, 30}, false, false), "Maxilar");
        window.m_mesh3DView->addMesh(6, boxMesh({-35, 35, -10, 35, -30, -4}, false, false), "Mandibula");
        window.setOrientationWorkspace(true);
        settle();
        require(window.m_upperCompositeMesh && window.m_lowerCompositeMesh, "bones were not turned into composites");
        require(window.m_orientationView->meshData(objectActorKey(kUpperCompositeLabel)) != nullptr &&
                    window.m_orientationView->meshData(objectActorKey(kLowerCompositeLabel)) != nullptr,
                "ORIENTACION does not show the segmented bones");
        std::cout << "Orientation with bones only OK\n";
    }

    // GUIAS: choose the guide, wrap its models, mark the support region, slot the planned cut, add a Boolean
    // figure, build and save.
    // The inspector has no mask tab: segmentation publishes objects, and the objects tab is where it lands.
    static void runInspectorTabs()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.show();
        settle();
        require(window.m_inspectorTabs != nullptr, "the inspector has no tabs");
        QStringList titles;
        for (int index = 0; index < window.m_inspectorTabs->count(); ++index)
            titles << window.m_inspectorTabs->tabText(index);
        require(!titles.contains(QStringLiteral("Mascaras")) && !titles.contains(QStringLiteral("Máscaras")),
                "the inspector still shows a mask tab: " + titles.join(QStringLiteral(", ")).toStdString());
        require(window.m_objectsInspectorTab != nullptr &&
                    window.m_inspectorTabs->indexOf(window.m_objectsInspectorTab) >= 0,
                "the inspector has no objects tab");
        std::cout << "Inspector tabs OK\n";
    }

    static void runGuidesWorkflow(const QString& artifactsDir)
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1700, 950);
        window.show();
        settle();

        // The osteotomy-time Le Fort segment is a wall whose face looks forward (+y). The live segment is moved
        // far away, as REPOSICION may leave it; GUIAS must still use the preserved pre-reposition geometry.
        const auto wall = boxMesh({-25.0, 25.0, -8.0, 0.0, -5.0, 25.0}, false, false);
        const auto movedWall = boxMesh({-25.0, 25.0, -8.0, 0.0, 25.0, 55.0}, false, false);
        const auto cranium = boxMesh({-30.0, 30.0, -20.0, -2.0, 26.0, 45.0}, false, false);
        window.addObjectEntry(QStringLiteral("Segmento Le Fort I"), QColor(230, 220, 200), kLeFortSegLabel);
        window.setRepositionMeshForLabel(kLeFortSegLabel, movedWall);
        window.m_repositionOriginalMeshes[kLeFortSegLabel] = wall;
        window.addObjectEntry(QStringLiteral("Base craneal"), QColor(220, 210, 190), kLeFortCranialLabel);
        window.setRepositionMeshForLabel(kLeFortCranialLabel, cranium);
        window.m_repositionOriginalMeshes[kLeFortCranialLabel] = cranium;
        // A planned Le Fort cut, as the osteotomy wizard would leave it.
        window.rememberOsteotomyCut(QStringLiteral("Le Fort I"),
                                    OsteotomyCore::LeFortPath({{{-10.0, 5.0, 9.4}, {10.0, 5.0, 9.6},
                                                                {-20.0, -5.0, 9.2}, {20.0, -5.0, 9.3}}}),
                                    GuideType::LeFort);

        for (auto* tab : window.findChildren<QToolButton*>(QStringLiteral("MT")))
            if (tab->text() == QStringLiteral("GUIAS"))
                tab->click();
        settle();
        require(window.m_viewModeStack->currentIndex() == 7 && window.m_guideView != nullptr,
                "GUIAS did not open its workspace");
        require(window.m_orthoStep == 7, "GUIAS is not the eighth step of the bar");
        require(window.findChild<QWidget*>(QStringLiteral("GuideControlPanel")) != nullptr,
                "the guides side panel is missing");
        // Only the reachable steps are on show: before the envelope there is nothing else to do.
        require(!window.m_guideRegionSection->isVisibleTo(&window) && !window.m_guideBuildSection->isVisibleTo(&window),
                "the panel shows steps that are not reachable yet");

        // 1. A Le Fort guide always wraps the Le Fort segment and the cranial base.
        require(window.m_guideTypeCombo->currentData().toInt() == static_cast<int>(GuideType::LeFort),
                "the guide does not start as a Le Fort guide");
        require(window.m_guideSourcesLabel->text().contains(QStringLiteral("Segmento Le Fort I")) &&
                    window.m_guideSourcesLabel->text().contains(QStringLiteral("Base craneal")),
                "the Le Fort guide does not name its two models: " + window.m_guideSourcesLabel->text().toStdString());
        require(window.m_guideSourcesLabel->text().contains(QStringLiteral("preoperatoria")),
                "the Le Fort guide does not identify its pre-reposition frame");
        double sourceBounds[6] = {}, liveBounds[6] = {};
        window.guideSourceMeshForLabel(kLeFortSegLabel)->GetBounds(sourceBounds);
        window.repositionMeshForLabel(kLeFortSegLabel)->GetBounds(liveBounds);
        require(sourceBounds[4] < 0.0 && liveBounds[4] > 20.0,
                "the Le Fort guide uses the repositioned segment instead of the osteotomy-time segment");
        window.m_guideDetailSpin->setValue(0.5); // coarse: this is a wiring test, not a geometry one
        window.computeGuideWrap();
        settle();
        require(window.m_guideWrapMesh && window.m_guideWrapMesh->GetNumberOfPolys() > 0, "the envelope was not built");
        require(window.m_guidePlan.sourceLabels == std::vector<int>({kLeFortSegLabel, kLeFortCranialLabel}),
                "the envelope was not built from the Le Fort segment and the cranial base");
        require(window.m_guidePrepared.ok, "the envelope was not measured for marking");
        require(!window.m_guideShowModelsCheck->isChecked() && window.m_guideShowWrapCheck->isChecked() &&
                    std::abs(window.m_guideWrapOpacitySpin->value() - 1.0) < 1e-9,
                "the envelope is not shown alone and opaque after computing it");
        require(window.m_guideRegionSection->isVisibleTo(&window) && !window.m_guideSlotSection->isVisibleTo(&window),
                "the support region step did not appear alone with the envelope");

        // 2. The support region, painted with the brush on the envelope, in any order.
        window.m_guideRegionButton->setChecked(true);
        require(window.m_guidePointMode == 1, "painting the region did not switch the mode");
        window.m_guideBrushSpin->setValue(4.0);
        for (double z = 3.0; z <= 17.0; z += 2.0)
            for (double x = -14.0; x <= 14.0; x += 2.0)
                window.onGuideSurfaceBrushed(x, 0.0, z, Qt::NoModifier);
        window.onGuideBrushFinished();
        require(window.m_guidePlan.paint.size() > 20, "the brushed region was not collected");
        require(window.m_guideSlotSection->isVisibleTo(&window) && window.m_guideBuildSection->isVisibleTo(&window),
                "the slot and build steps did not appear with the painted region");

        // A splint is copied into the guide plan, never reused by pointer, and a connector is placed with 3 points.
        // The splint was made against the repositioned Le Fort (+30 mm in z).
        // Its integrated copy must return to the preoperative Le Fort frame.
        const auto splintSource = boxMesh({-8.0, 8.0, 4.0, 7.0, 34.0, 48.0}, false, false);
        window.addObjectEntry(QStringLiteral("Férula intermedia"), QColor(235, 243, 248), kIntermediateSplintLabel);
        window.setRepositionMeshForLabel(kIntermediateSplintLabel, splintSource);
        const size_t figuresBeforeConnection = window.m_guidePlan.figures.size();
        window.m_guideSplintCopyCombo->setCurrentIndex(
            window.m_guideSplintCopyCombo->findData(kIntermediateSplintLabel));
        window.addGuideSplintCopy();
        require(window.m_guidePlan.figures.size() == figuresBeforeConnection + 1 &&
                    window.m_guidePlan.figures.back().sourceLabel == kIntermediateSplintLabel &&
                    window.m_guidePlan.figures.back().mesh != window.repositionMeshForLabel(kIntermediateSplintLabel),
                "the integrated splint is not an independent copy");
        double alignedSplintBounds[6] = {};
        GuideDesignCore::FigurePreview(window.m_guidePlan.figures.back())->GetBounds(alignedSplintBounds);
        require(alignedSplintBounds[4] < 5.0 && alignedSplintBounds[5] < 20.0,
                "the integrated splint did not return to the preoperative Le Fort frame");
        window.m_guideTubeButton->setChecked(true);
        for (const auto& point : {std::array<double, 3>{-5.0, 0.0, 8.0},
                                  std::array<double, 3>{0.0, -5.0, 11.0},
                                  std::array<double, 3>{5.0, 5.0, 14.0}})
            window.onGuidePointPicked(0, point[0], point[1], point[2]);
        require(window.m_guidePlan.figures.size() == figuresBeforeConnection + 2 &&
                    window.m_guidePlan.figures.back().shape == GuideFigureShape::CurvedTube &&
                    window.m_guidePlan.figures.back().controlPoints.size() == 3,
                "the curved connector was not collected");
        require(window.m_guidePlan.figures.back().controlPoints[1][1] > 8.0,
                "the connector still bends too close to the Le Fort segment");
        window.m_guideTubeButton->setChecked(false);
        window.m_guidePlan.figures.resize(figuresBeforeConnection); // keep the original geometry test focused
        window.refreshGuideFigureList();
        window.syncGuideView();
        window.m_guideRegionButton->setChecked(true);
        // The brushed patch shows on the envelope, which is its own coloured layer.
        auto* paintColors = window.m_guideWrapMesh->GetPointData()->GetArray("GuidePaint");
        require(paintColors != nullptr, "the envelope is not coloured as its own layer");
        bool paintedSeen = false;
        double rgb[3] = {};
        for (vtkIdType id = 0; id < paintColors->GetNumberOfTuples() && !paintedSeen; ++id) {
            paintColors->GetTuple(id, rgb);
            paintedSeen = rgb[0] < 30.0 && rgb[2] > 200.0;
        }
        require(paintedSeen, "the brushed region is not painted on the envelope");
        // Ctrl erases where the brush passes, and Alt-dragging resizes the brush.
        const size_t dabs = window.m_guidePlan.paint.size();
        window.onGuideSurfaceBrushed(0.0, 0.0, 10.0, Qt::ControlModifier);
        require(window.m_guidePlan.paint.size() == dabs + 1 && window.m_guidePlan.paint.back().erase,
                "Ctrl did not erase with the brush");
        window.onGuideSurfaceBrushed(0.0, 0.0, 10.0, Qt::NoModifier); // paint the spot back
        window.onGuideBrushRadiusDragged(-40.0);
        require(window.m_guideBrushSpin->value() > 4.0, "dragging up did not grow the brush");
        window.m_guideBrushSpin->setValue(4.0);
        window.m_guideRegionButton->setChecked(false);
        // Layers can be hidden one by one.
        window.m_guideShowWrapCheck->setChecked(false);
        window.m_guideShowWrapCheck->setChecked(true);

        // 3. The planned cut gets a slot, with the ends the user places.
        require(window.m_guideCutList->count() == 1, "the planned osteotomy is not offered as a slot");
        window.m_guideCutList->setCurrentRow(0);
        window.m_guideSlotEndsButton->setChecked(true);
        window.onGuidePointPicked(0, -6.0, 1.4, 9.3);
        window.onGuidePointPicked(0, 6.0, 1.4, 9.5);
        window.m_guideSlotEndsButton->setChecked(false);
        require(window.m_guidePlan.slotPlan.size() == 1 && window.m_guidePlan.slotPlan[0].hasExtent,
                "the slot ends were not recorded");
        require(window.m_guideCutList->item(0)->checkState() == Qt::Checked,
                "placing the ends did not tick the osteotomy");

        // 4. A Boolean figure: a thin box subtracted through the wall, placed on the surface with exact measurements.
        window.m_guideFigureShapeCombo->setCurrentIndex(
            window.m_guideFigureShapeCombo->findData(static_cast<int>(GuideFigureShape::Box)));
        window.m_guideFigureOperationCombo->setCurrentIndex(
            window.m_guideFigureOperationCombo->findData(static_cast<int>(GuideFigureOperation::Subtract)));
        // Figures live in a folded section: visibility is relative to that section, not to the window.
        require(window.m_guideFigureWidthSpin->isVisibleTo(window.m_guideFigureWidthSpin->parentWidget()) &&
                    !window.m_guideFigureDiameterSpin->isVisibleTo(window.m_guideFigureDiameterSpin->parentWidget()),
                "the box does not show its own measurements");
        window.m_guideFigureWidthSpin->setValue(6.0);
        window.m_guideFigureHeightSpin->setValue(1.0);
        window.m_guideFigureDepthSpin->setValue(10.0);
        window.m_guidePlaceFigureButton->setChecked(true);
        window.onGuidePointPicked(0, 8.0, 0.0, 5.0);
        window.m_guidePlaceFigureButton->setChecked(false);
        require(window.m_guidePlan.figures.size() == 1 && window.m_guideFigureList->count() == 1,
                "the figure was not placed");
        require(window.m_guideView->meshData(-560) != nullptr, "the figure is not previewed in the view");
        // Moving it with the gizmo and fixing it again keeps it in the plan.
        window.m_guideFigureList->setCurrentRow(0);
        window.m_guideMoveFigureButton->setChecked(true);
        require(window.m_guideView->hasGizmo(), "the gizmo did not start on the figure");
        window.m_guideMoveFigureButton->setChecked(false);
        require(!window.m_guideView->hasGizmo() && window.m_guidePlan.figures.size() == 1,
                "fixing the figure lost it");

        // 5. A fixation hole, drilled along the surface normal.
        window.m_guideHoleButton->setChecked(true);
        window.onGuidePointPicked(0, 0.0, 0.0, 15.0);
        window.m_guideHoleButton->setChecked(false);
        require(window.m_guidePlan.holes.size() == 1, "the fixation hole was not recorded");

        // 6. Build: one piece, shown in the view and listed as an object.
        window.m_guideThicknessSpin->setValue(2.5);
        window.buildGuideMesh();
        settle();
        require(window.m_guideMesh && window.m_guideMesh->GetNumberOfPolys() > 0, "the guide was not built");
        require(window.objectEntryExists(kGuideMeshLabel), "the guide is not in the object list");
        require(window.m_guideView->meshData(objectActorKey(kGuideMeshLabel)) != nullptr,
                "the guide is not shown in the GUIAS view");
        require(window.m_guideReportLabel->text().contains(QStringLiteral("1 restada")),
                "the report does not count the subtracted figure: " + window.m_guideReportLabel->text().toStdString());
        require(window.m_guideExportButton->isEnabled(), "the guide cannot be exported");
        require(window.m_guideExportSection->isVisibleTo(&window), "the export step did not appear with the guide");
        // Screenshots of the finished guide alone, to look at its rim and surface.
        window.m_guideShowWrapCheck->setChecked(false);
        window.m_guideShowFiguresCheck->setChecked(false);
        QDir().mkpath(artifactsDir);
        for (const auto& [viewIndex, name] : {std::pair{0, QStringLiteral("guide-front.png")},
                                              std::pair{1, QStringLiteral("guide-side.png")}}) {
            window.m_guideView->setStandardView(viewIndex);
            window.m_guideView->render();
            settle();
            const QImage shot = window.m_guideView->findChild<QVTKOpenGLNativeWidget*>()->grabFramebuffer();
            require(shot.save(QDir(artifactsDir).filePath(name)), "guide screenshot not written");
        }
        window.m_guideShowWrapCheck->setChecked(true);
        window.m_guideShowFiguresCheck->setChecked(true);
        window.m_guideThicknessCheck->setChecked(true); // thickness map must not throw
        settle();

        // 7. EDITAR: the guide as clay. The palette opens on the smoothing tool and the surface brush
        // drives whichever tool is active.
        require(window.m_guideEditSection->isVisibleTo(&window), "the edit step did not appear with the guide");
        window.m_guideEditButton->setChecked(true);
        settle();
        require(window.m_guideSculptActive && window.m_guideSculpt.Ready(), "the edit session did not open");
        require(window.m_guideSculptPalette->isVisibleTo(&window) && window.m_guideSculptTools.size() == 8,
                "the sculpting palette is not on show");
        require(window.m_guideSculptTool == 0 && window.m_guidePointMode == 5,
                "the edit did not start on the smoothing brush");
        require(!window.m_guideSculptTools[6]->isEnabled(), "undo is offered before anything was edited");
        require(window.m_guideSculptPalette->grab()
                    .scaled(window.m_guideSculptPalette->width() * 4, window.m_guideSculptPalette->height() * 4,
                            Qt::KeepAspectRatio, Qt::SmoothTransformation)
                    .save(QDir(artifactsDir).filePath(QStringLiteral("guide-tools.png"))),
                "the sculpting palette was not captured");

        // Points on the guide itself, as the picker would report them.
        std::vector<std::array<double, 3>> onGuide;
        for (int step = 0; step < 6; ++step) {
            double point[3] = {};
            const vtkIdType index =
                static_cast<vtkIdType>(step) * window.m_guideMesh->GetNumberOfPoints() / 7;
            window.m_guideMesh->GetPoint(index, point);
            onGuide.push_back({point[0], point[1], point[2]});
        }
        for (const auto& point : onGuide)
            window.onGuideSurfaceBrushed(point[0], point[1], point[2], Qt::NoModifier);
        window.onGuideBrushFinished();
        settle();
        require(window.m_guideSculptEdited && window.m_guideSculpt.CanUndo(), "the smoothing stroke was not recorded");
        require(window.m_guideSculptTools[6]->isEnabled(), "undo is not offered after a stroke");

        // A bead of material added along a stroke, and the contextual bar following the tool.
        window.setGuideSculptTool(2);
        require(!window.m_guideSculptLevelRow->isVisibleTo(window.m_guideSculptBar),
                "adding material offers a level it does not use");
        for (const auto& point : onGuide)
            window.onGuideSurfaceBrushed(point[0], point[1], point[2], Qt::NoModifier);
        window.onGuideBrushFinished();
        settle();
        window.setGuideSculptTool(1);
        require(window.m_guideSculptLevelLabel->text() == QStringLiteral("Calor"),
                "hot wax does not call its slider heat");

        // A trim: a window over the right-hand end of the guide, swept along the view.
        window.m_guideView->setStandardView(0);
        settle();
        window.setGuideSculptTool(5);
        require(window.m_guidePointMode == 6 && window.m_guideSculptTrimRow->isVisibleTo(window.m_guideSculptBar),
                "the trim tool did not switch the panel");
        for (const auto& point : {std::array<double, 3>{12.0, 0.0, -10.0}, std::array<double, 3>{30.0, 0.0, -10.0},
                                  std::array<double, 3>{30.0, 0.0, 30.0}, std::array<double, 3>{12.0, 0.0, 30.0}})
            window.onGuidePointPicked(0, point[0], point[1], point[2]);
        require(window.m_guideTrimPoints.size() == 4, "the trim contour was not collected");
        const double beforeTrim = window.m_guideMesh->GetNumberOfPoints();
        window.applyGuideTrim();
        settle();
        require(window.m_guideTrimPoints.empty(), "the trim contour was not cleared after applying it");
        require(window.m_guideMesh && window.m_guideMesh->GetNumberOfPolys() > 0, "the trim left no guide");
        require(window.m_guideMesh->GetNumberOfPoints() < beforeTrim, "the trim removed nothing");

        // Undo puts the trim back.
        window.guideSculptUndo();
        settle();
        require(window.m_guideSculpt.CanRedo(), "undo left nothing to redo");
        require(window.m_guideMesh->GetNumberOfPoints() > 0.9 * beforeTrim, "undo did not put the trim back");

        // Whatever was edited, the guide is still one closed piece.
        const MeshCheck edited = MeshRepairCore::Analyze(window.m_guideMesh);
        require(edited.Valid(), "the edited guide is not a closed mesh: " + edited.Summary().toStdString());
        auto pieces = vtkSmartPointer<vtkPolyDataConnectivityFilter>::New();
        pieces->SetInputData(window.m_guideMesh);
        pieces->SetExtractionModeToAllRegions();
        pieces->Update();
        require(pieces->GetNumberOfExtractedRegions() == 1, "the edited guide came apart");
        {
            window.m_guideView->setStandardView(0);
            window.m_guideView->render();
            settle();
            const QImage shot = window.m_guideView->findChild<QVTKOpenGLNativeWidget*>()->grabFramebuffer();
            require(shot.save(QDir(artifactsDir).filePath(QStringLiteral("guide-edited.png"))),
                    "the edited guide screenshot was not written");
        }
        window.m_guideEditButton->setChecked(false);
        settle();
        require(!window.m_guideSculptActive && window.m_guidePointMode == 0, "leaving the edit left it running");
        const auto guideObject = window.repositionMeshForLabel(kGuideMeshLabel);
        require(guideObject && guideObject->GetNumberOfPolys() == window.m_guideMesh->GetNumberOfPolys(),
                "the edited guide did not become the guide object");

        // The plan travels with the project and comes back.
        const QJsonObject saved = window.guidePlanJson();
        require(!saved.isEmpty(), "the guide plan was not written");
        ProjectState state;
        state.guidesPlan = saved;
        MainWindow reopened;
        reopened.setAttribute(Qt::WA_DontShowOnScreen);
        reopened.show();
        settle();
        reopened.restoreGuidePlan(state);
        require(reopened.m_guidePlan.paint.size() == window.m_guidePlan.paint.size() && reopened.m_guidePlan.slotPlan.size() == 1 &&
                    reopened.m_guidePlan.holes.size() == 1 && reopened.m_guidePlan.figures.size() == 1,
                "the guide plan did not survive the project");
        require(reopened.m_guideCutList->count() == 1, "the reloaded plan does not offer its cut again");
        require(reopened.m_guideFigureList->count() == 1, "the reloaded plan does not list its figure");

        // A chin guide wraps other models and only offers the genioplasty: the Le Fort cut disappears from the list.
        window.m_guideTypeCombo->setCurrentIndex(window.m_guideTypeCombo->findData(static_cast<int>(GuideType::Chin)));
        settle();
        require(window.m_guidePlan.type == GuideType::Chin && window.m_guidePlan.paint.empty() &&
                    !window.m_guideWrapMesh,
                "switching to a chin guide did not start its envelope and region again");
        require(window.m_guideCutList->count() == 0, "a chin guide offers the Le Fort cut");
        require(window.m_guideSourcesLabel->text().contains(QStringLiteral("falta")),
                "the chin guide does not say its models are missing");
        std::cout << "Guides workflow OK\n";
    }

    static void runBiteRegistrationWorkflow()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1500, 900);
        window.show();
        settle();

        window.m_leFortSegmentMesh = gridBox(-24.0, 24.0, -8.0, 8.0, 0.0, 18.0);
        window.m_genioBodyMesh = gridBox(-25.0, 25.0, -8.0, 8.0, -20.0, -2.0);
        window.m_biteScanMesh = gridBox(-22.0, 22.0, -6.0, 6.0, -3.0, 4.0);
        window.setBiteRegistrationWorkspace(true);
        settle();
        require(window.m_biteSegmentView->meshData(objectActorKey(kLeFortSegLabel)) != nullptr &&
                    window.m_biteSegmentView->meshData(objectActorKey(kGenioBodyLabel)) == nullptr,
                "upper bite stage does not isolate Le Fort");
        require(window.m_biteSegmentView->findChild<QLabel*>()->text().contains(QStringLiteral("LE FORT")),
                "upper bite stage title is not Le Fort");

        window.m_biteLeFortRegistered = true;
        window.syncBiteRegistrationView();
        settle();
        require(window.m_biteSegmentView->meshData(objectActorKey(kLeFortSegLabel)) == nullptr &&
                    window.m_biteSegmentView->meshData(objectActorKey(kGenioBodyLabel)) != nullptr,
                "lower bite stage does not switch to the mandible");

        const auto target = gridBox(-18.0, 18.0, -7.0, 7.0, -5.0, 7.0);
        auto shift = vtkSmartPointer<vtkTransform>::New();
        shift->Translate(1.2, -0.8, 0.6);
        auto shiftedFilter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
        shiftedFilter->SetInputData(target);
        shiftedFilter->SetTransform(shift);
        shiftedFilter->Update();
        QVector<QVector3D> movingHints {{-10.0f + 1.2f, -7.0f - 0.8f, 0.0f + 0.6f},
                                        {0.0f + 1.2f, -7.0f - 0.8f, 0.0f + 0.6f},
                                        {10.0f + 1.2f, -7.0f - 0.8f, 0.0f + 0.6f}};
        QVector<QVector3D> targetHints {{-10.0f, -7.0f, 0.0f}, {0.0f, -7.0f, 0.0f}, {10.0f, -7.0f, 0.0f}};
        QString report;
        QString error;
        auto correction = vtkSmartPointer<vtkMatrix4x4>::New();
        correction->Identity();
        const auto refined = window.refineBiteRegistrationWithIcp(
            shiftedFilter->GetOutput(), target, &report, &error, movingHints, targetHints, correction);
        require(refined && report.contains(QStringLiteral("ICP local mordida")) && error.isEmpty(),
                "landmark-local bite ICP did not run");
        require(std::abs(correction->GetElement(0, 3) + 1.2) < 0.25 &&
                    std::abs(correction->GetElement(1, 3) - 0.8) < 0.25 &&
                    std::abs(correction->GetElement(2, 3) + 0.6) < 0.25,
                "landmark-local bite ICP did not recover the rigid offset");
        std::cout << "Bite registration stages and local ICP OK\n";
    }

    // PLACAS + GUÍA Le Fort: plates on the planned bone, predictive holes carried into the guide's sleeves.
    static void runPlateWorkflow(const QString& artifactsDir)
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1700, 950);
        window.show();
        settle();

        // The anterior maxilla: cranial base above the cut (z >= 10), Le Fort segment below it (z <= 8). The plan
        // lowers the segment 6 mm, advances it 2 mm and turns it 2° about the vertical axis: an 8 mm gap at the
        // cut, which the plates have to bridge.
        const auto cranium = boxMesh({-25.0, 25.0, -10.0, 0.0, 10.0, 30.0}, false, false);
        const auto before = boxMesh({-25.0, 25.0, -10.0, 0.0, -10.0, 8.0}, false, false);
        auto motionTransform = vtkSmartPointer<vtkTransform>::New();
        motionTransform->PostMultiply();
        motionTransform->RotateZ(2.0);
        motionTransform->Translate(0.0, 2.0, -6.0);
        std::array<double, 16> motion{};
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                motion[static_cast<size_t>(4 * r + c)] = motionTransform->GetMatrix()->GetElement(r, c);
        auto moveFilter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
        moveFilter->SetInputData(before);
        moveFilter->SetTransform(motionTransform);
        moveFilter->Update();
        auto planned = vtkSmartPointer<vtkPolyData>::New();
        planned->DeepCopy(moveFilter->GetOutput());

        window.addObjectEntry(QStringLiteral("Segmento Le Fort I"), QColor(230, 220, 200), kLeFortSegLabel);
        window.setRepositionMeshForLabel(kLeFortSegLabel, planned);
        window.m_repositionOriginalMeshes[kLeFortSegLabel] = before; // as REPOSICIÓN keeps it
        window.addObjectEntry(QStringLiteral("Base craneal"), QColor(220, 210, 190), kLeFortCranialLabel);
        window.setRepositionMeshForLabel(kLeFortCranialLabel, cranium);
        window.rememberOsteotomyCut(QStringLiteral("Le Fort I"),
                                    OsteotomyCore::LeFortPath({{{-10.0, 5.0, 9.0}, {10.0, 5.0, 9.0},
                                                                {-20.0, -5.0, 9.0}, {20.0, -5.0, 9.0}}}),
                                    GuideType::LeFort);

        for (auto* tab : window.findChildren<QToolButton*>(QStringLiteral("MT")))
            if (tab->text() == QStringLiteral("GUIAS"))
                tab->click();
        settle();
        require(window.m_guideTypeCombo->currentText().contains(QStringLiteral("Placas")),
                "the Le Fort guide type is not «Placas + guía»");
        require(window.m_guidePlateSection->isVisibleTo(&window), "the custom plates section is missing");
        window.m_guideDetailSpin->setValue(0.5); // coarse: a wiring test

        // 1. A paranasal plate on the right: marking its holes switches to the planned bone.
        window.m_guidePlateSideCombo->setCurrentIndex(window.m_guidePlateSideCombo->findData(static_cast<int>(PlateSide::Right)));
        window.m_guidePlateTemplateCombo->setCurrentIndex(
            window.m_guidePlateTemplateCombo->findData(static_cast<int>(PlateTemplate::Paranasal)));
        window.m_guidePlateHolesButton->setChecked(true);
        settle();
        require(window.m_guidePlannedView && window.m_guidePlannedPrepared.ok && window.m_guidePointMode == 8,
                "marking plate holes did not open the planned bone");
        const auto onSegment = [&motion](double x, double z) {
            return std::array<double, 3>{motion[0] * x + motion[2] * z + motion[3], motion[4] * x + motion[6] * z + motion[7],
                                         motion[8] * x + motion[10] * z + motion[11]};
        };
        const std::vector<std::array<double, 3>> rightHoles{{-10.0, 0.0, 20.0}, {-10.0, 0.0, 14.0},
                                                            onSegment(-10.0, 4.0), onSegment(-10.0, -2.0)};
        for (const auto& p : rightHoles)
            window.onGuidePointPicked(0, p[0], p[1], p[2]);
        require(window.m_guidePendingPlateHoles.size() == 4, "the plate holes were not collected");
        window.createGuidePlate();
        settle();
        require(window.m_guidePlan.plates.size() == 1 && window.m_guidePlateMeshes.size() == 1 &&
                    window.m_guidePlateMeshes[0] && window.m_guidePlateMeshes[0]->GetNumberOfPolys() > 0,
                "the right plate was not built");
        require(window.m_guideView->meshData(kGuidePlateActorBase) != nullptr, "the plate is not shown");

        // 2. An L plate on the left: the piriform arm, "next arm", the buttress arm.
        window.m_guidePlateSideCombo->setCurrentIndex(window.m_guidePlateSideCombo->findData(static_cast<int>(PlateSide::Left)));
        window.m_guidePlateTemplateCombo->setCurrentIndex(
            window.m_guidePlateTemplateCombo->findData(static_cast<int>(PlateTemplate::LShape)));
        window.m_guidePlateHolesButton->setChecked(true);
        for (const auto& p : {std::array<double, 3>{8.0, 0.0, 20.0}, std::array<double, 3>{8.0, 0.0, 14.0},
                              onSegment(8.0, 3.0)})
            window.onGuidePointPicked(0, p[0], p[1], p[2]);
        require(window.m_guidePlateArmButton->isEnabled(), "the L plate does not offer its second arm");
        window.startGuidePlateArm();
        for (const auto& p : {std::array<double, 3>{18.0, 0.0, 20.0}, std::array<double, 3>{18.0, 0.0, 14.0},
                              onSegment(18.0, 3.0)})
            window.onGuidePointPicked(0, p[0], p[1], p[2]);
        window.createGuidePlate();
        settle();
        require(window.m_guidePlan.plates.size() == 2 && window.m_guidePlan.plates[1].kind == PlateTemplate::LShape &&
                    window.m_guidePlan.plates[1].struts.size() == 3,
                "the L plate was not built with two arms and a bar");
        for (const PlateDesign& plate : window.m_guidePlan.plates)
            for (const PlateHole& hole : plate.holes)
                require(hole.bone != PlateBone::Unknown, "a plate hole was not assigned to its bone");
        require(window.m_guidePlateCheckLabel->text().contains(QStringLiteral("Sin avisos")),
                "correct plates raised warnings: " + window.m_guidePlateCheckLabel->text().toStdString());
        QDir().mkpath(artifactsDir);
        // This synthetic face looks towards +y (a CT's anterior is -y), so the camera looks back along -y.
        window.m_guideView->setViewAlongDirection({4.0, 0.0, 10.0}, {0.0, -1.0, 0.0}, {0.0, 0.0, 1.0}, 32.0);
        window.m_guideView->render();
        settle();
        require(window.m_guideView->findChild<QVTKOpenGLNativeWidget*>()->grabFramebuffer().save(
                    QDir(artifactsDir).filePath(QStringLiteral("plates-planned.png"))),
                "the plates screenshot was not written");

        // 3. The predictive holes: the cranial ones stay, the segment's go back to where that bone is before the cut.
        const auto predicted = window.guidePredictiveHoles();
        require(predicted.size() == 10, "not every plate hole was predicted");
        for (const PredictiveHole& hole : predicted) {
            if (hole.bone == PlateBone::Cranial)
                require(std::hypot(std::hypot(hole.preopCenter[0] - hole.plannedCenter[0],
                                              hole.preopCenter[1] - hole.plannedCenter[1]),
                                   hole.preopCenter[2] - hole.plannedCenter[2]) < 1e-6,
                        "a cranial hole moved");
            else
                require(std::abs(hole.preopCenter[1]) < 0.05, "a segment hole did not go back to the pre-operative face");
        }

        // 4. The cutting guide, laid out from the plates and the cut on the bone before it: one piece, the slit on
        //    the osteotomy in pieces between bridges, a sleeve at every predictive hole, four 1.5 mm screws.
        require(window.m_guideGenerateButton->isEnabled(), "the cutting guide cannot be generated from the plates");
        window.m_guideGenerateButton->click();
        settle();
        require(window.m_guideWrapMesh && window.m_guidePrepared.ok, "the guide envelope was not built");
        require(!window.m_guidePlannedView, "the guide was not shown on the bone before the cut");
        require(!window.m_guidePlan.paint.empty() && window.m_guidePlan.slotPlan.size() >= 2 &&
                    window.m_guidePlan.holes.size() == 4,
                "the guide layout lacks its band, slit pieces or fixation screws");
        for (const GuideFixationHole& screw : window.m_guidePlan.holes)
            require(std::abs(screw.diameterMm - 1.5) < 1e-9, "a guide fixation screw is not 1.5 mm");
        require(window.m_guideMesh && window.m_guideMesh->GetNumberOfPolys() > 0, "the guide was not built");
        const QString guideReport = window.m_guideReportLabel->text();
        require(guideReport.contains(QStringLiteral("10 figura(s) sumada(s)")) &&
                    guideReport.contains(QStringLiteral("10 restada(s)")),
                "the guide does not carry one sleeve per predictive hole: " + guideReport.toStdString());
        require(guideReport.contains(QStringLiteral("1 pieza(s)")), "the guide came apart: " + guideReport.toStdString());
        require(guideReport.contains(QStringLiteral("4 agujero(s)")) && guideReport.contains(QStringLiteral("ranura")),
                "the guide report lacks its screws or slit: " + guideReport.toStdString());
        // The sleeve's bore is open where the drill goes, and its body is solid around it.
        auto guideDistance = vtkSmartPointer<vtkImplicitPolyDataDistance>::New();
        guideDistance->SetInput(window.m_guideMesh);
        for (const PredictiveHole& hole : predicted) {
            const auto along = [&hole](double mm, double sideways) {
                return std::array<double, 3>{hole.preopCenter[0] + hole.preopAxis[0] * mm + sideways,
                                             hole.preopCenter[1] + hole.preopAxis[1] * mm,
                                             hole.preopCenter[2] + hole.preopAxis[2] * mm};
            };
            const auto bore = along(2.5, 0.0);
            const auto wall = along(2.5, 1.5);
            require(guideDistance->EvaluateFunction(bore[0], bore[1], bore[2]) > 0.0,
                    "a sleeve's bore is closed at a predictive hole");
            require(guideDistance->EvaluateFunction(wall[0], wall[1], wall[2]) < 0.0,
                    "a sleeve's wall is missing at a predictive hole");
        }
        window.m_guideShowWrapCheck->setChecked(false); // the guide alone, with its sleeves
        window.m_guideView->setViewAlongDirection({4.0, 0.0, 10.0}, {-0.35, -1.0, -0.25}, {0.0, 0.0, 1.0}, 32.0);
        window.m_guideView->render();
        settle();
        require(window.m_guideView->findChild<QVTKOpenGLNativeWidget*>()->grabFramebuffer().save(
                    QDir(artifactsDir).filePath(QStringLiteral("plates-guide.png"))),
                "the guide screenshot was not written");

        // 5. The plates and their fabrication report are exported together.
        QTemporaryDir exportDir;
        require(exportDir.isValid(), "no temporary folder");
        QString exportReport;
        require(window.exportGuidePlateFiles(exportDir.path(), &exportReport), exportReport.toStdString());
        for (const QString& name : {QStringLiteral("placa_1_derecha.stl"), QStringLiteral("placa_2_izquierda.stl"),
                                    QStringLiteral("informe_placas.txt")})
            require(QFileInfo::exists(QDir(exportDir.path()).filePath(name)), "not exported: " + name.toStdString());
        QFile informe(QDir(exportDir.path()).filePath(QStringLiteral("informe_placas.txt")));
        require(informe.open(QIODevice::ReadOnly), "the report cannot be read");
        const QString informeText = QString::fromUtf8(informe.readAll());
        require(informeText.contains(QStringLiteral("Agujeros predictivos")) &&
                    informeText.contains(QStringLiteral("Sin avisos")),
                "the fabrication report is incomplete");

        // 6. The plates travel with the project.
        ProjectState state;
        state.guidesPlan = window.guidePlanJson();
        MainWindow reopened;
        reopened.setAttribute(Qt::WA_DontShowOnScreen);
        reopened.show();
        settle();
        reopened.restoreGuidePlan(state);
        require(reopened.m_guidePlan.plates.size() == 2 && reopened.m_guidePlan.plates[1].holes.size() == 6,
                "the plates did not survive the project");
        require(reopened.m_guidePlateList->count() == 2, "the reloaded plates are not listed");
        std::cout << "Custom plates + predictive guide OK\n";
    }

    // OSTEOTOMIA wizard: Le Fort I → BSSO (6 points, both sides) → genioplasty on the distal segment.
    static void runOsteotomyWorkflow(const QString& artifactsDir)
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1700, 950);
        window.show();
        settle();

        auto volume = vtkSmartPointer<vtkImageData>::New();
        volume->SetDimensions(16, 16, 16);
        volume->SetSpacing(0.5, 0.5, 0.5);
        volume->AllocateScalars(VTK_SHORT, 1);
        window.m_volume = volume;

        window.m_upperCompositeMesh =
            CompositeBlockCore::TagPart(gridBox(-30.0, 30.0, 0.0, 40.0, 0.0, 40.0), CompositeBlockCore::DentalPart);
        auto mandible = vtkSmartPointer<vtkAppendPolyData>::New();
        for (double sign : {1.0, -1.0}) {
            const double a = 20.0 * sign;
            const double b = 30.0 * sign;
            mandible->AddInputData(gridBox(std::min(a, b), std::max(a, b), -10.0, 40.0, -80.0, -60.0));
            mandible->AddInputData(gridBox(std::min(a, b), std::max(a, b), -40.0, -10.0, -80.0, -20.0));
        }
        mandible->AddInputData(gridBox(-30.0, 30.0, 30.0, 40.0, -80.0, -60.0));
        mandible->Update();
        window.m_lowerCompositeMesh = CompositeBlockCore::TagPart(mandible->GetOutput(), CompositeBlockCore::DentalPart);
        window.addObjectEntry(QStringLiteral("Compuesto maxilar"), QColor(200, 190, 170), kUpperCompositeLabel);
        window.addObjectEntry(QStringLiteral("Compuesto mandibular"), QColor(190, 180, 160), kLowerCompositeLabel);

        window.setOsteotomyWorkspace(true);
        settle();
        require(window.m_osteotomyWizard && window.m_osteotomyWizard->step() == OsteotomyWizardPanel::TypeStep,
                "osteotomy wizard did not start at the type step");
        require(window.m_osteotomyView->meshData(objectActorKey(kUpperCompositeLabel)) != nullptr,
                "upper composite not shown in the osteotomy view");

        const auto capture = [&](const QString& name) {
            window.m_osteotomyView->render();
            settle();
            const QImage view = window.m_osteotomyView->findChild<QVTKOpenGLNativeWidget*>()->grabFramebuffer();
            const QImage panel = window.m_osteotomyWizard->grab().toImage();
            QImage image(panel.width() + view.width(), std::max(panel.height(), view.height()), QImage::Format_RGB32);
            image.fill(QColor(30, 30, 32));
            QPainter painter(&image);
            painter.drawImage(0, 0, panel);
            painter.drawImage(panel.width(), 0, view);
            painter.end();
            QDir().mkpath(artifactsDir);
            require(image.save(QDir(artifactsDir).filePath(name)), "osteotomy screenshot not written");
        };
        const auto place = [&](const std::vector<OstPoint3>& points) {
            for (size_t i = 0; i < points.size(); ++i) {
                require(window.m_ostWizard.currentLandmark == static_cast<int>(i), "landmarks did not auto-advance");
                window.onOsteotomyLandmarkAdded(static_cast<int>(i), points[i][0], points[i][1], points[i][2]);
            }
        };

        // ── Le Fort I ─────────────────────────────────────────────────────
        window.selectOsteotomyType(static_cast<int>(OsteotomyType::LeFortI));
        window.osteotomyWizardNext();
        require(window.m_ostWizard.boneLabel == kUpperCompositeLabel, "Le Fort I did not select the maxillary composite");
        window.osteotomyWizardNext();
        require(window.m_osteotomyWizard->step() == OsteotomyWizardPanel::LandmarkStep &&
                    window.m_ostWizard.landmarks.size() == 4,
                "Le Fort I landmark step not reached");
        const std::vector<OstPoint3> leFort = {{-10.0, 35.0, 20.0}, {10.0, 35.0, 21.0}, {-25.0, 10.0, 15.0}, {25.0, 10.0, 14.0}};
        place(leFort);
        require(window.m_ostWizard.planReady && window.m_ostWizard.currentLandmark == -1, "Le Fort I plan not ready");
        {
            // The plan (landmarks and properties) is saved in the project.
            QTemporaryDir dir;
            require(dir.isValid(), "no temporary directory");
            const QString path = dir.filePath(QStringLiteral("osteotomia.maxilloproject"));
            QString error;
            require(ProjectSerializer::save(path, window.collectProjectState(), &error),
                    "osteotomy plan save failed: " + error.toStdString());
            ProjectState loaded;
            require(ProjectSerializer::load(path, loaded, &error), "osteotomy plan load failed: " + error.toStdString());
            MainWindow reopened;
            reopened.setAttribute(Qt::WA_DontShowOnScreen);
            reopened.show();
            settle();
            reopened.applyProjectState(loaded);
            settle();
            require(reopened.m_ostWizard.type == static_cast<int>(OsteotomyType::LeFortI) &&
                        reopened.m_ostWizard.landmarks == window.m_ostWizard.landmarks,
                    "osteotomy landmarks lost after reopening");
        }
        require(window.m_osteotomyView->meshData(kOsteotomyGuideActorKey) != nullptr, "cutting path guide not shown");
        require(window.m_axialView && window.m_axialView->surfaceContourCount() >= 1, "cutting path contour not on the CT slices");
        // Re-indicate a landmark.
        window.onOsteotomyLandmarkRemoved(1, 0);
        require(!window.m_ostWizard.planReady && window.m_ostWizard.currentLandmark == 1, "removing a landmark did not reopen it");
        window.onOsteotomyLandmarkAdded(1, leFort[1][0], leFort[1][1], leFort[1][2]);
        require(window.m_ostWizard.planReady, "re-indicated landmark did not rebuild the plan");

        window.osteotomyWizardNext();
        require(window.m_osteotomyWizard->step() == OsteotomyWizardPanel::PathStep, "path step not reached");
        OsteotomyWizardPanel::PathProperties properties = window.m_osteotomyWizard->pathProperties();
        require(std::abs(properties.widthMm - 120.0) < 1e-9 && std::abs(properties.extensionRightMm - 20.0) < 1e-9,
                "Le Fort I properties are not the ProPlan defaults");
        properties.widthMm = 100.0;
        window.m_osteotomyWizard->setPathProperties(properties);
        window.onOsteotomyPropertiesChanged();
        require(std::abs(window.m_ostWizard.path.widthMm - 100.0) < 1e-9, "width property not applied to the path");
        auto lift = vtkSmartPointer<vtkMatrix4x4>::New();
        lift->Identity();
        lift->SetElement(2, 3, 2.0);
        window.applyOsteotomyGizmo(lift);
        require(OsteotomyCore::PathField(window.m_ostWizard.path, leFort[0]) < -1.5, "gizmo did not move the cutting path");
        window.m_osteotomyView->setStandardView(0);
        capture(QStringLiteral("osteotomy-lefort.png"));

        window.osteotomyWizardNext();
        require(window.m_osteotomyWizard->step() == OsteotomyWizardPanel::FinalizeStep, "Le Fort I cut not applied");
        require(CompositeBlockCore::HasParts(window.m_leFortSegmentMesh) && CompositeBlockCore::HasParts(window.m_leFortCranialMesh),
                "Le Fort I segments lost the dental link");
        require(window.objectEntryExists(kLeFortSegLabel) && window.objectEntryExists(kLeFortCranialLabel) &&
                    window.m_ostWizard.createdLabels.size() == 2,
                "Le Fort I objects not created");
        require(!window.m_osteotomyView->meshData(kOsteotomyGuideActorKey), "guide still shown after the cut");
        window.osteotomyWizardNext(); // Crear otra osteotomía
        require(window.m_osteotomyWizard->step() == OsteotomyWizardPanel::TypeStep, "finalize did not return to the type step");

        // ── BSSO: 6 landmarks, both sides at once ────────────────────────
        window.selectOsteotomyType(static_cast<int>(OsteotomyType::Bsso));
        window.osteotomyWizardNext();
        window.osteotomyWizardNext();
        require(window.m_ostWizard.landmarks.size() == 6, "BSSO does not ask for 6 landmarks");
        place({{20.0, -25.0, -50.0}, {25.0, -8.0, -60.0}, {30.0, 10.0, -75.0},
               {-20.0, -25.0, -50.0}, {-25.0, -8.0, -60.0}, {-30.0, 10.0, -75.0}});
        require(window.m_ostWizard.planReady, "BSSO plan not ready: " + window.m_ostWizard.planError.toStdString());
        require(window.m_osteotomyView->meshData(kOsteotomyGuideActorKey) && window.m_osteotomyView->meshData(kOsteotomyGuideLeftActorKey),
                "BSSO guides not shown for both sides");
        window.osteotomyWizardNext();
        window.m_osteotomyView->setStandardView(0);
        capture(QStringLiteral("osteotomy-bsso.png"));
        window.osteotomyWizardNext();
        require(window.m_osteotomyWizard->step() == OsteotomyWizardPanel::FinalizeStep, "BSSO cut not applied");
        require(CompositeBlockCore::HasParts(window.m_bssoRightProximalMesh) && CompositeBlockCore::HasParts(window.m_bssoLeftProximalMesh) &&
                    CompositeBlockCore::HasParts(window.m_bssoDistalMesh),
                "BSSO segments missing");
        double bounds[6];
        window.m_bssoRightProximalMesh->GetBounds(bounds);
        require(bounds[0] > 15.0 && bounds[5] > -21.0, "right proximal segment does not hold the right ramus");
        require(window.objectEntryExists(kBssoProximalRightLabel) && window.objectEntryExists(kBssoProximalLeftLabel) &&
                    window.objectEntryExists(kBssoDistalLabel),
                "BSSO objects not created");
        window.osteotomyWizardNext();

        // REPOSICIÓN only shows structures that can be hidden: not the combined proximal mesh.
        window.setRepositionWorkspace(true);
        settle();
        require(window.m_repositionView->meshData(objectActorKey(kBssoProximalRightLabel)) != nullptr &&
                    window.m_repositionView->meshData(objectActorKey(kBssoProximalLabel)) == nullptr,
                "REPOSICIÓN shows the combined BSSO proximal mesh that no object entry can hide");
        window.setOsteotomyWorkspace(true);
        settle();

        // ── Genioplasty on the distal segment ─────────────────────────────
        window.selectOsteotomyType(static_cast<int>(OsteotomyType::Genioplasty));
        window.osteotomyWizardNext();
        require(window.m_ostWizard.boneLabel == kBssoDistalLabel, "genioplasty did not default to the distal segment");
        window.osteotomyWizardNext();
        place({{-18.0, 40.0, -82.0}, {-12.0, 40.0, -66.0}, {12.0, 40.0, -66.0}, {18.0, 40.0, -82.0}});
        require(window.m_ostWizard.planReady, "genioplasty plan not ready");
        require(std::abs(window.m_osteotomyWizard->pathProperties().widthMm - 50.0) < 1e-9, "genioplasty width is not 50 mm");
        window.osteotomyWizardNext();
        window.osteotomyWizardNext();
        require(window.m_osteotomyWizard->step() == OsteotomyWizardPanel::FinalizeStep, "genioplasty cut not applied");
        require(window.m_genioSegmentMesh && window.m_genioBodyMesh && !window.m_bssoDistalMesh &&
                    !window.objectEntryExists(kBssoDistalLabel) && window.objectEntryExists(kGenioSegmentLabel),
                "genioplasty did not replace the distal segment");
        window.m_genioSegmentMesh->GetBounds(bounds);
        require(bounds[3] > 39.0 && bounds[4] < -79.0, "chin segment is not the anterior-inferior part");
        std::cout << "Osteotomy wizard: Le Fort I, bilateral BSSO and genioplasty OK\n";
    }

private:
    // Finely tessellated box so clipped cuts are accurate.
    static vtkSmartPointer<vtkPolyData> gridBox(double x0, double x1, double y0, double y1, double z0, double z1)
    {
        auto append = vtkSmartPointer<vtkAppendPolyData>::New();
        const auto face = [&](const OstPoint3& o, const OstPoint3& p1, const OstPoint3& p2) {
            auto plane = vtkSmartPointer<vtkPlaneSource>::New();
            plane->SetOrigin(o[0], o[1], o[2]);
            plane->SetPoint1(p1[0], p1[1], p1[2]);
            plane->SetPoint2(p2[0], p2[1], p2[2]);
            const auto cells = [](const OstPoint3& a, const OstPoint3& b) {
                return std::max(1, static_cast<int>(std::ceil(std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]) / 1.5)));
            };
            plane->SetResolution(cells(o, p1), cells(o, p2));
            plane->Update();
            append->AddInputData(plane->GetOutput());
        };
        face({x0, y0, z0}, {x1, y0, z0}, {x0, y1, z0});
        face({x0, y0, z1}, {x1, y0, z1}, {x0, y1, z1});
        face({x0, y0, z0}, {x1, y0, z0}, {x0, y0, z1});
        face({x0, y1, z0}, {x1, y1, z0}, {x0, y1, z1});
        face({x0, y0, z0}, {x0, y1, z0}, {x0, y0, z1});
        face({x1, y0, z0}, {x1, y1, z0}, {x1, y0, z1});
        append->Update();
        auto triangles = vtkSmartPointer<vtkTriangleFilter>::New();
        triangles->SetInputConnection(append->GetOutputPort());
        triangles->Update();
        auto out = vtkSmartPointer<vtkPolyData>::New();
        out->DeepCopy(triangles->GetOutput());
        return out;
    }

    static QImage composeWorkspace(MainWindow& window)
    {
        const auto grabView = [](Mesh3DView* view) {
            view->render();
            settle();
            return view->findChild<QVTKOpenGLNativeWidget*>()->grabFramebuffer();
        };
        const QImage upper = grabView(window.m_splintUpperView);
        const QImage lower = grabView(window.m_splintLowerView);
        const QImage main = grabView(window.m_splintView);
        const QImage panel = window.m_splintDesignPanel->grab().toImage();
        const int viewsWidth = std::max(upper.width() + lower.width(), main.width());
        QImage image(panel.width() + viewsWidth, std::max(panel.height(), upper.height() + main.height()),
                     QImage::Format_RGB32);
        image.fill(QColor(30, 30, 32));
        QPainter painter(&image);
        painter.drawImage(0, 0, panel);
        painter.drawImage(panel.width(), 0, upper);
        painter.drawImage(panel.width() + upper.width(), 0, lower);
        painter.drawImage(panel.width(), upper.height(), main);
        return image;
    }
};

int main(int argc, char** argv)
{
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);
    app.setFont(QFont("Segoe UI", 10));
    HideDialogs hideDialogs;
    app.installEventFilter(&hideDialogs);
    const QString artifacts = argc > 1
        ? QString::fromLocal8Bit(argv[1])
        : QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../workspace-test-artifacts"));
    // Any unexpected modal dialog would block the run: close it and fail.
    QStringList unexpectedDialogs;
    QTimer dialogWatchdog;
    QObject::connect(&dialogWatchdog, &QTimer::timeout, [&unexpectedDialogs] {
        if (auto* modal = QApplication::activeModalWidget()) {
            const auto* box = qobject_cast<QMessageBox*>(modal);
            unexpectedDialogs << (box ? box->text() : modal->windowTitle());
            modal->close();
        }
    });
    dialogWatchdog.start(250);
    try {
        SplintWorkspaceTests::run(artifacts);
        SplintWorkspaceTests::runModelGuide(artifacts);
        SplintWorkspaceTests::runCompositeWorkflow(artifacts);
        SplintWorkspaceTests::runOrientationWithBonesOnly();
        SplintWorkspaceTests::runSplintFollowsReposition();
        SplintWorkspaceTests::runRepositionAnalysis();
        SplintWorkspaceTests::runBiteRegistrationWorkflow();
        SplintWorkspaceTests::runOsteotomyWorkflow(artifacts);
        SplintWorkspaceTests::runInspectorTabs();
        SplintWorkspaceTests::runGuidesWorkflow(artifacts);
        SplintWorkspaceTests::runPlateWorkflow(artifacts);
        if (!unexpectedDialogs.isEmpty()) {
            std::cerr << "FAIL unexpected dialogs: " << unexpectedDialogs.join(QStringLiteral(" | ")).toStdString() << '\n';
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
