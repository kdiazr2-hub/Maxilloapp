// ─────────────────────────────────────────────────────────────────────────────
// MainWindow — GUIAS: surgical guides, the step after FERULA.
//
// The panel walks the same five steps as 3-matic's Design tab: pick the models,
// wrap them, mark the support region, place the saw slots on the osteotomies
// already planned, drill the fixation holes, and build. The geometry is all in
// the cores (`WrapCore`, `GuideDesignCore`); this file only collects what the
// user decides into a `GuidePlan` and shows the result.
// ─────────────────────────────────────────────────────────────────────────────

#include "MainWindow.h"

#include "Mesh3DView.h"
#include "MeshRepairCore.h"
#include "ObjectLabels.h"
#include "SplintHeightmapGenerator.h"

#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStatusBar>
#include <QVBoxLayout>

#include <vtkPolyData.h>
#include <vtkSTLWriter.h>

#include <algorithm>

// Defined in MainWindowModels.cpp.
QString guidedSidePanelStyle(const QString& objectName);
// Defined in MainWindow.cpp.
QString meshLabelName(int label);

namespace
{
constexpr int kModeNone = 0;
constexpr int kModeRegion = 1;
constexpr int kModeSlotEnds = 2;
constexpr int kModeHoles = 3;

const QColor kGuideColor(214, 226, 240);
const QColor kWrapColor(170, 180, 190);
const QColor kRegionColor(10, 132, 255);
const QColor kSlotEndColor(255, 159, 10);
const QColor kHoleColor(52, 199, 89);

// Models worth offering for the envelope, in the order a surgeon thinks of them.
const std::vector<int> kSourceCandidates = {kLeFortSegLabel,  kLeFortCranialLabel, kBssoDistalLabel,
                                            kBssoProximalRightLabel, kBssoProximalLeftLabel, kGenioBodyLabel,
                                            kGenioSegmentLabel, kUpperCompositeLabel, kLowerCompositeLabel, 5, 6};
} // namespace

