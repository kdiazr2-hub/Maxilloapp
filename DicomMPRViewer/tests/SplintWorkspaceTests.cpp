#include "MainWindow.h"
#include "MPRView.h"
#include "Mesh3DView.h"
#include "ObjectLabels.h"
#include "ProjectSerializer.h"
#include "SplintContourEditCore.h"
#include "SplintDesignPanel.h"
#include "SplintPreviewScheduler.h"
#include "SplintTestGeometry.h"
#include "OsteotomyWizardPanel.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFont>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QVTKOpenGLNativeWidget.h>

#include <QDoubleSpinBox>
#include <QMessageBox>
#include <QStackedWidget>

#include <vtkAppendPolyData.h>
#include <vtkClipPolyData.h>
#include <vtkPlaneSource.h>
#include <vtkTriangleFilter.h>
#include <vtkFeatureEdges.h>
#include <vtkImageData.h>
#include <vtkMatrix4x4.h>
#include <vtkPlane.h>

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
        setJaw(true);
        window.setModelsWorkspace(true);
        settle();

        // Fine adjustment: the registered scan is drawn on the CT slices.
        window.updateModelWorkflowUi();
        require(window.m_axialView && window.m_axialView->surfaceContourCount() == 1,
                "registered scan contour not sent to the CT slices");

        // Block stage.
        window.m_modelStepStack->setCurrentIndex(0);
        window.createDentalCompositeModels();
        require(window.m_compositeStage == MainWindow::CompositeStage::Block, "composite did not open the block stage");
        require(window.m_upperCompositeBlock.valid && std::abs(window.m_upperCompositeBlock.sizeMm[2] - 15.0) < 1e-9,
                "initial cutting block is not 15 mm thick");
        require(window.m_modelMatchView->meshData(kCompositeBlockActorKey) != nullptr, "cutting block not shown");
        require(window.m_compositeBlockPanel->isVisibleTo(&window), "block panel not shown");
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
        std::cout << "Composite workspace: block, mandatory review, link, splint source, union and project OK\n";
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

        // ── Genioplasty on the distal segment ─────────────────────────────
        window.selectOsteotomyType(static_cast<int>(OsteotomyType::Genioplasty));
        window.osteotomyWizardNext();
        require(window.m_ostWizard.boneLabel == kBssoDistalLabel, "genioplasty did not default to the distal segment");
        window.osteotomyWizardNext();
        place({{-15.0, 40.0, -65.0}, {-15.0, 30.0, -80.0}, {15.0, 40.0, -65.0}, {15.0, 30.0, -80.0}});
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
        SplintWorkspaceTests::runCompositeWorkflow(artifacts);
        SplintWorkspaceTests::runOsteotomyWorkflow(artifacts);
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
