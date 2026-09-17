// ─────────────────────────────────────────────────────────────────────────────
// MainWindow — GUIAS: surgical guides, the step after FERULA.
//
// The panel walks the steps of 3-matic's Design tab: choose the guide (Le Fort
// or chin — each wraps its own models in their planned position), mark the
// support region, place the saw slots on the planned osteotomies, add Boolean
// figures (cylinders, boxes, spheres or imported STL shapes, added or
// subtracted), drill the fixation holes, and build. The geometry is all in the
// cores (`WrapCore`, `GuideBaseCore`, `GuideDesignCore`); this file only
// collects what the user decides into a `GuidePlan` and shows it.
// ─────────────────────────────────────────────────────────────────────────────

#include "MainWindow.h"

#include "Mesh3DView.h"
#include "MeshRepairCore.h"
#include "ObjectLabels.h"
#include "SplintHeightmapGenerator.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
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

#include <vtkIdList.h>
#include <vtkMatrix4x4.h>
#include <vtkPointData.h>
#include <vtkPointLocator.h>
#include <vtkPolyData.h>
#include <vtkUnsignedCharArray.h>
#include <vtkSTLReader.h>
#include <vtkSTLWriter.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>

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
constexpr int kModeFigure = 4;

constexpr int kGuideFigureActorBase = -560; // one actor per figure, counting down

const QColor kGuideColor(214, 226, 240);
const QColor kWrapColor(120, 196, 214);  // the envelope: its own layer, teal so it is not mistaken for bone
const QColor kPaintColor(10, 132, 255);  // the brushed region on the envelope
const QColor kRegionColor(10, 132, 255);
const QColor kSlotEndColor(255, 159, 10);
const QColor kHoleColor(52, 199, 89);
const QColor kSubtractColor(255, 69, 58);
const QColor kAddColor(48, 209, 88);

int figureActorKey(size_t index) { return kGuideFigureActorBase - static_cast<int>(index); }

QString figureName(const GuideFigure& figure)
{
    const QString op = figure.operation == GuideFigureOperation::Add ? QObject::tr("Unir") : QObject::tr("Restar");
    switch (figure.shape) {
    case GuideFigureShape::Cylinder:
        return QObject::tr("%1 · Cilindro Ø%2 × %3 mm").arg(op).arg(figure.diameterMm, 0, 'f', 1).arg(figure.lengthMm, 0, 'f', 1);
    case GuideFigureShape::Box:
        return QObject::tr("%1 · Caja %2 × %3 × %4 mm")
            .arg(op)
            .arg(figure.widthMm, 0, 'f', 1)
            .arg(figure.heightMm, 0, 'f', 1)
            .arg(figure.depthMm, 0, 'f', 1);
    case GuideFigureShape::Sphere:
        return QObject::tr("%1 · Esfera Ø%2 mm").arg(op).arg(figure.diameterMm, 0, 'f', 1);
    case GuideFigureShape::Mesh:
        return QObject::tr("%1 · %2").arg(op, QFileInfo(figure.sourcePath).fileName());
    }
    return op;
}

// An STL read and centred on its own middle, so its frame can be placed like any primitive's.
vtkSmartPointer<vtkPolyData> loadFigureMesh(const QString& path)
{
    if (path.isEmpty() || !QFileInfo::exists(path))
        return nullptr;
    auto reader = vtkSmartPointer<vtkSTLReader>::New();
    reader->SetFileName(path.toUtf8().constData());
    reader->Update();
    vtkPolyData* mesh = reader->GetOutput();
    if (!mesh || mesh->GetNumberOfPolys() == 0)
        return nullptr;
    double b[6] = {};
    mesh->GetBounds(b);
    auto center = vtkSmartPointer<vtkTransform>::New();
    center->Translate(-0.5 * (b[0] + b[1]), -0.5 * (b[2] + b[3]), -0.5 * (b[4] + b[5]));
    auto filter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
    filter->SetInputData(mesh);
    filter->SetTransform(center);
    filter->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(filter->GetOutput());
    return out;
}
} // namespace

