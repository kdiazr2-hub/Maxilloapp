#include "MainWindow.h"
#include "MaskToObjectCore.h"
#include "MeshGenerator.h"
#include "MPRView.h"
#include "SegmentationImporter.h"
#include "SplitMaskCore.h"
#include "ThresholdMaskCore.h"
#include "Mesh3DView.h"
#include "ObjectLabels.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QShortcut>
#include <QStatusBar>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>
#include <vtkImageData.h>
#include <vtkPolyData.h>

QString meshLabelName(int label);
QColor meshLabelColor(int label);

void MainWindow::calculateObjectFromMask(int label, bool smooth)
{
    QString error;
    auto mesh = MaskToObjectCore::Convert(m_segmentationLabelmap, label, &error,
                                          smooth ? MaskToObjectCore::Surface::Smooth : MaskToObjectCore::Surface::Exact,
                                          m_volume, closeCorticesFor(label));
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

void MainWindow::importWorkMaxillaSidecar(const QString& outputSegmentationPath)
{
    // The anatomical maxilla stays label 5. This copy is thickened only into air.
    const QFileInfo info(outputSegmentationPath);
    QString name = info.fileName();
    const QString suffix = QStringLiteral("_maxilar_trabajo");
    for (const QString& ext : {QStringLiteral(".nrrd"), QStringLiteral(".nii.gz"), QStringLiteral(".mha")})
        if (name.endsWith(ext)) {
            name = name.left(name.size() - ext.size()) + suffix + ext;
            break;
        }
    const QString path = info.dir().filePath(name);
    if (!QFileInfo::exists(path))
        return;
    QString error;
    const auto maskImage = SegmentationImporter::importLabelmap(path, &error);
    if (!maskImage)
        return;
    const auto mesh = MeshGenerator::generateMesh(maskImage, 1, true, 20, &error, false);
    if (!mesh || mesh->GetNumberOfPolys() == 0)
        return;
    const QString objectName = meshLabelName(kWorkMaxillaLabel);
    addObjectEntry(objectName, meshLabelColor(kWorkMaxillaLabel), kWorkMaxillaLabel);
    if (m_mesh3DView) {
        const int key = objectActorKey(kWorkMaxillaLabel);
        m_mesh3DView->addMesh(key, mesh, objectName);
        m_mesh3DView->setMeshColor(key, objectColorForLabel(kWorkMaxillaLabel));
    }
    m_appState.setProjectDirty(true);
}

void MainWindow::importUpperTeethSidecar(const QString& outputSegmentationPath)
{
    // Both arches are already labels 3 and 8. The sidecar is the copy root analysis reads.
    importToothSidecar(outputSegmentationPath, QStringLiteral("_dientes_superiores"), kUpperTeethLabel);
    importToothSidecar(outputSegmentationPath, QStringLiteral("_dientes_inferiores"), kLowerTeethLabel);
    importWorkMaxillaSidecar(outputSegmentationPath);
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
    const auto mesh = MaskToObjectCore::SlicerSurface(teeth, 1, dentalSmoothingFactor(), &error);
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
    m_appState.setProjectDirty(true);
}

namespace
{
void showSplitPreview(MainWindow* window, MPRView* axial, MPRView* coronal, MPRView* sagittal,
                      vtkImageData* preview)
{
    for (MPRView* view : {axial, coronal, sagittal}) {
        if (!view || !preview) continue;
        view->setSegmentationLabelmap(preview);
        view->setSegmentationHiddenLabels({});
        view->setSegmentationOpacity(0.55);
        view->setSegmentationVisible(true);
        view->noteSegmentationDataChanged();
        view->render();
    }
    Q_UNUSED(window);
}
}

void MainWindow::beginSplitMask()
{
    if (m_splitSession) return;
    if (!m_volume) {
        QMessageBox::warning(this, tr("Split mask"), tr("Primero cargue un volumen DICOM."));
        return;
    }
    if (!m_originBoneMask) {
        auto extracted = ThresholdMaskCore::blank(m_volume);
        std::int64_t found = 0;
        if (extracted && m_segmentationLabelmap
            && m_segmentationLabelmap->GetNumberOfPoints() == extracted->GetNumberOfPoints()
            && m_segmentationLabelmap->GetScalarType() == VTK_UNSIGNED_CHAR) {
            const auto* src = static_cast<const unsigned char*>(m_segmentationLabelmap->GetScalarPointer());
            auto* dst = static_cast<unsigned char*>(extracted->GetScalarPointer());
            const vtkIdType n = extracted->GetNumberOfPoints();
            for (vtkIdType i = 0; i < n; ++i) {
                if (src[i] != 1) continue;
                dst[i] = 1;
                ++found;
            }
        }
        if (found <= 0) {
            QMessageBox::warning(this, tr("Split mask"),
                                 tr("Primero cree la máscara Hueso con Segmentar."));
            return;
        }
        m_originBoneMask = extracted;
    }

    deactivateLassoTools();
    m_splitMarks.clear();
    m_splitUndo.clear();
    m_splitRedo.clear();
    m_splitResult = nullptr;
    m_splitRegion = 0;
    m_splitRadiusMm = 2.0;
    m_splitSession = true;
    rebuildSplitPreview();

    auto* dialog = new QDialog(this, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
    dialog->setWindowTitle(tr("Split mask"));
    dialog->setModal(false);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setMinimumWidth(360);
    m_splitDialog = dialog;

    auto* layout = new QVBoxLayout(dialog);
    auto* hint = new QLabel(tr("Tecla 1: maxilar, amarillo. Tecla 2: mandíbula, azul. "
                               "Arrastre para pintar. Ctrl borra. Alt y un arrastre vertical cambian el radio. "
                               "Lo pintado fuera del hueso se atenúa y no cuenta."), dialog);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    m_splitStatus = new QLabel(dialog);
    m_splitStatus->setWordWrap(true);
    layout->addWidget(m_splitStatus);

    auto* radiusRow = new QHBoxLayout();
    radiusRow->addWidget(new QLabel(tr("Radio"), dialog));
    m_splitRadiusBox = new QDoubleSpinBox(dialog);
    m_splitRadiusBox->setRange(0.5, 10.0);
    m_splitRadiusBox->setSingleStep(0.5);
    m_splitRadiusBox->setDecimals(1);
    m_splitRadiusBox->setSuffix(tr(" mm"));
    m_splitRadiusBox->setValue(m_splitRadiusMm);
    radiusRow->addWidget(m_splitRadiusBox);
    layout->addLayout(radiusRow);

    auto* islands = new QCheckBox(tr("Asignar las islas a la región más cercana"), dialog);
    layout->addWidget(islands);

    auto* buttons = new QHBoxLayout();
    auto* undo = new QPushButton(tr("Deshacer"), dialog);
    auto* redo = new QPushButton(tr("Rehacer"), dialog);
    auto* calculate = new QPushButton(tr("Calcular"), dialog);
    auto* accept = new QPushButton(tr("Aceptar"), dialog);
    auto* cancel = new QPushButton(tr("Cancelar"), dialog);
    buttons->addWidget(undo);
    buttons->addWidget(redo);
    buttons->addWidget(calculate);
    buttons->addWidget(accept);
    buttons->addWidget(cancel);
    layout->addLayout(buttons);

    connect(m_splitRadiusBox, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        m_splitRadiusMm = value;
    });
    connect(islands, &QCheckBox::toggled, this, [this](bool on) { m_splitAssignIslands = on; });
    connect(undo, &QPushButton::clicked, this, [this] { undoSplitMark(); });
    connect(redo, &QPushButton::clicked, this, [this] { redoSplitMark(); });
    connect(calculate, &QPushButton::clicked, this, [this] { calculateSplitMask(); });
    connect(accept, &QPushButton::clicked, this, [this, dialog] {
        acceptSplitMask();
        if (!m_splitSession && dialog) dialog->accept();
    });
    connect(cancel, &QPushButton::clicked, dialog, &QDialog::reject);
    connect(dialog, &QDialog::finished, this, [this](int) {
        if (m_splitSession) finishSplitSession();
    });

    auto* key1 = new QShortcut(Qt::Key_1, dialog);
    auto* key2 = new QShortcut(Qt::Key_2, dialog);
    key1->setContext(Qt::ApplicationShortcut);
    key2->setContext(Qt::ApplicationShortcut);
    connect(key1, &QShortcut::activated, this, [this] { m_splitRegion = 0; });
    connect(key2, &QShortcut::activated, this, [this] { m_splitRegion = 1; });

    for (MPRView* view : {m_axialView, m_coronalView, m_sagittalView}) {
        if (!view) continue;
        view->setBrushMode(true);
        connect(view, &MPRView::brushStroked, dialog,
                [this](MPRView*, double x, double y, double z, bool erase, bool starting) {
                    addSplitMark(x, y, z, erase, starting);
                });
        connect(view, &MPRView::brushRadiusDragged, dialog, [this](MPRView*, int deltaY) {
            if (!m_splitRadiusBox) return;
            const double next = std::clamp(m_splitRadiusMm + deltaY * 0.05, 0.5, 10.0);
            m_splitRadiusBox->setValue(next);
        });
    }

    dialog->show();
    statusBar()->showMessage(tr("Pinte el maxilar (1) y la mandíbula (2). Calcular muestra el corte; Aceptar crea las máscaras."));
}

void MainWindow::addSplitMark(double x, double y, double z, bool erase, bool starting)
{
    if (!m_splitSession || !m_originBoneMask) return;
    if (!erase && !starting && !m_splitMarks.isEmpty()) {
        const auto& last = m_splitMarks.back();
        const double dx = last.mm[0] - x;
        const double dy = last.mm[1] - y;
        const double dz = last.mm[2] - z;
        const double step = 0.35 * m_splitRadiusMm;
        if (last.region == m_splitRegion && dx * dx + dy * dy + dz * dz < step * step)
            return;
    }
    if (starting) {
        m_splitUndo.push_back(m_splitMarks);
        if (m_splitUndo.size() > 30) m_splitUndo.removeFirst();
        m_splitRedo.clear();
    }
    m_splitResult = nullptr;
    if (erase) {
        const double reach = m_splitRadiusMm * m_splitRadiusMm;
        QVector<SplitMarkRecord> kept;
        kept.reserve(m_splitMarks.size());
        for (const auto& mark : m_splitMarks) {
            const double dx = mark.mm[0] - x;
            const double dy = mark.mm[1] - y;
            const double dz = mark.mm[2] - z;
            if (mark.region == m_splitRegion && dx * dx + dy * dy + dz * dz <= reach) continue;
            kept.push_back(mark);
        }
        m_splitMarks = kept;
        rebuildSplitPreview();
    } else {
        SplitMarkRecord mark;
        mark.region = m_splitRegion;
        mark.mm[0] = x;
        mark.mm[1] = y;
        mark.mm[2] = z;
        mark.radiusMm = m_splitRadiusMm;
        m_splitMarks.push_back(mark);
        paintSplitMark(mark.region, mark.mm, mark.radiusMm);
        for (MPRView* view : {m_axialView, m_coronalView, m_sagittalView}) {
            if (!view) continue;
            view->noteSegmentationDataChanged();
            view->render();
        }
    }
    if (m_splitStatus) {
        int maxillary = 0;
        int mandibular = 0;
        for (const auto& mark : m_splitMarks) {
            if (mark.region == 0) ++maxillary;
            else ++mandibular;
        }
        m_splitStatus->setText(m_splitRegion == 0
            ? tr("Pincel maxilar (amarillo). Marcas: %1 maxilar, %2 mandíbula.").arg(maxillary).arg(mandibular)
            : tr("Pincel mandíbula (azul). Marcas: %1 maxilar, %2 mandíbula.").arg(maxillary).arg(mandibular));
    }
}

void MainWindow::rebuildSplitPreview()
{
    if (!m_originBoneMask) return;
    m_splitPreview = ThresholdMaskCore::blank(m_originBoneMask);
    if (!m_splitPreview) return;
    const vtkIdType n = m_splitPreview->GetNumberOfPoints();
    auto* dst = static_cast<unsigned char*>(m_splitPreview->GetScalarPointer());
    const auto* src = static_cast<const unsigned char*>(m_originBoneMask->GetScalarPointer());
    if (dst && src)
        std::memcpy(dst, src, static_cast<size_t>(n));
    for (const auto& mark : m_splitMarks)
        paintSplitMark(mark.region, mark.mm, mark.radiusMm);
    showSplitPreview(this, m_axialView, m_coronalView, m_sagittalView, m_splitPreview);
}

void MainWindow::paintSplitMark(int region, const double centerMm[3], double radiusMm)
{
    if (!m_splitPreview || !m_originBoneMask || !(radiusMm > 0.0)) return;
    double ijk[3] = {};
    m_originBoneMask->TransformPhysicalPointToContinuousIndex(centerMm, ijk);
    int extent[6] = {};
    m_originBoneMask->GetExtent(extent);
    double spacing[3] = {1, 1, 1};
    m_originBoneMask->GetSpacing(spacing);
    const double minSpacing = std::min(spacing[0], std::min(spacing[1], spacing[2]));
    const int cx = static_cast<int>(std::floor(ijk[0] + 0.5));
    const int cy = static_cast<int>(std::floor(ijk[1] + 0.5));
    const int cz = static_cast<int>(std::floor(ijk[2] + 0.5));
    const int reach = std::max(1, static_cast<int>(std::ceil(radiusMm / std::max(minSpacing, 1.0e-3))) + 1);
    const int nx = extent[1] - extent[0] + 1;
    const int ny = extent[3] - extent[2] + 1;
    auto* preview = static_cast<unsigned char*>(m_splitPreview->GetScalarPointer());
    const auto* origin = static_cast<const unsigned char*>(m_originBoneMask->GetScalarPointer());
    if (!preview || !origin) return;
    const double radius2 = radiusMm * radiusMm;
    for (int z = cz - reach; z <= cz + reach; ++z)
        for (int y = cy - reach; y <= cy + reach; ++y)
            for (int x = cx - reach; x <= cx + reach; ++x) {
                if (x < extent[0] || y < extent[2] || z < extent[4]
                    || x > extent[1] || y > extent[3] || z > extent[5]) continue;
                double point[3] = {};
                const double continuous[3] = {static_cast<double>(x), static_cast<double>(y), static_cast<double>(z)};
                m_originBoneMask->TransformContinuousIndexToPhysicalPoint(continuous, point);
                const double dx = point[0] - centerMm[0];
                const double dy = point[1] - centerMm[1];
                const double dz = point[2] - centerMm[2];
                if (dx * dx + dy * dy + dz * dz > radius2) continue;
                const vtkIdType i = (x - extent[0])
                    + static_cast<vtkIdType>(nx) * ((y - extent[2])
                    + static_cast<vtkIdType>(ny) * (z - extent[4]));
                const bool inside = origin[i] == 1;
                preview[i] = inside
                    ? static_cast<unsigned char>(region == 0 ? 5 : 6)
                    : static_cast<unsigned char>(14);
            }
    m_splitPreview->Modified();
}

void MainWindow::undoSplitMark()
{
    if (!m_splitSession || m_splitUndo.isEmpty()) return;
    m_splitRedo.push_back(m_splitMarks);
    m_splitMarks = m_splitUndo.takeLast();
    m_splitResult = nullptr;
    rebuildSplitPreview();
}

void MainWindow::redoSplitMark()
{
    if (!m_splitSession || m_splitRedo.isEmpty()) return;
    m_splitUndo.push_back(m_splitMarks);
    m_splitMarks = m_splitRedo.takeLast();
    m_splitResult = nullptr;
    rebuildSplitPreview();
}

void MainWindow::calculateSplitMask()
{
    if (!m_splitSession || !m_originBoneMask || !m_volume) return;
    if (m_splitMarks.isEmpty()) {
        statusBar()->showMessage(tr("Pinte al menos una región antes de calcular."));
        return;
    }
    std::vector<SplitMarker> markers;
    markers.reserve(static_cast<size_t>(m_splitMarks.size()));
    for (const auto& mark : m_splitMarks) {
        SplitMarker marker;
        marker.region = mark.region;
        marker.centerMm[0] = mark.mm[0];
        marker.centerMm[1] = mark.mm[1];
        marker.centerMm[2] = mark.mm[2];
        marker.radiusMm = mark.radiusMm;
        markers.push_back(marker);
    }
    const bool islands = m_splitAssignIslands;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    auto percent = std::make_shared<std::atomic<int>>(0);
    auto holder = std::make_shared<vtkSmartPointer<vtkImageData>>();
    auto* origin = m_originBoneMask.Get();
    auto* intensities = m_volume.Get();
    auto future = QtConcurrent::run([markers, islands, cancel, percent, holder, origin, intensities]() {
        SplitRequest request;
        request.regionCount = 2;
        request.originLabel = 1;
        request.assignIslandsToNearest = islands;
        request.markers = markers.data();
        request.markerCount = static_cast<int>(markers.size());
        *holder = SplitMaskCore::split(origin, intensities, request, cancel.get(),
            [](double fraction, void* user) {
                static_cast<std::atomic<int>*>(user)->store(static_cast<int>(fraction * 100.0),
                                                            std::memory_order_relaxed);
            }, percent.get());
    });

    QProgressDialog progress(tr("Dividiendo la máscara de hueso..."), tr("Cancelar"), 0, 100, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    QFutureWatcher<void> watcher;
    QEventLoop loop;
    QTimer timer;
    timer.setInterval(100);
    connect(&timer, &QTimer::timeout, &progress, [&] { progress.setValue(percent->load()); });
    connect(&progress, &QProgressDialog::canceled, &progress, [&] { cancel->store(true); });
    connect(&watcher, &QFutureWatcher<void>::finished, &loop, &QEventLoop::quit);
    timer.start();
    watcher.setFuture(future);
    loop.exec();
    timer.stop();
    progress.setValue(100);

    if (cancel->load() || !*holder) {
        statusBar()->showMessage(tr("Split mask cancelado. La máscara Hueso no cambió."));
        return;
    }
    m_splitResult = *holder;
    auto* src = static_cast<const unsigned char*>(m_splitResult->GetScalarPointer());
    auto* dst = static_cast<unsigned char*>(m_splitPreview->GetScalarPointer());
    if (!src || !dst) return;
    const vtkIdType n = m_splitPreview->GetNumberOfPoints();
    for (vtkIdType i = 0; i < n; ++i) {
        if (src[i] == 1) dst[i] = 5;
        else if (src[i] == 2) dst[i] = 6;
        else if (src[i] == 3) dst[i] = static_cast<unsigned char>(kUnassignedBoneLabel);
        else dst[i] = 0;
    }
    m_splitPreview->Modified();
    showSplitPreview(this, m_axialView, m_coronalView, m_sagittalView, m_splitPreview);
    statusBar()->showMessage(tr("Corte listo. Aceptar crea Maxilar y Mandíbula. Hueso queda igual."));
}

void MainWindow::acceptSplitMask()
{
    if (!m_splitSession) return;
    if (!m_splitResult) calculateSplitMask();
    if (!m_splitResult || !m_originBoneMask) return;

    auto colored = ThresholdMaskCore::blank(m_splitResult);
    if (!colored) return;
    const auto* src = static_cast<const unsigned char*>(m_splitResult->GetScalarPointer());
    auto* dst = static_cast<unsigned char*>(colored->GetScalarPointer());
    const vtkIdType n = colored->GetNumberOfPoints();
    bool present[11] = {};
    for (vtkIdType i = 0; i < n; ++i) {
        unsigned char label = 0;
        if (src[i] == 1) label = 5;
        else if (src[i] == 2) label = 6;
        else if (src[i] == 3) label = static_cast<unsigned char>(kUnassignedBoneLabel);
        dst[i] = label;
        if (label < 11) present[label] = true;
    }

    pushLabelmapUndo();
    if (!m_segmentationLabelmap || m_segmentationLabelmap->GetNumberOfPoints() != n) {
        m_segmentationLabelmap = colored;
    } else {
        auto destination = ThresholdMaskCore::unsignedLabelmap(m_segmentationLabelmap);
        const int clear[] = {1, 5, 6, kUnassignedBoneLabel};
        if (ThresholdMaskCore::stampMask(destination, colored, clear, 4) < 0) {
            QMessageBox::warning(this, tr("Split mask"), tr("La máscara no coincide con la segmentación actual."));
            return;
        }
        m_segmentationLabelmap = destination;
    }

    const QColor yellow(242, 209, 31);
    const QColor blue(64, 122, 242);
    const QColor gray(150, 150, 150);
    auto publish = [&](int label, const QColor& color) {
        if (!present[label]) return;
        addMaskEntry(meshLabelName(label), color, label);
        m_solidBoneLabels.insert(label);
        calculateObjectFromMask(label, true);
        setObjectEntryVisible(label, true);
        if (m_mesh3DView) {
            m_mesh3DView->setMeshColor(objectActorKey(label), color);
            m_mesh3DView->setMeshVisible(objectActorKey(label), true);
        }
    };
    publish(5, yellow);
    publish(6, blue);
    publish(kUnassignedBoneLabel, gray);
    refreshSegmentationOverlays();
    m_appState.setSegmentationDone(true);
    m_appState.setProjectDirty(true);
    updateButtonStates();
    statusBar()->showMessage(tr("Maxilar y mandíbula creados. La máscara Hueso de origen no se modificó."));
    finishSplitSession();
}

void MainWindow::finishSplitSession()
{
    m_splitSession = false;
    m_splitPreview = nullptr;
    m_splitResult = nullptr;
    for (MPRView* view : {m_axialView, m_coronalView, m_sagittalView})
        if (view) view->setBrushMode(false);
    m_splitStatus = nullptr;
    m_splitRadiusBox = nullptr;
    if (m_splitDialog) {
        auto* dialog = m_splitDialog;
        m_splitDialog = nullptr;
        dialog->disconnect(this);
        dialog->close();
    }
    refreshSegmentationOverlays();
}