QWidget* MainWindow::buildGuideControlPanel(QWidget* parent)
{
    auto* scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setFixedWidth(320);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* panel = new QWidget();
    panel->setObjectName(QStringLiteral("GuideControlPanel"));
    panel->setStyleSheet(guidedSidePanelStyle(panel->objectName()) +
                         QStringLiteral("#GuideControlPanel QPushButton { background:#2c2c2e; color:#f5f5f7;"
                                        "  border:1px solid #3a3a3c; border-radius:8px; padding:7px 10px;"
                                        "  font-size:11px; }"
                                        "#GuideControlPanel QPushButton:hover { background:#3a3a3c; }"
                                        "#GuideControlPanel QPushButton:checked { background:#0a84ff;"
                                        "  border-color:#64d2ff; color:#ffffff; font-weight:700; }"
                                        "#GuideControlPanel QPushButton:disabled { background:#232325;"
                                        "  border-color:#2c2c2e; color:#6e6e73; }"
                                        "#GuideControlPanel QListWidget { background:#242426; color:#f5f5f7;"
                                        "  border:1px solid #3a3a3c; border-radius:8px; font-size:11px; }"
                                        "#GuideControlPanel QLabel { color:#c7c7cc; font-size:11px; }"));
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(6);

    auto* title = new QLabel(tr("Guías quirúrgicas"), panel);
    title->setObjectName(QStringLiteral("GuidedPanelTitle"));
    layout->addWidget(title);
    m_guideHintLabel = new QLabel(panel);
    m_guideHintLabel->setWordWrap(true);
    m_guideHintLabel->setStyleSheet(QStringLiteral("color:#f5f5f7; font-size:11px; padding:2px 0 6px 0;"));
    layout->addWidget(m_guideHintLabel);

    const auto section = [&](const QString& text) {
        auto* label = new QLabel(text, panel);
        label->setObjectName(QStringLiteral("GuidedPanelSection"));
        layout->addWidget(label);
    };
    const auto spin = [&](double value, double lo, double hi, double step) {
        auto* box = new QDoubleSpinBox(panel);
        box->setRange(lo, hi);
        box->setSingleStep(step);
        box->setDecimals(2);
        box->setValue(value);
        box->setSuffix(tr(" mm"));
        return box;
    };

    // ── 1. Models and envelope ────────────────────────────────────────────
    section(tr("1. MODELOS DE LA GUÍA"));
    m_guideSourceList = new QListWidget(panel);
    m_guideSourceList->setMaximumHeight(110);
    layout->addWidget(m_guideSourceList);
    auto* wrapForm = new QFormLayout();
    m_guideGapSpin = spin(1.5, 0.0, 10.0, 0.1);
    m_guideDetailSpin = spin(0.3, 0.1, 1.0, 0.05);
    wrapForm->addRow(tr("Cierre de huecos:"), m_guideGapSpin);
    wrapForm->addRow(tr("Detalle:"), m_guideDetailSpin);
    layout->addLayout(wrapForm);
    m_guideWrapButton = new QPushButton(tr("Calcular envolvente"), panel);
    connect(m_guideWrapButton, &QPushButton::clicked, this, &MainWindow::computeGuideWrap);
    layout->addWidget(m_guideWrapButton);

    // ── 2. Support region ─────────────────────────────────────────────────
    section(tr("2. ZONA DE APOYO"));
    m_guideRegionButton = new QPushButton(tr("Marcar zona"), panel);
    m_guideRegionButton->setCheckable(true);
    connect(m_guideRegionButton, &QPushButton::toggled, this,
            [this](bool on) { setGuidePointMode(on ? kModeRegion : kModeNone); });
    layout->addWidget(m_guideRegionButton);
    auto* clearRegion = new QPushButton(tr("Borrar zona"), panel);
    connect(clearRegion, &QPushButton::clicked, this, &MainWindow::clearGuideRegion);
    layout->addWidget(clearRegion);

    // ── 3. Saw slots ──────────────────────────────────────────────────────
    section(tr("3. RANURAS DE SIERRA"));
    m_guideCutList = new QListWidget(panel);
    m_guideCutList->setMaximumHeight(90);
    connect(m_guideCutList, &QListWidget::itemChanged, this, [this](QListWidgetItem*) { updateGuideUi(); });
    layout->addWidget(m_guideCutList);
    m_guideSlotEndsButton = new QPushButton(tr("Marcar extremos"), panel);
    m_guideSlotEndsButton->setCheckable(true);
    connect(m_guideSlotEndsButton, &QPushButton::toggled, this,
            [this](bool on) { setGuidePointMode(on ? kModeSlotEnds : kModeNone); });
    layout->addWidget(m_guideSlotEndsButton);
    auto* clearEnds = new QPushButton(tr("Borrar extremos"), panel);
    connect(clearEnds, &QPushButton::clicked, this, &MainWindow::clearGuideSlotEnds);
    layout->addWidget(clearEnds);
    auto* bladeForm = new QFormLayout();
    m_guideBladeSpin = spin(0.6, 0.2, 2.0, 0.1);
    m_guideMarginSpin = spin(2.0, 0.0, 10.0, 0.5);
    bladeForm->addRow(tr("Hoja de sierra:"), m_guideBladeSpin);
    bladeForm->addRow(tr("Margen al borde:"), m_guideMarginSpin);
    layout->addLayout(bladeForm);

    // ── 4. Fixation holes ─────────────────────────────────────────────────
    section(tr("4. AGUJEROS DE FIJACIÓN"));
    m_guideHoleButton = new QPushButton(tr("Marcar agujeros"), panel);
    m_guideHoleButton->setCheckable(true);
    connect(m_guideHoleButton, &QPushButton::toggled, this,
            [this](bool on) { setGuidePointMode(on ? kModeHoles : kModeNone); });
    layout->addWidget(m_guideHoleButton);
    auto* holeForm = new QFormLayout();
    m_guideHoleDiameterSpin = spin(2.0, 0.5, 6.0, 0.1);
    holeForm->addRow(tr("Diámetro:"), m_guideHoleDiameterSpin);
    layout->addLayout(holeForm);
    auto* clearHoles = new QPushButton(tr("Borrar agujeros"), panel);
    connect(clearHoles, &QPushButton::clicked, this, &MainWindow::clearGuideHoles);
    layout->addWidget(clearHoles);

    // ── 5. Build ──────────────────────────────────────────────────────────
    section(tr("5. CREAR LA GUÍA"));
    auto* guideForm = new QFormLayout();
    m_guideThicknessSpin = spin(2.5, 0.5, 10.0, 0.1);
    m_guideClearanceSpin = spin(0.1, 0.0, 2.0, 0.05);
    guideForm->addRow(tr("Espesor:"), m_guideThicknessSpin);
    guideForm->addRow(tr("Holgura:"), m_guideClearanceSpin);
    layout->addLayout(guideForm);
    m_guideBuildButton = new QPushButton(tr("Crear guía"), panel);
    connect(m_guideBuildButton, &QPushButton::clicked, this, &MainWindow::buildGuideMesh);
    layout->addWidget(m_guideBuildButton);
    m_guideThicknessCheck = new QCheckBox(tr("Mapa de espesor"), panel);
    connect(m_guideThicknessCheck, &QCheckBox::toggled, this, [this](bool) { applyGuideThicknessColors(); });
    layout->addWidget(m_guideThicknessCheck);
    m_guideExportButton = new QPushButton(tr("Exportar STL"), panel);
    connect(m_guideExportButton, &QPushButton::clicked, this, &MainWindow::exportGuideStl);
    layout->addWidget(m_guideExportButton);

    m_guideReportLabel = new QLabel(panel);
    m_guideReportLabel->setWordWrap(true);
    m_guideReportLabel->setObjectName(QStringLiteral("MutedText"));
    layout->addWidget(m_guideReportLabel);
    layout->addStretch(1);
    scroll->setWidget(panel);
    return scroll;
}