QWidget* MainWindow::buildGuideControlPanel(QWidget* parent)
{
    auto* scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setFixedWidth(330);
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
                                        "#GuideControlPanel QComboBox { background:#2c2c2e; color:#f5f5f7;"
                                        "  border:1px solid #3a3a3c; border-radius:8px; padding:4px 8px; }"
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

    // ── 1. Guide type and envelope ────────────────────────────────────────
    section(tr("1. TIPO DE GUÍA"));
    m_guideTypeCombo = new QComboBox(panel);
    m_guideTypeCombo->addItem(tr("Guía Le Fort I"), static_cast<int>(GuideType::LeFort));
    m_guideTypeCombo->addItem(tr("Guía de mentón"), static_cast<int>(GuideType::Chin));
    connect(m_guideTypeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { setGuideType(static_cast<GuideType>(m_guideTypeCombo->currentData().toInt())); });
    layout->addWidget(m_guideTypeCombo);
    m_guideSourcesLabel = new QLabel(panel);
    m_guideSourcesLabel->setWordWrap(true);
    layout->addWidget(m_guideSourcesLabel);
    auto* wrapForm = new QFormLayout();
    m_guideGapSpin = spin(1.5, 0.0, 10.0, 0.1);
    m_guideDetailSpin = spin(0.3, 0.1, 1.0, 0.05);
    wrapForm->addRow(tr("Cierre de huecos:"), m_guideGapSpin);
    wrapForm->addRow(tr("Detalle:"), m_guideDetailSpin);
    layout->addLayout(wrapForm);
    m_guideWrapButton = new QPushButton(tr("Calcular envolvente"), panel);
    connect(m_guideWrapButton, &QPushButton::clicked, this, &MainWindow::computeGuideWrap);
    layout->addWidget(m_guideWrapButton);

    // ── Layers ────────────────────────────────────────────────────────────
    section(tr("CAPAS"));
    const auto layer = [&](const QString& text) {
        auto* check = new QCheckBox(text, panel);
        check->setChecked(true);
        connect(check, &QCheckBox::toggled, this, [this](bool) { applyGuideLayers(); });
        layout->addWidget(check);
        return check;
    };
    m_guideShowModelsCheck = layer(tr("Modelos (hueso)"));
    m_guideShowWrapCheck = layer(tr("Envolvente"));
    m_guideShowGuideCheck = layer(tr("Guía"));
    m_guideShowFiguresCheck = layer(tr("Figuras"));
    auto* layerForm = new QFormLayout();
    m_guideWrapOpacitySpin = new QDoubleSpinBox(panel);
    m_guideWrapOpacitySpin->setRange(0.1, 1.0);
    m_guideWrapOpacitySpin->setSingleStep(0.1);
    m_guideWrapOpacitySpin->setValue(0.6);
    connect(m_guideWrapOpacitySpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double) { applyGuideLayers(); });
    layerForm->addRow(tr("Opacidad envolvente:"), m_guideWrapOpacitySpin);
    layout->addLayout(layerForm);

    // ── 2. Support region ─────────────────────────────────────────────────
    section(tr("2. ZONA DE APOYO (PINCEL)"));
    m_guideRegionButton = new QPushButton(tr("Pintar zona"), panel);
    m_guideRegionButton->setCheckable(true);
    connect(m_guideRegionButton, &QPushButton::toggled, this,
            [this](bool on) { setGuidePointMode(on ? kModeRegion : kModeNone); });
    layout->addWidget(m_guideRegionButton);
    auto* regionForm = new QFormLayout();
    m_guideBrushSpin = spin(4.0, 0.5, 15.0, 0.5);
    regionForm->addRow(tr("Tamaño del pincel:"), m_guideBrushSpin);
    layout->addLayout(regionForm);
    auto* brushHelp = new QLabel(tr("Arrastre para pintar · Ctrl + arrastre para borrar · Alt + arrastre vertical "
                                    "cambia el tamaño"), panel);
    brushHelp->setWordWrap(true);
    layout->addWidget(brushHelp);
    auto* clearRegion = new QPushButton(tr("Borrar zona"), panel);
    connect(clearRegion, &QPushButton::clicked, this, &MainWindow::clearGuideRegion);
    layout->addWidget(clearRegion);

    // ── 3. Saw slots ──────────────────────────────────────────────────────
    section(tr("3. RANURAS DE SIERRA"));
    m_guideCutList = new QListWidget(panel);
    m_guideCutList->setMaximumHeight(80);
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

    // ── 4. Figures (Boolean tools) ────────────────────────────────────────
    section(tr("4. FIGURAS (OPERACIONES BOOLEANAS)"));
    auto* figureForm = new QFormLayout();
    m_guideFigureShapeCombo = new QComboBox(panel);
    m_guideFigureShapeCombo->addItem(tr("Cilindro"), static_cast<int>(GuideFigureShape::Cylinder));
    m_guideFigureShapeCombo->addItem(tr("Caja"), static_cast<int>(GuideFigureShape::Box));
    m_guideFigureShapeCombo->addItem(tr("Esfera"), static_cast<int>(GuideFigureShape::Sphere));
    connect(m_guideFigureShapeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { updateGuideFigureInputs(); });
    figureForm->addRow(tr("Figura:"), m_guideFigureShapeCombo);
    m_guideFigureOperationCombo = new QComboBox(panel);
    m_guideFigureOperationCombo->addItem(tr("Restar (ranura, orificio)"), static_cast<int>(GuideFigureOperation::Subtract));
    m_guideFigureOperationCombo->addItem(tr("Unir (añadir material)"), static_cast<int>(GuideFigureOperation::Add));
    figureForm->addRow(tr("Operación:"), m_guideFigureOperationCombo);
    m_guideFigureDiameterSpin = spin(3.0, 0.2, 40.0, 0.1);
    m_guideFigureLengthSpin = spin(12.0, 0.2, 80.0, 0.5);
    m_guideFigureWidthSpin = spin(12.0, 0.2, 80.0, 0.5);
    m_guideFigureHeightSpin = spin(1.0, 0.1, 80.0, 0.1);
    m_guideFigureDepthSpin = spin(12.0, 0.2, 80.0, 0.5);
    figureForm->addRow(tr("Diámetro:"), m_guideFigureDiameterSpin);
    figureForm->addRow(tr("Longitud:"), m_guideFigureLengthSpin);
    figureForm->addRow(tr("Ancho:"), m_guideFigureWidthSpin);
    figureForm->addRow(tr("Alto:"), m_guideFigureHeightSpin);
    figureForm->addRow(tr("Profundidad:"), m_guideFigureDepthSpin);
    layout->addLayout(figureForm);
    m_guidePlaceFigureButton = new QPushButton(tr("Colocar figura"), panel);
    m_guidePlaceFigureButton->setCheckable(true);
    connect(m_guidePlaceFigureButton, &QPushButton::toggled, this,
            [this](bool on) { setGuidePointMode(on ? kModeFigure : kModeNone); });
    layout->addWidget(m_guidePlaceFigureButton);
    auto* importFigure = new QPushButton(tr("Importar figura STL…"), panel);
    connect(importFigure, &QPushButton::clicked, this, &MainWindow::importGuideFigure);
    layout->addWidget(importFigure);
    m_guideFigureList = new QListWidget(panel);
    m_guideFigureList->setMaximumHeight(90);
    connect(m_guideFigureList, &QListWidget::currentRowChanged, this, [this](int) { updateGuideUi(); });
    layout->addWidget(m_guideFigureList);
    m_guideMoveFigureButton = new QPushButton(tr("Mover figura (gizmo)"), panel);
    m_guideMoveFigureButton->setCheckable(true);
    connect(m_guideMoveFigureButton, &QPushButton::toggled, this, &MainWindow::setGuideFigureGizmo);
    layout->addWidget(m_guideMoveFigureButton);
    auto* removeFigure = new QPushButton(tr("Borrar figura"), panel);
    connect(removeFigure, &QPushButton::clicked, this, &MainWindow::removeGuideFigure);
    layout->addWidget(removeFigure);

    // ── 5. Fixation holes ─────────────────────────────────────────────────
    section(tr("5. AGUJEROS DE FIJACIÓN"));
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

    // ── 6. Build ──────────────────────────────────────────────────────────
    section(tr("6. CREAR LA GUÍA"));
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
    updateGuideFigureInputs();
    return scroll;
}

