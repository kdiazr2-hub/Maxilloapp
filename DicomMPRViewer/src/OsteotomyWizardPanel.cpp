#include "OsteotomyWizardPanel.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace
{
QDoubleSpinBox* makeSpin(QWidget* parent, double min, double max, double step, int decimals)
{
    auto* spin = new QDoubleSpinBox(parent);
    spin->setRange(min, max);
    spin->setSingleStep(step);
    spin->setDecimals(decimals);
    spin->setSuffix(QObject::tr(" mm"));
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

void setSpin(QDoubleSpinBox* spin, double value)
{
    const QSignalBlocker blocker(spin);
    spin->setValue(value);
}

const QStringList kStepNames = {
    QObject::tr("Tipo"), QObject::tr("Hueso"), QObject::tr("Puntos"), QObject::tr("Trayectoria"), QObject::tr("Finalizar")};
} // namespace

OsteotomyWizardPanel::OsteotomyWizardPanel(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(10);

    auto* header = new QLabel(tr("Planear osteotomía"), this);
    header->setObjectName("PanelTitle");
    layout->addWidget(header);

    auto* progress = new QGridLayout();
    progress->setHorizontalSpacing(4);
    for (int i = 0; i < 5; ++i) {
        m_stepDots[static_cast<size_t>(i)] = new QLabel(QStringLiteral("●"), this);
        m_stepDots[static_cast<size_t>(i)]->setAlignment(Qt::AlignCenter);
        m_stepTexts[static_cast<size_t>(i)] = new QLabel(kStepNames[i], this);
        m_stepTexts[static_cast<size_t>(i)]->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
        progress->addWidget(m_stepDots[static_cast<size_t>(i)], 0, i);
        progress->addWidget(m_stepTexts[static_cast<size_t>(i)], 1, i);
    }
    layout->addLayout(progress);

    m_title = new QLabel(this);
    m_title->setStyleSheet(QStringLiteral("color:#0a84ff; font-weight:700;"));
    layout->addWidget(m_title);

    m_stack = new QStackedWidget(this);
    layout->addWidget(m_stack, 1);
    const auto makePage = [this] {
        auto* page = new QWidget(m_stack);
        auto* pageLayout = new QVBoxLayout(page);
        pageLayout->setContentsMargins(0, 0, 0, 0);
        pageLayout->setSpacing(8);
        m_stack->addWidget(page);
        return std::pair<QWidget*, QVBoxLayout*>(page, pageLayout);
    };

    // ── 0: type ───────────────────────────────────────────────────────────
    {
        auto [page, pageLayout] = makePage();
        auto* box = new QGroupBox(tr("Tipo de osteotomía"), page);
        auto* boxLayout = new QVBoxLayout(box);
        m_typeGroup = new QButtonGroup(this);
        for (int i = 0; i < 3; ++i) {
            m_typeButtons[static_cast<size_t>(i)] = new QRadioButton(box);
            m_typeGroup->addButton(m_typeButtons[static_cast<size_t>(i)], i);
            boxLayout->addWidget(m_typeButtons[static_cast<size_t>(i)]);
        }
        pageLayout->addWidget(box);
        pageLayout->addWidget(makeMuted(
            tr("Le Fort I: 4 puntos · BSSO: 3 puntos por lado (6) · Genioplastia: 4 puntos. "
               "Las osteotomías se pueden encadenar desde el paso Finalizar."),
            page));
        pageLayout->addStretch(1);
        connect(m_typeGroup, &QButtonGroup::idClicked, this, [this](int id) {
            if (!m_updating)
                emit typeChosen(id);
        });
    }

    // ── 1: bone ───────────────────────────────────────────────────────────
    {
        auto [page, pageLayout] = makePage();
        auto* form = new QFormLayout();
        m_boneCombo = new QComboBox(page);
        form->addRow(tr("Hueso a cortar:"), m_boneCombo);
        pageLayout->addLayout(form);
        m_linkNote = makeMuted(QString(), page);
        pageLayout->addWidget(m_linkNote);
        pageLayout->addStretch(1);
        connect(m_boneCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
            if (!m_updating && index >= 0)
                emit boneChosen(m_boneCombo->itemData(index).toInt());
        });
    }

    // ── 2: landmarks ──────────────────────────────────────────────────────
    {
        auto [page, pageLayout] = makePage();
        m_landmarkPrompt = new QLabel(page);
        m_landmarkPrompt->setWordWrap(true);
        m_landmarkPrompt->setStyleSheet(QStringLiteral(
            "background:#1f3b57; border:1px solid #0a84ff; border-radius:8px; padding:8px; font-weight:700;"));
        pageLayout->addWidget(m_landmarkPrompt);
        m_landmarkHint = makeMuted(QString(), page);
        pageLayout->addWidget(m_landmarkHint);
        auto* arrows = new QHBoxLayout();
        auto* previous = new QPushButton(tr("◀ Anterior"), page);
        auto* next = new QPushButton(tr("Siguiente ▶"), page);
        arrows->addWidget(previous);
        arrows->addWidget(next);
        pageLayout->addLayout(arrows);
        m_landmarkList = new QWidget(page);
        m_landmarkLayout = new QVBoxLayout(m_landmarkList);
        m_landmarkLayout->setContentsMargins(0, 0, 0, 0);
        m_landmarkLayout->setSpacing(4);
        pageLayout->addWidget(m_landmarkList);
        auto* clear = new QPushButton(tr("Borrar puntos"), page);
        pageLayout->addWidget(clear);
        pageLayout->addWidget(makeMuted(
            tr("Clic sobre el hueso marca el punto activo y avanza al siguiente. Arrastre un punto para moverlo; "
               "use las flechas o la lista para volver a indicar uno."),
            page));
        pageLayout->addStretch(1);
        connect(previous, &QPushButton::clicked, this, &OsteotomyWizardPanel::previousLandmarkRequested);
        connect(next, &QPushButton::clicked, this, &OsteotomyWizardPanel::nextLandmarkRequested);
        connect(clear, &QPushButton::clicked, this, &OsteotomyWizardPanel::clearLandmarksRequested);
    }

    // ── 3: cutting path ───────────────────────────────────────────────────
    {
        auto [page, pageLayout] = makePage();
        auto* adjustBox = new QGroupBox(tr("Modificar trayectoria de corte"), page);
        auto* adjustLayout = new QVBoxLayout(adjustBox);
        m_gizmoButton = new QPushButton(tr("Trasladar / rotar / redimensionar"), adjustBox);
        m_gizmoButton->setCheckable(true);
        m_gizmoButton->setStyleSheet(QStringLiteral("QPushButton:checked { background-color: #0a84ff; color: white; }"));
        adjustLayout->addWidget(m_gizmoButton);
        m_gizmoSideCombo = new QComboBox(adjustBox);
        m_gizmoSideCombo->addItem(tr("Corte derecho"), 0);
        m_gizmoSideCombo->addItem(tr("Corte izquierdo"), 1);
        adjustLayout->addWidget(m_gizmoSideCombo);
        auto* reset = new QPushButton(tr("Recalcular desde los puntos"), adjustBox);
        adjustLayout->addWidget(reset);
        adjustLayout->addWidget(makeMuted(
            tr("Flechas: trasladar · anillos: rotar · cubos: redimensionar. Pulse de nuevo el botón para aceptar."),
            adjustBox));
        pageLayout->addWidget(adjustBox);

        auto* pathBox = new QGroupBox(tr("Propiedades"), page);
        auto* pathForm = new QFormLayout(pathBox);
        m_widthSpin = makeSpin(pathBox, 5.0, 250.0, 1.0, 1);
        m_thicknessSpin = makeSpin(pathBox, 0.1, 5.0, 0.1, 1);
        m_extRightSpin = makeSpin(pathBox, 0.0, 100.0, 1.0, 1);
        m_extLeftSpin = makeSpin(pathBox, 0.0, 100.0, 1.0, 1);
        pathForm->addRow(tr("Ancho:"), m_widthSpin);
        pathForm->addRow(tr("Grosor:"), m_thicknessSpin);
        pathForm->addRow(tr("Extensión derecha:"), m_extRightSpin);
        pathForm->addRow(tr("Extensión izquierda:"), m_extLeftSpin);
        m_pathPropertiesBox = pathBox;
        pageLayout->addWidget(pathBox);

        auto* bssoBox = new QGroupBox(tr("Propiedades"), page);
        auto* bssoForm = new QFormLayout(bssoBox);
        m_bssoThicknessSpin = makeSpin(bssoBox, 0.1, 5.0, 0.1, 1);
        m_posteriorSpin = makeSpin(bssoBox, 0.0, 80.0, 1.0, 1);
        m_inferiorSpin = makeSpin(bssoBox, 0.0, 80.0, 1.0, 1);
        m_mediolateralSpin = makeSpin(bssoBox, 0.0, 60.0, 1.0, 1);
        bssoForm->addRow(tr("Grosor:"), m_bssoThicknessSpin);
        bssoForm->addRow(tr("Extensión posterior (rama):"), m_posteriorSpin);
        bssoForm->addRow(tr("Extensión inferior:"), m_inferiorSpin);
        bssoForm->addRow(tr("Extensión mediolateral:"), m_mediolateralSpin);
        m_bssoPropertiesBox = bssoBox;
        pageLayout->addWidget(bssoBox);

        m_showContourCheck = new QCheckBox(tr("Mostrar contorno en los cortes 2D"), page);
        m_showContourCheck->setChecked(true);
        pageLayout->addWidget(m_showContourCheck);
        auto* slices = new QPushButton(tr("Ver cortes TAC"), page);
        pageLayout->addWidget(slices);
        pageLayout->addStretch(1);

        connect(m_gizmoButton, &QPushButton::toggled, this, [this](bool active) {
            if (!m_updating)
                emit gizmoToggled(active);
        });
        connect(reset, &QPushButton::clicked, this, &OsteotomyWizardPanel::resetPathRequested);
        const auto changed = [this] {
            if (!m_updating)
                emit propertiesChanged();
        };
        for (QDoubleSpinBox* spin : {m_widthSpin, m_thicknessSpin, m_extRightSpin, m_extLeftSpin, m_bssoThicknessSpin,
                                     m_posteriorSpin, m_inferiorSpin, m_mediolateralSpin})
            connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, changed);
        connect(m_showContourCheck, &QCheckBox::toggled, this, [this](bool show) {
            if (!m_updating)
                emit showContourToggled(show);
        });
        connect(slices, &QPushButton::clicked, this, &OsteotomyWizardPanel::showSlicesRequested);
    }

    // ── 4: finalize ───────────────────────────────────────────────────────
    {
        auto [page, pageLayout] = makePage();
        pageLayout->addWidget(new QLabel(tr("Objetos nuevos:"), page));
        m_objectTable = new QTableWidget(0, 1, page);
        m_objectTable->horizontalHeader()->setVisible(false);
        m_objectTable->horizontalHeader()->setStretchLastSection(true);
        m_objectTable->verticalHeader()->setVisible(false);
        m_objectTable->setSelectionMode(QAbstractItemView::NoSelection);
        m_objectTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_objectTable->setMinimumHeight(110);
        pageLayout->addWidget(m_objectTable);
        pageLayout->addWidget(makeMuted(tr("Las partes del modelo dental quedan vinculadas a su segmento óseo."), page));
        auto* nextBox = new QGroupBox(tr("Siguiente paso"), page);
        auto* nextLayout = new QVBoxLayout(nextBox);
        m_nextGroup = new QButtonGroup(this);
        const QStringList nextNames = {tr("Crear otra osteotomía"), tr("Ir a registro oclusal"), tr("Ir a reposición")};
        for (int i = 0; i < nextNames.size(); ++i) {
            auto* radio = new QRadioButton(nextNames[i], nextBox);
            m_nextGroup->addButton(radio, i);
            nextLayout->addWidget(radio);
        }
        m_nextGroup->button(AnotherOsteotomy)->setChecked(true);
        pageLayout->addWidget(nextBox);
        pageLayout->addStretch(1);
    }

    m_status = makeMuted(QString(), this);
    layout->addWidget(m_status);
    auto* nav = new QHBoxLayout();
    auto* cancel = new QPushButton(tr("Cancelar"), this);
    m_backButton = new QPushButton(tr("Atrás"), this);
    m_nextButton = new QPushButton(tr("Siguiente"), this);
    m_nextButton->setObjectName("PrimaryButton");
    nav->addWidget(cancel);
    nav->addStretch(1);
    nav->addWidget(m_backButton);
    nav->addWidget(m_nextButton);
    layout->addLayout(nav);
    connect(cancel, &QPushButton::clicked, this, &OsteotomyWizardPanel::cancelRequested);
    connect(m_backButton, &QPushButton::clicked, this, &OsteotomyWizardPanel::backRequested);
    connect(m_nextButton, &QPushButton::clicked, this, &OsteotomyWizardPanel::nextRequested);

    setPathMode(false);
    setStep(TypeStep);
}

