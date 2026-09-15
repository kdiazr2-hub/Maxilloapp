// ─────────────────────────────────────────────────────────────────────────────
// MainWindow — OSTEOTOMIA workspace, ProPlan-style "Plan Osteotomy" wizard.
//
// Select type (Le Fort I / BSSO / genioplasty) → select bone (the linked dental
// scan is part of the composite and is cut with it) → indicate landmarks
// (auto-advance, drag or re-indicate) → modify the cutting path (gizmo and
// properties, contour on the CT slices) → apply and finalize (new objects and
// next step). Geometry lives in OsteotomyCore; this file only wires the UI.
// ─────────────────────────────────────────────────────────────────────────────

#include "MainWindow.h"

#include "CompositeBlockCore.h"
#include "LoggerCore.h"
#include "Mesh3DView.h"
#include "ObjectLabels.h"
#include "OsteotomyCore.h"
#include "OsteotomyWizardPanel.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QLocale>
#include <QMessageBox>
#include <QScrollArea>
#include <QStatusBar>
#include <QToolButton>

#include <vtkAppendPolyData.h>
#include <vtkMatrix4x4.h>
#include <vtkPolyData.h>
#include <vtkPolyDataNormals.h>

#include <algorithm>
#include <utility>

// Defined in MainWindow.cpp.
QString meshLabelName(int label);
QColor meshLabelColor(int label);

namespace
{
constexpr double kLandmarkRadiusMm = 1.2;
const QColor kGuideColor(255, 140, 40);
const std::array<QColor, 6> kLandmarkColors = {QColor(255, 80, 80),  QColor(80, 140, 255), QColor(60, 210, 110),
                                               QColor(0, 220, 230),  QColor(240, 200, 40), QColor(220, 90, 230)};

OsteotomyType typeOf(int type)
{
    return static_cast<OsteotomyType>(std::clamp(type, 0, 2));
}

bool hasMesh(const vtkSmartPointer<vtkPolyData>& mesh)
{
    return mesh && mesh->GetNumberOfCells() > 0;
}
} // namespace

QWidget* MainWindow::buildOsteotomyWizard(QWidget* parent)
{
    // ProPlan defaults per type (Properties.User.xml).
    m_ostProperties[static_cast<size_t>(OsteotomyType::LeFortI)].widthMm = OsteotomyCore::LeFortWidthMm;
    m_ostProperties[static_cast<size_t>(OsteotomyType::LeFortI)].extensionRightMm = OsteotomyCore::LeFortExtensionMm;
    m_ostProperties[static_cast<size_t>(OsteotomyType::LeFortI)].extensionLeftMm = OsteotomyCore::LeFortExtensionMm;
    m_ostProperties[static_cast<size_t>(OsteotomyType::Genioplasty)].widthMm = OsteotomyCore::GenioWidthMm;
    m_ostProperties[static_cast<size_t>(OsteotomyType::Genioplasty)].extensionRightMm = OsteotomyCore::GenioExtensionMm;
    m_ostProperties[static_cast<size_t>(OsteotomyType::Genioplasty)].extensionLeftMm = OsteotomyCore::GenioExtensionMm;

    auto* scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setFixedWidth(340);
    m_osteotomyWizard = new OsteotomyWizardPanel();
    scroll->setWidget(m_osteotomyWizard);

    auto* panel = m_osteotomyWizard;
    connect(panel, &OsteotomyWizardPanel::typeChosen, this, &MainWindow::selectOsteotomyType);
    connect(panel, &OsteotomyWizardPanel::boneChosen, this, &MainWindow::selectOsteotomyBone);
    connect(panel, &OsteotomyWizardPanel::landmarkChosen, this, &MainWindow::setOsteotomyLandmarkIndex);
    connect(panel, &OsteotomyWizardPanel::previousLandmarkRequested, this, [this] {
        const int count = static_cast<int>(m_ostWizard.landmarks.size());
        if (count > 0)
            setOsteotomyLandmarkIndex(((m_ostWizard.currentLandmark < 0 ? 0 : m_ostWizard.currentLandmark) - 1 + count) % count);
    });
    connect(panel, &OsteotomyWizardPanel::nextLandmarkRequested, this, [this] {
        const int count = static_cast<int>(m_ostWizard.landmarks.size());
        if (count > 0)
            setOsteotomyLandmarkIndex((m_ostWizard.currentLandmark + 1) % count);
    });
    connect(panel, &OsteotomyWizardPanel::clearLandmarksRequested, this, &MainWindow::clearOsteotomyLandmarks);
    connect(panel, &OsteotomyWizardPanel::gizmoToggled, this, &MainWindow::setOsteotomyGizmo);
    connect(panel, &OsteotomyWizardPanel::resetPathRequested, this, [this] {
        setOsteotomyGizmo(false);
        rebuildOsteotomyPlan();
        refreshOsteotomyWizard();
    });
    connect(panel, &OsteotomyWizardPanel::propertiesChanged, this, &MainWindow::onOsteotomyPropertiesChanged);
    connect(panel, &OsteotomyWizardPanel::showContourToggled, this, [this](bool show) {
        m_ostShowContour = show;
        ++m_osteotomyContourGeneration;
        refreshRegisteredArchContours();
    });
    connect(panel, &OsteotomyWizardPanel::showSlicesRequested, this, &MainWindow::showOsteotomySlicesInCt);
    connect(panel, &OsteotomyWizardPanel::backRequested, this, &MainWindow::osteotomyWizardBack);
    connect(panel, &OsteotomyWizardPanel::nextRequested, this, &MainWindow::osteotomyWizardNext);
    connect(panel, &OsteotomyWizardPanel::cancelRequested, this, &MainWindow::cancelOsteotomyWizard);

    if (m_osteotomyView) {
        connect(m_osteotomyView, &Mesh3DView::editablePointAdded, this, &MainWindow::onOsteotomyLandmarkAdded);
        connect(m_osteotomyView, &Mesh3DView::editablePointMoved, this, &MainWindow::onOsteotomyLandmarkMoved);
        connect(m_osteotomyView, &Mesh3DView::editablePointRemoved, this, &MainWindow::onOsteotomyLandmarkRemoved);
    }
    return scroll;
}

