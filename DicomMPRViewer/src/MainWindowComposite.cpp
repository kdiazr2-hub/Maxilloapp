// ─────────────────────────────────────────────────────────────────────────────
// MainWindow — MODELOS composite with cutting block (Phase 3).
//
// Registrar → Ajuste fino → Bloque → Revisar. The registered scan contour is
// drawn on the CT slices for fine adjustment; an oriented blue block chooses
// where the intraoral scan replaces the CT dentition (CompositeBlockCore); the
// result is only stored after an explicit review, also when both jaws are
// chained. The classic automatic trim (CompositeModelCore) stays selectable.
// ─────────────────────────────────────────────────────────────────────────────

#include "MainWindow.h"

#include "CompositeModelCore.h"
#include "LoggerCore.h"
#include "MPRView.h"
#include "Mesh3DView.h"
#include "ObjectLabels.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStatusBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <vtkPolyData.h>
#include <vtkSTLWriter.h>

#include <vector>

namespace
{
constexpr int kBlockMethod = 0;
constexpr int kClassicMethod = 1;
const QColor kBlockColor(40, 120, 255);
const std::array<unsigned char, 3> kReviewBoneColor{226, 212, 190};
const std::array<unsigned char, 3> kReviewDentalColor{250, 250, 244};
const std::array<double, 3> kUpperContourColor{1.0, 0.82, 0.0};
const std::array<double, 3> kLowerContourColor{0.0, 0.86, 1.0};

QDoubleSpinBox* makeSizeSpin(QWidget* parent, double min, double max)
{
    auto* spin = new QDoubleSpinBox(parent);
    spin->setRange(min, max);
    spin->setDecimals(1);
    spin->setSingleStep(1.0);
    spin->setSuffix(QStringLiteral(" mm"));
    spin->setKeyboardTracking(false);
    return spin;
}

bool writeBinaryStl(vtkPolyData* mesh, const QString& path)
{
    auto writer = vtkSmartPointer<vtkSTLWriter>::New();
    writer->SetFileName(path.toLocal8Bit().constData());
    writer->SetInputData(mesh);
    writer->SetFileTypeToBinary();
    return writer->Write() != 0;
}
} // namespace