void MainWindow::setGuidesWorkspace(bool enabled)
{
    if (m_viewModeStack && enabled)
        m_viewModeStack->setCurrentIndex(7);
    if (!enabled) {
        setGuidePointMode(kModeNone);
        if (m_guideMoveFigureButton && m_guideMoveFigureButton->isChecked())
            m_guideMoveFigureButton->setChecked(false);
        return;
    }
    refreshGuideSources();
    refreshGuideCutList();
    refreshGuideFigureList();
    syncGuideView();
    updateGuideUi();
}

void MainWindow::setGuideType(GuideType type)
{
    if (m_guidePlan.type == type && !m_guidePlan.sourceLabels.empty())
        return;
    m_guidePlan.type = type;
    m_guidePlan.sourceLabels = GuidePlanCore::SourceLabelsFor(type);
    // A different guide sits on different models: its envelope and region start again.
    m_guideWrapMesh = nullptr;
    m_guidePrepared = GuidePreparation{};
    m_guideMesh = nullptr;
    m_guidePlan.contour.clear();
    m_guidePlan.paint.clear();
    m_guidePlan.slotPlan.clear();
    m_guidePendingEnds.clear();
    refreshGuideSources();
    refreshGuideCutList();
    syncGuideView();
    updateGuideUi();
}

void MainWindow::refreshGuideSources()
{
    if (m_guidePlan.sourceLabels.empty())
        m_guidePlan.sourceLabels = GuidePlanCore::SourceLabelsFor(m_guidePlan.type);
    if (!m_guideSourcesLabel)
        return;
    QStringList parts;
    for (int label : GuidePlanCore::SourceLabelsFor(m_guidePlan.type)) {
        const auto mesh = repositionMeshForLabel(label);
        const bool present = mesh && mesh->GetNumberOfPolys() > 0;
        parts << (present ? QStringLiteral("✓ %1").arg(meshLabelName(label))
                          : tr("✗ %1 (falta)").arg(meshLabelName(label)));
    }
    m_guideSourcesLabel->setText(tr("Envolvente sobre los modelos en su posición planificada:\n%1")
                                     .arg(parts.join(QStringLiteral("\n"))));
}

void MainWindow::refreshGuideCutList()
{
    if (!m_guideCutList)
        return;
    // The osteotomy being planned counts too, so a guide can be designed before the cut is executed.
    if (m_ostWizard.path.valid && m_ostWizard.type != 1)
        rememberOsteotomyCut(tr("Trayectoria actual"), m_ostWizard.path,
                             m_ostWizard.type == 2 ? GuideType::Chin : GuideType::LeFort);

    QSet<QString> checked;
    for (int row = 0; row < m_guideCutList->count(); ++row)
        if (m_guideCutList->item(row)->checkState() == Qt::Checked)
            checked.insert(m_guideCutList->item(row)->text());

    QSignalBlocker blocker(m_guideCutList);
    m_guideCutList->clear();
    for (size_t i = 0; i < m_guideCuts.size(); ++i) {
        if (m_guideCuts[i].type != m_guidePlan.type)
            continue; // a Le Fort guide only offers Le Fort cuts, a chin guide the genioplasty
        auto* item = new QListWidgetItem(m_guideCuts[i].name, m_guideCutList);
        item->setData(Qt::UserRole, static_cast<int>(i));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(checked.contains(m_guideCuts[i].name) ? Qt::Checked : Qt::Unchecked);
    }
}

void MainWindow::rememberOsteotomyCut(const QString& name, const OsteotomyPath& path, GuideType type)
{
    if (!path.valid)
        return;
    for (auto& cut : m_guideCuts) {
        if (cut.name == name) {
            cut.path = path;
            cut.type = type;
            return;
        }
    }
    m_guideCuts.push_back({name, path, type});
}

void MainWindow::computeGuideWrap()
{
    std::vector<vtkPolyData*> meshes;
    QStringList missing;
    for (int label : GuidePlanCore::SourceLabelsFor(m_guidePlan.type)) {
        const auto mesh = repositionMeshForLabel(label);
        if (mesh && mesh->GetNumberOfPolys() > 0)
            meshes.push_back(mesh);
        else
            missing << meshLabelName(label);
    }
    if (!missing.isEmpty()) {
        QMessageBox::warning(this, tr("Guías"),
                             tr("Faltan modelos para esta guía: %1. Realice la osteotomía correspondiente primero.")
                                 .arg(missing.join(QStringLiteral(", "))));
        return;
    }
    m_guidePlan.sourceLabels = GuidePlanCore::SourceLabelsFor(m_guidePlan.type);
    m_guidePlan.wrap.gapClosingMm = m_guideGapSpin->value();
    m_guidePlan.wrap.smallestDetailMm = m_guideDetailSpin->value();
    m_guidePlan.design.base.smallestDetailMm = m_guideDetailSpin->value();
    m_guidePlan.design.base.thicknessMm = m_guideThicknessSpin->value();
    m_guidePlan.design.base.clearanceMm = m_guideClearanceSpin->value();

    statusBar()->showMessage(tr("Guías: calculando la envolvente…"));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const WrapResult wrap = WrapCore::Wrap(meshes, m_guidePlan.wrap);
    GuidePreparation prepared;
    if (wrap.ok)
        prepared = GuideDesignCore::Prepare(wrap.mesh, m_guidePlan.design); // measured once, reused while marking
    QApplication::restoreOverrideCursor();
    if (!wrap.ok) {
        QMessageBox::warning(this, tr("Guías"), wrap.error);
        return;
    }
    m_guideWrapMesh = wrap.mesh;
    m_guidePrepared = prepared;
    repaintGuideWrap();
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
    const std::vector<std::pair<QPushButton*, int>> owners = {{m_guideRegionButton, kModeRegion},
                                                              {m_guideSlotEndsButton, kModeSlotEnds},
                                                              {m_guideHoleButton, kModeHoles},
                                                              {m_guidePlaceFigureButton, kModeFigure}};
    for (const auto& [button, owned] : owners) {
        if (!button)
            continue;
        QSignalBlocker blocker(button);
        button->setChecked(mode == owned);
    }
    if (m_guideView) {
        // The region is painted with the surface brush; the other modes pick points.
        m_guideView->setSurfaceBrushMode(mode == kModeRegion);
        m_guideView->setPointPickMode(mode != kModeNone && mode != kModeRegion);
    }
    updateGuideUi();
}

