// ─────────────────────────────────────────────────────────────────────────────
// MainWindow — splint workspace, height-map method (Phase 2).
//
// Named designs (SplintDesignCore), guide points edited in the Mesh3DViews,
// live background preview (SplintPreviewScheduler: coarse 0.4 mm, then refined
// with thickness after 1 s idle), contour editing in the occlusal views and
// final creation with SplintHeightmapGenerator. The classic SplintGenerator
// panel stays available through the "Método" selector.
// ─────────────────────────────────────────────────────────────────────────────

#include "MainWindow.h"

#include "Mesh3DView.h"
#include "ObjectLabels.h"
#include "SplintContourEditCore.h"
#include "SplintDesignPanel.h"
#include "SplintPreviewScheduler.h"

#include <QComboBox>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QRegularExpression>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <vtkCellArray.h>
#include <vtkCleanPolyData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkSTLReader.h>
#include <vtkSTLWriter.h>

#include <algorithm>

namespace
{
constexpr int kHeightmapMethod = 0;
constexpr int kClassicMethod = 1;
constexpr double kPointRadiusMm = 0.6;
constexpr double kContourGrabDistanceMm = 3.0;
constexpr double kContourEditSpacingMm = 0.5;
constexpr double kCoarsePreviewGridMm = 0.4;
const QColor kUpperPointColor(255, 128, 0);
const QColor kLowerPointColor(0, 0, 255);
const QColor kContourColor(230, 30, 30);
const QColor kUpperSourceColor(128, 112, 210);
const QColor kLowerSourceColor(220, 82, 78);
const QColor kSplintColor(244, 238, 220);

vtkSmartPointer<vtkPolyData> contourPolyline(const SplintOcclusalFrame& frame, const std::vector<SplintContourUV>& contours)
{
    auto points = vtkSmartPointer<vtkPoints>::New();
    auto lines = vtkSmartPointer<vtkCellArray>::New();
    for (const SplintContourUV& contour : contours) {
        if (contour.size() < 2)
            continue;
        std::vector<vtkIdType> ids;
        for (const SplintPointUV& p : contour) {
            const SplintPoint3 world = frame.ToWorld(p[0], p[1], 0.0);
            ids.push_back(points->InsertNextPoint(world.data()));
        }
        ids.push_back(ids.front());
        lines->InsertNextCell(static_cast<vtkIdType>(ids.size()), ids.data());
    }
    auto polyline = vtkSmartPointer<vtkPolyData>::New();
    polyline->SetPoints(points);
    polyline->SetLines(lines);
    return polyline;
}

// Cheap content signature: counts, bounds and a sparse point sum.
QString meshFingerprint(vtkPolyData* mesh)
{
    double bounds[6] = {};
    mesh->GetBounds(bounds);
    QString key = QStringLiteral("%1/%2").arg(mesh->GetNumberOfPoints()).arg(mesh->GetNumberOfPolys());
    for (double v : bounds)
        key += QLatin1Char(',') + QString::number(v, 'g', 12);
    const vtkIdType step = std::max<vtkIdType>(1, mesh->GetNumberOfPoints() / 64);
    double sum[3] = {};
    double p[3] = {};
    for (vtkIdType i = 0; i < mesh->GetNumberOfPoints(); i += step) {
        mesh->GetPoint(i, p);
        sum[0] += p[0];
        sum[1] += p[1];
        sum[2] += p[2];
    }
    for (double v : sum)
        key += QLatin1Char(',') + QString::number(v, 'g', 12);
    return key;
}

vtkSmartPointer<vtkPolyData> readStl(const QString& path)
{
    auto reader = vtkSmartPointer<vtkSTLReader>::New();
    reader->SetFileName(QFile::encodeName(path).constData());
    auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
    clean->SetInputConnection(reader->GetOutputPort());
    clean->Update();
    auto mesh = vtkSmartPointer<vtkPolyData>::New();
    mesh->DeepCopy(clean->GetOutput());
    return mesh->GetNumberOfPolys() > 0 ? mesh : nullptr;
}

QJsonArray pointsToJson(const std::vector<SplintPoint3>& points)
{
    QJsonArray array;
    for (const SplintPoint3& p : points)
        array.append(QJsonArray{p[0], p[1], p[2]});
    return array;
}

bool writeTextFile(const QString& path, const QByteArray& content)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(content) == content.size();
}
} // namespace

// ── Panel and wiring ──────────────────────────────────────────────────────────