QWidget* MainWindow::buildCompositeBlockPanel(QWidget* parent)
{
    m_compositeBlockPanel = new QWidget(parent);
    m_compositeBlockPanel->setVisible(false);
    auto* layout = new QVBoxLayout(m_compositeBlockPanel);
    layout->setContentsMargins(10, 4, 10, 6);
    layout->setSpacing(4);

    m_compositeStageLabel = new QLabel(m_compositeBlockPanel);
    m_compositeStageLabel->setTextFormat(Qt::RichText);
    m_compositeStageLabel->setWordWrap(true);
    layout->addWidget(m_compositeStageLabel);

    // ── Block stage ───────────────────────────────────────────────────────
    m_compositeBlockControls = new QWidget(m_compositeBlockPanel);
    auto* blockLayout = new QVBoxLayout(m_compositeBlockControls);
    blockLayout->setContentsMargins(0, 0, 0, 0);
    auto* sizeRow = new QHBoxLayout();
    sizeRow->addWidget(new QLabel(tr("Método:"), m_compositeBlockControls));
    m_compositeMethodCombo = new QComboBox(m_compositeBlockControls);
    m_compositeMethodCombo->addItem(tr("Bloque de corte"), kBlockMethod);
    m_compositeMethodCombo->addItem(tr("Recorte automático (clásico)"), kClassicMethod);
    sizeRow->addWidget(m_compositeMethodCombo);
    m_compositeWidthSpin = makeSizeSpin(m_compositeBlockControls, 5.0, 160.0);
    m_compositeLengthSpin = makeSizeSpin(m_compositeBlockControls, 5.0, 160.0);
    m_compositeThicknessSpin = makeSizeSpin(m_compositeBlockControls, 2.0, 40.0);
    m_compositeThicknessSpin->setValue(CompositeBlockCore::DefaultThicknessMm);
    sizeRow->addWidget(new QLabel(tr("Ancho:"), m_compositeBlockControls));
    sizeRow->addWidget(m_compositeWidthSpin);
    sizeRow->addWidget(new QLabel(tr("Largo:"), m_compositeBlockControls));
    sizeRow->addWidget(m_compositeLengthSpin);
    sizeRow->addWidget(new QLabel(tr("Grosor:"), m_compositeBlockControls));
    sizeRow->addWidget(m_compositeThicknessSpin);
    sizeRow->addStretch(1);
    blockLayout->addLayout(sizeRow);

    auto* blockButtons = new QHBoxLayout();
    m_compositeGizmoButton = new QPushButton(tr("Mover bloque"), m_compositeBlockControls);
    m_compositeAcceptGizmoButton = new QPushButton(tr("Aceptar ajuste"), m_compositeBlockControls);
    m_compositeResetButton = new QPushButton(tr("Reiniciar bloque"), m_compositeBlockControls);
    auto* ctButton = new QPushButton(tr("Ver cortes TAC"), m_compositeBlockControls);
    ctButton->setToolTip(tr("Muestra el contorno del escaneo registrado sobre los cortes 2D del TAC."));
    m_compositeCalculateButton = new QPushButton(tr("Calcular compuesto"), m_compositeBlockControls);
    auto* cancelBlock = new QPushButton(tr("Cancelar"), m_compositeBlockControls);
    for (QPushButton* button : {m_compositeGizmoButton, m_compositeAcceptGizmoButton, m_compositeResetButton,
                                ctButton, m_compositeCalculateButton, cancelBlock})
        blockButtons->addWidget(button);
    blockButtons->addStretch(1);
    blockLayout->addLayout(blockButtons);
    layout->addWidget(m_compositeBlockControls);

    // ── Review stage ──────────────────────────────────────────────────────
    m_compositeReviewControls = new QWidget(m_compositeBlockPanel);
    auto* reviewButtons = new QHBoxLayout(m_compositeReviewControls);
    reviewButtons->setContentsMargins(0, 0, 0, 0);
    auto* back = new QPushButton(tr("← Atrás (reajustar bloque)"), m_compositeReviewControls);
    auto* accept = new QPushButton(tr("Aceptar compuesto"), m_compositeReviewControls);
    auto* cancelReview = new QPushButton(tr("Cancelar"), m_compositeReviewControls);
    reviewButtons->addWidget(back);
    reviewButtons->addWidget(accept);
    reviewButtons->addWidget(cancelReview);
    reviewButtons->addStretch(1);
    layout->addWidget(m_compositeReviewControls);

    for (QPushButton* primary : {m_compositeCalculateButton, accept})
        primary->setStyleSheet(QStringLiteral(
            "QPushButton { background:#0a84ff; color:#ffffff; font-weight:700; border:none; border-radius:10px; padding:5px 12px; }"
            "QPushButton:disabled { background:#242426; color:#636366; }"));

    connect(m_compositeMethodCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        if (m_compositeBlockGizmoActive)
            acceptCompositeBlockGizmo();
        updateCompositeStagePanel();
        updateCompositeBlockDisplay();
    });
    for (QDoubleSpinBox* spin : {m_compositeWidthSpin, m_compositeLengthSpin, m_compositeThicknessSpin})
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &MainWindow::onCompositeBlockSizeChanged);
    connect(m_compositeGizmoButton, &QPushButton::clicked, this, &MainWindow::startCompositeBlockGizmo);
    connect(m_compositeAcceptGizmoButton, &QPushButton::clicked, this, &MainWindow::acceptCompositeBlockGizmo);
    connect(m_compositeResetButton, &QPushButton::clicked, this, &MainWindow::resetCompositeBlock);
    connect(ctButton, &QPushButton::clicked, this, &MainWindow::showCompositeSlicesInCt);
    connect(m_compositeCalculateButton, &QPushButton::clicked, this, &MainWindow::calculateBlockComposite);
    connect(cancelBlock, &QPushButton::clicked, this, &MainWindow::cancelCompositeStage);
    connect(back, &QPushButton::clicked, this, &MainWindow::backToCompositeBlockStage);
    connect(accept, &QPushButton::clicked, this, &MainWindow::acceptCompositeReview);
    connect(cancelReview, &QPushButton::clicked, this, &MainWindow::cancelCompositeStage);
    if (m_modelMatchView)
        connect(m_modelMatchView, &Mesh3DView::gizmoMeshUpdated, this, &MainWindow::onCompositeBlockGizmoUpdated);
    return m_compositeBlockPanel;
}