void OsteotomyWizardPanel::setStep(int step)
{
    m_step = std::clamp(step, 0, 4);
    m_stack->setCurrentIndex(m_step);
    updateStepIndicator();
}

void OsteotomyWizardPanel::updateStepIndicator()
{
    static const QStringList titles = {tr("Seleccione el tipo de osteotomía"), tr("Seleccione el hueso"),
                                       tr("Indique los puntos anatómicos"), tr("Modifique la trayectoria de corte"),
                                       tr("Finalizar osteotomía")};
    m_title->setText(titles[m_step]);
    for (int i = 0; i < 5; ++i) {
        const bool done = i < m_step;
        const bool active = i == m_step;
        const QString color = done ? QStringLiteral("#34c759") : active ? QStringLiteral("#0a84ff") : QStringLiteral("#8e8e93");
        m_stepDots[static_cast<size_t>(i)]->setText(done ? QStringLiteral("✓") : QStringLiteral("●"));
        m_stepDots[static_cast<size_t>(i)]->setStyleSheet(QStringLiteral("font-size:18px; font-weight:700; color:%1;").arg(color));
        m_stepTexts[static_cast<size_t>(i)]->setStyleSheet(
            QStringLiteral("font-size:10px; color:%1; font-weight:%2;").arg(color, active ? QStringLiteral("700") : QStringLiteral("400")));
    }
}