QWidget* MainWindow::buildSplintMethodPanel(QWidget* parent, QWidget* classicPanel)
{
    auto* host = new QWidget(parent);
    host->setFixedWidth(340);
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto* methodRow = new QHBoxLayout();
    methodRow->setContentsMargins(8, 4, 8, 0);
    methodRow->addWidget(new QLabel(tr("Método:"), host));
    m_splintMethodCombo = new QComboBox(host);
    m_splintMethodCombo->addItem(tr("Mapa de altura (nuevo)"), kHeightmapMethod);
    m_splintMethodCombo->addItem(tr("Clásico"), kClassicMethod);
    methodRow->addWidget(m_splintMethodCombo, 1);
    layout->addLayout(methodRow);

    m_splintMethodStack = new QStackedWidget(host);
    m_splintDesignPanel = new SplintDesignPanel();
    auto* scroll = new QScrollArea(m_splintMethodStack);
    scroll->setWidgetResizable(true);
    scroll->setWidget(m_splintDesignPanel);
    m_splintMethodStack->addWidget(scroll);
    if (auto* classicScroll = qobject_cast<QScrollArea*>(classicPanel))
        classicScroll->setMinimumWidth(0);
    m_splintMethodStack->addWidget(classicPanel);
    layout->addWidget(m_splintMethodStack, 1);

    m_splintPreview = new SplintPreviewScheduler(this);
    m_splintRefineTimer = new QTimer(this);
    m_splintRefineTimer->setSingleShot(true);
    m_splintRefineTimer->setInterval(1000);
    connect(m_splintRefineTimer, &QTimer::timeout, this, &MainWindow::requestRefinedSplintPreview);
    connect(m_splintPreview, &SplintPreviewScheduler::previewReady, this, &MainWindow::onSplintPreviewReady);
    connect(m_splintPreview, &SplintPreviewScheduler::previewFailed, this, &MainWindow::onSplintPreviewFailed);

    auto* panel = m_splintDesignPanel;
    connect(panel, &SplintDesignPanel::designSelected, this, &MainWindow::selectSplintDesign);
    connect(panel, &SplintDesignPanel::newDesignRequested, this, &MainWindow::addSplintDesign);
    connect(panel, &SplintDesignPanel::copyDesignRequested, this, &MainWindow::copySplintDesign);
    connect(panel, &SplintDesignPanel::renameDesignRequested, this, &MainWindow::renameSplintDesign);
    connect(panel, &SplintDesignPanel::deleteDesignRequested, this, &MainWindow::deleteSplintDesign);
    connect(panel, &SplintDesignPanel::sourcesChanged, this, &MainWindow::onSplintSourcesChanged);
    connect(panel, &SplintDesignPanel::loadTestStlRequested, this, &MainWindow::loadSplintTestStl);
    connect(panel, &SplintDesignPanel::pointGroupToggled, this, [this](int group, bool active) {
        setSplintPointGroup(active ? group : -1);
    });
    connect(panel, &SplintDesignPanel::clearPointsRequested, this, &MainWindow::clearSplintDesignPoints);
    connect(panel, &SplintDesignPanel::paramsChanged, this, &MainWindow::onSplintParamsChanged);
    connect(panel, &SplintDesignPanel::showThicknessToggled, this, [this](bool) { updateSplintPreviewMesh(); });
    connect(panel, &SplintDesignPanel::contourEditToggled, this, &MainWindow::setSplintContourEditing);
    connect(panel, &SplintDesignPanel::resetContourRequested, this, &MainWindow::resetSplintContour);
    connect(panel, &SplintDesignPanel::influenceChanged, this, [this](double percent) {
        m_splintInfluencePercent = SplintContourEditCore::ClampInfluence(percent);
    });
    connect(panel, &SplintDesignPanel::createRequested, this, &MainWindow::createHeightmapSplint);
    connect(panel, &SplintDesignPanel::exportRequested, this, &MainWindow::exportHeightmapSplintStl);
    connect(panel, &SplintDesignPanel::exportPointsRequested, this, &MainWindow::exportSplintDesignPoints);
    connect(panel, &SplintDesignPanel::exportReportRequested, this, &MainWindow::exportSplintReport);

    connect(m_splintMethodCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        m_splintMethodStack->setCurrentIndex(index);
        setSplintPointCapture(SplintPointSet::None);
        setSplintPointGroup(-1);
        setSplintContourEditing(false);
        if (m_splintPreview)
            m_splintPreview->cancel();
        if (m_splintRefineTimer)
            m_splintRefineTimer->stop();
        clearSplintPreviewDisplay();
        for (Mesh3DView* view : {m_splintUpperView, m_splintLowerView, m_splintView})
            if (view) view->clearEditablePoints();
        syncSplintView();
        updateButtonStates();
        if (splintHeightmapMethodActive())
            requestSplintPreview();
    });

    if (!m_splintConfirm) {
        m_splintConfirm = [this](const QString& text) {
            return QMessageBox::question(this, tr("Férula"), text, QMessageBox::Yes | QMessageBox::Cancel,
                                         QMessageBox::Cancel) == QMessageBox::Yes;
        };
    }
    if (!m_splintAskName) {
        m_splintAskName = [this](const QString& prompt, const QString& current) -> std::optional<QString> {
            bool ok = false;
            const QString text = QInputDialog::getText(this, tr("Férula"), prompt, QLineEdit::Normal, current, &ok);
            if (!ok)
                return std::nullopt;
            return text;
        };
    }

    ensureSplintDesigns();
    refreshSplintDesignPanel();
    return host;
}

void MainWindow::connectSplintHeightmapViews()
{
    for (Mesh3DView* view : {m_splintUpperView, m_splintLowerView, m_splintView}) {
        if (!view) continue;
        connect(view, &Mesh3DView::editablePointAdded, this, &MainWindow::onSplintEditablePointAdded);
        connect(view, &Mesh3DView::editablePointMoved, this, &MainWindow::onSplintEditablePointMoved);
        connect(view, &Mesh3DView::editablePointDragFinished, this, &MainWindow::onSplintEditablePointDragFinished);
        connect(view, &Mesh3DView::editablePointRemoved, this, &MainWindow::onSplintEditablePointRemoved);
        connect(view, &Mesh3DView::planeDragStarted, this, &MainWindow::onSplintPlaneDragStarted);
        connect(view, &Mesh3DView::planeDragMoved, this, &MainWindow::onSplintPlaneDragMoved);
        connect(view, &Mesh3DView::planeDragFinished, this, &MainWindow::onSplintPlaneDragFinished);
    }
}

bool MainWindow::splintHeightmapMethodActive() const
{
    return m_splintMethodCombo && m_splintMethodCombo->currentData().toInt() == kHeightmapMethod;
}

// ── Designs ───────────────────────────────────────────────────────────────────

SplintDesign* MainWindow::activeSplintDesign()
{
    ensureSplintDesigns();
    return &m_splintDesigns[static_cast<size_t>(m_activeSplintDesign)];
}

const SplintDesign* MainWindow::activeSplintDesign() const
{
    if (m_splintDesigns.empty())
        return nullptr;
    const int index = std::clamp(m_activeSplintDesign, 0, static_cast<int>(m_splintDesigns.size()) - 1);
    return &m_splintDesigns[static_cast<size_t>(index)];
}

void MainWindow::ensureSplintDesigns()
{
    if (m_splintDesigns.empty())
        m_splintDesigns = SplintDesignCore::DefaultDesigns(kLeFortSegLabel, kSplintInitialMandibleChoice,
                                                           kSplintFinalMandibleChoice);
    m_activeSplintDesign = std::clamp(m_activeSplintDesign, 0, static_cast<int>(m_splintDesigns.size()) - 1);
}

