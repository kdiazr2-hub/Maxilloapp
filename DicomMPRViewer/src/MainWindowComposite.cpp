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
#include "MeshRepairCore.h"
#include "ScanEraseCore.h"
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
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <vtkPolyData.h>
#include <vtkSTLWriter.h>
#include <cmath>
#include <limits>
#include <vector>

#include <algorithm>
#include <vector>

namespace
{
constexpr int kBlockMethod = 0;
constexpr int kClassicMethod = 1;
constexpr int kPointsMethod = 2;
constexpr int kGingivalMethod = 3;
constexpr int kArchMethod = 4;
constexpr int kArchLabelKey = 6;
constexpr int kCompositeContourOverlayKey = 5;
const QColor kBlockColor(40, 120, 255);
const QColor kContourPointColor(255, 140, 0);
const std::array<unsigned char, 3> kReviewBoneColor{226, 212, 190};
const std::array<unsigned char, 3> kReviewDentalColor{226, 212, 190};
const std::array<double, 3> kUpperContourColor{1.0, 0.82, 0.0};
const std::array<double, 3> kOsteotomyContourColor{1.0, 0.45, 0.1};
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

    m_compositeLegacyControls = new QWidget(m_compositeBlockControls);
    auto* legacyLayout = new QVBoxLayout(m_compositeLegacyControls);
    legacyLayout->setContentsMargins(0, 0, 0, 0);
    auto* sizeRow = new QHBoxLayout();
    sizeRow->addWidget(new QLabel(tr("Método:"), m_compositeLegacyControls));
    m_compositeMethodCombo = new QComboBox(m_compositeLegacyControls);
    m_compositeMethodCombo->addItem(tr("Tres puntos"), kArchMethod);
    m_compositeMethodCombo->addItem(tr("Dientes y encía"), kGingivalMethod);
    m_compositeMethodCombo->addItem(tr("Contorno por puntos"), kPointsMethod);
    m_compositeMethodCombo->addItem(tr("Bloque de corte"), kBlockMethod);
    m_compositeMethodCombo->addItem(tr("Recorte automático (clásico)"), kClassicMethod);
    sizeRow->addWidget(m_compositeMethodCombo);
    m_compositeWidthSpin = makeSizeSpin(m_compositeLegacyControls, 5.0, 160.0);
    m_compositeLengthSpin = makeSizeSpin(m_compositeLegacyControls, 5.0, 160.0);
    m_compositeThicknessSpin = makeSizeSpin(m_compositeLegacyControls, 2.0, 40.0);
    m_compositeThicknessSpin->setValue(CompositeBlockCore::DefaultThicknessMm);
    sizeRow->addWidget(new QLabel(tr("Ancho:"), m_compositeLegacyControls));
    sizeRow->addWidget(m_compositeWidthSpin);
    sizeRow->addWidget(new QLabel(tr("Largo:"), m_compositeLegacyControls));
    sizeRow->addWidget(m_compositeLengthSpin);
    sizeRow->addWidget(new QLabel(tr("Grosor:"), m_compositeLegacyControls));
    sizeRow->addWidget(m_compositeThicknessSpin);
    sizeRow->addStretch(1);
    legacyLayout->addLayout(sizeRow);

    auto* legacyButtons = new QHBoxLayout();
    m_compositeGizmoButton = new QPushButton(tr("Mover bloque"), m_compositeLegacyControls);
    m_compositeAcceptGizmoButton = new QPushButton(tr("Aceptar ajuste"), m_compositeLegacyControls);
    m_compositeResetButton = new QPushButton(tr("Reiniciar bloque"), m_compositeLegacyControls);
    m_compositeClearPointsButton = new QPushButton(tr("Borrar puntos"), m_compositeLegacyControls);
    auto* ctButton = new QPushButton(tr("Ver cortes TAC"), m_compositeLegacyControls);
    ctButton->setToolTip(tr("Muestra el contorno del escaneo registrado sobre los cortes 2D del TAC."));
    auto* cancelBlock = new QPushButton(tr("Cancelar"), m_compositeLegacyControls);
    for (QPushButton* button : {m_compositeGizmoButton, m_compositeAcceptGizmoButton, m_compositeResetButton,
                                m_compositeClearPointsButton, ctButton, cancelBlock})
        legacyButtons->addWidget(button);
    legacyButtons->addStretch(1);
    legacyLayout->addLayout(legacyButtons);
    blockLayout->addWidget(m_compositeLegacyControls);

