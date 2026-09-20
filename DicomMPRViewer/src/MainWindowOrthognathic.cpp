// ─────────────────────────────────────────────────────────────────────────────
// MainWindow — ORTOGNÁTICA. The ribbon shows ARCHIVO and ORTOGNÁTICA;
// the planning modules (segmentation through cutting guides and plates) are steps of a bar at the top
// of the ribbon, above the actions of the current module. Each step keeps its
// hidden ribbon tab, so selecting a step clicks that tab and the module switches
// exactly as before. Every module opens in the frontal view.
// ─────────────────────────────────────────────────────────────────────────────

#include "MainWindow.h"
#include "Mesh3DView.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
#include <QToolButton>

#include <algorithm>
#include <array>
#include <vector>

namespace
{
constexpr int kOrthoSteps = 9;

const std::array<const char*, kOrthoSteps> kStepNames = {
    QT_TRANSLATE_NOOP("MainWindow", "Segmentación"),   QT_TRANSLATE_NOOP("MainWindow", "Modelos compuestos"),
    QT_TRANSLATE_NOOP("MainWindow", "Orientación"),    QT_TRANSLATE_NOOP("MainWindow", "Osteotomías"),
    QT_TRANSLATE_NOOP("MainWindow", "Registro de mordida"), QT_TRANSLATE_NOOP("MainWindow", "Reposición"),
    QT_TRANSLATE_NOOP("MainWindow", "Férulas"),      QT_TRANSLATE_NOOP("MainWindow", "Placa personalizada"),
    QT_TRANSLATE_NOOP("MainWindow", "Guías")};

const std::array<const char*, kOrthoSteps> kStepHints = {
    QT_TRANSLATE_NOOP("MainWindow", "Segmente el TAC y separe maxilar y mandíbula."),
    QT_TRANSLATE_NOOP("MainWindow", "Registre los escaneos intraorales y cree los modelos compuestos."),
    QT_TRANSLATE_NOOP("MainWindow", "Oriente los modelos al plano de Frankfort."),
    QT_TRANSLATE_NOOP("MainWindow", "Planifique Le Fort I, BSSO y mentoplastia."),
    QT_TRANSLATE_NOOP("MainWindow", "Registre los segmentos con el escaneo de mordida."),
    QT_TRANSLATE_NOOP("MainWindow", "Mueva los segmentos a su posición planificada."),
    QT_TRANSLATE_NOOP("MainWindow", "Diseñe las férulas intermedia y final."),
    QT_TRANSLATE_NOOP("MainWindow", "Defina las placas y sus perforaciones sobre el Le Fort definitivo."),
    QT_TRANSLATE_NOOP("MainWindow", "Cree la guía preoperatoria con las mismas perforaciones de las placas.")};

void repolish(QWidget* widget)
{
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}
} // namespace

const QStringList& MainWindow::orthognathicStepTitles()
{
    // Ribbon tab titles of the steps, in workflow order.
    static const QStringList titles = {QStringLiteral("SEGMENTACION"), QStringLiteral("MODELOS"),
                                       QStringLiteral("ORIENTACION"),  QStringLiteral("OSTEOTOMIA"),
                                       QStringLiteral("REGISTRO MORDIDA"), QStringLiteral("REPOSICIÓN"),
                                       QStringLiteral("FERULA"),   QStringLiteral("PLACAS"),
                                       QStringLiteral("GUIAS")};
    return titles;
}

QWidget* MainWindow::buildOrthognathicStepPanel(QWidget* parent)
{
    auto* panel = new QWidget(parent);
    panel->setObjectName(QStringLiteral("OrthognathicStepPanel"));
    panel->setAttribute(Qt::WA_StyledBackground, true);
    // ID selectors outrank the ribbon's tall QToolButton style: compact stepper buttons.
    panel->setStyleSheet(QStringLiteral(
        "#OrthognathicStepPanel { background:#1c1d20; border-bottom:1px solid #292b30; }"
        "#OrthognathicStepPanel QToolButton { background:transparent; color:#8e8e93;"
        "  border:0; border-bottom:2px solid transparent; border-radius:0; padding:7px 9px; font-size:11px;"
        "  min-width:0px; min-height:22px; }"
        "#OrthognathicStepPanel QPushButton { background:transparent; color:#a7aab2; border:0;"
        "  border-radius:6px; padding:4px 8px; min-width:0px; min-height:22px; font-size:15px; }"
        "#OrthognathicStepPanel QToolButton:hover, #OrthognathicStepPanel QPushButton:hover { background:#292b30; color:#ffffff; }"
        "#OrthognathicStepPanel QToolButton[guideState=\"done\"] { color:#30d158; }"
        "#OrthognathicStepPanel QToolButton:checked { color:#ffffff; border-bottom-color:#0a84ff; font-weight:700; }"
        "#OrthognathicStepPanel QPushButton:disabled { color:#48484a; }"
        "#OrthognathicStepPanel QLabel { color:#8e8e93; font-size:11px; padding-left:8px; }"));
    auto* layout = new QHBoxLayout(panel);
    layout->setContentsMargins(8, 3, 8, 3);
    layout->setSpacing(2);
    m_orthoPrevButton = new QPushButton(QStringLiteral("‹"), panel);
    m_orthoPrevButton->setToolTip(tr("Paso anterior"));
    connect(m_orthoPrevButton, &QPushButton::clicked, this, [this] { selectOrthognathicStep(m_orthoStep - 1); });
    layout->addWidget(m_orthoPrevButton);
    for (int i = 0; i < kOrthoSteps; ++i) {
        auto* button = new QToolButton(panel);
        button->setCheckable(true);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setToolTip(tr(kStepHints[static_cast<size_t>(i)]));
        connect(button, &QToolButton::clicked, this, [this, i] { selectOrthognathicStep(i); });
        m_orthoStepButtons.append(button);
        layout->addWidget(button);
    }
    m_orthoNextButton = new QPushButton(QStringLiteral("›"), panel);
    m_orthoNextButton->setToolTip(tr("Paso siguiente"));
    connect(m_orthoNextButton, &QPushButton::clicked, this, [this] { selectOrthognathicStep(m_orthoStep + 1); });
    layout->addWidget(m_orthoNextButton);
    m_orthoStepMessage = new QLabel(panel);
    layout->addWidget(m_orthoStepMessage, 1);
    m_orthoStepPanel = panel;
    panel->setVisible(false);
    updateOrthognathicSteps();
    return panel;
}