void MainWindow::setGuidesWorkspace(bool enabled)
{
    if (m_viewModeStack && enabled)
        m_viewModeStack->setCurrentIndex(7);
    if (!enabled) {
        setGuidePointMode(kModeNone);
        return;
    }
    refreshGuideSources();
    refreshGuideCutList();
    syncGuideView();
    updateGuideUi();
}

void MainWindow::refreshGuideSources()
{
    if (!m_guideSourceList)
        return;
    const bool firstTime = m_guideSourceList->count() == 0 && m_guidePlan.sourceLabels.empty();
    QSet<int> checked;
    for (int row = 0; row < m_guideSourceList->count(); ++row) {
        auto* item = m_guideSourceList->item(row);
        if (item->checkState() == Qt::Checked)
            checked.insert(item->data(Qt::UserRole).toInt());
    }
    for (int label : m_guidePlan.sourceLabels)
        checked.insert(label);

    QSignalBlocker blocker(m_guideSourceList);
    m_guideSourceList->clear();
    for (int label : kSourceCandidates) {
        const auto mesh = repositionMeshForLabel(label);
        if (!mesh || mesh->GetNumberOfPolys() == 0)
            continue;
        auto* item = new QListWidgetItem(meshLabelName(label), m_guideSourceList);
        item->setData(Qt::UserRole, label);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        // The first time, the models the surgeon is looking at: the visible ones.
        const bool on = checked.contains(label) || (firstTime && objectEntryVisible(label));
        item->setCheckState(on ? Qt::Checked : Qt::Unchecked);
    }
}

void MainWindow::refreshGuideCutList()
{
    if (!m_guideCutList)
        return;
    // The osteotomy being planned counts too, so a guide can be designed before the cut is executed.
    if (m_ostWizard.path.valid)
        rememberOsteotomyCut(tr("Trayectoria actual"), m_ostWizard.path);

    QSet<int> checked;
    for (int row = 0; row < m_guideCutList->count(); ++row)
        if (m_guideCutList->item(row)->checkState() == Qt::Checked)
            checked.insert(row);

    QSignalBlocker blocker(m_guideCutList);
    m_guideCutList->clear();
    for (size_t i = 0; i < m_guideCuts.size(); ++i) {
        auto* item = new QListWidgetItem(m_guideCuts[i].first, m_guideCutList);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(checked.contains(static_cast<int>(i)) ? Qt::Checked : Qt::Unchecked);
    }
}

