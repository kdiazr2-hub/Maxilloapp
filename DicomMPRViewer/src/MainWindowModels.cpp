#include "MainWindow.h"
#include "ModelWorkflowCore.h"
#include "Mesh3DView.h"
#include <QAction>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <vtkPolyData.h>

QWidget* MainWindow::buildModelGuide(QWidget* parent)
{
    m_modelGuide = new QWidget(parent);
    m_modelGuide->setObjectName(QStringLiteral("modelGuide"));
    auto* layout = new QVBoxLayout(m_modelGuide);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(3);
    const QStringList titles = {
        tr("1. STL superior"), tr("2. Puntos superiores"), tr("3. Registro / ajuste"), tr("4. Compuesto superior"),
        tr("5. STL inferior"), tr("6. Puntos inferiores"), tr("7. Registro / ajuste"), tr("8. Compuesto inferior")};
    for (const QString& title : titles) {
        auto* label = new QLabel(title, m_modelGuide);
        label->setWordWrap(true);
        label->setProperty("stepTitle", title);
        m_modelGuideSteps.append(label);
        layout->addWidget(label);
    }
    m_modelGuideMessage = new QLabel(m_modelGuide);
    m_modelGuideMessage->setWordWrap(true);
    m_modelGuideMessage->setStyleSheet(QStringLiteral("color:#f5f5f7; font-size:11px; padding:8px 2px 2px 2px;"));
    m_modelGuidePoints = new QLabel(m_modelGuide);
    m_modelGuidePoints->setWordWrap(true);
    m_modelGuidePoints->setStyleSheet(QStringLiteral("color:#ffd60a; font-size:10px; padding:0 2px;"));
    layout->addWidget(m_modelGuideMessage);
    layout->addWidget(m_modelGuidePoints);
    return m_modelGuide;
}

// Style shared by the guided side panels (MODELOS, ORIENTACION): next step amber, done green,
// active or pressed blue.
QString guidedSidePanelStyle(const QString& objectName)
{
    return QStringLiteral(
        "#%1 { background:#1c1c1e; }"
        "#%1 QLabel#GuidedPanelTitle { color:#ffffff; font-size:14px; font-weight:700; }"
        "#%1 QLabel#GuidedPanelSection { color:#98989d; font-size:10px; font-weight:700; padding-top:8px; }"
        "#%1 QToolButton { background:#2c2c2e; color:#f5f5f7; border:1px solid #3a3a3c;"
        "  border-radius:8px; padding:7px 10px; font-size:11px; }"
        "#%1 QToolButton:hover { background:#3a3a3c; }"
        "#%1 QToolButton[guideState=\"done\"] { background:#24342a; border-color:#34c759; color:#d8f8df; }"
        "#%1 QToolButton[guideState=\"next\"] { background:#6e6257; border-color:#f5d7ad;"
        "  color:#ffffff; font-weight:700; }"
        "#%1 QToolButton:disabled { background:#232325; border-color:#2c2c2e; color:#6e6e73; }"
        "#%1 QToolButton:pressed, #%1 QToolButton:checked {"
        "  background:#0a84ff; border-color:#64d2ff; color:#ffffff; font-weight:700; }").arg(objectName);
}

// Guided panel on the left, like the other modules: steps, instruction and the actions of the current
// step. Its buttons mirror the workflow actions, so enabling, visibility and guide colours stay in one place.
QWidget* MainWindow::buildModelControlPanel(QWidget* parent)
{
    auto* scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setFixedWidth(300);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* panel = new QWidget();
    panel->setObjectName(QStringLiteral("ModelControlPanel"));
    panel->setStyleSheet(guidedSidePanelStyle(panel->objectName()));
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(6);
    auto* title = new QLabel(tr("Modelos compuestos"), panel);
    title->setObjectName(QStringLiteral("GuidedPanelTitle"));
    layout->addWidget(title);
    layout->addWidget(buildModelGuide(panel));
    auto* section = new QLabel(tr("PASO ACTUAL"), panel);
    section->setObjectName(QStringLiteral("GuidedPanelSection"));
    layout->addWidget(section);
    m_modelControlButtons = new QVBoxLayout();
    m_modelControlButtons->setSpacing(5);
    layout->addLayout(m_modelControlButtons);
    layout->addStretch(1);
    scroll->setWidget(panel);
    return scroll;
}

