#include "SplintDesignPanel.h"

#include "SplintContourEditCore.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace
{
QDoubleSpinBox* makeSpin(QWidget* parent, double min, double max, double step, int decimals, const QString& suffix)
{
    auto* spin = new QDoubleSpinBox(parent);
    spin->setRange(min, max);
    spin->setSingleStep(step);
    spin->setDecimals(decimals);
    spin->setSuffix(suffix);
    spin->setKeyboardTracking(false);
    return spin;
}

QLabel* makeMuted(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setObjectName("MutedText");
    label->setWordWrap(true);
    return label;
}

void selectData(QComboBox* combo, int value)
{
    const QSignalBlocker blocker(combo);
    const int index = combo->findData(value);
    if (index >= 0)
        combo->setCurrentIndex(index);
}

void setSpin(QDoubleSpinBox* spin, double value)
{
    const QSignalBlocker blocker(spin);
    spin->setValue(value);
}

void setCheck(QCheckBox* check, bool value)
{
    const QSignalBlocker blocker(check);
    check->setChecked(value);
}
} // namespace

SplintDesignPanel::SplintDesignPanel(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(10);

    auto* title = new QLabel(tr("Férula por mapas de altura"), this);
    title->setObjectName("PanelTitle");
    layout->addWidget(title);

    // ── Design ────────────────────────────────────────────────────────────
    auto* designBox = new QGroupBox(tr("Diseño"), this);
    auto* designLayout = new QVBoxLayout(designBox);
    m_designCombo = new QComboBox(designBox);
    designLayout->addWidget(m_designCombo);
    auto* designButtons = new QHBoxLayout();
    const auto addDesignButton = [&](const QString& text, void (SplintDesignPanel::*signal)()) {
        auto* button = new QPushButton(text, designBox);
        connect(button, &QPushButton::clicked, this, signal);
        designButtons->addWidget(button);
        return button;
    };
    addDesignButton(tr("Nuevo"), &SplintDesignPanel::newDesignRequested);
    addDesignButton(tr("Copiar"), &SplintDesignPanel::copyDesignRequested);
    addDesignButton(tr("Renombrar"), &SplintDesignPanel::renameDesignRequested);
    m_deleteDesignButton = addDesignButton(tr("Borrar"), &SplintDesignPanel::deleteDesignRequested);
    designLayout->addLayout(designButtons);

    auto* sourceForm = new QFormLayout();
    m_upperSourceCombo = new QComboBox(designBox);
    m_lowerSourceCombo = new QComboBox(designBox);
    sourceForm->addRow(tr("Maxilar:"), m_upperSourceCombo);
    sourceForm->addRow(tr("Mandíbula:"), m_lowerSourceCombo);
    designLayout->addLayout(sourceForm);
    auto* testStlButton = new QPushButton(tr("Cargar STL de prueba…"), designBox);
    testStlButton->setToolTip(tr("Carga un STL superior y uno inferior como fuentes, sin DICOM ni planificación."));
    connect(testStlButton, &QPushButton::clicked, this, &SplintDesignPanel::loadTestStlRequested);
    designLayout->addWidget(testStlButton);
    layout->addWidget(designBox);

    connect(m_designCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (!m_updating && index >= 0)
            emit designSelected(index);
    });
    const auto sourceChanged = [this] {
        if (!m_updating)
            emit sourcesChanged();
    };
    connect(m_upperSourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, sourceChanged);
    connect(m_lowerSourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, sourceChanged);

    // ── Guide points ──────────────────────────────────────────────────────
    auto* pointsBox = new QGroupBox(tr("Puntos guía"), this);
    auto* pointsLayout = new QVBoxLayout(pointsBox);
    pointsLayout->addWidget(makeMuted(
        tr("Al menos 3 puntos por arcada, sobre los dientes justo debajo o encima de los brackets. "
           "Clic izquierdo: poner · arrastrar: mover · clic derecho: borrar."),
        pointsBox));
    auto* pointButtons = new QHBoxLayout();
    m_upperPointsButton = new QPushButton(tr("Puntos maxilar"), pointsBox);
    m_lowerPointsButton = new QPushButton(tr("Puntos mandíbula"), pointsBox);
    m_upperPointsButton->setCheckable(true);
    m_lowerPointsButton->setCheckable(true);
    m_upperPointsButton->setStyleSheet(QStringLiteral(
        "QPushButton:checked { background-color: rgb(255,128,0); color: black; }"));
    m_lowerPointsButton->setStyleSheet(QStringLiteral(
        "QPushButton:checked { background-color: rgb(0,0,255); color: white; }"));
    pointButtons->addWidget(m_upperPointsButton);
    pointButtons->addWidget(m_lowerPointsButton);
    pointsLayout->addLayout(pointButtons);
    m_pointStatus = makeMuted(QString(), pointsBox);
    pointsLayout->addWidget(m_pointStatus);
    auto* clearButtons = new QHBoxLayout();
    auto* clearUpper = new QPushButton(tr("Borrar maxilar"), pointsBox);
    auto* clearLower = new QPushButton(tr("Borrar mandíbula"), pointsBox);
    clearButtons->addWidget(clearUpper);
    clearButtons->addWidget(clearLower);
    pointsLayout->addLayout(clearButtons);
    layout->addWidget(pointsBox);

    const auto togglePoints = [this](int group, bool checked) {
        if (m_updating)
            return;
        m_updating = true;
        if (checked) {
            m_upperPointsButton->setChecked(group == UpperGroup);
            m_lowerPointsButton->setChecked(group == LowerGroup);
        }
        m_updating = false;
        emit pointGroupToggled(group, checked);
    };
    connect(m_upperPointsButton, &QPushButton::toggled, this, [togglePoints](bool c) { togglePoints(UpperGroup, c); });
    connect(m_lowerPointsButton, &QPushButton::toggled, this, [togglePoints](bool c) { togglePoints(LowerGroup, c); });
    connect(clearUpper, &QPushButton::clicked, this, [this] { emit clearPointsRequested(UpperGroup); });
    connect(clearLower, &QPushButton::clicked, this, [this] { emit clearPointsRequested(LowerGroup); });

    // ── Parameters ────────────────────────────────────────────────────────
    auto* paramsBox = new QGroupBox(tr("Parámetros"), this);
    auto* form = new QFormLayout(paramsBox);
    m_edgeOffsetSpin = makeSpin(paramsBox, 0.5, 5.0, 0.5, 1, tr(" mm"));
    m_filletSpin = makeSpin(paramsBox, 0.0, 1.0, 0.1, 1, tr(" mm"));
    m_clearanceSpin = makeSpin(paramsBox, 0.0, 2.0, 0.05, 2, tr(" mm"));
    m_minFeatureSpin = makeSpin(paramsBox, 0.0, 1.0, 0.1, 1, tr(" mm"));
    form->addRow(tr("Margen al borde:"), m_edgeOffsetSpin);
    form->addRow(tr("Fillet:"), m_filletSpin);
    form->addRow(tr("Holgura:"), m_clearanceSpin);
    form->addRow(tr("Detalle mínimo:"), m_minFeatureSpin);
    m_impressionUpperCheck = new QCheckBox(tr("Huella maxilar"), paramsBox);
    m_impressionLowerCheck = new QCheckBox(tr("Huella mandíbula"), paramsBox);
    m_undercutUpperCheck = new QCheckBox(tr("Eliminar retenciones maxilar (5°)"), paramsBox);
    m_undercutLowerCheck = new QCheckBox(tr("Eliminar retenciones mandíbula (45°)"), paramsBox);
    form->addRow(m_impressionUpperCheck);
    form->addRow(m_undercutUpperCheck);
    form->addRow(m_impressionLowerCheck);
    form->addRow(m_undercutLowerCheck);
    layout->addWidget(paramsBox);

    // ── Thickness ─────────────────────────────────────────────────────────
    auto* thicknessBox = new QGroupBox(tr("Grosor"), this);
    auto* thicknessForm = new QFormLayout(thicknessBox);
    m_showThicknessCheck = new QCheckBox(tr("Mostrar grosor"), thicknessBox);
    m_minThicknessSpin = makeSpin(thicknessBox, 0.2, 5.0, 0.1, 1, tr(" mm"));
    m_maxThicknessSpin = makeSpin(thicknessBox, 0.5, 10.0, 0.1, 1, tr(" mm"));
    thicknessForm->addRow(m_showThicknessCheck);
    thicknessForm->addRow(tr("Mínimo:"), m_minThicknessSpin);
    thicknessForm->addRow(tr("Máximo:"), m_maxThicknessSpin);
    thicknessForm->addRow(makeMuted(tr("Rojo: bajo el mínimo · amarillo: en rango · morado: sobre el máximo."), thicknessBox));
    layout->addWidget(thicknessBox);

    const auto paramChanged = [this] {
        updateUndercutEnabled();
        if (!m_updating)
            emit paramsChanged();
    };
    for (QDoubleSpinBox* spin : {m_edgeOffsetSpin, m_filletSpin, m_clearanceSpin, m_minFeatureSpin,
                                 m_minThicknessSpin, m_maxThicknessSpin})
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, paramChanged);
    for (QCheckBox* check : {m_impressionUpperCheck, m_impressionLowerCheck, m_undercutUpperCheck, m_undercutLowerCheck})
        connect(check, &QCheckBox::toggled, this, paramChanged);
    connect(m_showThicknessCheck, &QCheckBox::toggled, this, [this](bool show) {
        if (!m_updating)
            emit showThicknessToggled(show);
    });

    // ── Contour ───────────────────────────────────────────────────────────
    auto* contourBox = new QGroupBox(tr("Contorno"), this);
    auto* contourLayout = new QVBoxLayout(contourBox);
    auto* contourButtons = new QHBoxLayout();
    m_editContourButton = new QPushButton(tr("Editar contorno"), contourBox);
    m_editContourButton->setCheckable(true);
    m_resetContourButton = new QPushButton(tr("Restablecer"), contourBox);
    contourButtons->addWidget(m_editContourButton);
    contourButtons->addWidget(m_resetContourButton);
    contourLayout->addLayout(contourButtons);
    auto* influenceForm = new QFormLayout();
    m_influenceSpin = makeSpin(contourBox, SplintContourEditCore::kMinInfluencePercent,
                               SplintContourEditCore::kMaxInfluencePercent, 1.0, 0, tr(" %"));
    m_influenceSpin->setValue(SplintContourEditCore::kDefaultInfluencePercent);
    influenceForm->addRow(tr("Radio de influencia:"), m_influenceSpin);
    contourLayout->addLayout(influenceForm);
    m_contourNote = makeMuted(
        tr("Arrastre el contorno rojo en las vistas oclusales; Alt + arrastre vertical cambia el radio. "
           "Los cambios del contorno se pierden si se mueven los puntos guía."),
        contourBox);
    contourLayout->addWidget(m_contourNote);
    layout->addWidget(contourBox);

    connect(m_editContourButton, &QPushButton::toggled, this, [this](bool editing) {
        if (!m_updating)
            emit contourEditToggled(editing);
    });
    connect(m_resetContourButton, &QPushButton::clicked, this, &SplintDesignPanel::resetContourRequested);
    connect(m_influenceSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double percent) {
        if (!m_updating)
            emit influenceChanged(percent);
    });

    // ── Extras: bevel, wire holes, bracket margins ────────────────────────
    auto* extrasBox = new QGroupBox(tr("Bisel, agujeros y brackets"), this);
    auto* extrasLayout = new QVBoxLayout(extrasBox);
    const auto makeToolButton = [&](const QString& text) {
        auto* button = new QPushButton(text, extrasBox);
        button->setCheckable(true);
        button->setStyleSheet(QStringLiteral("QPushButton:checked { background-color: #0a84ff; color: white; }"));
        return button;
    };

    auto* bevelRow = new QHBoxLayout();
    m_bevelButton = makeToolButton(tr("Editar bisel"));
    m_removeBevelButton = new QPushButton(tr("Quitar bisel"), extrasBox);
    bevelRow->addWidget(m_bevelButton);
    bevelRow->addWidget(m_removeBevelButton);
    extrasLayout->addLayout(bevelRow);
    extrasLayout->addWidget(makeMuted(
        tr("Dos puntos sobre los dientes (maxilar y mandíbula) definen el plano del bisel. "
           "Arrastre para ajustarlos; Ctrl + arrastre los mueve en el aire."),
        extrasBox));

    auto* holeRow = new QHBoxLayout();
    m_holeButton = makeToolButton(tr("Colocar agujeros"));
    m_removeHolesButton = new QPushButton(tr("Quitar agujeros"), extrasBox);
    holeRow->addWidget(m_holeButton);
    holeRow->addWidget(m_removeHolesButton);
    extrasLayout->addLayout(holeRow);
    auto* holeForm = new QFormLayout();
    m_holeDiameterSpin = makeSpin(extrasBox, 0.5, 4.0, 0.1, 1, tr(" mm"));
    m_holeOrientationCombo = new QComboBox(extrasBox);
    m_holeOrientationCombo->addItem(tr("Normal a la superficie"), static_cast<int>(SplintHoleOrientation::SurfaceNormal));
    m_holeOrientationCombo->addItem(tr("Seguir el bisel"), static_cast<int>(SplintHoleOrientation::Bevel));
    holeForm->addRow(tr("Diámetro:"), m_holeDiameterSpin);
    holeForm->addRow(tr("Orientación:"), m_holeOrientationCombo);
    extrasLayout->addLayout(holeForm);
    extrasLayout->addWidget(makeMuted(
        tr("Clic sobre la vista previa coloca un cilindro; clic derecho sobre su marca lo quita. "
           "Todos los cilindros se restan al crear la férula."),
        extrasBox));

    auto* bracketRow = new QHBoxLayout();
    m_bracketButton = makeToolButton(tr("Marcar brackets"));
    m_clearMarksButton = new QPushButton(tr("Desmarcar todo"), extrasBox);
    bracketRow->addWidget(m_bracketButton);
    bracketRow->addWidget(m_clearMarksButton);
    extrasLayout->addLayout(bracketRow);
    auto* bracketForm = new QFormLayout();
    m_bracketOffsetCombo = new QComboBox(extrasBox);
    for (double value : {0.25, 0.5, 0.75, 1.0, 1.5, 2.0})
        m_bracketOffsetCombo->addItem(tr("%1 mm").arg(value, 0, 'f', 2), value);
    m_brushRadiusSpin = makeSpin(extrasBox, 0.5, 5.0, 0.25, 2, tr(" mm"));
    bracketForm->addRow(tr("Margen:"), m_bracketOffsetCombo);
    bracketForm->addRow(tr("Radio del pincel:"), m_brushRadiusSpin);
    extrasLayout->addLayout(bracketForm);
    extrasLayout->addWidget(makeMuted(
        tr("Clic o arrastre sobre los dientes marca; Ctrl desmarca; Alt + arrastre vertical cambia el pincel. "
           "Las zonas marcadas reciben el margen indicado en las huellas."),
        extrasBox));
    m_extrasSummary = makeMuted(QString(), extrasBox);
    extrasLayout->addWidget(m_extrasSummary);
    layout->addWidget(extrasBox);

    for (auto [button, tool] : {std::pair{m_bevelButton, int(BevelTool)}, std::pair{m_holeButton, int(WireHoleTool)},
                                std::pair{m_bracketButton, int(BracketTool)}}) {
        connect(button, &QPushButton::toggled, this, [this, tool = tool](bool active) {
            if (m_updating)
                return;
            if (active)
                setExtrasTool(tool);
            emit extrasToolToggled(tool, active);
        });
    }
    connect(m_removeBevelButton, &QPushButton::clicked, this, &SplintDesignPanel::removeBevelRequested);
    connect(m_removeHolesButton, &QPushButton::clicked, this, &SplintDesignPanel::removeWireHolesRequested);
    connect(m_clearMarksButton, &QPushButton::clicked, this, &SplintDesignPanel::clearBracketMarksRequested);
    const auto extrasChanged = [this] {
        if (!m_updating)
            emit extrasSettingsChanged();
    };
    connect(m_holeDiameterSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, extrasChanged);
    connect(m_brushRadiusSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, extrasChanged);
    connect(m_holeOrientationCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, extrasChanged);
    connect(m_bracketOffsetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, extrasChanged);

    // ── Preview, creation, export ─────────────────────────────────────────
    m_staleWarning = new QLabel(this);
    m_staleWarning->setWordWrap(true);
    m_staleWarning->setStyleSheet(QStringLiteral("color:#ff6b6b; font-weight:600;"));
    m_staleWarning->setVisible(false);
    layout->addWidget(m_staleWarning);
    m_previewStatus = makeMuted(QString(), this);
    layout->addWidget(m_previewStatus);
    m_createButton = new QPushButton(tr("Crear férula"), this);
    m_createButton->setObjectName("PrimaryButton");
    m_createButton->setMinimumHeight(34);
    connect(m_createButton, &QPushButton::clicked, this, &SplintDesignPanel::createRequested);
    layout->addWidget(m_createButton);
    m_exportButton = new QPushButton(tr("Exportar STL"), this);
    connect(m_exportButton, &QPushButton::clicked, this, &SplintDesignPanel::exportRequested);
    layout->addWidget(m_exportButton);
    auto* extraExports = new QHBoxLayout();
    auto* exportPoints = new QPushButton(tr("Exportar puntos"), this);
    auto* exportReport = new QPushButton(tr("Exportar informe"), this);
    connect(exportPoints, &QPushButton::clicked, this, &SplintDesignPanel::exportPointsRequested);
    connect(exportReport, &QPushButton::clicked, this, &SplintDesignPanel::exportReportRequested);
    extraExports->addWidget(exportPoints);
    extraExports->addWidget(exportReport);
    layout->addLayout(extraExports);
    m_report = makeMuted(QString(), this);
    m_report->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_report);
    layout->addStretch(1);

    setParams(SplintHeightmapParams{});
    setExtrasSettings(1.0, SplintHoleOrientation::SurfaceNormal, 0.5, 1.5);
    setExtrasSummary(false, false, 0, 0);
    setPointCounts(0, 0);
    setContourEdited(false);
    setCanCreate(false);
    setCanExport(false);
}

