// ─────────────────────────────────────────────────────────────────────────────
// MainWindow — REPOSICIÓN analysis (ProPlan 4.6.1): intersection volume,
// highlighted intersections, motion restriction and planned/pre-op display.
// Geometry lives in CollisionCore; this file only wires the UI.
// ─────────────────────────────────────────────────────────────────────────────

#include "MainWindow.h"

#include "CollisionCore.h"
#include "Mesh3DView.h"
#include "ObjectLabels.h"

#include <QApplication>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QStatusBar>

#include <vtkAppendPolyData.h>
#include <vtkPolyData.h>

#include <algorithm>
#include <cmath>

// Defined in MainWindow.cpp.
QString meshLabelName(int label);

namespace
{
bool hasPoints(const vtkSmartPointer<vtkPolyData>& mesh)
{
    return mesh && mesh->GetNumberOfPoints() > 0;
}
} // namespace

void MainWindow::setRepositionRestriction(int mode)
{
    m_repositionRestriction = std::clamp(mode, 0, 2);
    static const QStringList names = {tr("sin restricción"), tr("solo traslación"), tr("solo rotación")};
    statusBar()->showMessage(tr("Reposición: %1.").arg(names[m_repositionRestriction]));
}

QList<int> MainWindow::repositionCollisionPartners(const QList<int>& moved) const
{
    // A composite overlaps its own osteotomy segments: skip it once they exist.
    const bool maxillaCut = hasPoints(m_leFortSegmentMesh) || hasPoints(m_leFortCranialMesh);
    const bool mandibleCut = hasPoints(m_bssoDistalMesh) || hasPoints(m_genioBodyMesh) ||
                             hasPoints(m_bssoRightProximalMesh) || hasPoints(m_bssoLeftProximalMesh);
    QList<int> partners;
    for (int label : repositionStructureLabels()) {
        if (moved.contains(label) || label == kBiteScanLabel)
            continue;
        if ((label == kUpperCompositeLabel && maxillaCut) || (label == kLowerCompositeLabel && mandibleCut))
            continue;
        if (!hasPoints(repositionMeshForLabel(label)) || !objectEntryVisible(label))
            continue;
        partners << label;
    }
    return partners;
}

void MainWindow::analyzeRepositionIntersection(bool highlight)
{
    const QList<int> moved = selectedRepositionTargetLabels();
    if (moved.isEmpty()) {
        statusBar()->showMessage(tr("Intersección: marque las estructuras que movió."));
        return;
    }
    const QList<int> partners = repositionCollisionPartners(moved);
    clearRepositionAnalysis();
    if (partners.isEmpty()) {
        m_repositionLastIntersectionMm3 = 0.0;
        if (m_repositionIntersectionLabel)
            m_repositionIntersectionLabel->setText(tr("Sin otras estructuras visibles para comparar."));
        return;
    }
    auto others = vtkSmartPointer<vtkAppendPolyData>::New();
    for (int label : partners)
        others->AddInputData(repositionMeshForLabel(label));
    others->Update();

    QApplication::setOverrideCursor(Qt::WaitCursor);
    double total = 0.0;
    QStringList parts;
    const QLocale locale;
    for (int label : moved) {
        const auto mesh = repositionMeshForLabel(label);
        if (!hasPoints(mesh))
            continue;
        const IntersectionResult result = CollisionCore::Intersection(mesh, others->GetOutput(), 0.5);
        if (!result.ok)
            continue;
        total += result.volumeMm3;
        parts << tr("%1: %2 mm³").arg(meshLabelName(label), locale.toString(result.volumeMm3, 'f', 1));
        if (highlight && m_repositionView && result.highlight) {
            const int key = kRepositionHighlightBaseKey - label;
            m_repositionView->addMesh(key, result.highlight, tr("%1 (intersección)").arg(meshLabelName(label)));
            m_repositionView->setMeshScalarColoring(key, true);
            m_repositionView->setMeshPickable(key, false);
            m_repositionView->setMeshVisible(objectActorKey(label), false);
        }
    }
    QApplication::restoreOverrideCursor();

    m_repositionLastIntersectionMm3 = total;
    m_repositionHighlightActive = highlight;
    if (m_repositionHighlightButton)
        m_repositionHighlightButton->setText(highlight ? tr("Quitar resaltado") : tr("Resaltar"));
    const QString text = total > 0.0
        ? tr("Intersección total %1 mm³ (%2)").arg(locale.toString(total, 'f', 1), parts.join(QStringLiteral(" · ")))
        : tr("Sin intersección con las estructuras visibles.");
    if (m_repositionIntersectionLabel)
        m_repositionIntersectionLabel->setText(text);
    statusBar()->showMessage(total > 0.0 ? tr("Colisión detectada: %1").arg(text) : text);
    if (m_repositionView)
        m_repositionView->render();
}