void MainWindow::onGuideSurfaceBrushed(double x, double y, double z, Qt::KeyboardModifiers modifiers)
{
    if (m_guidePointMode != kModeRegion || !m_guideWrapMesh)
        return;
    GuideBrushStroke stroke;
    stroke.center = {x, y, z};
    stroke.radiusMm = m_guideBrushSpin ? m_guideBrushSpin->value() : 4.0;
    stroke.erase = modifiers.testFlag(Qt::ControlModifier);
    // Dragging reports many positions: a dab only when the brush has moved a fraction of its size.
    if (!m_guidePlan.paint.empty()) {
        const GuideBrushStroke& last = m_guidePlan.paint.back();
        const double moved = std::hypot(std::hypot(x - last.center[0], y - last.center[1]), z - last.center[2]);
        if (last.erase == stroke.erase && std::abs(last.radiusMm - stroke.radiusMm) < 1e-9 &&
            moved < 0.3 * stroke.radiusMm)
            return;
    }
    m_guidePlan.paint.push_back(stroke);

    // Colour the envelope under the dab right away.
    auto* colors = vtkUnsignedCharArray::SafeDownCast(m_guideWrapMesh->GetPointData()->GetArray("GuidePaint"));
    if (!colors) {
        repaintGuideWrap();
    } else {
        auto locator = vtkSmartPointer<vtkPointLocator>::New();
        locator->SetDataSet(m_guideWrapMesh);
        locator->BuildLocator();
        auto ids = vtkSmartPointer<vtkIdList>::New();
        locator->FindPointsWithinRadius(stroke.radiusMm, stroke.center.data(), ids);
        const QColor color = stroke.erase ? kWrapColor : kPaintColor;
        for (vtkIdType i = 0; i < ids->GetNumberOfIds(); ++i) {
            const vtkIdType id = ids->GetId(i);
            colors->SetTypedTuple(id, std::array<unsigned char, 3>{static_cast<unsigned char>(color.red()),
                                                                   static_cast<unsigned char>(color.green()),
                                                                   static_cast<unsigned char>(color.blue())}
                                          .data());
        }
        colors->Modified();
    }
    if (m_guideView)
        m_guideView->render();
}

void MainWindow::onGuideBrushRadiusDragged(double deltaYPixels)
{
    if (m_guidePointMode != kModeRegion || !m_guideBrushSpin)
        return;
    // Dragging up grows the brush.
    m_guideBrushSpin->setValue(std::clamp(m_guideBrushSpin->value() - 0.05 * deltaYPixels, 0.5, 15.0));
    statusBar()->showMessage(tr("Tamaño del pincel: %1 mm").arg(m_guideBrushSpin->value(), 0, 'f', 1));
}

void MainWindow::onGuideBrushFinished()
{
    updateGuideUi();
}

void MainWindow::repaintGuideWrap()
{
    if (!m_guideWrapMesh || m_guideWrapMesh->GetNumberOfPoints() == 0)
        return;
    auto colors = vtkSmartPointer<vtkUnsignedCharArray>::New();
    colors->SetName("GuidePaint");
    colors->SetNumberOfComponents(3);
    colors->SetNumberOfTuples(m_guideWrapMesh->GetNumberOfPoints());
    const std::array<unsigned char, 3> base{static_cast<unsigned char>(kWrapColor.red()),
                                            static_cast<unsigned char>(kWrapColor.green()),
                                            static_cast<unsigned char>(kWrapColor.blue())};
    for (vtkIdType id = 0; id < m_guideWrapMesh->GetNumberOfPoints(); ++id)
        colors->SetTypedTuple(id, base.data());
    if (!m_guidePlan.paint.empty()) {
        auto locator = vtkSmartPointer<vtkPointLocator>::New();
        locator->SetDataSet(m_guideWrapMesh);
        locator->BuildLocator();
        auto ids = vtkSmartPointer<vtkIdList>::New();
        const std::array<unsigned char, 3> painted{static_cast<unsigned char>(kPaintColor.red()),
                                                   static_cast<unsigned char>(kPaintColor.green()),
                                                   static_cast<unsigned char>(kPaintColor.blue())};
        for (const GuideBrushStroke& stroke : m_guidePlan.paint) { // in order: erasing and repainting both count
            locator->FindPointsWithinRadius(stroke.radiusMm, stroke.center.data(), ids);
            for (vtkIdType i = 0; i < ids->GetNumberOfIds(); ++i)
                colors->SetTypedTuple(ids->GetId(i), stroke.erase ? base.data() : painted.data());
        }
    }
    m_guideWrapMesh->GetPointData()->RemoveArray("GuidePaint");
    m_guideWrapMesh->GetPointData()->SetScalars(colors);
}