    m_compositeEraseControls = new QWidget(m_compositeBlockControls);
    auto* eraseRow = new QHBoxLayout(m_compositeEraseControls);
    eraseRow->setContentsMargins(0, 0, 0, 0);
    eraseRow->addWidget(new QLabel(tr("Borrador:"), m_compositeEraseControls));
    m_compositeEraseRadiusSpin = makeSizeSpin(m_compositeEraseControls, 1.0, 20.0);
    m_compositeEraseRadiusSpin->setValue(4.0);
    m_compositeEraseRadiusSpin->setToolTip(tr("Tamaño del círculo. También puede cambiarlo con Alt y arrastrar en vertical."));
    eraseRow->addWidget(m_compositeEraseRadiusSpin);
    auto* eraseHint = new QLabel(tr("Arrastre sobre el escaneo para quitar lo que no entra en el compuesto. Ctrl+Z deshace."),
                                 m_compositeEraseControls);
    eraseHint->setStyleSheet(QStringLiteral("color:#8e8e93;"));
    eraseRow->addWidget(eraseHint);
    eraseRow->addStretch(1);
    blockLayout->addWidget(m_compositeEraseControls);

    auto* calculateRow = new QHBoxLayout();
    m_compositeCalculateButton = new QPushButton(tr("Calcular compuesto"), m_compositeBlockControls);
    calculateRow->addWidget(m_compositeCalculateButton);
    calculateRow->addStretch(1);
    blockLayout->addLayout(calculateRow);
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
    connect(m_compositeClearPointsButton, &QPushButton::clicked, this, &MainWindow::clearCompositeContour);
    connect(ctButton, &QPushButton::clicked, this, &MainWindow::showCompositeSlicesInCt);
    connect(m_compositeEraseRadiusSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double value) {
                showCompositeEraseBrush();
                if (m_modelMatchView)
                    m_modelMatchView->setScreenBrushRadiusMm(value);
            });
    connect(m_compositeCalculateButton, &QPushButton::clicked, this, &MainWindow::calculateBlockComposite);
    connect(cancelBlock, &QPushButton::clicked, this, &MainWindow::cancelCompositeStage);
    connect(back, &QPushButton::clicked, this, &MainWindow::backToCompositeBlockStage);
    connect(accept, &QPushButton::clicked, this, &MainWindow::acceptCompositeReview);
    connect(cancelReview, &QPushButton::clicked, this, &MainWindow::cancelCompositeStage);
    if (m_modelMatchView) {
        connect(m_modelMatchView, &Mesh3DView::gizmoMeshUpdated, this, &MainWindow::onCompositeBlockGizmoUpdated);
        connect(m_modelMatchView, &Mesh3DView::editablePointAdded, this, &MainWindow::onCompositeContourPointAdded);
        connect(m_modelMatchView, &Mesh3DView::editablePointMoved, this, &MainWindow::onCompositeContourPointMoved);
        connect(m_modelMatchView, &Mesh3DView::editablePointRemoved, this, &MainWindow::onCompositeContourPointRemoved);
        connect(m_modelMatchView, &Mesh3DView::brushScreenMoved, this, &MainWindow::onCompositeScanScreenMoved);
        connect(m_modelMatchView, &Mesh3DView::surfaceBrushFinished, this, &MainWindow::onCompositeScanBrushFinished);
        connect(m_modelMatchView, &Mesh3DView::brushRadiusDragged, this, &MainWindow::onCompositeEraseRadiusDragged);
    }
    return m_compositeBlockPanel;
}

bool MainWindow::compositeBlockMethodActive() const
{
    return !m_compositeMethodCombo || m_compositeMethodCombo->currentData().toInt() == kBlockMethod;
}

bool MainWindow::compositePointsMethodActive() const
{
    return m_compositeMethodCombo && m_compositeMethodCombo->currentData().toInt() == kPointsMethod;
}

bool MainWindow::compositeGingivalMethodActive() const
{
    return m_compositeMethodCombo && m_compositeMethodCombo->currentData().toInt() == kGingivalMethod;
}

bool MainWindow::compositeArchMethodActive() const
{
    return !m_compositeMethodCombo || m_compositeMethodCombo->currentData().toInt() == kArchMethod;
}

CompositeCutBlock& MainWindow::compositeBlockForStep(int step)
{
    return step == 0 ? m_upperCompositeBlock : m_lowerCompositeBlock;
}

vtkSmartPointer<vtkPolyData> MainWindow::compositeDentalForStep(int step, QString* error) const
{
    return step == 0 ? resolvedUpperArchWorldMesh(error) : resolvedLowerArchWorldMesh(error);
}