void OsteotomyWizardPanel::setTypes(const QStringList& names, const std::array<bool, 3>& available,
                                    const std::array<bool, 3>& done, int current)
{
    m_updating = true;
    for (int i = 0; i < 3; ++i) {
        QRadioButton* button = m_typeButtons[static_cast<size_t>(i)];
        QString text = i < names.size() ? names[i] : QString();
        if (done[static_cast<size_t>(i)])
            text += tr("  ✓ aplicada");
        else if (!available[static_cast<size_t>(i)])
            text += tr("  (falta el modelo compuesto)");
        button->setText(text);
        button->setEnabled(available[static_cast<size_t>(i)]);
        button->setChecked(i == current);
    }
    m_updating = false;
}

int OsteotomyWizardPanel::selectedType() const
{
    return m_typeGroup->checkedId();
}

void OsteotomyWizardPanel::setBoneOptions(const std::vector<BoneOption>& options, int currentLabel, const QString& linkNote)
{
    m_updating = true;
    m_boneCombo->clear();
    for (const BoneOption& option : options)
        m_boneCombo->addItem(option.name, option.label);
    const int index = m_boneCombo->findData(currentLabel);
    m_boneCombo->setCurrentIndex(index >= 0 ? index : 0);
    m_linkNote->setText(linkNote);
    m_updating = false;
}

