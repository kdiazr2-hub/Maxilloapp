#include "MainWindow.h"
#include "Mesh3DView.h"
#include "ObjectLabels.h"
#include "ProjectSerializer.h"
#include "SplintContourEditCore.h"
#include "SplintDesignPanel.h"
#include "SplintPreviewScheduler.h"
#include "SplintTestGeometry.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QVTKOpenGLNativeWidget.h>

#include <vtkFeatureEdges.h>

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
                        a.lowerPoints == b.lowerPoints && a.params.edgeOffsetMm == b.params.edgeOffsetMm,
                    "design changed after reopening: " + a.name.toStdString());
        }
        require(reopened.objectEntryExists(kIntermediateSplintLabel) && reopened.m_intermediateSplintMesh != nullptr,
                "created splint lost after reopening");
        std::cout << "Splint workspace: preview, contour editing, creation, designs and project OK\n";
    }

private:
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
    try {
        SplintWorkspaceTests::run(artifacts);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