// The registered scan the composite will merge. Gingival mode shows and merges the boxed
// cast; the imported STL and the mesh the eraser edits stay as they are.
vtkSmartPointer<vtkPolyData> boxedCompositeScan(vtkPolyData* scan, int step)
{
    if (!scan)
        return nullptr;
    const CompositeJaw jaw = step == 0 ? CompositeJaw::Upper : CompositeJaw::Lower;
    if (auto boxed = CompositeBlockCore::BoxScan(scan, jaw))
        return boxed;
    return scan;
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
    const CompositeJaw jaw = step == 0 ? CompositeJaw::Upper : CompositeJaw::Lower;
    // A block saved before the jaw rule may face the wrong way (mandible toward the cusps): start it again.
    if (!block.valid || !CompositeBlockCore::BlockFacesJaw(block, jaw)) {
        const double thickness = m_compositeThicknessSpin ? m_compositeThicknessSpin->value()
                                                          : CompositeBlockCore::DefaultThicknessMm;
        block = CompositeBlockCore::InitialBlock(dental, bone, thickness, &error, jaw);
        if (!block.valid) {
            m_autoCreateBothComposites = false;
            QMessageBox::warning(this, tr("Modelo compuesto"), error);
            return;
        }
    }
    m_compositeStageStep = step;
    m_compositeReviewMesh = nullptr;
    m_scanEraseUndo.clear();
    m_scanEraseStrokeOpen = false;
    m_scanErasePressing = false;
    m_scanEraseHasHit = false;
    m_compositeStage = CompositeStage::Block;
    showCompositeStage();
    statusBar()->showMessage(compositeArchMethodActive()
        ? tr("Marque tres puntos en la encía vestibular: posterior derecho, posterior izquierdo y anterior. "
             "La línea roja rodea la arcada, por delante de los dientes. Siguiente quita los dientes del TAC y los reemplaza por el escaneo.")
        : compositeGingivalMethodActive()
        ? tr("Borre del escaneo lo que sobre y pulse Calcular compuesto. Une los dientes y un poco de encía.")
        : compositePointsMethodActive()
        ? tr("Modelo compuesto: marque puntos sobre el escaneo rodeando toda la zona que reemplaza al TAC "
             "(clic: poner · arrastrar: mover · clic derecho: borrar) y pulse Calcular compuesto.")
        : tr("Modelo compuesto: ajuste el bloque azul para que cubra los dientes y pulse Calcular compuesto."));
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
    if (m_modelMatchView && (m_compositeStage != CompositeStage::Block || !compositeGingivalMethodActive()))
        m_modelMatchView->setSurfaceBrushMode(false);
    if (m_compositeStage == CompositeStage::Review)
        updateCompositeReviewDisplay();
    else if (active)
        updateCompositeBlockDisplay();
    updateCompositeContourDisplay();
    updateModelWorkflowUi();
}