void MainWindow::toggleRepositionHighlight()
{
    if (m_repositionHighlightActive) {
        clearRepositionAnalysis();
        syncRepositionSelectionVisibility();
        if (m_repositionView)
            m_repositionView->render();
        return;
    }
    analyzeRepositionIntersection(true);
}

void MainWindow::clearRepositionAnalysis()
{
    if (m_repositionView) {
        for (int label : repositionStructureLabels())
            m_repositionView->removeMesh(kRepositionHighlightBaseKey - label);
    }
    if (m_repositionHighlightActive) {
        m_repositionHighlightActive = false;
        syncRepositionSelectionVisibility();
    }
    if (m_repositionHighlightButton)
        m_repositionHighlightButton->setText(tr("Resaltar"));
    if (m_repositionIntersectionLabel)
        m_repositionIntersectionLabel->clear();
}

void MainWindow::updateRepositionMeasurements()
{
    if (!m_repositionMeasureLabel)
        return;
    const QLocale locale;
    const auto signedText = [&locale](double value) {
        const double rounded = std::abs(value) < 0.05 ? 0.0 : value;
        return (rounded > 0.0 ? QStringLiteral("+") : QString()) + locale.toString(rounded, 'f', 1);
    };
    QStringList lines;
    for (const auto& [label, reference] : m_segmentReferences) {
        const auto mesh = repositionMeshForLabel(label);
        if (!hasPoints(mesh) || reference.landmarks.empty())
            continue;
        lines << tr("%1 desde el corte:").arg(meshLabelName(label));
        const LandmarkMovement movement = OsteotomyCore::MeasureLandmarkMovement(reference, mesh);
        if (!movement.valid) {
            lines << tr("  no disponible (la malla ya no coincide con la del corte)");
            continue;
        }
        for (size_t i = 0; i < movement.displacements.size(); ++i) {
            const OstPoint3& d = movement.displacements[i];
            QString vertical = tr("Z %1").arg(signedText(d[2]));
            if (label == kLeFortSegLabel && std::abs(d[2]) >= 0.05)
                vertical += d[2] > 0.0 ? tr(" impactación") : tr(" descenso");
            lines << tr("  P%1: X %2 · Y %3 · %4 mm").arg(i + 1).arg(signedText(d[0]), signedText(d[1]), vertical);
        }
    }
    m_repositionMeasureLabel->setText(lines.join(QLatin1Char('\n')));
    m_repositionMeasureLabel->setVisible(!lines.isEmpty());
}

void MainWindow::toggleRepositionPreOp()
{
    if (!m_repositionView)
        return;
    m_repositionPreOpVisible = !m_repositionPreOpVisible;
    int shown = 0;
    for (int label : repositionStructureLabels()) {
        const int key = kRepositionPreOpBaseKey - label;
        const auto it = m_repositionOriginalMeshes.find(label);
        if (m_repositionPreOpVisible && it != m_repositionOriginalMeshes.end() && hasPoints(it->second)) {
            m_repositionView->addMesh(key, it->second, tr("%1 (pre-op)").arg(meshLabelName(label)));
            m_repositionView->setMeshColor(key, QColor(150, 155, 170));
            m_repositionView->setMeshOpacity(key, 0.35);
            m_repositionView->setMeshPickable(key, false);
            ++shown;
        } else {
            m_repositionView->removeMesh(key);
        }
    }
    if (m_repositionPreOpButton)
        m_repositionPreOpButton->setText(m_repositionPreOpVisible ? tr("Ocultar pre-op") : tr("Ver pre-op"));
    statusBar()->showMessage(!m_repositionPreOpVisible ? tr("Vista planificada.")
                             : shown > 0 ? tr("Pre-op: posición original en gris translúcido.")
                                         : tr("Pre-op: todavía no hay estructuras movidas."));
    m_repositionView->render();
}