int OsteotomyWizardPanel::selectedBone() const
{
    return m_boneCombo->currentData().toInt();
}

void OsteotomyWizardPanel::setLandmarks(const QStringList& names, const QStringList& hints, const std::vector<bool>& placed,
                                        int current)
{
    if (static_cast<int>(m_landmarkButtons.size()) != names.size()) {
        for (QPushButton* button : m_landmarkButtons)
            button->deleteLater();
        m_landmarkButtons.clear();
        for (int i = 0; i < names.size(); ++i) {
            auto* button = new QPushButton(m_landmarkList);
            button->setCheckable(true);
            connect(button, &QPushButton::clicked, this, [this, i] { emit landmarkChosen(i); });
            m_landmarkLayout->addWidget(button);
            m_landmarkButtons.push_back(button);
        }
    }
    int placedCount = 0;
    for (bool p : placed)
        placedCount += p ? 1 : 0;
    for (int i = 0; i < names.size(); ++i) {
        const bool isPlaced = i < static_cast<int>(placed.size()) && placed[static_cast<size_t>(i)];
        const bool active = i == current;
        QPushButton* button = m_landmarkButtons[static_cast<size_t>(i)];
        const QSignalBlocker blocker(button);
        button->setText(QStringLiteral("%1. %2%3").arg(i + 1).arg(names[i], isPlaced ? QStringLiteral("  ✓") : QString()));
        button->setChecked(active);
        button->setStyleSheet(active ? QStringLiteral("background:#1f3b57; border:1px solid #0a84ff; font-weight:700;")
                              : isPlaced ? QStringLiteral("background:#24342a; border:1px solid #34c759;")
                                         : QString());
    }
    if (current >= 0 && current < names.size()) {
        m_landmarkPrompt->setText(tr("%1 — Punto %2 de %3").arg(names[current]).arg(current + 1).arg(names.size()));
        m_landmarkHint->setText(current < hints.size() ? hints[current] : QString());
    } else {
        m_landmarkPrompt->setText(tr("Puntos completos: %1 de %2").arg(placedCount).arg(names.size()));
        m_landmarkHint->setText(tr("Arrastre un punto para ajustarlo o pulse Siguiente."));
    }
}