void MainWindow::selectOrthognathicStep(int step)
{
    const QStringList& titles = orthognathicStepTitles();
    if (step < 0 || step >= titles.size())
        return;
    for (auto* tab : findChildren<QToolButton*>(QStringLiteral("MT"))) {
        if (tab != m_orthoTab && tab->text() == titles[step]) {
            tab->click(); // switches the module and calls onModuleTabActivated
            return;
        }
    }
}

void MainWindow::onModuleTabActivated(const QString& title)
{
    const int step = orthognathicStepTitles().indexOf(title);
    if (step >= 0) {
        m_orthoStep = step;
        if (m_orthoTab)
            m_orthoTab->setChecked(true); // exclusive with ARCHIVO
    }
    if (m_orthoStepPanel)
        m_orthoStepPanel->setVisible(step >= 0);
    updateOrthognathicSteps();
    showModuleViewsFrontal(title);
}

void MainWindow::updateOrthognathicSteps()
{
    if (m_orthoStepButtons.size() != kOrthoSteps)
        return;
    const auto present = [](vtkPolyData* mesh) { return mesh && mesh->GetNumberOfPoints() > 0; };
    const std::array<bool, kOrthoSteps> done = {
        m_segmentationLabelmap != nullptr || (objectEntryExists(5) && objectEntryExists(6)),
        present(m_upperCompositeMesh) && present(m_lowerCompositeMesh) && m_compositeStage == CompositeStage::None,
        present(m_orientedInitialMandibleMeshForSplint),
        present(m_leFortSegmentMesh) || present(m_bssoDistalMesh) || present(m_genioBodyMesh),
        m_biteLeFortRegistered || m_biteMandibleRegistered,
        false, // repositioning has no single finished state
        std::any_of(m_splintDesigns.begin(), m_splintDesigns.end(),
                    [this](const SplintDesign& design) { return objectEntryExists(design.label); }),
        !m_guidePlan.plates.empty(),
        present(m_guideMesh)};
    for (int i = 0; i < kOrthoSteps; ++i) {
        QToolButton* button = m_orthoStepButtons[i];
        const QString name = tr(kStepNames[static_cast<size_t>(i)]);
        button->setText(done[static_cast<size_t>(i)] ? QStringLiteral("✓ %1. %2").arg(i + 1).arg(name)
                                                     : QStringLiteral("%1. %2").arg(i + 1).arg(name));
        button->setChecked(i == m_orthoStep);
        const QVariant state = done[static_cast<size_t>(i)] ? QVariant(QStringLiteral("done")) : QVariant();
        if (button->property("guideState") != state) {
            button->setProperty("guideState", state);
            repolish(button);
        }
    }
    if (m_orthoStepMessage)
        m_orthoStepMessage->setText(tr("Paso %1 de %2 · %3")
                                        .arg(m_orthoStep + 1)
                                        .arg(kOrthoSteps)
                                        .arg(tr(kStepHints[static_cast<size_t>(m_orthoStep)])));
    if (m_orthoPrevButton)
        m_orthoPrevButton->setEnabled(m_orthoStep > 0);
    if (m_orthoNextButton)
        m_orthoNextButton->setEnabled(m_orthoStep < kOrthoSteps - 1);
}

void MainWindow::showModuleViewsFrontal(const QString& title)
{
    std::vector<Mesh3DView*> views;
    if (title == QStringLiteral("MODELOS"))
        views = {m_modelMaxillaView, m_modelUpperArchView, m_modelMandibleView, m_modelLowerArchView, m_modelMatchView};
    else if (title == QStringLiteral("ORIENTACION"))
        views = {m_orientationView};
    else if (title == QStringLiteral("OSTEOTOMIA"))
        views = {m_osteotomyView};
    else if (title == QStringLiteral("REGISTRO MORDIDA"))
        views = {m_biteSegmentView, m_biteScanView, m_biteRegistrationView};
    else if (title == QStringLiteral("REPOSICIÓN"))
        views = {m_repositionView};
    else if (title == QStringLiteral("FERULA"))
        views = {m_splintView}; // the upper/lower source views keep their occlusal cameras for marking
    else if (title == QStringLiteral("GUIAS"))
        views = {m_guideView};
    else if (title == QStringLiteral("PLACAS"))
        views = {m_guideView};
    else
        views = {m_mesh3DView};
    for (Mesh3DView* view : views)
        if (view)
            view->setStandardView(0);
}