void SplintDesignPanel::setDesigns(const QStringList& names, int currentIndex, bool currentIsBuiltIn)
{
    m_updating = true;
    m_designCombo->clear();
    m_designCombo->addItems(names);
    m_designCombo->setCurrentIndex(currentIndex);
    m_deleteDesignButton->setEnabled(!currentIsBuiltIn);
    m_updating = false;
}

void SplintDesignPanel::setSourceOptions(const std::vector<SourceOption>& upper, const std::vector<SourceOption>& lower)
{
    m_updating = true;
    const int upperValue = upperSource();
    const int lowerValue = lowerSource();
    for (auto [combo, options] : {std::pair{m_upperSourceCombo, &upper}, std::pair{m_lowerSourceCombo, &lower}}) {
        combo->clear();
        for (const SourceOption& option : *options)
            combo->addItem(option.name, option.value);
    }
    selectData(m_upperSourceCombo, upperValue);
    selectData(m_lowerSourceCombo, lowerValue);
    m_updating = false;
}

void SplintDesignPanel::setSources(int upperValue, int lowerValue)
{
    selectData(m_upperSourceCombo, upperValue);
    selectData(m_lowerSourceCombo, lowerValue);
}

int SplintDesignPanel::upperSource() const
{
    return m_upperSourceCombo->currentData().toInt();
}