void MainWindow::applyGuideLayers()
{
    if (!m_guideView)
        return;
    const auto on = [](QCheckBox* check) { return !check || check->isChecked(); };
    for (int label : GuidePlanCore::SourceLabelsFor(m_guidePlan.type))
        m_guideView->setMeshVisible(objectActorKey(label), on(m_guideShowModelsCheck));
    m_guideView->setMeshVisible(kGuideWrapActorKey, on(m_guideShowWrapCheck));
    m_guideView->setMeshOpacity(kGuideWrapActorKey, m_guideWrapOpacitySpin ? m_guideWrapOpacitySpin->value() : 0.6);
    m_guideView->setMeshVisible(objectActorKey(kGuideMeshLabel), on(m_guideShowGuideCheck));
    for (size_t i = 0; i < m_guidePlan.figures.size(); ++i)
        m_guideView->setMeshVisible(figureActorKey(i), on(m_guideShowFiguresCheck));
    m_guideView->render();
}

void MainWindow::onGuidePointPicked(int, double x, double y, double z)
{
    const std::array<double, 3> point{x, y, z};
    const auto normalAt = [this](const std::array<double, 3>& p) {
        return m_guidePrepared.ok ? GuideDesignCore::SurfaceNormalAt(m_guidePrepared, p)
                                  : std::array<double, 3>{0.0, 0.0, 1.0};
    };
    switch (m_guidePointMode) {
    case kModeRegion:
        m_guidePlan.contour.push_back(point);
        break;
    case kModeSlotEnds: {
        const auto* item = m_guideCutList ? m_guideCutList->currentItem() : nullptr;
        if (!item) {
            statusBar()->showMessage(tr("Guías: elija primero la osteotomía en la lista de ranuras."));
            return;
        }
        m_guidePendingEnds.push_back(point);
        if (m_guidePendingEnds.size() < 2)
            break;
        GuideSlot slot;
        slot.path = m_guideCuts[static_cast<size_t>(item->data(Qt::UserRole).toInt())].path;
        slot.start = m_guidePendingEnds[0];
        slot.end = m_guidePendingEnds[1];
        slot.hasExtent = true;
        m_guidePlan.slotPlan.push_back(slot);
        m_guidePendingEnds.clear();
        m_guideCutList->currentItem()->setCheckState(Qt::Checked);
        break;
    }
    case kModeHoles: {
        GuideFixationHole hole;
        hole.center = point;
        hole.axis = normalAt(point);
        hole.diameterMm = m_guideHoleDiameterSpin ? m_guideHoleDiameterSpin->value() : 2.0;
        m_guidePlan.holes.push_back(hole);
        break;
    }
    case kModeFigure: {
        // Seated on the surface, its z axis along the normal and centred in the guide wall, so a figure that
        // is longer than the wall goes through it.
        GuideFigure figure = guideFigureFromInputs();
        const auto normal = normalAt(point);
        const double wallMiddle = m_guideClearanceSpin->value() + 0.5 * m_guideThicknessSpin->value();
        const std::array<double, 3> center{x + normal[0] * wallMiddle, y + normal[1] * wallMiddle,
                                           z + normal[2] * wallMiddle};
        figure.matrix = GuideDesignCore::FrameAt(center, normal);
        m_guidePlan.figures.push_back(figure);
        refreshGuideFigureList();
        if (m_guideFigureList)
            m_guideFigureList->setCurrentRow(static_cast<int>(m_guidePlan.figures.size()) - 1);
        syncGuideView();
        break;
    }
    default:
        return;
    }
    rebuildGuideMarkers();
    updateGuideUi();
}

void MainWindow::updateGuideFigureInputs()
{
    if (!m_guideFigureShapeCombo)
        return;
    const auto shape = static_cast<GuideFigureShape>(m_guideFigureShapeCombo->currentData().toInt());
    const auto show = [](QDoubleSpinBox* box, bool visible) {
        if (box)
            box->setVisible(visible);
    };
    const bool cylinder = shape == GuideFigureShape::Cylinder;
    const bool box = shape == GuideFigureShape::Box;
    const bool sphere = shape == GuideFigureShape::Sphere;
    show(m_guideFigureDiameterSpin, cylinder || sphere);
    show(m_guideFigureLengthSpin, cylinder);
    show(m_guideFigureWidthSpin, box);
    show(m_guideFigureHeightSpin, box);
    show(m_guideFigureDepthSpin, box);
    // The labels of hidden rows go with them.
    for (auto* spinBox : {m_guideFigureDiameterSpin, m_guideFigureLengthSpin, m_guideFigureWidthSpin,
                          m_guideFigureHeightSpin, m_guideFigureDepthSpin}) {
        if (!spinBox || !spinBox->parentWidget())
            continue;
        const auto labels = spinBox->parentWidget()->findChildren<QLabel*>();
        for (QLabel* label : labels)
            if (label->buddy() == spinBox)
                label->setVisible(spinBox->isVisibleTo(spinBox->parentWidget()));
    }
}

GuideFigure MainWindow::guideFigureFromInputs() const
{
    GuideFigure figure;
    figure.shape = static_cast<GuideFigureShape>(m_guideFigureShapeCombo->currentData().toInt());
    figure.operation = static_cast<GuideFigureOperation>(m_guideFigureOperationCombo->currentData().toInt());
    figure.diameterMm = m_guideFigureDiameterSpin->value();
    figure.lengthMm = m_guideFigureLengthSpin->value();
    figure.widthMm = m_guideFigureWidthSpin->value();
    figure.heightMm = m_guideFigureHeightSpin->value();
    figure.depthMm = m_guideFigureDepthSpin->value();
    return figure;
}