// ── State ─────────────────────────────────────────────────────────────────────

void MainWindow::startOsteotomyWizard()
{
    if (!m_osteotomyWizard)
        return;
    setOsteotomyGizmo(false);
    const bool upper = hasMesh(m_upperCompositeMesh);
    const int type = m_ostWizard.type;
    const auto landmarks = m_ostWizard.landmarks; // a reopened project's plan survives the first start
    const bool keep = std::exchange(m_ostKeepRestoredLandmarks, false);
    m_ostWizard = OsteotomyWizardState{};
    m_ostWizard.type = (!upper && type == static_cast<int>(OsteotomyType::LeFortI)) ? static_cast<int>(OsteotomyType::Bsso) : type;
    const size_t count = OsteotomyCore::Landmarks(typeOf(m_ostWizard.type)).size();
    if (keep && m_ostWizard.type == type && landmarks.size() == count)
        m_ostWizard.landmarks = landmarks;
    else
        m_ostWizard.landmarks.assign(count, std::nullopt);
    const auto firstMissing = std::find(m_ostWizard.landmarks.begin(), m_ostWizard.landmarks.end(), std::nullopt);
    m_ostWizard.currentLandmark = firstMissing == m_ostWizard.landmarks.end()
        ? -1 : static_cast<int>(firstMissing - m_ostWizard.landmarks.begin());
    m_ostGuideMeshes.clear();
    ++m_osteotomyContourGeneration;
    refreshRegisteredArchContours();
    showOsteotomyScene();
    rebuildOsteotomyPlan();
    m_osteotomyWizard->setStep(OsteotomyWizardPanel::TypeStep);
    selectOsteotomyBone(0);
    refreshOsteotomyWizard();
}

vtkSmartPointer<vtkPolyData> MainWindow::osteotomyBoneMesh(int label) const
{
    if (label == kUpperCompositeLabel)
        return m_upperCompositeMesh;
    if (label == kLowerCompositeLabel)
        return m_lowerCompositeMesh;
    if (label == kBssoDistalLabel)
        return m_bssoDistalMesh;
    return nullptr;
}

std::vector<MainWindow::OsteotomyBoneChoice> MainWindow::osteotomyBoneChoices(int type) const
{
    std::vector<OsteotomyBoneChoice> choices;
    switch (typeOf(type)) {
    case OsteotomyType::LeFortI:
        if (hasMesh(m_upperCompositeMesh))
            choices.push_back({tr("Compuesto maxilar"), kUpperCompositeLabel});
        break;
    case OsteotomyType::Bsso:
        if (hasMesh(m_lowerCompositeMesh))
            choices.push_back({tr("Compuesto mandibular"), kLowerCompositeLabel});
        break;
    case OsteotomyType::Genioplasty:
        if (hasMesh(m_bssoDistalMesh))
            choices.push_back({tr("Mandíbula distal (tras BSSO)"), kBssoDistalLabel});
        if (hasMesh(m_lowerCompositeMesh))
            choices.push_back({tr("Compuesto mandibular"), kLowerCompositeLabel});
        break;
    }
    return choices;
}