void MainWindow::updateCompositeStagePanel()
{
    if (!m_compositeBlockPanel || m_compositeStage == CompositeStage::None)
        return;
    const QString jaw = m_compositeStageStep == 0 ? tr("maxilar") : tr("mandibular");
    const QString zone = compositeArchMethodActive() ? tr("Tres puntos")
                         : compositeGingivalMethodActive() ? tr("Dientes y encía")
                         : compositePointsMethodActive() ? tr("Contorno")
                                                         : tr("Bloque");
    const QString steps = m_compositeStage == CompositeStage::Review
        ? tr("Registrar ✓ · Ajuste fino ✓ · %1 ✓ · <b>Revisar</b>").arg(zone)
        : tr("Registrar ✓ · Ajuste fino ✓ · <b>%1</b> · Revisar").arg(zone);
    m_compositeStageLabel->setText(tr("<b>Compuesto %1</b> — %2").arg(jaw, steps));

    const bool review = m_compositeStage == CompositeStage::Review;
    const bool computing = m_compositeStage == CompositeStage::Computing;
    m_compositeBlockControls->setVisible(!review);
    m_compositeReviewControls->setVisible(review);
    m_compositeBlockControls->setEnabled(!computing);
    const bool showErase = compositeGingivalMethodActive();
    const bool archMethod = compositeArchMethodActive();
    if (m_compositeStageLabel)
        m_compositeStageLabel->setVisible(!(showErase && !review));
    if (m_compositeLegacyControls)
        m_compositeLegacyControls->setVisible(!showErase && !archMethod);
    if (m_compositeEraseControls)
        m_compositeEraseControls->setVisible(showErase && !review);

    const bool blockMethod = compositeBlockMethodActive();
    const CompositeCutBlock& block = compositeBlockForStep(m_compositeStageStep);
    const bool pointsMethod = compositePointsMethodActive();
    const bool gingivalMethod = compositeGingivalMethodActive();
    const bool arch = compositeArchMethodActive();
    for (QWidget* w : {static_cast<QWidget*>(m_compositeWidthSpin), static_cast<QWidget*>(m_compositeLengthSpin)})
        w->setEnabled(blockMethod);
    // Points method: the bone is replaced up to the line of the points, so thickness does not apply.
    m_compositeThicknessSpin->setEnabled(blockMethod);
    m_compositeResetButton->setEnabled(blockMethod || pointsMethod || gingivalMethod || arch);
    m_compositeResetButton->setText(pointsMethod || arch ? tr("Reiniciar puntos")
                                     : gingivalMethod ? tr("Reiniciar")
                                                      : tr("Reiniciar bloque"));
    m_compositeGizmoButton->setVisible(blockMethod);
    m_compositeAcceptGizmoButton->setVisible(blockMethod);
    m_compositeGizmoButton->setEnabled(blockMethod && !m_compositeBlockGizmoActive);
    m_compositeAcceptGizmoButton->setEnabled(blockMethod && m_compositeBlockGizmoActive);
    const auto& contour = m_compositeContours[static_cast<size_t>(std::clamp(m_compositeStageStep, 0, 1))];
    const bool contourReady = arch ? static_cast<int>(contour.size()) == 3
                                   : gingivalMethod || !pointsMethod
                                         || static_cast<int>(contour.size()) >= CompositeBlockCore::MinContourPoints;
    m_compositeClearPointsButton->setVisible(pointsMethod || arch);
    m_compositeClearPointsButton->setEnabled(!contour.empty());
    m_compositeCalculateButton->setEnabled(!computing && contourReady);
    m_compositeCalculateButton->setText(computing ? tr("Calculando…")
                                        : arch && !contourReady ? tr("Siguiente (%1/3 puntos)").arg(contour.size())
                                        : arch ? tr("Siguiente")
                                        : contourReady ? tr("Calcular compuesto")
                                                       : tr("Calcular compuesto (%1/%2 puntos)")
                                                             .arg(contour.size()).arg(CompositeBlockCore::MinContourPoints));
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
        // Contour points go on the scan, not on the bone behind it.
        m_modelMatchView->setMeshPickable(boneLabel, compositeArchMethodActive() ||
                                                          (!compositePointsMethodActive() && !compositeGingivalMethodActive()));
    }
    if (const auto source = step == 0 ? m_upperArchMesh : m_lowerArchMesh) {
        const auto arch = compositeGingivalMethodActive() ? boxedCompositeScan(source, step) : source;
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
    if (compositeArchMethodActive())
        m_modelMatchView->setTitle(step == 0 ? tr("REEMPLAZAR DIENTES — MAXILAR") : tr("REEMPLAZAR DIENTES — MANDÍBULA"));
    else if (compositeGingivalMethodActive())
        m_modelMatchView->setTitle(step == 0 ? tr("DIENTES Y ENCÍA — MAXILAR") : tr("DIENTES Y ENCÍA — MANDÍBULA"));
    else if (compositePointsMethodActive())
        m_modelMatchView->setTitle(step == 0 ? tr("CONTORNO POR PUNTOS — MAXILAR") : tr("CONTORNO POR PUNTOS — MANDÍBULA"));
    else
        m_modelMatchView->setTitle(step == 0 ? tr("BLOQUE DE CORTE — MAXILAR") : tr("BLOQUE DE CORTE — MANDÍBULA"));
    updateCompositeContourDisplay();
    if (m_modelMatchView) {
        const bool gingival = compositeGingivalMethodActive();
        m_modelMatchView->setSurfaceBrushMode(gingival);
        m_modelMatchView->setScreenBrush(gingival);
        if (gingival && m_compositeEraseRadiusSpin)
            m_modelMatchView->setScreenBrushRadiusMm(m_compositeEraseRadiusSpin->value());
    }
    showCompositeEraseBrush();
    m_modelMatchView->render();
}

// ── Contour points around the scan ────────────────────────────────────────────