bool MainWindow::compositeBlockMethodActive() const
{
    return !m_compositeMethodCombo || m_compositeMethodCombo->currentData().toInt() == kBlockMethod;
}

CompositeCutBlock& MainWindow::compositeBlockForStep(int step)
{
    return step == 0 ? m_upperCompositeBlock : m_lowerCompositeBlock;
}

vtkSmartPointer<vtkPolyData> MainWindow::compositeDentalForStep(int step, QString* error) const
{
    return step == 0 ? resolvedUpperArchWorldMesh(error) : resolvedLowerArchWorldMesh(error);
}

// ── Stages ────────────────────────────────────────────────────────────────────

void MainWindow::startCompositeBlockStage(int step)
{
    const auto bone = meshForAnatomicLabel(step == 0 ? 5 : 6);
    QString error;
    const auto dental = compositeDentalForStep(step, &error);
    if (!bone || !dental) {
        m_autoCreateBothComposites = false;
        QMessageBox::warning(this, tr("Modelo compuesto"),
                             error.isEmpty() ? tr("Faltan el hueso o el escaneo registrado para el compuesto.") : error);
        return;
    }
    CompositeCutBlock& block = compositeBlockForStep(step);
    if (!block.valid) {
        const double thickness = m_compositeThicknessSpin ? m_compositeThicknessSpin->value()
                                                          : CompositeBlockCore::DefaultThicknessMm;
        block = CompositeBlockCore::InitialBlock(dental, bone, thickness, &error);
        if (!block.valid) {
            m_autoCreateBothComposites = false;
            QMessageBox::warning(this, tr("Modelo compuesto"), error);
            return;
        }
    }
    m_compositeStageStep = step;
    m_compositeReviewMesh = nullptr;
    m_compositeStage = CompositeStage::Block;
    showCompositeStage();
    statusBar()->showMessage(tr("Modelo compuesto: ajuste el bloque azul para que cubra los dientes y pulse Calcular compuesto."));
}

void MainWindow::showCompositeStage()
{
    const bool active = m_compositeStage != CompositeStage::None;
    if (m_compositeBlockPanel)
        m_compositeBlockPanel->setVisible(active);
    if (m_modelStepStack)
        m_modelStepStack->setVisible(!active && !(m_upperCompositeMesh && m_lowerCompositeMesh));
    if (m_compositeButton)
        m_compositeButton->setVisible(!active);
    updateCompositeStagePanel();
    if (m_compositeStage == CompositeStage::Review)
        updateCompositeReviewDisplay();
    else if (active)
        updateCompositeBlockDisplay();
    updateModelWorkflowUi();
}

void MainWindow::updateCompositeStagePanel()
{
    if (!m_compositeBlockPanel || m_compositeStage == CompositeStage::None)
        return;
    const QString jaw = m_compositeStageStep == 0 ? tr("maxilar") : tr("mandibular");
    const QString steps = m_compositeStage == CompositeStage::Review
        ? tr("Registrar ✓ · Ajuste fino ✓ · Bloque ✓ · <b>Revisar</b>")
        : tr("Registrar ✓ · Ajuste fino ✓ · <b>Bloque</b> · Revisar");
    m_compositeStageLabel->setText(tr("<b>Compuesto %1</b> — %2").arg(jaw, steps));

    const bool review = m_compositeStage == CompositeStage::Review;
    const bool computing = m_compositeStage == CompositeStage::Computing;
    m_compositeBlockControls->setVisible(!review);
    m_compositeReviewControls->setVisible(review);
    m_compositeBlockControls->setEnabled(!computing);

    const bool blockMethod = compositeBlockMethodActive();
    const CompositeCutBlock& block = compositeBlockForStep(m_compositeStageStep);
    for (QWidget* w : {static_cast<QWidget*>(m_compositeWidthSpin), static_cast<QWidget*>(m_compositeLengthSpin),
                       static_cast<QWidget*>(m_compositeThicknessSpin), static_cast<QWidget*>(m_compositeResetButton)})
        w->setEnabled(blockMethod);
    m_compositeGizmoButton->setEnabled(blockMethod && !m_compositeBlockGizmoActive);
    m_compositeAcceptGizmoButton->setEnabled(blockMethod && m_compositeBlockGizmoActive);
    m_compositeCalculateButton->setText(computing ? tr("Calculando…") : tr("Calcular compuesto"));
    if (block.valid) {
        const QSignalBlocker w(m_compositeWidthSpin), l(m_compositeLengthSpin), t(m_compositeThicknessSpin);
        m_compositeWidthSpin->setValue(block.sizeMm[0]);
        m_compositeLengthSpin->setValue(block.sizeMm[1]);
        m_compositeThicknessSpin->setValue(block.sizeMm[2]);
    }
}