void MainWindow::refreshSplintDesignPanel()
{
    if (!m_splintDesignPanel)
        return;
    const SplintDesign& design = *activeSplintDesign();

    QStringList names;
    for (const SplintDesign& d : m_splintDesigns)
        names << d.name;
    m_splintDesignPanel->setDesigns(names, m_activeSplintDesign, design.builtIn);

    std::vector<SplintDesignPanel::SourceOption> upper = {
        {tr("Segmento Le Fort I"), kLeFortSegLabel},
        {tr("Compuesto maxilar"), kUpperCompositeLabel},
        {tr("Maxilar"), 5},
    };
    std::vector<SplintDesignPanel::SourceOption> lower = {
        {tr("Mandíbula inicial"), kSplintInitialMandibleChoice},
        {tr("Mandíbula final"), kSplintFinalMandibleChoice},
        {tr("Post-mentón"), kGenioBodyLabel},
        {tr("Distal BSSO"), kBssoDistalLabel},
        {tr("Compuesto mandibular"), kLowerCompositeLabel},
        {tr("Mandíbula"), 6},
    };
    if (m_splintTestUpperMesh)
        upper.push_back({tr("STL de prueba superior"), kSplintTestUpperChoice});
    if (m_splintTestLowerMesh)
        lower.push_back({tr("STL de prueba inferior"), kSplintTestLowerChoice});
    m_splintDesignPanel->setSourceOptions(upper, lower);
    m_splintDesignPanel->setSources(design.upperSource, design.lowerSource);
    m_splintDesignPanel->setParams(design.params);
    m_splintDesignPanel->setActivePointGroup(m_splintPointGroup);
    m_splintDesignPanel->setContourEditing(m_splintContourEditing);
    m_splintDesignPanel->setInfluencePercent(m_splintInfluencePercent);
    updateSplintPanelState();
}

void MainWindow::updateSplintPanelState()
{
    if (!m_splintDesignPanel)
        return;
    const SplintDesign& design = *activeSplintDesign();
    m_splintDesignPanel->setPointCounts(static_cast<int>(design.upperPoints.size()),
                                        static_cast<int>(design.lowerPoints.size()));
    m_splintDesignPanel->setContourEdited(!design.editedContourUV.empty());
    const bool enoughPoints = design.upperPoints.size() >= 3 && design.lowerPoints.size() >= 3;
    const bool sources = splintSourceMesh(design.upperSource) && splintSourceMesh(design.lowerSource);
    m_splintDesignPanel->setCanCreate(!m_splintHeightmapBuildInProgress && enoughPoints && sources);
    const auto created = repositionMeshForLabel(design.label);
    m_splintDesignPanel->setCanExport(created && created->GetNumberOfPolys() > 0);
}

void MainWindow::selectSplintDesign(int index)
{
    if (index < 0 || index >= static_cast<int>(m_splintDesigns.size()))
        return;
    if (m_splintPreview)
        m_splintPreview->cancel();
    if (m_splintRefineTimer)
        m_splintRefineTimer->stop();
    setSplintContourEditing(false);
    m_activeSplintDesign = index;
    m_splintPreviewValid = false;
    clearSplintPreviewDisplay();
    refreshSplintDesignPanel();
    syncSplintView();
    requestSplintPreview();
}

void MainWindow::selectSplintDesignByLabel(int label)
{
    const int index = SplintDesignCore::IndexOfLabel(m_splintDesigns, label);
    if (index >= 0 && index != m_activeSplintDesign)
        selectSplintDesign(index);
}

void MainWindow::addSplintDesign()
{
    const SplintDesign current = *activeSplintDesign();
    const auto name = m_splintAskName
        ? m_splintAskName(tr("Nombre del diseño nuevo:"), SplintDesignCore::UniqueName(m_splintDesigns, tr("Férula")))
        : std::optional<QString>(tr("Férula"));
    if (!name)
        return;
    int index = -1;
    QString error;
    if (!SplintDesignCore::AddDesign(m_splintDesigns, *name, current.upperSource, current.lowerSource, &index, &error)) {
        QMessageBox::warning(this, tr("Férula"), error);
        return;
    }
    selectSplintDesign(index);
}

void MainWindow::copySplintDesign()
{
    int index = -1;
    QString error;
    if (!SplintDesignCore::CopyDesign(m_splintDesigns, m_activeSplintDesign, &index, &error)) {
        QMessageBox::warning(this, tr("Férula"), error);
        return;
    }
    selectSplintDesign(index);
}

void MainWindow::renameSplintDesign()
{
    const QString currentName = activeSplintDesign()->name;
    const auto name = m_splintAskName ? m_splintAskName(tr("Nuevo nombre del diseño:"), currentName) : std::nullopt;
    if (!name)
        return;
    QString error;
    if (!SplintDesignCore::RenameDesign(m_splintDesigns, m_activeSplintDesign, *name, &error)) {
        QMessageBox::warning(this, tr("Férula"), error);
        return;
    }
    const SplintDesign& design = *activeSplintDesign();
    if (objectEntryExists(design.label))
        addObjectEntry(tr("Férula %1").arg(design.name), objectColorForLabel(design.label), design.label);
    refreshSplintDesignPanel();
}

void MainWindow::deleteSplintDesign()
{
    const SplintDesign design = *activeSplintDesign();
    if (design.builtIn) {
        QMessageBox::information(this, tr("Férula"), tr("Los diseños Intermedia y Final no se pueden borrar."));
        return;
    }
    if (m_splintConfirm &&
        !m_splintConfirm(tr("¿Borrar el diseño «%1»? También se quita su férula creada.").arg(design.name)))
        return;
    QString error;
    if (!SplintDesignCore::RemoveDesign(m_splintDesigns, m_activeSplintDesign, &error)) {
        QMessageBox::warning(this, tr("Férula"), error);
        return;
    }
    removeObjectEntry(design.label);
    for (Mesh3DView* view : {m_mesh3DView, m_repositionView, m_splintUpperView, m_splintLowerView, m_splintView})
        if (view) view->removeMesh(objectActorKey(design.label));
    selectSplintDesign(std::min(m_activeSplintDesign, static_cast<int>(m_splintDesigns.size()) - 1));
    statusBar()->showMessage(tr("Diseño «%1» borrado.").arg(design.name));
}

void MainWindow::onSplintSourcesChanged()
{
    if (!m_splintDesignPanel)
        return;
    SplintDesign* design = activeSplintDesign();
    design->upperSource = m_splintDesignPanel->upperSource();
    design->lowerSource = m_splintDesignPanel->lowerSource();
    m_splintPreviewValid = false;
    clearSplintPreviewDisplay();
    syncSplintView();
    updateSplintPanelState();
    requestSplintPreview();
}