void MainWindow::refreshOsteotomyWizard()
{
    if (!m_osteotomyWizard)
        return;
    auto* panel = m_osteotomyWizard;
    const OsteotomyType type = typeOf(m_ostWizard.type);
    const std::array<bool, 3> available = {!osteotomyBoneChoices(0).empty(), !osteotomyBoneChoices(1).empty(),
                                           !osteotomyBoneChoices(2).empty()};
    const std::array<bool, 3> done = {
        hasMesh(m_leFortSegmentMesh) && hasMesh(m_leFortCranialMesh),
        hasMesh(m_bssoRightProximalMesh) && hasMesh(m_bssoLeftProximalMesh) &&
            (hasMesh(m_bssoDistalMesh) || hasMesh(m_genioBodyMesh)),
        hasMesh(m_genioSegmentMesh) && hasMesh(m_genioBodyMesh)};
    panel->setTypes({OsteotomyCore::TypeName(OsteotomyType::LeFortI), OsteotomyCore::TypeName(OsteotomyType::Bsso),
                     OsteotomyCore::TypeName(OsteotomyType::Genioplasty)},
                    available, done, m_ostWizard.type);

    std::vector<OsteotomyWizardPanel::BoneOption> bones;
    for (const OsteotomyBoneChoice& choice : osteotomyBoneChoices(m_ostWizard.type))
        bones.push_back({choice.name, choice.label});
    const auto bone = osteotomyBoneMesh(m_ostWizard.boneLabel);
    panel->setBoneOptions(bones, m_ostWizard.boneLabel,
                          CompositeBlockCore::HasParts(bone)
                              ? tr("El modelo dental vinculado se selecciona con el hueso: se corta con él y cada parte "
                                   "queda unida a su nuevo segmento.")
                              : tr("Este hueso no tiene modelo dental vinculado."));

    QStringList names, hints;
    std::vector<bool> placed;
    for (const OsteotomyLandmark& landmark : OsteotomyCore::Landmarks(type)) {
        names << landmark.name;
        hints << landmark.hint;
    }
    for (const auto& point : m_ostWizard.landmarks)
        placed.push_back(point.has_value());
    panel->setLandmarks(names, hints, placed, m_ostWizard.currentLandmark);

    panel->setPathMode(type == OsteotomyType::Bsso);
    const OsteotomyTypeProperties& p = m_ostProperties[static_cast<size_t>(m_ostWizard.type)];
    OsteotomyWizardPanel::PathProperties properties;
    properties.widthMm = p.widthMm;
    properties.thicknessMm = p.thicknessMm;
    properties.extensionRightMm = p.extensionRightMm;
    properties.extensionLeftMm = p.extensionLeftMm;
    properties.posteriorExtensionMm = p.posteriorExtensionMm;
    properties.inferiorExtensionMm = p.inferiorExtensionMm;
    properties.mediolateralExtensionMm = p.mediolateralExtensionMm;
    panel->setPathProperties(properties);
    panel->setGizmoActive(m_ostWizard.gizmoActive);
    panel->setShowContour(m_ostShowContour);

    std::vector<OsteotomyWizardPanel::ObjectRow> rows;
    for (int label : m_ostWizard.createdLabels)
        rows.push_back({meshLabelName(label), objectColorForLabel(label)});
    panel->setCreatedObjects(rows);

    const int step = panel->step();
    const bool allPlaced = std::all_of(m_ostWizard.landmarks.begin(), m_ostWizard.landmarks.end(),
                                       [](const auto& point) { return point.has_value(); });
    bool canNext = true;
    QString nextText = tr("Siguiente");
    QString status;
    switch (step) {
    case OsteotomyWizardPanel::TypeStep:
        canNext = available[static_cast<size_t>(m_ostWizard.type)];
        status = canNext ? tr("%1: pulse Siguiente.").arg(OsteotomyCore::TypeName(type))
                         : tr("Cree primero el modelo compuesto necesario para esta osteotomía.");
        break;
    case OsteotomyWizardPanel::BoneStep:
        canNext = hasMesh(bone);
        break;
    case OsteotomyWizardPanel::LandmarkStep:
        canNext = allPlaced && m_ostWizard.planReady;
        status = allPlaced && !m_ostWizard.planReady ? m_ostWizard.planError
                                                     : tr("Puntos: %1 de %2.")
                                                           .arg(std::count(placed.begin(), placed.end(), true))
                                                           .arg(placed.size());
        break;
    case OsteotomyWizardPanel::PathStep:
        canNext = m_ostWizard.planReady && !m_ostWizard.gizmoActive;
        nextText = tr("Aplicar corte");
        status = m_ostWizard.gizmoActive ? tr("Ajuste con el gizmo y pulse de nuevo el botón para aceptar.")
                 : m_ostWizard.planReady ? tr("La guía naranja muestra el corte (grosor %1 mm).").arg(QLocale().toString(p.thicknessMm, 'f', 1))
                                         : m_ostWizard.planError;
        break;
    case OsteotomyWizardPanel::FinalizeStep:
        nextText = tr("Finalizar");
        status = tr("%1 aplicada.").arg(OsteotomyCore::TypeName(type));
        break;
    default:
        break;
    }
    panel->setNavigation(step > OsteotomyWizardPanel::TypeStep && step < OsteotomyWizardPanel::FinalizeStep, canNext, nextText);
    panel->setStatus(status);
}

void MainWindow::selectOsteotomyType(int type)
{
    type = std::clamp(type, 0, 2);
    if (type != m_ostWizard.type) {
        setOsteotomyGizmo(false);
        m_ostWizard.type = type;
        m_ostWizard.landmarks.assign(OsteotomyCore::Landmarks(typeOf(type)).size(), std::nullopt);
        m_ostWizard.currentLandmark = 0;
        m_ostWizard.planReady = false;
        m_ostWizard.planError.clear();
        updateOsteotomyGuideDisplay();
        updateOsteotomyLandmarkMarkers();
    }
    selectOsteotomyBone(0);
    refreshOsteotomyWizard();
}

void MainWindow::selectOsteotomyBone(int label)
{
    const auto choices = osteotomyBoneChoices(m_ostWizard.type);
    const auto found = std::find_if(choices.begin(), choices.end(),
                                    [label](const OsteotomyBoneChoice& choice) { return choice.label == label; });
    m_ostWizard.boneLabel = found != choices.end() ? label : (choices.empty() ? 0 : choices.front().label);
    refreshOsteotomyWizard();
}

// ── Navigation ────────────────────────────────────────────────────────────────

void MainWindow::setOsteotomyWizardStep(int step)
{
    if (!m_osteotomyWizard)
        return;
    m_osteotomyWizard->setStep(step);
    if (m_osteotomyView) {
        m_osteotomyView->setPointPickMode(false);
        const bool landmarks = step == OsteotomyWizardPanel::LandmarkStep;
        m_osteotomyView->setPointEditMode(landmarks, landmarks ? m_ostWizard.currentLandmark : -1);
        if (landmarks)
            m_osteotomyView->setStandardView(0);
    }
    updateOsteotomyLandmarkMarkers();
    refreshOsteotomyWizard();
}

