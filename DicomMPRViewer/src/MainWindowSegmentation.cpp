#include "MainWindow.h"
#include "MaskToObjectCore.h"
#include "Mesh3DView.h"
#include "ObjectLabels.h"
#include <QMessageBox>
#include <QStatusBar>
#include <QTableWidget>
#include <vtkPolyData.h>

QString meshLabelName(int label);

void MainWindow::calculateObjectFromMask(int label, bool smooth)
{
    QString error;
    auto mesh = MaskToObjectCore::Convert(m_segmentationLabelmap, label, &error,
                                          smooth ? MaskToObjectCore::Surface::Smooth : MaskToObjectCore::Surface::Exact);
    if (!mesh) {
        QMessageBox::warning(this, tr("Convertir máscara a objeto"), error);
        return;
    }
    QString name = meshLabelName(label);
    QColor color = maskColorForLabel(label);
    if (m_maskTable) {
        for (int row = 0; row < m_maskTable->rowCount(); ++row) {
            const auto* item = m_maskTable->item(row, 0);
            if (item && item->data(Qt::UserRole).toInt() == label) {
                if (const auto* text = m_maskTable->item(row, 1)) name = text->text();
                break;
            }
        }
    }
    const int key = objectActorKey(label);
    addObjectEntry(name, color, label);
    auto* dedicated = label == 5 ? m_modelMaxillaView : label == 6 ? m_modelMandibleView : nullptr;
    for (auto* view : {m_mesh3DView, m_modelMaxillaView, m_modelUpperArchView,
                       m_modelMandibleView, m_modelLowerArchView, m_modelMatchView,
                       m_orientationView, m_osteotomyView, m_biteSegmentView, m_biteScanView,
                       m_biteRegistrationView, m_repositionView, m_splintUpperView,
                       m_splintLowerView, m_splintView}) {
        if (!view || (view != m_mesh3DView && view != dedicated && !view->meshData(key))) continue;
        view->addMesh(key, mesh, name);
        view->setMeshColor(key, objectColorForLabel(label));
        view->setMeshVisible(key, objectEntryVisible(label));
    }
    // Conversion creates an independent object; mask data, surface and undo stay untouched.
    syncModelViews();
    updateButtonStates();
    m_appState.setProjectDirty(true);
    statusBar()->showMessage(smooth
        ? tr("Objeto liso creado desde la máscara: %1. Relleno conservado; la máscara no cambia.").arg(name)
        : tr("Objeto exacto (vóxeles) creado desde la máscara: %1. Relleno conservado; la máscara no cambia.").arg(name));
}