void MainWindow::onSplintParamsChanged()
{
    if (!m_splintDesignPanel)
        return;
    SplintDesign* design = activeSplintDesign();
    design->params = m_splintDesignPanel->params(design->params);
    requestSplintPreview();
}

// ── Guide points ──────────────────────────────────────────────────────────────

void MainWindow::setSplintPointGroup(int group)
{
    m_splintPointGroup = (group == 0 || group == 1) ? group : -1;
    if (m_splintPointGroup >= 0) {
        setSplintContourEditing(false);
        setSplintPointCapture(SplintPointSet::None);
    }
    const bool active = m_splintPointGroup >= 0;
    if (m_splintUpperView) m_splintUpperView->setPointEditMode(active, m_splintPointGroup == 0 ? 0 : -1);
    if (m_splintLowerView) m_splintLowerView->setPointEditMode(active, m_splintPointGroup == 1 ? 1 : -1);
    if (m_splintView) m_splintView->setPointEditMode(active, m_splintPointGroup);
    if (m_splintDesignPanel)
        m_splintDesignPanel->setActivePointGroup(m_splintPointGroup);
    if (active)
        statusBar()->showMessage(m_splintPointGroup == 0
            ? tr("Férula: marque al menos 3 puntos en los dientes superiores.")
            : tr("Férula: marque al menos 3 puntos en los dientes inferiores."));
}

void MainWindow::clearSplintDesignPoints(int group)
{
    SplintDesign* design = activeSplintDesign();
    auto& points = group == 0 ? design->upperPoints : design->lowerPoints;
    if (points.empty())
        return;
    if (!design->editedContourUV.empty()) {
        if (!confirmSplintContourLoss())
            return;
        discardSplintEditedContour(tr("Contorno editado descartado: cambiaron los puntos."));
    }
    points.clear();
    rebuildSplintEditablePoints();
    updateSplintPanelState();
    requestSplintPreview();
}

void MainWindow::onSplintEditablePointAdded(int group, double x, double y, double z)
{
    if (!splintHeightmapMethodActive() || (group != 0 && group != 1))
        return;
    SplintDesign* design = activeSplintDesign();
    if (!design->editedContourUV.empty()) {
        if (!confirmSplintContourLoss())
            return;
        discardSplintEditedContour(tr("Contorno editado descartado: cambiaron los puntos."));
    }
    (group == 0 ? design->upperPoints : design->lowerPoints).push_back({x, y, z});
    rebuildSplintEditablePoints();
    updateSplintPanelState();
    requestSplintPreview();
}

void MainWindow::onSplintEditablePointMoved(int group, int index, double x, double y, double z)
{
    if (!splintHeightmapMethodActive() || (group != 0 && group != 1))
        return;
    SplintDesign* design = activeSplintDesign();
    if (!m_splintPointDragStarted) {
        m_splintPointDragStarted = true;
        m_splintPointDragSnapshot = *design;
        m_splintPointDragRejected = !design->editedContourUV.empty() && !confirmSplintContourLoss();
        if (!m_splintPointDragRejected && !design->editedContourUV.empty())
            discardSplintEditedContour(tr("Contorno editado descartado: cambiaron los puntos."));
    }
    if (m_splintPointDragRejected)
        return;
    auto& points = group == 0 ? design->upperPoints : design->lowerPoints;
    if (index < 0 || index >= static_cast<int>(points.size()))
        return;
    points[static_cast<size_t>(index)] = {x, y, z};
    rebuildSplintEditablePoints();
    requestSplintPreview();
}

void MainWindow::onSplintEditablePointDragFinished(int, int)
{
    if (m_splintPointDragStarted && m_splintPointDragRejected) {
        *activeSplintDesign() = m_splintPointDragSnapshot;
        rebuildSplintEditablePoints();
    }
    m_splintPointDragStarted = false;
    m_splintPointDragRejected = false;
    updateSplintPanelState();
}

void MainWindow::onSplintEditablePointRemoved(int group, int index)
{
    if (!splintHeightmapMethodActive() || (group != 0 && group != 1))
        return;
    SplintDesign* design = activeSplintDesign();
    auto& points = group == 0 ? design->upperPoints : design->lowerPoints;
    if (index < 0 || index >= static_cast<int>(points.size()))
        return;
    if (!design->editedContourUV.empty()) {
        if (!confirmSplintContourLoss())
            return;
        discardSplintEditedContour(tr("Contorno editado descartado: cambiaron los puntos."));
    }
    points.erase(points.begin() + index);
    rebuildSplintEditablePoints();
    updateSplintPanelState();
    requestSplintPreview();
}

void MainWindow::rebuildSplintEditablePoints()
{
    const bool active = splintHeightmapMethodActive();
    const SplintDesign* design = activeSplintDesign();
    const std::vector<SplintPoint3> none;
    const auto& upper = active && design ? design->upperPoints : none;
    const auto& lower = active && design ? design->lowerPoints : none;
    if (m_splintUpperView) {
        m_splintUpperView->setEditablePoints(0, upper, kUpperPointColor, kPointRadiusMm);
        m_splintUpperView->setEditablePoints(1, none, kLowerPointColor, kPointRadiusMm);
    }
    if (m_splintLowerView) {
        m_splintLowerView->setEditablePoints(0, none, kUpperPointColor, kPointRadiusMm);
        m_splintLowerView->setEditablePoints(1, lower, kLowerPointColor, kPointRadiusMm);
    }
    if (m_splintView) {
        m_splintView->setEditablePoints(0, upper, kUpperPointColor, kPointRadiusMm);
        m_splintView->setEditablePoints(1, lower, kLowerPointColor, kPointRadiusMm);
    }
}

bool MainWindow::confirmSplintContourLoss()
{
    return !m_splintConfirm ||
           m_splintConfirm(tr("El contorno editado se perderá porque cambian los puntos. ¿Continuar?"));
}

void MainWindow::discardSplintEditedContour(const QString& reason)
{
    SplintDesign* design = activeSplintDesign();
    if (design->editedContourUV.empty())
        return;
    design->editedContourUV.clear();
    setSplintContourEditing(false);
    updateSplintPanelState();
    statusBar()->showMessage(reason);
}