void MainWindow::updateCompositeBlockDisplay()
{
    if (!m_modelMatchView || m_compositeStage == CompositeStage::None || m_compositeStage == CompositeStage::Review)
        return;
    const int step = m_compositeStageStep;
    const int boneLabel = step == 0 ? 5 : 6;
    const int archLabel = step == 0 ? kUpperArchLabel : kLowerArchLabel;
    m_modelMatchView->clearMeshes(true);
    if (const auto bone = meshForAnatomicLabel(boneLabel)) {
        m_modelMatchView->addMesh(boneLabel, bone, tr("Hueso"));
        m_modelMatchView->setMeshColor(boneLabel, objectColorForLabel(boneLabel));
        // Translucent so the block and the teeth it replaces stay visible.
        m_modelMatchView->setMeshOpacity(boneLabel, 0.45);
    }
    if (const auto arch = step == 0 ? m_upperArchMesh : m_lowerArchMesh) {
        const int key = objectActorKey(archLabel);
        m_modelMatchView->addMesh(key, arch, tr("Escaneo registrado"));
        m_modelMatchView->setMeshColor(key, objectColorForLabel(archLabel));
    }
    const CompositeCutBlock& block = compositeBlockForStep(step);
    if (compositeBlockMethodActive() && block.valid) {
        m_modelMatchView->addMesh(kCompositeBlockActorKey, CompositeBlockCore::BlockMesh(block), tr("Bloque de corte"));
        m_modelMatchView->setMeshColor(kCompositeBlockActorKey, kBlockColor);
        m_modelMatchView->setMeshOpacity(kCompositeBlockActorKey, 0.35);
        m_modelMatchView->setMeshPickable(kCompositeBlockActorKey, false);
    }
    m_modelMatchView->setTitle(step == 0 ? tr("BLOQUE DE CORTE — MAXILAR") : tr("BLOQUE DE CORTE — MANDÍBULA"));
    m_modelMatchView->render();
}

void MainWindow::updateCompositeReviewDisplay()
{
    if (!m_modelMatchView || !m_compositeReviewMesh)
        return;
    m_modelMatchView->clearMeshes(true);
    const bool parts = CompositeBlockCore::HasParts(m_compositeReviewMesh);
    const auto display = parts
        ? CompositeBlockCore::PartColoredCopy(m_compositeReviewMesh, kReviewBoneColor, kReviewDentalColor)
        : m_compositeReviewMesh;
    m_modelMatchView->addMesh(kCompositeReviewActorKey, display, tr("Compuesto en revisión"));
    const int compositeLabel = m_compositeStageStep == 0 ? kUpperCompositeLabel : kLowerCompositeLabel;
    m_modelMatchView->setMeshColor(kCompositeReviewActorKey, objectColorForLabel(compositeLabel));
    m_modelMatchView->setMeshScalarColoring(kCompositeReviewActorKey, parts);
    m_modelMatchView->setTitle(m_compositeStageStep == 0
        ? tr("REVISIÓN — COMPUESTO MAXILAR: acepte o vuelva atrás")
        : tr("REVISIÓN — COMPUESTO MANDIBULAR: acepte o vuelva atrás"));
    m_modelMatchView->render();
}

// ── Block editing ─────────────────────────────────────────────────────────────

void MainWindow::onCompositeBlockSizeChanged()
{
    if (m_compositeStage != CompositeStage::Block)
        return;
    CompositeCutBlock& block = compositeBlockForStep(m_compositeStageStep);
    if (!block.valid)
        return;
    block = CompositeBlockCore::WithSize(block, m_compositeWidthSpin->value(), m_compositeLengthSpin->value(),
                                         m_compositeThicknessSpin->value());
    updateCompositeBlockDisplay();
}