int SplintDesignPanel::lowerSource() const
{
    return m_lowerSourceCombo->currentData().toInt();
}

void SplintDesignPanel::setParams(const SplintHeightmapParams& p)
{
    m_updating = true;
    setSpin(m_edgeOffsetSpin, p.edgeOffsetMm);
    setSpin(m_filletSpin, p.filletMm);
    setSpin(m_clearanceSpin, p.clearanceMm);
    setSpin(m_minFeatureSpin, p.minFeatureMm);
    setCheck(m_impressionUpperCheck, p.impressionUpper);
    setCheck(m_impressionLowerCheck, p.impressionLower);
    setCheck(m_undercutUpperCheck, p.undercutUpper);
    setCheck(m_undercutLowerCheck, p.undercutLower);
    setSpin(m_minThicknessSpin, p.minThicknessMm);
    setSpin(m_maxThicknessSpin, p.maxThicknessMm);
    updateUndercutEnabled();
    m_updating = false;
}

SplintHeightmapParams SplintDesignPanel::params(const SplintHeightmapParams& base) const
{
    SplintHeightmapParams p = base;
    p.edgeOffsetMm = m_edgeOffsetSpin->value();
    p.filletMm = m_filletSpin->value();
    p.clearanceMm = m_clearanceSpin->value();
    p.minFeatureMm = m_minFeatureSpin->value();
    p.impressionUpper = m_impressionUpperCheck->isChecked();
    p.impressionLower = m_impressionLowerCheck->isChecked();
    p.undercutUpper = p.impressionUpper && m_undercutUpperCheck->isChecked();
    p.undercutLower = p.impressionLower && m_undercutLowerCheck->isChecked();
    p.minThicknessMm = m_minThicknessSpin->value();
    p.maxThicknessMm = std::max(m_maxThicknessSpin->value(), p.minThicknessMm);
    return p;
}