void MainWindow::refreshGuideFigureList()
{
    if (!m_guideFigureList)
        return;
    const int current = m_guideFigureList->currentRow();
    QSignalBlocker blocker(m_guideFigureList);
    m_guideFigureList->clear();
    for (const GuideFigure& figure : m_guidePlan.figures)
        m_guideFigureList->addItem(figureName(figure));
    if (current >= 0 && current < m_guideFigureList->count())
        m_guideFigureList->setCurrentRow(current);
}

void MainWindow::importGuideFigure()
{
    const QString path =
        QFileDialog::getOpenFileName(this, tr("Importar figura"), QString(), tr("STL (*.stl)"));
    if (path.isEmpty())
        return;
    const auto mesh = loadFigureMesh(path);
    if (!mesh) {
        QMessageBox::warning(this, tr("Guías"), tr("No se pudo leer %1.").arg(path));
        return;
    }
    GuideFigure figure;
    figure.shape = GuideFigureShape::Mesh;
    figure.operation = static_cast<GuideFigureOperation>(m_guideFigureOperationCombo->currentData().toInt());
    figure.mesh = mesh;
    figure.sourcePath = path;
    // Placed at the middle of the marked region when there is one; the gizmo takes it from there.
    std::array<double, 3> center{0.0, 0.0, 0.0};
    if (!m_guidePlan.contour.empty()) {
        for (const auto& p : m_guidePlan.contour)
            for (int a = 0; a < 3; ++a)
                center[static_cast<size_t>(a)] += p[static_cast<size_t>(a)] / static_cast<double>(m_guidePlan.contour.size());
    } else if (m_guideWrapMesh) {
        double b[6] = {};
        m_guideWrapMesh->GetBounds(b);
        center = {0.5 * (b[0] + b[1]), 0.5 * (b[2] + b[3]), 0.5 * (b[4] + b[5])};
    }
    const auto normal = m_guidePrepared.ok ? GuideDesignCore::SurfaceNormalAt(m_guidePrepared, center)
                                           : std::array<double, 3>{0.0, 0.0, 1.0};
    figure.matrix = GuideDesignCore::FrameAt(center, normal);
    m_guidePlan.figures.push_back(figure);
    refreshGuideFigureList();
    if (m_guideFigureList)
        m_guideFigureList->setCurrentRow(static_cast<int>(m_guidePlan.figures.size()) - 1);
    syncGuideView();
    updateGuideUi();
    statusBar()->showMessage(tr("Guías: figura importada; muévala con «Mover figura (gizmo)»."));
}

void MainWindow::removeGuideFigure()
{
    const int row = m_guideFigureList ? m_guideFigureList->currentRow() : -1;
    if (row < 0 || row >= static_cast<int>(m_guidePlan.figures.size()))
        return;
    if (m_guideMoveFigureButton && m_guideMoveFigureButton->isChecked())
        m_guideMoveFigureButton->setChecked(false);
    m_guidePlan.figures.erase(m_guidePlan.figures.begin() + row);
    refreshGuideFigureList();
    syncGuideView();
    updateGuideUi();
}

void MainWindow::setGuideFigureGizmo(bool active)
{
    if (!m_guideView)
        return;
    if (!active) {
        if (m_guideView->hasGizmo())
            m_guideView->stopGizmo(); // emits gizmoMeshUpdated → onGuideGizmoUpdated
        return;
    }
    const int row = m_guideFigureList ? m_guideFigureList->currentRow() : -1;
    if (row < 0 || row >= static_cast<int>(m_guidePlan.figures.size())) {
        QSignalBlocker blocker(m_guideMoveFigureButton);
        m_guideMoveFigureButton->setChecked(false);
        statusBar()->showMessage(tr("Guías: elija la figura en la lista para moverla."));
        return;
    }
    setGuidePointMode(kModeNone);
    m_guideView->startGizmo(figureActorKey(static_cast<size_t>(row)));
    statusBar()->showMessage(tr("Guías: mueva y gire la figura; vuelva a pulsar «Mover figura» para fijarla."));
}

void MainWindow::onGuideGizmoUpdated(int label, vtkSmartPointer<vtkPolyData>)
{
    for (size_t i = 0; i < m_guidePlan.figures.size(); ++i) {
        if (figureActorKey(i) != label)
            continue;
        // The gizmo's matrix is what it applied since it started: put the figure's frame through it.
        auto current = vtkSmartPointer<vtkMatrix4x4>::New();
        current->DeepCopy(m_guidePlan.figures[i].matrix.data());
        auto moved = vtkSmartPointer<vtkMatrix4x4>::New();
        vtkMatrix4x4::Multiply4x4(m_guideView->lastGizmoTransformMatrix(), current, moved);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                m_guidePlan.figures[i].matrix[static_cast<size_t>(4 * r + c)] = moved->GetElement(r, c);
        syncGuideView();
        return;
    }
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

    // While marking, the outline is drawn the way the guide will be cut: rounded and laid on the surface.
    vtkSmartPointer<vtkPolyData> outline;
    if (m_guidePrepared.ok && m_guidePlan.paint.empty() && GuideBaseCore::ContourValid(m_guidePlan.contour)) {
        GuideBaseParams params = m_guidePlan.design.base;
        params.cornerRadiusMm = m_guideCornerSpin ? m_guideCornerSpin->value() : params.cornerRadiusMm;
        const GuideRegion region = GuideBaseCore::MakeRegion(m_guidePrepared.wrapField, m_guidePlan.contour, params);
        if (region.valid)
            outline = GuideBaseCore::RegionOutline(region, 0.3);
    }
    if (outline && outline->GetNumberOfLines() > 0)
        m_guideView->setOverlayPolyline(kGuideRegionOverlayKey, outline, kRegionColor, 3.0);
    else if (m_guidePlan.contour.size() >= 2)
        m_guideView->setOverlayPolyline(kGuideRegionOverlayKey, GuideBaseCore::ContourPolyline(m_guidePlan.contour),
                                        kRegionColor, 2.0);
    else
        m_guideView->removeOverlay(kGuideRegionOverlayKey);
    m_guideView->render();
}

