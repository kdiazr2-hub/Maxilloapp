// ─────────────────────────────────────────────────────────────────────────────
// MainWindow — ORIENTACION guided side panel (Frankfort plane), like MODELOS:
// steps, instruction and the module actions as buttons. The actions keep their
// own enabling logic (updateFrankfurtPointStatus); the guide follows it.
// ─────────────────────────────────────────────────────────────────────────────

#include "MainWindow.h"

#include <QAction>
#include <QFrame>
#include <QLabel>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

// Defined in MainWindowModels.cpp.
QString guidedSidePanelStyle(const QString& objectName);

QWidget* MainWindow::buildOrientationControlPanel(QWidget* parent)
{
    auto* scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setFixedWidth(300);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* panel = new QWidget();
    panel->setObjectName(QStringLiteral("OrientationControlPanel"));
    panel->setStyleSheet(guidedSidePanelStyle(panel->objectName()));
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(6);
    auto* title = new QLabel(tr("Orientación — plano de Frankfort"), panel);
    title->setObjectName(QStringLiteral("GuidedPanelTitle"));
    title->setWordWrap(true);
    layout->addWidget(title);
    const QStringList steps = {tr("1. Porion derecho"), tr("2. Porion izquierdo"), tr("3. Orbitale derecho"),
                               tr("4. Orbitale izquierdo"), tr("5. Alinear al plano de Frankfort"),
                               tr("6. Línea media (opcional)"), tr("7. Guardar orientación")};
    for (const QString& step : steps) {
        auto* label = new QLabel(step, panel);
        label->setWordWrap(true);
        label->setProperty("stepTitle", step);
        m_orientationGuideSteps.append(label);
        layout->addWidget(label);
    }
    m_orientationGuideMessage = new QLabel(panel);
    m_orientationGuideMessage->setWordWrap(true);
    m_orientationGuideMessage->setStyleSheet(QStringLiteral("color:#f5f5f7; font-size:11px; padding:8px 2px 2px 2px;"));
    layout->addWidget(m_orientationGuideMessage);
    auto* section = new QLabel(tr("ACCIONES"), panel);
    section->setObjectName(QStringLiteral("GuidedPanelSection"));
    layout->addWidget(section);
    m_orientationControlButtons = new QVBoxLayout();
    m_orientationControlButtons->setSpacing(5);
    layout->addLayout(m_orientationControlButtons);
    layout->addStretch(1);
    scroll->setWidget(panel);
    return scroll;
}

void MainWindow::populateOrientationControlPanel()
{
    if (m_orientationControlPopulated || !m_orientationControlButtons || !m_alignFrankfurtAct)
        return;
    m_orientationControlPopulated = true;
    for (QAction* action : {m_alignFrankfurtAct, m_midlineGizmoAct,
                            m_midlineAcceptGizmoAct, m_saveOrientationAct}) {
        auto* button = new QToolButton();
        button->setDefaultAction(action);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setProperty("guideState", action->property("guideState"));
        m_orientationControlButtons->addWidget(button);
        // Landmarks unlock one after another and alignment enables the rest: the guide follows.
        connect(action, &QAction::changed, this, &MainWindow::updateOrientationGuide);
    }
}

void MainWindow::updateOrientationGuide()
{
    populateOrientationControlPanel();
    if (m_orientationGuideSteps.isEmpty() || !m_orientationGuideMessage)
        return;
    const int points = std::min(static_cast<int>(m_frankfurtPoints.size()), 4);
    const bool hasModels = m_upperCompositeMesh || m_lowerCompositeMesh;
    const bool aligned = m_saveOrientationAct && m_saveOrientationAct->isEnabled();
    const bool adjusting = m_midlineAcceptGizmoAct && m_midlineAcceptGizmoAct->isEnabled();
    const int current = !hasModels ? -1 : points < 4 ? points : !aligned ? 4 : adjusting ? 5 : 6;
    for (int i = 0; i < m_orientationGuideSteps.size(); ++i) {
        const bool done = (i < 4 && i < points) || (i == 4 && aligned);
        auto* label = m_orientationGuideSteps[i];
        label->setText((done ? QStringLiteral("✓ ") : QString()) + label->property("stepTitle").toString());
        label->setProperty("current", current == i);
        label->setStyleSheet(current == i
            ? QStringLiteral("color:#ffffff;background:#252b33;border-left:3px solid #0a84ff;padding:7px 8px;font-weight:600;border-radius:3px;")
            : done ? QStringLiteral("color:#30d158;padding:7px 8px;")
                   : QStringLiteral("color:#a7aab2;padding:7px 8px;"));
    }
    static const char* const landmarkHints[4] = {
        QT_TR_NOOP("Marque el Porion derecho: punto más alto del conducto auditivo en la vista lateral derecha."),
        QT_TR_NOOP("Marque el Porion izquierdo: punto más alto del conducto auditivo en la vista lateral izquierda."),
        QT_TR_NOOP("Marque el Orbitale derecho: punto más bajo del reborde orbitario en la vista frontal."),
        QT_TR_NOOP("Marque el Orbitale izquierdo: punto más bajo del reborde orbitario en la vista frontal.")};
    QString message;
    if (!hasModels)
        message = tr("No hay modelos que orientar: créelos en MODELOS o use «Continuar sin escaneo».");
    else if (points < 4)
        message = tr(landmarkHints[points]);
    else if (!aligned)
        message = tr("Los 4 puntos están marcados. Pulse «Alinear al plano de Frankfort».");
    else if (adjusting)
        message = tr("Gire con el gizmo hasta centrar la línea media y pulse «Aceptar ajuste de línea media».");
    else
        message = tr("Orientación lista. Si hace falta, ajuste la línea media; después pulse «Guardar orientación y seguir».");
    m_orientationGuideMessage->setText(message);
}