void MainWindow::osteotomyWizardNext()
{
    if (!m_osteotomyWizard)
        return;
    switch (m_osteotomyWizard->step()) {
    case OsteotomyWizardPanel::TypeStep:
        if (osteotomyBoneChoices(m_ostWizard.type).empty())
            return;
        setOsteotomyWizardStep(OsteotomyWizardPanel::BoneStep);
        break;
    case OsteotomyWizardPanel::BoneStep:
        if (!hasMesh(osteotomyBoneMesh(m_ostWizard.boneLabel)))
            return;
        setOsteotomyWizardStep(OsteotomyWizardPanel::LandmarkStep);
        statusBar()->showMessage(tr("%1: marque los puntos en la vista frontal.").arg(OsteotomyCore::TypeName(typeOf(m_ostWizard.type))));
        break;
    case OsteotomyWizardPanel::LandmarkStep:
        if (!m_ostWizard.planReady)
            return;
        setOsteotomyWizardStep(OsteotomyWizardPanel::PathStep);
        break;
    case OsteotomyWizardPanel::PathStep:
        setOsteotomyGizmo(false);
        if (!applyOsteotomyCut())
            return;
        setOsteotomyWizardStep(OsteotomyWizardPanel::FinalizeStep);
        break;
    case OsteotomyWizardPanel::FinalizeStep:
        finishOsteotomyWizard();
        break;
    default:
        break;
    }
}

void MainWindow::osteotomyWizardBack()
{
    if (!m_osteotomyWizard)
        return;
    const int step = m_osteotomyWizard->step();
    if (step <= OsteotomyWizardPanel::TypeStep || step >= OsteotomyWizardPanel::FinalizeStep)
        return;
    setOsteotomyGizmo(false);
    setOsteotomyWizardStep(step - 1);
}

void MainWindow::cancelOsteotomyWizard()
{
    if (m_osteotomyView)
        m_osteotomyView->setPointEditMode(false);
    startOsteotomyWizard();
    statusBar()->showMessage(tr("Planificación de osteotomía cancelada."));
}

void MainWindow::finishOsteotomyWizard()
{
    const int action = m_osteotomyWizard ? m_osteotomyWizard->nextAction() : OsteotomyWizardPanel::AnotherOsteotomy;
    if (action == OsteotomyWizardPanel::OcclusionRegistration) {
        setBiteRegistrationWorkspace(true);
    } else if (action == OsteotomyWizardPanel::Reposition) {
        setRepositionWorkspace(true);
    } else {
        startOsteotomyWizard();
        statusBar()->showMessage(tr("Seleccione la siguiente osteotomía."));
    }
}

// ── Landmarks ─────────────────────────────────────────────────────────────────

void MainWindow::setOsteotomyLandmarkIndex(int index)
{
    const int count = static_cast<int>(m_ostWizard.landmarks.size());
    if (index < 0 || index >= count)
        return;
    m_ostWizard.currentLandmark = index;
    if (m_osteotomyWizard && m_osteotomyWizard->step() == OsteotomyWizardPanel::LandmarkStep && m_osteotomyView)
        m_osteotomyView->setPointEditMode(true, index);
    const auto landmarks = OsteotomyCore::Landmarks(typeOf(m_ostWizard.type));
    statusBar()->showMessage(tr("Indique: %1").arg(landmarks[static_cast<size_t>(index)].name));
    refreshOsteotomyWizard();
}

void MainWindow::onOsteotomyLandmarkAdded(int group, double x, double y, double z)
{
    if (!m_osteotomyWizard || m_osteotomyWizard->step() != OsteotomyWizardPanel::LandmarkStep)
        return;
    const int count = static_cast<int>(m_ostWizard.landmarks.size());
    if (group < 0 || group >= count || group != m_ostWizard.currentLandmark)
        return;
    m_ostWizard.landmarks[static_cast<size_t>(group)] = OstPoint3{x, y, z};
    int next = -1;
    for (int step = 1; step <= count; ++step) {
        const int candidate = (group + step) % count;
        if (!m_ostWizard.landmarks[static_cast<size_t>(candidate)]) {
            next = candidate;
            break;
        }
    }
    m_ostWizard.currentLandmark = next;
    if (m_osteotomyView)
        m_osteotomyView->setPointEditMode(true, next);
    rebuildOsteotomyPlan();
    updateOsteotomyLandmarkMarkers();
    refreshOsteotomyWizard();
}

void MainWindow::onOsteotomyLandmarkMoved(int group, int, double x, double y, double z)
{
    if (!m_osteotomyWizard || m_osteotomyWizard->step() != OsteotomyWizardPanel::LandmarkStep)
        return;
    if (group < 0 || group >= static_cast<int>(m_ostWizard.landmarks.size()) || !m_ostWizard.landmarks[static_cast<size_t>(group)])
        return;
    m_ostWizard.landmarks[static_cast<size_t>(group)] = OstPoint3{x, y, z};
    rebuildOsteotomyPlan();
    refreshOsteotomyWizard();
}

void MainWindow::onOsteotomyLandmarkRemoved(int group, int)
{
    if (!m_osteotomyWizard || m_osteotomyWizard->step() != OsteotomyWizardPanel::LandmarkStep)
        return;
    if (group < 0 || group >= static_cast<int>(m_ostWizard.landmarks.size()))
        return;
    m_ostWizard.landmarks[static_cast<size_t>(group)].reset();
    m_ostWizard.currentLandmark = group;
    if (m_osteotomyView)
        m_osteotomyView->setPointEditMode(true, group);
    rebuildOsteotomyPlan();
    updateOsteotomyLandmarkMarkers();
    refreshOsteotomyWizard();
}