void MainWindow::updateCompositeContourDisplay()
{
    if (!m_modelMatchView)
        return;
    const bool points = compositePointsMethodActive();
    const bool arch = compositeArchMethodActive();
    const bool active = m_compositeStage == CompositeStage::Block && (points || arch);
    const int step = std::clamp(m_compositeStageStep, 0, 1);
    const CompositeBlockCore::CompositeContour none;
    const auto& contour = active ? m_compositeContours[static_cast<size_t>(step)] : none;
    const QColor pointColor = arch ? QColor(220, 30, 40) : kContourPointColor;
    m_modelMatchView->setEditablePoints(0, contour, pointColor, 2.2);
    m_modelMatchView->setPointEditMode(active, active ? 0 : -1);
    const CompositeCutBlock& block = compositeBlockForStep(step);
    if (arch && block.valid && contour.size() == 3) {
        const auto ribbon = CompositeBlockCore::ArchRibbon(meshForAnatomicLabel(step == 0 ? 5 : 6), block, contour);
        if (ribbon && ribbon->GetNumberOfPolys() > 0) {
            m_modelMatchView->addMesh(kCompositeContourActorKey, ribbon, tr("Zona de reemplazo"));
            m_modelMatchView->setMeshColor(kCompositeContourActorKey, QColor(220, 30, 40));
            m_modelMatchView->setMeshOpacity(kCompositeContourActorKey, 0.45);
            m_modelMatchView->setMeshPickable(kCompositeContourActorKey, false);
        } else {
            m_modelMatchView->removeMesh(kCompositeContourActorKey);
        }
        std::vector<std::pair<std::array<double, 3>, QString>> labels;
        for (const auto& p : contour)
            labels.push_back({p, QString()});
        if (labels.size() == 3) {
            int anterior = 0;
            for (int i = 1; i < 3; ++i)
                if (labels[static_cast<size_t>(i)].first[1] < labels[static_cast<size_t>(anterior)].first[1])
                    anterior = i;
            const int right = labels[(anterior + 1) % 3].first[0] < labels[(anterior + 2) % 3].first[0]
                                  ? (anterior + 1) % 3
                                  : (anterior + 2) % 3;
            const int left = 3 - anterior - right;
            const QString side = step == 0 ? QStringLiteral("U") : QStringLiteral("L");
            labels[static_cast<size_t>(right)].second = side + QStringLiteral("R");
            labels[static_cast<size_t>(left)].second = side + QStringLiteral("L");
            labels[static_cast<size_t>(anterior)].second = side + QStringLiteral("F");
        }
        m_modelMatchView->setOverlayLabels(kArchLabelKey, labels, QColor(255, 220, 40));
        m_modelMatchView->removeOverlay(kCompositeContourOverlayKey);
        m_modelMatchView->render();
        return;
    }
    m_modelMatchView->setOverlayLabels(kArchLabelKey, {}, QColor());
    if (points && block.valid && contour.size() >= 2) {
        m_modelMatchView->setOverlayPolyline(kCompositeContourOverlayKey, CompositeBlockCore::ContourPolyline(block, contour),
                                             kContourPointColor, 3.0);
    } else {
        m_modelMatchView->removeOverlay(kCompositeContourOverlayKey);
    }
    if (points && block.valid && static_cast<int>(contour.size()) >= CompositeBlockCore::MinContourPoints) {
        m_modelMatchView->addMesh(kCompositeContourActorKey, CompositeBlockCore::ContourWallMesh(block, contour),
                                  tr("Contorno del compuesto"));
        m_modelMatchView->setMeshColor(kCompositeContourActorKey, kContourPointColor);
        m_modelMatchView->setMeshOpacity(kCompositeContourActorKey, 0.25);
        m_modelMatchView->setMeshPickable(kCompositeContourActorKey, false);
    } else {
        m_modelMatchView->removeMesh(kCompositeContourActorKey);
    }
    m_modelMatchView->render();
}

void MainWindow::refreshCompositeScanMesh()
{
    if (!m_modelMatchView || m_compositeStage != CompositeStage::Block)
        return;
    const int step = m_compositeStageStep;
    const int archLabel = step == 0 ? kUpperArchLabel : kLowerArchLabel;
    const auto source = step == 0 ? m_upperArchMesh : m_lowerArchMesh;
    if (!source)
        return;
    const auto arch = compositeGingivalMethodActive() ? boxedCompositeScan(source, step) : source;
    const int key = objectActorKey(archLabel);
    m_modelMatchView->addMesh(key, arch, tr("Escaneo registrado"));
    m_modelMatchView->setMeshColor(key, objectColorForLabel(archLabel));
    showCompositeEraseBrush();
}

void MainWindow::showCompositeEraseBrush()
{
    if (!m_modelMatchView)
        return;
    m_modelMatchView->removeMesh(kScanEraseBrushActorKey);
    if (m_compositeStage != CompositeStage::Block || !compositeGingivalMethodActive()) {
        m_modelMatchView->setSurfaceBrushMode(false);
        m_modelMatchView->setScreenBrush(false);
    }
}

void MainWindow::onCompositeScanScreenMoved(int x, int y, Qt::KeyboardModifiers modifiers)
{
    if (m_compositeStage != CompositeStage::Block || !compositeGingivalMethodActive() || modifiers.testFlag(Qt::AltModifier))
        return;
    // The circle is already at the cursor. Deletion runs after this event so the
    // marker is not waiting on a projection of the scan.
    m_scanErasePressing = true;
    m_scanErasePending.append(QPoint(x, y));
    if (m_scanEraseQueued)
        return;
    m_scanEraseQueued = true;
    QTimer::singleShot(0, this, [this] { flushCompositeScanErase(); });
}

