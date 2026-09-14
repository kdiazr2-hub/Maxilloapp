#include "MainWindow.h"
#include "ModelWorkflowCore.h"
#include "Mesh3DView.h"
#include <QAction>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <vtkPolyData.h>

QWidget* MainWindow::buildModelGuide(QWidget* parent)
{
    m_modelGuide = new QWidget(parent);
    m_modelGuide->setObjectName(QStringLiteral("modelGuide"));
    auto* grid = new QGridLayout(m_modelGuide);
    grid->setContentsMargins(10, 6, 10, 6);
    grid->setSpacing(4);
    const QStringList titles = {
        tr("1. STL superior"), tr("2. Puntos superiores"), tr("3. Registro / ajuste"), tr("4. Compuesto superior"),
        tr("5. STL inferior"), tr("6. Puntos inferiores"), tr("7. Registro / ajuste"), tr("8. Compuesto inferior")};
    for (int i = 0; i < titles.size(); ++i) {
        auto* label = new QLabel(titles[i], m_modelGuide);
        label->setWordWrap(true);
        label->setProperty("stepTitle", titles[i]);
        m_modelGuideSteps.append(label);
        grid->addWidget(label, i / 4, i % 4);
        grid->setColumnStretch(i % 4, 1);
    }
    m_modelGuideMessage = new QLabel(m_modelGuide);
    m_modelGuideMessage->setWordWrap(true);
    m_modelGuidePoints = new QLabel(m_modelGuide);
    m_modelGuidePoints->setWordWrap(true);
    grid->addWidget(m_modelGuideMessage, 2, 0, 1, 4);
    grid->addWidget(m_modelGuidePoints, 3, 0, 1, 4);
    return m_modelGuide;
}

void MainWindow::updateModelWorkflowUi()
{
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
            ? QStringLiteral("color:#ffffff;background:#006ecb;padding:4px;font-weight:700;")
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