void OsteotomyWizardPanel::setPathMode(bool bsso)
{
    m_bssoMode = bsso;
    m_pathPropertiesBox->setVisible(!bsso);
    m_bssoPropertiesBox->setVisible(bsso);
    m_gizmoSideCombo->setVisible(bsso);
}

void OsteotomyWizardPanel::setPathProperties(const PathProperties& p)
{
    m_updating = true;
    setSpin(m_widthSpin, p.widthMm);
    setSpin(m_thicknessSpin, p.thicknessMm);
    setSpin(m_extRightSpin, p.extensionRightMm);
    setSpin(m_extLeftSpin, p.extensionLeftMm);
    setSpin(m_bssoThicknessSpin, p.thicknessMm);
    setSpin(m_posteriorSpin, p.posteriorExtensionMm);
    setSpin(m_inferiorSpin, p.inferiorExtensionMm);
    setSpin(m_mediolateralSpin, p.mediolateralExtensionMm);
    m_updating = false;
}

OsteotomyWizardPanel::PathProperties OsteotomyWizardPanel::pathProperties() const
{
    PathProperties p;
    p.widthMm = m_widthSpin->value();
    p.thicknessMm = m_bssoMode ? m_bssoThicknessSpin->value() : m_thicknessSpin->value();
    p.extensionRightMm = m_extRightSpin->value();
    p.extensionLeftMm = m_extLeftSpin->value();
    p.posteriorExtensionMm = m_posteriorSpin->value();
    p.inferiorExtensionMm = m_inferiorSpin->value();
    p.mediolateralExtensionMm = m_mediolateralSpin->value();
    return p;
}

void OsteotomyWizardPanel::setGizmoActive(bool active)
{
    m_updating = true;
    m_gizmoButton->setChecked(active);
    m_gizmoSideCombo->setEnabled(!active);
    m_updating = false;
}

int OsteotomyWizardPanel::gizmoSide() const
{
    return m_gizmoSideCombo->currentData().toInt();
}

bool OsteotomyWizardPanel::showContour() const
{
    return m_showContourCheck->isChecked();
}

void OsteotomyWizardPanel::setShowContour(bool show)
{
    const QSignalBlocker blocker(m_showContourCheck);
    m_showContourCheck->setChecked(show);
}

void OsteotomyWizardPanel::setCreatedObjects(const std::vector<ObjectRow>& rows)
{
    m_objectTable->setRowCount(static_cast<int>(rows.size()));
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        auto* item = new QTableWidgetItem(QStringLiteral("■  %1").arg(rows[static_cast<size_t>(i)].name));
        item->setForeground(rows[static_cast<size_t>(i)].color);
        m_objectTable->setItem(i, 0, item);
    }
}

int OsteotomyWizardPanel::nextAction() const
{
    return m_nextGroup->checkedId();
}

void OsteotomyWizardPanel::setNavigation(bool canBack, bool canNext, const QString& nextText)
{
    m_backButton->setEnabled(canBack);
    m_nextButton->setEnabled(canNext);
    m_nextButton->setText(nextText);
}

void OsteotomyWizardPanel::setStatus(const QString& text)
{
    m_status->setText(text);
}