void SplintDesignPanel::setPointCounts(int upper, int lower)
{
    m_pointStatus->setText(tr("Maxilar %1/3 · Mandíbula %2/3").arg(upper).arg(lower));
}

void SplintDesignPanel::setActivePointGroup(int group)
{
    m_updating = true;
    m_upperPointsButton->setChecked(group == UpperGroup);
    m_lowerPointsButton->setChecked(group == LowerGroup);
    m_updating = false;
}

void SplintDesignPanel::setContourEditing(bool editing)
{
    m_updating = true;
    m_editContourButton->setChecked(editing);
    m_updating = false;
}

void SplintDesignPanel::setContourEdited(bool edited)
{
    m_resetContourButton->setEnabled(edited);
}

void SplintDesignPanel::setInfluencePercent(double percent)
{
    setSpin(m_influenceSpin, percent);
}

double SplintDesignPanel::influencePercent() const
{
    return m_influenceSpin->value();
}

bool SplintDesignPanel::showThickness() const
{
    return m_showThicknessCheck->isChecked();
}

void SplintDesignPanel::setPreviewStatus(const QString& text)
{
    m_previewStatus->setText(text);
}

void SplintDesignPanel::setReport(const QString& text)
{
    m_report->setText(text);
}

void SplintDesignPanel::setCanCreate(bool enabled)
{
    m_createButton->setEnabled(enabled);
}