void MainWindow::clearOsteotomyLandmarks()
{
    std::fill(m_ostWizard.landmarks.begin(), m_ostWizard.landmarks.end(), std::nullopt);
    setOsteotomyLandmarkIndex(0);
    rebuildOsteotomyPlan();
    updateOsteotomyLandmarkMarkers();
    refreshOsteotomyWizard();
}

void MainWindow::updateOsteotomyLandmarkMarkers()
{
    if (!m_osteotomyView)
        return;
    const int step = m_osteotomyWizard ? m_osteotomyWizard->step() : -1;
    const bool visible = step == OsteotomyWizardPanel::LandmarkStep || step == OsteotomyWizardPanel::PathStep;
    for (int i = 0; i < static_cast<int>(kLandmarkColors.size()); ++i) {
        std::vector<std::array<double, 3>> points;
        if (visible && i < static_cast<int>(m_ostWizard.landmarks.size()) && m_ostWizard.landmarks[static_cast<size_t>(i)])
            points.push_back(*m_ostWizard.landmarks[static_cast<size_t>(i)]);
        m_osteotomyView->setEditablePoints(i, points, kLandmarkColors[static_cast<size_t>(i)], kLandmarkRadiusMm);
    }
}

// ── Cutting path ──────────────────────────────────────────────────────────────

void MainWindow::rebuildOsteotomyPlan()
{
    m_ostWizard.planReady = false;
    m_ostWizard.planError.clear();
    const bool allPlaced = !m_ostWizard.landmarks.empty() &&
                           std::all_of(m_ostWizard.landmarks.begin(), m_ostWizard.landmarks.end(),
                                       [](const auto& point) { return point.has_value(); });
    if (allPlaced) {
        const auto& l = m_ostWizard.landmarks;
        const OsteotomyTypeProperties& p = m_ostProperties[static_cast<size_t>(m_ostWizard.type)];
        switch (typeOf(m_ostWizard.type)) {
        case OsteotomyType::LeFortI:
            m_ostWizard.path = OsteotomyCore::LeFortPath({*l[0], *l[1], *l[2], *l[3]}, p.widthMm, p.thicknessMm,
                                                         p.extensionRightMm, p.extensionLeftMm);
            m_ostWizard.planReady = m_ostWizard.path.valid;
            m_ostWizard.planError = m_ostWizard.path.error;
            break;
        case OsteotomyType::Genioplasty:
            m_ostWizard.path = OsteotomyCore::GenioPath({*l[0], *l[1], *l[2], *l[3]}, p.widthMm, p.thicknessMm,
                                                        p.extensionRightMm, p.extensionLeftMm);
            m_ostWizard.planReady = m_ostWizard.path.valid;
            m_ostWizard.planError = m_ostWizard.path.error;
            break;
        case OsteotomyType::Bsso: {
            BssoPlan plan;
            plan.right = {*l[0], *l[1], *l[2]};
            plan.left = {*l[3], *l[4], *l[5]};
            plan.thicknessMm = p.thicknessMm;
            plan.posteriorExtensionMm = p.posteriorExtensionMm;
            plan.inferiorExtensionMm = p.inferiorExtensionMm;
            plan.mediolateralExtensionMm = p.mediolateralExtensionMm;
            m_ostWizard.bsso = plan;
            const BssoSidePlanes right = OsteotomyCore::BssoPlanes(plan, false);
            const BssoSidePlanes left = OsteotomyCore::BssoPlanes(plan, true);
            m_ostWizard.planReady = right.valid && left.valid;
            m_ostWizard.planError = !right.valid ? right.error : left.error;
            break;
        }
        }
    }
    updateOsteotomyGuideDisplay();
}

void MainWindow::onOsteotomyPropertiesChanged()
{
    if (!m_osteotomyWizard)
        return;
    const auto values = m_osteotomyWizard->pathProperties();
    OsteotomyTypeProperties& p = m_ostProperties[static_cast<size_t>(m_ostWizard.type)];
    p.widthMm = values.widthMm;
    p.thicknessMm = values.thicknessMm;
    p.extensionRightMm = values.extensionRightMm;
    p.extensionLeftMm = values.extensionLeftMm;
    p.posteriorExtensionMm = values.posteriorExtensionMm;
    p.inferiorExtensionMm = values.inferiorExtensionMm;
    p.mediolateralExtensionMm = values.mediolateralExtensionMm;
    if (m_ostWizard.planReady) {
        // Keep a path moved with the gizmo; only its properties change.
        m_ostWizard.path.widthMm = p.widthMm;
        m_ostWizard.path.thicknessMm = p.thicknessMm;
        m_ostWizard.path.extensionStartMm = p.extensionRightMm;
        m_ostWizard.path.extensionEndMm = p.extensionLeftMm;
        m_ostWizard.bsso.thicknessMm = p.thicknessMm;
        m_ostWizard.bsso.posteriorExtensionMm = p.posteriorExtensionMm;
        m_ostWizard.bsso.inferiorExtensionMm = p.inferiorExtensionMm;
        m_ostWizard.bsso.mediolateralExtensionMm = p.mediolateralExtensionMm;
        updateOsteotomyGuideDisplay();
    }
    refreshOsteotomyWizard();
}