void MainWindow::flushCompositeScanErase()
{
    m_scanEraseQueued = false;
    if (m_compositeStage != CompositeStage::Block || !compositeGingivalMethodActive() || !m_modelMatchView) {
        m_scanErasePending.clear();
        return;
    }
    const QVector<QPoint> samples = m_scanErasePending;
    m_scanErasePending.clear();
    if (samples.isEmpty())
        return;
    const int step = std::clamp(m_compositeStageStep, 0, 1);
    auto& arch = step == 0 ? m_upperArchMesh : m_lowerArchMesh;
    if (!arch || arch->GetNumberOfPolys() == 0)
        return;
    const double radiusMm = m_compositeEraseRadiusSpin ? m_compositeEraseRadiusSpin->value() : 4.0;
    const double radiusPx = std::max(1.0, radiusMm * m_modelMatchView->widgetPixelsPerMm());
    bool changed = false;
    for (const QPoint& sample : samples) {
        if (!arch || arch->GetNumberOfPoints() == 0)
            break;
        const vtkIdType n = arch->GetNumberOfPoints();
        const vtkMTimeType meshTime = arch->GetMTime();
        const vtkMTimeType cameraTime = m_modelMatchView->cameraMTime();
        if (m_scanEraseCacheStep != step || m_scanEraseCacheMeshTime != meshTime
            || m_scanEraseCacheCameraTime != cameraTime
            || m_scanEraseDisplayX.size() != static_cast<int>(n)) {
            m_scanEraseDisplayX.resize(static_cast<int>(n));
            m_scanEraseDisplayY.resize(static_cast<int>(n));
            for (vtkIdType i = 0; i < n; ++i) {
                double world[3] = {};
                arch->GetPoint(i, world);
                double wx = 0.0;
                double wy = 0.0;
                if (!m_modelMatchView->projectToWidget(world, wx, wy)) {
                    m_scanEraseDisplayX[static_cast<int>(i)] = std::numeric_limits<float>::quiet_NaN();
                    m_scanEraseDisplayY[static_cast<int>(i)] = std::numeric_limits<float>::quiet_NaN();
                    continue;
                }
                m_scanEraseDisplayX[static_cast<int>(i)] = static_cast<float>(wx);
                m_scanEraseDisplayY[static_cast<int>(i)] = static_cast<float>(wy);
            }
            m_scanEraseCacheStep = step;
            m_scanEraseCacheMeshTime = meshTime;
            m_scanEraseCacheCameraTime = cameraTime;
        }
        const auto erased = ScanEraseCore::EraseScreen(arch, m_scanEraseDisplayX.constData(),
                                                       m_scanEraseDisplayY.constData(), sample.x(), sample.y(),
                                                       radiusPx);
        if (!erased)
            continue;
        if (!m_scanEraseStrokeOpen) {
            ScanEraseSnapshot snapshot;
            snapshot.step = step;
            snapshot.mesh = vtkSmartPointer<vtkPolyData>::New();
            snapshot.mesh->DeepCopy(arch);
            m_scanEraseUndo.append(snapshot);
            while (m_scanEraseUndo.size() > 12)
                m_scanEraseUndo.removeFirst();
            m_scanEraseStrokeOpen = true;
        }
        arch = erased;
        changed = true;
        m_scanEraseCacheStep = -1;
    }
    if (changed) {
        refreshCompositeScanMesh();
        statusBar()->showMessage(tr("Borrador %1 mm. Ctrl+Z deshace el trazo.").arg(radiusMm, 0, 'f', 1));
    }
    if (!m_scanErasePending.isEmpty()) {
        m_scanEraseQueued = true;
        QTimer::singleShot(0, this, [this] { flushCompositeScanErase(); });
    }
}

void MainWindow::onCompositeScanBrushFinished()
{
    m_scanEraseStrokeOpen = false;
    m_scanErasePressing = false;
    m_scanEraseHasHit = false;
    showCompositeEraseBrush();
}

void MainWindow::onCompositeEraseRadiusDragged(double deltaY)
{
    if (!m_compositeEraseRadiusSpin || !compositeGingivalMethodActive())
        return;
    m_scanErasePressing = true;
    m_compositeEraseRadiusSpin->setValue(m_compositeEraseRadiusSpin->value() - deltaY * 0.04);
    if (m_modelMatchView)
        m_modelMatchView->setScreenBrushRadiusMm(m_compositeEraseRadiusSpin->value());
}