// ── Preview ───────────────────────────────────────────────────────────────────

vtkSmartPointer<vtkPolyData> MainWindow::splintSourceMesh(int choice) const
{
    if (choice == kSplintTestUpperChoice)
        return m_splintTestUpperMesh;
    if (choice == kSplintTestLowerChoice)
        return m_splintTestLowerMesh;
    const auto mesh = meshForSplintSourceLabel(choice);
    if (!mesh || mesh->GetNumberOfPolys() == 0)
        return nullptr;
    const QString fingerprint = meshFingerprint(mesh);
    SplintSourceCacheEntry& entry = m_splintSourceCache[choice];
    if (!entry.mesh || entry.fingerprint != fingerprint) {
        entry.fingerprint = fingerprint;
        // Composite sources (and their osteotomy segments) carry the linked
        // high-resolution scan: the splint is built on that dental part.
        if (auto dental = CompositeBlockCore::ExtractPart(mesh, CompositeBlockCore::DentalPart)) {
            entry.mesh = dental;
        } else {
            entry.mesh = vtkSmartPointer<vtkPolyData>::New();
            entry.mesh->DeepCopy(mesh);
        }
    }
    return entry.mesh;
}

SplintHeightmapInputs MainWindow::splintInputsForDesign(const SplintDesign& design) const
{
    SplintHeightmapInputs inputs;
    inputs.upperPoints = design.upperPoints;
    inputs.lowerPoints = design.lowerPoints;
    inputs.params = design.params;
    inputs.contourOverrideUV = design.editedContourUV;
    return inputs;
}

void MainWindow::requestSplintPreview()
{
    if (!splintHeightmapMethodActive() || !m_splintPreview || !m_splintDesignPanel)
        return;
    m_splintRefineTimer->stop();
    const SplintDesign& design = *activeSplintDesign();
    const auto upper = splintSourceMesh(design.upperSource);
    const auto lower = splintSourceMesh(design.lowerSource);
    if (!upper || !lower) {
        m_splintPreview->cancel();
        m_splintPreviewValid = false;
        clearSplintPreviewDisplay();
        m_splintDesignPanel->setPreviewStatus(tr("Seleccione fuentes válidas para el maxilar y la mandíbula."));
        return;
    }
    if (design.upperPoints.size() < 3 || design.lowerPoints.size() < 3) {
        m_splintPreview->cancel();
        m_splintPreviewValid = false;
        clearSplintPreviewDisplay();
        m_splintDesignPanel->setPreviewStatus(tr("Marque al menos 3 puntos por arcada (maxilar %1/3, mandíbula %2/3).")
                                                  .arg(design.upperPoints.size()).arg(design.lowerPoints.size()));
        return;
    }
    SplintHeightmapInputs inputs = splintInputsForDesign(design);
    inputs.params.gridResolutionMm = std::max(kCoarsePreviewGridMm, design.params.gridResolutionMm);
    inputs.params.computeThickness = false;
    m_splintCoarseGeneration = m_splintPreview->request(upper, lower, inputs);
    m_splintDesignPanel->setPreviewStatus(tr("Vista previa: calculando…"));
}

void MainWindow::requestRefinedSplintPreview()
{
    if (!splintHeightmapMethodActive() || !m_splintPreview)
        return;
    const SplintDesign& design = *activeSplintDesign();
    const auto upper = splintSourceMesh(design.upperSource);
    const auto lower = splintSourceMesh(design.lowerSource);
    if (!upper || !lower || design.upperPoints.size() < 3 || design.lowerPoints.size() < 3)
        return;
    SplintHeightmapInputs inputs = splintInputsForDesign(design);
    inputs.params.computeThickness = true;
    m_splintPreview->request(upper, lower, inputs);
    if (m_splintDesignPanel)
        m_splintDesignPanel->setPreviewStatus(tr("Vista previa: refinando con grosor…"));
}

void MainWindow::onSplintPreviewReady(quint64 generation, const SplintHeightmapResult& result)
{
    if (!splintHeightmapMethodActive())
        return;
    SplintDesign* design = activeSplintDesign();
    if (!design->editedContourUV.empty() &&
        !SplintDesignCore::SameFrame(design->editedContourFrame, result.frame)) {
        discardSplintEditedContour(tr("Contorno editado descartado: cambió el plano oclusal."));
        requestSplintPreview();
        return;
    }

    const bool coarse = generation == m_splintCoarseGeneration;
    m_splintPreviewResult = result;
    m_splintPreviewValid = true;
    m_splintPreviewRefined = !coarse;
    m_splintLastReport = result.report;
    updateSplintPreviewMesh();
    updateSplintContourOverlay();
    applySplintOcclusalCameras(false);
    if (m_splintContourEditing) {
        for (Mesh3DView* view : {m_splintUpperView, m_splintLowerView})
            if (view) view->setPlaneDragMode(true, result.frame.origin, result.frame.normal);
    }
    if (m_splintDesignPanel) {
        m_splintDesignPanel->setPreviewStatus(coarse
            ? tr("Vista previa rápida lista; refinando…")
            : tr("Vista previa lista. Grosor mínimo %1 mm (P5 %2 mm).")
                  .arg(result.minThicknessMm, 0, 'f', 2).arg(result.p05ThicknessMm, 0, 'f', 2));
        m_splintDesignPanel->setReport(result.report);
    }
    updateSplintPanelState();
    if (coarse)
        m_splintRefineTimer->start();
}

void MainWindow::onSplintPreviewFailed(quint64, const QString& error)
{
    m_splintPreviewValid = false;
    clearSplintPreviewDisplay();
    if (m_splintDesignPanel)
        m_splintDesignPanel->setPreviewStatus(tr("Vista previa no disponible: %1").arg(error));
}

void MainWindow::clearSplintPreviewDisplay()
{
    if (m_splintView)
        m_splintView->removeMesh(kSplintPreviewActorKey);
    for (Mesh3DView* view : {m_splintUpperView, m_splintLowerView})
        if (view) view->removeOverlay(kSplintContourOverlayKey);
}