void MainWindow::rememberOsteotomyCut(const QString& name, const OsteotomyPath& path)
{
    if (!path.valid)
        return;
    for (auto& cut : m_guideCuts) {
        if (cut.first == name) {
            cut.second = path;
            return;
        }
    }
    m_guideCuts.emplace_back(name, path);
}

void MainWindow::computeGuideWrap()
{
    std::vector<vtkPolyData*> meshes;
    std::vector<int> labels;
    for (int row = 0; row < (m_guideSourceList ? m_guideSourceList->count() : 0); ++row) {
        auto* item = m_guideSourceList->item(row);
        if (item->checkState() != Qt::Checked)
            continue;
        const int label = item->data(Qt::UserRole).toInt();
        if (const auto mesh = repositionMeshForLabel(label)) {
            meshes.push_back(mesh);
            labels.push_back(label);
        }
    }
    if (meshes.empty()) {
        QMessageBox::warning(this, tr("Guías"), tr("Marque al menos un modelo para la envolvente."));
        return;
    }
    m_guidePlan.sourceLabels = labels;
    m_guidePlan.wrap.gapClosingMm = m_guideGapSpin->value();
    m_guidePlan.wrap.smallestDetailMm = m_guideDetailSpin->value();

    statusBar()->showMessage(tr("Guías: calculando la envolvente…"));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const WrapResult wrap = WrapCore::Wrap(meshes, m_guidePlan.wrap);
    QApplication::restoreOverrideCursor();
    if (!wrap.ok) {
        QMessageBox::warning(this, tr("Guías"), wrap.error);
        return;
    }
    m_guideWrapMesh = wrap.mesh;
    m_guidePrepared = GuidePreparation{}; // the envelope changed: it has to be measured again
    syncGuideView();
    updateGuideUi();
    if (m_guideReportLabel)
        m_guideReportLabel->setText(wrap.report);
    statusBar()->showMessage(wrap.report);
}

void MainWindow::setGuidePointMode(int mode)
{
    m_guidePointMode = mode;
    if (mode != kModeSlotEnds)
        m_guidePendingEnds.clear();
    for (auto* button : {m_guideRegionButton, m_guideSlotEndsButton, m_guideHoleButton}) {
        if (!button)
            continue;
        QSignalBlocker blocker(button);
        const int owned = button == m_guideRegionButton  ? kModeRegion
                          : button == m_guideSlotEndsButton ? kModeSlotEnds
                                                            : kModeHoles;
        button->setChecked(mode == owned);
    }
    if (m_guideView)
        m_guideView->setPointPickMode(mode != kModeNone);
    updateGuideUi();
}

void MainWindow::onGuidePointPicked(int, double x, double y, double z)
{
    const std::array<double, 3> point{x, y, z};
    switch (m_guidePointMode) {
    case kModeRegion:
        m_guidePlan.contour.push_back(point);
        break;
    case kModeSlotEnds: {
        const int row = m_guideCutList ? m_guideCutList->currentRow() : -1;
        if (row < 0 || row >= static_cast<int>(m_guideCuts.size())) {
            statusBar()->showMessage(tr("Guías: elija primero la osteotomía en la lista de ranuras."));
            return;
        }
        m_guidePendingEnds.push_back(point);
        if (m_guidePendingEnds.size() < 2)
            break;
        GuideSlot slot;
        slot.path = m_guideCuts[static_cast<size_t>(row)].second;
        slot.start = m_guidePendingEnds[0];
        slot.end = m_guidePendingEnds[1];
        slot.hasExtent = true;
        m_guidePlan.slotPlan.push_back(slot);
        m_guidePendingEnds.clear();
        if (auto* item = m_guideCutList->item(row))
            item->setCheckState(Qt::Checked);
        break;
    }
    case kModeHoles: {
        GuideFixationHole hole;
        hole.center = point;
        hole.axis = m_guidePrepared.ok ? GuideDesignCore::SurfaceNormalAt(m_guidePrepared, point)
                                       : std::array<double, 3>{0.0, 0.0, 1.0};
        hole.diameterMm = m_guideHoleDiameterSpin ? m_guideHoleDiameterSpin->value() : 2.0;
        m_guidePlan.holes.push_back(hole);
        break;
    }
    default:
        return;
    }
    rebuildGuideMarkers();
    updateGuideUi();
}