void SplintDesignPanel::setCanExport(bool enabled)
{
    m_exportButton->setEnabled(enabled);
}

void SplintDesignPanel::setExtrasTool(int tool)
{
    m_updating = true;
    m_bevelButton->setChecked(tool == BevelTool);
    m_holeButton->setChecked(tool == WireHoleTool);
    m_bracketButton->setChecked(tool == BracketTool);
    m_updating = false;
}

void SplintDesignPanel::setExtrasSettings(double holeDiameterMm, SplintHoleOrientation orientation,
                                          double bracketOffsetMm, double brushRadiusMm)
{
    m_updating = true;
    setSpin(m_holeDiameterSpin, holeDiameterMm);
    selectData(m_holeOrientationCombo, static_cast<int>(orientation));
    int offsetIndex = -1;
    for (int i = 0; i < m_bracketOffsetCombo->count(); ++i)
        if (std::abs(m_bracketOffsetCombo->itemData(i).toDouble() - bracketOffsetMm) < 1e-6)
            offsetIndex = i;
    if (offsetIndex < 0) {
        m_bracketOffsetCombo->addItem(tr("%1 mm").arg(bracketOffsetMm, 0, 'f', 2), bracketOffsetMm);
        offsetIndex = m_bracketOffsetCombo->count() - 1;
    }
    {
        const QSignalBlocker blocker(m_bracketOffsetCombo);
        m_bracketOffsetCombo->setCurrentIndex(offsetIndex);
    }
    setSpin(m_brushRadiusSpin, brushRadiusMm);
    m_updating = false;
}