void MainWindow::updateSplintPreviewMesh()
{
    if (!m_splintView)
        return;
    if (!m_splintPreviewValid || !m_splintPreviewResult.mesh || !splintHeightmapMethodActive()) {
        m_splintView->removeMesh(kSplintPreviewActorKey);
        return;
    }
    const SplintDesign& design = *activeSplintDesign();
    auto mesh = vtkSmartPointer<vtkPolyData>::New();
    mesh->DeepCopy(m_splintPreviewResult.mesh);
    const bool thickness = m_splintDesignPanel && m_splintDesignPanel->showThickness() && m_splintPreviewRefined;
    if (thickness)
        SplintHeightmapGenerator::ApplyThicknessColors(mesh, design.params.minThicknessMm, design.params.maxThicknessMm);
    m_splintView->addMesh(kSplintPreviewActorKey, mesh, tr("Vista previa férula"));
    m_splintView->setMeshColor(kSplintPreviewActorKey, QColor(205, 205, 205));
    m_splintView->setMeshOpacity(kSplintPreviewActorKey, thickness ? 1.0 : 0.45);
    m_splintView->setMeshScalarColoring(kSplintPreviewActorKey, thickness);
    m_splintView->setMeshPickable(kSplintPreviewActorKey, false);
}

void MainWindow::updateSplintContourOverlay()
{
    if (!m_splintPreviewValid || !splintHeightmapMethodActive()) {
        for (Mesh3DView* view : {m_splintUpperView, m_splintLowerView})
            if (view) view->removeOverlay(kSplintContourOverlayKey);
        return;
    }
    const SplintDesign& design = *activeSplintDesign();
    const auto& contours = design.editedContourUV.empty() ? m_splintPreviewResult.contoursUV : design.editedContourUV;
    const auto lines = contourPolyline(m_splintPreviewResult.frame, contours);
    for (Mesh3DView* view : {m_splintUpperView, m_splintLowerView})
        if (view) view->setOverlayPolyline(kSplintContourOverlayKey, lines, kContourColor, 2.5);
}

void MainWindow::applySplintOcclusalCameras(bool force)
{
    if (!m_splintPreviewValid)
        return;
    const SplintOcclusalFrame& frame = m_splintPreviewResult.frame;
    if (!force && m_splintCameraFrameSet && SplintDesignCore::SameFrame(m_splintCameraFrame, frame, 2.0, 0.995))
        return;
    m_splintCameraFrame = frame;
    m_splintCameraFrameSet = true;
    const SplintPoint3 down{-frame.normal[0], -frame.normal[1], -frame.normal[2]};
    // Maxillary teeth are seen from below, mandibular teeth from above; anterior up.
    if (m_splintUpperView)
        m_splintUpperView->setViewAlongDirection(frame.origin, frame.normal, frame.axisV);
    if (m_splintLowerView)
        m_splintLowerView->setViewAlongDirection(frame.origin, down, frame.axisV);
}

// ── Contour editing ───────────────────────────────────────────────────────────

void MainWindow::setSplintContourEditing(bool editing)
{
    if (editing && !m_splintPreviewValid) {
        statusBar()->showMessage(tr("Férula: primero se necesita una vista previa (3 puntos por arcada)."));
        if (m_splintDesignPanel)
            m_splintDesignPanel->setContourEditing(false);
        return;
    }
    m_splintContourEditing = editing;
    if (editing)
        setSplintPointGroup(-1);
    for (Mesh3DView* view : {m_splintUpperView, m_splintLowerView}) {
        if (!view) continue;
        if (editing)
            view->setPlaneDragMode(true, m_splintPreviewResult.frame.origin, m_splintPreviewResult.frame.normal);
        else
            view->setPlaneDragMode(false);
    }
    m_splintDragContour = -1;
    m_splintInfluenceDrag = false;
    if (m_splintDesignPanel)
        m_splintDesignPanel->setContourEditing(editing);
    if (editing)
        statusBar()->showMessage(tr("Férula: arrastre el contorno rojo en las vistas oclusales. "
                                    "Los cambios se pierden si se mueven los puntos."));
}

void MainWindow::resetSplintContour()
{
    SplintDesign* design = activeSplintDesign();
    if (design->editedContourUV.empty())
        return;
    design->editedContourUV.clear();
    updateSplintPanelState();
    updateSplintContourOverlay();
    requestSplintPreview();
}

void MainWindow::onSplintPlaneDragStarted(double x, double y, double z, Qt::KeyboardModifiers modifiers)
{
    m_splintDragContour = -1;
    m_splintInfluenceDrag = modifiers.testFlag(Qt::AltModifier);
    if (!m_splintContourEditing || !m_splintPreviewValid || m_splintInfluenceDrag)
        return;
    const SplintOcclusalFrame& frame = m_splintPreviewResult.frame;
    const SplintPoint3 local = frame.ToLocal({x, y, z});
    SplintDesign* design = activeSplintDesign();
    std::vector<SplintContourUV> contours =
        design->editedContourUV.empty() ? m_splintPreviewResult.contoursUV : design->editedContourUV;
    for (SplintContourUV& contour : contours)
        contour = SplintContourEditCore::Densify(contour, kContourEditSpacingMm);
    const auto handle = SplintContourEditCore::FindHandle(contours, {local[0], local[1]}, kContourGrabDistanceMm);
    if (!handle)
        return;
    design->editedContourUV = std::move(contours);
    design->editedContourFrame = frame;
    m_splintDragContour = handle->contour;
    m_splintDragVertex = handle->vertex;
    m_splintDragStartContour = design->editedContourUV[static_cast<size_t>(handle->contour)];
    m_splintDragStartUV = {local[0], local[1]};
    updateSplintPanelState();
}