void MainWindow::setOsteotomyGizmo(bool active)
{
    if (!m_osteotomyView)
        return;
    if (active) {
        if (!m_ostWizard.planReady || !m_osteotomyWizard || m_osteotomyWizard->step() != OsteotomyWizardPanel::PathStep) {
            refreshOsteotomyWizard();
            return;
        }
        const bool left = typeOf(m_ostWizard.type) == OsteotomyType::Bsso && m_osteotomyWizard->gizmoSide() == 1;
        m_ostWizard.gizmoSide = left ? 1 : 0;
        const int key = left ? kOsteotomyGuideLeftActorKey : kOsteotomyGuideActorKey;
        if (!m_osteotomyView->meshData(key)) {
            refreshOsteotomyWizard();
            return;
        }
        m_ostWizard.gizmoActive = true;
        m_osteotomyView->startGizmo(key);
        statusBar()->showMessage(tr("Gizmo: flechas trasladan, anillos rotan, cubos redimensionan la trayectoria."));
    } else if (m_ostWizard.gizmoActive) {
        m_ostWizard.gizmoActive = false;
        if (m_osteotomyView->hasGizmo())
            m_osteotomyView->stopGizmo(); // → onOsteotomyGizmoUpdated → applyOsteotomyGizmo
    }
    refreshOsteotomyWizard();
}

void MainWindow::applyOsteotomyGizmo(vtkSmartPointer<vtkMatrix4x4> matrix)
{
    if (!matrix || !m_ostWizard.planReady)
        return;
    if (typeOf(m_ostWizard.type) == OsteotomyType::Bsso) {
        m_ostWizard.bsso = OsteotomyCore::TransformBsso(m_ostWizard.bsso, matrix, m_ostWizard.gizmoSide == 0,
                                                        m_ostWizard.gizmoSide == 1);
        const BssoSidePlanes right = OsteotomyCore::BssoPlanes(m_ostWizard.bsso, false);
        const BssoSidePlanes left = OsteotomyCore::BssoPlanes(m_ostWizard.bsso, true);
        m_ostWizard.planReady = right.valid && left.valid;
        m_ostWizard.planError = !right.valid ? right.error : left.error;
    } else {
        m_ostWizard.path = OsteotomyCore::TransformPath(m_ostWizard.path, matrix);
        m_ostWizard.planReady = m_ostWizard.path.valid;
        m_ostWizard.planError = m_ostWizard.path.error;
        m_ostProperties[static_cast<size_t>(m_ostWizard.type)].widthMm = m_ostWizard.path.widthMm;
    }
    updateOsteotomyGuideDisplay();
    refreshOsteotomyWizard();
}

void MainWindow::updateOsteotomyGuideDisplay()
{
    m_ostGuideMeshes.clear();
    if (m_ostWizard.planReady) {
        if (typeOf(m_ostWizard.type) == OsteotomyType::Bsso) {
            for (bool left : {false, true})
                if (auto guide = OsteotomyCore::BssoGuideMesh(m_ostWizard.bsso, left))
                    m_ostGuideMeshes.push_back(guide);
        } else if (auto guide = OsteotomyCore::PathGuideMesh(m_ostWizard.path)) {
            m_ostGuideMeshes.push_back(guide);
        }
    }
    if (m_osteotomyView) {
        const int keys[2] = {kOsteotomyGuideActorKey, kOsteotomyGuideLeftActorKey};
        for (int i = 0; i < 2; ++i) {
            if (i >= static_cast<int>(m_ostGuideMeshes.size())) {
                m_osteotomyView->removeMesh(keys[i]);
                continue;
            }
            m_osteotomyView->addMesh(keys[i], m_ostGuideMeshes[static_cast<size_t>(i)], tr("Trayectoria de corte"));
            m_osteotomyView->setMeshColor(keys[i], kGuideColor);
            m_osteotomyView->setMeshOpacity(keys[i], 0.6);
            m_osteotomyView->setMeshPickable(keys[i], false);
        }
        m_osteotomyView->render();
    }
    ++m_osteotomyContourGeneration;
    refreshRegisteredArchContours();
}

std::vector<vtkSmartPointer<vtkPolyData>> MainWindow::osteotomyContourMeshes() const
{
    return m_ostShowContour ? m_ostGuideMeshes : std::vector<vtkSmartPointer<vtkPolyData>>{};
}

void MainWindow::showOsteotomySlicesInCt()
{
    refreshRegisteredArchContours();
    for (auto* tab : findChildren<QToolButton*>(QStringLiteral("MT"))) {
        if (tab && tab->text() == tr("SEGMENTACION")) {
            tab->click();
            break;
        }
    }
    statusBar()->showMessage(tr("Contorno naranja: trayectoria de corte sobre los cortes del TAC. Vuelva a OSTEOTOMIA para continuar."));
}

// ── Apply ─────────────────────────────────────────────────────────────────────

void MainWindow::showOsteotomyScene()
{
    if (!m_osteotomyView)
        return;
    m_osteotomyView->clearMeshes(true);
    const auto add = [this](int label, vtkPolyData* mesh) {
        if (!mesh || mesh->GetNumberOfCells() == 0)
            return;
        const int key = objectActorKey(label);
        m_osteotomyView->addMesh(key, mesh, meshLabelName(label));
        m_osteotomyView->setMeshColor(key, objectColorForLabel(label));
        m_osteotomyView->setMeshOpacity(key, 1.0);
        m_osteotomyView->setMeshVisible(key, objectEntryVisible(label));
    };
    if (hasMesh(m_leFortCranialMesh) && hasMesh(m_leFortSegmentMesh)) {
        add(kLeFortCranialLabel, m_leFortCranialMesh);
        add(kLeFortSegLabel, m_leFortSegmentMesh);
    } else {
        add(kUpperCompositeLabel, m_upperCompositeMesh);
    }
    const bool bsso = hasMesh(m_bssoRightProximalMesh) || hasMesh(m_bssoLeftProximalMesh);
    if (hasMesh(m_genioBodyMesh) && hasMesh(m_genioSegmentMesh)) {
        add(kGenioBodyLabel, m_genioBodyMesh);
        add(kGenioSegmentLabel, m_genioSegmentMesh);
    } else if (hasMesh(m_bssoDistalMesh)) {
        add(kBssoDistalLabel, m_bssoDistalMesh);
    } else if (!bsso) {
        add(kLowerCompositeLabel, m_lowerCompositeMesh);
    }
    if (bsso) {
        add(kBssoProximalRightLabel, m_bssoRightProximalMesh);
        add(kBssoProximalLeftLabel, m_bssoLeftProximalMesh);
    }
    updateOsteotomyGuideDisplay();
    m_osteotomyView->render();
}