void MainWindow::rebuildGuideMarkers()
{
    if (!m_guideView)
        return;
    m_guideView->clearPointMarkers();
    for (const auto& p : m_guidePlan.contour)
        m_guideView->addPointMarker(p[0], p[1], p[2], kRegionColor);
    for (const auto& slot : m_guidePlan.slotPlan)
        if (slot.hasExtent) {
            m_guideView->addPointMarker(slot.start[0], slot.start[1], slot.start[2], kSlotEndColor);
            m_guideView->addPointMarker(slot.end[0], slot.end[1], slot.end[2], kSlotEndColor);
        }
    for (const auto& p : m_guidePendingEnds)
        m_guideView->addPointMarker(p[0], p[1], p[2], kSlotEndColor);
    for (const auto& hole : m_guidePlan.holes)
        m_guideView->addPointMarker(hole.center[0], hole.center[1], hole.center[2], kHoleColor);

    if (m_guidePlan.contour.size() >= 2)
        m_guideView->setOverlayPolyline(kGuideRegionOverlayKey, GuideBaseCore::ContourPolyline(m_guidePlan.contour),
                                        kRegionColor, 2.5);
    else
        m_guideView->removeOverlay(kGuideRegionOverlayKey);
    m_guideView->render();
}

void MainWindow::clearGuideRegion()
{
    m_guidePlan.contour.clear();
    rebuildGuideMarkers();
    updateGuideUi();
}

void MainWindow::clearGuideSlotEnds()
{
    m_guidePlan.slotPlan.clear();
    m_guidePendingEnds.clear();
    rebuildGuideMarkers();
    updateGuideUi();
}

void MainWindow::clearGuideHoles()
{
    m_guidePlan.holes.clear();
    rebuildGuideMarkers();
    updateGuideUi();
}