void MainWindow::onSplintPlaneDragMoved(double x, double y, double z, double deltaY, Qt::KeyboardModifiers modifiers)
{
    if (!m_splintContourEditing)
        return;
    if (m_splintInfluenceDrag || modifiers.testFlag(Qt::AltModifier)) {
        m_splintInfluencePercent = SplintContourEditCore::InfluenceAfterDrag(m_splintInfluencePercent, deltaY);
        if (m_splintDesignPanel)
            m_splintDesignPanel->setInfluencePercent(m_splintInfluencePercent);
        statusBar()->showMessage(tr("Radio de influencia: %1 %").arg(m_splintInfluencePercent, 0, 'f', 0));
        return;
    }
    SplintDesign* design = activeSplintDesign();
    if (m_splintDragContour < 0 || m_splintDragContour >= static_cast<int>(design->editedContourUV.size()))
        return;
    const SplintPoint3 local = m_splintPreviewResult.frame.ToLocal({x, y, z});
    const SplintPointUV delta{local[0] - m_splintDragStartUV[0], local[1] - m_splintDragStartUV[1]};
    design->editedContourUV[static_cast<size_t>(m_splintDragContour)] =
        SplintContourEditCore::Drag(m_splintDragStartContour, m_splintDragVertex, delta, m_splintInfluencePercent);
    updateSplintContourOverlay();
}

void MainWindow::onSplintPlaneDragFinished(double, double, double)
{
    const bool contourChanged = m_splintDragContour >= 0;
    m_splintDragContour = -1;
    m_splintInfluenceDrag = false;
    if (contourChanged)
        requestSplintPreview();
}

// ── Views ─────────────────────────────────────────────────────────────────────

void MainWindow::syncSplintHeightmapView()
{
    for (Mesh3DView* view : {m_splintUpperView, m_splintLowerView, m_splintView})
        if (view) view->clearMeshes();

    const SplintDesign& design = *activeSplintDesign();
    const auto addTo = [](Mesh3DView* view, int key, vtkPolyData* mesh, const QString& name, const QColor& color) {
        if (!view || !mesh) return;
        view->addMesh(key, mesh, name);
        view->setMeshColor(key, color);
        view->setMeshOpacity(key, 1.0);
    };
    const auto upper = splintSourceMesh(design.upperSource);
    const auto lower = splintSourceMesh(design.lowerSource);
    const int upperKey = objectActorKey(design.upperSource);
    const int lowerKey = objectActorKey(design.lowerSource);
    addTo(m_splintUpperView, upperKey, upper, tr("Maxilar"), kUpperSourceColor);
    addTo(m_splintView, upperKey, upper, tr("Maxilar"), kUpperSourceColor);
    addTo(m_splintLowerView, lowerKey, lower, tr("Mandíbula"), kLowerSourceColor);
    addTo(m_splintView, lowerKey, lower, tr("Mandíbula"), kLowerSourceColor);
    if (const auto created = repositionMeshForLabel(design.label)) {
        const int key = objectActorKey(design.label);
        addTo(m_splintView, key, created, tr("Férula %1").arg(design.name), QColor(235, 243, 248));
        m_splintView->setMeshVisible(key, objectEntryVisible(design.label));
        m_splintView->setMeshPickable(key, false);
    }

    rebuildSplintEditablePoints();
    updateSplintPreviewMesh();
    updateSplintContourOverlay();
    if (m_splintCameraFrameSet && m_splintPreviewValid) {
        applySplintOcclusalCameras(true);
    } else {
        if (m_splintUpperView) m_splintUpperView->setStandardView(3);
        if (m_splintLowerView) m_splintLowerView->setStandardView(4);
    }
    for (Mesh3DView* view : {m_splintUpperView, m_splintLowerView, m_splintView})
        if (view) view->render();
}

// ── Creation, export, test sources ────────────────────────────────────────────

void MainWindow::createHeightmapSplint()
{
    if (m_splintHeightmapBuildInProgress)
        return;
    const SplintDesign design = *activeSplintDesign();
    const auto upper = splintSourceMesh(design.upperSource);
    const auto lower = splintSourceMesh(design.lowerSource);
    if (!upper || !lower) {
        QMessageBox::warning(this, tr("Férula"), tr("Seleccione fuentes válidas para el maxilar y la mandíbula."));
        return;
    }
    if (design.upperPoints.size() < 3 || design.lowerPoints.size() < 3) {
        QMessageBox::warning(this, tr("Férula"),
                             tr("Marque al menos 3 puntos en los dientes superiores y 3 en los inferiores."));
        return;
    }

    SplintHeightmapInputs inputs = splintInputsForDesign(design);
    inputs.params.computeThickness = true;
    m_splintHeightmapBuildInProgress = true;
    if (m_progressBar) {
        m_progressBar->setRange(0, 0);
        m_progressBar->setVisible(true);
    }
    statusBar()->showMessage(tr("Creando férula «%1»…").arg(design.name));
    updateSplintPanelState();

    auto* watcher = new QFutureWatcher<SplintHeightmapResult>(this);
    connect(watcher, &QFutureWatcher<SplintHeightmapResult>::finished, this,
            [this, watcher, label = design.label, name = design.name] {
                const SplintHeightmapResult result = watcher->result();
                watcher->deleteLater();
                onHeightmapSplintCreated(label, name, result);
            });
    watcher->setFuture(QtConcurrent::run([upper, lower, inputs]() mutable {
        inputs.upperTeeth = upper;
        inputs.lowerTeeth = lower;
        return SplintHeightmapGenerator::Generate(inputs);
    }));
}

void MainWindow::onHeightmapSplintCreated(int label, const QString& designName, const SplintHeightmapResult& result)
{
    m_splintHeightmapBuildInProgress = false;
    if (m_progressBar) {
        m_progressBar->setRange(0, 100);
        m_progressBar->setVisible(false);
    }
    if (!result.ok) {
        updateSplintPanelState();
        QMessageBox::warning(this, tr("Férula"), tr("No se pudo crear la férula:\n%1").arg(result.error));
        return;
    }
    addObjectEntry(tr("Férula %1").arg(designName), kSplintColor, label);
    setRepositionMeshForLabel(label, result.mesh);
    m_splintLastReport = result.report;
    if (m_splintDesignPanel)
        m_splintDesignPanel->setReport(result.report);
    syncSplintView();
    updateSplintPanelState();
    updateButtonStates();
    statusBar()->showMessage(tr("Férula «%1» creada. Grosor mínimo %2 mm (P5 %3 mm).")
                                 .arg(designName)
                                 .arg(result.minThicknessMm, 0, 'f', 2)
                                 .arg(result.p05ThicknessMm, 0, 'f', 2));
}