void MainWindow::startCompositeBlockGizmo()
{
    if (m_compositeStage != CompositeStage::Block || !compositeBlockMethodActive() || !m_modelMatchView)
        return;
    m_compositeBlockAtGizmoStart = compositeBlockForStep(m_compositeStageStep);
    m_modelMatchView->startGizmo(kCompositeBlockActorKey);
    m_compositeBlockGizmoActive = m_modelMatchView->hasGizmo();
    updateCompositeStagePanel();
    if (m_compositeBlockGizmoActive)
        statusBar()->showMessage(tr("Mueva, gire o redimensione el bloque con el gizmo y pulse Aceptar ajuste."));
}

void MainWindow::acceptCompositeBlockGizmo()
{
    if (m_modelMatchView && m_modelMatchView->hasGizmo())
        m_modelMatchView->stopGizmo(); // emits gizmoMeshUpdated → onCompositeBlockGizmoUpdated
    m_compositeBlockGizmoActive = false;
    updateCompositeStagePanel();
}

void MainWindow::onCompositeBlockGizmoUpdated(int meshLabel, vtkSmartPointer<vtkPolyData>)
{
    if (meshLabel != kCompositeBlockActorKey || !m_compositeBlockGizmoActive || !m_modelMatchView)
        return;
    compositeBlockForStep(m_compositeStageStep) =
        CompositeBlockCore::TransformBlock(m_compositeBlockAtGizmoStart, m_modelMatchView->lastGizmoTransformMatrix());
    m_compositeBlockGizmoActive = false;
    updateCompositeStagePanel();
    updateCompositeBlockDisplay();
}

void MainWindow::resetCompositeBlock()
{
    if (m_compositeStage != CompositeStage::Block)
        return;
    if (m_compositeBlockGizmoActive) {
        m_compositeBlockGizmoActive = false;
        if (m_modelMatchView && m_modelMatchView->hasGizmo())
            m_modelMatchView->stopGizmo();
    }
    compositeBlockForStep(m_compositeStageStep).valid = false;
    startCompositeBlockStage(m_compositeStageStep);
}

void MainWindow::showCompositeSlicesInCt()
{
    refreshRegisteredArchContours();
    for (auto* tab : findChildren<QToolButton*>(QStringLiteral("MT"))) {
        if (tab && tab->text() == tr("SEGMENTACION")) {
            tab->click();
            break;
        }
    }
    statusBar()->showMessage(tr("Contorno del escaneo registrado sobre los cortes del TAC (amarillo: superior, celeste: inferior). "
                                "Vuelva a MODELOS para continuar con el bloque."));
}

// ── Computation and review ────────────────────────────────────────────────────

void MainWindow::calculateBlockComposite()
{
    if (m_compositeStage != CompositeStage::Block || m_compositeInProgress)
        return;
    if (m_compositeBlockGizmoActive)
        acceptCompositeBlockGizmo();

    const int step = m_compositeStageStep;
    const auto boneSource = meshForAnatomicLabel(step == 0 ? 5 : 6);
    QString error;
    const auto dentalSource = compositeDentalForStep(step, &error);
    if (!boneSource || !dentalSource) {
        QMessageBox::warning(this, tr("Modelo compuesto"),
                             error.isEmpty() ? tr("Faltan el hueso o el escaneo registrado para el compuesto.") : error);
        return;
    }
    auto bone = vtkSmartPointer<vtkPolyData>::New();
    bone->DeepCopy(boneSource);
    auto dental = vtkSmartPointer<vtkPolyData>::New();
    dental->DeepCopy(dentalSource);
    const CompositeCutBlock block = compositeBlockForStep(step);
    const bool blockMethod = compositeBlockMethodActive();

    m_compositeStage = CompositeStage::Computing;
    m_compositeInProgress = true;
    if (m_progressBar) {
        m_progressBar->setRange(0, 0);
        m_progressBar->setVisible(true);
    }
    updateCompositeStagePanel();
    statusBar()->showMessage(tr("Calculando modelo compuesto en segundo plano…"));

    auto* watcher = new QFutureWatcher<CompositeBlockResult>(this);
    connect(watcher, &QFutureWatcher<CompositeBlockResult>::finished, this, [this, watcher, step] {
        const CompositeBlockResult result = watcher->result();
        watcher->deleteLater();
        onBlockCompositeCalculated(step, result);
    });
    watcher->setFuture(QtConcurrent::run([bone, dental, block, blockMethod, step]() {
        if (blockMethod)
            return CompositeBlockCore::CreateBlockComposite(bone, dental, block);
        CompositeBlockResult classic;
        QString report;
        QString error;
        classic.composite = CompositeModelCore::CreateNonDestructiveComposite(
            bone, nullptr, dental, nullptr,
            step == 0 ? QStringLiteral("Maxilar") : QStringLiteral("Mandibula"), &report, &error);
        classic.ok = classic.composite && classic.composite->GetNumberOfPolys() > 0;
        classic.report = report;
        if (!classic.ok)
            classic.error = error.isEmpty() ? QStringLiteral("No se pudo calcular el compuesto clásico.") : error;
        return classic;
    }));
}