void MainWindow::populateModelControlPanel()
{
    if (m_modelControlPopulated || !m_modelControlButtons || !m_importUpperAct || !m_modelBackAct)
        return;
    m_modelControlPopulated = true;
    const auto addButton = [this](QAction* action) {
        if (!action)
            return;
        auto* button = new QToolButton();
        button->setDefaultAction(action);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setProperty("guideState", action->property("guideState"));
        m_modelControlButtons->addWidget(button);
        // Only the actions of the current step are shown (updateModelWorkflowActions decides).
        button->setVisible(action->isVisible());
        connect(action, &QAction::changed, button, [button, action] { button->setVisible(action->isVisible()); });
    };
    for (QAction* action : {m_importUpperAct, m_importLowerAct, m_maxPtsAct, m_upperPtsAct, m_mandPtsAct,
                            m_lowerPtsAct, m_matchUpperAct, m_matchLowerAct, m_matchBothAct, m_adjustArchAct,
                            m_acceptGizmoAct, m_compositeAct, m_continueNoMatchAct, m_exportAct, m_exportPackAct,
                            m_clearPtsAct, m_resetArchAct})
        addButton(action);
    auto* divider = new QFrame();
    divider->setFrameShape(QFrame::HLine);
    divider->setStyleSheet(QStringLiteral("color:#3a3a3c;"));
    m_modelControlButtons->addWidget(divider);
    addButton(m_modelBackAct);
}

void MainWindow::updateModelWorkflowUi()
{
    populateModelControlPanel();
    updateModelWorkflowActions();
    if (!m_modelGuide) return;
    ModelWorkflowCore::Input input;
    input.jaws[0] = {meshForAnatomicLabel(5) != nullptr, m_upperArchMesh != nullptr,
        m_upperRegistrationCalculated, m_upperCompositeMesh != nullptr,
        int(m_maxillaBonePoints.size()), int(m_upperArchPoints.size())};
    input.jaws[1] = {meshForAnatomicLabel(6) != nullptr, m_lowerArchMesh != nullptr,
        m_lowerRegistrationCalculated, m_lowerCompositeMesh != nullptr,
        int(m_mandibleBonePoints.size()), int(m_lowerArchPoints.size())};
    input.jaw = m_compositeStage == CompositeStage::None
        ? (m_modelStepStack ? m_modelStepStack->currentIndex() : 0) : m_compositeStageStep;
    switch (m_compositeStage) {
    case CompositeStage::Block: input.phase = ModelWorkflowCore::Phase::Block; break;
    case CompositeStage::Computing: input.phase = ModelWorkflowCore::Phase::Computing; break;
    case CompositeStage::Review: input.phase = ModelWorkflowCore::Phase::Review; break;
    default: break;
    }
    input.adjusting = m_dentalGizmoActive;
    if (m_dentalPointSet == DentalPointSet::MaxillaBone || m_dentalPointSet == DentalPointSet::MandibleBone)
        input.capture = ModelWorkflowCore::PointTarget::Bone;
    else if (m_dentalPointSet != DentalPointSet::None)
        input.capture = ModelWorkflowCore::PointTarget::Dental;
    const auto state = ModelWorkflowCore::Evaluate(input);
    for (int i = 0; i < m_modelGuideSteps.size(); ++i) {
        auto* label = m_modelGuideSteps[i];
        label->setText((state.done[i] ? QStringLiteral("✓ ") : QString()) + label->property("stepTitle").toString());
        label->setProperty("complete", state.done[i]);
        label->setProperty("current", state.current == i);
        label->setStyleSheet(state.current == i
            ? QStringLiteral("color:#ffffff;background:#006ecb;padding:4px;font-weight:700;border-radius:4px;")
            : state.done[i] ? QStringLiteral("color:#65d696;padding:4px;")
                            : QStringLiteral("color:#ababaf;padding:4px;"));
    }
    m_modelGuideMessage->setText(state.instruction);
    m_modelGuidePoints->setText(state.points);
    m_modelGuidePoints->setVisible(!state.points.isEmpty());
    const auto limit = [](QAction* action, bool allowed) { if (action) action->setEnabled(action->isEnabled() && allowed); };
    for (auto* action : {m_maxPtsAct, m_upperPtsAct, m_mandPtsAct, m_lowerPtsAct}) limit(action, state.canCapture);
    for (auto* action : {m_matchUpperAct, m_matchLowerAct}) limit(action, state.canRegister);
    limit(m_adjustArchAct, state.canAdjust);
    limit(m_compositeAct, state.canBuild);
    if (m_compositeButton && state.current >= 0)
        m_compositeButton->setEnabled(m_compositeButton->isEnabled() && state.canBuild);
    if (state.canCapture && !state.canRegister) {
        clearGuidedActionStates({m_maxPtsAct, m_upperPtsAct, m_mandPtsAct, m_lowerPtsAct, m_matchUpperAct, m_matchLowerAct});
        const bool bone = state.nextTarget == ModelWorkflowCore::PointTarget::Bone;
        setGuidedNext(input.jaw == 0 ? (bone ? m_maxPtsAct : m_upperPtsAct) : (bone ? m_mandPtsAct : m_lowerPtsAct), state.points);
    }
}