void MainWindow::exportHeightmapSplintStl()
{
    const SplintDesign& design = *activeSplintDesign();
    const auto mesh = repositionMeshForLabel(design.label);
    if (!mesh || mesh->GetNumberOfPolys() == 0) {
        QMessageBox::warning(this, tr("Férula"), tr("Primero cree la férula «%1».").arg(design.name));
        return;
    }
    QString fileName = design.name;
    fileName.replace(QRegularExpression(QStringLiteral("[^\\w\\-]+")), QStringLiteral("_"));
    const QString path = QFileDialog::getSaveFileName(this, tr("Exportar férula STL"),
                                                      QStringLiteral("ferula_%1.stl").arg(fileName),
                                                      tr("STL (*.stl)"));
    if (path.isEmpty())
        return;
    auto writer = vtkSmartPointer<vtkSTLWriter>::New();
    writer->SetFileName(QFile::encodeName(path).constData());
    writer->SetInputData(mesh);
    writer->SetFileTypeToBinary();
    if (writer->Write() == 0) {
        QMessageBox::warning(this, tr("Férula"), tr("No se pudo escribir %1.").arg(path));
        return;
    }
    statusBar()->showMessage(tr("Férula exportada: %1").arg(path));
}

void MainWindow::loadSplintTestStl()
{
    const QString upperPath = QFileDialog::getOpenFileName(this, tr("STL de prueba: dientes superiores"),
                                                           QString(), tr("STL (*.stl)"));
    if (upperPath.isEmpty())
        return;
    const QString lowerPath = QFileDialog::getOpenFileName(this, tr("STL de prueba: dientes inferiores"),
                                                           QFileInfo(upperPath).absolutePath(), tr("STL (*.stl)"));
    if (lowerPath.isEmpty())
        return;
    const auto upper = readStl(upperPath);
    const auto lower = readStl(lowerPath);
    if (!upper || !lower) {
        QMessageBox::warning(this, tr("Férula"), tr("No se pudo leer %1.").arg(!upper ? upperPath : lowerPath));
        return;
    }
    setSplintTestSources(upper, lower);
}

void MainWindow::setSplintTestSources(vtkSmartPointer<vtkPolyData> upper, vtkSmartPointer<vtkPolyData> lower)
{
    m_splintTestUpperMesh = upper;
    m_splintTestLowerMesh = lower;
    SplintDesign* design = activeSplintDesign();
    design->upperSource = kSplintTestUpperChoice;
    design->lowerSource = kSplintTestLowerChoice;
    m_splintPreviewValid = false;
    m_splintCameraFrameSet = false;
    refreshSplintDesignPanel();
    syncSplintView();
    requestSplintPreview();
    statusBar()->showMessage(tr("STL de prueba cargados: %1 triángulos superiores, %2 inferiores.")
                                 .arg(upper ? upper->GetNumberOfPolys() : 0)
                                 .arg(lower ? lower->GetNumberOfPolys() : 0));
}

void MainWindow::exportSplintDesignPoints()
{
    const SplintDesign& design = *activeSplintDesign();
    const QString path = QFileDialog::getSaveFileName(this, tr("Exportar puntos de férula"),
                                                      QStringLiteral("puntos_ferula.json"), tr("JSON (*.json)"));
    if (path.isEmpty())
        return;
    QJsonObject root;
    root[QStringLiteral("design")] = design.name;
    root[QStringLiteral("upperPoints")] = pointsToJson(design.upperPoints);
    root[QStringLiteral("lowerPoints")] = pointsToJson(design.lowerPoints);
    root[QStringLiteral("params")] = SplintDesignCore::ParamsToJson(design.params);
    if (!writeTextFile(path, QJsonDocument(root).toJson(QJsonDocument::Indented))) {
        QMessageBox::warning(this, tr("Férula"), tr("No se pudo escribir %1.").arg(path));
        return;
    }
    statusBar()->showMessage(tr("Puntos exportados: %1").arg(path));
}

void MainWindow::exportSplintReport()
{
    const SplintDesign& design = *activeSplintDesign();
    if (m_splintLastReport.isEmpty()) {
        QMessageBox::information(this, tr("Férula"), tr("Todavía no hay informe: calcule una vista previa o cree la férula."));
        return;
    }
    const QString path = QFileDialog::getSaveFileName(this, tr("Exportar informe de férula"),
                                                      QStringLiteral("informe_ferula.txt"), tr("Texto (*.txt)"));
    if (path.isEmpty())
        return;
    const QString text = tr("Diseño: %1\nFecha: %2\n\n%3\n")
                             .arg(design.name, QDateTime::currentDateTime().toString(Qt::ISODate), m_splintLastReport);
    if (!writeTextFile(path, text.toUtf8())) {
        QMessageBox::warning(this, tr("Férula"), tr("No se pudo escribir %1.").arg(path));
        return;
    }
    statusBar()->showMessage(tr("Informe exportado: %1").arg(path));
}

// ── Project ───────────────────────────────────────────────────────────────────

void MainWindow::restoreSplintDesigns(const ProjectState& state)
{
    const auto defaults = SplintDesignCore::DefaultDesigns(kLeFortSegLabel, kSplintInitialMandibleChoice,
                                                           kSplintFinalMandibleChoice);
    m_splintDesigns = SplintDesignCore::DesignsFromJson(state.splintDesigns, defaults);
    m_activeSplintDesign = 0;
    for (size_t i = 0; i < m_splintDesigns.size(); ++i)
        if (m_splintDesigns[i].id == state.activeSplintDesignId)
            m_activeSplintDesign = static_cast<int>(i);
    // Extra designs' splints come back as generic object meshes; the names in
    // the object table follow the design names.
    for (const SplintDesign& design : m_splintDesigns)
        if (!design.builtIn && objectEntryExists(design.label))
            addObjectEntry(tr("Férula %1").arg(design.name), objectColorForLabel(design.label), design.label);

    if (m_splintPreview)
        m_splintPreview->cancel();
    m_splintPreviewValid = false;
    m_splintCameraFrameSet = false;
    m_splintSourceCache.clear();
    m_splintTestUpperMesh = nullptr;
    m_splintTestLowerMesh = nullptr;
    m_splintLastReport.clear();
    setSplintPointGroup(-1);
    setSplintContourEditing(false);
    clearSplintPreviewDisplay();
    refreshSplintDesignPanel();
    rebuildSplintEditablePoints();
}