void MainWindow::onBlockCompositeCalculated(int step, const CompositeBlockResult& result)
{
    m_compositeInProgress = false;
    if (m_progressBar) {
        m_progressBar->setRange(0, 100);
        m_progressBar->setVisible(false);
    }
    if (m_compositeStage != CompositeStage::Computing || step != m_compositeStageStep)
        return; // cancelled while computing
    if (!result.ok) {
        m_compositeStage = CompositeStage::Block;
        showCompositeStage();
        QMessageBox::warning(this, tr("Modelo compuesto"), result.error);
        return;
    }
    m_compositeReviewMesh = result.composite;
    m_compositeReviewReport = result.report;
    m_compositeStage = CompositeStage::Review;
    showCompositeStage();
    LoggerCore::instance().logCustom(QStringLiteral("COMPOSITE"), result.report);
    statusBar()->showMessage(tr("Revise el compuesto (%1) y pulse Aceptar compuesto o Atrás.").arg(result.report));
}

void MainWindow::backToCompositeBlockStage()
{
    if (m_compositeStage != CompositeStage::Review)
        return;
    m_compositeReviewMesh = nullptr;
    m_compositeStage = CompositeStage::Block;
    showCompositeStage();
    statusBar()->showMessage(tr("Reajuste el bloque y vuelva a calcular el compuesto."));
}

void MainWindow::acceptCompositeReview()
{
    if (m_compositeStage != CompositeStage::Review || !m_compositeReviewMesh)
        return;
    const auto mesh = m_compositeReviewMesh;
    const int step = m_compositeStageStep;
    m_compositeReviewMesh = nullptr;
    m_compositeStage = CompositeStage::None;
    if (m_modelMatchView)
        m_modelMatchView->setTitle(tr("MATCH PREVIEW"));
    showCompositeStage();
    if (m_modelStepStack)
        m_modelStepStack->setVisible(true);
    // Stores the composite, advances the workflow and continues a chained lower composite.
    onDentalCompositeFinished(step, mesh);
}

void MainWindow::cancelCompositeStage()
{
    if (m_compositeStage == CompositeStage::None)
        return;
    if (m_compositeBlockGizmoActive) {
        m_compositeBlockGizmoActive = false;
        if (m_modelMatchView && m_modelMatchView->hasGizmo())
            m_modelMatchView->stopGizmo();
    }
    m_autoCreateBothComposites = false;
    m_compositeReviewMesh = nullptr;
    m_compositeStage = CompositeStage::None;
    if (m_modelMatchView)
        m_modelMatchView->setTitle(tr("MATCH PREVIEW"));
    showCompositeStage();
    syncModelViews();
    statusBar()->showMessage(tr("Modelo compuesto cancelado."));
}

// ── Fine adjustment on the CT slices ──────────────────────────────────────────

void MainWindow::refreshRegisteredArchContours()
{
    const auto upper = m_upperRegistrationCalculated ? m_upperArchMesh : vtkSmartPointer<vtkPolyData>();
    const auto lower = m_lowerRegistrationCalculated ? m_lowerArchMesh : vtkSmartPointer<vtkPolyData>();
    const vtkMTimeType upperTime = upper ? upper->GetMTime() : 0;
    const vtkMTimeType lowerTime = lower ? lower->GetMTime() : 0;
    if (upper == m_contourUpperArchMesh && lower == m_contourLowerArchMesh &&
        upperTime == m_contourUpperArchTime && lowerTime == m_contourLowerArchTime)
        return;
    m_contourUpperArchMesh = upper;
    m_contourLowerArchMesh = lower;
    m_contourUpperArchTime = upperTime;
    m_contourLowerArchTime = lowerTime;

    std::vector<MPRSurfaceContour> contours;
    if (upper)
        contours.push_back({upper, kUpperContourColor});
    if (lower)
        contours.push_back({lower, kLowerContourColor});
    for (MPRView* view : {m_axialView, m_coronalView, m_sagittalView})
        if (view) view->setSurfaceContours(contours);
}