void MainWindow::onCompositeContourPointAdded(int group, double x, double y, double z)
{
    if (group != 0 || m_compositeStage != CompositeStage::Block
        || (!compositePointsMethodActive() && !compositeArchMethodActive()))
        return;
    auto& contour = m_compositeContours[static_cast<size_t>(std::clamp(m_compositeStageStep, 0, 1))];
    if (compositeArchMethodActive() && static_cast<int>(contour.size()) >= 3) {
        statusBar()->showMessage(tr("Ya están los tres puntos. Arrastre uno para mover la línea, o pulse Siguiente."));
        return;
    }
    contour.push_back({x, y, z});
    updateCompositeContourDisplay();
    updateCompositeStagePanel();
    if (compositeArchMethodActive()) {
        statusBar()->showMessage(static_cast<int>(contour.size()) < 3
            ? tr("Punto %1 de 3. Derecho, izquierdo y anterior.").arg(contour.size())
            : tr("Línea roja lista. Siguiente quita los dientes del TAC de ese lado y los reemplaza por el escaneo."));
    } else {
        statusBar()->showMessage(static_cast<int>(contour.size()) < CompositeBlockCore::MinContourPoints
            ? tr("Contorno: %1 punto(s). Siga rodeando el escaneo.").arg(contour.size())
            : tr("Contorno: %1 puntos. Siga rodeando o pulse Calcular compuesto.").arg(contour.size()));
    }
}

void MainWindow::onCompositeContourPointMoved(int group, int index, double x, double y, double z)
{
    if (group != 0 || m_compositeStage != CompositeStage::Block
        || (!compositePointsMethodActive() && !compositeArchMethodActive()))
        return;
    auto& contour = m_compositeContours[static_cast<size_t>(std::clamp(m_compositeStageStep, 0, 1))];
    if (index < 0 || index >= static_cast<int>(contour.size()))
        return;
    contour[static_cast<size_t>(index)] = {x, y, z};
    updateCompositeContourDisplay();
}

void MainWindow::onCompositeContourPointRemoved(int group, int index)
{
    if (group != 0 || m_compositeStage != CompositeStage::Block
        || (!compositePointsMethodActive() && !compositeArchMethodActive()))
        return;
    auto& contour = m_compositeContours[static_cast<size_t>(std::clamp(m_compositeStageStep, 0, 1))];
    if (index < 0 || index >= static_cast<int>(contour.size()))
        return;
    contour.erase(contour.begin() + index);
    updateCompositeContourDisplay();
    updateCompositeStagePanel();
}

