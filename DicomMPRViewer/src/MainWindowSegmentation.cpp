#include "MainWindow.h"
#include "MaskToObjectCore.h"
#include "MeshGenerator.h"
#include "SegmentationImporter.h"
#include "Mesh3DView.h"
#include "ObjectLabels.h"
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QStatusBar>
#include <QTableWidget>
#include <vtkPolyData.h>

QString meshLabelName(int label);
QColor meshLabelColor(int label);

void MainWindow::calculateObjectFromMask(int label, bool smooth)
{
    QString error;
    auto mesh = MaskToObjectCore::Convert(m_segmentationLabelmap, label, &error,
                                          smooth ? MaskToObjectCore::Surface::Smooth : MaskToObjectCore::Surface::Exact,
                                          m_volume);
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

void MainWindow::importUpperTeethSidecar(const QString& outputSegmentationPath)
{
    // Both arches come out on their own. The labelmap still keeps them inside the jaw.
    importToothSidecar(outputSegmentationPath, QStringLiteral("_dientes_superiores"), kUpperTeethLabel);
    importToothSidecar(outputSegmentationPath, QStringLiteral("_dientes_inferiores"), kLowerTeethLabel);
}

void MainWindow::importToothSidecar(const QString& outputSegmentationPath, const QString& suffix, int label)
{
    const QFileInfo info(outputSegmentationPath);
    QString name = info.fileName();
    for (const QString& ext : {QStringLiteral(".nrrd"), QStringLiteral(".nii.gz"), QStringLiteral(".mha")})
        if (name.endsWith(ext)) {
            name = name.left(name.size() - ext.size()) + suffix + ext;
            break;
        }
    const QString path = info.dir().filePath(name);
    if (!QFileInfo::exists(path))
        return;
    const QString objectName = meshLabelName(label);
    QString error;
    const auto teeth = SegmentationImporter::importLabelmap(path, &error);
    if (!teeth) {
        statusBar()->showMessage(tr("%1: %2").arg(objectName, error), 8000);
        return;
    }
    const auto exact = MeshGenerator::generateMesh(teeth, 1, false, 0, &error, false);
    const auto mesh = exact ? MeshGenerator::smoothToothMesh(exact) : nullptr;
    if (!mesh || mesh->GetNumberOfPolys() == 0) {
        statusBar()->showMessage(tr("%1: %2").arg(objectName, error), 8000);
        return;
    }
    addObjectEntry(objectName, meshLabelColor(label), label);
    if (m_mesh3DView) {
        const int key = objectActorKey(label);
        m_mesh3DView->addMesh(key, mesh, objectName);
        m_mesh3DView->setMeshColor(key, objectColorForLabel(label));
    }
    setObjectEntryVisible(label, false);
    m_appState.setProjectDirty(true);
}