// ── Project ───────────────────────────────────────────────────────────────────

QJsonObject MainWindow::osteotomyPlanJson() const
{
    QJsonObject plan;
    plan[QStringLiteral("type")] = m_ostWizard.type;
    QJsonArray landmarks;
    for (const auto& point : m_ostWizard.landmarks)
        landmarks.append(point ? QJsonValue(QJsonArray{(*point)[0], (*point)[1], (*point)[2]}) : QJsonValue());
    plan[QStringLiteral("landmarks")] = landmarks;
    QJsonArray properties;
    for (const OsteotomyTypeProperties& p : m_ostProperties) {
        QJsonObject o;
        o[QStringLiteral("widthMm")] = p.widthMm;
        o[QStringLiteral("thicknessMm")] = p.thicknessMm;
        o[QStringLiteral("extensionRightMm")] = p.extensionRightMm;
        o[QStringLiteral("extensionLeftMm")] = p.extensionLeftMm;
        o[QStringLiteral("posteriorExtensionMm")] = p.posteriorExtensionMm;
        o[QStringLiteral("inferiorExtensionMm")] = p.inferiorExtensionMm;
        o[QStringLiteral("mediolateralExtensionMm")] = p.mediolateralExtensionMm;
        properties.append(o);
    }
    plan[QStringLiteral("properties")] = properties;
    QJsonObject references;
    for (const auto& [label, reference] : m_segmentReferences) {
        QJsonArray ids, points, marks;
        for (vtkIdType id : reference.ids)
            ids.append(static_cast<qint64>(id));
        for (const OstPoint3& p : reference.points)
            for (double v : p) points.append(v);
        for (const OstPoint3& p : reference.landmarks)
            for (double v : p) marks.append(v);
        references[QString::number(label)] = QJsonObject{{QStringLiteral("ids"), ids},
                                                         {QStringLiteral("points"), points},
                                                         {QStringLiteral("landmarks"), marks}};
    }
    if (!references.isEmpty())
        plan[QStringLiteral("segmentReferences")] = references;
    return plan;
}

void MainWindow::restoreOsteotomyPlan(const ProjectState& state)
{
    const QJsonObject plan = state.osteotomyPlan;
    if (plan.isEmpty())
        return; // older project: keep the ProPlan defaults
    const QJsonArray properties = plan.value(QStringLiteral("properties")).toArray();
    for (int i = 0; i < 3 && i < properties.size(); ++i) {
        const QJsonObject o = properties[i].toObject();
        OsteotomyTypeProperties& p = m_ostProperties[static_cast<size_t>(i)];
        p.widthMm = o.value(QStringLiteral("widthMm")).toDouble(p.widthMm);
        p.thicknessMm = o.value(QStringLiteral("thicknessMm")).toDouble(p.thicknessMm);
        p.extensionRightMm = o.value(QStringLiteral("extensionRightMm")).toDouble(p.extensionRightMm);
        p.extensionLeftMm = o.value(QStringLiteral("extensionLeftMm")).toDouble(p.extensionLeftMm);
        p.posteriorExtensionMm = o.value(QStringLiteral("posteriorExtensionMm")).toDouble(p.posteriorExtensionMm);
        p.inferiorExtensionMm = o.value(QStringLiteral("inferiorExtensionMm")).toDouble(p.inferiorExtensionMm);
        p.mediolateralExtensionMm = o.value(QStringLiteral("mediolateralExtensionMm")).toDouble(p.mediolateralExtensionMm);
    }
    const auto triples = [](const QJsonArray& values) {
        std::vector<OstPoint3> out;
        for (qsizetype i = 0; i + 2 < values.size(); i += 3)
            out.push_back({values[i].toDouble(), values[i + 1].toDouble(), values[i + 2].toDouble()});
        return out;
    };
    m_segmentReferences.clear();
    const QJsonObject references = plan.value(QStringLiteral("segmentReferences")).toObject();
    for (auto it = references.begin(); it != references.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        SegmentReference reference;
        for (const QJsonValue& id : o.value(QStringLiteral("ids")).toArray())
            reference.ids.push_back(static_cast<vtkIdType>(id.toInteger()));
        reference.points = triples(o.value(QStringLiteral("points")).toArray());
        reference.landmarks = triples(o.value(QStringLiteral("landmarks")).toArray());
        if (reference.ids.size() == reference.points.size())
            m_segmentReferences[it.key().toInt()] = std::move(reference);
    }
    m_ostWizard = OsteotomyWizardState{};
    m_ostWizard.type = std::clamp(plan.value(QStringLiteral("type")).toInt(0), 0, 2);
    m_ostWizard.landmarks.assign(OsteotomyCore::Landmarks(typeOf(m_ostWizard.type)).size(), std::nullopt);
    const QJsonArray landmarks = plan.value(QStringLiteral("landmarks")).toArray();
    for (int i = 0; i < landmarks.size() && i < static_cast<int>(m_ostWizard.landmarks.size()); ++i) {
        const QJsonArray p = landmarks[i].toArray();
        if (p.size() == 3)
            m_ostWizard.landmarks[static_cast<size_t>(i)] = OstPoint3{p[0].toDouble(), p[1].toDouble(), p[2].toDouble()};
    }
    m_ostKeepRestoredLandmarks = true;
}