void MainWindow::buildGuideMesh()
{
    if (!m_guideWrapMesh || m_guideWrapMesh->GetNumberOfPolys() == 0) {
        QMessageBox::warning(this, tr("Guías"), tr("Calcule primero la envolvente."));
        return;
    }
    QString error;
    if (!GuideBaseCore::ContourValid(m_guidePlan.contour, &error)) {
        QMessageBox::warning(this, tr("Guías"), error);
        return;
    }
    m_guidePlan.design.base.thicknessMm = m_guideThicknessSpin->value();
    m_guidePlan.design.base.clearanceMm = m_guideClearanceSpin->value();
    m_guidePlan.design.base.smallestDetailMm = m_guideDetailSpin->value();
    m_guidePlan.design.slot.bladeThicknessMm = m_guideBladeSpin->value();
    m_guidePlan.design.slot.smallestDetailMm = m_guideDetailSpin->value();
    m_guidePlan.design.edgeMarginMm = m_guideMarginSpin->value();

    // Only the osteotomies ticked in the list get a slot.
    std::vector<GuideSlot> chosen;
    for (int row = 0; row < (m_guideCutList ? m_guideCutList->count() : 0); ++row) {
        if (m_guideCutList->item(row)->checkState() != Qt::Checked)
            continue;
        const OsteotomyPath& path = m_guideCuts[static_cast<size_t>(row)].second;
        bool placed = false;
        for (const GuideSlot& slot : m_guidePlan.slotPlan)
            if (slot.hasExtent && slot.path.points == path.points) {
                chosen.push_back(slot);
                placed = true;
            }
        if (!placed) {
            GuideSlot slot;
            slot.path = path; // no ends marked: the whole region, still short of the rim
            chosen.push_back(slot);
        }
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    statusBar()->showMessage(tr("Guías: construyendo…"));
    if (!m_guidePrepared.ok)
        m_guidePrepared = GuideDesignCore::Prepare(m_guideWrapMesh, m_guidePlan.design);
    GuideDesignResult result;
    if (m_guidePrepared.ok)
        result = GuideDesignCore::Build(m_guidePrepared, m_guidePlan.contour, chosen, m_guidePlan.holes,
                                        m_guidePlan.design);
    QApplication::restoreOverrideCursor();
    if (!m_guidePrepared.ok) {
        QMessageBox::warning(this, tr("Guías"), m_guidePrepared.error);
        return;
    }
    if (!result.ok) {
        QMessageBox::warning(this, tr("Guías"), result.error);
        return;
    }
    m_guideMesh = result.mesh;
    if (!objectEntryExists(kGuideMeshLabel))
        addObjectEntry(tr("Guía quirúrgica"), kGuideColor, kGuideMeshLabel);
    setRepositionMeshForLabel(kGuideMeshLabel, m_guideMesh);
    syncGuideView();
    applyGuideThicknessColors();
    updateGuideUi();
    QString report = result.report;
    if (result.pieces > 1)
        report += QStringLiteral(" ") + tr("Atención: la guía quedó en %1 piezas; suba el margen al borde o acorte "
                                           "las ranuras.").arg(result.pieces);
    if (m_guideReportLabel)
        m_guideReportLabel->setText(report);
    statusBar()->showMessage(report);
}

void MainWindow::applyGuideThicknessColors()
{
    if (!m_guideView || !m_guideMesh)
        return;
    const int key = objectActorKey(kGuideMeshLabel);
    const bool on = m_guideThicknessCheck && m_guideThicknessCheck->isChecked();
    if (on) {
        const double nominal = m_guidePlan.design.base.thicknessMm;
        SplintHeightmapGenerator::ApplyThicknessColors(m_guideMesh, 0.6 * nominal, 1.4 * nominal);
        m_guideView->addMesh(key, m_guideMesh, meshLabelName(kGuideMeshLabel));
    }
    m_guideView->setMeshScalarColoring(key, on);
    m_guideView->render();
}

void MainWindow::exportGuideStl()
{
    if (!m_guideMesh || m_guideMesh->GetNumberOfPolys() == 0) {
        QMessageBox::warning(this, tr("Guías"), tr("Primero cree la guía."));
        return;
    }
    const QString path = QFileDialog::getSaveFileName(this, tr("Exportar guía"), QStringLiteral("guia.stl"),
                                                      tr("STL (*.stl)"));
    if (path.isEmpty())
        return;
    vtkSmartPointer<vtkPolyData> output = m_guideMesh;
    QString validation;
    const MeshCheck check = MeshRepairCore::Analyze(m_guideMesh);
    if (check.Valid()) {
        validation = tr("STL válido: %1").arg(check.Summary());
    } else {
        const MeshRepairResult repaired = MeshRepairCore::Repair(m_guideMesh);
        output = repaired.ok ? repaired.mesh : m_guideMesh;
        validation = repaired.ok ? repaired.report : tr("STL con avisos: %1").arg(repaired.after.Summary());
    }
    auto writer = vtkSmartPointer<vtkSTLWriter>::New();
    writer->SetFileName(path.toUtf8().constData());
    writer->SetInputData(output);
    writer->SetFileTypeToBinary();
    if (writer->Write() != 1) {
        QMessageBox::warning(this, tr("Guías"), tr("No se pudo escribir %1.").arg(path));
        return;
    }
    statusBar()->showMessage(tr("Guía exportada: %1 · %2").arg(path, validation));
}

void MainWindow::syncGuideView()
{
    if (!m_guideView)
        return;
    m_guideView->clearMeshes(true);
    // The models the guide sits on, then the envelope over them, then the guide itself.
    for (int label : m_guidePlan.sourceLabels) {
        const auto mesh = repositionMeshForLabel(label);
        if (!mesh || mesh->GetNumberOfPolys() == 0)
            continue;
        const int key = objectActorKey(label);
        m_guideView->addMesh(key, mesh, meshLabelName(label));
        m_guideView->setMeshColor(key, objectColorForLabel(label));
        m_guideView->setMeshOpacity(key, 1.0);
    }
    if (m_guideWrapMesh && m_guideWrapMesh->GetNumberOfPolys() > 0) {
        m_guideView->addMesh(kGuideWrapActorKey, m_guideWrapMesh, tr("Envolvente"));
        m_guideView->setMeshColor(kGuideWrapActorKey, kWrapColor);
        m_guideView->setMeshOpacity(kGuideWrapActorKey, 0.35);
    } else {
        m_guideView->removeMesh(kGuideWrapActorKey);
    }
    if (m_guideMesh && m_guideMesh->GetNumberOfPolys() > 0) {
        const int key = objectActorKey(kGuideMeshLabel);
        m_guideView->addMesh(key, m_guideMesh, meshLabelName(kGuideMeshLabel));
        m_guideView->setMeshColor(key, kGuideColor);
    }
    rebuildGuideMarkers();
}

void MainWindow::updateGuideUi()
{
    const bool hasWrap = m_guideWrapMesh && m_guideWrapMesh->GetNumberOfPolys() > 0;
    const bool hasRegion = GuideBaseCore::ContourValid(m_guidePlan.contour);
    const bool hasGuide = m_guideMesh && m_guideMesh->GetNumberOfPolys() > 0;
    if (m_guideRegionButton) m_guideRegionButton->setEnabled(hasWrap);
    if (m_guideSlotEndsButton) m_guideSlotEndsButton->setEnabled(hasWrap && m_guideCutList && m_guideCutList->count() > 0);
    if (m_guideHoleButton) m_guideHoleButton->setEnabled(hasWrap);
    if (m_guideBuildButton) m_guideBuildButton->setEnabled(hasWrap && hasRegion);
    if (m_guideExportButton) m_guideExportButton->setEnabled(hasGuide);
    if (m_guideThicknessCheck) m_guideThicknessCheck->setEnabled(hasGuide);

    if (!m_guideHintLabel)
        return;
    QString hint;
    if (!hasWrap)
        hint = tr("Marque los modelos reposicionados sobre los que se apoya la guía y pulse «Calcular envolvente».");
    else if (!hasRegion)
        hint = tr("Pulse «Marcar zona» y haga clic en la envolvente rodeando la superficie de apoyo (mínimo 3 puntos).");
    else if (m_guidePointMode == kModeSlotEnds)
        hint = m_guidePendingEnds.empty()
                   ? tr("Elija la osteotomía en la lista y marque el inicio de la ranura.")
                   : tr("Marque ahora el final de la ranura.");
    else if (m_guidePointMode == kModeHoles)
        hint = tr("Haga clic donde quiera cada agujero de fijación; se taladra perpendicular a la superficie.");
    else if (!hasGuide)
        hint = tr("Elija qué osteotomías llevan ranura, ajuste espesor y holgura, y pulse «Crear guía».");
    else
        hint = tr("Revise el mapa de espesor y exporte el STL. Las ranuras nunca llegan al borde: la guía sale "
                  "de una pieza.");
    m_guideHintLabel->setText(hint);
}

QJsonObject MainWindow::guidePlanJson() const
{
    if (m_guidePlan.contour.empty() && m_guidePlan.sourceLabels.empty())
        return {};
    return GuidePlanCore::ToJson(m_guidePlan);
}

void MainWindow::restoreGuidePlan(const ProjectState& state)
{
    if (state.guidesPlan.isEmpty())
        return;
    m_guidePlan = GuidePlanCore::FromJson(state.guidesPlan);
    m_guideWrapMesh = nullptr;
    m_guideMesh = nullptr;
    m_guidePrepared = GuidePreparation{};
    // The cuts the saved slots follow are offered again in the list.
    int index = 1;
    for (const GuideSlot& slot : m_guidePlan.slotPlan)
        rememberOsteotomyCut(tr("Osteotomía %1").arg(index++), slot.path);
    if (m_guideGapSpin) m_guideGapSpin->setValue(m_guidePlan.wrap.gapClosingMm);
    if (m_guideDetailSpin) m_guideDetailSpin->setValue(m_guidePlan.wrap.smallestDetailMm);
    if (m_guideThicknessSpin) m_guideThicknessSpin->setValue(m_guidePlan.design.base.thicknessMm);
    if (m_guideClearanceSpin) m_guideClearanceSpin->setValue(m_guidePlan.design.base.clearanceMm);
    if (m_guideBladeSpin) m_guideBladeSpin->setValue(m_guidePlan.design.slot.bladeThicknessMm);
    if (m_guideMarginSpin) m_guideMarginSpin->setValue(m_guidePlan.design.edgeMarginMm);
    refreshGuideSources();
    refreshGuideCutList();
    updateGuideUi();
}