void SplintDesignPanel::setExtrasSummary(bool bevel, bool bevelPending, int holes, int marks)
{
    m_removeBevelButton->setEnabled(bevel || bevelPending);
    m_removeHolesButton->setEnabled(holes > 0);
    m_clearMarksButton->setEnabled(marks > 0);
    const QString bevelText = bevel ? tr("bisel activo") : bevelPending ? tr("bisel: falta 1 punto") : tr("sin bisel");
    m_extrasSummary->setText(tr("%1 · %2 agujero(s) · %3 marca(s) de bracket").arg(bevelText).arg(holes).arg(marks));
}

double SplintDesignPanel::wireHoleDiameter() const
{
    return m_holeDiameterSpin->value();
}

SplintHoleOrientation SplintDesignPanel::wireHoleOrientation() const
{
    return static_cast<SplintHoleOrientation>(m_holeOrientationCombo->currentData().toInt());
}

double SplintDesignPanel::bracketOffset() const
{
    return m_bracketOffsetCombo->currentData().toDouble();
}

double SplintDesignPanel::brushRadius() const
{
    return m_brushRadiusSpin->value();
}

void SplintDesignPanel::setStaleWarning(const QString& text)
{
    m_staleWarning->setText(text);
    m_staleWarning->setVisible(!text.isEmpty());
}

void SplintDesignPanel::updateUndercutEnabled()
{
    m_undercutUpperCheck->setEnabled(m_impressionUpperCheck->isChecked());
    m_undercutLowerCheck->setEnabled(m_impressionLowerCheck->isChecked());
}