// ── Export ────────────────────────────────────────────────────────────────────

void MainWindow::writeCompositeStl(const QVector<vtkSmartPointer<vtkPolyData>>& parts,
                                   vtkSmartPointer<vtkPolyData> mergedMesh, const QString& path)
{
    QMessageBox box(this);
    box.setWindowTitle(tr("Exportar modelo compuesto"));
    box.setText(tr("¿Unir las partes en una sola malla cerrada para imprimir?"));
    box.setInformativeText(tr("La unión por vóxeles (0,3 mm) sella uniones pequeñas y rellena el interior; tarda unos segundos."));
    auto* unionButton = box.addButton(tr("Unir por vóxeles"), QMessageBox::AcceptRole);
    auto* plainButton = box.addButton(tr("Guardar sin unir"), QMessageBox::ActionRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();

    const auto finish = [this, path](vtkPolyData* mesh, const QString& note) {
        if (!writeBinaryStl(mesh, path)) {
            QMessageBox::critical(this, tr("Exportar STL"), tr("No se pudo escribir el STL."));
            return;
        }
        LoggerCore::instance().logExport(path, QStringLiteral("composite_stl"));
        statusBar()->showMessage(tr("Modelo compuesto exportado: %1 %2").arg(path, note));
    };

    if (box.clickedButton() == plainButton) {
        finish(mergedMesh, QString());
        return;
    }
    if (box.clickedButton() != unionButton)
        return;

    std::vector<vtkSmartPointer<vtkPolyData>> meshes(parts.begin(), parts.end());
    if (m_progressBar) {
        m_progressBar->setRange(0, 0);
        m_progressBar->setVisible(true);
    }
    statusBar()->showMessage(tr("Uniendo el modelo compuesto por vóxeles…"));
    auto* watcher = new QFutureWatcher<VoxelUnionResult>(this);
    connect(watcher, &QFutureWatcher<VoxelUnionResult>::finished, this, [this, watcher, finish] {
        const VoxelUnionResult result = watcher->result();
        watcher->deleteLater();
        if (m_progressBar) {
            m_progressBar->setRange(0, 100);
            m_progressBar->setVisible(false);
        }
        if (!result.ok) {
            QMessageBox::warning(this, tr("Exportar STL"), tr("No se pudo unir por vóxeles:\n%1").arg(result.error));
            return;
        }
        finish(result.mesh, QStringLiteral("(%1)").arg(result.report));
    });
    watcher->setFuture(QtConcurrent::run([meshes]() {
        std::vector<vtkPolyData*> raw;
        for (const auto& mesh : meshes)
            raw.push_back(mesh);
        return CompositeBlockCore::VoxelUnion(raw, 0.3, 3);
    }));
}

// ── Project ───────────────────────────────────────────────────────────────────

QJsonObject MainWindow::compositeBlocksJson() const
{
    QJsonObject blocks;
    if (m_upperCompositeBlock.valid)
        blocks[QStringLiteral("upper")] = CompositeBlockCore::BlockToJson(m_upperCompositeBlock);
    if (m_lowerCompositeBlock.valid)
        blocks[QStringLiteral("lower")] = CompositeBlockCore::BlockToJson(m_lowerCompositeBlock);
    return blocks;
}

void MainWindow::restoreCompositeBlocks(const ProjectState& state)
{
    m_upperCompositeBlock = CompositeBlockCore::BlockFromJson(state.compositeBlocks.value(QStringLiteral("upper")).toObject());
    m_lowerCompositeBlock = CompositeBlockCore::BlockFromJson(state.compositeBlocks.value(QStringLiteral("lower")).toObject());
    m_compositeBlockGizmoActive = false;
    m_compositeReviewMesh = nullptr;
    if (m_compositeStage != CompositeStage::None) {
        m_compositeStage = CompositeStage::None;
        showCompositeStage();
    }
    m_contourUpperArchMesh = nullptr;
    m_contourLowerArchMesh = nullptr;
    refreshRegisteredArchContours();
}