bool MainWindow::applyOsteotomyCut()
{
    const OsteotomyType type = typeOf(m_ostWizard.type);
    const int boneLabel = m_ostWizard.boneLabel;
    const auto bone = osteotomyBoneMesh(boneLabel);
    if (!hasMesh(bone) || !m_ostWizard.planReady) {
        QMessageBox::warning(this, tr("Osteotomía"), tr("Falta el hueso o la trayectoria de corte."));
        return false;
    }
    statusBar()->showMessage(tr("Aplicando %1…").arg(OsteotomyCore::TypeName(type)));

    m_ostWizard.createdLabels.clear();
    const auto commit = [this](int label, const vtkSmartPointer<vtkPolyData>& mesh) {
        if (m_mesh3DView) {
            m_mesh3DView->addMesh(objectActorKey(label), mesh, meshLabelName(label));
            m_mesh3DView->setMeshColor(objectActorKey(label), meshLabelColor(label));
        }
        removeObjectEntry(label);
        addObjectEntry(meshLabelName(label), meshLabelColor(label), label);
        m_ostWizard.createdLabels.push_back(label);
    };
    const auto discard = [this](int label) {
        removeObjectEntry(label);
        for (Mesh3DView* view : {m_mesh3DView, m_osteotomyView})
            if (view) view->removeMesh(objectActorKey(label));
    };

    if (type == OsteotomyType::LeFortI) {
        const OsteotomySplitResult split = OsteotomyCore::SplitByPath(bone, m_ostWizard.path);
        if (!split.ok) {
            QMessageBox::warning(this, tr("Le Fort I"), split.error);
            return false;
        }
        m_leFortCranialMesh = split.positive;
        m_leFortSegmentMesh = split.negative;
        commit(kLeFortCranialLabel, m_leFortCranialMesh);
        commit(kLeFortSegLabel, m_leFortSegmentMesh);
        m_segmentReferences[kLeFortSegLabel] = OsteotomyCore::CaptureSegmentReference(m_leFortSegmentMesh, m_ostWizard.path.points);
        if (m_leFortExportAct)
            m_leFortExportAct->setEnabled(true);
    } else if (type == OsteotomyType::Bsso) {
        const BssoSplitResult split = OsteotomyCore::SplitBsso(bone, m_ostWizard.bsso);
        if (!split.ok) {
            QMessageBox::warning(this, tr("BSSO"), split.error);
            return false;
        }
        m_bssoRightProximalMesh = split.proximalRight;
        m_bssoLeftProximalMesh = split.proximalLeft;
        m_bssoDistalMesh = split.distal;
        auto append = vtkSmartPointer<vtkAppendPolyData>::New();
        append->AddInputData(split.proximalRight);
        append->AddInputData(split.proximalLeft);
        append->Update();
        m_bssoProximalMesh = vtkSmartPointer<vtkPolyData>::New();
        m_bssoProximalMesh->DeepCopy(append->GetOutput());
        m_bssoSideSplitDone = {true, true};
        m_mandibleMovement = {};
        m_mandibleMovementResetMatrix.clear();
        // A new sagittal split supersedes an earlier chin osteotomy on the old mandible.
        m_genioBodyMesh = nullptr;
        m_genioSegmentMesh = nullptr;
        discard(kGenioBodyLabel);
        discard(kGenioSegmentLabel);
        m_segmentReferences.erase(kGenioSegmentLabel);
        discard(kBssoProximalLabel);
        commit(kBssoDistalLabel, m_bssoDistalMesh);
        commit(kBssoProximalRightLabel, m_bssoRightProximalMesh);
        commit(kBssoProximalLeftLabel, m_bssoLeftProximalMesh);
    } else {
        const OsteotomySplitResult split = OsteotomyCore::SplitByPath(bone, m_ostWizard.path);
        if (!split.ok) {
            QMessageBox::warning(this, tr("Genioplastia"), split.error);
            return false;
        }
        m_genioSegmentMesh = split.negative;
        m_genioBodyMesh = split.positive;
        m_mandibleMovement = {};
        m_mandibleMovementResetMatrix.clear();
        if (boneLabel == kBssoDistalLabel) {
            m_bssoDistalMesh = nullptr; // superseded by the post-genioplasty body
            discard(kBssoDistalLabel);
        }
        commit(kGenioBodyLabel, m_genioBodyMesh);
        commit(kGenioSegmentLabel, m_genioSegmentMesh);
        m_segmentReferences[kGenioSegmentLabel] = OsteotomyCore::CaptureSegmentReference(m_genioSegmentMesh, m_ostWizard.path.points);
    }

    m_ostWizard.planReady = false;
    std::fill(m_ostWizard.landmarks.begin(), m_ostWizard.landmarks.end(), std::nullopt);
    showOsteotomyScene();
    syncVisibilityPanelToAllViews();
    updateButtonStates();
    LoggerCore::instance().logCustom(QStringLiteral("OSTEOTOMY"),
                                     QStringLiteral("%1 applied on label %2").arg(OsteotomyCore::TypeName(type)).arg(boneLabel));
    statusBar()->showMessage(tr("%1 aplicada: %2 objetos nuevos.").arg(OsteotomyCore::TypeName(type)).arg(m_ostWizard.createdLabels.size()));
    return true;
}