void MainWindow::clearGuideRegion()
{
    m_guidePlan.contour.clear();
    m_guidePlan.paint.clear();
    repaintGuideWrap();
    if (m_guideView)
        m_guideView->render();
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
    const bool brushed = GuideBaseCore::PaintValid(m_guidePlan.paint);
    if (!brushed && !GuideBaseCore::ContourValid(m_guidePlan.contour, &error)) {
        QMessageBox::warning(this, tr("Guías"), tr("Pinte con el pincel la zona de apoyo de la guía."));
        return;
    }
    if (m_guideMoveFigureButton && m_guideMoveFigureButton->isChecked())
        m_guideMoveFigureButton->setChecked(false); // fix the figure being moved first
    m_guidePlan.design.base.thicknessMm = m_guideThicknessSpin->value();
    m_guidePlan.design.base.clearanceMm = m_guideClearanceSpin->value();
    m_guidePlan.design.base.smallestDetailMm = m_guideDetailSpin->value();
    if (m_guideCornerSpin)
        m_guidePlan.design.base.cornerRadiusMm = m_guideCornerSpin->value();
    m_guidePlan.design.slot.bladeThicknessMm = m_guideBladeSpin->value();
    m_guidePlan.design.slot.smallestDetailMm = m_guideDetailSpin->value();
    m_guidePlan.design.edgeMarginMm = m_guideMarginSpin->value();

    // Only the osteotomies ticked in the list get a slot.
    std::vector<GuideSlot> chosen;
    for (int row = 0; row < (m_guideCutList ? m_guideCutList->count() : 0); ++row) {
        auto* item = m_guideCutList->item(row);
        if (item->checkState() != Qt::Checked)
            continue;
        const OsteotomyPath& path = m_guideCuts[static_cast<size_t>(item->data(Qt::UserRole).toInt())].path;
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
    // Imported figures reloaded from their file if the mesh is not in memory (after opening a project).
    for (GuideFigure& figure : m_guidePlan.figures)
        if (figure.shape == GuideFigureShape::Mesh && !figure.mesh)
            figure.mesh = loadFigureMesh(figure.sourcePath);

    QApplication::setOverrideCursor(Qt::WaitCursor);
    statusBar()->showMessage(tr("Guías: construyendo…"));
    if (!m_guidePrepared.ok)
        m_guidePrepared = GuideDesignCore::Prepare(m_guideWrapMesh, m_guidePlan.design);
    GuideDesignResult result;
    if (m_guidePrepared.ok) {
        const GuideRegion region =
            brushed ? GuideBaseCore::MakeBrushRegion(m_guidePrepared.wrapField, m_guidePlan.paint, m_guidePlan.design.base)
                    : GuideBaseCore::MakeRegion(m_guidePrepared.wrapField, m_guidePlan.contour, m_guidePlan.design.base);
        result = GuideDesignCore::Build(m_guidePrepared, region, chosen, m_guidePlan.holes, m_guidePlan.figures,
                                        m_guidePlan.design);
    }
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
    updateButtonStates();
    QString report = result.report;
    if (result.pieces > 1)
        report += QStringLiteral(" ") + tr("Atención: la guía quedó en %1 piezas; suba el margen al borde, acorte "
                                           "las ranuras o revise las figuras restadas.").arg(result.pieces);
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
    const QString suggested = m_guidePlan.type == GuideType::Chin ? QStringLiteral("guia_menton.stl")
                                                                  : QStringLiteral("guia_lefort.stl");
    const QString path = QFileDialog::getSaveFileName(this, tr("Exportar guía"), suggested, tr("STL (*.stl)"));
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
    const bool gizmoRunning = m_guideView->hasGizmo();
    if (gizmoRunning)
        return; // rebuilding the scene would drop the figure being moved
    m_guideView->clearMeshes(true);
    // The models the guide sits on, in their planned position, then the envelope, the guide and the figures.
    for (int label : GuidePlanCore::SourceLabelsFor(m_guidePlan.type)) {
        const auto mesh = repositionMeshForLabel(label);
        if (!mesh || mesh->GetNumberOfPolys() == 0)
            continue;
        const int key = objectActorKey(label);
        m_guideView->addMesh(key, mesh, meshLabelName(label));
        m_guideView->setMeshColor(key, objectColorForLabel(label));
        m_guideView->setMeshOpacity(key, 1.0);
    }
    if (m_guideWrapMesh && m_guideWrapMesh->GetNumberOfPolys() > 0) {
        if (!m_guideWrapMesh->GetPointData()->GetArray("GuidePaint"))
            repaintGuideWrap();
        m_guideView->addMesh(kGuideWrapActorKey, m_guideWrapMesh, tr("Envolvente"));
        m_guideView->setMeshColor(kGuideWrapActorKey, kWrapColor);
        m_guideView->setMeshScalarColoring(kGuideWrapActorKey, true); // teal, with the brushed region in blue
        m_guideView->setMeshOpacity(kGuideWrapActorKey, m_guideWrapOpacitySpin ? m_guideWrapOpacitySpin->value() : 0.6);
    }
    if (m_guideMesh && m_guideMesh->GetNumberOfPolys() > 0) {
        const int key = objectActorKey(kGuideMeshLabel);
        m_guideView->addMesh(key, m_guideMesh, meshLabelName(kGuideMeshLabel));
        m_guideView->setMeshColor(key, kGuideColor);
    }
    for (size_t i = 0; i < m_guidePlan.figures.size(); ++i) {
        GuideFigure& figure = m_guidePlan.figures[i];
        if (figure.shape == GuideFigureShape::Mesh && !figure.mesh)
            figure.mesh = loadFigureMesh(figure.sourcePath);
        const auto preview = GuideDesignCore::FigurePreview(figure);
        if (!preview || preview->GetNumberOfPolys() == 0)
            continue;
        const int key = figureActorKey(i);
        m_guideView->addMesh(key, preview, figureName(figure));
        m_guideView->setMeshColor(key, figure.operation == GuideFigureOperation::Add ? kAddColor : kSubtractColor);
        m_guideView->setMeshOpacity(key, 0.55);
        m_guideView->setMeshPickable(key, false); // clicks go through to the surface while placing
    }
    rebuildGuideMarkers();
    applyGuideLayers();
}

void MainWindow::updateGuideUi()
{
    const bool hasWrap = m_guideWrapMesh && m_guideWrapMesh->GetNumberOfPolys() > 0;
    const bool hasRegion = GuideBaseCore::PaintValid(m_guidePlan.paint) || GuideBaseCore::ContourValid(m_guidePlan.contour);
    const bool hasGuide = m_guideMesh && m_guideMesh->GetNumberOfPolys() > 0;
    const bool figureSelected = m_guideFigureList && m_guideFigureList->currentRow() >= 0;
    if (m_guideRegionButton) m_guideRegionButton->setEnabled(hasWrap);
    if (m_guideSlotEndsButton) m_guideSlotEndsButton->setEnabled(hasWrap && m_guideCutList && m_guideCutList->count() > 0);
    if (m_guideHoleButton) m_guideHoleButton->setEnabled(hasWrap);
    if (m_guidePlaceFigureButton) m_guidePlaceFigureButton->setEnabled(hasWrap);
    if (m_guideMoveFigureButton) m_guideMoveFigureButton->setEnabled(figureSelected);
    if (m_guideBuildButton) m_guideBuildButton->setEnabled(hasWrap && hasRegion);
    if (m_guideExportButton) m_guideExportButton->setEnabled(hasGuide);
    if (m_guideThicknessCheck) m_guideThicknessCheck->setEnabled(hasGuide);

    if (!m_guideHintLabel)
        return;
    QString hint;
    if (!hasWrap)
        hint = m_guidePlan.type == GuideType::Chin
                   ? tr("Guía de mentón: pulse «Calcular envolvente» sobre el segmento de mentón y la mandíbula "
                        "post-mentón en su posición planificada.")
                   : tr("Guía Le Fort I: pulse «Calcular envolvente» sobre el segmento Le Fort y la base craneal "
                        "en su posición planificada.");
    else if (m_guidePointMode == kModeRegion || !hasRegion)
        hint = tr("Pulse «Pintar zona» y pinte sobre la envolvente la superficie de apoyo (se ve en azul). "
                  "Ctrl borra; Alt + arrastre vertical cambia el tamaño. La base solo cubre la cara pintada.");
    else if (m_guidePointMode == kModeSlotEnds)
        hint = m_guidePendingEnds.empty() ? tr("Elija la osteotomía en la lista y marque el inicio de la ranura.")
                                          : tr("Marque ahora el final de la ranura.");
    else if (m_guidePointMode == kModeFigure)
        hint = tr("Haga clic donde quiera la figura: se coloca perpendicular a la superficie, centrada en la pared "
                  "de la guía. Luego ajústela con «Mover figura».");
    else if (m_guidePointMode == kModeHoles)
        hint = tr("Haga clic donde quiera cada agujero de fijación; se taladra perpendicular a la superficie.");
    else if (!hasGuide)
        hint = tr("Elija ranuras, añada figuras o agujeros, ajuste espesor y holgura, y pulse «Crear guía».");
    else
        hint = tr("Revise el mapa de espesor y exporte el STL. Las ranuras nunca llegan al borde: la guía sale "
                  "de una pieza.");
    m_guideHintLabel->setText(hint);
}

QJsonObject MainWindow::guidePlanJson() const
{
    if (m_guidePlan.contour.empty() && m_guidePlan.paint.empty() && m_guidePlan.figures.empty() && !m_guideWrapMesh)
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
        rememberOsteotomyCut(tr("Osteotomía %1").arg(index++), slot.path, m_guidePlan.type);
    QStringList missingFiles;
    for (GuideFigure& figure : m_guidePlan.figures)
        if (figure.shape == GuideFigureShape::Mesh && !(figure.mesh = loadFigureMesh(figure.sourcePath)))
            missingFiles << figure.sourcePath;
    if (m_guideTypeCombo) {
        QSignalBlocker blocker(m_guideTypeCombo);
        m_guideTypeCombo->setCurrentIndex(m_guideTypeCombo->findData(static_cast<int>(m_guidePlan.type)));
    }
    if (m_guideGapSpin) m_guideGapSpin->setValue(m_guidePlan.wrap.gapClosingMm);
    if (m_guideDetailSpin) m_guideDetailSpin->setValue(m_guidePlan.wrap.smallestDetailMm);
    if (m_guideCornerSpin) m_guideCornerSpin->setValue(m_guidePlan.design.base.cornerRadiusMm);
    if (m_guideBrushSpin && !m_guidePlan.paint.empty()) m_guideBrushSpin->setValue(m_guidePlan.paint.back().radiusMm);
    if (m_guideThicknessSpin) m_guideThicknessSpin->setValue(m_guidePlan.design.base.thicknessMm);
    if (m_guideClearanceSpin) m_guideClearanceSpin->setValue(m_guidePlan.design.base.clearanceMm);
    if (m_guideBladeSpin) m_guideBladeSpin->setValue(m_guidePlan.design.slot.bladeThicknessMm);
    if (m_guideMarginSpin) m_guideMarginSpin->setValue(m_guidePlan.design.edgeMarginMm);
    refreshGuideSources();
    refreshGuideCutList();
    refreshGuideFigureList();
    updateGuideUi();
    if (!missingFiles.isEmpty())
        statusBar()->showMessage(tr("Guías: no se encontraron las figuras importadas %1.")
                                     .arg(missingFiles.join(QStringLiteral(", "))));
}