void MainWindow::clearCompositeContour()
{
    if (m_compositeStage != CompositeStage::Block)
        return;
    m_compositeContours[static_cast<size_t>(std::clamp(m_compositeStageStep, 0, 1))].clear();
    updateCompositeContourDisplay();
    updateCompositeStagePanel();
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
    if (compositePointsMethodActive() || compositeArchMethodActive())
        m_compositeContours[static_cast<size_t>(std::clamp(m_compositeStageStep, 0, 1))].clear();
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
    const bool gingivalMethod = compositeGingivalMethodActive();
    const bool archMethod = compositeArchMethodActive();
    if (gingivalMethod) {
        if (auto boxed = CompositeBlockCore::BoxScan(dental, step == 0 ? CompositeJaw::Upper : CompositeJaw::Lower))
            dental = boxed;
    }
    const CompositeCutBlock block = compositeBlockForStep(step);
    const bool blockMethod = compositeBlockMethodActive();
    const bool pointsMethod = compositePointsMethodActive();
    const CompositeBlockCore::CompositeContour contour = m_compositeContours[static_cast<size_t>(std::clamp(step, 0, 1))];
    if (archMethod && static_cast<int>(contour.size()) != 3) {
        statusBar()->showMessage(tr("Marque los tres puntos antes de seguir."));
        return;
    }
    if (pointsMethod && !CompositeBlockCore::ContourValid(block, contour, &error)) {
        statusBar()->showMessage(error);
        return;
    }

    m_compositeStage = CompositeStage::Computing;
    m_compositeInProgress = true;
    if (m_progressBar) {
        m_progressBar->setRange(0, 0);
        m_progressBar->setVisible(true);
    }
    updateCompositeStagePanel();
    updateCompositeContourDisplay();
    statusBar()->showMessage(tr("Calculando modelo compuesto en segundo plano…"));

    auto* watcher = new QFutureWatcher<CompositeBlockResult>(this);
    connect(watcher, &QFutureWatcher<CompositeBlockResult>::finished, this, [this, watcher, step] {
        const CompositeBlockResult result = watcher->result();
        watcher->deleteLater();
        onBlockCompositeCalculated(step, result);
    });
    watcher->setFuture(QtConcurrent::run([bone, dental, block, blockMethod, pointsMethod, gingivalMethod, archMethod, contour, step]() {
        if (archMethod)
            return CompositeBlockCore::CreateArchComposite(bone, dental, block, contour);
        if (gingivalMethod)
            return CompositeBlockCore::CreateGingivalComposite(bone, dental, block);
        if (pointsMethod)
            return CompositeBlockCore::CreateContourComposite(bone, dental, block, contour);
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
    const auto osteotomyGuides = osteotomyContourMeshes();
    if (upper == m_contourUpperArchMesh && lower == m_contourLowerArchMesh &&
        upperTime == m_contourUpperArchTime && lowerTime == m_contourLowerArchTime &&
        osteotomyGuides == m_contourOsteotomyMeshes)
        return;
    m_contourUpperArchMesh = upper;
    m_contourLowerArchMesh = lower;
    m_contourUpperArchTime = upperTime;
    m_contourLowerArchTime = lowerTime;
    m_contourOsteotomyMeshes = osteotomyGuides;

    std::vector<MPRSurfaceContour> contours;
    if (upper)
        contours.push_back({upper, kUpperContourColor});
    if (lower)
        contours.push_back({lower, kLowerContourColor});
    // Cutting path of the osteotomy being planned (ProPlan "Show Contour").
    for (const auto& guide : osteotomyGuides)
        contours.push_back({guide, kOsteotomyContourColor});
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
        // Unjoined parts are open surfaces: report the check, never remesh silently.
        const MeshCheck check = MeshRepairCore::Analyze(mesh);
        const QString validation = check.Valid() ? tr("STL válido: %1").arg(check.Summary())
                                                 : tr("STL no cerrado (use «Unir por vóxeles» para imprimir): %1").arg(check.Summary());
        statusBar()->showMessage(tr("Modelo compuesto exportado: %1 %2 · %3").arg(path, note, validation));
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

// ── Without intraoral scan ────────────────────────────────────────────────────

bool MainWindow::createBoneOnlyComposites()
{
    const auto copyMesh = [](vtkPolyData* source) -> vtkSmartPointer<vtkPolyData> {
        if (!source || source->GetNumberOfPoints() <= 0)
            return nullptr;
        auto copy = vtkSmartPointer<vtkPolyData>::New();
        copy->DeepCopy(source);
        return copy;
    };
    bool available = m_upperCompositeMesh || m_lowerCompositeMesh;
    if (!m_upperCompositeMesh) {
        m_upperCompositeMesh = copyMesh(meshForAnatomicLabel(5));
        if (m_upperCompositeMesh) {
            addObjectEntry(tr("Compuesto maxilar (sin STL)"), objectColorForLabel(kUpperCompositeLabel), kUpperCompositeLabel);
            available = true;
        }
    }
    if (!m_lowerCompositeMesh) {
        m_lowerCompositeMesh = copyMesh(meshForAnatomicLabel(6));
        if (m_lowerCompositeMesh) {
            addObjectEntry(tr("Compuesto mandibular (sin STL)"), objectColorForLabel(kLowerCompositeLabel), kLowerCompositeLabel);
            available = true;
        }
    }
    m_appState.setUpperCompositeReady(m_upperCompositeMesh != nullptr);
    m_appState.setLowerCompositeReady(m_lowerCompositeMesh != nullptr);
    return available;
}

// ── Project ───────────────────────────────────────────────────────────────────

QJsonObject MainWindow::compositeBlocksJson() const
{
    QJsonObject blocks;
    if (m_upperCompositeBlock.valid)
        blocks[QStringLiteral("upper")] = CompositeBlockCore::BlockToJson(m_upperCompositeBlock);
    if (m_lowerCompositeBlock.valid)
        blocks[QStringLiteral("lower")] = CompositeBlockCore::BlockToJson(m_lowerCompositeBlock);
    if (!m_compositeContours[0].empty())
        blocks[QStringLiteral("upperContour")] = CompositeBlockCore::ContourToJson(m_compositeContours[0]);
    if (!m_compositeContours[1].empty())
        blocks[QStringLiteral("lowerContour")] = CompositeBlockCore::ContourToJson(m_compositeContours[1]);
    return blocks;
}

void MainWindow::restoreCompositeBlocks(const ProjectState& state)
{
    m_upperCompositeBlock = CompositeBlockCore::BlockFromJson(state.compositeBlocks.value(QStringLiteral("upper")).toObject());
    m_lowerCompositeBlock = CompositeBlockCore::BlockFromJson(state.compositeBlocks.value(QStringLiteral("lower")).toObject());
    m_compositeContours[0] = CompositeBlockCore::ContourFromJson(state.compositeBlocks.value(QStringLiteral("upperContour")).toArray());
    m_compositeContours[1] = CompositeBlockCore::ContourFromJson(state.compositeBlocks.value(QStringLiteral("lowerContour")).toArray());
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
