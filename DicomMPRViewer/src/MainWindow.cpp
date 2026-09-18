#include "MainWindow.h"
#include "MPRView.h"
#include "AppStateManager.h"
#include "BoneSplitterService.h"
#include "BoneCavityFill.h"
#include "CompositeModelCore.h"
#include "CranioPalette.h"
#include "LoggerCore.h"
#include "NrrdVolumeExporter.h"
#include "ProjectSerializer.h"
#include "RegistrationResult.h"
#include "SegmentationMaskEditor.h"
#include "AngleMeasurementTool.h"
#include "AnnotationTool.h"
#include "AreaMeasurementTool.h"
#include "DistanceMeasurementTool.h"
#include "MeasurementSerialization.h"
#include "MeasurementTool.h"
#include "Mesh3DView.h"
#include "PerpendicularDistanceTool.h"
#include "ROIStatisticsTool.h"
#include "SeriesSelectionDialog.h"
#include "SegmentationImporter.h"
#include "StandaloneDentalSegmentatorService.h"
#include "SegmentationProgressDialog.h"
#include "SplintGenerator.h"
#include "TransformCore.h"
#include "GeometryValidation.h"
#include "MeshGenerator.h"
#include "ObjectLabels.h"

#include <QAction>
#include <QActionGroup>
#include <QAbstractItemView>
#include <QApplication>
#include <QTimer>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCursor>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QIcon>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QRadioButton>
#include <QDebug>
#include <QScrollArea>
#include <QSet>
#include <QSignalBlocker>
#include <QSlider>
#include <QSizePolicy>
#include <QPixmap>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextStream>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVector>
#include <QVector3D>
#include <QVTKOpenGLNativeWidget.h>
#include <QWidget>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <utility>
#include <vector>

#include <vtkAppendPolyData.h>
#include <vtkBooleanOperationPolyDataFilter.h>
#include <vtkCellArray.h>
#include <vtkClipPolyData.h>
#include <vtkCutter.h>
#include <vtkPlane.h>
#include <vtkCleanPolyData.h>
#include <vtkContourFilter.h>
#include <vtkIdList.h>
#include <vtkImplicitBoolean.h>
#include <vtkImplicitPolyDataDistance.h>
#include <vtkIterativeClosestPointTransform.h>
#include <vtkLandmarkTransform.h>
#include <vtkMatrix4x4.h>
#include <vtkResliceCursor.h>
#include <vtkImageData.h>
#include <vtkLineSource.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataConnectivityFilter.h>
#include <vtkPolyDataNormals.h>
#include <vtkRegularPolygonSource.h>
#include <vtkSTLReader.h>
#include <vtkSTLWriter.h>
#include "MeshRepairCore.h"
#include <vtkStaticPointLocator.h>
#include <vtkSampleFunction.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>
#include <vtkTriangleFilter.h>
#include <vtkTubeFilter.h>
#include <vtkXMLPolyDataWriter.h>

// ─────────────────────────────────────────────────────────────────────────────
static MeasurementViewOrientation toMeasurementView(MPROrientation orientation)
{
    switch (orientation) {
        case MPROrientation::Axial:    return MeasurementViewOrientation::Axial;
        case MPROrientation::Coronal:  return MeasurementViewOrientation::Coronal;
        case MPROrientation::Sagittal: return MeasurementViewOrientation::Sagittal;
    }
    return MeasurementViewOrientation::Axial;
}

static QString typeLabel(MeasurementType type)
{
    switch (type) {
        case MeasurementType::Distance:              return "Distancia";
        case MeasurementType::Angle:                 return "Angulo";
        case MeasurementType::Annotation:            return "Anotacion";
        case MeasurementType::Area:                  return "Area";
        case MeasurementType::ROIStatistics:         return "ROI";
        case MeasurementType::PerpendicularDistance: return "Perpendicular";
    }
    return "Distancia";
}

static QString viewLabel(MeasurementViewOrientation view)
{
    switch (view) {
        case MeasurementViewOrientation::Axial:    return "Axial";
        case MeasurementViewOrientation::Coronal:  return "Coronal";
        case MeasurementViewOrientation::Sagittal: return "Sagital";
    }
    return "Axial";
}

static QString valueLabel(const Measurement& measurement)
{
    if (measurement.type == MeasurementType::Distance) {
        return QString("%1 mm").arg(measurement.value, 0, 'f', 1);
    }
    if (measurement.type == MeasurementType::Angle) {
        return QString("%1 deg").arg(measurement.value, 0, 'f', 1);
    }
    if (measurement.type == MeasurementType::Area ||
        measurement.type == MeasurementType::ROIStatistics) {
        return QString("%1 mm2").arg(measurement.areaMm2 > 0.0 ? measurement.areaMm2 : measurement.value,
                                     0, 'f', 1);
    }
    if (measurement.type == MeasurementType::PerpendicularDistance) {
        return QString("%1 mm").arg(measurement.value, 0, 'f', 1);
    }
    return QString();
}

static constexpr int kObjectOpacityRole = Qt::UserRole + 2;
static constexpr int kObjectOnTopRole = Qt::UserRole + 3;

static vtkSmartPointer<vtkMatrix4x4> identityMatrix()
{
    auto m = vtkSmartPointer<vtkMatrix4x4>::New();
    m->Identity();
    return m;
}

static QString matrixToText(vtkMatrix4x4* matrix)
{
    if (!matrix) return QStringLiteral("<sin matriz>");
    QStringList rows;
    for (int r = 0; r < 4; ++r) {
        rows << QString("%1 %2 %3 %4")
            .arg(matrix->GetElement(r, 0), 0, 'f', 8)
            .arg(matrix->GetElement(r, 1), 0, 'f', 8)
            .arg(matrix->GetElement(r, 2), 0, 'f', 8)
            .arg(matrix->GetElement(r, 3), 0, 'f', 8);
    }
    return rows.join('\n');
}

static QString meshBoundsText(vtkPolyData* mesh)
{
    if (!mesh || mesh->GetNumberOfPoints() == 0) return QStringLiteral("<empty>");
    double b[6] = {};
    mesh->GetBounds(b);
    return QString("[%1, %2] x [%3, %4] x [%5, %6]")
        .arg(b[0], 0, 'f', 3).arg(b[1], 0, 'f', 3)
        .arg(b[2], 0, 'f', 3).arg(b[3], 0, 'f', 3)
        .arg(b[4], 0, 'f', 3).arg(b[5], 0, 'f', 3);
}

static QString meshCentroidText(vtkPolyData* mesh)
{
    if (!mesh || mesh->GetNumberOfPoints() == 0) return QStringLiteral("<empty>");
    double p[3] = {};
    double c[3] = {};
    const vtkIdType step = std::max<vtkIdType>(1, mesh->GetNumberOfPoints() / 20000);
    vtkIdType count = 0;
    for (vtkIdType i = 0; i < mesh->GetNumberOfPoints(); i += step) {
        mesh->GetPoint(i, p);
        c[0] += p[0]; c[1] += p[1]; c[2] += p[2];
        ++count;
    }
    if (count > 0) {
        c[0] /= static_cast<double>(count);
        c[1] /= static_cast<double>(count);
        c[2] /= static_cast<double>(count);
    }
    return QString("(%1, %2, %3)")
        .arg(c[0], 0, 'f', 3).arg(c[1], 0, 'f', 3).arg(c[2], 0, 'f', 3);
}

static double meshDiagonalLength(vtkPolyData* mesh)
{
    if (!mesh || mesh->GetNumberOfPoints() == 0) return 0.0;
    double b[6] = {};
    mesh->GetBounds(b);
    const double dx = b[1] - b[0];
    const double dy = b[3] - b[2];
    const double dz = b[5] - b[4];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

static bool fitPlaneFromPolyData(vtkPolyData* mesh,
                                 QVector3D preferredNormal,
                                 QVector3D* centerOut,
                                 QVector3D* normalOut,
                                 double* radiusOut)
{
    if (!mesh || mesh->GetNumberOfPoints() < 3 || !centerOut || !normalOut)
        return false;

    QVector3D center(0.0f, 0.0f, 0.0f);
    double p[3] = {};
    const vtkIdType pointCount = mesh->GetNumberOfPoints();
    for (vtkIdType i = 0; i < pointCount; ++i) {
        mesh->GetPoint(i, p);
        center += QVector3D(static_cast<float>(p[0]),
                            static_cast<float>(p[1]),
                            static_cast<float>(p[2]));
    }
    center /= static_cast<float>(pointCount);

    QVector3D normal(0.0f, 0.0f, 0.0f);
    auto ids = vtkSmartPointer<vtkIdList>::New();
    for (vtkIdType cellId = 0; cellId < mesh->GetNumberOfCells(); ++cellId) {
        mesh->GetCellPoints(cellId, ids);
        const vtkIdType n = ids->GetNumberOfIds();
        if (n < 3) continue;

        for (vtkIdType i = 0; i < n; ++i) {
            double pi[3] = {};
            double pj[3] = {};
            mesh->GetPoint(ids->GetId(i), pi);
            mesh->GetPoint(ids->GetId((i + 1) % n), pj);

            // Newell normal is robust for the transformed plane polygon, including
            // non-uniform gizmo scaling.
            normal += QVector3D(
                static_cast<float>((pi[1] - pj[1]) * (pi[2] + pj[2])),
                static_cast<float>((pi[2] - pj[2]) * (pi[0] + pj[0])),
                static_cast<float>((pi[0] - pj[0]) * (pi[1] + pj[1])));
        }
    }

    if (normal.lengthSquared() < 1e-8f) {
        double p0[3] = {};
        mesh->GetPoint(0, p0);
        const QVector3D a(static_cast<float>(p0[0]),
                          static_cast<float>(p0[1]),
                          static_cast<float>(p0[2]));
        for (vtkIdType i = 1; i < pointCount - 1 && normal.lengthSquared() < 1e-8f; ++i) {
            double p1[3] = {};
            mesh->GetPoint(i, p1);
            const QVector3D b(static_cast<float>(p1[0]),
                              static_cast<float>(p1[1]),
                              static_cast<float>(p1[2]));
            for (vtkIdType j = i + 1; j < pointCount; ++j) {
                double p2[3] = {};
                mesh->GetPoint(j, p2);
                const QVector3D c(static_cast<float>(p2[0]),
                                  static_cast<float>(p2[1]),
                                  static_cast<float>(p2[2]));
                normal = QVector3D::crossProduct(b - a, c - a);
                if (normal.lengthSquared() >= 1e-8f)
                    break;
            }
        }
    }

    if (normal.lengthSquared() < 1e-8f)
        return false;

    normal.normalize();
    if (preferredNormal.lengthSquared() > 1e-8f) {
        preferredNormal.normalize();
        if (QVector3D::dotProduct(normal, preferredNormal) < 0.0f)
            normal = -normal;
    }

    double radius2 = 0.0;
    for (vtkIdType i = 0; i < pointCount; ++i) {
        mesh->GetPoint(i, p);
        const QVector3D q(static_cast<float>(p[0]),
                          static_cast<float>(p[1]),
                          static_cast<float>(p[2]));
        radius2 = std::max(radius2, static_cast<double>((q - center).lengthSquared()));
    }

    *centerOut = center;
    *normalOut = normal;
    if (radiusOut)
        *radiusOut = std::sqrt(radius2);
    return true;
}

static QVector3D fallbackInPlaneAxis(QVector3D normal)
{
    if (normal.lengthSquared() < 1e-8f)
        return QVector3D(1.0f, 0.0f, 0.0f);
    normal.normalize();

    QVector3D axis = QVector3D::crossProduct(normal, QVector3D(0.0f, 0.0f, 1.0f));
    if (axis.lengthSquared() < 1e-8f)
        axis = QVector3D::crossProduct(normal, QVector3D(0.0f, 1.0f, 0.0f));
    if (axis.lengthSquared() < 1e-8f)
        return QVector3D(1.0f, 0.0f, 0.0f);
    axis.normalize();
    return axis;
}

static QVector3D projectedInPlaneAxis(QVector3D axis, QVector3D normal)
{
    if (normal.lengthSquared() < 1e-8f)
        return axis.lengthSquared() > 1e-8f ? axis.normalized() : QVector3D(1.0f, 0.0f, 0.0f);

    normal.normalize();
    axis -= normal * QVector3D::dotProduct(axis, normal);
    if (axis.lengthSquared() < 1e-8f)
        axis = fallbackInPlaneAxis(normal);
    else
        axis.normalize();
    return axis;
}

// Also used by MainWindowOsteotomy.cpp.
QString meshLabelName(int label)
{
    switch (label) {
        case 1: return "Hueso";
        case 2: return "Tejido blando";
        case 3: return "Dientes superiores";
        case 4: return "Via aerea";
        case 5: return "Maxilar";
        case 6: return "Mandíbula";
        case 7: return "Canal mandibular";
        case 8: return "Dientes inferiores";
        case kUpperArchLabel: return "Arco superior STL";
        case kLowerArchLabel: return "Arco inferior STL";
        case kUpperCompositeLabel: return "Compuesto maxilar";
        case kLowerCompositeLabel: return "Compuesto mandibular";
        case kLeFortCranialLabel:  return "Base craneal";
        case kLeFortSegLabel:      return "Segmento Le Fort I";
        case kBssoGuideLabel:      return "Guia BSSO";
        case kBssoDistalLabel:     return "Segmento distal";
        case kBssoProximalLabel:   return "Ramas proximales BSSO";
        case kBssoProximalRightLabel: return "Rama proximal derecha BSSO";
        case kBssoProximalLeftLabel:  return "Rama proximal izquierda BSSO";
        case kGenioBodyLabel:      return "Mandibula post-menton";
        case kGenioSegmentLabel:   return "Segmento menton";
        case kBiteScanLabel:       return "Escaneo de mordida";
        case kIntermediateSplintLabel: return "Ferula intermedia";
        case kFinalSplintLabel:        return "Ferula final";
        case kGuideWrapLabel:      return "Envolvente de la guía";
        case kGuideMeshLabel:      return "Guía quirúrgica";
        default: return QString("Label %1").arg(label);
    }
}

static bool isEditableSegmentationLabel(int label)
{
    return label > 0 && label < kUpperArchLabel;
}

QColor meshLabelColor(int label)
{
    switch (label) {
        case 1: return CranioPalette::bone();
        case 2: return CranioPalette::softTissue();
        case 3: return CranioPalette::dental();
        case 5: return CranioPalette::maxilla();
        case 6: return CranioPalette::mandible();
        case 7: return CranioPalette::canal();
        case 8: return CranioPalette::dental();
        case kUpperArchLabel: return CranioPalette::dental();
        case kLowerArchLabel: return CranioPalette::dental();
        case kUpperCompositeLabel: return CranioPalette::maxilla();
        case kLowerCompositeLabel: return CranioPalette::mandible();
        case kLeFortCranialLabel:  return CranioPalette::boneDark();
        case kLeFortSegLabel:      return CranioPalette::osteotomy();
        case kBssoGuideLabel:      return CranioPalette::guide();
        case kBssoDistalLabel:     return CranioPalette::mandible();
        case kBssoProximalLabel:   return CranioPalette::boneLight();
        case kBssoProximalRightLabel: return QColor(211, 198, 185);
        case kBssoProximalLeftLabel:  return QColor(198, 184, 171);
        case kGenioBodyLabel:      return CranioPalette::mandible();
        case kGenioSegmentLabel:   return QColor(225, 212, 166);
        case kBiteScanLabel:       return QColor(245, 245, 235);
        case kIntermediateSplintLabel: return QColor(244, 238, 220);
        case kFinalSplintLabel:        return QColor(250, 246, 232);
        default: return CranioPalette::fallback();
    }

    switch (label) {
        case 1: return QColor(255, 200, 100);   // Hueso  — amber
        case 2: return QColor(255, 140, 140);   // Tejido — coral
        case 3: return QColor(245, 220, 90);     // Dientes superiores — amarillo
        case 5: return QColor(255, 160, 160);   // Maxilar   — rosado
        case 6: return QColor(140, 190, 255);   // Mandíbula — azul
        case 7: return QColor(220, 95, 80);      // Canal mandibular — rojo
        case 8: return QColor(255, 185, 70);     // Dientes inferiores — naranja
        case kUpperArchLabel: return QColor(235, 150, 35);
        case kLowerArchLabel: return QColor(245, 185, 65);
        case kUpperCompositeLabel: return QColor(210, 185, 140);
        case kLowerCompositeLabel: return QColor(185, 205, 220);
        case kLeFortCranialLabel:  return QColor(85, 145, 210);   // blue — cranial base
        case kLeFortSegLabel:      return QColor(210, 70, 70);    // red — Le Fort segment
        case kBssoGuideLabel:      return QColor(96, 150, 58);    // green — BSSO guide
        case kBssoDistalLabel:     return QColor(185, 215, 235);
        case kBssoProximalLabel:   return QColor(125, 175, 220);
        case kBssoProximalRightLabel: return QColor(120, 180, 225);
        case kBssoProximalLeftLabel:  return QColor(145, 200, 235);
        case kGenioBodyLabel:      return QColor(185, 215, 235);
        case kGenioSegmentLabel:   return QColor(225, 210, 95);
        case kBiteScanLabel:       return QColor(245, 245, 235);
        default: return QColor(200, 140, 220);
    }
}

static std::vector<int> presentSegmentationLabels(vtkImageData* labelmap)
{
    std::array<bool, 256> present {};
    if (!labelmap || labelmap->GetNumberOfPoints() == 0) return {};

    int extent[6] = {};
    labelmap->GetExtent(extent);
    for (int z = extent[4]; z <= extent[5]; ++z) {
        for (int y = extent[2]; y <= extent[3]; ++y) {
            for (int x = extent[0]; x <= extent[1]; ++x) {
                const int label = static_cast<int>(std::lround(
                    labelmap->GetScalarComponentAsDouble(x, y, z, 0)));
                if (label > 0 && label < static_cast<int>(present.size())) {
                    present[static_cast<size_t>(label)] = true;
                }
            }
        }
    }

    const std::array<int, 8> preferredOrder {5, 6, 3, 8, 7, 1, 2, 4};
    std::vector<int> labels;
    for (int label : preferredOrder) {
        if (present[static_cast<size_t>(label)]) labels.push_back(label);
    }
    for (int label = 1; label < static_cast<int>(present.size()); ++label) {
        if (!present[static_cast<size_t>(label)]) continue;
        if (std::find(labels.begin(), labels.end(), label) == labels.end()) {
            labels.push_back(label);
        }
    }
    return labels;
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("Planificación Maxilofacial");
    setMinimumSize(1100, 700);
    resize(1400, 850);

    m_upperArchRegistrationMatrix = identityMatrix();
    m_lowerArchRegistrationMatrix = identityMatrix();

    setStyleSheet(
        "QMainWindow, QDialog { background-color:#1c1c1e; color:#f5f5f7; font-family:'Segoe UI Variable','Segoe UI'; }"
        "QWidget { selection-background-color:#0a84ff; selection-color:#ffffff; }"
        "QMenuBar { background-color:#1c1c1e; color:#d1d1d6; border-bottom:1px solid #2c2c2e; }"
        "QMenuBar::item { padding:5px 10px; border-radius:8px; }"
        "QMenuBar::item:selected { background-color:#2c2c2e; color:#ffffff; }"
        "QMenu { background-color:#2c2c2e; color:#f5f5f7; border:1px solid #3a3a3c; border-radius:12px; padding:6px; }"
        "QMenu::item { padding:7px 22px; border-radius:8px; }"
        "QMenu::item:selected { background-color:#0a84ff; color:#ffffff; }"
        "QStatusBar { background-color:#1c1c1e; color:#98989d; font-size:11px; border-top:1px solid #2c2c2e; }"
        "QGroupBox { border:1px solid #3a3a3c; border-radius:12px; margin-top:12px; padding-top:12px; font-weight:600; color:#f5f5f7; background:#242426; }"
        "QGroupBox::title { subcontrol-origin:margin; left:10px; padding:0 4px; color:#0a84ff; }"
        "QProgressBar { border:1px solid #3a3a3c; border-radius:8px; background:#2c2c2e; color:#f5f5f7; text-align:center; font-size:10px; }"
        "QProgressBar::chunk { background:#0a84ff; border-radius:8px; }"
        "QPushButton { background-color:#3a3a3c; color:#f5f5f7; border:1px solid #4a4a4c; border-radius:12px; padding:7px 18px; font-weight:600; }"
        "QPushButton:hover { background-color:#48484a; }"
        "QPushButton:pressed { background-color:#2c2c2e; }"
        "QPushButton:checked { background-color:#0a84ff; border-color:#0a84ff; color:#ffffff; }"
        "QPushButton:disabled { background-color:#242426; border-color:#2c2c2e; color:#636366; }"
        "QPushButton:default { background-color:#0a84ff; border-color:#0a84ff; color:#ffffff; }"
        "QPushButton:default:hover { background-color:#1d9bf0; }"
        "QLineEdit, QComboBox, QDoubleSpinBox, QSpinBox { background:#2c2c2e; color:#f5f5f7; border:1px solid #3a3a3c; border-radius:10px; padding:5px 8px; }"
        "QLineEdit:focus, QComboBox:focus, QDoubleSpinBox:focus, QSpinBox:focus { border:1px solid #0a84ff; }"
        "QTableWidget { background:#1f1f21; color:#f5f5f7; gridline-color:#2c2c2e; border:1px solid #2c2c2e; border-radius:10px; }"
        "QHeaderView::section { background:#2c2c2e; color:#f5f5f7; border:0; border-bottom:1px solid #3a3a3c; padding:5px; font-weight:600; }"
        "QTableWidget::item:selected { background:#1f3b57; color:#ffffff; }"
        "QTabWidget::pane { border:0; background:#1f1f21; }"
        "QTabBar::tab { background:#2c2c2e; color:#98989d; border:0; border-radius:10px; padding:7px 12px; margin:2px; }"
        "QTabBar::tab:selected { background:#3a3a3c; color:#ffffff; }"
        "QScrollBar:vertical { background:#1c1c1e; width:10px; margin:0; }"
        "QScrollBar::handle:vertical { background:#4a4a4c; border-radius:5px; min-height:28px; }"
        "QScrollBar::handle:vertical:hover { background:#5a5a5c; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }"
        "QDialogButtonBox QPushButton { min-width:88px; }");

    buildMenuBar();
    menuBar()->hide();
    buildToolBar();
    buildCentralWidget();
    qApp->installEventFilter(this);

    // ── Progress bar (lives in the status bar) ─────────────────────────────
    m_progressBar = new QProgressBar(this);
    m_progressBar->setFixedWidth(220);
    m_progressBar->setFixedHeight(14);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setVisible(false);
    statusBar()->addPermanentWidget(m_progressBar);

    // ── Cache directory ────────────────────────────────────────────────────
    m_cacheDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                 + QStringLiteral("/DicomCache");

    // ── Async worker thread ────────────────────────────────────────────────
    // Register types used in cross-thread signals (queued connections).
    qRegisterMetaType<vtkSmartPointer<vtkImageData>>("vtkSmartPointer<vtkImageData>");
    qRegisterMetaType<DicomVolumeLoader::VolumeMetadata>("DicomVolumeLoader::VolumeMetadata");
    qRegisterMetaType<SeriesInfo>("SeriesInfo");
    qRegisterMetaType<QVector<SeriesInfo>>("QVector<SeriesInfo>");

    m_asyncLoader  = new AsyncDicomLoader;         // no parent: will be moved to thread
    m_loaderThread = new QThread(this);
    m_asyncLoader->moveToThread(m_loaderThread);

    // Clean up worker when thread finishes
    connect(m_loaderThread, &QThread::finished,
            m_asyncLoader,  &QObject::deleteLater);

    // Wire signals from worker → main-thread slots (auto = queued across threads)
    connect(m_asyncLoader, &AsyncDicomLoader::statusChanged,
            this,          &MainWindow::onLoadStatusChanged);
    connect(m_asyncLoader, &AsyncDicomLoader::progressChanged,
            this,          &MainWindow::onLoadProgressChanged);
    connect(m_asyncLoader, &AsyncDicomLoader::seriesFound,
            this,          &MainWindow::onSeriesFound);
    connect(m_asyncLoader, &AsyncDicomLoader::previewReady,
            this,          &MainWindow::onPreviewReady);
    connect(m_asyncLoader, &AsyncDicomLoader::volumeReady,
            this,          &MainWindow::onVolumeReady);
    connect(m_asyncLoader, &AsyncDicomLoader::errorOccurred,
            this,          &MainWindow::onLoadError);

    // Wire main-thread signal → worker slot for multi-series user selection
    connect(this,          &MainWindow::requestLoadSeries,
            m_asyncLoader, &AsyncDicomLoader::loadSeries);

    m_loaderThread->start();

    // ── Measurement manager ────────────────────────────────────────────────
    connect(&m_measurementManager, &MeasurementManager::measurementsChanged,
            this, [this] {
                refreshMeasurementTable();
                refreshMeasurementOverlays();
            });

    m_aiSegmentationService = new StandaloneDentalSegmentatorService(this);
    connect(m_aiSegmentationService, &AISegmentationService::statusChanged,
            this, [this](const QString& message) { statusBar()->showMessage(message); });
    connect(m_aiSegmentationService, &AISegmentationService::progressChanged,
            this, [this](int progress) {
                if (!m_progressBar) return;
                m_progressBar->setVisible(true);
                m_progressBar->setFormat(tr("Segmentacion IA  %p%"));
                m_progressBar->setValue(progress);
            });
    connect(m_aiSegmentationService, &AISegmentationService::segmentationFinished,
            this, &MainWindow::onSegmentationFinished);
    connect(m_aiSegmentationService, &AISegmentationService::errorOccurred,
            this, &MainWindow::onSegmentationError);
    connect(m_aiSegmentationService, &AISegmentationService::cancelled,
            this, &MainWindow::onSegmentationCancelled);

    // ── Bone splitter (Max/Mand) ───────────────────────────────────────────
    m_boneSplitter = new BoneSplitterService(this);
    connect(m_boneSplitter, &BoneSplitterService::statusChanged,
            this, [this](const QString& msg) { statusBar()->showMessage(msg); });
    connect(m_boneSplitter, &BoneSplitterService::progressChanged,
            this, [this](int pct) {
                if (!m_progressBar) return;
                m_progressBar->setVisible(pct > 0 && pct < 100);
                m_progressBar->setFormat(tr("Max/Mand  %p%"));
                m_progressBar->setValue(pct);
            });
    connect(m_boneSplitter, &BoneSplitterService::splitFinished,
            this, &MainWindow::onBoneSplitFinished);
    connect(m_boneSplitter, &BoneSplitterService::errorOccurred,
            this, &MainWindow::onBoneSplitError);

    statusBar()->showMessage(
        "Ready  —  File › Open DICOM Folder  (Ctrl+O)");
}

// ─────────────────────────────────────────────────────────────────────────────
MainWindow::~MainWindow()
{
    if (qApp) qApp->removeEventFilter(this);
    // Gracefully stop the worker thread before destroying objects
    m_loaderThread->quit();
    m_loaderThread->wait(5000);  // max 5 s
}

void MainWindow::buildMenuBar()
{
    // ── File ──────────────────────────────────────────────────────────────
    auto* fileMenu = menuBar()->addMenu(tr("&File"));

    m_openAction = fileMenu->addAction(tr("Open DICOM Folder…"));
    m_openAction->setShortcut(QKeySequence::Open);
    connect(m_openAction, &QAction::triggered, this, &MainWindow::onOpenDicomFolder);

    fileMenu->addSeparator();

    auto* exitAct = fileMenu->addAction(tr("E&xit"));
    exitAct->setShortcut(QKeySequence::Quit);
    connect(exitAct, &QAction::triggered, qApp, &QApplication::quit);

    // ── View – window/level presets ───────────────────────────────────────
    auto* viewMenu = menuBar()->addMenu(tr("&View"));

    auto addPreset = [&](const QString& label, QKeySequence key, auto slot) {
        auto* act = viewMenu->addAction(label);
        act->setShortcut(key);
        connect(act, &QAction::triggered, this, slot);
    };

    addPreset(tr("Bone Window       (W:2000 / L:500)"),  Qt::Key_1, &MainWindow::onBoneWindow);
    addPreset(tr("Soft Tissue       (W:400  / L:40)"),   Qt::Key_2, &MainWindow::onSoftTissueWindow);
    addPreset(tr("Lung / Air        (W:1500 / L:-600)"), Qt::Key_3, &MainWindow::onLungWindow);
    addPreset(tr("Brain             (W:80   / L:40)"),   Qt::Key_4, &MainWindow::onBrainWindow);

    viewMenu->addSeparator();

    auto* resetCamAct = viewMenu->addAction(tr("Reset Camera"));
    resetCamAct->setShortcut(Qt::Key_R);
    connect(resetCamAct, &QAction::triggered, this, [this] {
        if (m_axialView)    m_axialView->resetCamera();
        if (m_coronalView)  m_coronalView->resetCamera();
        if (m_sagittalView) m_sagittalView->resetCamera();
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// buildToolBar — Mimics-style ribbon:
//
//   ┌─[FILE]─[IMAGE]─[MEASURE]─[SEGMENT]─[3D TOOLS]──────────────┐  ← module tab bar
//   ├─────────────────────────────────────────────────────────────┤
//   │  [buttons for the selected module only]                     │  ← QStackedWidget
//   └─────────────────────────────────────────────────────────────┘
//
// Clicking a module tab switches the stacked page — no other pages visible.
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::buildToolBar()
{
    auto* ribbon = addToolBar(tr("Ribbon"));
    ribbon->setMovable(false);
    ribbon->setFloatable(false);
    ribbon->setStyleSheet("QToolBar { background:#1c1c1e; border:0; padding:0; spacing:0; }");

    // ── Root widget ───────────────────────────────────────────────────────
    auto* rw   = new QWidget(ribbon);
    auto* vbox = new QVBoxLayout(rw);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    rw->setStyleSheet(
        // ── Module selector tabs (top row) ────────────────────────────────
        "QToolButton#MT { background:#1c1c1e; color:#a1a1a6; border:0;"
        "  border-right:1px solid #2c2c2e; padding:6px 26px;"
        "  font-size:12px; font-weight:700; min-height:34px; }"
        "QToolButton#MT:checked { color:#ffffff; background:#242426;"
        "  border-bottom:3px solid #0a84ff; }"
        "QToolButton#MT:hover   { color:#ffffff; background:#2c2c2e; }"
        // ── Content ribbon buttons (stacked pages) ────────────────────────
        "QToolButton { color:#f5f5f7; background:transparent;"
        "  border:1px solid transparent; border-radius:12px;"
        "  padding:5px 10px; font-size:11px;"
        "  min-width:56px; min-height:54px; }"
        "QToolButton:hover   { background:#2c2c2e; border-color:#3a3a3c; }"
        "QToolButton:checked { background:#1f3b57; border-color:#0a84ff; color:#ffffff; }"
        "QToolButton:pressed { background:#3a3a3c; }"
        "QToolButton:disabled { color:#5f5f63; background:transparent; border-color:transparent; }"
        "QToolButton[guideState=\"next\"] { background:#6e6257; border-color:#f5d7ad;"
        "  color:#ffffff; font-weight:700; }"
        "QToolButton[guideState=\"next\"]:disabled { background:#4b4038; border-color:#8d765f;"
        "  color:#ffffff; font-weight:700; }"
        "QToolButton[guideState=\"done\"] { background:#24342a; border-color:#34c759;"
        "  color:#d8f8df; font-weight:700; }");

    // ── Tab bar (module names) ────────────────────────────────────────────
    auto* tabBar    = new QWidget(rw);
    auto* tabLayout = new QHBoxLayout(tabBar);
    tabLayout->setContentsMargins(0, 0, 0, 0);
    tabLayout->setSpacing(0);
    tabBar->setFixedHeight(34);
    tabBar->setStyleSheet("background:#1c1c1e; border-bottom:1px solid #2c2c2e;");
    vbox->addWidget(tabBar);

    // ── ORTOGNÁTICA step bar, above the actions of the current step ───────
    vbox->addWidget(buildOrthognathicStepPanel(rw));

    // ── Stacked content area ──────────────────────────────────────────────
    auto* stack = new QStackedWidget(rw);
    stack->setFixedHeight(82);
    stack->setStyleSheet(
        "QStackedWidget { background:#242426; border-top:1px solid #2c2c2e; border-bottom:1px solid #2c2c2e; }");
    vbox->addWidget(stack);

    ribbon->addWidget(rw);

    // Only one tab checked at a time
    auto* tabGroup = new QButtonGroup(rw);
    tabGroup->setExclusive(true);

    // Brand logo removed for branding removal
    // (Pestañas se alinean directamente a la izquierda)

    // ── Factory: add a module tab + its empty content page ───────────────
    // Returns {page widget, its HBoxLayout} so the caller populates it.
    auto addModule = [&](const QString& title) -> std::pair<QWidget*, QHBoxLayout*> {
        auto* tab = new QToolButton(tabBar);
        tab->setObjectName("MT");
        tab->setText(title);
        tab->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
        tabLayout->addWidget(tab);
        // Planning modules are ORTOGNÁTICA steps: their tab stays hidden and is clicked from the step rail.
        if (orthognathicStepTitles().contains(title)) {
            tab->setVisible(false);
        } else {
            tab->setCheckable(true);
            tabGroup->addButton(tab);
        }

        auto* page   = new QWidget(stack);
        auto* layout = new QHBoxLayout(page);
        layout->setContentsMargins(8, 4, 8, 4);
        layout->setSpacing(6);
        stack->addWidget(page);

        const int idx = stack->count() - 1;
        connect(tab, &QToolButton::clicked, this, [this, stack, idx, title] {
            if (title != tr("SEGMENTACION")) cancelBoneCavityFill();
            stack->setCurrentIndex(idx);
            const bool models     = title == tr("MODELOS");
            const bool orientation = title == tr("ORIENTACION");
            const bool osteotomy  = title == tr("OSTEOTOMIA");
            const bool biteRegistration = title == tr("REGISTRO MORDIDA");
            const bool reposition = title == tr("REPOSICIÓN");
            const bool splint     = title == tr("FERULA");
            const bool guides     = title == tr("GUIAS");
            setModelsWorkspace(models);
            setOrientationWorkspace(orientation);
            setOsteotomyWorkspace(osteotomy);
            setBiteRegistrationWorkspace(biteRegistration);
            setRepositionWorkspace(reposition);
            setSplintWorkspace(splint);
            setGuidesWorkspace(guides);
            onModuleTabActivated(title);
        });
        if (idx == 0) { tab->setChecked(true); stack->setCurrentIndex(0); }

        return {page, layout};
    };

    // ── Wraps a QAction as a ribbon button ────────────────────────────────
    auto makeActBtn = [](QAction* act, QWidget* parent) {
        auto* b = new QToolButton(parent);
        b->setDefaultAction(act);
        b->setProperty("guideState", act ? act->property("guideState") : QVariant());
        b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        b->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
        return b;
    };

    // ── Thin vertical separator between button groups ─────────────────────
    auto addSep = [](QHBoxLayout* row, QWidget* parent) {
        auto* f = new QFrame(parent);
        f->setFrameShape(QFrame::VLine);
        f->setFrameShadow(QFrame::Plain);
        f->setFixedWidth(1);
        f->setStyleSheet("QFrame { color:#2c2c2e; margin:6px 2px; }");
        row->addWidget(f);
    };

    // ════════════════════════════════════════════════════════════════════════
    // FILE
    // ════════════════════════════════════════════════════════════════════════
    {
        auto mod  = addModule(tr("ARCHIVO"));
        auto* page = mod.first;
        auto* row  = mod.second;
        if (m_openAction)
            row->addWidget(makeActBtn(m_openAction, page));
        addSep(row, page);

        m_saveProjectAct = new QAction(tr("Guardar\nProyecto"), this);
        m_saveProjectAct->setShortcut(QKeySequence::Save);
        m_saveProjectAct->setEnabled(false);
        connect(m_saveProjectAct, &QAction::triggered, this, &MainWindow::onSaveProject);
        row->addWidget(makeActBtn(m_saveProjectAct, page));

        auto* openProjAct = new QAction(tr("Abrir\nProyecto"), this);
        connect(openProjAct, &QAction::triggered, this, &MainWindow::onOpenProject);
        row->addWidget(makeActBtn(openProjAct, page));

        m_exportLogAct = new QAction(tr("Log\nClínico"), this);
        connect(m_exportLogAct, &QAction::triggered, this, &MainWindow::exportClinicalLog);
        row->addWidget(makeActBtn(m_exportLogAct, page));

        addSep(row, page);

        auto* bonePresetAct = new QAction(tr("Hueso"), this);
        bonePresetAct->setToolTip(tr("Ventana para cortical osea y estructuras mineralizadas."));
        connect(bonePresetAct, &QAction::triggered, this, &MainWindow::onBoneWindow);
        row->addWidget(makeActBtn(bonePresetAct, page));

        auto* softPresetAct = new QAction(tr("Tejidos\nblandos"), this);
        softPresetAct->setToolTip(tr("Ventana para tejidos blandos faciales."));
        connect(softPresetAct, &QAction::triggered, this, &MainWindow::onSoftTissueWindow);
        row->addWidget(makeActBtn(softPresetAct, page));

        auto* brainPresetAct = new QAction(tr("Cerebro"), this);
        brainPresetAct->setToolTip(tr("Ventana para parenquima cerebral."));
        connect(brainPresetAct, &QAction::triggered, this, &MainWindow::onBrainWindow);
        row->addWidget(makeActBtn(brainPresetAct, page));

        row->addStretch(1);
    }

    // ════════════════════════════════════════════════════════════════════════
    // IMAGE
    // ════════════════════════════════════════════════════════════════════════
    if constexpr (false) {
        auto  mod  = addModule(tr("IMAGE"));
        auto* page = mod.first;
        auto* row  = mod.second;
        auto addPreset = [&](const QString& text, auto slot, const QString& tooltip) {
            auto* act = new QAction(text, this);
            act->setToolTip(tooltip);
            connect(act, &QAction::triggered, this, slot);
            row->addWidget(makeActBtn(act, page));
            return act;
        };
        addPreset(tr("Hueso"), &MainWindow::onBoneWindow,
                  tr("Ventana para cortical osea y estructuras mineralizadas."));
        addPreset(tr("Tejido"), &MainWindow::onSoftTissueWindow,
                  tr("Ventana para tejidos blandos faciales."));

        auto* dentalAct = new QAction(tr("Dental"), this);
        dentalAct->setToolTip(tr("Ventana de alta densidad para dientes y restauraciones."));
        connect(dentalAct, &QAction::triggered, this, [this] {
            const WindowLevelPreset dental{3500.0, 1200.0, "Dental"};
            broadcastPreset(dental);
        });
        row->addWidget(makeActBtn(dentalAct, page));

        addSep(row, page);
        addPreset(tr("Avz\nPulmon"), &MainWindow::onLungWindow,
                  tr("Preset avanzado para aire y via aerea."));
        addPreset(tr("Avz\nCerebro"), &MainWindow::onBrainWindow,
                  tr("Preset avanzado para parenquima cerebral."));
        addSep(row, page);
        auto* resetAct = new QAction(tr("Reset\nCam"), this);
        connect(resetAct, &QAction::triggered, this, [this] {
            if (m_axialView)    m_axialView->resetCamera();
            if (m_coronalView)  m_coronalView->resetCamera();
            if (m_sagittalView) m_sagittalView->resetCamera();
        });
        row->addWidget(makeActBtn(resetAct, page));
        row->addStretch(1);
    }

    // ════════════════════════════════════════════════════════════════════════
    // MEASURE
    // ════════════════════════════════════════════════════════════════════════
    {
        auto  mod  = addModule(tr("MEDIDAS"));
        auto* page = mod.first;
        auto* row  = mod.second;
        auto* toolGroup = new QActionGroup(this);
        toolGroup->setExclusive(true);

        auto addTool = [&](const QString& text, MeasurementToolMode mode,
                           const QKeySequence& key, bool checked = false) {
            auto* act = new QAction(text, this);
            act->setCheckable(true);
            act->setShortcut(key);
            toolGroup->addAction(act);
            addAction(act);
            connect(act, &QAction::triggered, this,
                    [this, mode] { setMeasurementTool(mode); });
            row->addWidget(makeActBtn(act, page));
            if (checked) act->setChecked(true);
        };
        addTool(tr("Cursor"),  MeasurementToolMode::Cursor,               Qt::Key_C, true);
        addTool(tr("Dist"),    MeasurementToolMode::Distance,             Qt::Key_D);
        addTool(tr("Angulo"),  MeasurementToolMode::Angle,                Qt::Key_A);
        addTool(tr("Area"),    MeasurementToolMode::Area,                 Qt::Key_P);
        addTool(tr("ROI○"),    MeasurementToolMode::ROICircle,            Qt::Key_O);
        addTool(tr("ROI□"),    MeasurementToolMode::ROIRectangle,         Qt::Key_I);
        addTool(tr("Perp"),    MeasurementToolMode::PerpendicularDistance, Qt::Key_L);
        addTool(tr("Nota"),    MeasurementToolMode::Annotation,           Qt::Key_N);
        addSep(row, page);

        auto* delAct = new QAction(tr("Eliminar"), this);
        delAct->setShortcut(QKeySequence::Delete);
        addAction(delAct);
        connect(delAct, &QAction::triggered, this, &MainWindow::deleteSelectedMeasurement);
        row->addWidget(makeActBtn(delAct, page));

        m_toggleMeasurementsAct = new QAction(tr("Mostrar\nMed."), this);
        m_toggleMeasurementsAct->setCheckable(true);
        m_toggleMeasurementsAct->setChecked(true);
        m_toggleMeasurementsAct->setToolTip(tr("Mostrar/Ocultar medidas."));
        connect(m_toggleMeasurementsAct, &QAction::toggled, this, [this](bool v) {
            m_toggleMeasurementsAct->setText(v ? tr("Mostrar\nMed.") : tr("Ocultar\nMed."));
            m_measurementManager.setAllVisible(v);
        });
        row->addWidget(makeActBtn(m_toggleMeasurementsAct, page));
        addSep(row, page);

        auto* saveAct = new QAction(tr("Guardar\nJSON"), this);
        saveAct->setToolTip(tr("Exportar mediciones JSON."));
        connect(saveAct, &QAction::triggered, this, &MainWindow::saveMeasurements);
        row->addWidget(makeActBtn(saveAct, page));

        auto* loadAct = new QAction(tr("Cargar\nJSON"), this);
        loadAct->setToolTip(tr("Importar mediciones JSON."));
        connect(loadAct, &QAction::triggered, this, &MainWindow::loadMeasurements);
        row->addWidget(makeActBtn(loadAct, page));
        row->addStretch(1);
    }

    // ════════════════════════════════════════════════════════════════════════
    // SEGMENT — clicking Hueso or Tejido segments AND auto-generates the mesh
    // ════════════════════════════════════════════════════════════════════════
    {
        auto  mod  = addModule(tr("SEGMENTACION"));
        auto* page = mod.first;
        auto* row  = mod.second;

        // Segment + auto-mesh: stores the label so onSegmentationFinished()
        // knows which mesh to generate without any user interaction.
        auto addSeg = [&](const QString& text, SegmentationTarget target, int label) -> QAction* {
            auto* act = new QAction(text, this);
            connect(act, &QAction::triggered, this, [this, target, label] {
                m_pendingAutoMeshLabel = label;   // onSegmentationFinished reads this
                startAISegmentation(target);
            });
            row->addWidget(makeActBtn(act, page));
            m_autoSegmentationAct = act;
            return act;
        };
        addSeg(tr("Segmentación\nAutomática"), SegmentationTarget::FullAuto, -3);

        m_manualAirwayAct = new QAction(tr("Vía Aérea\nManual"), this);
        connect(m_manualAirwayAct, &QAction::triggered, this, [this] {
            if (!m_volume) {
                QMessageBox::warning(this, tr("Vía Aérea Manual"), tr("Primero cargue un volumen DICOM."));
                return;
            }
            deactivateLassoTools();
            setMeasurementTool(MeasurementToolMode::Cursor);
            m_airwayPointCaptureStep = 1;
            for (MPRView* view : {m_axialView, m_coronalView, m_sagittalView}) {
                if (view) {
                    view->setMeasurementPickingEnabled(true);
                }
            }
            statusBar()->showMessage(tr("Vía Aérea Manual: haga clic en el LÍMITE SUPERIOR (Nasofaringe) de la vía aérea..."));
            showAirwayDialog(1);
        });
        row->addWidget(makeActBtn(m_manualAirwayAct, page));
        addSep(row, page);

        m_toggleSegmentationOverlayAct = new QAction(tr("Overlay\nSEG"), this);
        m_toggleSegmentationOverlayAct->setCheckable(true);
        m_toggleSegmentationOverlayAct->setChecked(true);
        connect(m_toggleSegmentationOverlayAct, &QAction::toggled,
                this, [this](bool) { refreshSegmentationOverlays(); });
        row->addWidget(makeActBtn(m_toggleSegmentationOverlayAct, page));
        addSep(row, page);

        // ── Bone splitter ────────────────────────────────────────────────
        m_splitAct = new QAction(tr("Max /\nMand."), this);
        m_splitAct->setEnabled(false);
        m_splitAct->setToolTip(
            tr("Dividir la máscara de hueso en Maxilar (label 5) y "
               "Mandíbula (label 6): mandíbula aislada, resto del hueso como maxilar/cráneo."));
        connect(m_splitAct, &QAction::triggered, this, [this] {
            if (!m_segmentationLabelmap) {
                QMessageBox::warning(this, tr("Max/Mand"),
                    tr("Primero ejecute la segmentación de hueso."));
                return;
            }
            if (m_boneSplitter && m_boneSplitter->isRunning()) return;
            m_splitAct->setEnabled(false);
            m_boneSplitter->split(m_segmentationLabelmap, m_segmentationOutputDir);
            // Re-enable when done (connected via onBoneSplitFinished/Error)
            auto* guard = m_boneSplitter;
            connect(guard, &BoneSplitterService::splitFinished, m_splitAct,
                    [this] { updateButtonStates(); },
                    Qt::SingleShotConnection);
            connect(guard, &BoneSplitterService::errorOccurred, m_splitAct,
                    [this] { updateButtonStates(); },
                    Qt::SingleShotConnection);
        });
        row->addWidget(makeActBtn(m_splitAct, page));
        addSep(row, page);

        // ── Lasso edit tools ─────────────────────────────────────────────
        // These are checkable toggle buttons (one active at a time).
        // When active they switch all MPR views into lasso drawing mode.
        auto makeLassoBtn = [&](const QString& text, bool addMode) {
            auto* btn = new QToolButton(page);
            btn->setText(text);
            btn->setCheckable(true);
            btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
            btn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
            btn->setToolTip(addMode
                ? tr("Lasso Agregar: dibuja para añadir voxeles a la máscara seleccionada")
                : tr("Lasso Quitar: dibuja para eliminar voxeles de la máscara seleccionada"));
            row->addWidget(btn);
            return btn;
        };

        m_lassoAddButton = makeLassoBtn(tr("Lasso\n+"), true);
        m_lassoSubButton = makeLassoBtn(tr("Lasso\n−"), false);

        m_fillBoneCavityAct = new QAction(tr("Rellenar\ncavidad"), this);
        m_fillBoneCavityAct->setObjectName("FillBoneCavityAction");
        m_fillBoneCavityAct->setCheckable(true);
        m_fillBoneCavityAct->setEnabled(false);
        m_fillBoneCavityAct->setToolTip(tr("Seleccionar una cavidad en un corte 2D de la máscara ósea y previsualizar su relleno."));
        connect(m_fillBoneCavityAct, &QAction::toggled, this, [this](bool checked) {
            if (checked) beginBoneCavityFill();
            else cancelBoneCavityFill();
        });
        row->addWidget(makeActBtn(m_fillBoneCavityAct, page));

        // Mutual exclusion: checking one unchecks the other and updates all views.
        // "Lasso +" works on 2-D slices only (adding geometry in 3D has no meaning).
        // "Lasso −" works on both 2-D slices (labelmap edit) AND the 3-D mesh view.
        connect(m_lassoAddButton, &QToolButton::toggled, this,
                [this](bool checked) {
            if (checked && m_lassoSubButton) m_lassoSubButton->setChecked(false);
            if (checked) cancelBoneCavityFill();
            if (checked) {
                m_dentalPointSet = DentalPointSet::None;
                if (m_mesh3DView) m_mesh3DView->setPointPickMode(false);
            }
            if (m_axialView)    m_axialView->setLassoMode(checked, true);
            if (m_coronalView)  m_coronalView->setLassoMode(checked, true);
            if (m_sagittalView) m_sagittalView->setLassoMode(checked, true);
            // Disable 3D lasso when switching to add mode
            if (m_mesh3DView)   m_mesh3DView->setLassoEraseMode(false);
        });
        connect(m_lassoSubButton, &QToolButton::toggled, this,
                [this](bool checked) {
            if (checked && m_lassoAddButton) m_lassoAddButton->setChecked(false);
            if (checked) cancelBoneCavityFill();
            if (checked) {
                m_dentalPointSet = DentalPointSet::None;
                if (m_mesh3DView) m_mesh3DView->setPointPickMode(false);
            }
            if (m_axialView)    m_axialView->setLassoMode(checked, false);
            if (m_coronalView)  m_coronalView->setLassoMode(checked, false);
            if (m_sagittalView) m_sagittalView->setLassoMode(checked, false);
            if (m_mesh3DView)   m_mesh3DView->setLassoEraseMode(checked);
        });

        row->addStretch(1);
    }

    // ════════════════════════════════════════════════════════════════════════
    // MODELOS DENTALES
    // ════════════════════════════════════════════════════════════════════════
    {
        auto  mod  = addModule(tr("MODELOS"));
        auto* page = mod.first;
        // The MODELOS actions live in the guided panel on the left of the workspace (MainWindowModels.cpp);
        // the ribbon only points to it and the buttons created below stay in a hidden row.
        auto* modelHint = new QLabel(tr("Siga los pasos en el panel izquierdo de Modelos."), page);
        modelHint->setStyleSheet("color:#98989d; font-size:11px; padding-left:8px;");
        mod.second->addWidget(modelHint);
        mod.second->addStretch(1);
        auto* hiddenRibbon = new QWidget(page);
        hiddenRibbon->setVisible(false);
        auto* row = new QHBoxLayout(hiddenRibbon);

        m_modelBackAct = new QAction(tr("Atrás: rehacer el paso"), this);
        m_modelBackAct->setEnabled(false);
        m_modelBackAct->setToolTip(tr("Volver al paso anterior del flujo de modelos."));
        connect(m_modelBackAct, &QAction::triggered, this, &MainWindow::goBackModelWorkflow);
        row->addWidget(makeActBtn(m_modelBackAct, page));
        addSep(row, page);

        m_importUpperAct = new QAction(tr("Cargar STL superior"), this);
        m_importUpperAct->setEnabled(false);
        m_importUpperAct->setToolTip(tr("Importar escaneo intraoral superior."));
        connect(m_importUpperAct, &QAction::triggered, this, &MainWindow::importUpperArchStl);
        row->addWidget(makeActBtn(m_importUpperAct, page));

        m_importLowerAct = new QAction(tr("Cargar STL inferior"), this);
        m_importLowerAct->setEnabled(false);
        m_importLowerAct->setToolTip(tr("Importar escaneo intraoral inferior."));
        connect(m_importLowerAct, &QAction::triggered, this, &MainWindow::importLowerArchStl);
        row->addWidget(makeActBtn(m_importLowerAct, page));
        addSep(row, page);

        m_maxPtsAct = new QAction(tr("Puntos en el maxilar (TAC)"), this);
        m_maxPtsAct->setCheckable(true);
        m_maxPtsAct->setEnabled(false);
        m_maxPtsAct->setToolTip(tr("Seleccionar puntos homologos en el maxilar segmentado."));
        connect(m_maxPtsAct, &QAction::triggered, this,
                [this](bool checked) {
                    setDentalPointCapture(checked ? DentalPointSet::MaxillaBone : DentalPointSet::None);
                });
        row->addWidget(makeActBtn(m_maxPtsAct, page));

        m_upperPtsAct = new QAction(tr("Puntos en el STL superior"), this);
        m_upperPtsAct->setCheckable(true);
        m_upperPtsAct->setEnabled(false);
        m_upperPtsAct->setToolTip(tr("Seleccionar puntos homologos en el STL superior."));
        connect(m_upperPtsAct, &QAction::triggered, this,
                [this](bool checked) {
                    setDentalPointCapture(checked ? DentalPointSet::UpperArch : DentalPointSet::None);
                });
        row->addWidget(makeActBtn(m_upperPtsAct, page));

        m_matchUpperAct = new QAction(tr("Registrar STL superior"), this);
        m_matchUpperAct->setEnabled(false);
        m_matchUpperAct->setToolTip(tr("Registrar STL superior contra maxilar segmentado."));
        connect(m_matchUpperAct, &QAction::triggered, this, &MainWindow::alignUpperArchToMaxilla);
        row->addWidget(makeActBtn(m_matchUpperAct, page));
        addSep(row, page);

        m_mandPtsAct = new QAction(tr("Puntos en la mandíbula (TAC)"), this);
        m_mandPtsAct->setCheckable(true);
        m_mandPtsAct->setEnabled(false);
        m_mandPtsAct->setToolTip(tr("Seleccionar puntos homologos en la mandibula segmentada."));
        connect(m_mandPtsAct, &QAction::triggered, this,
                [this](bool checked) {
                    setDentalPointCapture(checked ? DentalPointSet::MandibleBone : DentalPointSet::None);
                });
        row->addWidget(makeActBtn(m_mandPtsAct, page));

        m_lowerPtsAct = new QAction(tr("Puntos en el STL inferior"), this);
        m_lowerPtsAct->setCheckable(true);
        m_lowerPtsAct->setEnabled(false);
        m_lowerPtsAct->setToolTip(tr("Seleccionar puntos homologos en el STL inferior."));
        connect(m_lowerPtsAct, &QAction::triggered, this,
                [this](bool checked) {
                    setDentalPointCapture(checked ? DentalPointSet::LowerArch : DentalPointSet::None);
                });
        row->addWidget(makeActBtn(m_lowerPtsAct, page));

        m_matchLowerAct = new QAction(tr("Registrar STL inferior"), this);
        m_matchLowerAct->setEnabled(false);
        m_matchLowerAct->setToolTip(tr("Registrar STL inferior contra mandibula segmentada."));
        connect(m_matchLowerAct, &QAction::triggered, this, &MainWindow::alignLowerArchToMandible);
        row->addWidget(makeActBtn(m_matchLowerAct, page));
        addSep(row, page);

        m_matchBothAct = new QAction(tr("Registrar ambos STL"), this);
        m_matchBothAct->setEnabled(false);
        m_matchBothAct->setToolTip(tr("Registrar STL superior e inferior cuando ambas arcadas tienen puntos suficientes."));
        connect(m_matchBothAct, &QAction::triggered, this, &MainWindow::alignBothDentalArches);
        row->addWidget(makeActBtn(m_matchBothAct, page));
        addSep(row, page);

        m_adjustArchAct = new QAction(tr("Ajustar escaneo (gizmo)"), this);
        m_adjustArchAct->setEnabled(false);
        m_adjustArchAct->setToolTip(tr("Activar gizmo para ajustar manualmente el STL registrado antes de crear el compuesto."));
        connect(m_adjustArchAct, &QAction::triggered, this, [this] {
            const int step = m_modelStepStack ? m_modelStepStack->currentIndex() : 0;
            if (step == 0 && m_upperArchMesh && m_upperRegistrationCalculated) {
                startDentalAdjustmentGizmo(objectActorKey(kUpperArchLabel));
            } else if (step == 1 && m_lowerArchMesh && m_lowerRegistrationCalculated) {
                startDentalAdjustmentGizmo(objectActorKey(kLowerArchLabel));
            } else {
                statusBar()->showMessage(tr("Primero registra la arcada activa para habilitar el ajuste manual."));
            }
        });
        row->addWidget(makeActBtn(m_adjustArchAct, page));

        m_acceptGizmoAct = new QAction(tr("Aceptar ajuste"), this);
        m_acceptGizmoAct->setEnabled(false);
        m_acceptGizmoAct->setToolTip(tr("Confirmar el ajuste manual del gizmo y actualizar el STL registrado."));
        connect(m_acceptGizmoAct, &QAction::triggered, this,
                [this] { commitActiveDentalGizmos(true); });
        row->addWidget(makeActBtn(m_acceptGizmoAct, page));

        m_compositeAct = new QAction(tr("Crear modelo compuesto"), this);
        m_compositeAct->setEnabled(false);
        m_compositeAct->setToolTip(tr("Crear modelo compuesto usando registros aceptados."));
        connect(m_compositeAct, &QAction::triggered, this, &MainWindow::createDentalCompositeModels);
        row->addWidget(makeActBtn(m_compositeAct, page));

        m_continueNoMatchAct = new QAction(tr("Continuar sin escaneo"), this);
        m_continueNoMatchAct->setEnabled(false);
        m_continueNoMatchAct->setToolTip(
            tr("Pasar a orientacion usando solo las mallas TAC disponibles, sin STL intraoral."));
        connect(m_continueNoMatchAct, &QAction::triggered,
                this, &MainWindow::continueToOrientationWithoutMatch);
        row->addWidget(makeActBtn(m_continueNoMatchAct, page));

        m_exportAct = new QAction(tr("Exportar STL"), this);
        m_exportAct->setEnabled(false);
        m_exportAct->setToolTip(tr("Exportar el objeto seleccionado en coordenadas fisicas."));
        connect(m_exportAct, &QAction::triggered, this, &MainWindow::exportDentalCompositeStl);
        row->addWidget(makeActBtn(m_exportAct, page));

        m_exportPackAct = new QAction(tr("Exportar registro"), this);
        m_exportPackAct->setEnabled(false);
        m_exportPackAct->setToolTip(tr("Exportar matrices y metricas del registro."));
        connect(m_exportPackAct, &QAction::triggered, this, &MainWindow::exportDentalRegistrationPackage);
        row->addWidget(makeActBtn(m_exportPackAct, page));

        m_clearPtsAct = new QAction(tr("Borrar todos los puntos"), this);
        m_clearPtsAct->setEnabled(false);
        m_clearPtsAct->setToolTip(tr("Eliminar todos los puntos de registro."));
        connect(m_clearPtsAct, &QAction::triggered, this, [this] {
            const auto ret = QMessageBox::question(
                this, tr("Limpiar puntos"),
                tr("¿Eliminar todos los puntos de registro?"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (ret == QMessageBox::Yes)
                clearDentalRegistrationPoints();
        });
        row->addWidget(makeActBtn(m_clearPtsAct, page));

        m_resetArchAct = new QAction(tr("Restaurar STL originales"), this);
        m_resetArchAct->setEnabled(false);
        m_resetArchAct->setToolTip(tr("Restaurar STL importados y descartar registros aceptados."));
        connect(m_resetArchAct, &QAction::triggered, this, [this] {
            const auto ret = QMessageBox::question(
                this, tr("Reset STL"),
                tr("¿Restaurar los STL originales y descartar registros/composites actuales?"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (ret == QMessageBox::Yes)
                resetDentalArchTransforms();
        });
        row->addWidget(makeActBtn(m_resetArchAct, page));
        row->addStretch(1);
    }

    // ════════════════════════════════════════════════════════════════════════
    // ORIENTACION — Frankfurt plane alignment before osteotomies
    // ════════════════════════════════════════════════════════════════════════
    {
        auto  mod  = addModule(tr("ORIENTACION"));
        auto* page = mod.first;
        // The ORIENTACION actions live in the guided panel on the left (MainWindowOrientation.cpp).
        auto* orientationHint = new QLabel(tr("Siga los pasos en el panel izquierdo de Orientación."), page);
        orientationHint->setStyleSheet("color:#98989d; font-size:11px; padding-left:8px;");
        mod.second->addWidget(orientationHint);
        mod.second->addStretch(1);
        auto* hiddenOrientationRibbon = new QWidget(page);
        hiddenOrientationRibbon->setVisible(false);
        auto* row = new QHBoxLayout(hiddenOrientationRibbon);

        // Sequential landmarks: each enabled only after the previous is placed.
        m_frankfortPorionDAct = new QAction(tr("Porion derecho"), this);
        m_frankfortPorionDAct->setEnabled(false);
        m_frankfortPorionDAct->setCheckable(true);
        m_frankfortPorionDAct->setToolTip(tr("Marcar Porion derecho — vista lateral derecha."));
        connect(m_frankfortPorionDAct, &QAction::triggered, this, [this] {
            m_frankfortPorionDAct->setChecked(false);  // uncheck while picking; re-checked on point placed
            m_frankfurtCapturingIdx = 0;
            if (m_orientationView) {
                // Index 1 = camera at -X = patient's right side visible
                m_orientationView->setStandardView(1);
                m_orientationView->setPointPickMode(true);
            }
            statusBar()->showMessage(tr("Marque el Porion derecho en la vista lateral derecha."));
        });
        row->addWidget(makeActBtn(m_frankfortPorionDAct, page));

        m_frankfortPorionIAct = new QAction(tr("Porion izquierdo"), this);
        m_frankfortPorionIAct->setEnabled(false);
        m_frankfortPorionIAct->setCheckable(true);
        m_frankfortPorionIAct->setToolTip(tr("Marcar Porion izquierdo — vista lateral izquierda."));
        connect(m_frankfortPorionIAct, &QAction::triggered, this, [this] {
            m_frankfortPorionIAct->setChecked(false);
            m_frankfurtCapturingIdx = 1;
            if (m_orientationView) {
                // Index 2 = camera at +X = patient's left side visible
                m_orientationView->setStandardView(2);
                m_orientationView->setPointPickMode(true);
            }
            statusBar()->showMessage(tr("Marque el Porion izquierdo en la vista lateral izquierda."));
        });
        row->addWidget(makeActBtn(m_frankfortPorionIAct, page));

        m_frankfortOrbitalDAct = new QAction(tr("Orbitale derecho"), this);
        m_frankfortOrbitalDAct->setEnabled(false);
        m_frankfortOrbitalDAct->setCheckable(true);
        m_frankfortOrbitalDAct->setToolTip(tr("Marcar Orbitale derecho — vista frontal."));
        connect(m_frankfortOrbitalDAct, &QAction::triggered, this, [this] {
            m_frankfortOrbitalDAct->setChecked(false);
            m_frankfurtCapturingIdx = 2;
            if (m_orientationView) {
                m_orientationView->setStandardView(0);  // frontal
                m_orientationView->setPointPickMode(true);
            }
            statusBar()->showMessage(tr("Marque el Orbitale derecho en la vista frontal."));
        });
        row->addWidget(makeActBtn(m_frankfortOrbitalDAct, page));

        m_frankfortOrbitalIAct = new QAction(tr("Orbitale izquierdo"), this);
        m_frankfortOrbitalIAct->setEnabled(false);
        m_frankfortOrbitalIAct->setCheckable(true);
        m_frankfortOrbitalIAct->setToolTip(tr("Marcar Orbitale izquierdo — vista frontal."));
        connect(m_frankfortOrbitalIAct, &QAction::triggered, this, [this] {
            m_frankfortOrbitalIAct->setChecked(false);
            m_frankfurtCapturingIdx = 3;
            if (m_orientationView) {
                m_orientationView->setStandardView(0);  // frontal
                m_orientationView->setPointPickMode(true);
            }
            statusBar()->showMessage(tr("Marque el Orbitale izquierdo en la vista frontal."));
        });
        row->addWidget(makeActBtn(m_frankfortOrbitalIAct, page));

        addSep(row, page);

        m_alignFrankfurtAct = new QAction(tr("Alinear al plano de Frankfort"), this);
        m_alignFrankfurtAct->setEnabled(false);
        m_alignFrankfurtAct->setToolTip(tr("Rotar craneo para que el plano de Frankfort sea horizontal."));
        connect(m_alignFrankfurtAct, &QAction::triggered, this, &MainWindow::alignFrankfurtPlane);
        row->addWidget(makeActBtn(m_alignFrankfurtAct, page));

        addSep(row, page);

        // Midline fine-tune via interactive gizmo (same mechanism as MODELOS).
        m_midlineGizmoAct = new QAction(tr("Ajustar línea media (gizmo)"), this);
        m_midlineGizmoAct->setEnabled(false);
        m_midlineGizmoAct->setToolTip(tr("Activar gizmo para ajustar rotacion de linea media en vista frontal."));
        connect(m_midlineGizmoAct, &QAction::triggered, this, [this] {
            if (!m_orientationView) return;
            if (m_orientationView->hasGizmo()) return;

            // Build a combined mesh (upper + lower) so the gizmo moves everything together
            auto append = vtkSmartPointer<vtkAppendPolyData>::New();
            bool hasAny = false;
            if (m_upperCompositeMesh) { append->AddInputData(m_upperCompositeMesh); hasAny = true; }
            if (m_lowerCompositeMesh) { append->AddInputData(m_lowerCompositeMesh); hasAny = true; }
            if (!hasAny) return;
            append->Update();
            auto combined = vtkSmartPointer<vtkPolyData>::New();
            combined->DeepCopy(append->GetOutput());

            // Hide individual composites, show combined under gizmo label
            m_orientationView->addMesh(kOrientGizmoTempLabel, combined, tr("Compuesto (gizmo)"));
            const QColor combinedColor = meshLabelColor(kUpperCompositeLabel);
            m_orientationView->setMeshColor(kOrientGizmoTempLabel, combinedColor);
            if (m_upperCompositeMesh)
                m_orientationView->setMeshVisible(objectActorKey(kUpperCompositeLabel), false);
            if (m_lowerCompositeMesh)
                m_orientationView->setMeshVisible(objectActorKey(kLowerCompositeLabel), false);

            m_orientationView->setStandardView(0);  // frontal for midline adjustment
            m_orientationView->startGizmo(kOrientGizmoTempLabel);
            if (m_midlineAcceptGizmoAct) m_midlineAcceptGizmoAct->setEnabled(true);
            m_midlineGizmoAct->setEnabled(false);
            statusBar()->showMessage(tr("Gizmo activo — ajuste la linea media y pulse Aceptar Gizmo."));
        });
        row->addWidget(makeActBtn(m_midlineGizmoAct, page));

        m_midlineAcceptGizmoAct = new QAction(tr("Aceptar ajuste de línea media"), this);
        m_midlineAcceptGizmoAct->setEnabled(false);
        m_midlineAcceptGizmoAct->setToolTip(tr("Confirmar ajuste de linea media y aplicar la misma transformacion al modelo inferior."));
        connect(m_midlineAcceptGizmoAct, &QAction::triggered, this, [this] {
            if (!m_orientationView) return;
            m_orientationView->stopGizmo();  // emits gizmoMeshUpdated → onOrientationGizmoUpdated
            m_midlineAcceptGizmoAct->setEnabled(false);
            m_midlineGizmoAct->setEnabled(true);
            updateFrankfurtPointStatus();
        });
        row->addWidget(makeActBtn(m_midlineAcceptGizmoAct, page));

        addSep(row, page);

        m_saveOrientationAct = new QAction(tr("Guardar orientación y seguir"), this);
        m_saveOrientationAct->setEnabled(false);
        m_saveOrientationAct->setToolTip(tr("Guardar orientacion actual y pasar al modulo de Osteotomia."));
        connect(m_saveOrientationAct, &QAction::triggered, this, [this] {
            if (m_orientationView && m_orientationView->hasGizmo())
                m_orientationView->stopGizmo();
            if (!m_upperCompositeMesh && !m_lowerCompositeMesh) {
                QMessageBox::warning(this, tr("Orientacion"),
                                     tr("No hay modelos compuestos orientados para enviar a osteotomia."));
                return;
            }
            if (m_lowerCompositeMesh && m_lowerCompositeMesh->GetNumberOfPoints() > 0) {
                m_orientedInitialMandibleMeshForSplint = vtkSmartPointer<vtkPolyData>::New();
                m_orientedInitialMandibleMeshForSplint->DeepCopy(m_lowerCompositeMesh);
            }
            if (m_viewFullScreen)
                exitViewFullScreen();

            // Navigate to osteotomy workspace; meshes are already transformed in-place.
            for (auto* tab : findChildren<QToolButton*>(QStringLiteral("MT"))) {
                if (tab && tab->text() == tr("OSTEOTOMIA")) { tab->click(); break; }
            }
            setOsteotomyWorkspace(true);
            statusBar()->showMessage(tr("Orientacion guardada. Modulo de Osteotomia activado."));
        });
        row->addWidget(makeActBtn(m_saveOrientationAct, page));

        addSep(row, page);

        m_exportOrientedAct = new QAction(tr("Exportar modelos orientados"), this);
        m_exportOrientedAct->setEnabled(false);
        m_exportOrientedAct->setToolTip(tr("Exportar modelos compuestos orientados como STL."));
        connect(m_exportOrientedAct, &QAction::triggered, this, &MainWindow::exportOrientedCompositeStl);
        row->addWidget(makeActBtn(m_exportOrientedAct, page));

        row->addStretch(1);
    }

    // ════════════════════════════════════════════════════════════════════════
    // OSTEOTOMIA — Le Fort I  (landmark-defined cut plane)
    // ════════════════════════════════════════════════════════════════════════
    {
        auto  mod  = addModule(tr("OSTEOTOMIA"));
        auto* page = mod.first;
        auto* row  = mod.second;

        // ── Status label (point capture progress) ──────────────────────────
        m_leFortCutLabel = new QLabel(tr("Le Fort I  0/4 pts"), page);
        m_leFortCutLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        m_leFortCutLabel->setStyleSheet("color:#0a84ff; font-size:10px; padding:0 4px;");
        m_leFortCutLabel->setMinimumWidth(110);
        row->addWidget(m_leFortCutLabel);

        addSep(row, page);

        // ── 4 cephalometric landmark buttons ────────────────────────────────
        m_leFortPirDACt = new QAction(tr("Piriforme\nDer"), this);
        m_leFortPirDACt->setEnabled(false);
        m_leFortPirDACt->setCheckable(true);
        m_leFortPirDACt->setToolTip(tr("Marcar borde lateral derecho de la apertura piriforme."));
        connect(m_leFortPirDACt, &QAction::triggered, this, [this] {
            startLeFortPointCapture(0);
        });
        row->addWidget(makeActBtn(m_leFortPirDACt, page));

        m_leFortPirIAct = new QAction(tr("Piriforme\nIzq"), this);
        m_leFortPirIAct->setEnabled(false);
        m_leFortPirIAct->setCheckable(true);
        m_leFortPirIAct->setToolTip(tr("Marcar borde lateral izquierdo de la apertura piriforme."));
        connect(m_leFortPirIAct, &QAction::triggered, this, [this] {
            startLeFortPointCapture(1);
        });
        row->addWidget(makeActBtn(m_leFortPirIAct, page));

        m_leFortPilaxDAct = new QAction(tr("Pilax\nDer"), this);
        m_leFortPilaxDAct->setEnabled(false);
        m_leFortPilaxDAct->setCheckable(true);
        m_leFortPilaxDAct->setToolTip(tr("Marcar pilar maxilomalar derecho (contrafuerte cigomaticomaxilar)."));
        connect(m_leFortPilaxDAct, &QAction::triggered, this, [this] {
            startLeFortPointCapture(2);
        });
        row->addWidget(makeActBtn(m_leFortPilaxDAct, page));

        m_leFortPilaxIAct = new QAction(tr("Pilax\nIzq"), this);
        m_leFortPilaxIAct->setEnabled(false);
        m_leFortPilaxIAct->setCheckable(true);
        m_leFortPilaxIAct->setToolTip(tr("Marcar pilar maxilomalar izquierdo (contrafuerte cigomaticomaxilar)."));
        connect(m_leFortPilaxIAct, &QAction::triggered, this, [this] {
            startLeFortPointCapture(3);
        });
        row->addWidget(makeActBtn(m_leFortPilaxIAct, page));

        addSep(row, page);

        // ── Plane gizmo controls ─────────────────────────────────────────────
        m_leFortAdjustPlaneAct = new QAction(tr("Ajustar\nPlano"), this);
        m_leFortAdjustPlaneAct->setEnabled(false);
        m_leFortAdjustPlaneAct->setToolTip(
            tr("Mover/rotar el plano de corte interactivamente con el gizmo."));
        connect(m_leFortAdjustPlaneAct, &QAction::triggered, this, [this] {
            if (!m_osteotomyView) return;
            // Disable landmark picking while gizmo is active
            if (m_leFortPirDACt)    m_leFortPirDACt->setEnabled(false);
            if (m_leFortPirIAct)    m_leFortPirIAct->setEnabled(false);
            if (m_leFortPilaxDAct)  m_leFortPilaxDAct->setEnabled(false);
            if (m_leFortPilaxIAct)  m_leFortPilaxIAct->setEnabled(false);
            if (m_leFortSplitAct)   m_leFortSplitAct->setEnabled(false);
            if (m_leFortAdjustPlaneAct) m_leFortAdjustPlaneAct->setEnabled(false);
            if (m_leFortGuidePropsAct)  m_leFortGuidePropsAct->setEnabled(false);
            if (m_leFortAcceptPlaneAct) m_leFortAcceptPlaneAct->setEnabled(true);
            m_osteotomyView->startGizmo(kLeFortPlaneLabel);
            statusBar()->showMessage(
                tr("Gizmo activo: arrastra las flechas para mover el plano, los anillos para rotarlo. "
                   "Usa los cubos para cambiar tamano. Acepta cuando este en posicion."));
        });
        row->addWidget(makeActBtn(m_leFortAdjustPlaneAct, page));

        m_leFortGuidePropsAct = new QAction(tr("Prop.\nCorte"), this);
        m_leFortGuidePropsAct->setEnabled(false);
        m_leFortGuidePropsAct->setToolTip(
            tr("Editar ancho, grosor y extensiones derecha/izquierda de la guia Le Fort I."));
        connect(m_leFortGuidePropsAct, &QAction::triggered,
                this, &MainWindow::showLeFortGuidePropertiesDialog);
        row->addWidget(makeActBtn(m_leFortGuidePropsAct, page));

        m_leFortAcceptPlaneAct = new QAction(tr("Aceptar\nPlano"), this);
        m_leFortAcceptPlaneAct->setEnabled(false);
        m_leFortAcceptPlaneAct->setToolTip(
            tr("Confirmar la posicion actual del plano de corte."));
        connect(m_leFortAcceptPlaneAct, &QAction::triggered, this, [this] {
            if (m_osteotomyView) m_osteotomyView->stopGizmo();
            // onOsteotomyGizmoUpdated() will fire via gizmoMeshUpdated signal
        });
        row->addWidget(makeActBtn(m_leFortAcceptPlaneAct, page));

        addSep(row, page);

        // ── Object selector (exclusive checkable pair) ───────────────────────
        auto* targetGroup = new QActionGroup(this);
        targetGroup->setExclusive(true);

        m_leFortTargMaxAct = new QAction(tr("Maxilar"), this);
        m_leFortTargMaxAct->setCheckable(true);
        m_leFortTargMaxAct->setChecked(true);
        m_leFortTargMaxAct->setEnabled(false);
        m_leFortTargMaxAct->setToolTip(tr("Aplicar la osteotomia al modelo compuesto superior (maxilar)."));
        connect(m_leFortTargMaxAct, &QAction::triggered, this, [this] {
            m_leFortTargetLabel = kUpperCompositeLabel;
            m_bssoGuideReady = false;
            if (m_osteotomyView) m_osteotomyView->removeMesh(kBssoGuideLabel);
            updateOsteotomyWorkflowUi();
        });
        targetGroup->addAction(m_leFortTargMaxAct);
        row->addWidget(makeActBtn(m_leFortTargMaxAct, page));

        m_leFortTargMandAct = new QAction(tr("Mandibula"), this);
        m_leFortTargMandAct->setCheckable(true);
        m_leFortTargMandAct->setEnabled(false);
        m_leFortTargMandAct->setToolTip(tr("Aplicar la osteotomia al modelo compuesto inferior (mandibula)."));
        connect(m_leFortTargMandAct, &QAction::triggered, this, [this] {
            m_leFortTargetLabel = kLowerCompositeLabel;
            setBssoActiveSide(false);
            updateOsteotomyWorkflowUi();
        });
        targetGroup->addAction(m_leFortTargMandAct);
        row->addWidget(makeActBtn(m_leFortTargMandAct, page));

        addSep(row, page);

        // ── Dividir + Exportar ───────────────────────────────────────────────
        m_leFortSplitAct = new QAction(tr("Dividir"), this);
        m_leFortSplitAct->setEnabled(false);
        m_leFortSplitAct->setToolTip(tr("Aplicar la osteotomia activa: Le Fort I o BSSO."));
        connect(m_leFortSplitAct, &QAction::triggered, this, &MainWindow::executeOsteotomySplit);
        row->addWidget(makeActBtn(m_leFortSplitAct, page));

        addSep(row, page);

        m_leFortExportAct = new QAction(tr("Exportar\nSegmentos"), this);
        m_leFortExportAct->setEnabled(false);
        m_leFortExportAct->setToolTip(tr("Exportar base craneal, segmento Le Fort I y mandibula como STL."));
        connect(m_leFortExportAct, &QAction::triggered, this, &MainWindow::exportLeFortSegments);
        row->addWidget(makeActBtn(m_leFortExportAct, page));

        addSep(row, page);

        m_bssoStatusLabel = new QLabel(tr("BSSO Der  0/3 pts"), page);
        m_bssoStatusLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        m_bssoStatusLabel->setStyleSheet("color:#34c759; font-size:10px; padding:0 4px;");
        m_bssoStatusLabel->setMinimumWidth(100);
        row->addWidget(m_bssoStatusLabel);

        m_bssoAutoGuideAct = new QAction(tr("BSSO\nDer"), this);
        m_bssoAutoGuideAct->setEnabled(false);
        m_bssoAutoGuideAct->setCheckable(true);
        m_bssoAutoGuideAct->setToolTip(tr("Crear guia sagital de rama derecha sobre la mandibula."));
        connect(m_bssoAutoGuideAct, &QAction::triggered, this, [this] {
            setBssoActiveSide(false);
            updateBssoGuide();
            if (m_osteotomyView) {
                m_osteotomyView->setStandardView(0);
                m_osteotomyView->resetCamera();
            }
        });
        row->addWidget(makeActBtn(m_bssoAutoGuideAct, page));

        m_bssoLeftGuideAct = new QAction(tr("BSSO\nIzq"), this);
        m_bssoLeftGuideAct->setEnabled(false);
        m_bssoLeftGuideAct->setCheckable(true);
        m_bssoLeftGuideAct->setToolTip(tr("Crear guia sagital de rama izquierda sobre la mandibula."));
        connect(m_bssoLeftGuideAct, &QAction::triggered, this, [this] {
            setBssoActiveSide(true);
            updateBssoGuide();
            if (m_osteotomyView) {
                m_osteotomyView->setStandardView(0);
                m_osteotomyView->resetCamera();
            }
        });
        row->addWidget(makeActBtn(m_bssoLeftGuideAct, page));

        auto startBssoPick = [this](int idx, QAction* act, const QString& message) {
            if (act) act->setChecked(false);
            m_leFortCapturingIdx = -1;
            m_bssoCapturingIdx = idx;
            setBssoActiveSide(idx >= 3);
            m_bssoGuideReady = false;
            if (m_osteotomyView) m_osteotomyView->removeMesh(kBssoGuideLabel);
            updateBssoPointStatus();
            if (m_osteotomyView) m_osteotomyView->setPointPickMode(true);
            statusBar()->showMessage(message);
        };

        m_bssoRamusRightAct = new QAction(tr("Rama\nDer"), this);
        m_bssoRamusRightAct->setEnabled(false);
        m_bssoRamusRightAct->setCheckable(true);
        m_bssoRamusRightAct->setToolTip(tr("Marcar punto superior/posterior de la osteotomia sagital derecha."));
        connect(m_bssoRamusRightAct, &QAction::triggered, this, [this, startBssoPick] {
            startBssoPick(0, m_bssoRamusRightAct, tr("Marque el punto de rama derecha para BSSO."));
        });
        row->addWidget(makeActBtn(m_bssoRamusRightAct, page));

        m_bssoBodyRightAct = new QAction(tr("Cuerpo\nDer"), this);
        m_bssoBodyRightAct->setEnabled(false);
        m_bssoBodyRightAct->setCheckable(true);
        m_bssoBodyRightAct->setToolTip(tr("Marcar punto anterior/inferior del cuerpo mandibular derecho."));
        connect(m_bssoBodyRightAct, &QAction::triggered, this, [this, startBssoPick] {
            startBssoPick(1, m_bssoBodyRightAct, tr("Marque el punto de cuerpo mandibular derecho para BSSO."));
        });
        row->addWidget(makeActBtn(m_bssoBodyRightAct, page));

        m_bssoPlaneRightAct = new QAction(tr("Basal\nDer"), this);
        m_bssoPlaneRightAct->setEnabled(false);
        m_bssoPlaneRightAct->setCheckable(true);
        m_bssoPlaneRightAct->setToolTip(tr("Marcar punto basal derecho para definir el plano sagital."));
        connect(m_bssoPlaneRightAct, &QAction::triggered, this, [this, startBssoPick] {
            startBssoPick(2, m_bssoPlaneRightAct, tr("Marque el punto basal derecho para BSSO."));
        });
        row->addWidget(makeActBtn(m_bssoPlaneRightAct, page));

        m_bssoRamusLeftAct = new QAction(tr("Rama\nIzq"), this);
        m_bssoRamusLeftAct->setEnabled(false);
        m_bssoRamusLeftAct->setCheckable(true);
        m_bssoRamusLeftAct->setToolTip(tr("Marcar punto superior/posterior de la osteotomia sagital izquierda."));
        connect(m_bssoRamusLeftAct, &QAction::triggered, this, [this, startBssoPick] {
            startBssoPick(3, m_bssoRamusLeftAct, tr("Marque el punto de rama izquierda para BSSO."));
        });
        row->addWidget(makeActBtn(m_bssoRamusLeftAct, page));

        m_bssoBodyLeftAct = new QAction(tr("Cuerpo\nIzq"), this);
        m_bssoBodyLeftAct->setEnabled(false);
        m_bssoBodyLeftAct->setCheckable(true);
        m_bssoBodyLeftAct->setToolTip(tr("Marcar punto anterior/inferior del cuerpo mandibular izquierdo."));
        connect(m_bssoBodyLeftAct, &QAction::triggered, this, [this, startBssoPick] {
            startBssoPick(4, m_bssoBodyLeftAct, tr("Marque el punto de cuerpo mandibular izquierdo para BSSO."));
        });
        row->addWidget(makeActBtn(m_bssoBodyLeftAct, page));

        m_bssoPlaneLeftAct = new QAction(tr("Basal\nIzq"), this);
        m_bssoPlaneLeftAct->setEnabled(false);
        m_bssoPlaneLeftAct->setCheckable(true);
        m_bssoPlaneLeftAct->setToolTip(tr("Marcar punto basal izquierdo para definir el plano sagital."));
        connect(m_bssoPlaneLeftAct, &QAction::triggered, this, [this, startBssoPick] {
            startBssoPick(5, m_bssoPlaneLeftAct, tr("Marque el punto basal izquierdo para BSSO."));
        });
        row->addWidget(makeActBtn(m_bssoPlaneLeftAct, page));

        m_bssoAdjustGuideAct = new QAction(tr("Ajustar\nBSSO"), this);
        m_bssoAdjustGuideAct->setEnabled(false);
        m_bssoAdjustGuideAct->setToolTip(tr("Mover, rotar o escalar las guias BSSO con el gizmo."));
        connect(m_bssoAdjustGuideAct, &QAction::triggered, this, [this] {
            if (!m_osteotomyView || !m_bssoGuideVisualMesh) return;
            if (m_bssoAdjustGuideAct) m_bssoAdjustGuideAct->setEnabled(false);
            if (m_bssoAcceptGuideAct) m_bssoAcceptGuideAct->setEnabled(true);
            if (m_bssoGuidePropsAct)  m_bssoGuidePropsAct->setEnabled(false);
            m_osteotomyView->startGizmo(kBssoGuideLabel);
            statusBar()->showMessage(tr("Gizmo BSSO activo: ajuste las guias sagitales y pulse Aceptar BSSO."));
        });
        row->addWidget(makeActBtn(m_bssoAdjustGuideAct, page));

        m_bssoGuidePropsAct = new QAction(tr("Prop.\nBSSO"), this);
        m_bssoGuidePropsAct->setEnabled(false);
        m_bssoGuidePropsAct->setToolTip(tr("Editar largo, alto, extension medial y grosor de la guia BSSO."));
        connect(m_bssoGuidePropsAct, &QAction::triggered,
                this, &MainWindow::showBssoGuidePropertiesDialog);
        row->addWidget(makeActBtn(m_bssoGuidePropsAct, page));

        m_bssoAcceptGuideAct = new QAction(tr("Aceptar\nBSSO"), this);
        m_bssoAcceptGuideAct->setEnabled(false);
        m_bssoAcceptGuideAct->setToolTip(tr("Confirmar la posicion actual de las guias BSSO."));
        connect(m_bssoAcceptGuideAct, &QAction::triggered, this, [this] {
            if (m_osteotomyView) m_osteotomyView->stopGizmo();
            m_bssoGuideReady = m_bssoGuideVisualMesh && m_bssoGuideVisualMesh->GetNumberOfPoints() > 0;
            if (m_leFortSplitAct) {
                m_leFortSplitAct->setEnabled(m_bssoGuideReady);
                m_leFortSplitAct->setToolTip(m_bssoActiveLeftSide
                    ? tr("Dividir mandibula usando la guia BSSO izquierda.")
                    : tr("Dividir mandibula usando la guia BSSO derecha."));
            }
        });
        row->addWidget(makeActBtn(m_bssoAcceptGuideAct, page));

        addSep(row, page);

        m_genioStatusLabel = new QLabel(tr("Mentón  0/4 pts"), page);
        m_genioStatusLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        m_genioStatusLabel->setStyleSheet("color:#34c759; font-size:10px; padding:0 4px;");
        m_genioStatusLabel->setMinimumWidth(110);
        row->addWidget(m_genioStatusLabel);

        auto startGenioPick = [this](int idx, QAction* act, const QString& message) {
            if (act) act->setChecked(false);
            m_leFortCapturingIdx = -1;
            m_bssoCapturingIdx = -1;
            m_genioCapturingIdx = idx;
            m_genioPlaneNormal = QVector3D(0, 0, 0);
            m_genioPlaneCenter = QVector3D(0, 0, 0);
            if (m_osteotomyView) {
                m_osteotomyView->stopGizmo();
                m_osteotomyView->removeMesh(kGenioPlaneLabel);
                m_osteotomyView->removeMesh(kGenioCutLineLabel);
                m_osteotomyView->setStandardView(0);
                m_osteotomyView->setPointPickMode(true);
                m_osteotomyView->render();
            }
            updateGenioPointStatus();
            statusBar()->showMessage(message);
        };

        m_genioApicalRightAct = new QAction(tr("Apical\nDer"), this);
        m_genioApicalRightAct->setEnabled(false);
        m_genioApicalRightAct->setCheckable(true);
        m_genioApicalRightAct->setToolTip(tr("Marcar punto apical derecho para osteotomia de menton."));
        connect(m_genioApicalRightAct, &QAction::triggered, this, [this, startGenioPick] {
            startGenioPick(0, m_genioApicalRightAct, tr("Marque el punto apical derecho del menton."));
        });
        row->addWidget(makeActBtn(m_genioApicalRightAct, page));

        m_genioBasalRightAct = new QAction(tr("Basal\nDer"), this);
        m_genioBasalRightAct->setEnabled(false);
        m_genioBasalRightAct->setCheckable(true);
        m_genioBasalRightAct->setToolTip(tr("Marcar punto basal derecho para osteotomia de menton."));
        connect(m_genioBasalRightAct, &QAction::triggered, this, [this, startGenioPick] {
            startGenioPick(1, m_genioBasalRightAct, tr("Marque el punto basal derecho del menton."));
        });
        row->addWidget(makeActBtn(m_genioBasalRightAct, page));

        m_genioApicalLeftAct = new QAction(tr("Apical\nIzq"), this);
        m_genioApicalLeftAct->setEnabled(false);
        m_genioApicalLeftAct->setCheckable(true);
        m_genioApicalLeftAct->setToolTip(tr("Marcar punto apical izquierdo para osteotomia de menton."));
        connect(m_genioApicalLeftAct, &QAction::triggered, this, [this, startGenioPick] {
            startGenioPick(2, m_genioApicalLeftAct, tr("Marque el punto apical izquierdo del menton."));
        });
        row->addWidget(makeActBtn(m_genioApicalLeftAct, page));

        m_genioBasalLeftAct = new QAction(tr("Basal\nIzq"), this);
        m_genioBasalLeftAct->setEnabled(false);
        m_genioBasalLeftAct->setCheckable(true);
        m_genioBasalLeftAct->setToolTip(tr("Marcar punto basal izquierdo para osteotomia de menton."));
        connect(m_genioBasalLeftAct, &QAction::triggered, this, [this, startGenioPick] {
            startGenioPick(3, m_genioBasalLeftAct, tr("Marque el punto basal izquierdo del menton."));
        });
        row->addWidget(makeActBtn(m_genioBasalLeftAct, page));

        m_genioAdjustPlaneAct = new QAction(tr("Ajustar\nMentón"), this);
        m_genioAdjustPlaneAct->setEnabled(false);
        m_genioAdjustPlaneAct->setToolTip(tr("Mover, rotar o escalar el plano de menton con el gizmo."));
        connect(m_genioAdjustPlaneAct, &QAction::triggered, this, [this] {
            if (!m_osteotomyView || !m_genioPlaneVisualMesh) return;
            if (m_genioAdjustPlaneAct) m_genioAdjustPlaneAct->setEnabled(false);
            if (m_genioAcceptPlaneAct) m_genioAcceptPlaneAct->setEnabled(true);
            m_osteotomyView->startGizmo(kGenioPlaneLabel);
            statusBar()->showMessage(tr("Gizmo de menton activo. Ajuste el plano y pulse Aceptar Menton."));
        });
        row->addWidget(makeActBtn(m_genioAdjustPlaneAct, page));

        m_genioAcceptPlaneAct = new QAction(tr("Aceptar\nMentón"), this);
        m_genioAcceptPlaneAct->setEnabled(false);
        m_genioAcceptPlaneAct->setToolTip(tr("Confirmar el plano actual de osteotomia de menton."));
        connect(m_genioAcceptPlaneAct, &QAction::triggered, this, [this] {
            if (m_osteotomyView) m_osteotomyView->stopGizmo();
            if (m_genioAdjustPlaneAct) m_genioAdjustPlaneAct->setEnabled(true);
            if (m_genioAcceptPlaneAct) m_genioAcceptPlaneAct->setEnabled(false);
            rebuildGenioPlaneFromState();
            statusBar()->showMessage(tr("Plano de menton aceptado."));
        });
        row->addWidget(makeActBtn(m_genioAcceptPlaneAct, page));

        row->addStretch(1);
    }

    // REGISTRO MORDIDA - align osteotomy segments to bite scan
    {
        auto  mod  = addModule(tr("REGISTRO MORDIDA"));
        auto* page = mod.first;
        auto* row  = mod.second;

        m_importBiteScanAct = new QAction(tr("STL\nMordida"), this);
        m_importBiteScanAct->setToolTip(tr("Importar escaneo STL de mordida/oclusion."));
        connect(m_importBiteScanAct, &QAction::triggered, this, &MainWindow::importBiteScanStl);
        row->addWidget(makeActBtn(m_importBiteScanAct, page));
        addSep(row, page);

        m_biteLeFortPtsAct = new QAction(tr("Pts\nLeFort"), this);
        m_biteLeFortPtsAct->setCheckable(true);
        m_biteLeFortPtsAct->setEnabled(false);
        m_biteLeFortPtsAct->setToolTip(tr("Seleccionar puntos homologos sobre el segmento Le Fort I."));
        connect(m_biteLeFortPtsAct, &QAction::triggered, this, [this](bool checked) {
            setBitePointCapture(checked ? BitePointSet::LeFortSegment : BitePointSet::None);
        });
        row->addWidget(makeActBtn(m_biteLeFortPtsAct, page));

        m_biteUpperScanPtsAct = new QAction(tr("Pts\nMord Sup"), this);
        m_biteUpperScanPtsAct->setCheckable(true);
        m_biteUpperScanPtsAct->setEnabled(false);
        m_biteUpperScanPtsAct->setToolTip(tr("Seleccionar los puntos correspondientes superiores sobre el escaneo de mordida."));
        connect(m_biteUpperScanPtsAct, &QAction::triggered, this, [this](bool checked) {
            setBitePointCapture(checked ? BitePointSet::UpperBiteScan : BitePointSet::None);
        });
        row->addWidget(makeActBtn(m_biteUpperScanPtsAct, page));

        m_biteRegisterLeFortAct = new QAction(tr("Reg\nLeFort"), this);
        m_biteRegisterLeFortAct->setEnabled(false);
        m_biteRegisterLeFortAct->setToolTip(tr("Adaptar el escaneo de mordida a la posicion del segmento Le Fort I."));
        connect(m_biteRegisterLeFortAct, &QAction::triggered, this, &MainWindow::alignLeFortToBiteScan);
        row->addWidget(makeActBtn(m_biteRegisterLeFortAct, page));
        addSep(row, page);

        m_biteMandiblePtsAct = new QAction(tr("Pts\nDistal"), this);
        m_biteMandiblePtsAct->setCheckable(true);
        m_biteMandiblePtsAct->setEnabled(false);
        m_biteMandiblePtsAct->setToolTip(tr("Seleccionar puntos homologos sobre el segmento mandibular distal."));
        connect(m_biteMandiblePtsAct, &QAction::triggered, this, [this](bool checked) {
            setBitePointCapture(checked ? BitePointSet::MandibleDistal : BitePointSet::None);
        });
        row->addWidget(makeActBtn(m_biteMandiblePtsAct, page));

        m_biteLowerScanPtsAct = new QAction(tr("Pts\nMord Inf"), this);
        m_biteLowerScanPtsAct->setCheckable(true);
        m_biteLowerScanPtsAct->setEnabled(false);
        m_biteLowerScanPtsAct->setToolTip(tr("Seleccionar los puntos correspondientes inferiores sobre el escaneo de mordida."));
        connect(m_biteLowerScanPtsAct, &QAction::triggered, this, [this](bool checked) {
            setBitePointCapture(checked ? BitePointSet::LowerBiteScan : BitePointSet::None);
        });
        row->addWidget(makeActBtn(m_biteLowerScanPtsAct, page));

        m_biteRegisterMandibleAct = new QAction(tr("Reg\nMand"), this);
        m_biteRegisterMandibleAct->setEnabled(false);
        m_biteRegisterMandibleAct->setToolTip(tr("Alinear el segmento mandibular distal al escaneo de mordida."));
        connect(m_biteRegisterMandibleAct, &QAction::triggered, this, &MainWindow::alignMandibleDistalToBiteScan);
        row->addWidget(makeActBtn(m_biteRegisterMandibleAct, page));
        addSep(row, page);

        m_biteAdjustGizmoAct = new QAction(tr("Ajustar\nMatch"), this);
        m_biteAdjustGizmoAct->setEnabled(false);
        m_biteAdjustGizmoAct->setToolTip(tr("Ajustar manualmente con gizmo el ultimo match de mordida."));
        connect(m_biteAdjustGizmoAct, &QAction::triggered, this, [this] {
            const int label = m_biteMandibleRegistered
                ? currentBiteMandibleTargetLabel()
                : (m_biteLeFortRegistered ? kBiteScanLabel : -1);
            startBiteAdjustmentGizmo(label);
        });
        row->addWidget(makeActBtn(m_biteAdjustGizmoAct, page));

        m_biteAcceptGizmoAct = new QAction(tr("Aceptar\nAjuste"), this);
        m_biteAcceptGizmoAct->setEnabled(false);
        m_biteAcceptGizmoAct->setToolTip(tr("Confirmar el ajuste manual del match de mordida."));
        connect(m_biteAcceptGizmoAct, &QAction::triggered, this, &MainWindow::acceptBiteAdjustmentGizmo);
        row->addWidget(makeActBtn(m_biteAcceptGizmoAct, page));
        addSep(row, page);

        m_biteClearPtsAct = new QAction(tr("Limpiar\nPts"), this);
        m_biteClearPtsAct->setEnabled(false);
        m_biteClearPtsAct->setToolTip(tr("Eliminar los puntos del registro de mordida."));
        connect(m_biteClearPtsAct, &QAction::triggered, this, &MainWindow::clearBiteRegistrationPoints);
        row->addWidget(makeActBtn(m_biteClearPtsAct, page));

        row->addStretch(1);
    }

    // REPOSICION - postoperative segment movement
    {
        auto  mod  = addModule(tr("REPOSICIÓN"));
        auto* page = mod.first;
        auto* row  = mod.second;

        m_repositionGizmoAct = new QAction(tr("Gizmo\nMover"), this);
        m_repositionGizmoAct->setEnabled(false);
        m_repositionGizmoAct->setToolTip(tr("Activar control interactivo para mover/rotar/escalar el segmento seleccionado."));
        connect(m_repositionGizmoAct, &QAction::triggered, this, &MainWindow::startRepositionGizmo);
        row->addWidget(makeActBtn(m_repositionGizmoAct, page));

        m_repositionAcceptGizmoAct = new QAction(tr("Aceptar\nGizmo"), this);
        m_repositionAcceptGizmoAct->setEnabled(false);
        m_repositionAcceptGizmoAct->setToolTip(tr("Confirmar el ajuste interactivo del segmento."));
        connect(m_repositionAcceptGizmoAct, &QAction::triggered, this, &MainWindow::acceptRepositionGizmo);
        row->addWidget(makeActBtn(m_repositionAcceptGizmoAct, page));

        addSep(row, page);

        m_repositionResetAct = new QAction(tr("Reset\nObjeto"), this);
        m_repositionResetAct->setEnabled(false);
        m_repositionResetAct->setToolTip(tr("Restaurar el segmento seleccionado al estado inicial de reposicion."));
        connect(m_repositionResetAct, &QAction::triggered, this, &MainWindow::resetRepositionTarget);
        row->addWidget(makeActBtn(m_repositionResetAct, page));

        m_repositionHomeAct = new QAction(tr("Enviar\nHome"), this);
        m_repositionHomeAct->setEnabled(false);
        m_repositionHomeAct->setToolTip(tr("Enviar el segmento seleccionado a su posicion de origen."));
        connect(m_repositionHomeAct, &QAction::triggered, this, &MainWindow::sendRepositionTargetHome);
        row->addWidget(makeActBtn(m_repositionHomeAct, page));

        addSep(row, page);

        m_repositionFixedViewAct = new QAction(tr("Vista\nFija"), this);
        m_repositionFixedViewAct->setEnabled(false);
        m_repositionFixedViewAct->setCheckable(true);
        m_repositionFixedViewAct->setToolTip(tr("Alternar vista frontal fija para reposicion."));
        connect(m_repositionFixedViewAct, &QAction::toggled, this, [this](bool checked) {
            if (!m_repositionView) return;
            if (checked) {
                m_repositionView->setStandardView(0);
            } else {
                m_repositionView->resetCamera();
            }
            m_repositionView->render();
        });
        row->addWidget(makeActBtn(m_repositionFixedViewAct, page));

        row->addStretch(1);
    }

    // FERULA - intermediate/final occlusal splints
    {
        auto  mod  = addModule(tr("FERULA"));
        auto* page = mod.first;
        auto* row  = mod.second;

        m_splintIntermediateAct = new QAction(tr("Ferula\nInterm."), this);
        m_splintIntermediateAct->setEnabled(false);
        m_splintIntermediateAct->setToolTip(
            tr("Crear ferula intermedia: Le Fort reposicionado contra mandibula inicial."));
        connect(m_splintIntermediateAct, &QAction::triggered,
                this, &MainWindow::createIntermediateSplint);
        row->addWidget(makeActBtn(m_splintIntermediateAct, page));

        m_splintFinalAct = new QAction(tr("Ferula\nFinal"), this);
        m_splintFinalAct->setEnabled(false);
        m_splintFinalAct->setToolTip(
            tr("Crear ferula final: Le Fort reposicionado contra mandibula distal reposicionada."));
        connect(m_splintFinalAct, &QAction::triggered,
                this, &MainWindow::createFinalSplint);
        row->addWidget(makeActBtn(m_splintFinalAct, page));

        addSep(row, page);

        m_splintExportAct = new QAction(tr("Exportar\nFerulas"), this);
        m_splintExportAct->setEnabled(false);
        m_splintExportAct->setToolTip(tr("Exportar las ferulas generadas como STL."));
        connect(m_splintExportAct, &QAction::triggered,
                this, &MainWindow::exportSplintStl);
        row->addWidget(makeActBtn(m_splintExportAct, page));

        row->addStretch(1);
    }

    // GUIAS - surgical guides built on the repositioned models
    {
        auto  mod  = addModule(tr("GUIAS"));
        auto* page = mod.first;
        // The GUIAS actions live in the guided panel on the left of the workspace (MainWindowGuides.cpp).
        auto* guideHint = new QLabel(tr("Siga los pasos en el panel izquierdo de Guías."), page);
        guideHint->setStyleSheet("color:#98989d; font-size:11px; padding-left:8px;");
        mod.second->addWidget(guideHint);
        mod.second->addStretch(1);
    }

    // ORTOGNÁTICA: opens the step rail on the current planning module (MainWindowOrthognathic.cpp).
    m_orthoTab = new QToolButton(tabBar);
    m_orthoTab->setObjectName("MT");
    m_orthoTab->setText(tr("ORTOGNÁTICA"));
    m_orthoTab->setCheckable(true);
    m_orthoTab->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    tabGroup->addButton(m_orthoTab);
    tabLayout->addWidget(m_orthoTab);
    connect(m_orthoTab, &QToolButton::clicked, this, [this] { selectOrthognathicStep(m_orthoStep); });

    tabLayout->addStretch(1);   // push module tabs to the left
}

QWidget* MainWindow::buildLeftMenuPanel()
{
    // Left panel replaced by ribbon toolbar — kept as stub for API compatibility.
    return nullptr;

    // (unreachable — silences unused-variable warning on old code)
    auto* panel = new QWidget(this);
    panel->setObjectName("LeftMenuPanel");
    panel->setFixedWidth(176);
    panel->setStyleSheet(
        "#LeftMenuPanel { background:#1f1f21; border-right:1px solid #2c2c2e; }"
        "#LeftMenuPanel QLabel#MenuTitle { color:#f5f5f7; font-weight:700; font-size:12px; }"
        "#LeftMenuPanel QGroupBox { border:1px solid #2c2c2e; border-radius:12px;"
        "  margin-top:8px; padding-top:8px; font-weight:700; background:#242426; }"
        "#LeftMenuPanel QGroupBox::title { subcontrol-origin:margin; left:7px;"
        "  color:#0a84ff; font-size:10px; }"
        "#LeftMenuPanel QToolButton { color:#f5f5f7; background:#2c2c2e;"
        "  border:1px solid #3a3a3c; border-radius:10px; padding:6px 8px; text-align:left; font-size:11px; }"
        "#LeftMenuPanel QToolButton:hover { background:#3a3a3c; }"
        "#LeftMenuPanel QToolButton:checked { background:#0a84ff; color:#ffffff; border-color:#0a84ff; }"
        "#LeftMenuPanel QToolButton:disabled { background:#242426; color:#636366; }"
        "#LeftMenuPanel QComboBox { background:#2c2c2e; color:#f5f5f7;"
        "  border:1px solid #3a3a3c; border-radius:10px; padding:4px; font-size:10px; }");

    auto* root = new QVBoxLayout(panel);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(5);

    auto* title = new QLabel(tr("PLANIFICACIÓN MAXILOFACIAL"), panel);
    title->setObjectName("MenuTitle");
    title->setAlignment(Qt::AlignCenter);
    root->addWidget(title);

    auto makeActionButton = [](QAction* action, QWidget* parent) {
        auto* button = new QToolButton(parent);
        button->setDefaultAction(action);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setMinimumHeight(24);
        return button;
    };

    auto makeGroup = [](QVBoxLayout* parentLayout, QWidget* parent, const QString& titleText) {
        auto* box = new QGroupBox(titleText, parent);
        auto* layout = new QVBoxLayout(box);
        layout->setContentsMargins(6, 10, 6, 6);
        layout->setSpacing(4);
        parentLayout->addWidget(box);
        return layout;
    };

    auto* projectLayout = makeGroup(root, panel, tr("FILE"));
    if (m_openAction) {
        projectLayout->addWidget(makeActionButton(m_openAction, panel));
    } else {
        auto* openAct = new QAction(tr("Open DICOM"), this);
        connect(openAct, &QAction::triggered, this, &MainWindow::onOpenDicomFolder);
        projectLayout->addWidget(makeActionButton(openAct, panel));
    }

    auto* viewLayout = makeGroup(root, panel, tr("IMAGE"));
    auto addViewAction = [&](const QString& text, auto slot) {
        auto* action = new QAction(text, this);
        connect(action, &QAction::triggered, this, slot);
        viewLayout->addWidget(makeActionButton(action, panel));
    };
    addViewAction(tr("1 - Bone"), &MainWindow::onBoneWindow);
    addViewAction(tr("2 - Soft Tissue"), &MainWindow::onSoftTissueWindow);
    addViewAction(tr("3 - Lung"), &MainWindow::onLungWindow);
    addViewAction(tr("4 - Brain"), &MainWindow::onBrainWindow);

    auto* resetCameraAct = new QAction(tr("Reset Camera"), this);
    connect(resetCameraAct, &QAction::triggered, this, [this] {
        if (m_axialView)    m_axialView->resetCamera();
        if (m_coronalView)  m_coronalView->resetCamera();
        if (m_sagittalView) m_sagittalView->resetCamera();
    });
    viewLayout->addWidget(makeActionButton(resetCameraAct, panel));

    auto* moduleLayout = makeGroup(root, panel, tr("MODULES"));
    m_leftModuleStack = new QStackedWidget(panel);
    m_leftModuleStack->setVisible(false);

    auto makePage = [&] {
        auto* page = new QWidget(m_leftModuleStack);
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(5);
        m_leftModuleStack->addWidget(page);
        return std::pair<QWidget*, QVBoxLayout*>(page, layout);
    };

    auto [measurementPage, measurementRoot] = makePage();
    auto* measureLayout = makeGroup(measurementRoot, measurementPage, tr("MEASURE"));
    auto* toolGroup = new QActionGroup(this);
    toolGroup->setExclusive(true);

    auto addMeasureTool = [&](const QString& text,
                              MeasurementToolMode mode,
                              const QKeySequence& key,
                              bool checked = false) {
        auto* action = new QAction(text, this);
        action->setCheckable(true);
        action->setShortcut(key);
        toolGroup->addAction(action);
        addAction(action);
        connect(action, &QAction::triggered, this, [this, mode] {
            setMeasurementTool(mode);
        });
        measureLayout->addWidget(makeActionButton(action, measurementPage));
        action->setChecked(checked);
        return action;
    };

    addMeasureTool(tr("Cursor"), MeasurementToolMode::Cursor, Qt::Key_C, true);
    addMeasureTool(tr("Distancia"), MeasurementToolMode::Distance, Qt::Key_D);
    addMeasureTool(tr("Angulo"), MeasurementToolMode::Angle, Qt::Key_A);
    addMeasureTool(tr("Area"), MeasurementToolMode::Area, Qt::Key_P);
    addMeasureTool(tr("ROI Circular"), MeasurementToolMode::ROICircle, Qt::Key_O);
    addMeasureTool(tr("ROI Rect"), MeasurementToolMode::ROIRectangle, Qt::Key_I);
    addMeasureTool(tr("Perpendicular"), MeasurementToolMode::PerpendicularDistance, Qt::Key_L);
    addMeasureTool(tr("Anotacion"), MeasurementToolMode::Annotation, Qt::Key_N);

    auto* dataLayout = makeGroup(measurementRoot, measurementPage, tr("ANALYZE"));
    auto* deleteAct = new QAction(tr("Eliminar medida"), this);
    deleteAct->setShortcut(QKeySequence::Delete);
    addAction(deleteAct);
    connect(deleteAct, &QAction::triggered, this, &MainWindow::deleteSelectedMeasurement);
    dataLayout->addWidget(makeActionButton(deleteAct, measurementPage));

    m_toggleMeasurementsAct = new QAction(tr("Medidas ON"), this);
    m_toggleMeasurementsAct->setCheckable(true);
    m_toggleMeasurementsAct->setChecked(true);
    connect(m_toggleMeasurementsAct, &QAction::toggled, this, [this](bool visible) {
        m_toggleMeasurementsAct->setText(visible ? tr("Medidas ON") : tr("Medidas OFF"));
        m_measurementManager.setAllVisible(visible);
    });
    dataLayout->addWidget(makeActionButton(m_toggleMeasurementsAct, measurementPage));

    auto* saveAct = new QAction(tr("Guardar JSON"), this);
    connect(saveAct, &QAction::triggered, this, &MainWindow::saveMeasurements);
    dataLayout->addWidget(makeActionButton(saveAct, measurementPage));

    auto* loadAct = new QAction(tr("Cargar JSON"), this);
    connect(loadAct, &QAction::triggered, this, &MainWindow::loadMeasurements);
    dataLayout->addWidget(makeActionButton(loadAct, measurementPage));
    measurementRoot->addStretch(1);

    auto [segmentationPage, segmentationRoot] = makePage();
    auto* segmentLayout = makeGroup(segmentationRoot, segmentationPage, tr("SEGMENT"));
    auto* backendInfoAct = new QAction(tr("Backend IA"), this);
    connect(backendInfoAct, &QAction::triggered,
            this, &MainWindow::showSegmentationBackendInfo);
    segmentLayout->addWidget(makeActionButton(backendInfoAct, segmentationPage));

    auto addSegmentationAction = [&](const QString& text, SegmentationTarget target) {
        auto* action = new QAction(text, this);
        connect(action, &QAction::triggered, this, [this, target] {
            startAISegmentation(target);
        });
        segmentLayout->addWidget(makeActionButton(action, segmentationPage));
    };
    addSegmentationAction(tr("Segmentar hueso"), SegmentationTarget::Bone);
    addSegmentationAction(tr("Segmentar tejido blando"), SegmentationTarget::SoftTissue);
    addSegmentationAction(tr("Segmentar dientes"), SegmentationTarget::Teeth);
    addSegmentationAction(tr("Segmentar via aerea"), SegmentationTarget::Airway);

    auto* manualAirwayAct = new QAction(tr("Vía aérea manual"), this);
    connect(manualAirwayAct, &QAction::triggered, this, [this] {
        if (!m_volume) {
            QMessageBox::warning(this, tr("Vía Aérea Manual"), tr("Primero cargue un volumen DICOM."));
            return;
        }
        deactivateLassoTools();
        setMeasurementTool(MeasurementToolMode::Cursor);
        m_airwayPointCaptureStep = 1;
        for (MPRView* view : {m_axialView, m_coronalView, m_sagittalView}) {
            if (view) {
                view->setMeasurementPickingEnabled(true);
            }
        }
        statusBar()->showMessage(tr("Vía Aérea Manual: haga clic en el LÍMITE SUPERIOR (Nasofaringe) de la vía aérea..."));
        showAirwayDialog(1);
    });
    segmentLayout->addWidget(makeActionButton(manualAirwayAct, segmentationPage));

    m_toggleSegmentationOverlayAct = new QAction(tr("Overlay SEG ON"), this);
    m_toggleSegmentationOverlayAct->setCheckable(true);
    m_toggleSegmentationOverlayAct->setChecked(true);
    connect(m_toggleSegmentationOverlayAct, &QAction::toggled, this, [this](bool visible) {
        m_toggleSegmentationOverlayAct->setText(
            visible ? tr("Overlay SEG ON") : tr("Overlay SEG OFF"));
        refreshSegmentationOverlays();
    });
    segmentLayout->addWidget(makeActionButton(m_toggleSegmentationOverlayAct, segmentationPage));

    auto* meshLayout = makeGroup(segmentationRoot, segmentationPage, tr("3D TOOLS"));
    m_structureCombo = new QComboBox(segmentationPage);
    m_structureCombo->addItem("1 Hueso", 1);
    m_structureCombo->addItem("2 Tejido blando", 2);
    m_structureCombo->addItem("4 Via aerea", 4);
    m_structureCombo->addItem("5 Maxilar", 5);
    m_structureCombo->addItem("6 Mandibula", 6);
    m_structureCombo->addItem("7 Canal mandibular", 7);
    m_structureCombo->setMinimumHeight(24);
    meshLayout->addWidget(m_structureCombo);

    auto* meshAct = new QAction(tr("Generar malla"), this);
    connect(meshAct, &QAction::triggered, this, &MainWindow::generateSelectedMesh);
    meshLayout->addWidget(makeActionButton(meshAct, segmentationPage));

    auto* reset3DAct = new QAction(tr("Reset 3D"), this);
    connect(reset3DAct, &QAction::triggered, this, [this] {
        if (m_mesh3DView) m_mesh3DView->resetCamera();
    });
    meshLayout->addWidget(makeActionButton(reset3DAct, segmentationPage));

    auto* clear3DAct = new QAction(tr("Limpiar 3D"), this);
    connect(clear3DAct, &QAction::triggered, this, [this] {
        if (m_mesh3DView) m_mesh3DView->clearMeshes();
        if (m_objectTable) m_objectTable->setRowCount(0);
    });
    meshLayout->addWidget(makeActionButton(clear3DAct, segmentationPage));
    segmentationRoot->addStretch(1);

    auto* measurementModuleAct = new QAction(tr("Mediciones >"), this);
    measurementModuleAct->setCheckable(true);
    auto* segmentationModuleAct = new QAction(tr("Segmentacion >"), this);
    segmentationModuleAct->setCheckable(true);

    auto setOpenModule = [this, measurementModuleAct, segmentationModuleAct](int index,
                                                                            bool open) {
        if (!m_leftModuleStack) return;
        m_leftModuleStack->setVisible(open);
        if (open) m_leftModuleStack->setCurrentIndex(index);

        const bool measurementsOpen = open && index == 0;
        const bool segmentationOpen = open && index == 1;
        measurementModuleAct->setChecked(measurementsOpen);
        segmentationModuleAct->setChecked(segmentationOpen);
        measurementModuleAct->setText(
            measurementsOpen ? tr("Mediciones v") : tr("Mediciones >"));
        segmentationModuleAct->setText(
            segmentationOpen ? tr("Segmentacion v") : tr("Segmentacion >"));
    };

    connect(measurementModuleAct, &QAction::triggered, this,
            [this, setOpenModule] {
                const bool open = !(m_leftModuleStack &&
                                    m_leftModuleStack->isVisible() &&
                                    m_leftModuleStack->currentIndex() == 0);
                setOpenModule(0, open);
            });
    connect(segmentationModuleAct, &QAction::triggered, this,
            [this, setOpenModule] {
                const bool open = !(m_leftModuleStack &&
                                    m_leftModuleStack->isVisible() &&
                                    m_leftModuleStack->currentIndex() == 1);
                setOpenModule(1, open);
            });

    moduleLayout->addWidget(makeActionButton(measurementModuleAct, panel));
    moduleLayout->addWidget(makeActionButton(segmentationModuleAct, panel));
    root->addWidget(m_leftModuleStack, 1);
    return panel;
}

void MainWindow::buildCentralWidget()
{
    auto* root = new QWidget(this);
    auto* hbox = new QHBoxLayout(root);
    hbox->setContentsMargins(4, 4, 4, 4);
    hbox->setSpacing(4);

    // ── 2×2 MPR + 3D view grid ─────────────────────────────────────────────
    m_viewModeStack = new QStackedWidget(root);
    auto* viewGridPanel = new QWidget(m_viewModeStack);
    auto* viewGrid      = new QGridLayout(viewGridPanel);
    viewGrid->setContentsMargins(0, 0, 0, 0);
    viewGrid->setSpacing(4);
    viewGrid->setRowStretch(0, 1);
    viewGrid->setRowStretch(1, 1);
    viewGrid->setColumnStretch(0, 1);
    viewGrid->setColumnStretch(1, 1);

    m_axialView    = new MPRView(MPROrientation::Axial,    viewGridPanel);
    m_coronalView  = new MPRView(MPROrientation::Coronal,  viewGridPanel);
    m_sagittalView = new MPRView(MPROrientation::Sagittal, viewGridPanel);
    m_mesh3DView   = new Mesh3DView(viewGridPanel);

    viewGrid->addWidget(m_axialView,    0, 0);
    viewGrid->addWidget(m_coronalView,  0, 1);
    viewGrid->addWidget(m_sagittalView, 1, 0);
    viewGrid->addWidget(m_mesh3DView,   1, 1);

    auto wireView = [this](MPRView* view) {
        connect(view, &MPRView::reslicePositionChanged,
                this, &MainWindow::syncResliceViews);
        connect(view, &MPRView::fullScreenToggleRequested,
                this, [this](MPRView* source) {
                    toggleViewFullScreen(static_cast<QWidget*>(source));
                });
        connect(view, &MPRView::physicalPointClicked,
                this, &MainWindow::handleMeasurementPoint);
        connect(view, &MPRView::measurementCompleteRequested,
                this, &MainWindow::finishMeasurementTool);
        connect(view, &MPRView::lassoEditCompleted,
                this, &MainWindow::onLassoEdit);
    };
    wireView(m_axialView);
    wireView(m_coronalView);
    wireView(m_sagittalView);
    connect(m_mesh3DView, &Mesh3DView::fullScreenToggleRequested,
            this, [this](Mesh3DView* source) {
                toggleViewFullScreen(static_cast<QWidget*>(source));
            });
    connect(m_mesh3DView, &Mesh3DView::pointPicked,
            this, &MainWindow::onDentalPointPicked);
    connect(m_mesh3DView, &Mesh3DView::meshEdited,
            this, &MainWindow::onSurfaceMeshEdited);
    connect(m_mesh3DView, &Mesh3DView::gizmoMeshUpdated,
            this, &MainWindow::onDentalGizmoMeshUpdated);

    m_viewModeStack->addWidget(viewGridPanel);

    // ── MODELOS panel ──────────────────────────────────────────────────────
    //
    //  ┌───────────────────────────────────┐
    //  │  bone view  │  arch STL view      │  ← top pair (step-switched)
    //  ├───────────────────────────────────┤
    //  │      match preview (full width)   │  ← both meshes overlaid
    //  └───────────────────────────────────┘
    //         [ Crear Modelo Compuesto ]
    //
    auto* modelPanel = new QWidget(m_viewModeStack);
    auto* modelRow = new QHBoxLayout(modelPanel);
    modelRow->setContentsMargins(0, 0, 0, 0);
    modelRow->setSpacing(0);
    modelRow->addWidget(buildModelControlPanel(modelPanel));
    auto* modelContent = new QWidget(modelPanel);
    modelRow->addWidget(modelContent, 1);
    auto* modelPanelLayout = new QVBoxLayout(modelContent);
    modelPanelLayout->setContentsMargins(0, 0, 0, 0);
    modelPanelLayout->setSpacing(0);

    // ── Vertical splitter: top pair | match preview ────────────────────
    m_modelSplitter = new QSplitter(Qt::Vertical, modelPanel);
    auto* modelSplitter = m_modelSplitter;
    modelSplitter->setHandleWidth(4);
    modelPanelLayout->addWidget(modelSplitter, 1);

    // ── Top section: step stack (2 cols) ─────────────────────────────
    m_modelStepStack = new QStackedWidget(modelSplitter);
    modelSplitter->addWidget(m_modelStepStack);

    // Step 0: Maxilar | Arco Superior
    auto* upperPage = new QWidget(m_modelStepStack);
    auto* upperGrid = new QGridLayout(upperPage);
    upperGrid->setContentsMargins(0, 0, 0, 0);
    upperGrid->setSpacing(4);
    upperGrid->setColumnStretch(0, 1);
    upperGrid->setColumnStretch(1, 1);
    m_modelMaxillaView   = new Mesh3DView(upperPage);
    m_modelUpperArchView = new Mesh3DView(upperPage);
    m_modelMaxillaView  ->setTitle(tr("MAXILAR"));
    m_modelUpperArchView->setTitle(tr("ARCO SUPERIOR"));
    upperGrid->addWidget(m_modelMaxillaView,   0, 0);
    upperGrid->addWidget(m_modelUpperArchView, 0, 1);
    m_modelStepStack->addWidget(upperPage);   // index 0

    // Step 1: Mandíbula | Arco Inferior
    auto* lowerPage = new QWidget(m_modelStepStack);
    auto* lowerGrid = new QGridLayout(lowerPage);
    lowerGrid->setContentsMargins(0, 0, 0, 0);
    lowerGrid->setSpacing(4);
    lowerGrid->setColumnStretch(0, 1);
    lowerGrid->setColumnStretch(1, 1);
    m_modelMandibleView  = new Mesh3DView(lowerPage);
    m_modelLowerArchView = new Mesh3DView(lowerPage);
    m_modelMandibleView ->setTitle(tr("MANDÍBULA"));
    m_modelLowerArchView->setTitle(tr("ARCO INFERIOR"));
    lowerGrid->addWidget(m_modelMandibleView,  0, 0);
    lowerGrid->addWidget(m_modelLowerArchView, 0, 1);
    m_modelStepStack->addWidget(lowerPage);   // index 1

    m_modelStepStack->setCurrentIndex(0);

    // ── Bottom section: match preview ─────────────────────────────────
    m_modelMatchView = new Mesh3DView(modelSplitter);
    m_modelMatchView->setTitle(tr("MATCH PREVIEW"));
    modelSplitter->addWidget(m_modelMatchView);
    modelSplitter->setStretchFactor(0, 2);   // top gets 2/3
    modelSplitter->setStretchFactor(1, 1);   // bottom gets 1/3

    // ── "Crear Modelo Compuesto" button ──────────────────────────────
    m_compositeButton = new QPushButton(tr("Crear Modelo Compuesto"), modelPanel);
    m_compositeButton->setFixedHeight(34);
    m_compositeButton->setStyleSheet(
        "QPushButton { background:#0a84ff; color:#ffffff; font-weight:700; font-size:12px;"
        "  border:none; border-radius:12px; margin:4px 8px; }"
        "QPushButton:hover    { background:#1d9bf0; }"
        "QPushButton:pressed  { background:#0066cc; }"
        "QPushButton:disabled { background:#242426; color:#636366; }");
    connect(m_compositeButton, &QPushButton::clicked,
            this, &MainWindow::createDentalCompositeModels);
    modelPanelLayout->addWidget(m_compositeButton, 0);
    modelPanelLayout->addWidget(buildCompositeBlockPanel(modelPanel), 0);

    // ── Wire all model views ──────────────────────────────────────────
    auto wireModelView = [this](Mesh3DView* view) {
        connect(view, &Mesh3DView::fullScreenToggleRequested,
                this, [this](Mesh3DView* source) {
                    toggleViewFullScreen(static_cast<QWidget*>(source));
                });
        connect(view, &Mesh3DView::pointPicked,
                this, &MainWindow::onDentalPointPicked);
        connect(view, &Mesh3DView::gizmoMeshUpdated,
                this, &MainWindow::onDentalGizmoMeshUpdated);
    };
    wireModelView(m_modelMaxillaView);
    wireModelView(m_modelUpperArchView);
    wireModelView(m_modelMandibleView);
    wireModelView(m_modelLowerArchView);
    wireModelView(m_modelMatchView);

    // ── Gizmo feedback: keep arch mesh pointers in sync after stopGizmo() ──
    m_viewModeStack->addWidget(modelPanel);   // index 1

    // ── PLAN / ORIENTACION workspace — page 2 ─────────────────────────────
    {
        auto* planPanel = new QWidget(m_viewModeStack);
        auto* planLayout = new QHBoxLayout(planPanel);
        planLayout->setContentsMargins(0, 0, 0, 0);
        planLayout->setSpacing(0);
        planLayout->addWidget(buildOrientationControlPanel(planPanel));

        m_orientationView = new Mesh3DView(planPanel);
        m_orientationView->setTitle(tr("ORIENTACIÓN — Plano de Frankfort"));
        planLayout->addWidget(m_orientationView, 1);

        connect(m_orientationView, &Mesh3DView::fullScreenToggleRequested,
                this, [this](Mesh3DView* source) {
                    toggleViewFullScreen(static_cast<QWidget*>(source));
        });
        connect(m_orientationView, &Mesh3DView::pointPicked,
                this, &MainWindow::onOrientationPointPicked);
        connect(m_orientationView, &Mesh3DView::gizmoMeshUpdated,
                this, &MainWindow::onOrientationGizmoUpdated);

        m_viewModeStack->addWidget(planPanel);  // index 2
    }

    // ── OSTEOTOMIA workspace — page 3 ─────────────────────────────────────
    {
        auto* ostPanel  = new QWidget(m_viewModeStack);
        auto* ostLayout = new QVBoxLayout(ostPanel);
        ostLayout->setContentsMargins(0, 0, 0, 0);
        ostLayout->setSpacing(0);
        m_osteotomyWizardPanel = nullptr;

        auto* ostRow = new QHBoxLayout();
        ostRow->setContentsMargins(0, 0, 0, 0);
        ostRow->setSpacing(0);
        ostLayout->addLayout(ostRow, 1);
        m_osteotomyView = new Mesh3DView(ostPanel);
        m_osteotomyView->setTitle(tr("OSTEOTOMÍA"));
        ostRow->addWidget(buildOsteotomyWizard(ostPanel));
        ostRow->addWidget(m_osteotomyView, 1);

        connect(m_osteotomyView, &Mesh3DView::fullScreenToggleRequested,
                this, [this](Mesh3DView* source) {
                    toggleViewFullScreen(static_cast<QWidget*>(source));
        });
        connect(m_osteotomyView, &Mesh3DView::pointPicked,
                this, &MainWindow::onLeFortPointPicked);
        connect(m_osteotomyView, &Mesh3DView::gizmoMeshUpdated,
                this, &MainWindow::onOsteotomyGizmoUpdated);

        m_viewModeStack->addWidget(ostPanel);  // index 3
    }

    // REGISTRO MORDIDA workspace - page 4
    {
        auto* bitePanel = new QWidget(m_viewModeStack);
        auto* biteLayout = new QVBoxLayout(bitePanel);
        biteLayout->setContentsMargins(0, 0, 0, 0);
        biteLayout->setSpacing(0);

        auto* biteSplitter = new QSplitter(Qt::Vertical, bitePanel);
        biteSplitter->setHandleWidth(4);
        biteLayout->addWidget(biteSplitter, 1);

        auto* biteTopPanel = new QWidget(biteSplitter);
        auto* biteTopGrid = new QGridLayout(biteTopPanel);
        biteTopGrid->setContentsMargins(0, 0, 0, 0);
        biteTopGrid->setSpacing(4);
        biteTopGrid->setColumnStretch(0, 1);
        biteTopGrid->setColumnStretch(1, 1);

        m_biteSegmentView = new Mesh3DView(biteTopPanel);
        m_biteScanView = new Mesh3DView(biteTopPanel);
        m_biteSegmentView->setTitle(tr("LE FORT / MANDIBULA"));
        m_biteScanView->setTitle(tr("ESCANEO DE MORDIDA"));
        biteTopGrid->addWidget(m_biteSegmentView, 0, 0);
        biteTopGrid->addWidget(m_biteScanView, 0, 1);

        m_biteRegistrationView = new Mesh3DView(biteSplitter);
        m_biteRegistrationView->setTitle(tr("MATCH MORDIDA"));
        biteSplitter->addWidget(biteTopPanel);
        biteSplitter->addWidget(m_biteRegistrationView);
        biteSplitter->setStretchFactor(0, 2);
        biteSplitter->setStretchFactor(1, 1);

        auto wireBiteView = [this](Mesh3DView* view) {
            connect(view, &Mesh3DView::fullScreenToggleRequested,
                    this, [this](Mesh3DView* source) {
                        toggleViewFullScreen(static_cast<QWidget*>(source));
                    });
            connect(view, &Mesh3DView::pointPicked,
                    this, &MainWindow::onBitePointPicked);
            connect(view, &Mesh3DView::gizmoMeshUpdated,
                    this, &MainWindow::onBiteGizmoMeshUpdated);
        };
        wireBiteView(m_biteSegmentView);
        wireBiteView(m_biteScanView);
        wireBiteView(m_biteRegistrationView);

        m_viewModeStack->addWidget(bitePanel);  // index 4
    }

    // REPOSICION workspace - page 5
    {
        auto* repPanel = new QWidget(m_viewModeStack);
        auto* repLayout = new QHBoxLayout(repPanel);
        repLayout->setContentsMargins(0, 0, 0, 0);
        repLayout->setSpacing(4);

        m_repositionSplitter = new QSplitter(Qt::Horizontal, repPanel);
        m_repositionSplitter->setHandleWidth(4);

        auto* controls = buildRepositionControlPanel(m_repositionSplitter);
        m_repositionView = new Mesh3DView(m_repositionSplitter);
        m_repositionView->setTitle(tr("REPOSICIÓN"));

        m_repositionSplitter->addWidget(controls);
        m_repositionSplitter->addWidget(m_repositionView);
        m_repositionSplitter->setStretchFactor(0, 0);
        m_repositionSplitter->setStretchFactor(1, 1);
        repLayout->addWidget(m_repositionSplitter, 1);

        connect(m_repositionView, &Mesh3DView::fullScreenToggleRequested,
                this, [this](Mesh3DView* source) {
                    toggleViewFullScreen(static_cast<QWidget*>(source));
                });
        connect(m_repositionView, &Mesh3DView::pointPicked,
                this, &MainWindow::onRepositionPivotPicked);
        connect(m_repositionView, &Mesh3DView::gizmoMeshUpdated,
                this, &MainWindow::onRepositionGizmoUpdated);

        m_viewModeStack->addWidget(repPanel);  // index 5
    }

    // FERULA workspace - page 6
    {
        auto* splintPanel = new QWidget(m_viewModeStack);
        auto* splintLayout = new QHBoxLayout(splintPanel);
        splintLayout->setContentsMargins(0, 0, 0, 0);
        splintLayout->setSpacing(6);

        auto* controlsHost = new QWidget(splintPanel);
        controlsHost->setObjectName("SplintControlsHost");
        auto* controls = new QVBoxLayout(controlsHost);
        controls->setContentsMargins(14, 12, 14, 12);
        controls->setSpacing(10);

        auto* title = new QLabel(tr("Diseno de ferula"), controlsHost);
        title->setObjectName("PanelTitle");
        controls->addWidget(title);

        auto* form = new QFormLayout();
        form->setLabelAlignment(Qt::AlignLeft);
        form->setFormAlignment(Qt::AlignTop);
        form->setHorizontalSpacing(10);
        form->setVerticalSpacing(8);

        m_splintDesignCombo = new QComboBox(controlsHost);
        m_splintDesignCombo->addItem(tr("Ferula intermedia"), 0);
        m_splintDesignCombo->addItem(tr("Ferula final"), 1);
        form->addRow(tr("Splint Design:"), m_splintDesignCombo);

        m_splintUpperPartCombo = new QComboBox(controlsHost);
        m_splintUpperPartCombo->addItem(tr("LeFort"), kLeFortSegLabel);
        m_splintUpperPartCombo->addItem(tr("Compuesto maxilar"), kUpperCompositeLabel);
        m_splintUpperPartCombo->addItem(tr("Maxilar"), 5);
        form->addRow(tr("Select Maxilla Parts:"), m_splintUpperPartCombo);

        m_splintLowerPartCombo = new QComboBox(controlsHost);
        m_splintLowerPartCombo->addItem(tr("Mandibula inicial"), kSplintInitialMandibleChoice);
        m_splintLowerPartCombo->addItem(tr("Mandibula final"), kSplintFinalMandibleChoice);
        m_splintLowerPartCombo->addItem(tr("Post-menton"), kGenioBodyLabel);
        m_splintLowerPartCombo->addItem(tr("Distal BSSO"), kBssoDistalLabel);
        m_splintLowerPartCombo->addItem(tr("Compuesto mandibular"), kLowerCompositeLabel);
        m_splintLowerPartCombo->addItem(tr("Mandibula"), 6);
        form->addRow(tr("Select Mandible Parts:"), m_splintLowerPartCombo);

        m_splintThicknessSpin = new QDoubleSpinBox(controlsHost);
        m_splintThicknessSpin->setRange(1.0, 12.0);
        m_splintThicknessSpin->setDecimals(1);
        m_splintThicknessSpin->setSingleStep(0.5);
        m_splintThicknessSpin->setSuffix(tr(" mm"));
        m_splintThicknessSpin->setValue(3.0);
        form->addRow(tr("Grosor vertical:"), m_splintThicknessSpin);

        controls->addLayout(form);

        auto* pointHelp = new QLabel(
            tr("Puede crear primero una ferula en herradura automatica y luego ajustarla con gizmo.\n\n"
               "Si quiere refinar el contorno, marque puntos opcionales sobre las superficies:\n"
               "- Maxilar vestibular\n"
               "- Maxilar palatino\n"
               "- Mandibula vestibular\n"
               "- Mandibula lingual\n\n"
               "Use varios puntos por superficie. Si no marca puntos, se generara una herradura base automatica."),
            controlsHost);
        pointHelp->setWordWrap(true);
        pointHelp->setObjectName("MutedText");
        controls->addWidget(pointHelp);

        auto makePointButton = [controlsHost](const QString& text) {
            auto* button = new QPushButton(text, controlsHost);
            button->setCheckable(true);
            button->setMinimumHeight(34);
            return button;
        };

        m_splintUpperVestibularButton = makePointButton(tr("Maxilar vestibular"));
        m_splintUpperPalatalButton = makePointButton(tr("Maxilar palatino"));
        m_splintLowerVestibularButton = makePointButton(tr("Mandibula vestibular"));
        m_splintLowerLingualButton = makePointButton(tr("Mandibula lingual"));
        controls->addWidget(m_splintUpperVestibularButton);
        controls->addWidget(m_splintUpperPalatalButton);
        controls->addWidget(m_splintLowerVestibularButton);
        controls->addWidget(m_splintLowerLingualButton);

        connect(m_splintUpperVestibularButton, &QPushButton::toggled, this,
                [this](bool checked) { setSplintPointCapture(checked ? SplintPointSet::UpperVestibular : SplintPointSet::None); });
        connect(m_splintUpperPalatalButton, &QPushButton::toggled, this,
                [this](bool checked) { setSplintPointCapture(checked ? SplintPointSet::UpperPalatal : SplintPointSet::None); });
        connect(m_splintLowerVestibularButton, &QPushButton::toggled, this,
                [this](bool checked) { setSplintPointCapture(checked ? SplintPointSet::LowerVestibular : SplintPointSet::None); });
        connect(m_splintLowerLingualButton, &QPushButton::toggled, this,
                [this](bool checked) { setSplintPointCapture(checked ? SplintPointSet::LowerLingual : SplintPointSet::None); });

        m_splintPointStatusLabel = new QLabel(controlsHost);
        m_splintPointStatusLabel->setObjectName("MutedText");
        m_splintPointStatusLabel->setWordWrap(true);
        controls->addWidget(m_splintPointStatusLabel);

        m_splintClearPointsButton = new QPushButton(tr("Limpiar puntos"), controlsHost);
        connect(m_splintClearPointsButton, &QPushButton::clicked,
                this, &MainWindow::clearSplintPoints);
        controls->addWidget(m_splintClearPointsButton);

        m_splintAdjustButton = new QPushButton(tr("Ajustar con gizmo"), controlsHost);
        connect(m_splintAdjustButton, &QPushButton::clicked,
                this, &MainWindow::startSplintGizmo);
        controls->addWidget(m_splintAdjustButton);

        m_splintAcceptAdjustButton = new QPushButton(tr("Aceptar ajuste"), controlsHost);
        connect(m_splintAcceptAdjustButton, &QPushButton::clicked,
                this, &MainWindow::acceptSplintGizmo);
        controls->addWidget(m_splintAcceptAdjustButton);

        m_splintCreateButton = new QPushButton(tr("Crear ferula"), controlsHost);
        connect(m_splintCreateButton, &QPushButton::clicked,
                this, &MainWindow::createSelectedSplint);
        controls->addWidget(m_splintCreateButton);
        controls->addStretch(1);

        auto* scroll = new QScrollArea(splintPanel);
        scroll->setWidgetResizable(true);
        scroll->setWidget(controlsHost);
        // Height-map method (new) and this classic panel, chosen with "Método".
        splintLayout->addWidget(buildSplintMethodPanel(splintPanel, scroll));

        auto* viewSplitter = new QSplitter(Qt::Vertical, splintPanel);
        auto* topSplitter = new QSplitter(Qt::Horizontal, viewSplitter);
        m_splintUpperView = new Mesh3DView(topSplitter);
        m_splintUpperView->setTitle(tr("MAXILAR / LE FORT"));
        m_splintLowerView = new Mesh3DView(topSplitter);
        m_splintLowerView->setTitle(tr("MANDIBULA"));
        topSplitter->addWidget(m_splintUpperView);
        topSplitter->addWidget(m_splintLowerView);
        topSplitter->setStretchFactor(0, 1);
        topSplitter->setStretchFactor(1, 1);

        m_splintView = new Mesh3DView(viewSplitter);
        m_splintView->setTitle(tr("PREVIEW FERULA"));
        viewSplitter->addWidget(topSplitter);
        viewSplitter->addWidget(m_splintView);
        viewSplitter->setStretchFactor(0, 1);
        viewSplitter->setStretchFactor(1, 1);
        splintLayout->addWidget(viewSplitter, 1);

        for (Mesh3DView* view : {m_splintUpperView, m_splintLowerView, m_splintView}) {
            connect(view, &Mesh3DView::fullScreenToggleRequested,
                    this, [this](Mesh3DView* source) {
                        toggleViewFullScreen(static_cast<QWidget*>(source));
                    });
            connect(view, &Mesh3DView::pointPicked,
                    this, &MainWindow::onSplintPointPicked);
            connect(view, &Mesh3DView::gizmoMeshUpdated,
                    this, &MainWindow::onSplintGizmoUpdated);
        }
        connectSplintHeightmapViews();

        auto syncSplintControls = [this] {
            updateSplintControls();
            syncSplintView();
            updateButtonStates();
        };
        connect(m_splintDesignCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, syncSplintControls);
        connect(m_splintUpperPartCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, syncSplintControls);
        connect(m_splintLowerPartCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, syncSplintControls);
        connect(m_splintThicknessSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, syncSplintControls);

        m_viewModeStack->addWidget(splintPanel);  // index 6
    }

    // GUIAS workspace - page 7
    {
        auto* guidePanel = new QWidget(m_viewModeStack);
        auto* guideLayout = new QHBoxLayout(guidePanel);
        guideLayout->setContentsMargins(0, 0, 0, 0);
        guideLayout->setSpacing(0);
        guideLayout->addWidget(buildGuideControlPanel(guidePanel));

        m_guideView = new Mesh3DView(guidePanel);
        m_guideView->setTitle(tr("GUÍAS QUIRÚRGICAS"));
        guideLayout->addWidget(m_guideView, 1);

        connect(m_guideView, &Mesh3DView::fullScreenToggleRequested, this,
                [this](Mesh3DView* source) { toggleViewFullScreen(static_cast<QWidget*>(source)); });
        connect(m_guideView, &Mesh3DView::pointPicked, this, &MainWindow::onGuidePointPicked);
        connect(m_guideView, &Mesh3DView::gizmoMeshUpdated, this, &MainWindow::onGuideGizmoUpdated);
        connect(m_guideView, &Mesh3DView::surfaceBrushed, this, &MainWindow::onGuideSurfaceBrushed);
        connect(m_guideView, &Mesh3DView::brushRadiusDragged, this, &MainWindow::onGuideBrushRadiusDragged);
        connect(m_guideView, &Mesh3DView::surfaceBrushFinished, this, &MainWindow::onGuideBrushFinished);

        m_viewModeStack->addWidget(guidePanel);  // index 7
    }

    hbox->addWidget(m_viewModeStack, 1);

    // ── Right panel (Project Manager) ──────────────────────────────────────
    m_infoPanel = buildInfoPanel();
    hbox->addWidget(m_infoPanel);

    setCentralWidget(root);
}

// ─────────────────────────────────────────────────────────────────────────────
// refreshModelViews
//
// Populates the individual model views and the match-preview from the meshes
// that are currently available (from segmentation and STL imports).
// Called every time the user enters the MODELOS workspace and whenever the
// active step changes (upper ↔ lower pair).
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::syncModelViews()
{
    // ── Individual dedicated views ────────────────────────────────────────
    if (auto maxilla = meshForAnatomicLabel(5)) {
        if (m_modelMaxillaView) {
            m_modelMaxillaView->addMesh(5, maxilla, meshLabelName(5));
            m_modelMaxillaView->setMeshColor(5, objectColorForLabel(5));
        }
    }
    if (auto mandible = meshForAnatomicLabel(6)) {
        if (m_modelMandibleView) {
            m_modelMandibleView->addMesh(6, mandible, meshLabelName(6));
            m_modelMandibleView->setMeshColor(6, objectColorForLabel(6));
        }
    }
    // Raw arch STLs (unregistered) — use objectActorKey so they don't collide
    // with label keys
    if (m_upperArchMesh) {
        const int k = objectActorKey(kUpperArchLabel);
        if (m_modelUpperArchView) {
            m_modelUpperArchView->addMesh(k, m_upperArchMesh, meshLabelName(kUpperArchLabel));
            m_modelUpperArchView->setMeshColor(k, objectColorForLabel(kUpperArchLabel));
        }
    }
    if (m_lowerArchMesh) {
        const int k = objectActorKey(kLowerArchLabel);
        if (m_modelLowerArchView) {
            m_modelLowerArchView->addMesh(k, m_lowerArchMesh, meshLabelName(kLowerArchLabel));
            m_modelLowerArchView->setMeshColor(k, objectColorForLabel(kLowerArchLabel));
        }
    }

    // ── Match preview — shows only the active pair ────────────────────────
    if (!m_modelMatchView) return;

    // Refresh geometry without resetting a camera the user has already positioned.
    m_modelMatchView->clearMeshes(true);

    const int step = m_modelStepStack ? m_modelStepStack->currentIndex() : 0;
    if (step == 0) {
        if (m_upperCompositeMesh) {
            const int ck = objectActorKey(kUpperCompositeLabel);
            m_modelMatchView->addMesh(ck, m_upperCompositeMesh, meshLabelName(kUpperCompositeLabel));
            m_modelMatchView->setMeshColor(ck, objectColorForLabel(kUpperCompositeLabel));
        } else {
            if (auto maxilla = meshForAnatomicLabel(5)) {
                m_modelMatchView->addMesh(5, maxilla, meshLabelName(5));
                m_modelMatchView->setMeshColor(5, objectColorForLabel(5));
            }
            if (m_upperArchMesh) {
                const int archKey = objectActorKey(kUpperArchLabel);
                m_modelMatchView->addMesh(archKey, m_upperArchMesh, meshLabelName(kUpperArchLabel));
                m_modelMatchView->setMeshColor(archKey, objectColorForLabel(kUpperArchLabel));
            }
        }
    } else {
        if (m_lowerCompositeMesh) {
            const int ck = objectActorKey(kLowerCompositeLabel);
            m_modelMatchView->addMesh(ck, m_lowerCompositeMesh, meshLabelName(kLowerCompositeLabel));
            m_modelMatchView->setMeshColor(ck, objectColorForLabel(kLowerCompositeLabel));
        } else {
            if (auto mandible = meshForAnatomicLabel(6)) {
                m_modelMatchView->addMesh(6, mandible, meshLabelName(6));
                m_modelMatchView->setMeshColor(6, objectColorForLabel(6));
            }
            if (m_lowerArchMesh) {
                const int rawKey = objectActorKey(kLowerArchLabel);
                m_modelMatchView->addMesh(rawKey, m_lowerArchMesh, meshLabelName(kLowerArchLabel));
                m_modelMatchView->setMeshColor(rawKey, objectColorForLabel(kLowerArchLabel));
            }
        }
    }
    syncVisibilityPanelToAllViews();
}

// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::setBiteRegistrationWorkspace(bool enabled)
{
    if (m_viewModeStack && enabled) {
        m_viewModeStack->setCurrentIndex(4);
        QApplication::processEvents();
    }

    if (!enabled) {
        m_bitePointSet = BitePointSet::None;
        if (m_biteSegmentView)
            m_biteSegmentView->setPointPickMode(false);
        if (m_biteScanView)
            m_biteScanView->setPointPickMode(false);
        if (m_biteRegistrationView) {
            m_biteRegistrationView->setPointPickMode(false);
            if (m_biteRegistrationView->hasGizmo())
                m_biteRegistrationView->stopGizmo();
        }
        m_biteGizmoActive = false;
        m_biteGizmoTargetLabel = -1;
        updateBiteRegistrationUi();
        return;
    }

    if (!m_biteSegmentView || !m_biteScanView || !m_biteRegistrationView) return;
    syncBiteRegistrationView();
    updateBiteRegistrationUi();
    for (Mesh3DView* view : {m_biteSegmentView, m_biteScanView, m_biteRegistrationView}) {
        if (!view) continue;
        view->setStandardView(0);
    }
    // Once the GL widgets are exposed, frame again in the frontal view.
    QTimer::singleShot(120, this, [this] {
        for (Mesh3DView* view : {m_biteSegmentView, m_biteScanView, m_biteRegistrationView}) {
            if (view) view->setStandardView(0);
        }
    });
    statusBar()->showMessage(
        tr("Registro de mordida: importe el STL de mordida y marque puntos Le Fort/mordida, luego distal/mordida."));
}

void MainWindow::importBiteScanStl()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Importar escaneo de mordida STL"), QString(), tr("STL (*.stl)"));
    if (path.isEmpty()) return;

    QString error;
    auto mesh = loadStlMesh(path, &error);
    if (!mesh) {
        QMessageBox::critical(this, tr("Escaneo de mordida"), error);
        return;
    }

    const ValidationResult vr = GeometryValidation::validateMeshScale(mesh, QStringLiteral("STL mordida"));
    if (vr.isError()) {
        QMessageBox::warning(this, tr("Escaneo de mordida - Escala"),
            tr("La malla tiene una escala inusual:\n%1\n\n"
               "Verifique que el STL este en milimetros.").arg(vr.message));
        LoggerCore::instance().logValidation(QStringLiteral("bite_scan"), vr.message, false);
        return;
    }
    if (vr.isWarn()) {
        statusBar()->showMessage(tr("Advertencia escala mordida: ") + vr.message);
        LoggerCore::instance().logValidation(QStringLiteral("bite_scan"), vr.message, true);
    }

    LoggerCore::instance().logStlLoad(path, QStringLiteral("bite_scan"));

    m_biteScanMesh = mesh;
    m_preBiteMandibleMeshForSplint = nullptr;
    m_orientedInitialMandibleMeshForSplint = nullptr;
    m_biteUpperScanPoints.clear();
    m_biteLowerScanPoints.clear();
    m_biteLeFortRegistered = false;
    m_biteMandibleRegistered = false;
    m_biteGizmoTargetLabel = -1;
    m_biteGizmoActive = false;
    m_intermediateSplintMesh = nullptr;
    m_finalSplintMesh = nullptr;
    m_biteLeFortRegistrationMatrix = identityMatrix();
    m_biteMandibleRegistrationMatrix = identityMatrix();
    m_biteLeFortRegistrationReport.clear();
    m_biteMandibleRegistrationReport.clear();

    const int actorKey = objectActorKey(kBiteScanLabel);
    const QColor color = objectColorForLabel(kBiteScanLabel);
    if (m_mesh3DView) {
        m_mesh3DView->addMesh(actorKey, mesh, meshLabelName(kBiteScanLabel));
        m_mesh3DView->setMeshColor(actorKey, color);
        m_mesh3DView->setMeshOpacity(actorKey, 0.58);
    }
    addObjectEntry(meshLabelName(kBiteScanLabel), objectColorForLabel(kBiteScanLabel), kBiteScanLabel);
    syncBiteRegistrationView();
    for (Mesh3DView* view : {m_biteSegmentView, m_biteScanView, m_biteRegistrationView}) {
        if (!view) continue;
        view->resetCamera();
        view->render();
    }
    updateBiteRegistrationUi();
    statusBar()->showMessage(tr("Escaneo de mordida importado. Marque puntos superiores e inferiores sobre la mordida."));
}

void MainWindow::setBitePointCapture(BitePointSet set)
{
    deactivateLassoTools();
    if (m_dentalPointSet != DentalPointSet::None)
        setDentalPointCapture(DentalPointSet::None);

    m_bitePointSet = set;
    const bool segmentMode = set == BitePointSet::LeFortSegment ||
                             set == BitePointSet::MandibleDistal;
    const bool scanMode = set == BitePointSet::UpperBiteScan ||
                          set == BitePointSet::LowerBiteScan;
    if (m_biteSegmentView)
        m_biteSegmentView->setPointPickMode(segmentMode);
    if (m_biteScanView)
        m_biteScanView->setPointPickMode(scanMode);
    if (m_biteRegistrationView)
        m_biteRegistrationView->setPointPickMode(set != BitePointSet::None);
    updateBiteRegistrationUi();

    QString mode = tr("Captura de puntos de mordida desactivada");
    switch (set) {
        case BitePointSet::LeFortSegment:  mode = tr("Seleccione puntos en el segmento Le Fort I"); break;
        case BitePointSet::UpperBiteScan:  mode = tr("Seleccione puntos superiores en el escaneo de mordida"); break;
        case BitePointSet::MandibleDistal: mode = tr("Seleccione puntos en el segmento mandibular distal"); break;
        case BitePointSet::LowerBiteScan:  mode = tr("Seleccione puntos inferiores en el escaneo de mordida"); break;
        case BitePointSet::None: break;
    }
    statusBar()->showMessage(mode);
}

void MainWindow::onBitePointPicked(int actorLabel, double x, double y, double z)
{
    if (m_bitePointSet == BitePointSet::None) return;

    const bool leFortActor = actorLabel == kLeFortSegLabel || actorLabel == objectActorKey(kLeFortSegLabel);
    const int mandibleTarget = currentBiteMandibleTargetLabel();
    const bool distalActor = mandibleTarget > 0 &&
        (actorLabel == mandibleTarget || actorLabel == objectActorKey(mandibleTarget));
    const bool chinActor = actorLabel == kGenioSegmentLabel ||
                           actorLabel == objectActorKey(kGenioSegmentLabel);
    const bool biteActor = actorLabel == kBiteScanLabel || actorLabel == objectActorKey(kBiteScanLabel);

    QVector<QVector3D>* target = nullptr;
    bool accepted = false;
    switch (m_bitePointSet) {
        case BitePointSet::LeFortSegment:
            accepted = leFortActor;
            target = &m_biteLeFortSegmentPoints;
            break;
        case BitePointSet::UpperBiteScan:
            accepted = biteActor;
            target = &m_biteUpperScanPoints;
            break;
        case BitePointSet::MandibleDistal:
            if (chinActor) {
                statusBar()->showMessage(tr("Punto rechazado: marque la mandibula distal/post-menton, no el segmento de menton."));
                return;
            }
            accepted = distalActor;
            target = &m_biteMandibleSegmentPoints;
            break;
        case BitePointSet::LowerBiteScan:
            accepted = biteActor;
            target = &m_biteLowerScanPoints;
            break;
        case BitePointSet::None:
            break;
    }

    if (!accepted || !target) {
        statusBar()->showMessage(tr("Punto rechazado: seleccione la superficie correcta para este paso."));
        return;
    }

    target->append(QVector3D(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)));
    rebuildBitePointMarkers();
    updateBiteRegistrationUi();
    statusBar()->showMessage(
        tr("Registro mordida | LeFort:%1 MordSup:%2 Distal:%3 MordInf:%4")
            .arg(m_biteLeFortSegmentPoints.size())
            .arg(m_biteUpperScanPoints.size())
            .arg(m_biteMandibleSegmentPoints.size())
            .arg(m_biteLowerScanPoints.size()));
}

void MainWindow::clearBiteRegistrationPoints()
{
    m_biteLeFortSegmentPoints.clear();
    m_biteUpperScanPoints.clear();
    m_biteMandibleSegmentPoints.clear();
    m_biteLowerScanPoints.clear();
    m_bitePointSet = BitePointSet::None;
    if (m_biteSegmentView)
        m_biteSegmentView->setPointPickMode(false);
    if (m_biteScanView)
        m_biteScanView->setPointPickMode(false);
    if (m_biteRegistrationView)
        m_biteRegistrationView->setPointPickMode(false);
    rebuildBitePointMarkers();
    updateBiteRegistrationUi();
    statusBar()->showMessage(tr("Registro de mordida: puntos eliminados."));
}

void MainWindow::rebuildBitePointMarkers()
{
    for (Mesh3DView* view : {m_biteSegmentView, m_biteScanView, m_biteRegistrationView}) {
        if (view) view->clearPointMarkers();
    }

    auto addList = [](Mesh3DView* view, const QVector<QVector3D>& points, const QColor& color) {
        if (!view) return;
        for (const QVector3D& p : points)
            view->addPointMarker(p.x(), p.y(), p.z(), color);
    };

    const QColor leFortColor(255, 95, 95);
    const QColor upperScanColor(255, 190, 75);
    const QColor mandibleColor(90, 190, 255);
    const QColor lowerScanColor(95, 230, 220);

    addList(m_biteSegmentView, m_biteLeFortSegmentPoints, leFortColor);
    addList(m_biteSegmentView, m_biteMandibleSegmentPoints, mandibleColor);
    addList(m_biteScanView, m_biteUpperScanPoints, upperScanColor);
    addList(m_biteScanView, m_biteLowerScanPoints, lowerScanColor);

    addList(m_biteRegistrationView, m_biteLeFortSegmentPoints, leFortColor);
    addList(m_biteRegistrationView, m_biteUpperScanPoints, upperScanColor);
    addList(m_biteRegistrationView, m_biteMandibleSegmentPoints, mandibleColor);
    addList(m_biteRegistrationView, m_biteLowerScanPoints, lowerScanColor);
}

void MainWindow::syncBiteRegistrationView()
{
    if (!m_biteSegmentView || !m_biteScanView || !m_biteRegistrationView) return;

    for (Mesh3DView* view : {m_biteSegmentView, m_biteScanView, m_biteRegistrationView}) {
        if (!view) continue;
        view->clearMeshes();
        view->clearPointMarkers();
    }

    auto add = [this](Mesh3DView* view, int label, vtkPolyData* mesh, double opacity = 1.0) {
        if (!view || !mesh || mesh->GetNumberOfPoints() <= 0) return;
        const int actorKey = objectActorKey(label);
        view->addMesh(actorKey, mesh, meshLabelName(label));
        view->setMeshColor(actorKey, objectColorForLabel(label));
        view->setMeshOpacity(actorKey, opacity);
        view->setMeshVisible(actorKey, objectEntryVisible(label));
    };

    // Le Fort segment
    {
        auto mesh = repositionMeshForLabel(kLeFortSegLabel);
        add(m_biteSegmentView, kLeFortSegLabel, mesh, 1.0);
        add(m_biteRegistrationView, kLeFortSegLabel, mesh, 1.0);
    }
    // Mandible target — only the single active distal segment, no proximal ramas or chin piece
    {
        const int mandTarget = currentBiteMandibleTargetLabel();
        if (mandTarget > 0) {
            auto mesh = repositionMeshForLabel(mandTarget);
            add(m_biteSegmentView, mandTarget, mesh, 1.0);
            add(m_biteRegistrationView, mandTarget, mesh, 1.0);
            const bool showChinAsBlock =
                mandTarget == kGenioBodyLabel &&
                (m_biteMandibleRegistered || m_biteGizmoActive) &&
                m_bitePointSet != BitePointSet::MandibleDistal;
            if (showChinAsBlock) {
                auto chinMesh = repositionMeshForLabel(kGenioSegmentLabel);
                add(m_biteRegistrationView, kGenioSegmentLabel, chinMesh, 1.0);
            }
        }
    }

    auto biteMesh = repositionMeshForLabel(kBiteScanLabel);
    if (!m_biteScanMesh && biteMesh)
        m_biteScanMesh = biteMesh;
    add(m_biteScanView, kBiteScanLabel, biteMesh, 1.0);
    add(m_biteRegistrationView, kBiteScanLabel, biteMesh, 0.58);
    rebuildBitePointMarkers();
    syncVisibilityPanelToAllViews();
    for (Mesh3DView* view : {m_biteSegmentView, m_biteScanView, m_biteRegistrationView}) {
        if (!view) continue;
        view->resetCamera();
        view->render();
    }
}

int MainWindow::currentBiteMandibleTargetLabel() const
{
    // Bite registration must use the final tooth-bearing mandibular segment.
    // Do not let visibility decide the target: hiding/showing objects in the
    // Project Manager should not silently switch the surgical model being moved.
    const QVector<int> candidates {
        kGenioBodyLabel,
        kBssoDistalLabel,
        kLowerCompositeLabel,
        6
    };

    for (int label : candidates) {
        auto mesh = repositionMeshForLabel(label);
        if (mesh && mesh->GetNumberOfPoints() > 0)
            return label;
    }
    return -1;
}

void MainWindow::updateBiteRegistrationUi()
{
    auto biteMesh = repositionMeshForLabel(kBiteScanLabel);
    if (!m_biteScanMesh && biteMesh)
        m_biteScanMesh = biteMesh;
    const bool hasBite = biteMesh && biteMesh->GetNumberOfPoints() > 0;
    auto leFortMesh = repositionMeshForLabel(kLeFortSegLabel);
    const bool hasLeFort = leFortMesh && leFortMesh->GetNumberOfPoints() > 0;
    const int mandibleTarget = currentBiteMandibleTargetLabel();
    const bool hasDistal = mandibleTarget > 0 && repositionMeshForLabel(mandibleTarget);
    const bool canLeFort =
        hasBite && hasLeFort &&
        std::min(m_biteLeFortSegmentPoints.size(), m_biteUpperScanPoints.size()) >= 3;
    const bool canMandible =
        hasBite && hasDistal &&
        std::min(m_biteMandibleSegmentPoints.size(), m_biteLowerScanPoints.size()) >= 3;
    const bool hasAnyPoints =
        !m_biteLeFortSegmentPoints.isEmpty() || !m_biteUpperScanPoints.isEmpty() ||
        !m_biteMandibleSegmentPoints.isEmpty() || !m_biteLowerScanPoints.isEmpty();

    if (m_importBiteScanAct) m_importBiteScanAct->setEnabled(true);
    if (m_biteLeFortPtsAct) m_biteLeFortPtsAct->setEnabled(hasLeFort);
    if (m_biteUpperScanPtsAct) m_biteUpperScanPtsAct->setEnabled(hasBite);
    if (m_biteRegisterLeFortAct) m_biteRegisterLeFortAct->setEnabled(canLeFort);
    if (m_biteMandiblePtsAct) m_biteMandiblePtsAct->setEnabled(hasDistal);
    if (m_biteLowerScanPtsAct) m_biteLowerScanPtsAct->setEnabled(hasBite);
    if (m_biteRegisterMandibleAct) m_biteRegisterMandibleAct->setEnabled(canMandible);
    if (m_biteAdjustGizmoAct) {
        const int adjustTarget = m_biteMandibleRegistered
            ? mandibleTarget
            : (m_biteLeFortRegistered ? kBiteScanLabel : -1);
        const bool canAdjust = adjustTarget > 0 && repositionMeshForLabel(adjustTarget);
        m_biteAdjustGizmoAct->setEnabled(canAdjust && !m_biteGizmoActive);
    }
    if (m_biteAcceptGizmoAct) m_biteAcceptGizmoAct->setEnabled(m_biteGizmoActive);
    if (m_biteClearPtsAct) m_biteClearPtsAct->setEnabled(hasAnyPoints);

    auto check = [](QAction* action, bool checked) {
        if (!action) return;
        const QSignalBlocker blocker(action);
        action->setChecked(checked);
    };
    check(m_biteLeFortPtsAct, m_bitePointSet == BitePointSet::LeFortSegment);
    check(m_biteUpperScanPtsAct, m_bitePointSet == BitePointSet::UpperBiteScan);
    check(m_biteMandiblePtsAct, m_bitePointSet == BitePointSet::MandibleDistal);
    check(m_biteLowerScanPtsAct, m_bitePointSet == BitePointSet::LowerBiteScan);
}

void MainWindow::transformBiteScanPointLists(vtkMatrix4x4* matrix)
{
    if (!matrix) return;
    auto transformPointList = [matrix](QVector<QVector3D>& points) {
        for (QVector3D& point : points) {
            const double in[4] = {point.x(), point.y(), point.z(), 1.0};
            double out[4] = {};
            matrix->MultiplyPoint(in, out);
            point = QVector3D(static_cast<float>(out[0]),
                              static_cast<float>(out[1]),
                              static_cast<float>(out[2]));
        }
    };
    transformPointList(m_biteUpperScanPoints);
    transformPointList(m_biteLowerScanPoints);
}

void MainWindow::applyBiteMandibleBlockTransform(int mandibleLabel, vtkMatrix4x4* matrix)
{
    if (!matrix || TransformCore::IsIdentity(matrix)) return;

    const bool mandibularTarget =
        mandibleLabel == kGenioBodyLabel ||
        mandibleLabel == kBssoDistalLabel ||
        mandibleLabel == kLowerCompositeLabel ||
        mandibleLabel == 6;
    if (!mandibularTarget) return;

    auto chinMesh = repositionMeshForLabel(kGenioSegmentLabel);
    if (!chinMesh || chinMesh->GetNumberOfPoints() <= 0) return;

    auto movedChin = TransformCore::ApplyTransformToPolyData(chinMesh, matrix);
    if (!movedChin || movedChin->GetNumberOfPoints() <= 0) return;

    setRepositionMeshForLabel(kGenioSegmentLabel, movedChin);
    m_repositionOriginalMeshes.erase(kGenioSegmentLabel);
    m_biteMandibleRegistrationReport +=
        tr("\nEl segmento de menton se movio con la mandibula como bloque.\n");
}

void MainWindow::startBiteAdjustmentGizmo(int label)
{
    if (!m_biteRegistrationView) return;

    auto mesh = repositionMeshForLabel(label);
    if (label <= 0 || !mesh || mesh->GetNumberOfPoints() <= 0) {
        statusBar()->showMessage(tr("Registro mordida: no hay match disponible para ajustar."));
        return;
    }

    setBitePointCapture(BitePointSet::None);
    deactivateLassoTools();
    syncBiteRegistrationView();

    const int actorKey = objectActorKey(label);
    if (!m_biteRegistrationView->meshData(actorKey)) {
        statusBar()->showMessage(tr("Registro mordida: no se encontro el objeto en la vista de match."));
        return;
    }

    if (m_biteRegistrationView->hasGizmo())
        m_biteRegistrationView->stopGizmo();
    m_biteRegistrationView->startGizmo(actorKey);
    if (!m_biteRegistrationView->hasGizmo()) {
        statusBar()->showMessage(tr("Registro mordida: no se pudo activar el gizmo."));
        return;
    }

    if (m_viewFullScreen && m_fullScreenView != m_biteRegistrationView)
        exitViewFullScreen();
    if (!m_viewFullScreen)
        toggleViewFullScreen(m_biteRegistrationView);

    m_biteGizmoTargetLabel = label;
    m_biteGizmoActive = true;
    updateBiteRegistrationUi();
    statusBar()->showMessage(tr("Gizmo activo para %1. Ajuste y pulse Aceptar Ajuste.").arg(meshLabelName(label)));
}

void MainWindow::acceptBiteAdjustmentGizmo()
{
    if (m_biteRegistrationView && m_biteRegistrationView->hasGizmo())
        m_biteRegistrationView->stopGizmo();
    m_biteGizmoActive = false;
    m_biteGizmoTargetLabel = -1;
    updateBiteRegistrationUi();
    statusBar()->showMessage(tr("Registro mordida: ajuste manual aceptado."));
}

void MainWindow::onBiteGizmoMeshUpdated(int meshLabel, vtkSmartPointer<vtkPolyData> newMesh)
{
    if (!newMesh || newMesh->GetNumberOfPoints() <= 0) return;

    int label = meshLabel >= 1000 ? meshLabel - 1000 : meshLabel;
    if (m_biteGizmoTargetLabel > 0)
        label = m_biteGizmoTargetLabel;
    if (label <= 0) return;

    auto baked = vtkSmartPointer<vtkPolyData>::New();
    baked->DeepCopy(newMesh);

    auto* sourceView = qobject_cast<Mesh3DView*>(sender());
    auto manualDelta = sourceView
        ? sourceView->lastGizmoTransformMatrix()
        : TransformCore::IdentityMatrix();

    setRepositionMeshForLabel(label, baked);
    if (label == kBiteScanLabel) {
        transformBiteScanPointLists(manualDelta);
        if (m_biteLeFortRegistrationMatrix && manualDelta)
            m_biteLeFortRegistrationMatrix = TransformCore::ComposeTransforms(
                {m_biteLeFortRegistrationMatrix.GetPointer(), manualDelta.GetPointer()});
        m_biteLeFortRegistrationReport +=
            tr("\nAjuste manual con gizmo aplicado al escaneo de mordida.\n");
        m_repositionOriginalMeshes.erase(kBiteScanLabel);
    } else {
        applyBiteMandibleBlockTransform(label, manualDelta);
        recordMandibleMovement(label, manualDelta, true);
        m_mandibleMovementResetMatrix.clear();
        if (m_biteMandibleRegistrationMatrix && manualDelta)
            m_biteMandibleRegistrationMatrix = TransformCore::ComposeTransforms(
                {m_biteMandibleRegistrationMatrix.GetPointer(), manualDelta.GetPointer()});
        m_biteMandibleRegistrationReport +=
            tr("\nAjuste manual con gizmo aplicado al segmento mandibular.\n");
        m_repositionOriginalMeshes.erase(label);
    }

    m_biteGizmoActive = false;
    m_biteGizmoTargetLabel = -1;
    syncBiteRegistrationView();
    updateBiteRegistrationUi();
}

vtkSmartPointer<vtkPolyData> MainWindow::refineBiteRegistrationWithIcp(
    vtkPolyData* moving, vtkPolyData* biteScan, QString* report, QString* error,
    vtkMatrix4x4* outputMatrix) const
{
    if (outputMatrix) outputMatrix->Identity();

    if (!moving || !biteScan ||
        moving->GetNumberOfPoints() == 0 || biteScan->GetNumberOfPoints() == 0) {
        if (error) *error = tr("No hay mallas validas para refinar el registro de mordida.");
        return nullptr;
    }

    auto cleanTri = [](vtkPolyData* input) -> vtkSmartPointer<vtkPolyData> {
        auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
        clean->SetInputData(input);
        clean->Update();

        auto tri = vtkSmartPointer<vtkTriangleFilter>::New();
        tri->SetInputConnection(clean->GetOutputPort());
        tri->Update();

        auto out = vtkSmartPointer<vtkPolyData>::New();
        out->DeepCopy(tri->GetOutput());
        return out;
    };

    auto cropByBox = [](vtkPolyData* input, const double box[6]) -> vtkSmartPointer<vtkPolyData> {
        if (!input || input->GetNumberOfCells() == 0) return nullptr;
        auto polys = vtkSmartPointer<vtkCellArray>::New();
        auto ids = vtkSmartPointer<vtkIdList>::New();
        double p[3] = {};
        double c[3] = {};

        for (vtkIdType cellId = 0; cellId < input->GetNumberOfCells(); ++cellId) {
            input->GetCellPoints(cellId, ids);
            const vtkIdType n = ids->GetNumberOfIds();
            if (n < 3) continue;
            c[0] = c[1] = c[2] = 0.0;
            for (vtkIdType i = 0; i < n; ++i) {
                input->GetPoint(ids->GetId(i), p);
                c[0] += p[0]; c[1] += p[1]; c[2] += p[2];
            }
            c[0] /= static_cast<double>(n);
            c[1] /= static_cast<double>(n);
            c[2] /= static_cast<double>(n);

            const bool inside = c[0] >= box[0] && c[0] <= box[1] &&
                                c[1] >= box[2] && c[1] <= box[3] &&
                                c[2] >= box[4] && c[2] <= box[5];
            if (inside) polys->InsertNextCell(ids);
        }

        auto out = vtkSmartPointer<vtkPolyData>::New();
        out->SetPoints(input->GetPoints());
        out->SetPolys(polys);

        auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
        clean->SetInputData(out);
        clean->Update();

        auto result = vtkSmartPointer<vtkPolyData>::New();
        result->DeepCopy(clean->GetOutput());
        return result;
    };

    auto keepNearTarget = [](vtkPolyData* input, vtkPolyData* target,
                             double maxDistance) -> vtkSmartPointer<vtkPolyData> {
        if (!input || !target || input->GetNumberOfCells() == 0 || target->GetNumberOfPoints() == 0)
            return nullptr;

        auto distance = vtkSmartPointer<vtkImplicitPolyDataDistance>::New();
        distance->SetInput(target);

        auto polys = vtkSmartPointer<vtkCellArray>::New();
        auto ids = vtkSmartPointer<vtkIdList>::New();
        double p[3] = {};
        double c[3] = {};
        for (vtkIdType cellId = 0; cellId < input->GetNumberOfCells(); ++cellId) {
            input->GetCellPoints(cellId, ids);
            const vtkIdType n = ids->GetNumberOfIds();
            if (n < 3) continue;

            c[0] = c[1] = c[2] = 0.0;
            for (vtkIdType i = 0; i < n; ++i) {
                input->GetPoint(ids->GetId(i), p);
                c[0] += p[0]; c[1] += p[1]; c[2] += p[2];
            }
            c[0] /= static_cast<double>(n);
            c[1] /= static_cast<double>(n);
            c[2] /= static_cast<double>(n);

            bool keep = std::abs(distance->EvaluateFunction(c)) <= maxDistance;
            if (!keep) {
                for (vtkIdType i = 0; i < n; ++i) {
                    input->GetPoint(ids->GetId(i), p);
                    if (std::abs(distance->EvaluateFunction(p)) <= maxDistance) {
                        keep = true;
                        break;
                    }
                }
            }
            if (keep) polys->InsertNextCell(ids);
        }

        auto out = vtkSmartPointer<vtkPolyData>::New();
        out->SetPoints(input->GetPoints());
        out->SetPolys(polys);

        auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
        clean->SetInputData(out);
        clean->Update();

        auto result = vtkSmartPointer<vtkPolyData>::New();
        result->DeepCopy(clean->GetOutput());
        return result;
    };

    auto movingTri = cleanTri(moving);
    auto biteTri = cleanTri(biteScan);
    if (!movingTri || !biteTri || movingTri->GetNumberOfPoints() == 0 || biteTri->GetNumberOfPoints() == 0) {
        if (error) *error = tr("No se pudieron limpiar las mallas para ICP de mordida.");
        return nullptr;
    }

    double biteBounds[6] = {};
    biteTri->GetBounds(biteBounds);
    const double sx = std::max(1.0, biteBounds[1] - biteBounds[0]);
    const double sy = std::max(1.0, biteBounds[3] - biteBounds[2]);
    const double sz = std::max(1.0, biteBounds[5] - biteBounds[4]);
    const double biteDiag = std::sqrt(sx * sx + sy * sy + sz * sz);
    const double pad = std::clamp(biteDiag * 0.08, 4.0, 12.0);
    double roiBox[6] = {
        biteBounds[0] - pad, biteBounds[1] + pad,
        biteBounds[2] - pad, biteBounds[3] + pad,
        biteBounds[4] - pad, biteBounds[5] + pad
    };

    auto movingRoi = cropByBox(movingTri, roiBox);
    const double keepDistance = std::clamp(biteDiag * 0.05, 3.0, 7.0);
    auto movingLocal = keepNearTarget(movingRoi, biteTri, keepDistance);
    if (!movingLocal || movingLocal->GetNumberOfPoints() < 50) {
        movingLocal = movingRoi;
    }

    if (!movingLocal || movingLocal->GetNumberOfPoints() < 50 || biteTri->GetNumberOfPoints() < 50) {
        if (report) {
            *report = tr("ICP local mordida: omitido; ROI insuficiente despues de landmarks.");
        }
        auto out = vtkSmartPointer<vtkPolyData>::New();
        out->DeepCopy(movingTri);
        return out;
    }

    auto icp = vtkSmartPointer<vtkIterativeClosestPointTransform>::New();
    icp->SetSource(movingLocal);
    icp->SetTarget(biteTri);
    icp->GetLandmarkTransform()->SetModeToRigidBody();
    icp->StartByMatchingCentroidsOff();
    icp->CheckMeanDistanceOn();
    icp->SetMaximumNumberOfIterations(80);
    icp->SetMaximumMeanDistance(0.001);
    icp->Update();

    if (outputMatrix)
        outputMatrix->DeepCopy(icp->GetMatrix());

    auto fullFilter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
    fullFilter->SetInputData(movingTri);
    fullFilter->SetTransform(icp);
    fullFilter->Update();

    auto localFilter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
    localFilter->SetInputData(movingLocal);
    localFilter->SetTransform(icp);
    localFilter->Update();

    auto distance = vtkSmartPointer<vtkImplicitPolyDataDistance>::New();
    distance->SetInput(biteTri);

    vtkPolyData* local = localFilter->GetOutput();
    const vtkIdType total = local ? local->GetNumberOfPoints() : 0;
    const vtkIdType step = std::max<vtkIdType>(1, total / 25000);
    double p[3] = {};
    double sum = 0.0;
    double sumSq = 0.0;
    double maxDist = 0.0;
    vtkIdType count = 0;
    for (vtkIdType i = 0; i < total; i += step) {
        local->GetPoint(i, p);
        const double d = std::abs(distance->EvaluateFunction(p));
        sum += d;
        sumSq += d * d;
        maxDist = std::max(maxDist, d);
        ++count;
    }

    const double mean = count > 0 ? sum / static_cast<double>(count) : 0.0;
    const double rmse = count > 0 ? std::sqrt(sumSq / static_cast<double>(count)) : 0.0;
    if (report) {
        *report = tr("ICP local mordida: RMSE %1 mm | media %2 mm | max %3 mm | puntos ROI %4")
            .arg(rmse, 0, 'f', 3)
            .arg(mean, 0, 'f', 3)
            .arg(maxDist, 0, 'f', 3)
            .arg(count);
    }

    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(fullFilter->GetOutput());
    return out;
}

void MainWindow::alignLeFortToBiteScan()
{
    auto biteMesh = repositionMeshForLabel(kBiteScanLabel);
    if (!m_biteScanMesh && biteMesh)
        m_biteScanMesh = biteMesh;
    if (!biteMesh) {
        QMessageBox::warning(this, tr("Registro Le Fort"), tr("Importe primero el escaneo de mordida STL."));
        return;
    }
    auto leFortMesh = repositionMeshForLabel(kLeFortSegLabel);
    if (!leFortMesh) {
        QMessageBox::warning(this, tr("Registro Le Fort"), tr("Realice primero la osteotomia Le Fort I."));
        return;
    }

    QString error;
    auto landmarkMatrix = identityMatrix();
    auto icpMatrix = identityMatrix();
    double landmarkRms = 0.0;
    // Clinical occlusion workflow:
    // 1) Keep the Le Fort segment where the surgeon positioned/planned it.
    // 2) Adapt the bite registration STL to the Le Fort segment.
    // 3) Then adapt the mandibular segment to the already positioned bite.
    auto transformed = transformMesh(biteMesh, m_biteUpperScanPoints,
                                     m_biteLeFortSegmentPoints, &error,
                                     landmarkMatrix, &landmarkRms);
    if (!transformed) {
        QMessageBox::warning(this, tr("Registro Le Fort"), error);
        return;
    }

    QString icpReport;
    QString icpError;
    if (auto refined = refineBiteRegistrationWithIcp(transformed, leFortMesh,
                                                     &icpReport, &icpError, icpMatrix)) {
        transformed = refined;
    } else if (!icpError.isEmpty()) {
        icpReport = tr("ICP local mordida: omitido (%1)").arg(icpError);
    }

    auto finalMatrix = TransformCore::ComposeTransforms(
        {landmarkMatrix.GetPointer(), icpMatrix.GetPointer()});
    m_biteLeFortRegistrationMatrix = finalMatrix;
    m_biteLeFortRegistrationReport =
        tr("Segmento fijo: Le Fort I\n"
           "Objeto movido: escaneo de mordida superior\n"
           "Landmark RMS: %1 mm\n"
           "%2\n\n"
           "Matriz landmarks:\n%3\n\n"
           "Matriz ICP:\n%4\n\n"
           "Matriz final T_bite_to_LeFort:\n%5\n")
            .arg(landmarkRms, 0, 'f', 3)
            .arg(icpReport.isEmpty() ? tr("ICP local mordida: omitido o sin reporte") : icpReport)
            .arg(matrixToText(landmarkMatrix))
            .arg(matrixToText(icpMatrix))
            .arg(matrixToText(finalMatrix));

    const QString summary =
        tr("Registro Le Fort - mordida completado.\n\n"
           "LM-RMS: %1 mm\n"
           "%2\n\n"
           "Aceptar y adaptar el escaneo de mordida a la posicion del Le Fort I?")
            .arg(landmarkRms, 0, 'f', 3)
            .arg(icpReport.isEmpty() ? tr("(ICP local no aplicado)") : icpReport);
    const auto ret = QMessageBox::question(
        this, tr("Registro Le Fort - Mordida"), summary,
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (ret != QMessageBox::Yes) {
        statusBar()->showMessage(tr("Registro Le Fort - mordida rechazado."));
        return;
    }

    setRepositionMeshForLabel(kBiteScanLabel, transformed);
    transformBiteScanPointLists(finalMatrix);
    m_repositionOriginalMeshes.erase(kBiteScanLabel);
    m_biteLeFortRegistered = true;
    m_biteLeFortSegmentPoints.clear();
    m_biteUpperScanPoints.clear();
    setBitePointCapture(BitePointSet::None);
    syncBiteRegistrationView();
    updateBiteRegistrationUi();
    statusBar()->showMessage(tr("Mordida adaptada al Le Fort I. Ajuste el match con gizmo si es necesario."));
    startBiteAdjustmentGizmo(kBiteScanLabel);
}

void MainWindow::alignMandibleDistalToBiteScan()
{
    auto biteMesh = repositionMeshForLabel(kBiteScanLabel);
    if (!m_biteScanMesh && biteMesh)
        m_biteScanMesh = biteMesh;
    if (!biteMesh) {
        QMessageBox::warning(this, tr("Registro mandibular"), tr("Importe primero el escaneo de mordida STL."));
        return;
    }
    const int mandibleTarget = currentBiteMandibleTargetLabel();
    auto mandibleMesh = repositionMeshForLabel(mandibleTarget);
    if (mandibleTarget <= 0 || !mandibleMesh) {
        QMessageBox::warning(this, tr("Registro mandibular"), tr("No se encontro una mandibula activa para registrar."));
        return;
    }
    if (!m_preBiteMandibleMeshForSplint) {
        m_preBiteMandibleMeshForSplint = vtkSmartPointer<vtkPolyData>::New();
        m_preBiteMandibleMeshForSplint->DeepCopy(mandibleMesh);
    }

    QString error;
    auto landmarkMatrix = identityMatrix();
    auto icpMatrix = identityMatrix();
    double landmarkRms = 0.0;
    auto transformed = transformMesh(mandibleMesh, m_biteMandibleSegmentPoints,
                                     m_biteLowerScanPoints, &error, landmarkMatrix, &landmarkRms);
    if (!transformed) {
        QMessageBox::warning(this, tr("Registro mandibular"), error);
        return;
    }

    QString icpReport;
    QString icpError;
    if (auto refined = refineBiteRegistrationWithIcp(transformed, biteMesh,
                                                     &icpReport, &icpError, icpMatrix)) {
        transformed = refined;
    } else if (!icpError.isEmpty()) {
        icpReport = tr("ICP local mordida: omitido (%1)").arg(icpError);
    }

    auto finalMatrix = TransformCore::ComposeTransforms(
        {landmarkMatrix.GetPointer(), icpMatrix.GetPointer()});
    m_biteMandibleRegistrationReport =
        tr("Segmento: %1\n"
           "Referencia: escaneo de mordida inferior\n"
           "Landmark RMS: %2 mm\n"
           "%3\n\n"
           "Matriz landmarks:\n%4\n\n"
           "Matriz ICP:\n%5\n\n"
           "Matriz final:\n%6\n")
            .arg(meshLabelName(mandibleTarget))
            .arg(landmarkRms, 0, 'f', 3)
            .arg(icpReport.isEmpty() ? tr("ICP local mordida: omitido o sin reporte") : icpReport)
            .arg(matrixToText(landmarkMatrix))
            .arg(matrixToText(icpMatrix))
            .arg(matrixToText(finalMatrix));

    const QString summary =
        tr("Registro mandibular distal - mordida completado.\n\n"
           "LM-RMS: %1 mm\n"
           "%2\n\n"
           "Aceptar y mover %3?")
            .arg(landmarkRms, 0, 'f', 3)
            .arg(icpReport.isEmpty() ? tr("(ICP local no aplicado)") : icpReport)
            .arg(meshLabelName(mandibleTarget));
    const auto ret = QMessageBox::question(
        this, tr("Registro mandibular - Mordida"), summary,
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (ret != QMessageBox::Yes) {
        statusBar()->showMessage(tr("Registro mandibular - mordida rechazado."));
        return;
    }

    captureMandibleMovementReference(mandibleTarget, mandibleMesh);
    recordMandibleMovement(mandibleTarget, finalMatrix, true);
    m_mandibleMovementResetMatrix.clear();
    m_biteMandibleRegistrationMatrix = m_biteMandibleRegistered
        ? TransformCore::ComposeTransforms({m_biteMandibleRegistrationMatrix.GetPointer(), finalMatrix.GetPointer()})
        : TransformCore::CloneMatrix(finalMatrix);
    setRepositionMeshForLabel(mandibleTarget, transformed);
    applyBiteMandibleBlockTransform(mandibleTarget, finalMatrix);
    m_repositionOriginalMeshes.erase(mandibleTarget);
    m_biteMandibleRegistered = true;
    m_biteMandibleSegmentPoints.clear();
    m_biteLowerScanPoints.clear();
    setBitePointCapture(BitePointSet::None);
    syncBiteRegistrationView();
    updateBiteRegistrationUi();
    statusBar()->showMessage(tr("Registro mandibular - mordida aplicado. Ajuste el bloque con gizmo si es necesario."));
    startBiteAdjustmentGizmo(mandibleTarget);
}

QWidget* MainWindow::buildRepositionControlPanel(QWidget* parent)
{
    auto* scroll = new QScrollArea(parent);
    scroll->setObjectName("RepositionScrollArea");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMinimumWidth(340);
    scroll->setMaximumWidth(380);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* panel = new QWidget(scroll);
    panel->setObjectName("RepositionPanel");
    panel->setStyleSheet(
        "#RepositionPanel { background:#1c1c1e; border-right:1px solid #2c2c2e; }"
        "#RepositionPanel QLabel { color:#f5f5f7; font-size:11px; }"
        "#RepositionPanel QLabel#PanelTitle { font-size:15px; font-weight:700; }"
        "#RepositionPanel QLabel#SectionTitle { color:#0a84ff; font-size:11px; font-weight:700; margin-top:8px; }"
        "#RepositionPanel QPushButton { background:#2c2c2e; color:#f5f5f7; border:1px solid #3a3a3c;"
        "  border-radius:10px; padding:6px 8px; min-height:24px; }"
        "#RepositionPanel QPushButton:hover { background:#3a3a3c; }"
        "#RepositionPanel QPushButton:pressed { background:#0a84ff; color:#ffffff; }"
        "#RepositionPanel QPushButton:checked { background:#0a84ff; border-color:#0a84ff; color:#ffffff; }"
        "#RepositionPanel QPushButton:disabled { background:#242426; color:#636366; }"
        // Leave room for both horizontal step buttons in the Windows style.
        "#RepositionPanel QDoubleSpinBox { padding-right:60px; }");

    auto* root = new QVBoxLayout(panel);
    root->setSizeConstraint(QLayout::SetMinimumSize);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    auto* title = new QLabel(tr("Reposición"), panel);
    title->setObjectName("PanelTitle");
    root->addWidget(title);

    auto addSection = [&](const QString& text) {
        auto* label = new QLabel(text, panel);
        label->setObjectName("SectionTitle");
        root->addWidget(label);
        return label;
    };
    auto makeButton = [&](const QString& text, auto slot) {
        auto* button = new QPushButton(text, panel);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        connect(button, &QPushButton::clicked, this, slot);
        return button;
    };

    addSection(tr("Estructuras a mover"));
    m_repositionObjectList = new QListWidget(panel);
    m_repositionObjectList->setObjectName("RepositionStructureList");
    m_repositionObjectList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_repositionObjectList->setFixedHeight(180);
    m_repositionObjectList->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_repositionObjectList->setUniformItemSizes(true);
    m_repositionObjectList->setStyleSheet(
        "QListWidget { background:#242426; color:#f5f5f7; border:1px solid #3a3a3c; border-radius:6px; }"
        "QListWidget::item { min-height:28px; padding:2px 4px; }"
        "QListWidget::item:selected { background:#1f3b57; }");
    m_repositionObjectList->setToolTip(
        tr("Marque las estructuras que se moveran en bloque. Seleccione una fila para ver sus valores."));
    connect(m_repositionObjectList, &QListWidget::itemChanged,
            this, [this] {
                const bool restartGizmo = m_repositionView && m_repositionView->hasGizmo();
                if (restartGizmo)
                    acceptRepositionGizmo();
                m_repositionTargetLabel = currentRepositionTargetLabel();
                m_repositionPickingPivot = false;
                if (m_repositionView)
                    m_repositionView->setPointPickMode(false);
                refreshRepositionPivotMarker();
                syncRepositionSelectionVisibility();
                updateRepositionControls();
                if (restartGizmo && !selectedRepositionTargetLabels().isEmpty())
                    startRepositionGizmo();
            });
    connect(m_repositionObjectList, &QListWidget::currentItemChanged,
            this, [this] {
                m_repositionTargetLabel = currentRepositionTargetLabel();
                m_repositionPickingPivot = false;
                if (m_repositionView)
                    m_repositionView->setPointPickMode(false);
                refreshRepositionPivotMarker();
                updateRepositionControls();
            });
    root->addWidget(m_repositionObjectList);

    auto* topButtons = new QHBoxLayout;
    topButtons->setSpacing(6);
    topButtons->addWidget(makeButton(tr("Control interactivo"), [this] { startRepositionGizmo(); }));
    topButtons->addWidget(makeButton(tr("Aceptar"), [this] { acceptRepositionGizmo(); }));
    root->addLayout(topButtons);

    addSection(tr("Traslación"));
    m_repositionStepSpin = new QDoubleSpinBox(panel);
    m_repositionStepSpin->setRange(0.1, 20.0);
    m_repositionStepSpin->setDecimals(2);
    m_repositionStepSpin->setSingleStep(0.5);
    m_repositionStepSpin->setValue(1.0);
    m_repositionStepSpin->setSuffix(tr(" mm"));
    root->addWidget(m_repositionStepSpin);

    auto addMoveRow = [&](const QString& label, const QString& left, const QString& right,
                          std::function<void(double)> leftFn,
                          std::function<void(double)> rightFn,
                          QLabel** valueLabel) {
        auto* row = new QHBoxLayout;
        row->setSpacing(6);
        auto* caption = new QLabel(label, panel);
        caption->setMinimumWidth(58);
        row->addWidget(caption);
        auto* leftBtn = new QPushButton(left, panel);
        auto* rightBtn = new QPushButton(right, panel);
        connect(leftBtn, &QPushButton::clicked, this, [this, leftFn] {
            leftFn(m_repositionStepSpin ? m_repositionStepSpin->value() : 1.0);
        });
        connect(rightBtn, &QPushButton::clicked, this, [this, rightFn] {
            rightFn(m_repositionStepSpin ? m_repositionStepSpin->value() : 1.0);
        });
        row->addWidget(leftBtn);
        row->addWidget(rightBtn);
        auto* value = new QLabel(tr("0.0 mm"), panel);
        value->setMinimumWidth(58);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(value);
        if (valueLabel) *valueLabel = value;
        root->addLayout(row);
    };

    addMoveRow(tr("Lateral"), tr("Left"), tr("Right"),
               [this](double s) { translateRepositionTarget(-s, 0.0, 0.0); },
               [this](double s) { translateRepositionTarget( s, 0.0, 0.0); },
               &m_repositionLateralValue);
    addMoveRow(tr("Ant/Post"), tr("Post"), tr("Ant"),
               [this](double s) { translateRepositionTarget(0.0,  s, 0.0); },
               [this](double s) { translateRepositionTarget(0.0, -s, 0.0); },
               &m_repositionAntPostValue);
    addMoveRow(tr("Vertical"), tr("Down"), tr("Up"),
               [this](double s) { translateRepositionTarget(0.0, 0.0, -s); },
               [this](double s) { translateRepositionTarget(0.0, 0.0,  s); },
               &m_repositionVerticalValue);

    addSection(tr("Rotación"));
    m_repositionPivotButton = makeButton(tr("Cambiar centro de rotación"), [this] {
        beginDefineRepositionPivot();
    });
    m_repositionPivotButton->setObjectName("RepositionChangeRotationCenter");
    m_repositionPivotButton->setCheckable(true);
    m_repositionPivotButton->setToolTip(
        tr("Elija un nuevo centro sobre la estructura activa. Pulse de nuevo para cancelar."));
    root->addWidget(m_repositionPivotButton);
    m_repositionPivotValue = new QLabel(tr("Centro: centro de malla"), panel);
    m_repositionPivotValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_repositionPivotValue->setStyleSheet("color:#d1d1d6; font-size:10px;");
    root->addWidget(m_repositionPivotValue);

    m_repositionRotStepSpin = new QDoubleSpinBox(panel);
    m_repositionRotStepSpin->setRange(0.1, 30.0);
    m_repositionRotStepSpin->setDecimals(2);
    m_repositionRotStepSpin->setSingleStep(0.5);
    m_repositionRotStepSpin->setValue(1.0);
    m_repositionRotStepSpin->setSuffix(tr(" deg"));
    root->addWidget(m_repositionRotStepSpin);

    auto addRotRow = [&](const QString& label, const QString& left, const QString& right,
                         std::function<void(double)> leftFn,
                         std::function<void(double)> rightFn,
                         QLabel** valueLabel) {
        auto* row = new QHBoxLayout;
        row->setSpacing(6);
        auto* caption = new QLabel(label, panel);
        caption->setMinimumWidth(58);
        row->addWidget(caption);
        auto* leftBtn = new QPushButton(left, panel);
        auto* rightBtn = new QPushButton(right, panel);
        connect(leftBtn, &QPushButton::clicked, this, [this, leftFn] {
            leftFn(m_repositionRotStepSpin ? m_repositionRotStepSpin->value() : 1.0);
        });
        connect(rightBtn, &QPushButton::clicked, this, [this, rightFn] {
            rightFn(m_repositionRotStepSpin ? m_repositionRotStepSpin->value() : 1.0);
        });
        row->addWidget(leftBtn);
        row->addWidget(rightBtn);
        auto* value = new QLabel(tr("0.0 deg"), panel);
        value->setMinimumWidth(58);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(value);
        if (valueLabel) *valueLabel = value;
        root->addLayout(row);
    };

    addRotRow(tr("Sagital X"), tr("Down"), tr("Up"),
              [this](double d) { rotateRepositionTarget(1.0, 0.0, 0.0, -d); },
              [this](double d) { rotateRepositionTarget(1.0, 0.0, 0.0,  d); },
              &m_repositionRotXValue);
    addRotRow(tr("Coronal Y"), tr("Tilt L"), tr("Tilt R"),
              [this](double d) { rotateRepositionTarget(0.0, 1.0, 0.0, -d); },
              [this](double d) { rotateRepositionTarget(0.0, 1.0, 0.0,  d); },
              &m_repositionRotYValue);
    addRotRow(tr("Axial Z"), tr("Left"), tr("Right"),
              [this](double d) { rotateRepositionTarget(0.0, 0.0, 1.0, -d); },
              [this](double d) { rotateRepositionTarget(0.0, 0.0, 1.0,  d); },
              &m_repositionRotZValue);

    addSection(tr("Restricciones"));
    auto* restrictCombo = new QComboBox(panel);
    restrictCombo->setObjectName("RepositionRestriction");
    restrictCombo->addItems({tr("Sin restricción"), tr("Solo traslación"), tr("Solo rotación")});
    connect(restrictCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) { setRepositionRestriction(index); });
    root->addWidget(restrictCombo);

    addSection(tr("Análisis"));
    auto* analysisRow = new QHBoxLayout;
    analysisRow->setSpacing(6);
    analysisRow->addWidget(makeButton(tr("Intersección"), [this] { analyzeRepositionIntersection(false); }));
    m_repositionHighlightButton = makeButton(tr("Resaltar"), [this] { toggleRepositionHighlight(); });
    analysisRow->addWidget(m_repositionHighlightButton);
    root->addLayout(analysisRow);
    m_repositionIntersectionLabel = new QLabel(panel);
    m_repositionIntersectionLabel->setWordWrap(true);
    m_repositionIntersectionLabel->setStyleSheet("color:#ff9f0a; font-size:10px;");
    root->addWidget(m_repositionIntersectionLabel);
    m_repositionPreOpButton = makeButton(tr("Ver pre-op"), [this] { toggleRepositionPreOp(); });
    m_repositionPreOpButton->setToolTip(tr("Muestra la posición original de las estructuras movidas en gris translúcido."));
    root->addWidget(m_repositionPreOpButton);
    addSection(tr("Mediciones"));
    m_repositionMeasureLabel = new QLabel(panel);
    m_repositionMeasureLabel->setWordWrap(true);
    m_repositionMeasureLabel->setStyleSheet("color:#f5f5f7; font-size:10px;");
    m_repositionMeasureLabel->setToolTip(tr("Desplazamiento de los puntos de la trayectoria de corte (P1 derecha → último izquierda).\n"
                                            "Ejes orientados: X lateral, Y antero-posterior, Z vertical (Z+ superior)."));
    m_repositionMeasureLabel->setVisible(false);
    root->addWidget(m_repositionMeasureLabel);

    root->addWidget(makeButton(tr("Toggle fixed view"), [this] {
        if (!m_repositionView) return;
        if (m_repositionFixedViewAct) {
            m_repositionFixedViewAct->setChecked(!m_repositionFixedViewAct->isChecked());
        } else {
            m_repositionView->setStandardView(0);
            m_repositionView->render();
        }
    }));

    root->addStretch(1);
    scroll->setWidget(panel);
    return scroll;
}

void MainWindow::setRepositionWorkspace(bool enabled)
{
    if (m_viewModeStack && enabled) {
        m_viewModeStack->setCurrentIndex(5);
        QApplication::processEvents();
    }
    if (!enabled) {
        m_repositionPickingPivot = false;
        if (m_repositionView)
            m_repositionView->setPointPickMode(false);
        if (m_repositionView && m_repositionView->hasGizmo())
            m_repositionView->stopGizmo();
        if (m_repositionAcceptGizmoAct) m_repositionAcceptGizmoAct->setEnabled(false);
        updateRepositionControls();
        return;
    }
    if (!m_repositionView) return;

    syncRepositionView();
    m_repositionView->render();
    statusBar()->showMessage(tr("Reposicion: seleccione un segmento y use los controles de traslacion/rotacion."));
}

void MainWindow::setSplintWorkspace(bool enabled)
{
    if (m_viewModeStack && enabled) {
        m_viewModeStack->setCurrentIndex(6);
        QApplication::processEvents();
    }
    if (!enabled) {
        if (m_splintDesignPanel) {
            setSplintPointGroup(-1);
            setSplintContourEditing(false);
        }
        return;
    }
    if (!m_splintView)
        return;

    if (splintHeightmapMethodActive()) {
        refreshSplintDesignPanel();
        syncSplintView();
        updateButtonStates();
        requestSplintPreview();
        statusBar()->showMessage(tr("Férula: elija el diseño y las fuentes, marque 3 puntos por arcada y revise la vista previa."));
        return;
    }

    updateSplintControls();
    syncSplintView();
    rebuildSplintPointMarkers();
    updateButtonStates();
    for (Mesh3DView* view : {m_splintUpperView, m_splintLowerView, m_splintView}) {
        if (!view) continue;
        view->setStandardView(0);
        view->resetCamera();
        view->render();
    }
    statusBar()->showMessage(tr("Ferula: seleccione arcadas, marque puntos guia y cree la ferula."));
}

int MainWindow::selectedSplintUpperLabel() const
{
    if (m_splintUpperPartCombo)
        return m_splintUpperPartCombo->currentData().toInt();
    return kLeFortSegLabel;
}

int MainWindow::selectedSplintLowerLabel(bool finalSplint) const
{
    if (m_splintLowerPartCombo) {
        const int choice = m_splintLowerPartCombo->currentData().toInt();
        if (choice != kSplintInitialMandibleChoice && choice != kSplintFinalMandibleChoice)
            return choice;
        if (choice == kSplintFinalMandibleChoice)
            finalSplint = true;
    }

    if (finalSplint) {
        const int current = currentBiteMandibleTargetLabel();
        if (current > 0)
            return current;
    }

    if (m_repositionOriginalMeshes.find(kGenioBodyLabel) != m_repositionOriginalMeshes.end())
        return kGenioBodyLabel;
    if (m_repositionOriginalMeshes.find(kBssoDistalLabel) != m_repositionOriginalMeshes.end())
        return kBssoDistalLabel;
    if (repositionMeshForLabel(kLowerCompositeLabel))
        return kLowerCompositeLabel;
    return 6;
}

vtkSmartPointer<vtkPolyData> MainWindow::meshForSplintSourceLabel(int label) const
{
    if (label == kSplintInitialMandibleChoice)
        return initialMandibleMeshForSplint();
    if (label == kSplintFinalMandibleChoice)
        return finalMandibleMeshForSplint();

    if (auto mesh = repositionMeshForLabel(label))
        return mesh;
    if (auto mesh = meshForAnatomicLabel(label))
        return mesh;
    return nullptr;
}

void MainWindow::updateSplintControls()
{
    const bool finalSplint =
        m_splintDesignCombo && m_splintDesignCombo->currentData().toInt() == 1;

    if (m_splintLowerPartCombo) {
        const int current = m_splintLowerPartCombo->currentData().toInt();
        if (current == kSplintInitialMandibleChoice || current == kSplintFinalMandibleChoice) {
            QSignalBlocker blocker(m_splintLowerPartCombo);
            const int wanted = finalSplint ? kSplintFinalMandibleChoice : kSplintInitialMandibleChoice;
            for (int i = 0; i < m_splintLowerPartCombo->count(); ++i) {
                if (m_splintLowerPartCombo->itemData(i).toInt() == wanted) {
                    m_splintLowerPartCombo->setCurrentIndex(i);
                    break;
                }
            }
        }
    }

    auto upperMesh = meshForSplintSourceLabel(selectedSplintUpperLabel());
    const bool hasUpper = upperMesh && upperMesh->GetNumberOfPoints() > 0;
    const int lowerChoice = m_splintLowerPartCombo
        ? m_splintLowerPartCombo->currentData().toInt()
        : selectedSplintLowerLabel(finalSplint);
    auto lowerMesh = meshForSplintSourceLabel(lowerChoice);
    const bool hasLower = lowerMesh && lowerMesh->GetNumberOfPoints() > 0;
    const bool hasGuidePoints = hasEnoughSplintGuidePoints();
    vtkPolyData* activeSplint = finalSplint ? m_finalSplintMesh.Get() : m_intermediateSplintMesh.Get();
    const bool hasActiveSplint = activeSplint && activeSplint->GetNumberOfPoints() > 0;

    if (m_splintCreateButton)
        m_splintCreateButton->setEnabled(!m_splintInProgress && hasUpper && hasLower);

    if (m_splintClearPointsButton) {
        const bool hasAnyPoints =
            !m_splintUpperVestibularPoints.isEmpty() ||
            !m_splintUpperPalatalPoints.isEmpty() ||
            !m_splintLowerVestibularPoints.isEmpty() ||
            !m_splintLowerLingualPoints.isEmpty();
        m_splintClearPointsButton->setEnabled(hasAnyPoints);
    }

    if (m_splintAdjustButton)
        m_splintAdjustButton->setEnabled(!m_splintInProgress && hasActiveSplint && !m_splintGizmoActive);
    if (m_splintAcceptAdjustButton)
        m_splintAcceptAdjustButton->setEnabled(!m_splintInProgress && hasActiveSplint && m_splintGizmoActive);

    if (m_splintPointStatusLabel) {
        const QString status = tr("Puntos: Max V %1 | Max P %2 | Mand V %3 | Mand L %4\n"
                                  "%5")
            .arg(m_splintUpperVestibularPoints.size())
            .arg(m_splintUpperPalatalPoints.size())
            .arg(m_splintLowerVestibularPoints.size())
            .arg(m_splintLowerLingualPoints.size())
            .arg(m_splintGizmoActive
                 ? tr("Gizmo activo. Ajuste la ferula y pulse Aceptar ajuste.")
                 : (hasGuidePoints
                    ? tr("Hay suficientes puntos para refinar el contorno.")
                    : tr("Sin puntos: se creara una herradura automatica.")));
        m_splintPointStatusLabel->setText(status);
    }
}

void MainWindow::syncSplintView()
{
    if (!m_splintView)
        return;
    if (splintHeightmapMethodActive()) {
        syncSplintHeightmapView();
        return;
    }

    for (Mesh3DView* view : {m_splintUpperView, m_splintLowerView, m_splintView}) {
        if (view) view->clearMeshes();
    }

    auto addTo = [this](Mesh3DView* view, int label, vtkPolyData* mesh, const QColor& overrideColor = QColor()) {
        if (!view || !mesh || mesh->GetNumberOfPoints() <= 0)
            return;
        const int actorKey = objectActorKey(label);
        view->addMesh(actorKey, mesh, meshLabelName(label));
        view->setMeshColor(actorKey, overrideColor.isValid() ? overrideColor : objectColorForLabel(label));
        view->setMeshOpacity(actorKey, 1.0);
        view->setMeshVisible(actorKey, objectEntryVisible(label));
    };

    const bool finalSplint =
        m_splintDesignCombo && m_splintDesignCombo->currentData().toInt() == 1;
    const int upperLabel = selectedSplintUpperLabel();
    const int lowerChoice = m_splintLowerPartCombo
        ? m_splintLowerPartCombo->currentData().toInt()
        : selectedSplintLowerLabel(finalSplint);
    const int lowerLabel = selectedSplintLowerLabel(finalSplint);
    auto upperMesh = meshForSplintSourceLabel(upperLabel);
    auto lowerMesh = meshForSplintSourceLabel(lowerChoice);

    addTo(m_splintUpperView, upperLabel, upperMesh, QColor(128, 112, 210));
    addTo(m_splintView, upperLabel, upperMesh, QColor(128, 112, 210));
    addTo(m_splintLowerView, lowerLabel, lowerMesh, QColor(220, 82, 78));
    addTo(m_splintView, lowerLabel, lowerMesh, QColor(220, 82, 78));

    const int activeSplintLabel = finalSplint ? kFinalSplintLabel : kIntermediateSplintLabel;
    vtkPolyData* activeSplint = finalSplint ? m_finalSplintMesh.Get() : m_intermediateSplintMesh.Get();
    if (activeSplint) {
        addTo(m_splintUpperView, activeSplintLabel, activeSplint, QColor(235, 243, 248));
        addTo(m_splintLowerView, activeSplintLabel, activeSplint, QColor(235, 243, 248));
        addTo(m_splintView, activeSplintLabel, activeSplint, QColor(235, 243, 248));
    }

    syncVisibilityPanelToAllViews();
    rebuildSplintPointMarkers();
    for (Mesh3DView* view : {m_splintUpperView, m_splintLowerView, m_splintView}) {
        if (view) view->render();
    }
}

vtkSmartPointer<vtkPolyData> MainWindow::initialMandibleMeshForSplint() const
{
    auto copy = [](vtkPolyData* mesh) -> vtkSmartPointer<vtkPolyData> {
        if (!mesh || mesh->GetNumberOfPoints() <= 0)
            return nullptr;
        auto out = vtkSmartPointer<vtkPolyData>::New();
        out->DeepCopy(mesh);
        return out;
    };

    auto original = [this, &copy](int label) -> vtkSmartPointer<vtkPolyData> {
        const auto it = m_repositionOriginalMeshes.find(label);
        if (it == m_repositionOriginalMeshes.end())
            return nullptr;
        return copy(it->second);
    };

    if (auto mesh = copy(m_orientedInitialMandibleMeshForSplint)) return mesh;
    if (auto mesh = copy(repositionMeshForLabel(kLowerCompositeLabel))) return mesh;
    if (auto mesh = copy(m_lowerCompositeMesh)) return mesh;
    if (auto mesh = copy(m_preBiteMandibleMeshForSplint)) return mesh;
    if (auto mesh = original(kGenioBodyLabel)) return mesh;
    if (auto mesh = original(kBssoDistalLabel)) return mesh;
    if (auto mesh = copy(repositionMeshForLabel(kGenioBodyLabel))) return mesh;
    if (auto mesh = copy(repositionMeshForLabel(kBssoDistalLabel))) return mesh;
    if (auto mesh = copy(repositionMeshForLabel(6))) return mesh;
    return copy(meshForAnatomicLabel(6));
}

vtkSmartPointer<vtkPolyData> MainWindow::finalMandibleMeshForSplint() const
{
    auto copy = [](vtkPolyData* mesh) -> vtkSmartPointer<vtkPolyData> {
        if (!mesh || mesh->GetNumberOfPoints() <= 0)
            return nullptr;
        auto out = vtkSmartPointer<vtkPolyData>::New();
        out->DeepCopy(mesh);
        return out;
    };

    if (auto mesh = copy(repositionMeshForLabel(currentBiteMandibleTargetLabel()))) return mesh;
    if (auto mesh = copy(m_genioBodyMesh)) return mesh;
    if (auto mesh = copy(m_bssoDistalMesh)) return mesh;
    if (auto mesh = copy(m_lowerCompositeMesh)) return mesh;
    return copy(meshForAnatomicLabel(6));
}

void MainWindow::setSplintPointCapture(SplintPointSet set)
{
    m_splintPointSet = set;

    auto syncButton = [](QPushButton* button, bool checked) {
        if (!button) return;
        QSignalBlocker blocker(button);
        button->setChecked(checked);
    };
    syncButton(m_splintUpperVestibularButton, set == SplintPointSet::UpperVestibular);
    syncButton(m_splintUpperPalatalButton, set == SplintPointSet::UpperPalatal);
    syncButton(m_splintLowerVestibularButton, set == SplintPointSet::LowerVestibular);
    syncButton(m_splintLowerLingualButton, set == SplintPointSet::LowerLingual);

    const bool upperMode = set == SplintPointSet::UpperVestibular || set == SplintPointSet::UpperPalatal;
    const bool lowerMode = set == SplintPointSet::LowerVestibular || set == SplintPointSet::LowerLingual;
    if (m_splintUpperView) m_splintUpperView->setPointPickMode(upperMode);
    if (m_splintLowerView) m_splintLowerView->setPointPickMode(lowerMode);
    if (m_splintView) m_splintView->setPointPickMode(set != SplintPointSet::None);

    QString message;
    switch (set) {
    case SplintPointSet::UpperVestibular:
        message = tr("Ferula: marque puntos en la cara vestibular del maxilar.");
        break;
    case SplintPointSet::UpperPalatal:
        message = tr("Ferula: marque puntos en la cara palatina del maxilar.");
        break;
    case SplintPointSet::LowerVestibular:
        message = tr("Ferula: marque puntos en la cara vestibular mandibular.");
        break;
    case SplintPointSet::LowerLingual:
        message = tr("Ferula: marque puntos en la cara lingual mandibular.");
        break;
    case SplintPointSet::None:
        message = tr("Ferula: seleccione una superficie para marcar puntos guia.");
        break;
    }
    statusBar()->showMessage(message);
    updateSplintControls();
}

void MainWindow::onSplintPointPicked(int actorLabel, double x, double y, double z)
{
    Q_UNUSED(actorLabel);
    QVector<QVector3D>* target = nullptr;
    switch (m_splintPointSet) {
    case SplintPointSet::UpperVestibular:
        target = &m_splintUpperVestibularPoints;
        break;
    case SplintPointSet::UpperPalatal:
        target = &m_splintUpperPalatalPoints;
        break;
    case SplintPointSet::LowerVestibular:
        target = &m_splintLowerVestibularPoints;
        break;
    case SplintPointSet::LowerLingual:
        target = &m_splintLowerLingualPoints;
        break;
    case SplintPointSet::None:
        return;
    }

    target->append(QVector3D(static_cast<float>(x),
                             static_cast<float>(y),
                             static_cast<float>(z)));
    rebuildSplintPointMarkers();
    updateSplintControls();
}

int MainWindow::splintPointCount(SplintPointSet set) const
{
    switch (set) {
    case SplintPointSet::UpperVestibular:
        return m_splintUpperVestibularPoints.size();
    case SplintPointSet::UpperPalatal:
        return m_splintUpperPalatalPoints.size();
    case SplintPointSet::LowerVestibular:
        return m_splintLowerVestibularPoints.size();
    case SplintPointSet::LowerLingual:
        return m_splintLowerLingualPoints.size();
    case SplintPointSet::None:
        return 0;
    }
    return 0;
}

bool MainWindow::hasEnoughSplintGuidePoints() const
{
    return splintPointCount(SplintPointSet::UpperVestibular) >= 3 &&
           splintPointCount(SplintPointSet::UpperPalatal) >= 3 &&
           splintPointCount(SplintPointSet::LowerVestibular) >= 3 &&
           splintPointCount(SplintPointSet::LowerLingual) >= 3;
}

void MainWindow::rebuildSplintPointMarkers()
{
    for (Mesh3DView* view : {m_splintUpperView, m_splintLowerView, m_splintView}) {
        if (view) view->clearPointMarkers();
    }

    auto addPoints = [](Mesh3DView* view, const QVector<QVector3D>& points, const QColor& color) {
        if (!view) return;
        for (const QVector3D& p : points)
            view->addPointMarker(p.x(), p.y(), p.z(), color);
    };

    const QColor upperVestibular(255, 149, 0);
    const QColor upperPalatal(128, 112, 210);
    const QColor lowerVestibular(10, 132, 255);
    const QColor lowerLingual(52, 199, 89);

    addPoints(m_splintUpperView, m_splintUpperVestibularPoints, upperVestibular);
    addPoints(m_splintUpperView, m_splintUpperPalatalPoints, upperPalatal);
    addPoints(m_splintLowerView, m_splintLowerVestibularPoints, lowerVestibular);
    addPoints(m_splintLowerView, m_splintLowerLingualPoints, lowerLingual);

    addPoints(m_splintView, m_splintUpperVestibularPoints, upperVestibular);
    addPoints(m_splintView, m_splintUpperPalatalPoints, upperPalatal);
    addPoints(m_splintView, m_splintLowerVestibularPoints, lowerVestibular);
    addPoints(m_splintView, m_splintLowerLingualPoints, lowerLingual);

    updateSplintControls();

    for (Mesh3DView* view : {m_splintUpperView, m_splintLowerView, m_splintView}) {
        if (view) view->render();
    }
}

void MainWindow::clearSplintPoints()
{
    m_splintUpperVestibularPoints.clear();
    m_splintUpperPalatalPoints.clear();
    m_splintLowerVestibularPoints.clear();
    m_splintLowerLingualPoints.clear();
    setSplintPointCapture(SplintPointSet::None);
    rebuildSplintPointMarkers();
    updateSplintControls();
}

void MainWindow::createIntermediateSplint()
{
    if (splintHeightmapMethodActive()) {
        selectSplintDesignByLabel(kIntermediateSplintLabel);
        createHeightmapSplint();
        return;
    }
    if (m_splintDesignCombo)
        m_splintDesignCombo->setCurrentIndex(0);
    createSplint(false);
}

void MainWindow::createFinalSplint()
{
    if (splintHeightmapMethodActive()) {
        selectSplintDesignByLabel(kFinalSplintLabel);
        createHeightmapSplint();
        return;
    }
    if (m_splintDesignCombo)
        m_splintDesignCombo->setCurrentIndex(1);
    createSplint(true);
}

void MainWindow::createSelectedSplint()
{
    const bool finalSplint =
        m_splintDesignCombo && m_splintDesignCombo->currentData().toInt() == 1;
    createSplint(finalSplint);
}

void MainWindow::startSplintGizmo()
{
    if (!m_splintView)
        return;

    const bool finalSplint =
        m_splintDesignCombo && m_splintDesignCombo->currentData().toInt() == 1;
    const int label = finalSplint ? kFinalSplintLabel : kIntermediateSplintLabel;
    vtkPolyData* mesh = finalSplint ? m_finalSplintMesh.Get() : m_intermediateSplintMesh.Get();
    if (!mesh || mesh->GetNumberOfPoints() <= 0) {
        statusBar()->showMessage(tr("Ferula: primero cree la ferula para poder ajustarla."));
        return;
    }

    syncSplintView();
    const int actorKey = objectActorKey(label);
    if (!m_splintView->meshData(actorKey)) {
        statusBar()->showMessage(tr("Ferula: no encuentro la ferula en la vista de preview."));
        return;
    }

    setSplintPointCapture(SplintPointSet::None);
    deactivateLassoTools();

    if (m_splintView->hasGizmo())
        m_splintView->stopGizmo();
    m_splintView->startGizmo(actorKey);
    if (!m_splintView->hasGizmo()) {
        statusBar()->showMessage(tr("Ferula: no se pudo activar el gizmo."));
        return;
    }

    m_splintGizmoTargetLabel = label;
    m_splintGizmoActive = true;
    updateSplintControls();
    statusBar()->showMessage(tr("Gizmo activo para %1. Ajuste posicion y tamano, luego pulse Aceptar ajuste.")
                                 .arg(meshLabelName(label)));
}

void MainWindow::acceptSplintGizmo()
{
    if (m_splintView && m_splintView->hasGizmo())
        m_splintView->stopGizmo();
    m_splintGizmoActive = false;
    m_splintGizmoTargetLabel = -1;
    updateSplintControls();
    statusBar()->showMessage(tr("Ferula: ajuste manual aceptado."));
}

void MainWindow::onSplintGizmoUpdated(int meshLabel, vtkSmartPointer<vtkPolyData> newMesh)
{
    if (!newMesh || newMesh->GetNumberOfPoints() <= 0)
        return;

    int label = meshLabel >= 1000 ? meshLabel - 1000 : meshLabel;
    if (m_splintGizmoTargetLabel > 0)
        label = m_splintGizmoTargetLabel;
    if (label <= 0)
        return;

    auto baked = vtkSmartPointer<vtkPolyData>::New();
    baked->DeepCopy(newMesh);

    if (label == kIntermediateSplintLabel)
        m_intermediateSplintMesh = baked;
    else if (label == kFinalSplintLabel)
        m_finalSplintMesh = baked;

    setRepositionMeshForLabel(label, baked);
    m_splintGizmoActive = false;
    m_splintGizmoTargetLabel = -1;
    syncSplintView();
    updateSplintControls();
    updateButtonStates();
    statusBar()->showMessage(tr("Ferula: ajuste interactivo aplicado a %1.").arg(meshLabelName(label)));
}

void MainWindow::createSplint(bool finalSplint)
{
    if (m_splintInProgress)
        return;

    auto copy = [](vtkPolyData* mesh) -> vtkSmartPointer<vtkPolyData> {
        if (!mesh || mesh->GetNumberOfPoints() <= 0)
            return nullptr;
        auto out = vtkSmartPointer<vtkPolyData>::New();
        out->DeepCopy(mesh);
        return out;
    };

    const int upperLabel = selectedSplintUpperLabel();
    const int lowerChoice = m_splintLowerPartCombo
        ? m_splintLowerPartCombo->currentData().toInt()
        : (finalSplint ? kSplintFinalMandibleChoice : kSplintInitialMandibleChoice);
    auto upper = copy(meshForSplintSourceLabel(upperLabel));
    auto lower = copy(meshForSplintSourceLabel(lowerChoice));
    if (!upper || !lower) {
        QMessageBox::warning(this, tr("Ferula"),
                             tr("No encuentro una fuente superior e inferior valida para generar la ferula.\n\n"
                                "Superior: Segmento Le Fort I, Compuesto maxilar o Maxilar.\n"
                                "Inferior: Mandibula post-menton, Segmento distal BSSO, Compuesto mandibular o Mandibula."));
        return;
    }

    auto toGuidePoints = [](const QVector<QVector3D>& points) {
        std::vector<std::array<double, 3>> out;
        out.reserve(static_cast<size_t>(points.size()));
        for (const QVector3D& p : points)
            out.push_back({static_cast<double>(p.x()),
                           static_cast<double>(p.y()),
                           static_cast<double>(p.z())});
        return out;
    };

    const int label = finalSplint ? kFinalSplintLabel : kIntermediateSplintLabel;
    const QString name = meshLabelName(label);
    const auto upperVestibular = toGuidePoints(m_splintUpperVestibularPoints);
    const auto upperPalatal = toGuidePoints(m_splintUpperPalatalPoints);
    const auto lowerVestibular = toGuidePoints(m_splintLowerVestibularPoints);
    const auto lowerLingual = toGuidePoints(m_splintLowerLingualPoints);
    const double thicknessMm = m_splintThicknessSpin ? m_splintThicknessSpin->value() : 3.0;

    m_splintInProgress = true;
    m_splintGizmoActive = false;
    m_splintGizmoTargetLabel = -1;
    if (m_splintIntermediateAct) m_splintIntermediateAct->setEnabled(false);
    if (m_splintFinalAct) m_splintFinalAct->setEnabled(false);
    if (m_splintCreateButton) m_splintCreateButton->setEnabled(false);
    if (m_progressBar) {
        m_progressBar->setRange(0, 0);
        m_progressBar->setVisible(true);
    }
    statusBar()->showMessage(tr("Calculando %1...").arg(name));

    QThread* worker = QThread::create([this, upper, lower, label, name, thicknessMm,
                                        upperVestibular, upperPalatal,
                                        lowerVestibular, lowerLingual]() {
        SplintGenerationInputs inputs;
        inputs.upperOcclusion = upper;
        inputs.lowerOcclusion = lower;
        inputs.name = name;
        inputs.thicknessMm = thicknessMm;
        inputs.archWidthMm = 18.0;
        inputs.borderPaddingMm = 4.0;
        inputs.indentationDepthMm = 0.0;
        inputs.filletMm = 1.4;
        inputs.useUpperImpression = false;
        inputs.useLowerImpression = false;
        inputs.tryBooleanIndentation = false;
        inputs.upperVestibularPoints = upperVestibular;
        inputs.upperPalatalPoints = upperPalatal;
        inputs.lowerVestibularPoints = lowerVestibular;
        inputs.lowerLingualPoints = lowerLingual;
        auto result = SplintGenerator::Generate(inputs);
        QMetaObject::invokeMethod(this, [this, label, result]() mutable {
            onSplintGenerated(label, result.mesh, result.report, result.booleanApplied);
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void MainWindow::onSplintGenerated(int label, vtkSmartPointer<vtkPolyData> mesh,
                                   const QString& report, bool booleanApplied)
{
    m_splintInProgress = false;
    if (m_progressBar) {
        m_progressBar->setRange(0, 100);
        m_progressBar->setVisible(false);
    }

    if (!mesh || mesh->GetNumberOfPoints() <= 0) {
        QMessageBox::warning(this, tr("Ferula"), tr("No se pudo generar la ferula."));
        updateSplintControls();
        updateButtonStates();
        return;
    }

    if (label == kIntermediateSplintLabel)
        m_intermediateSplintMesh = mesh;
    else if (label == kFinalSplintLabel)
        m_finalSplintMesh = mesh;

    const QColor color = meshLabelColor(label);
    addObjectEntry(meshLabelName(label), color, label);
    setRepositionMeshForLabel(label, mesh);
    syncSplintView();
    startSplintGizmo();
    updateSplintControls();
    updateButtonStates();
    statusBar()->showMessage(report + (booleanApplied ? tr(" Boolean aplicado.") : tr(" Indentacion por distancia.")));
}

void MainWindow::exportSplintStl()
{
    if (splintHeightmapMethodActive()) {
        exportHeightmapSplintStl();
        return;
    }
    if (!m_intermediateSplintMesh && !m_finalSplintMesh) {
        QMessageBox::warning(this, tr("Ferula"), tr("No hay ferulas para exportar."));
        return;
    }

    const QString dirPath = QFileDialog::getExistingDirectory(this, tr("Exportar ferulas STL"), QString());
    if (dirPath.isEmpty())
        return;

    auto write = [](vtkPolyData* mesh, const QString& path) -> bool {
        if (!mesh || mesh->GetNumberOfPoints() <= 0)
            return false;
        auto writer = vtkSmartPointer<vtkSTLWriter>::New();
        writer->SetFileName(path.toLocal8Bit().constData());
        writer->SetInputData(mesh);
        writer->Write();
        return writer->GetErrorCode() == 0;
    };

    QDir dir(dirPath);
    bool ok = true;
    if (m_intermediateSplintMesh)
        ok = write(m_intermediateSplintMesh, dir.filePath(QStringLiteral("ferula_intermedia.stl"))) && ok;
    if (m_finalSplintMesh)
        ok = write(m_finalSplintMesh, dir.filePath(QStringLiteral("ferula_final.stl"))) && ok;

    statusBar()->showMessage(ok
        ? tr("Ferulas exportadas: %1").arg(dirPath)
        : tr("Una o mas ferulas no pudieron exportarse."));
}

vtkSmartPointer<vtkPolyData> MainWindow::repositionMeshForLabel(int label) const
{
    switch (label) {
        case 5:
            if (m_mesh3DView) {
                if (auto mesh = m_mesh3DView->meshData(objectActorKey(5))) return mesh;
                if (auto mesh = m_mesh3DView->meshData(5)) return mesh;
            }
            break;
        case 6:
            if (m_mesh3DView) {
                if (auto mesh = m_mesh3DView->meshData(objectActorKey(6))) return mesh;
                if (auto mesh = m_mesh3DView->meshData(6)) return mesh;
            }
            break;
        case kUpperCompositeLabel: if (m_upperCompositeMesh) return m_upperCompositeMesh; break;
        case kLowerCompositeLabel: if (m_lowerCompositeMesh) return m_lowerCompositeMesh; break;
        case kLeFortCranialLabel:  if (m_leFortCranialMesh) return m_leFortCranialMesh; break;
        case kLeFortSegLabel:      if (m_leFortSegmentMesh) return m_leFortSegmentMesh; break;
        case kBssoDistalLabel:     if (m_bssoDistalMesh) return m_bssoDistalMesh; break;
        case kBssoProximalLabel:   if (m_bssoProximalMesh) return m_bssoProximalMesh; break;
        case kBssoProximalRightLabel: if (m_bssoRightProximalMesh) return m_bssoRightProximalMesh; break;
        case kBssoProximalLeftLabel:  if (m_bssoLeftProximalMesh) return m_bssoLeftProximalMesh; break;
        case kGenioBodyLabel:      if (m_genioBodyMesh) return m_genioBodyMesh; break;
        case kGenioSegmentLabel:   if (m_genioSegmentMesh) return m_genioSegmentMesh; break;
        case kIntermediateSplintLabel: if (m_intermediateSplintMesh) return m_intermediateSplintMesh; break;
        case kFinalSplintLabel:        if (m_finalSplintMesh) return m_finalSplintMesh; break;
        case kBiteScanLabel:
            if (m_biteScanMesh) return m_biteScanMesh;
            break;
        default: break;
    }
    if (m_mesh3DView) {
        if (auto mesh = m_mesh3DView->meshData(objectActorKey(label))) return mesh;
        if (auto mesh = m_mesh3DView->meshData(label)) return mesh;
    }
    if (m_osteotomyView) {
        if (auto mesh = m_osteotomyView->meshData(objectActorKey(label))) return mesh;
        if (auto mesh = m_osteotomyView->meshData(label)) return mesh;
    }
    if (m_repositionView) {
        if (auto mesh = m_repositionView->meshData(objectActorKey(label))) return mesh;
        if (auto mesh = m_repositionView->meshData(label)) return mesh;
    }
    if (m_splintView) {
        if (auto mesh = m_splintView->meshData(objectActorKey(label))) return mesh;
        if (auto mesh = m_splintView->meshData(label)) return mesh;
    }
    if (m_splintUpperView) {
        if (auto mesh = m_splintUpperView->meshData(objectActorKey(label))) return mesh;
        if (auto mesh = m_splintUpperView->meshData(label)) return mesh;
    }
    if (m_splintLowerView) {
        if (auto mesh = m_splintLowerView->meshData(objectActorKey(label))) return mesh;
        if (auto mesh = m_splintLowerView->meshData(label)) return mesh;
    }
    return nullptr;
}

void MainWindow::setRepositionMeshForLabel(int label, vtkSmartPointer<vtkPolyData> mesh)
{
    if (!mesh || mesh->GetNumberOfPoints() <= 0) return;

    auto copy = vtkSmartPointer<vtkPolyData>::New();
    copy->DeepCopy(mesh);

    switch (label) {
        case kUpperCompositeLabel: m_upperCompositeMesh = copy; break;
        case kLowerCompositeLabel: m_lowerCompositeMesh = copy; break;
        case kLeFortCranialLabel:  m_leFortCranialMesh = copy; break;
        case kLeFortSegLabel:      m_leFortSegmentMesh = copy; break;
        case kBssoDistalLabel:     m_bssoDistalMesh = copy; break;
        case kBssoProximalLabel:   m_bssoProximalMesh = copy; break;
        case kBssoProximalRightLabel: m_bssoRightProximalMesh = copy; break;
        case kBssoProximalLeftLabel:  m_bssoLeftProximalMesh = copy; break;
        case kGenioBodyLabel:      m_genioBodyMesh = copy; break;
        case kGenioSegmentLabel:   m_genioSegmentMesh = copy; break;
        case kIntermediateSplintLabel: m_intermediateSplintMesh = copy; break;
        case kFinalSplintLabel:        m_finalSplintMesh = copy; break;
        case kBiteScanLabel:       m_biteScanMesh = copy; break;
        default: break;
    }

    const int actorKey = objectActorKey(label);
    const QColor color = objectColorForLabel(label);
    const QVector<Mesh3DView*> views = (label == kBiteScanLabel)
        ? QVector<Mesh3DView*>{m_mesh3DView, m_biteScanView, m_biteRegistrationView, m_repositionView}
        : QVector<Mesh3DView*>{m_mesh3DView, m_modelMatchView, m_orientationView,
                               m_osteotomyView, m_biteSegmentView,
                               m_biteRegistrationView, m_repositionView,
                               m_splintUpperView, m_splintLowerView, m_splintView};
    for (Mesh3DView* view : views) {
        if (!view) continue;
        view->addMesh(actorKey, copy, meshLabelName(label));
        view->setMeshColor(actorKey, color);
        view->setMeshOpacity(actorKey, 1.0);
        view->setMeshVisible(actorKey, objectEntryVisible(label));
        view->render();
    }

    if (label == kUpperCompositeLabel && m_modelMaxillaView) {
        m_modelMaxillaView->addMesh(actorKey, copy, meshLabelName(label));
        m_modelMaxillaView->setMeshColor(actorKey, color);
        m_modelMaxillaView->setMeshVisible(actorKey, objectEntryVisible(label));
    } else if (label == kLowerCompositeLabel && m_modelMandibleView) {
        m_modelMandibleView->addMesh(actorKey, copy, meshLabelName(label));
        m_modelMandibleView->setMeshColor(actorKey, color);
        m_modelMandibleView->setMeshVisible(actorKey, objectEntryVisible(label));
    }

    syncVisibilityPanelToAllViews();
}

int MainWindow::currentRepositionTargetLabel() const
{
    if (!m_repositionObjectList || !m_repositionObjectList->currentItem())
        return m_repositionTargetLabel;
    return m_repositionObjectList->currentItem()->data(Qt::UserRole).toInt();
}

QList<int> MainWindow::selectedRepositionTargetLabels() const
{
    QList<int> labels;
    if (!m_repositionObjectList)
        return labels;

    for (int row = 0; row < m_repositionObjectList->count(); ++row) {
        auto* item = m_repositionObjectList->item(row);
        if (item && item->checkState() == Qt::Checked)
            labels.append(item->data(Qt::UserRole).toInt());
    }
    return labels;
}

QVector<int> MainWindow::repositionStructureLabels() const
{
    QVector<int> labels{kLeFortSegLabel, kBssoDistalLabel, kBssoProximalRightLabel, kBssoProximalLeftLabel};
    // The combined proximal mesh has no object entry, so it could never be hidden:
    // once the right/left rami exist it is not a structure of its own.
    const auto has = [](const vtkSmartPointer<vtkPolyData>& mesh) { return mesh && mesh->GetNumberOfPoints() > 0; };
    if (!has(m_bssoRightProximalMesh) && !has(m_bssoLeftProximalMesh))
        labels << kBssoProximalLabel;
    labels << kGenioSegmentLabel << kGenioBodyLabel << kBiteScanLabel << kUpperCompositeLabel << kLowerCompositeLabel
           << kLeFortCranialLabel << kIntermediateSplintLabel << kFinalSplintLabel;
    return labels;
}

void MainWindow::updateRepositionTargetList()
{
    if (!m_repositionObjectList) return;

    const int previous = currentRepositionTargetLabel();
    const bool firstPopulation = m_repositionObjectList->count() == 0;
    QSet<int> previouslySelected;
    for (int label : selectedRepositionTargetLabels())
        previouslySelected.insert(label);
    const auto candidates = repositionStructureLabels();

    QSignalBlocker blocker(m_repositionObjectList);
    m_repositionObjectList->clear();
    QListWidgetItem* currentItem = nullptr;
    for (int label : candidates) {
        auto mesh = repositionMeshForLabel(label);
        if (!mesh || mesh->GetNumberOfPoints() <= 0) continue;
        auto* item = new QListWidgetItem(meshLabelName(label), m_repositionObjectList);
        item->setData(Qt::UserRole, label);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        const bool shouldCheck = previouslySelected.contains(label);
        item->setCheckState(shouldCheck ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(item->text());
        if (label == previous) currentItem = item;
    }

    if (!currentItem && m_repositionObjectList->count() > 0)
        currentItem = m_repositionObjectList->item(0);
    if (currentItem) {
        if (firstPopulation)
            currentItem->setCheckState(Qt::Checked);
        m_repositionObjectList->setCurrentItem(currentItem);
        m_repositionTargetLabel = currentItem->data(Qt::UserRole).toInt();
    } else {
        m_repositionTargetLabel = -1;
    }
}

void MainWindow::syncRepositionSelectionVisibility()
{
    if (!m_repositionView || !m_repositionObjectList) return;
    for (int row = 0; row < m_repositionObjectList->count(); ++row) {
        auto* item = m_repositionObjectList->item(row);
        const int label = item->data(Qt::UserRole).toInt();
        const int actorKey = objectActorKey(label);
        if (!m_repositionView->meshData(actorKey)) {
            auto source = repositionMeshForLabel(label);
            if (!source || source->GetNumberOfPoints() <= 0) continue;
            auto copy = vtkSmartPointer<vtkPolyData>::New();
            copy->DeepCopy(source);
            m_repositionView->addMesh(actorKey, copy, meshLabelName(label));
            m_repositionView->setMeshColor(actorKey, objectColorForLabel(label));
        }
        // Selection guarantees visibility here; global visibility still controls reference objects.
        m_repositionView->setMeshVisible(actorKey,
            item->checkState() == Qt::Checked || objectEntryVisible(label));
    }
}

void MainWindow::updateRepositionControls()
{
    updateMandibleMovementSummary();
    updateRepositionMeasurements();
    const int label = currentRepositionTargetLabel();
    const bool hasTarget = label > 0 && repositionMeshForLabel(label);
    const bool hasSelection = !selectedRepositionTargetLabels().isEmpty();
    if (m_repositionGizmoAct) m_repositionGizmoAct->setEnabled(hasTarget && hasSelection);
    if (m_repositionResetAct) m_repositionResetAct->setEnabled(hasTarget && m_repositionOriginalMeshes.count(label) > 0);
    if (m_repositionHomeAct) m_repositionHomeAct->setEnabled(hasTarget);
    if (m_repositionFixedViewAct) m_repositionFixedViewAct->setEnabled(m_repositionView != nullptr);
    if (m_repositionPivotButton) {
        m_repositionPivotButton->setEnabled(hasTarget);
        m_repositionPivotButton->setChecked(m_repositionPickingPivot);
    }

    const QVector3D t = m_repositionTranslationMm.value(label);
    const QVector3D r = m_repositionRotationDeg.value(label);
    if (m_repositionLateralValue)  m_repositionLateralValue->setText(QString("%1 mm").arg(t.x(), 0, 'f', 1));
    if (m_repositionAntPostValue)  m_repositionAntPostValue->setText(QString("%1 mm").arg(t.y(), 0, 'f', 1));
    if (m_repositionVerticalValue) m_repositionVerticalValue->setText(QString("%1 mm").arg(t.z(), 0, 'f', 1));
    if (m_repositionRotXValue)     m_repositionRotXValue->setText(QString("%1 deg").arg(r.x(), 0, 'f', 1));
    if (m_repositionRotYValue)     m_repositionRotYValue->setText(QString("%1 deg").arg(r.y(), 0, 'f', 1));
    if (m_repositionRotZValue)     m_repositionRotZValue->setText(QString("%1 deg").arg(r.z(), 0, 'f', 1));
    if (m_repositionPivotValue) {
        if (m_repositionPivotWorld.contains(label)) {
            const QVector3D p = m_repositionPivotWorld.value(label);
            m_repositionPivotValue->setText(
                tr("Centro: %1, %2, %3")
                    .arg(p.x(), 0, 'f', 1)
                    .arg(p.y(), 0, 'f', 1)
                    .arg(p.z(), 0, 'f', 1));
        } else {
            m_repositionPivotValue->setText(tr("Centro: centro de malla"));
        }
    }
}

void MainWindow::captureMandibleMovementReference(int label, vtkPolyData* mesh)
{
    if (m_mandibleMovement.targetLabel == label || !mesh || mesh->GetNumberOfPoints() == 0) return;
    m_mandibleMovement = {};
    m_mandibleMovementResetMatrix.clear();
    auto composite = m_orientedInitialMandibleMeshForSplint
        ? m_orientedInitialMandibleMeshForSplint : repositionMeshForLabel(kLowerCompositeLabel);
    if (!composite || composite->GetNumberOfPoints() == 0) return;

    // A cut segment must still coincide with the oriented composite before establishing zero.
    // Never infer a new initial pose from an already moved segment in a legacy project.
    auto locator = vtkSmartPointer<vtkStaticPointLocator>::New();
    locator->SetDataSet(composite);
    locator->BuildLocator();
    const vtkIdType step = std::max<vtkIdType>(1, mesh->GetNumberOfPoints() / 512);
    int coincident = 0, sampled = 0;
    for (vtkIdType i = 0; i < mesh->GetNumberOfPoints(); i += step) {
        double point[3], closest[3];
        mesh->GetPoint(i, point);
        const vtkIdType nearest = locator->FindClosestPoint(point);
        if (nearest < 0) continue;
        composite->GetPoint(nearest, closest);
        double distanceSquared = 0.0;
        for (int axis = 0; axis < 3; ++axis)
            distanceSquared += std::pow(point[axis] - closest[axis], 2.0);
        if (distanceSquared < 1e-6) ++coincident;
        ++sampled;
    }
    if (sampled == 0 || coincident < 0.8 * sampled) return;
    const double* center = mesh->GetCenter();
    m_mandibleMovement.targetLabel = label;
    m_mandibleMovement.referenceCenter = {center[0], center[1], center[2]};
    m_mandibleMovement.currentMatrix = TransformCore::MatrixToVector(TransformCore::IdentityMatrix());
}

void MainWindow::recordMandibleMovement(int label, vtkMatrix4x4* delta, bool biteStage)
{
    if (label != m_mandibleMovement.targetLabel || m_mandibleMovement.currentMatrix.size() != 16) return;
    auto total = TransformCore::ComposeTransforms(
        {TransformCore::MatrixFromVector(m_mandibleMovement.currentMatrix).GetPointer(), delta});
    const auto motion = delta ? TransformCore::DescribeRigidMovement(total, {0.0, 0.0, 0.0})
                              : TransformCore::RigidMovement{};
    m_mandibleMovement.currentMatrix = motion.valid ? TransformCore::MatrixToVector(total) : QVector<double>{};
    if (biteStage) m_mandibleMovement.registrationMatrix = m_mandibleMovement.currentMatrix;
    updateMandibleMovementSummary();
}

void MainWindow::updateMandibleMovementSummary()
{
    if (!m_mandibleMovementPanel) return;
    const bool inReposition = m_viewModeStack && m_viewModeStack->currentIndex() == 5;
    m_mandibleMovementPanel->setVisible(inReposition);
    if (!inReposition) return;
    const bool hasReference = m_mandibleMovement.referenceCenter.size() == 3 &&
        objectEntryExists(m_mandibleMovement.targetLabel);
    std::array<double, 3> reference{};
    if (hasReference)
        std::copy_n(m_mandibleMovement.referenceCenter.begin(), 3, reference.begin());
    auto describe = [&](const QVector<double>& values) {
        return hasReference && values.size() == 16
            ? TransformCore::DescribeRigidMovement(TransformCore::MatrixFromVector(values), reference)
            : TransformCore::RigidMovement{};
    };
    const auto registration = describe(m_mandibleMovement.registrationMatrix);
    const auto current = describe(m_mandibleMovement.currentMatrix);
    m_mandibleMovementReference->setText(
        tr("%1\nRef.: compuesta orientada")
            .arg(meshLabelName(hasReference ? m_mandibleMovement.targetLabel : kGenioBodyLabel)));
    m_mandibleMovementStatus->setVisible(!registration.valid || !current.valid);
    m_mandibleMovementStatus->setText(!hasReference
        ? tr("Sin referencia previa guardada.") : tr("Movimiento no disponible como transformación rígida."));
    m_mandibleMovementTable->setVisible(registration.valid || current.valid);
    const QLocale locale(QLocale::Spanish, QLocale::Colombia);
    for (int column = 1; column <= 2; ++column) {
        const auto& motion = column == 1 ? registration : current;
        for (int row = 0; row < 6; ++row) {
            const double value = row < 3 ? motion.displacementMm[row] : motion.rotationDeg[row - 3];
            const double rounded = std::abs(value) < 0.005 ? 0.0 : value;
            m_mandibleMovementTable->item(row, column)->setText(motion.valid
                ? (rounded > 0.0 ? "+" : "") + locale.toString(rounded, 'f', 2) : tr("N/D"));
        }
    }
}

void MainWindow::syncRepositionView()
{
    if (!m_repositionView) return;
    if (m_repositionView->hasGizmo())
        acceptRepositionGizmo();

    // Resolve loaded-project objects with the same lookup used by the structure list.
    // Keep existing actors until all sources have been collected, including view-only meshes.
    std::map<int, vtkSmartPointer<vtkPolyData>> sources;
    bool hadMeshes = false;
    for (int label : repositionStructureLabels()) {
        hadMeshes = hadMeshes || m_repositionView->meshData(objectActorKey(label)) != nullptr;
        if (auto source = repositionMeshForLabel(label)) {
            if (source->GetNumberOfPoints() <= 0) continue;
            auto copy = vtkSmartPointer<vtkPolyData>::New();
            copy->DeepCopy(source);
            sources[label] = copy;
        }
    }
    m_repositionView->clearPointMarkers();
    for (int label : repositionStructureLabels()) {
        const int actorKey = objectActorKey(label);
        auto it = sources.find(label);
        if (it == sources.end()) {
            m_repositionView->removeMesh(actorKey);
            continue;
        }
        m_repositionView->addMesh(actorKey, it->second, meshLabelName(label));
        m_repositionView->setMeshColor(actorKey, objectColorForLabel(label));
        m_repositionView->setMeshOpacity(actorKey, 1.0);
    }

    // Frame the available structures once, including those selected later.
    if (!hadMeshes && !sources.empty())
        m_repositionView->setStandardView(0);
    updateRepositionTargetList();
    updateRepositionControls();
    refreshRepositionPivotMarker();
    syncVisibilityPanelToAllViews();
    m_repositionView->render();
}

void MainWindow::applyRepositionTransform(vtkMatrix4x4* matrix, const QString& description)
{
    const QList<int> labels = selectedRepositionTargetLabels();
    if (labels.isEmpty() || !matrix) {
        statusBar()->showMessage(tr("Reposicion: marque al menos una estructura."));
        return;
    }

    int applied = 0;
    for (int label : labels) {
        auto mesh = repositionMeshForLabel(label);
        if (label <= 0 || !mesh || mesh->GetNumberOfPoints() <= 0) continue;
        if (m_repositionOriginalMeshes.count(label) == 0) {
            if (label == m_mandibleMovement.targetLabel)
                m_mandibleMovementResetMatrix = m_mandibleMovement.currentMatrix;
            auto original = vtkSmartPointer<vtkPolyData>::New();
            original->DeepCopy(mesh);
            m_repositionOriginalMeshes[label] = original;
        }

        auto transform = vtkSmartPointer<vtkTransform>::New();
        transform->SetMatrix(matrix);
        auto filter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
        filter->SetInputData(mesh);
        filter->SetTransform(transform);
        filter->Update();

        auto moved = vtkSmartPointer<vtkPolyData>::New();
        moved->DeepCopy(filter->GetOutput());
        setRepositionMeshForLabel(label, moved);
        recordMandibleMovement(label, matrix);
        ++applied;
    }
    clearRepositionAnalysis(); // the previous intersection no longer applies
    updateRepositionControls();
    statusBar()->showMessage(tr("Reposicion: %1 aplicado sobre %2 estructura(s).")
                                 .arg(description).arg(applied));
}

void MainWindow::beginDefineRepositionPivot()
{
    if (m_repositionPickingPivot) {
        m_repositionPickingPivot = false;
        if (m_repositionView)
            m_repositionView->setPointPickMode(false);
        updateRepositionControls();
        statusBar()->showMessage(tr("Cambio de centro cancelado. Se conserva el centro anterior."));
        return;
    }

    const int label = currentRepositionTargetLabel();
    auto mesh = repositionMeshForLabel(label);
    if (!m_repositionView || label <= 0 || !mesh || mesh->GetNumberOfPoints() <= 0) {
        statusBar()->showMessage(tr("Reposicion: seleccione un segmento valido para definir el centro."));
        updateRepositionControls();
        return;
    }

    if (m_repositionView->hasGizmo())
        acceptRepositionGizmo();

    m_repositionPickingPivot = true;
    m_repositionView->setPointPickMode(true);
    updateRepositionControls();
    statusBar()->showMessage(
        tr("Reposición: haga clic sobre %1 para cambiar el centro de rotación.")
            .arg(meshLabelName(label)));
}

void MainWindow::onRepositionPivotPicked(int actorLabel, double x, double y, double z)
{
    if (!m_repositionPickingPivot)
        return;

    const int label = currentRepositionTargetLabel();
    const int expectedActor = objectActorKey(label);
    if (label <= 0 || (actorLabel != expectedActor && actorLabel != label)) {
        statusBar()->showMessage(
            tr("Reposicion: el punto debe estar sobre el segmento activo."));
        return;
    }

    const QVector3D pivot(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
    m_repositionPivotWorld[label] = pivot;
    m_repositionPickingPivot = false;
    if (m_repositionView)
        m_repositionView->setPointPickMode(false);

    refreshRepositionPivotMarker();
    updateRepositionControls();
    statusBar()->showMessage(
        tr("Reposición: centro de rotación actualizado a %1, %2, %3.")
            .arg(x, 0, 'f', 2)
            .arg(y, 0, 'f', 2)
            .arg(z, 0, 'f', 2));
}

void MainWindow::refreshRepositionPivotMarker()
{
    if (!m_repositionView)
        return;

    m_repositionView->clearPointMarkers();
    const int label = currentRepositionTargetLabel();
    if (!m_repositionPivotWorld.contains(label))
        return;

    const QVector3D pivot = m_repositionPivotWorld.value(label);
    m_repositionView->addPointMarker(
        pivot.x(), pivot.y(), pivot.z(), QColor(10, 132, 255));
}

void MainWindow::translateRepositionTarget(double dx, double dy, double dz)
{
    const QList<int> labels = selectedRepositionTargetLabels();
    if (labels.isEmpty()) return;
    if (m_repositionRestriction == 2) {
        statusBar()->showMessage(tr("Restricción activa: solo rotación."));
        return;
    }
    auto transform = vtkSmartPointer<vtkTransform>::New();
    transform->Translate(dx, dy, dz);
    applyRepositionTransform(transform->GetMatrix(), tr("traslacion"));
    for (int label : labels) {
        m_repositionTranslationMm[label] =
            m_repositionTranslationMm.value(label) + QVector3D(dx, dy, dz);
        if (m_repositionPivotWorld.contains(label)) {
            m_repositionPivotWorld[label] =
                m_repositionPivotWorld.value(label) + QVector3D(dx, dy, dz);
        }
    }
    refreshRepositionPivotMarker();
    updateRepositionControls();
}

void MainWindow::rotateRepositionTarget(double axisX, double axisY, double axisZ, double degrees)
{
    const QList<int> labels = selectedRepositionTargetLabels();
    if (labels.isEmpty()) return;
    if (m_repositionRestriction == 1) {
        statusBar()->showMessage(tr("Restricción activa: solo traslación."));
        return;
    }
    const int activeLabel = currentRepositionTargetLabel();
    auto activeMesh = repositionMeshForLabel(activeLabel);
    if (!activeMesh || activeMesh->GetNumberOfPoints() <= 0) return;

    QVector3D pivot;
    if (m_repositionPivotWorld.contains(activeLabel)) {
        pivot = m_repositionPivotWorld.value(activeLabel);
    } else {
        double b[6] = {};
        activeMesh->GetBounds(b);
        pivot = QVector3D(static_cast<float>(0.5 * (b[0] + b[1])),
                          static_cast<float>(0.5 * (b[2] + b[3])),
                          static_cast<float>(0.5 * (b[4] + b[5])));
    }

    auto transform = vtkSmartPointer<vtkTransform>::New();
    transform->Translate(pivot.x(), pivot.y(), pivot.z());
    transform->RotateWXYZ(degrees, axisX, axisY, axisZ);
    transform->Translate(-pivot.x(), -pivot.y(), -pivot.z());
    applyRepositionTransform(transform->GetMatrix(), tr("rotacion"));

    for (int label : labels) {
        QVector3D r = m_repositionRotationDeg.value(label);
        r += QVector3D(axisX * degrees, axisY * degrees, axisZ * degrees);
        m_repositionRotationDeg[label] = r;
        if (m_repositionPivotWorld.contains(label)) {
            const QVector3D p = m_repositionPivotWorld.value(label);
            const double in[4] = {p.x(), p.y(), p.z(), 1.0};
            double out[4] = {};
            transform->GetMatrix()->MultiplyPoint(in, out);
            m_repositionPivotWorld[label] = QVector3D(
                static_cast<float>(out[0]), static_cast<float>(out[1]), static_cast<float>(out[2]));
        }
    }
    refreshRepositionPivotMarker();
    updateRepositionControls();
}

void MainWindow::resetRepositionTarget()
{
    const int label = currentRepositionTargetLabel();
    auto it = m_repositionOriginalMeshes.find(label);
    if (label <= 0 || it == m_repositionOriginalMeshes.end() || !it->second) {
        statusBar()->showMessage(tr("Reposicion: no hay transformacion para resetear."));
        return;
    }
    auto restored = vtkSmartPointer<vtkPolyData>::New();
    restored->DeepCopy(it->second);
    setRepositionMeshForLabel(label, restored);
    if (label == m_mandibleMovement.targetLabel)
        m_mandibleMovement.currentMatrix = m_mandibleMovementResetMatrix;
    m_repositionTranslationMm.remove(label);
    m_repositionRotationDeg.remove(label);
    m_repositionPivotWorld.remove(label);
    refreshRepositionPivotMarker();
    updateRepositionControls();
    statusBar()->showMessage(tr("Reposicion: %1 restaurado.").arg(meshLabelName(label)));
}

void MainWindow::sendRepositionTargetHome()
{
    resetRepositionTarget();
}

void MainWindow::startRepositionGizmo()
{
    if (!m_repositionView) return;
    if (m_repositionRestriction != 0) {
        statusBar()->showMessage(tr("El control interactivo traslada y rota: quite la restricción o use los botones."));
        return;
    }
    m_repositionPickingPivot = false;
    m_repositionView->setPointPickMode(false);
    updateRepositionControls();
    const QList<int> labels = selectedRepositionTargetLabels();
    const int current = currentRepositionTargetLabel();
    const int label = labels.contains(current) ? current : (labels.isEmpty() ? -1 : labels.front());
    auto mesh = repositionMeshForLabel(label);
    if (labels.isEmpty() || label <= 0 || !mesh || mesh->GetNumberOfPoints() <= 0) {
        statusBar()->showMessage(tr("Reposicion: marque una estructura y seleccione su fila para activar el gizmo."));
        return;
    }

    // Commit any preceding interaction before taking snapshots for the new group.
    if (m_repositionView->hasGizmo())
        m_repositionView->stopGizmo();

    m_repositionGizmoGroupLabels = labels;
    m_repositionGizmoGroupMeshes.clear();
    for (int groupLabel : labels) {
        auto groupMesh = repositionMeshForLabel(groupLabel);
        if (!groupMesh || groupMesh->GetNumberOfPoints() <= 0) continue;
        auto snapshot = vtkSmartPointer<vtkPolyData>::New();
        snapshot->DeepCopy(groupMesh);
        m_repositionGizmoGroupMeshes[groupLabel] = snapshot;
        if (m_repositionOriginalMeshes.count(groupLabel) == 0) {
            if (groupLabel == m_mandibleMovement.targetLabel)
                m_mandibleMovementResetMatrix = m_mandibleMovement.currentMatrix;
            auto original = vtkSmartPointer<vtkPolyData>::New();
            original->DeepCopy(groupMesh);
            m_repositionOriginalMeshes[groupLabel] = original;
        }
    }

    syncRepositionSelectionVisibility();
    m_repositionView->startGizmo(objectActorKey(label));
    if (!m_repositionView->hasGizmo()) {
        m_repositionGizmoGroupLabels.clear();
        m_repositionGizmoGroupMeshes.clear();
        updateRepositionControls();
        return;
    }
    if (m_repositionAcceptGizmoAct) m_repositionAcceptGizmoAct->setEnabled(true);
    if (m_repositionGizmoAct) m_repositionGizmoAct->setEnabled(false);
    statusBar()->showMessage(tr("Gizmo activo para %1. Se moveran %2 estructura(s) en bloque.")
                                 .arg(meshLabelName(label))
                                 .arg(m_repositionGizmoGroupLabels.size()));
}

void MainWindow::acceptRepositionGizmo()
{
    if (m_repositionView && m_repositionView->hasGizmo())
        m_repositionView->stopGizmo();
    if (m_repositionAcceptGizmoAct) m_repositionAcceptGizmoAct->setEnabled(false);
    if (m_repositionGizmoAct) m_repositionGizmoAct->setEnabled(currentRepositionTargetLabel() > 0);
    m_repositionGizmoGroupLabels.clear();
    m_repositionGizmoGroupMeshes.clear();
    updateRepositionControls();
}

void MainWindow::onRepositionGizmoUpdated(int meshLabel, vtkSmartPointer<vtkPolyData> newMesh)
{
    if (!newMesh || newMesh->GetNumberOfPoints() <= 0) return;
    int label = meshLabel >= 1000 ? meshLabel - 1000 : meshLabel;
    if (label <= 0)
        label = currentRepositionTargetLabel();
    if (label <= 0) return;
    auto matrix = m_repositionView
        ? m_repositionView->lastGizmoTransformMatrix()
        : TransformCore::IdentityMatrix();
    if (label == m_mandibleMovement.targetLabel ||
        m_repositionGizmoGroupLabels.contains(m_mandibleMovement.targetLabel))
        recordMandibleMovement(m_mandibleMovement.targetLabel, matrix);
    for (int groupLabel : m_repositionGizmoGroupLabels) {
        if (!m_repositionPivotWorld.contains(groupLabel)) continue;
        const QVector3D p = m_repositionPivotWorld.value(groupLabel);
        const double in[4] = {p.x(), p.y(), p.z(), 1.0};
        double out[4] = {};
        matrix->MultiplyPoint(in, out);
        m_repositionPivotWorld[groupLabel] = QVector3D(
            static_cast<float>(out[0]), static_cast<float>(out[1]), static_cast<float>(out[2]));
    }
    clearRepositionAnalysis();
    setRepositionMeshForLabel(label, newMesh);
    for (int groupLabel : m_repositionGizmoGroupLabels) {
        if (groupLabel == label) continue;
        const auto it = m_repositionGizmoGroupMeshes.find(groupLabel);
        if (it == m_repositionGizmoGroupMeshes.end() || !it->second) continue;
        auto moved = TransformCore::ApplyTransformToPolyData(it->second, matrix);
        if (moved && moved->GetNumberOfPoints() > 0)
            setRepositionMeshForLabel(groupLabel, moved);
    }
    m_repositionGizmoGroupLabels.clear();
    m_repositionGizmoGroupMeshes.clear();
    if (m_repositionAcceptGizmoAct) m_repositionAcceptGizmoAct->setEnabled(false);
    if (m_repositionGizmoAct) m_repositionGizmoAct->setEnabled(true);
    refreshRepositionPivotMarker();
    updateRepositionControls();
    statusBar()->showMessage(tr("Reposicion: ajuste interactivo aplicado a %1.").arg(meshLabelName(label)));
}

void MainWindow::setOrientationWorkspace(bool enabled)
{
    if (m_viewModeStack) {
        if (enabled)
            m_viewModeStack->setCurrentIndex(2);
        // When disabling, do NOT reset index — setModelsWorkspace already owns index 1
        // and the default MPR grid is set by its own logic.
    }

    if (enabled && m_orientationView) {
        // Orientation rotates the composites. With segmented bones and no registered scan
        // waiting for its composite, orient the bones directly (as «Continuar sin match»).
        const bool scanPending = m_compositeStage != CompositeStage::None ||
                                 (m_upperRegistrationCalculated && !m_upperCompositeMesh) ||
                                 (m_lowerRegistrationCalculated && !m_lowerCompositeMesh);
        if (!m_upperCompositeMesh && !m_lowerCompositeMesh && !scanPending && createBoneOnlyComposites())
            publishCompositeMeshesToSceneViews();
        m_orientationView->clearMeshes();
        m_orientationView->clearPointMarkers();

        if (m_upperCompositeMesh) {
            const int k = objectActorKey(kUpperCompositeLabel);
            m_orientationView->addMesh(k, m_upperCompositeMesh,
                                       meshLabelName(kUpperCompositeLabel));
            m_orientationView->setMeshColor(k, objectColorForLabel(kUpperCompositeLabel));
        }
        if (m_lowerCompositeMesh) {
            const int k = objectActorKey(kLowerCompositeLabel);
            m_orientationView->addMesh(k, m_lowerCompositeMesh,
                                       meshLabelName(kLowerCompositeLabel));
            m_orientationView->setMeshColor(k, objectColorForLabel(kLowerCompositeLabel));
        }
        // The models being oriented are always shown, even when hidden in the object list.
        for (int label : {kUpperCompositeLabel, kLowerCompositeLabel}) {
            if (!(label == kUpperCompositeLabel ? m_upperCompositeMesh : m_lowerCompositeMesh))
                continue;
            if (!objectEntryExists(label))
                addObjectEntry(meshLabelName(label), objectColorForLabel(label), label);
            if (!objectEntryVisible(label))
                setObjectEntryVisible(label, true);
        }
        // A registered scan still waiting for its composite: show the bones as reference.
        if (!m_upperCompositeMesh && !m_lowerCompositeMesh) {
            for (int i = 0; i < 2; ++i) {
                if (const auto bone = meshForAnatomicLabel(i == 0 ? 5 : 6)) {
                    const int key = kOrientationBoneReferenceKey - i;
                    m_orientationView->addMesh(key, bone, i == 0 ? tr("Maxilar (referencia)") : tr("Mandíbula (referencia)"));
                    m_orientationView->setMeshColor(key, objectColorForLabel(i == 0 ? 5 : 6));
                    m_orientationView->setMeshOpacity(key, 0.6);
                }
            }
        }

        syncVisibilityPanelToAllViews();
        m_orientationView->setStandardView(0);
        m_frankfurtPoints.clear();
        m_frankfurtCapturingIdx = -1;
        updateFrankfurtPointStatus();
        // Only Porion Der is enabled on entry — sequential unlock via updateFrankfurtPointStatus()
        if (m_frankfortPorionDAct)  { m_frankfortPorionDAct->setEnabled(true);  m_frankfortPorionDAct->setChecked(false); }
        if (m_frankfortPorionIAct)  { m_frankfortPorionIAct->setEnabled(false); m_frankfortPorionIAct->setChecked(false); }
        if (m_frankfortOrbitalDAct) { m_frankfortOrbitalDAct->setEnabled(false);m_frankfortOrbitalDAct->setChecked(false); }
        if (m_frankfortOrbitalIAct) { m_frankfortOrbitalIAct->setEnabled(false);m_frankfortOrbitalIAct->setChecked(false); }
        if (m_alignFrankfurtAct)    m_alignFrankfurtAct->setEnabled(false);
        if (m_midlineGizmoAct)      m_midlineGizmoAct->setEnabled(false);
        if (m_midlineAcceptGizmoAct) m_midlineAcceptGizmoAct->setEnabled(false);
        if (m_saveOrientationAct)   m_saveOrientationAct->setEnabled(false);
        if (m_exportOrientedAct)    m_exportOrientedAct->setEnabled(false);

        // Only accepted composites are oriented: say why the view is empty.
        const bool hasComposite = m_upperCompositeMesh || m_lowerCompositeMesh;
        m_orientationView->setTitle(hasComposite ? tr("ORIENTACIÓN — Plano de Frankfort")
                                                 : tr("ORIENTACIÓN — sin modelos compuestos"));
        if (!hasComposite) {
            if (m_frankfortPorionDAct) m_frankfortPorionDAct->setEnabled(false);
            statusBar()->showMessage(m_compositeStage != CompositeStage::None
                ? tr("Orientación: el modelo compuesto aún no está aceptado. En MODELOS pulse «Calcular compuesto» "
                     "y después «Aceptar compuesto».")
                : tr("Orientación: no hay modelos compuestos. Créelos en MODELOS o use «Continuar sin match» "
                     "para orientar con los huesos segmentados."));
        }
        m_orientationView->render();
    }

    if (!enabled && m_orientationView) {
        m_orientationView->setPointPickMode(false);
        m_frankfurtCapturingIdx = -1;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::setModelsWorkspace(bool enabled)
{
    if (m_viewModeStack)
        m_viewModeStack->setCurrentIndex(enabled ? 1 : 0);
    if (enabled) {
        updateButtonStates();
        if (m_compositeStage != CompositeStage::None) {
            showCompositeStage();
            return;
        }
        // If both composites already exist, jump directly to final view
        if (m_upperCompositeMesh && m_lowerCompositeMesh) {
            showFinalCompositeView();
        } else {
            // Reset step panel + composite button to initial state
            if (m_modelStepStack) {
                m_modelStepStack->setVisible(true);
                m_modelStepStack->setCurrentIndex(0);
            }
            if (m_compositeButton) {
                m_compositeButton->setText(tr("Crear Modelo Compuesto"));
                disconnect(m_compositeButton, &QPushButton::clicked,
                           this, &MainWindow::exportDentalCompositeStl);
                connect(m_compositeButton, &QPushButton::clicked,
                        this, &MainWindow::createDentalCompositeModels,
                        Qt::UniqueConnection);
            }
            syncModelViews();
            updateButtonStates();
        }
    }
    if (!enabled) {
        for (Mesh3DView* v : {m_modelMaxillaView, m_modelUpperArchView,
                              m_modelMandibleView, m_modelLowerArchView,
                              m_modelMatchView})
            if (v) v->setPointPickMode(false);
        if (m_mesh3DView) m_mesh3DView->setPointPickMode(false);
        m_dentalPointSet = DentalPointSet::None;
    }
}

QWidget* MainWindow::buildInfoPanel()
{
    auto* panel = new QWidget(this);
    panel->setObjectName("RightPanel");
    panel->setFixedWidth(300);
    panel->setStyleSheet(
        "#RightPanel { background:#1f1f21; border-left:1px solid #2c2c2e; }"
        // ── QTabWidget ────────────────────────────────────────────────────────
        "#RightPanel QTabWidget::pane  { border:0; background:#1f1f21; }"
        "#RightPanel QTabBar::tab      { background:#242426; color:#98989d;"
        "  padding:6px 12px; border:0; font-size:10px; min-width:70px;"
        "  border-radius:10px; margin:3px 2px; }"
        "#RightPanel QTabBar::tab:selected { background:#3a3a3c; color:#ffffff; font-weight:bold; }"
        "#RightPanel QTabBar::tab:hover { background:#2c2c2e; color:#f5f5f7; }"
        // ── Tables ────────────────────────────────────────────────────────────
        "#RightPanel QTableWidget { background:#1f1f21; color:#f5f5f7;"
        "  gridline-color:#2c2c2e; font-size:10px; border:0; }"
        "#RightPanel QHeaderView::section { background:#2c2c2e; color:#f5f5f7;"
        "  padding:5px; border:0; border-bottom:1px solid #3a3a3c; font-size:10px; font-weight:bold; }"
        "#RightPanel QTableWidget::item:selected { background:#1f3b57; color:#ffffff; }"
        // ── Buttons ───────────────────────────────────────────────────────────
        "#RightPanel QPushButton { background:#0a84ff; color:#ffffff; border:0;"
        "  border-radius:10px; padding:6px 10px; font-size:10px; font-weight:600; }"
        "#RightPanel QPushButton:hover    { background:#1d9bf0; }"
        "#RightPanel QPushButton:checked  { background:#1f3b57; }"
        "#RightPanel QPushButton:disabled { background:#242426; color:#636366; }"
        // ── Labels & misc ─────────────────────────────────────────────────────
        "#RightPanel QLabel#PanelTitle { background:#242426; color:#f5f5f7;"
        "  font-size:11px; font-weight:700; border-bottom:1px solid #2c2c2e; padding:7px; }"
        "#RightPanel QGroupBox { border:1px solid #2c2c2e; border-radius:12px;"
        "  margin-top:8px; padding-top:8px; font-size:9px; font-weight:700;"
        "  color:#f5f5f7; background:#242426; }"
        "#RightPanel QGroupBox::title { subcontrol-origin:margin; left:7px; color:#0a84ff; }"
        "#RightPanel QComboBox { background:#2c2c2e; color:#f5f5f7;"
        "  border:1px solid #3a3a3c; border-radius:10px; padding:4px; font-size:10px; }"
        "QComboBox QAbstractItemView { background:#2c2c2e; color:#f5f5f7; selection-background-color:#1f3b57; selection-color:#ffffff; }");

    auto* rootLayout = new QVBoxLayout(panel);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // ── Title bar (à la "Project Management" de Mimics) ──────────────────────
    auto* titleBar = new QLabel(tr("Project Manager"), panel);
    titleBar->setObjectName("PanelTitle");
    titleBar->setAlignment(Qt::AlignCenter);
    titleBar->setFixedHeight(28);
    rootLayout->addWidget(titleBar);

    // ── Tab widget ────────────────────────────────────────────────────────────
    auto* tabs = new QTabWidget(panel);
    tabs->setDocumentMode(true);
    rootLayout->addWidget(tabs, 1);

    // ════════════════════════════════════════════════════════════════════════
    // Tab 0 — ESTUDIO (study info)
    // ════════════════════════════════════════════════════════════════════════
    auto* studyTab    = new QWidget(tabs);
    auto* studyLayout = new QVBoxLayout(studyTab);
    studyLayout->setSizeConstraint(QLayout::SetMinimumSize);
    studyTab->setStyleSheet("background:#1f1f21;");
    studyLayout->setContentsMargins(8, 10, 8, 8);
    studyLayout->setSpacing(4);

    auto addRow = [&](const QString& heading, QLabel*& valueLabel,
                      QWidget* parent, QVBoxLayout* layout) {
        auto* head = new QLabel(heading.toUpper(), parent);
        head->setStyleSheet("color:#0a84ff; font-size:9px; font-weight:700; margin-top:4px;");
        layout->addWidget(head);
        valueLabel = new QLabel(tr("—"), parent);
        valueLabel->setWordWrap(true);
        valueLabel->setStyleSheet("color:#f5f5f7; font-size:10px; padding-left:6px;");
        layout->addWidget(valueLabel);
    };
    addRow(tr("Paciente"),       m_lblPatientName, studyTab, studyLayout);
    addRow(tr("ID"),             m_lblPatientId,   studyTab, studyLayout);
    addRow(tr("Fecha estudio"),  m_lblStudyDate,   studyTab, studyLayout);
    addRow(tr("Dimensiones"),    m_lblDimensions,  studyTab, studyLayout);
    addRow(tr("Espaciado (mm)"), m_lblSpacing,     studyTab, studyLayout);
    addRow(tr("Rango HU"),       m_lblHuRange,     studyTab, studyLayout);
    addRow(tr("Cortes"),         m_lblNumSlices,   studyTab, studyLayout);
    m_mandibleMovementPanel = new QWidget(studyTab);
    m_mandibleMovementPanel->setObjectName("MandibleMovementSummary");
    auto* movementLayout = new QVBoxLayout(m_mandibleMovementPanel);
    movementLayout->setContentsMargins(0, 10, 0, 0);
    movementLayout->setSpacing(5);
    auto* movementTitle = new QLabel(tr("MOVIMIENTO MANDIBULAR"), m_mandibleMovementPanel);
    movementTitle->setStyleSheet("color:#0a84ff; font-size:10px; font-weight:700;");
    movementLayout->addWidget(movementTitle);
    m_mandibleMovementReference = new QLabel(m_mandibleMovementPanel);
    m_mandibleMovementReference->setWordWrap(true);
    m_mandibleMovementReference->setStyleSheet("color:#f5f5f7; font-size:10px;");
    movementLayout->addWidget(m_mandibleMovementReference);
    m_mandibleMovementStatus = new QLabel(m_mandibleMovementPanel);
    m_mandibleMovementStatus->setWordWrap(true);
    m_mandibleMovementStatus->setStyleSheet("color:#ffb340; font-size:10px;");
    movementLayout->addWidget(m_mandibleMovementStatus);
    m_mandibleMovementTable = new QTableWidget(6, 3, m_mandibleMovementPanel);
    m_mandibleMovementTable->setObjectName("MandibleMovementTable");
    m_mandibleMovementTable->setHorizontalHeaderLabels({tr("Movimiento"), tr("Registro"), tr("Actual")});
    m_mandibleMovementTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_mandibleMovementTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_mandibleMovementTable->setFocusPolicy(Qt::NoFocus);
    m_mandibleMovementTable->verticalHeader()->hide();
    m_mandibleMovementTable->verticalHeader()->setMinimumSectionSize(24);
    m_mandibleMovementTable->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_mandibleMovementTable->setShowGrid(false);
    m_mandibleMovementTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_mandibleMovementTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_mandibleMovementTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_mandibleMovementTable->horizontalHeader()->setMinimumSectionSize(60);
    m_mandibleMovementTable->horizontalHeader()->setFixedHeight(26);
    m_mandibleMovementTable->setStyleSheet(
        "QTableWidget { color:#f5f5f7; font-size:10px; border:0; border-radius:0; }"
        "QHeaderView::section { color:#98989d; font-size:9px; padding:3px; }");
    m_mandibleMovementTable->setToolTip(
        tr("Traslación del centro inicial del segmento, no diferencia entre centros de mallas distintas.\n"
           "Rotaciones sobre ejes fijos XYZ: R = Rz * Ry * Rx.\n"
           "Registro: mordida aceptada y su ajuste manual. Actual: incluye la reposición posterior."));
    const QStringList movementRows {tr("Lateral X (mm)"), tr("Ant./post. Y (mm)"), tr("Vertical Z (mm)"),
                                   tr("Sagital X (°)"), tr("Coronal Y (°)"), tr("Axial Z (°)")};
    for (int row = 0; row < 6; ++row) {
        m_mandibleMovementTable->setRowHeight(row, 24);
        m_mandibleMovementTable->setItem(row, 0, new QTableWidgetItem(movementRows[row]));
        for (int column = 1; column <= 2; ++column) {
            auto* item = new QTableWidgetItem(tr("N/D"));
            item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_mandibleMovementTable->setItem(row, column, item);
        }
    }
    m_mandibleMovementTable->setFixedHeight(
        26 + 6 * 24 + 2 * m_mandibleMovementTable->frameWidth());
    movementLayout->addWidget(m_mandibleMovementTable);
    studyLayout->addWidget(m_mandibleMovementPanel);
    m_mandibleMovementPanel->hide();
    if (m_viewModeStack)
        connect(m_viewModeStack, &QStackedWidget::currentChanged, this,
                [this] { updateMandibleMovementSummary(); });
    studyLayout->addStretch(1);
    auto* studyScroll = new QScrollArea(tabs);
    studyScroll->setObjectName("StudyInfoScrollArea");
    studyScroll->setWidgetResizable(true);
    studyScroll->setFrameShape(QFrame::NoFrame);
    studyScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    studyScroll->setWidget(studyTab);
    tabs->addTab(studyScroll, tr("Estudio"));

    // ════════════════════════════════════════════════════════════════════════
    // Tab 1 — MEDIDAS (measurements)
    // ════════════════════════════════════════════════════════════════════════
    auto* measTab    = new QWidget(tabs);
    auto* measLayout = new QVBoxLayout(measTab);
    measLayout->setContentsMargins(4, 4, 4, 4);
    measLayout->setSpacing(4);

    // Measurement table (4 columns — ID stored in Qt::UserRole on col 0)
    m_measurementTable = new QTableWidget(0, 4, measTab);
    m_measurementTable->setHorizontalHeaderLabels({tr("Tipo"), tr("Vista"), tr("Valor"), tr("Texto")});
    m_measurementTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_measurementTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_measurementTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_measurementTable->verticalHeader()->hide();
    m_measurementTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_measurementTable->horizontalHeader()->setStretchLastSection(true);
    measLayout->addWidget(m_measurementTable, 1);

    // Row 1: Delete + Toggle visibility
    // (m_toggleMeasurementsAct is created in buildToolBar; use it here)
    m_deleteMeasurementButton = new QPushButton(tr("Eliminar"), measTab);
    connect(m_deleteMeasurementButton, &QPushButton::clicked,
            this, &MainWindow::deleteSelectedMeasurement);

    auto* toggleMeasBtn = new QPushButton(tr("Medidas ON"), measTab);
    toggleMeasBtn->setCheckable(true);
    toggleMeasBtn->setChecked(true);
    connect(toggleMeasBtn, &QPushButton::toggled, this, [this, toggleMeasBtn](bool v) {
        toggleMeasBtn->setText(v ? tr("Medidas ON") : tr("Medidas OFF"));
        if (m_toggleMeasurementsAct) m_toggleMeasurementsAct->setChecked(v);
    });

    auto* measBtnRow1 = new QHBoxLayout;
    measBtnRow1->setSpacing(4);
    measBtnRow1->addWidget(m_deleteMeasurementButton);
    measBtnRow1->addWidget(toggleMeasBtn);
    measLayout->addLayout(measBtnRow1);

    // Row 2: Save / Load JSON
    auto* saveJsonBtn = new QPushButton(tr("Guardar JSON"), measTab);
    connect(saveJsonBtn, &QPushButton::clicked, this, &MainWindow::saveMeasurements);
    auto* loadJsonBtn = new QPushButton(tr("Cargar JSON"), measTab);
    connect(loadJsonBtn, &QPushButton::clicked, this, &MainWindow::loadMeasurements);

    auto* measBtnRow2 = new QHBoxLayout;
    measBtnRow2->setSpacing(4);
    measBtnRow2->addWidget(saveJsonBtn);
    measBtnRow2->addWidget(loadJsonBtn);
    measLayout->addLayout(measBtnRow2);

    tabs->addTab(measTab, tr("Medidas"));

    // ════════════════════════════════════════════════════════════════════════
    // Tab 2 — MÁSCARAS (segmentation masks + 3D tools)
    // ════════════════════════════════════════════════════════════════════════
    auto* maskTab    = new QWidget(tabs);
    auto* maskLayout = new QVBoxLayout(maskTab);
    maskLayout->setContentsMargins(4, 4, 4, 4);
    maskLayout->setSpacing(6);

    // ── Dynamic mask list (starts empty; addMaskEntry() populates it) ────────
    // Col 0: color swatch — click to change  Col 1: name — double-click to edit
    // Col 2: visible checkbox
    m_maskTable = new QTableWidget(0, 3, maskTab);
    m_maskTable->setHorizontalHeaderLabels({tr(""), tr("Nombre"), tr("V")});
    m_maskTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_maskTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_maskTable->setEditTriggers(QAbstractItemView::DoubleClicked);
    m_maskTable->verticalHeader()->hide();
    m_maskTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_maskTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_maskTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_maskTable->setColumnWidth(0, 24);
    m_maskTable->setColumnWidth(2, 26);
    m_maskTable->setShowGrid(false);
    m_maskTable->setMinimumHeight(60);
    m_maskTable->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(m_maskTable, &QTableWidget::itemSelectionChanged, this, [this] {
        if (!m_maskTable || !m_maskTable->hasFocus() || !m_objectTable) return;
        const QSignalBlocker blocker(m_objectTable);
        m_objectTable->clearSelection();
    });

    // ── Color swatch click → QColorDialog → update actor color ──────────────
    connect(m_maskTable, &QTableWidget::itemClicked, this, [this](QTableWidgetItem* item) {
        if (!item || item->column() != 0) return;
        const QColor cur = item->background().color();
        const QColor nxt = QColorDialog::getColor(cur, this, tr("Color de mascara"));
        if (!nxt.isValid()) return;
        item->setBackground(QBrush(nxt));
        const int label = item->data(Qt::UserRole).toInt();
        applyMaskColorToAllViews(label, nxt);
    });

    // ── Visibility checkbox → toggle actor visibility ─────────────────────
    connect(m_maskTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (!item || item->column() != 2) return;
        const int label   = item->data(Qt::UserRole).toInt();
        const bool visible = item->checkState() == Qt::Checked;
        if (visible) {
            m_hiddenMaskLabels.erase(label);
        } else {
            m_hiddenMaskLabels.insert(label);
        }
        for (Mesh3DView* view : {m_mesh3DView, m_modelMaxillaView, m_modelUpperArchView,
                                 m_modelMandibleView, m_modelLowerArchView,
                                 m_modelMatchView, m_orientationView, m_osteotomyView,
                                 m_repositionView, m_splintUpperView, m_splintLowerView,
                                 m_splintView}) {
            if (view) view->setMeshVisible(label, visible);
        }
        syncVisibilityPanelToAllViews();
        refreshSegmentationOverlays();
    });

    connect(m_maskTable, &QTableWidget::customContextMenuRequested,
            this, [this](const QPoint& pos) {
        auto* item = m_maskTable ? m_maskTable->itemAt(pos) : nullptr;
        if (!item) return;
        const int row = item->row();
        m_maskTable->selectRow(row);

        auto* idItem = m_maskTable->item(row, 0);
        if (!idItem) idItem = m_maskTable->item(row, 1);
        const int label = idItem ? idItem->data(Qt::UserRole).toInt() : -1;
        if (label <= 0) return;

        QMenu menu(this);
        QAction* convertAct = menu.addAction(tr("Convertir máscara a objeto 3D (liso)"));
        QAction* convertExactAct = menu.addAction(tr("Convertir máscara a objeto 3D exacto (vóxeles)"));
        menu.addSeparator();
        QAction* deleteAct = menu.addAction(tr("Eliminar mascara"));
        QAction* chosen = menu.exec(m_maskTable->viewport()->mapToGlobal(pos));
        if (chosen == convertAct) {
            calculateObjectFromMask(label, true);
        } else if (chosen == convertExactAct) {
            calculateObjectFromMask(label, false);
        } else if (chosen == deleteAct) {
            deleteSelectedMask();
        }
    });

    maskLayout->addWidget(m_maskTable);

    auto* deleteMaskButton = new QPushButton(tr("Eliminar mascara"), maskTab);
    deleteMaskButton->setObjectName("ToolButton");
    connect(deleteMaskButton, &QPushButton::clicked,
            this, &MainWindow::deleteSelectedMask);
    maskLayout->addWidget(deleteMaskButton);

    maskLayout->addStretch(1);
    tabs->addTab(maskTab, tr("Mascaras"));

    auto* objectBox = new QGroupBox(tr("OBJETOS"), panel);
    objectBox->setObjectName("ObjectDisplayPanel");
    objectBox->setStyleSheet(
        "#ObjectDisplayPanel QLabel, #ObjectDisplayPanel QCheckBox { color:#f5f5f7; }"
        "#ObjectDisplayPanel QLabel:disabled, #ObjectDisplayPanel QCheckBox:disabled { color:#98989d; }");
    objectBox->setMinimumHeight(260);
    auto* objectLayout = new QVBoxLayout(objectBox);
    objectLayout->setContentsMargins(4, 4, 4, 4);
    objectLayout->setSpacing(6);

    m_objectTable = new QTableWidget(0, 3, objectBox);
    m_objectTable->setObjectName("ObjectTable");
    m_objectTable->setMinimumHeight(140);
    m_objectTable->setHorizontalHeaderLabels({tr(""), tr("Objeto"), tr("V")});
    m_objectTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_objectTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_objectTable->setEditTriggers(QAbstractItemView::DoubleClicked);
    m_objectTable->verticalHeader()->hide();
    m_objectTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_objectTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_objectTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_objectTable->setColumnWidth(0, 24);
    m_objectTable->setColumnWidth(2, 26);
    m_objectTable->setShowGrid(false);

    connect(m_objectTable, &QTableWidget::itemSelectionChanged, this, [this] {
        updateObjectAppearanceControls();
        if (!m_objectTable || !m_objectTable->hasFocus() || !m_maskTable) return;
        const QSignalBlocker blocker(m_maskTable);
        m_maskTable->clearSelection();
    });

    connect(m_objectTable, &QTableWidget::itemClicked, this, [this](QTableWidgetItem* item) {
        if (!item || item->column() != 0) return;
        const QColor cur = item->background().color();
        const QColor nxt = QColorDialog::getColor(cur, this, tr("Color de objeto"));
        if (!nxt.isValid()) return;
        item->setBackground(QBrush(nxt));
        const int label = item->data(Qt::UserRole + 1).toInt();
        applyObjectColorToAllViews(label, nxt);
    });

    connect(m_objectTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (!item || item->column() != 2) return;
        syncVisibilityPanelToAllViews();
        if (m_viewModeStack && m_viewModeStack->currentIndex() == 4)
            syncBiteRegistrationView();
    });

    objectLayout->addWidget(m_objectTable, 1);

    auto* opacityRow = new QHBoxLayout;
    opacityRow->setSpacing(6);
    auto* opacityLabel = new QLabel(tr("Opacidad"), objectBox);
    m_objectOpacitySlider = new QSlider(Qt::Horizontal, objectBox);
    m_objectOpacitySlider->setObjectName("ObjectOpacitySlider");
    m_objectOpacitySlider->setRange(0, 100);
    m_objectOpacitySlider->setValue(100);
    m_objectOpacitySlider->setAccessibleName(tr("Opacidad del objeto"));
    m_objectOpacitySlider->setToolTip(tr("Opacidad del objeto seleccionado"));
    m_objectOpacitySlider->setStyleSheet(
        "QSlider::groove:horizontal { height:4px; background:#48484a; border-radius:2px; }"
        "QSlider::sub-page:horizontal { background:#0a84ff; border-radius:2px; }"
        "QSlider::handle:horizontal { background:#f5f5f7; border:1px solid #98989d; width:12px; margin:-5px 0; border-radius:6px; }"
        "QSlider::handle:horizontal:disabled { background:#636366; }");
    opacityLabel->setBuddy(m_objectOpacitySlider);
    m_objectOpacityValue = new QLabel("100 %", objectBox);
    m_objectOpacityValue->setMinimumWidth(44);
    m_objectOpacityValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    opacityRow->addWidget(opacityLabel);
    opacityRow->addWidget(m_objectOpacitySlider, 1);
    opacityRow->addWidget(m_objectOpacityValue);
    objectLayout->addLayout(opacityRow);
    m_objectOnTopCheck = new QCheckBox(tr("Mostrar por encima"), objectBox);
    m_objectOnTopCheck->setObjectName("ObjectAlwaysOnTop");
    m_objectOnTopCheck->setToolTip(tr("Superposición visual sin cambiar la posición del objeto."));
    objectLayout->addWidget(m_objectOnTopCheck);
    auto changeAppearance = [this] {
        const auto* item = m_objectTable->item(m_objectTable->currentRow(), 0);
        if (!item || !m_objectTable->selectionModel()->hasSelection()) return;
        setObjectDisplayOptions(item->data(Qt::UserRole + 1).toInt(),
                                m_objectOpacitySlider->value() / 100.0, m_objectOnTopCheck->isChecked());
    };
    connect(m_objectOpacitySlider, &QSlider::valueChanged, this, changeAppearance);
    connect(m_objectOnTopCheck, &QCheckBox::toggled, this, changeAppearance);
    connect(m_objectTable->model(), &QAbstractItemModel::rowsAboutToBeRemoved, this,
            [this](const QModelIndex&, int first, int last) {
                for (int row = first; row <= last; ++row) {
                    if (auto* item = m_objectTable->item(row, 0))
                        applyObjectDisplayOptionsToAllViews(item->data(Qt::UserRole + 1).toInt(), -1.0, false);
                }
            });
    updateObjectAppearanceControls();

    auto* deleteObjectButton = new QPushButton(tr("Eliminar objeto"), objectBox);
    connect(deleteObjectButton, &QPushButton::clicked, this, [this] {
        if (!m_objectTable || m_objectTable->currentRow() < 0) return;
        auto* item = m_objectTable->item(m_objectTable->currentRow(), 0);
        if (!item) item = m_objectTable->item(m_objectTable->currentRow(), 1);
        const int actorKey = item ? item->data(Qt::UserRole).toInt() : -1;
        const int label = item ? item->data(Qt::UserRole + 1).toInt() : -1;
        for (Mesh3DView* v : {m_mesh3DView, m_modelMaxillaView, m_modelUpperArchView,
                              m_modelMandibleView, m_modelLowerArchView,
                              m_modelMatchView, m_orientationView, m_osteotomyView,
                              m_repositionView, m_splintUpperView, m_splintLowerView,
                              m_splintView}) {
            if (!v) continue;
            if (actorKey > 0) v->removeMesh(actorKey);
            if (label > 0) v->removeMesh(label);
        }
        if (label > 0) removeObjectEntry(label);
    });
    objectLayout->addWidget(deleteObjectButton);
    rootLayout->addWidget(objectBox, 0);

    return panel;
}

// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::onOpenDicomFolder()
{
    const QString folder = QFileDialog::getExistingDirectory(
        this,
        tr("Select DICOM Series Folder"),
        QString(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    if (!folder.isEmpty())
        loadVolume(folder);
}

// ─────────────────────────────────────────────────────────────────────────────
// onSaveProject
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::onSaveProject()
{
    // Suggest a filename next to the previously used project file, if any
    const QString defaultPath = m_projectFilePath.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
              + QStringLiteral("/proyecto.maxilloproject")
        : m_projectFilePath;

    QString path = QFileDialog::getSaveFileName(
        this,
        tr("Guardar proyecto"),
        defaultPath,
        tr("Proyecto de Planificación Maxilofacial (*.maxilloproject)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".maxilloproject"), Qt::CaseInsensitive))
        path += QStringLiteral(".maxilloproject");

    saveProjectTo(path);
}

// ─────────────────────────────────────────────────────────────────────────────
// collectProjectState — snapshot of everything the project file stores.
// ─────────────────────────────────────────────────────────────────────────────
ProjectState MainWindow::collectProjectState() const
{
    ProjectState state;
    state.dicomFolder   = m_dicomFolder;
    state.presetWindow  = m_currentPreset.window;
    state.presetLevel   = m_currentPreset.level;
    state.labelmap      = m_segmentationLabelmap;
    state.upperArchOriginalMesh = m_upperArchOriginalMesh;
    state.lowerArchOriginalMesh = m_lowerArchOriginalMesh;
    state.upperArchMesh      = m_upperArchMesh;
    state.lowerArchMesh      = m_lowerArchMesh;
    state.upperCompositeMesh = m_upperCompositeMesh;
    state.lowerCompositeMesh = m_lowerCompositeMesh;
    for (const auto& [label, mesh] : m_repositionOriginalMeshes)
        if (mesh)
            state.preRepositionMeshes[label] = mesh;
    state.mandibleMovement = m_mandibleMovement;
    state.maxillaBonePoints  = m_maxillaBonePoints;
    state.upperArchPoints    = m_upperArchPoints;
    state.mandibleBonePoints = m_mandibleBonePoints;
    state.lowerArchPoints    = m_lowerArchPoints;
    state.upperRegistrationMatrix = TransformCore::MatrixToVector(m_upperArchRegistrationMatrix);
    state.lowerRegistrationMatrix = TransformCore::MatrixToVector(m_lowerArchRegistrationMatrix);
    state.upperRegistrationReport = m_upperRegistrationReport;
    state.lowerRegistrationReport = m_lowerRegistrationReport;
    state.upperRegistrationCalculated = m_upperRegistrationCalculated;
    state.lowerRegistrationCalculated = m_lowerRegistrationCalculated;
    // Phase 5: registration metrics
    state.upperLandmarkRms = m_upperRegResult.landmarkRms;
    state.upperMeanDist    = m_upperRegResult.meanDistance;
    state.upperP95Dist     = m_upperRegResult.p95Distance;
    state.lowerLandmarkRms = m_lowerRegResult.landmarkRms;
    state.lowerMeanDist    = m_lowerRegResult.meanDistance;
    state.lowerP95Dist     = m_lowerRegResult.p95Distance;

    // Hidden labels
    for (int lbl : m_hiddenMaskLabels)
        state.hiddenMaskLabels.append(lbl);

    // Mask table rows
    if (m_maskTable) {
        for (int row = 0; row < m_maskTable->rowCount(); ++row) {
            auto* col0 = m_maskTable->item(row, 0);
            auto* col1 = m_maskTable->item(row, 1);
            auto* col2 = m_maskTable->item(row, 2);
            if (!col0) continue;
            ProjMaskEntry e;
            e.label   = col0->data(Qt::UserRole).toInt();
            e.name    = col1 ? col1->text() : meshLabelName(e.label);
            e.color   = col0->background().color();
            e.visible = col2 ? (col2->checkState() == Qt::Checked) : true;
            state.masks.append(e);
            // Also grab the mesh
            if (auto mesh = m_mesh3DView ? m_mesh3DView->meshData(e.label) : nullptr)
                state.maskMeshes[e.label] = mesh;
        }
    }

    // Object table rows
    if (m_objectTable) {
        for (int row = 0; row < m_objectTable->rowCount(); ++row) {
            auto* col0 = m_objectTable->item(row, 0);
            auto* col1 = m_objectTable->item(row, 1);
            auto* col2 = m_objectTable->item(row, 2);
            if (!col0) continue;
            const int label    = col0->data(Qt::UserRole + 1).toInt();
            const int actorKey = col0->data(Qt::UserRole).toInt();
            // Skip special arch/composite labels — saved separately above
            ProjObjectEntry e;
            e.label = label;
            e.name  = col1 ? col1->text() : meshLabelName(label);
            e.color = col0->background().color();
            e.visible = col2 ? (col2->checkState() == Qt::Checked) : true;
            e.opacity = col0->data(kObjectOpacityRole).isValid() ? col0->data(kObjectOpacityRole).toDouble() : -1.0;
            e.alwaysOnTop = col0->data(kObjectOnTopRole).toBool();
            state.objects.append(e);
            const bool savedSeparately =
                label == kUpperArchLabel || label == kLowerArchLabel ||
                label == kUpperCompositeLabel || label == kLowerCompositeLabel;
            if (!savedSeparately && m_mesh3DView) {
                if (auto mesh = m_mesh3DView->meshData(actorKey))
                    state.objectMeshes[label] = mesh;
            }
        }
    }

    state.splintDesigns = SplintDesignCore::DesignsToJson(m_splintDesigns);
    state.compositeBlocks = compositeBlocksJson();
    state.osteotomyPlan = osteotomyPlanJson();
    state.guidesPlan = guidePlanJson();
    if (m_activeSplintDesign >= 0 && m_activeSplintDesign < static_cast<int>(m_splintDesigns.size()))
        state.activeSplintDesignId = m_splintDesigns[static_cast<size_t>(m_activeSplintDesign)].id;
    return state;
}

void MainWindow::saveProjectTo(const QString& path)
{
    const ProjectState state = collectProjectState();

    statusBar()->showMessage(tr("Guardando proyecto…"));
    QApplication::processEvents();

    QString saveError;
    if (!ProjectSerializer::save(path, state, &saveError)) {
        QMessageBox::critical(this, tr("Guardar proyecto"), saveError);
        statusBar()->showMessage(tr("Error al guardar el proyecto."));
        return;
    }

    m_projectFilePath = path;
    setWindowTitle(QStringLiteral("Planificación Maxilofacial - ")
                   + QFileInfo(path).completeBaseName());
    LoggerCore::instance().logExport(path, QStringLiteral("project"));
    statusBar()->showMessage(tr("Proyecto guardado: ") + path);
}

// ─────────────────────────────────────────────────────────────────────────────
// onOpenProject
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::onOpenProject()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("Abrir proyecto"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        tr("Proyecto de Planificación Maxilofacial (*.maxilloproject)"));
    if (path.isEmpty()) return;

    QString loadError;
    ProjectState state;
    if (!ProjectSerializer::load(path, state, &loadError)) {
        QMessageBox::critical(this, tr("Abrir proyecto"), loadError);
        return;
    }

    m_projectFilePath = path;

    // If the DICOM folder is available, reload the volume (uses cache — fast).
    // The rest of the project state will be applied in onVolumeReady() once
    // the async load completes.
    setWindowTitle(QStringLiteral("Planificación Maxilofacial - ")
                   + QFileInfo(path).completeBaseName());

    if (!state.dicomFolder.isEmpty() && QDir(state.dicomFolder).exists()) {
        m_pendingProjectState = state;
        loadVolume(state.dicomFolder);
        statusBar()->showMessage(tr("Cargando proyecto… (recargando volumen DICOM)"));
    } else {
        // DICOM folder unavailable — apply everything except the volume
        if (!state.dicomFolder.isEmpty())
            QMessageBox::warning(this, tr("Abrir proyecto"),
                tr("La carpeta DICOM \"%1\" no existe en este equipo.\n"
                   "Se cargarán los meshes y la segmentación sin el volumen.")
                    .arg(state.dicomFolder));
        applyProjectState(state);
        statusBar()->showMessage(tr("Proyecto cargado (sin volumen DICOM): ") + path);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// applyProjectState — restores segmentation / meshes / tables from a loaded
// ProjectState.  Called either directly (no DICOM) or from onVolumeReady()
// after the async DICOM load finishes.
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::applyProjectState(const ProjectState& state)
{
    cancelBoneCavityFill();
    m_mandibleMovement = {};
    m_mandibleMovementResetMatrix.clear();
    // ── Segmentation labelmap ─────────────────────────────────────────────
    m_segmentationLabelmap = state.labelmap;
    m_maskSmoothingIterations.clear();
    m_hiddenMaskLabels.clear();
    for (int lbl : state.hiddenMaskLabels)
        m_hiddenMaskLabels.insert(lbl);
    refreshSegmentationOverlays();

    // ── Window / level preset ─────────────────────────────────────────────
    WindowLevelPreset preset;
    preset.window = state.presetWindow;
    preset.level  = state.presetLevel;
    preset.name   = "Custom";
    broadcastPreset(preset);
    m_currentPreset = preset;

    // ── Clear tables + views ──────────────────────────────────────────────
    if (m_maskTable)   m_maskTable->setRowCount(0);
    if (m_objectTable) m_objectTable->setRowCount(0);
    if (m_mesh3DView)  m_mesh3DView->clearMeshes();
    for (Mesh3DView* v : {m_modelMaxillaView, m_modelUpperArchView,
                          m_modelMandibleView, m_modelLowerArchView,
                          m_modelMatchView})
        if (v) v->clearMeshes();
    if (m_orientationView) m_orientationView->clearMeshes();
    if (m_osteotomyView) m_osteotomyView->clearMeshes();
    if (m_biteSegmentView) m_biteSegmentView->clearMeshes();
    if (m_biteScanView) m_biteScanView->clearMeshes();
    if (m_biteRegistrationView) m_biteRegistrationView->clearMeshes();
    if (m_repositionView) m_repositionView->clearMeshes();
    if (m_splintUpperView) m_splintUpperView->clearMeshes();
    if (m_splintLowerView) m_splintLowerView->clearMeshes();
    if (m_splintView) m_splintView->clearMeshes();
    m_repositionOriginalMeshes.clear();
    for (auto it = state.preRepositionMeshes.cbegin(); it != state.preRepositionMeshes.cend(); ++it)
        m_repositionOriginalMeshes[it.key()] = it.value();
    m_repositionTranslationMm.clear();
    m_repositionRotationDeg.clear();
    m_repositionTargetLabel = -1;
    if (m_repositionObjectList) m_repositionObjectList->clear();
    m_biteScanMesh = nullptr;
    m_preBiteMandibleMeshForSplint = nullptr;
    m_orientedInitialMandibleMeshForSplint = nullptr;
    m_bitePointSet = BitePointSet::None;
    m_biteLeFortSegmentPoints.clear();
    m_biteUpperScanPoints.clear();
    m_biteMandibleSegmentPoints.clear();
    m_biteLowerScanPoints.clear();
    m_biteLeFortRegistered = false;
    m_biteMandibleRegistered = false;
    m_biteGizmoTargetLabel = -1;
    m_biteGizmoActive = false;

    // ── Restore arch / composite meshes ──────────────────────────────────
    m_upperArchMesh      = state.upperArchMesh;
    m_lowerArchMesh      = state.lowerArchMesh;
    m_upperArchOriginalMesh = state.upperArchOriginalMesh;
    m_lowerArchOriginalMesh = state.lowerArchOriginalMesh;
    if (!m_upperArchOriginalMesh && m_upperArchMesh) {
        m_upperArchOriginalMesh = vtkSmartPointer<vtkPolyData>::New();
        m_upperArchOriginalMesh->DeepCopy(m_upperArchMesh);
    }
    if (!m_lowerArchOriginalMesh && m_lowerArchMesh) {
        m_lowerArchOriginalMesh = vtkSmartPointer<vtkPolyData>::New();
        m_lowerArchOriginalMesh->DeepCopy(m_lowerArchMesh);
    }
    m_upperCompositeMesh = state.upperCompositeMesh;
    m_lowerCompositeMesh = state.lowerCompositeMesh;

    // Backward compatibility: older project files may have stored arches or
    // composites only as generic object meshes. Rehydrate the internal model
    // pointers so MODELOS keeps working after loading those projects.
    auto legacyObjectMesh = [&](int label) -> vtkSmartPointer<vtkPolyData> {
        auto it = state.objectMeshes.find(label);
        return it != state.objectMeshes.end() ? it.value() : nullptr;
    };
    if (!m_upperArchMesh)      m_upperArchMesh      = legacyObjectMesh(kUpperArchLabel);
    if (!m_lowerArchMesh)      m_lowerArchMesh      = legacyObjectMesh(kLowerArchLabel);
    if (!m_upperCompositeMesh) m_upperCompositeMesh = legacyObjectMesh(kUpperCompositeLabel);
    if (!m_lowerCompositeMesh) m_lowerCompositeMesh = legacyObjectMesh(kLowerCompositeLabel);
    m_intermediateSplintMesh = legacyObjectMesh(kIntermediateSplintLabel);
    m_finalSplintMesh = legacyObjectMesh(kFinalSplintLabel);

    m_upperArchRegistrationMatrix = TransformCore::MatrixFromVector(state.upperRegistrationMatrix);
    m_lowerArchRegistrationMatrix = TransformCore::MatrixFromVector(state.lowerRegistrationMatrix);
    m_upperRegistrationReport = state.upperRegistrationReport;
    m_lowerRegistrationReport = state.lowerRegistrationReport;
    m_upperRegistrationCalculated = state.upperRegistrationCalculated;
    m_lowerRegistrationCalculated = state.lowerRegistrationCalculated;

    // ── Restore dental registration points ───────────────────────────────
    m_maxillaBonePoints  = state.maxillaBonePoints;
    m_upperArchPoints    = state.upperArchPoints;
    m_mandibleBonePoints = state.mandibleBonePoints;
    m_lowerArchPoints    = state.lowerArchPoints;

    // ── Restore mask meshes + table ───────────────────────────────────────
    for (const auto& e : state.masks) {
        addMaskEntry(e.name, e.color, e.label);
        if (!e.visible) {
            m_hiddenMaskLabels.insert(e.label);
            // update checkbox in table
            if (m_maskTable) {
                for (int row = 0; row < m_maskTable->rowCount(); ++row) {
                    auto* item = m_maskTable->item(row, 2);
                    if (item && item->data(Qt::UserRole).toInt() == e.label)
                        item->setCheckState(Qt::Unchecked);
                }
            }
        }
        if (auto it = state.maskMeshes.find(e.label); it != state.maskMeshes.end()) {
            if (m_mesh3DView) {
                m_mesh3DView->addMesh(e.label, it.value(), e.name);
                m_mesh3DView->setMeshColor(e.label, e.color);
            }
        }
    }

    // ── Restore object meshes + table ─────────────────────────────────────
    for (const auto& e : state.objects) {
        addObjectEntry(e.name, e.color, e.label);
        if (e.opacity >= 0.0)
            setObjectDisplayOptions(e.label, e.opacity, e.alwaysOnTop);
        if (auto it = state.objectMeshes.find(e.label); it != state.objectMeshes.end()) {
            const int actorKey = objectActorKey(e.label);
            if (m_mesh3DView) {
                m_mesh3DView->addMesh(actorKey, it.value(), e.name);
                m_mesh3DView->setMeshColor(actorKey, e.color);
            }
        }
        setObjectEntryVisible(e.label, e.visible);
    }

    // ── Restore special arch / composite objects ──────────────────────────
    auto restoreSpecial = [&](vtkPolyData* mesh, int label) {
        if (!mesh) return;
        const int actorKey = objectActorKey(label);
        const QColor color = meshLabelColor(label);
        const QString name = meshLabelName(label);
        if (m_mesh3DView) {
            m_mesh3DView->addMesh(actorKey, mesh, name);
            m_mesh3DView->setMeshColor(actorKey, color);
        }
        addObjectEntry(name, color, label);
    };
    restoreSpecial(m_upperArchMesh.Get(),      kUpperArchLabel);
    restoreSpecial(m_lowerArchMesh.Get(),      kLowerArchLabel);
    restoreSpecial(m_upperCompositeMesh.Get(), kUpperCompositeLabel);
    restoreSpecial(m_lowerCompositeMesh.Get(), kLowerCompositeLabel);
    restoreSpecial(m_intermediateSplintMesh.Get(), kIntermediateSplintLabel);
    restoreSpecial(m_finalSplintMesh.Get(),        kFinalSplintLabel);
    restoreSplintDesigns(state);
    restoreCompositeBlocks(state);
    restoreOsteotomyPlan(state);
    syncVisibilityPanelToAllViews();

    // ── Populate MODELOS views ────────────────────────────────────────────
    syncModelViews();

    // ── Phase 5: restore RegistrationResult metrics ───────────────────────
    m_upperRegResult = {};
    m_upperRegResult.landmarkRms  = state.upperLandmarkRms;
    m_upperRegResult.meanDistance = state.upperMeanDist;
    m_upperRegResult.p95Distance  = state.upperP95Dist;
    m_upperRegResult.accepted     = state.upperRegistrationCalculated;
    m_upperRegResult.report       = state.upperRegistrationReport;
    m_upperRegResult.matrix       = state.upperRegistrationMatrix;

    m_lowerRegResult = {};
    m_lowerRegResult.landmarkRms  = state.lowerLandmarkRms;
    m_lowerRegResult.meanDistance = state.lowerMeanDist;
    m_lowerRegResult.p95Distance  = state.lowerP95Dist;
    m_lowerRegResult.accepted     = state.lowerRegistrationCalculated;
    m_lowerRegResult.report       = state.lowerRegistrationReport;
    m_lowerRegResult.matrix       = state.lowerRegistrationMatrix;

    // ── Phase 1: sync AppStateManager from loaded state ───────────────────
    m_appState.setVolumeLoaded(true);   // volume was already loaded or is being loaded
    m_appState.setSegmentationDone(state.labelmap != nullptr);
    m_appState.setUpperArchImported(m_upperArchMesh != nullptr);
    m_appState.setLowerArchImported(m_lowerArchMesh != nullptr);
    m_appState.setUpperRegistered(state.upperRegistrationCalculated);
    m_appState.setLowerRegistered(state.lowerRegistrationCalculated);
    m_appState.setUpperCompositeReady(m_upperCompositeMesh != nullptr);
    m_appState.setLowerCompositeReady(m_lowerCompositeMesh != nullptr);
    m_mandibleMovement = state.mandibleMovement;
    updateMandibleMovementSummary();
    updateButtonStates();

    refreshSegmentationOverlays();
    refreshInfoPanel();
}

// ─────────────────────────────────────────────────────────────────────────────
// loadVolume — async entry point
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::loadVolume(const QString& folderPath)
{
    if (m_loadInProgress) return;   // guard against double-click
    cancelBoneCavityFill();
    m_mandibleMovement = {};
    m_mandibleMovementResetMatrix.clear();
    updateMandibleMovementSummary();

    m_dicomFolder = folderPath;  // remember for project save

    // Phase 1: reset state machine for new study
    m_appState.reset();
    updateButtonStates();

    // Reset state for new study
    m_measurementManager.clear();
    m_segmentationLabelmap = nullptr;
    m_maskSmoothingIterations.clear();
    m_hiddenMaskLabels.clear();
    refreshSegmentationOverlays();
    if (m_mesh3DView) m_mesh3DView->clearMeshes();
    for (Mesh3DView* v : {m_modelMaxillaView, m_modelUpperArchView,
                          m_modelMandibleView, m_modelLowerArchView,
                          m_modelMatchView})
        if (v) v->clearMeshes();
    if (m_maskTable) m_maskTable->setRowCount(0);
    if (m_objectTable) m_objectTable->setRowCount(0);
    m_upperArchMesh = nullptr;
    m_lowerArchMesh = nullptr;
    m_upperArchOriginalMesh = nullptr;
    m_lowerArchOriginalMesh = nullptr;
    m_upperCompositeMesh = nullptr;
    m_lowerCompositeMesh = nullptr;
    m_upperArchRegistrationMatrix = identityMatrix();
    m_lowerArchRegistrationMatrix = identityMatrix();
    m_upperRegistrationReport.clear();
    m_lowerRegistrationReport.clear();
    m_upperRegistrationCalculated = false;
    m_lowerRegistrationCalculated = false;
    m_upperRegResult = {};
    m_lowerRegResult = {};
    m_maxillaBonePoints.clear();
    m_upperArchPoints.clear();
    m_mandibleBonePoints.clear();
    m_lowerArchPoints.clear();
    m_frankfurtPoints.clear();
    m_frankfurtCapturingIdx = -1;
    m_dentalPointSet = DentalPointSet::None;
    if (m_toggleMeasurementsAct) {
        m_toggleMeasurementsAct->setChecked(true);
        m_measurementManager.setAllVisible(true);
    }
    m_previewActive = false;
    m_currentPreset = WindowLevelPresets::Bone;

    setLoadingUiEnabled(true);

    // Kick off on the worker thread (queued connection)
    QMetaObject::invokeMethod(
        m_asyncLoader, "startLoad",
        Qt::QueuedConnection,
        Q_ARG(QString, folderPath),
        Q_ARG(QString, m_cacheDir));
}

// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::setLoadingUiEnabled(bool loading)
{
    m_loadInProgress = loading;
    m_progressBar->setVisible(loading);
    if (m_openAction) m_openAction->setEnabled(!loading);
}

// ─────────────────────────────────────────────────────────────────────────────
// updateButtonStates  (Phase 1)
//
// Reads m_appState and enables/disables all toolbar actions whose enabled
// state depends on workflow progress.  Call this whenever state changes.
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::setGuidedActionState(QAction* action, const QString& state)
{
    if (!action) return;
    action->setProperty("guideState", state);

    const auto objects = action->associatedObjects();
    for (QObject* object : objects) {
        auto* widget = qobject_cast<QWidget*>(object);
        if (!widget) continue;
        widget->setProperty("guideState", state);
        if (widget->style()) {
            widget->style()->unpolish(widget);
            widget->style()->polish(widget);
        }
        widget->update();
    }
}

void MainWindow::clearGuidedActionStates(const QVector<QAction*>& actions)
{
    for (QAction* action : actions)
        setGuidedActionState(action, QString());
}

void MainWindow::setGuidedNext(QAction* action, const QString& message)
{
    setGuidedActionState(action, QStringLiteral("next"));
    if (!message.isEmpty() && statusBar())
        statusBar()->showMessage(message);
}

void MainWindow::setGuidedDone(QAction* action)
{
    setGuidedActionState(action, QStringLiteral("done"));
}

void MainWindow::updateButtonStates()
{
    using C = AppCapability;

    const bool hasMaxilla =
        m_mesh3DView &&
        (m_mesh3DView->meshData(objectActorKey(5)) || m_mesh3DView->meshData(5));
    const bool hasMandible =
        m_mesh3DView &&
        (m_mesh3DView->meshData(objectActorKey(6)) || m_mesh3DView->meshData(6));
    const int upperPairs = std::min(m_maxillaBonePoints.size(), m_upperArchPoints.size());
    const int lowerPairs = std::min(m_mandibleBonePoints.size(), m_lowerArchPoints.size());
    const bool hasAnyPoints =
        !m_maxillaBonePoints.isEmpty() || !m_upperArchPoints.isEmpty() ||
        !m_mandibleBonePoints.isEmpty() || !m_lowerArchPoints.isEmpty();
    const bool hasExportableObject =
        m_upperArchMesh || m_lowerArchMesh || m_upperCompositeMesh || m_lowerCompositeMesh ||
        m_intermediateSplintMesh || m_finalSplintMesh || hasMaxilla || hasMandible;
    const bool hasVolume = m_volume != nullptr;
    const bool hasSegmentation = m_segmentationLabelmap != nullptr;

    clearGuidedActionStates({
        m_openAction, m_saveProjectAct, m_autoSegmentationAct,
        m_toggleSegmentationOverlayAct, m_splitAct
    });

    m_appState.setMaxillaAvailable(hasMaxilla);
    m_appState.setMandibleAvailable(hasMandible);
    m_appState.setUpperPointPairCount(upperPairs);
    m_appState.setLowerPointPairCount(lowerPairs);
    m_appState.setAnyRegistrationPoints(hasAnyPoints);
    m_appState.setExportableObjectAvailable(hasExportableObject);

    if (m_saveProjectAct)  m_saveProjectAct->setEnabled(m_appState.can(C::SaveProject));
    if (m_autoSegmentationAct) m_autoSegmentationAct->setEnabled(hasVolume && !m_loadInProgress);
    if (m_manualAirwayAct) m_manualAirwayAct->setEnabled(hasVolume && !m_loadInProgress);
    if (m_toggleSegmentationOverlayAct) m_toggleSegmentationOverlayAct->setEnabled(hasSegmentation);
    if (m_fillBoneCavityAct) m_fillBoneCavityAct->setEnabled(hasVolume && hasSegmentation && !m_loadInProgress);
    if (m_splitAct)        m_splitAct->setEnabled(m_appState.can(C::SplitBone));
    if (m_importUpperAct)  m_importUpperAct->setEnabled(m_appState.can(C::ImportUpperArch));
    if (m_importLowerAct)  m_importLowerAct->setEnabled(m_appState.can(C::ImportLowerArch));
    if (m_maxPtsAct)       m_maxPtsAct->setEnabled(m_appState.can(C::SelectMaxillaryPoints));
    if (m_upperPtsAct)     m_upperPtsAct->setEnabled(m_appState.can(C::SelectMaxillaryPoints));
    if (m_mandPtsAct)      m_mandPtsAct->setEnabled(m_appState.can(C::SelectMandibularPoints));
    if (m_lowerPtsAct)     m_lowerPtsAct->setEnabled(m_appState.can(C::SelectMandibularPoints));
    if (m_matchUpperAct)   m_matchUpperAct->setEnabled(m_appState.can(C::MatchUpper));
    if (m_matchLowerAct)   m_matchLowerAct->setEnabled(m_appState.can(C::MatchLower));
    if (m_matchBothAct)    m_matchBothAct->setEnabled(m_appState.can(C::MatchBoth));
    if (m_adjustArchAct)   m_adjustArchAct->setEnabled(
        (m_upperRegistrationCalculated && m_upperArchMesh) ||
        (m_lowerRegistrationCalculated && m_lowerArchMesh));
    if (m_acceptGizmoAct)  m_acceptGizmoAct->setEnabled(m_dentalGizmoActive);
    if (m_compositeAct)    m_compositeAct->setEnabled(m_appState.can(C::CreateComposite));
    if (m_continueNoMatchAct) m_continueNoMatchAct->setEnabled((hasMaxilla || hasMandible) && !m_compositeInProgress);
    if (m_exportAct)       m_exportAct->setEnabled(m_appState.can(C::ExportComposite));
    if (m_exportPackAct)   m_exportPackAct->setEnabled(m_appState.can(C::ExportPackage));
    if (m_clearPtsAct)     m_clearPtsAct->setEnabled(m_appState.can(C::ClearPoints));
    if (m_resetArchAct)    m_resetArchAct->setEnabled(m_appState.can(C::ResetStl));
    auto hasPoly = [](vtkPolyData* mesh) {
        return mesh && mesh->GetNumberOfPoints() > 0;
    };
    const bool hasUpperForSplint =
        hasPoly(repositionMeshForLabel(kLeFortSegLabel).Get()) ||
        hasPoly(repositionMeshForLabel(kUpperCompositeLabel).Get()) ||
        hasPoly(repositionMeshForLabel(5).Get()) ||
        hasPoly(meshForAnatomicLabel(5).Get());
    const bool hasInitialMandibleForSplint =
        hasPoly(m_orientedInitialMandibleMeshForSplint) ||
        hasPoly(m_preBiteMandibleMeshForSplint) ||
        hasPoly(repositionMeshForLabel(kGenioBodyLabel).Get()) ||
        hasPoly(repositionMeshForLabel(kBssoDistalLabel).Get()) ||
        hasPoly(repositionMeshForLabel(kLowerCompositeLabel).Get()) ||
        hasPoly(repositionMeshForLabel(6).Get()) ||
        hasPoly(meshForAnatomicLabel(6).Get());
    const bool hasFinalMandibleForSplint =
        hasPoly(repositionMeshForLabel(currentBiteMandibleTargetLabel()).Get()) ||
        hasPoly(repositionMeshForLabel(kGenioBodyLabel).Get()) ||
        hasPoly(repositionMeshForLabel(kBssoDistalLabel).Get()) ||
        hasPoly(repositionMeshForLabel(kLowerCompositeLabel).Get()) ||
        hasPoly(repositionMeshForLabel(6).Get()) ||
        hasPoly(meshForAnatomicLabel(6).Get());
    if (m_splintIntermediateAct) m_splintIntermediateAct->setEnabled(
        !m_splintInProgress && hasUpperForSplint && hasInitialMandibleForSplint);
    if (m_splintFinalAct) m_splintFinalAct->setEnabled(
        !m_splintInProgress && hasUpperForSplint && hasFinalMandibleForSplint);
    if (m_splintExportAct) m_splintExportAct->setEnabled(
        !m_splintInProgress && (m_intermediateSplintMesh || m_finalSplintMesh));
    updateSplintControls();

    if (!hasVolume) {
        setGuidedNext(m_openAction, tr("Empiece abriendo una carpeta DICOM."));
    } else {
        setGuidedDone(m_openAction);
        if (!hasSegmentation && m_autoSegmentationAct) {
            setGuidedNext(m_autoSegmentationAct,
                          tr("Estudio cargado. Ejecute segmentacion automatica para crear las mascaras."));
        } else if (hasSegmentation && (!hasMaxilla || !hasMandible) && m_splitAct) {
            setGuidedNext(m_splitAct,
                          tr("Mascara lista. Divida maxilar y mandibula para continuar."));
        } else if (hasMaxilla && hasMandible) {
            setGuidedDone(m_splitAct);
        }
    }

    // ORIENTACION buttons are managed sequentially by setOrientationWorkspace() and
    // updateFrankfurtPointStatus(); only sync the overall composite-availability guard here.
    const bool hasCompositeForOrientation = (m_upperCompositeMesh || m_lowerCompositeMesh);
    if (!hasCompositeForOrientation) {
        if (m_frankfortPorionDAct)  m_frankfortPorionDAct->setEnabled(false);
        if (m_frankfortPorionIAct)  m_frankfortPorionIAct->setEnabled(false);
        if (m_frankfortOrbitalDAct) m_frankfortOrbitalDAct->setEnabled(false);
        if (m_frankfortOrbitalIAct) m_frankfortOrbitalIAct->setEnabled(false);
        if (m_alignFrankfurtAct)    m_alignFrankfurtAct->setEnabled(false);
        if (m_midlineGizmoAct)      m_midlineGizmoAct->setEnabled(false);
        if (m_midlineAcceptGizmoAct) m_midlineAcceptGizmoAct->setEnabled(false);
        if (m_saveOrientationAct)   m_saveOrientationAct->setEnabled(false);
        if (m_exportOrientedAct)    m_exportOrientedAct->setEnabled(false);
    }
    updateModelWorkflowUi();
    updateOrthognathicSteps();
}

void MainWindow::updateModelWorkflowActions()
{
    refreshRegisteredArchContours();
    const int step = m_modelStepStack ? m_modelStepStack->currentIndex() : 0;
    const bool finalComposite = m_upperCompositeMesh && m_lowerCompositeMesh;
    const bool upperStep = !finalComposite && step == 0;
    const bool lowerStep = !finalComposite && step == 1;
    const bool hasMaxilla =
        m_mesh3DView &&
        (m_mesh3DView->meshData(objectActorKey(5)) || m_mesh3DView->meshData(5));
    const bool hasMandible =
        m_mesh3DView &&
        (m_mesh3DView->meshData(objectActorKey(6)) || m_mesh3DView->meshData(6));
    const int upperPairs = std::min(m_maxillaBonePoints.size(), m_upperArchPoints.size());
    const int lowerPairs = std::min(m_mandibleBonePoints.size(), m_lowerArchPoints.size());
    const bool anyPoints =
        !m_maxillaBonePoints.isEmpty() || !m_upperArchPoints.isEmpty() ||
        !m_mandibleBonePoints.isEmpty() || !m_lowerArchPoints.isEmpty();

    auto showOnly = [](QAction* action, bool visible, bool enabled) {
        if (!action) return;
        action->setVisible(visible);
        action->setEnabled(visible && enabled);
    };
    auto check = [](QAction* action, bool checked) {
        if (!action || !action->isCheckable()) return;
        const QSignalBlocker blocker(action);
        action->setChecked(checked);
    };

    check(m_maxPtsAct,   m_dentalPointSet == DentalPointSet::MaxillaBone);
    check(m_upperPtsAct, m_dentalPointSet == DentalPointSet::UpperArch);
    check(m_mandPtsAct,  m_dentalPointSet == DentalPointSet::MandibleBone);
    check(m_lowerPtsAct, m_dentalPointSet == DentalPointSet::LowerArch);

    const QVector<QAction*> modelActions = {
        m_modelBackAct, m_importUpperAct, m_importLowerAct,
        m_maxPtsAct, m_upperPtsAct, m_matchUpperAct,
        m_mandPtsAct, m_lowerPtsAct, m_matchLowerAct, m_matchBothAct,
        m_adjustArchAct, m_acceptGizmoAct, m_compositeAct, m_continueNoMatchAct,
        m_exportAct, m_exportPackAct, m_clearPtsAct, m_resetArchAct
    };
    clearGuidedActionStates(modelActions);

    showOnly(m_importUpperAct, false, false);
    showOnly(m_importLowerAct, false, false);
    showOnly(m_maxPtsAct, false, false);
    showOnly(m_upperPtsAct, false, false);
    showOnly(m_matchUpperAct, false, false);
    showOnly(m_mandPtsAct, false, false);
    showOnly(m_lowerPtsAct, false, false);
    showOnly(m_matchLowerAct, false, false);
    showOnly(m_matchBothAct, false, false);
    showOnly(m_adjustArchAct, false, false);
    showOnly(m_acceptGizmoAct, false, false);
    showOnly(m_compositeAct, false, false);
    showOnly(m_continueNoMatchAct, false, false);
    showOnly(m_exportAct, false, false);
    showOnly(m_exportPackAct, false, false);
    showOnly(m_clearPtsAct, false, false);
    showOnly(m_resetArchAct, false, false);

    if (m_compositeStage != CompositeStage::None) {
        // The block / review panel drives the workflow until the composite is accepted; Atrás stays available.
        showOnly(m_modelBackAct, true, !m_compositeInProgress);
        if (m_compositeButton) m_compositeButton->setVisible(false);
        return;
    }

    if (finalComposite) {
        showOnly(m_modelBackAct, true, true);
        showOnly(m_continueNoMatchAct, false, false);
        showOnly(m_exportAct, true, true);
        showOnly(m_exportPackAct, true, m_upperRegistrationCalculated || m_lowerRegistrationCalculated);
        showOnly(m_resetArchAct, true, m_upperArchMesh || m_lowerArchMesh);
        setGuidedDone(m_compositeAct);
        setGuidedNext(m_exportAct, tr("Modelos compuestos listos. Exporte STL o continue con el siguiente modulo."));
        if (m_compositeButton) {
            m_compositeButton->setVisible(true);
            m_compositeButton->setText(tr("Exportar STL compuesto"));
            m_compositeButton->setEnabled(true);
        }
        return;
    }

    if (upperStep) {
        const bool upperLoaded = m_upperArchMesh != nullptr;
        const bool canUpperPoints = hasMaxilla && upperLoaded;
        const bool canUpperMatch =
            canUpperPoints && upperPairs >= 3 && m_maxillaBonePoints.size() == m_upperArchPoints.size();
        const bool canUpperComposite = m_upperRegistrationCalculated && upperLoaded && hasMaxilla;

        showOnly(m_modelBackAct, upperLoaded || anyPoints || m_upperRegistrationCalculated,
                 upperLoaded || anyPoints || m_upperRegistrationCalculated);
        showOnly(m_importUpperAct, !upperLoaded, hasMaxilla);
        showOnly(m_maxPtsAct, upperLoaded && !m_upperCompositeMesh, canUpperPoints);
        showOnly(m_upperPtsAct, upperLoaded && !m_upperCompositeMesh, canUpperPoints);
        showOnly(m_matchUpperAct, upperLoaded && !m_upperCompositeMesh, canUpperMatch);
        showOnly(m_adjustArchAct, m_upperRegistrationCalculated && !m_upperCompositeMesh, m_upperArchMesh != nullptr);
        showOnly(m_acceptGizmoAct, m_dentalGizmoActive, m_dentalGizmoActive);
        showOnly(m_compositeAct, upperLoaded && !m_upperCompositeMesh, canUpperComposite);
        showOnly(m_continueNoMatchAct, true, (hasMaxilla || hasMandible) && !m_compositeInProgress);
        showOnly(m_clearPtsAct, anyPoints, anyPoints);
        showOnly(m_resetArchAct, upperLoaded, upperLoaded);
        if (upperLoaded) setGuidedDone(m_importUpperAct);
        if (!m_maxillaBonePoints.isEmpty()) setGuidedDone(m_maxPtsAct);
        if (!m_upperArchPoints.isEmpty()) setGuidedDone(m_upperPtsAct);
        if (m_upperRegistrationCalculated) setGuidedDone(m_matchUpperAct);
        if (m_upperCompositeMesh) setGuidedDone(m_compositeAct);

        if (!upperLoaded) {
            setGuidedNext(m_importUpperAct,
                          tr("Modelos: importe el STL superior o use Continuar sin match si no tiene escaneo."));
        } else if (upperPairs < 3) {
            if (m_maxillaBonePoints.size() <= m_upperArchPoints.size())
                setGuidedNext(m_maxPtsAct, tr("Marque el siguiente punto homologo en el maxilar del TAC."));
            else
                setGuidedNext(m_upperPtsAct, tr("Marque el punto correspondiente en el STL superior."));
        } else if (!m_upperRegistrationCalculated) {
            setGuidedNext(m_matchUpperAct, tr("Calcule el registro superior y ajuste el gizmo si es necesario."));
        } else if (m_dentalGizmoActive) {
            setGuidedNext(m_acceptGizmoAct, tr("Acepte el ajuste manual del STL superior."));
        } else if (!m_upperCompositeMesh) {
            setGuidedNext(m_compositeAct, tr("Cree el modelo compuesto maxilar."));
        }
        if (m_compositeButton) {
            m_compositeButton->setVisible(upperLoaded && !m_upperCompositeMesh);
            m_compositeButton->setText(tr("Crear compuesto maxilar"));
            m_compositeButton->setEnabled(canUpperComposite && !m_compositeInProgress);
        }
        return;
    }

    if (lowerStep) {
        const bool lowerLoaded = m_lowerArchMesh != nullptr;
        const bool canLowerPoints = hasMandible && lowerLoaded;
        const bool canLowerMatch =
            canLowerPoints && lowerPairs >= 3 && m_mandibleBonePoints.size() == m_lowerArchPoints.size();
        const bool canLowerComposite = m_lowerRegistrationCalculated && lowerLoaded && hasMandible;

        showOnly(m_modelBackAct, true, true);
        showOnly(m_importLowerAct, !lowerLoaded, hasMandible);
        showOnly(m_mandPtsAct, lowerLoaded && !m_lowerCompositeMesh, canLowerPoints);
        showOnly(m_lowerPtsAct, lowerLoaded && !m_lowerCompositeMesh, canLowerPoints);
        showOnly(m_matchLowerAct, lowerLoaded && !m_lowerCompositeMesh, canLowerMatch);
        showOnly(m_adjustArchAct, m_lowerRegistrationCalculated && !m_lowerCompositeMesh, m_lowerArchMesh != nullptr);
        showOnly(m_acceptGizmoAct, m_dentalGizmoActive, m_dentalGizmoActive);
        showOnly(m_compositeAct, lowerLoaded && !m_lowerCompositeMesh, canLowerComposite);
        showOnly(m_continueNoMatchAct, true, (hasMaxilla || hasMandible) && !m_compositeInProgress);
        showOnly(m_clearPtsAct, anyPoints, anyPoints);
        showOnly(m_resetArchAct, lowerLoaded || m_upperArchMesh, lowerLoaded || m_upperArchMesh);
        if (lowerLoaded) setGuidedDone(m_importLowerAct);
        if (!m_mandibleBonePoints.isEmpty()) setGuidedDone(m_mandPtsAct);
        if (!m_lowerArchPoints.isEmpty()) setGuidedDone(m_lowerPtsAct);
        if (m_lowerRegistrationCalculated) setGuidedDone(m_matchLowerAct);
        if (m_lowerCompositeMesh) setGuidedDone(m_compositeAct);

        if (!lowerLoaded) {
            setGuidedNext(m_importLowerAct,
                          tr("Modelos: importe el STL inferior o use Continuar sin match si no tiene escaneo."));
        } else if (lowerPairs < 3) {
            if (m_mandibleBonePoints.size() <= m_lowerArchPoints.size())
                setGuidedNext(m_mandPtsAct, tr("Marque el siguiente punto homologo en la mandibula del TAC."));
            else
                setGuidedNext(m_lowerPtsAct, tr("Marque el punto correspondiente en el STL inferior."));
        } else if (!m_lowerRegistrationCalculated) {
            setGuidedNext(m_matchLowerAct, tr("Calcule el registro inferior y ajuste el gizmo si es necesario."));
        } else if (m_dentalGizmoActive) {
            setGuidedNext(m_acceptGizmoAct, tr("Acepte el ajuste manual del STL inferior."));
        } else if (!m_lowerCompositeMesh) {
            setGuidedNext(m_compositeAct, tr("Cree el modelo compuesto mandibular."));
        }
        if (m_compositeButton) {
            m_compositeButton->setVisible(lowerLoaded && !m_lowerCompositeMesh);
            m_compositeButton->setText(tr("Crear compuesto mandibular"));
            m_compositeButton->setEnabled(canLowerComposite && !m_compositeInProgress);
        }
    }
}

void MainWindow::goBackModelWorkflow()
{
    if (m_compositeInProgress) {
        statusBar()->showMessage(tr("Espere a que termine el calculo del modelo compuesto."));
        return;
    }

    if (m_dentalPointSet != DentalPointSet::None) {
        setDentalPointCapture(DentalPointSet::None);
        statusBar()->showMessage(tr("Captura de puntos cancelada. Puede escoger el paso anterior."));
        return;
    }

    if (m_compositeStage == CompositeStage::Review) {
        backToCompositeBlockStage();
        updateButtonStates();
        return;
    }
    if (m_compositeStage == CompositeStage::Block) {
        cancelCompositeStage();
        updateButtonStates();
        statusBar()->showMessage(tr("Volvió al registro: ajuste el escaneo o vuelva a crear el compuesto."));
        return;
    }

    if (m_dentalGizmoActive) {
        commitActiveDentalGizmos(false);
        statusBar()->showMessage(tr("Ajuste manual aceptado antes de volver."));
        return;
    }

    // Undo the last completed step of the current jaw, so it can be done again.
    const int step = m_modelStepStack ? m_modelStepStack->currentIndex() : 0;
    const bool upper = step == 0;
    const QString jaw = upper ? tr("superior") : tr("inferior");
    const int archLabel = upper ? kUpperArchLabel : kLowerArchLabel;
    const int compositeLabel = upper ? kUpperCompositeLabel : kLowerCompositeLabel;
    auto& composite = upper ? m_upperCompositeMesh : m_lowerCompositeMesh;
    auto& arch = upper ? m_upperArchMesh : m_lowerArchMesh;
    auto& original = upper ? m_upperArchOriginalMesh : m_lowerArchOriginalMesh;
    bool& registered = upper ? m_upperRegistrationCalculated : m_lowerRegistrationCalculated;
    auto& bonePoints = upper ? m_maxillaBonePoints : m_mandibleBonePoints;
    auto& archPoints = upper ? m_upperArchPoints : m_lowerArchPoints;
    const auto confirm = [this](const QString& text) {
        return QMessageBox::question(this, tr("Atrás"), text, QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            == QMessageBox::Yes;
    };
    const auto removeFromViews = [this](int label) {
        removeObjectEntry(label);
        for (Mesh3DView* view : {m_mesh3DView, m_modelMaxillaView, m_modelUpperArchView, m_modelMandibleView,
                                 m_modelLowerArchView, m_modelMatchView, m_orientationView, m_osteotomyView,
                                 m_repositionView, m_splintUpperView, m_splintLowerView, m_splintView})
            if (view) view->removeMesh(objectActorKey(label));
    };
    const auto finish = [this](const QString& message) {
        rebuildDentalPointMarkers();
        syncModelViews();
        updateButtonStates();
        statusBar()->showMessage(message);
    };

    if (composite) {
        if (!confirm(tr("¿Descartar el compuesto %1 para crearlo de nuevo? El contorno marcado se conserva.").arg(jaw)))
            return;
        composite = nullptr;
        removeFromViews(compositeLabel);
        if (upper) m_appState.setUpperCompositeReady(false);
        else m_appState.setLowerCompositeReady(false);
        if (m_modelStepStack) m_modelStepStack->setVisible(true);
        if (m_modelMatchView) m_modelMatchView->setTitle(tr("MATCH PREVIEW"));
        if (m_compositeButton) {
            disconnect(m_compositeButton, &QPushButton::clicked, this, &MainWindow::exportDentalCompositeStl);
            connect(m_compositeButton, &QPushButton::clicked, this, &MainWindow::createDentalCompositeModels,
                    Qt::UniqueConnection);
        }
        finish(tr("Compuesto %1 descartado: vuelva a crearlo.").arg(jaw));
        return;
    }
    if (registered) {
        if (!confirm(tr("¿Deshacer el registro del STL %1? Volverá a marcar los puntos homólogos.").arg(jaw)))
            return;
        if (original) {
            arch = vtkSmartPointer<vtkPolyData>::New();
            arch->DeepCopy(original);
            const int key = objectActorKey(archLabel);
            for (Mesh3DView* view : {m_mesh3DView, upper ? m_modelUpperArchView : m_modelLowerArchView}) {
                if (!view) continue;
                view->addMesh(key, arch, meshLabelName(archLabel));
                view->setMeshColor(key, objectColorForLabel(archLabel));
            }
        }
        registered = false;
        if (upper) {
            m_upperArchRegistrationMatrix = identityMatrix();
            m_upperRegistrationReport.clear();
            m_upperRegResult = {};
            m_appState.setUpperRegistered(false);
        } else {
            m_lowerArchRegistrationMatrix = identityMatrix();
            m_lowerRegistrationReport.clear();
            m_lowerRegResult = {};
            m_appState.setLowerRegistered(false);
        }
        bonePoints.clear();
        archPoints.clear();
        finish(tr("Registro %1 deshecho: marque de nuevo los puntos homólogos.").arg(jaw));
        return;
    }
    if (!bonePoints.isEmpty() || !archPoints.isEmpty()) {
        bonePoints.clear();
        archPoints.clear();
        finish(tr("Puntos %1 borrados: márquelos de nuevo.").arg(jaw));
        return;
    }
    if (arch) {
        if (!confirm(tr("¿Quitar el STL %1 para cargarlo de nuevo?").arg(jaw)))
            return;
        arch = nullptr;
        original = nullptr;
        removeFromViews(archLabel);
        if (upper) m_appState.setUpperArchImported(false);
        else m_appState.setLowerArchImported(false);
        finish(tr("STL %1 quitado: cárguelo de nuevo.").arg(jaw));
        return;
    }
    if (!upper && m_modelStepStack) {
        m_modelStepStack->setCurrentIndex(0);
        finish(tr("Volvió a la arcada superior."));
        return;
    }
    statusBar()->showMessage(tr("Ya está en el primer paso."));
}

// ─────────────────────────────────────────────────────────────────────────────
// exportClinicalLog  (Phase 4)
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::continueToOrientationWithoutMatch()
{
    if (m_compositeInProgress) {
        statusBar()->showMessage(tr("Espere a que termine el calculo actual antes de continuar."));
        return;
    }

    if (m_dentalPointSet != DentalPointSet::None)
        setDentalPointCapture(DentalPointSet::None);
    if (m_dentalGizmoActive)
        commitActiveDentalGizmos(false);

    if (!createBoneOnlyComposites()) {
        QMessageBox::warning(this, tr("Continuar sin match"),
                             tr("No hay maxilar ni mandibula calculados para orientar.\n"
                                "Primero calcule las mallas desde Segmentacion."));
        return;
    }
    publishCompositeMeshesToSceneViews();
    showFinalCompositeView(true);
    updateButtonStates();
    statusBar()->showMessage(
        tr("Orientacion iniciada sin match STL. Use los puntos de Frankfort para continuar."));
}

void MainWindow::exportClinicalLog()
{
    const QString defaultPath =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
        + QStringLiteral("/log_clinico_maxillo.txt");
    const QString path = QFileDialog::getSaveFileName(
        this,
        tr("Exportar Log Clínico"),
        defaultPath,
        tr("Archivo de texto (*.txt)"));
    if (path.isEmpty()) return;

    QString err;
    if (!LoggerCore::instance().saveToFile(path, &err))
        QMessageBox::warning(this, tr("Log Clínico"), err);
    else
        statusBar()->showMessage(tr("Log clínico exportado: ") + path);
}

// ─────────────────────────────────────────────────────────────────────────────
// Async callbacks (always called on the main thread via queued connection)
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::onLoadStatusChanged(const QString& message)
{
    // Keep progress bar text + status bar in sync
    if (m_progressBar && m_progressBar->isVisible())
        m_progressBar->setFormat(message + QStringLiteral("  %p%"));
    statusBar()->showMessage(message);
}

void MainWindow::onLoadProgressChanged(int percent)
{
    if (m_progressBar) m_progressBar->setValue(percent);
}

void MainWindow::onSeriesFound(const QVector<SeriesInfo>& series)
{
    if (series.size() <= 1) return;   // single series: worker continues automatically

    // Multiple series: let the user pick
    SeriesSelectionDialog dlg(series, this);
    if (dlg.exec() != QDialog::Accepted) {
        setLoadingUiEnabled(false);
        statusBar()->showMessage(tr("Load cancelled."));
        return;
    }
    // Forward selected series to the worker thread
    emit requestLoadSeries(dlg.selectedSeries(), m_cacheDir);
}

void MainWindow::onPreviewReady(vtkSmartPointer<vtkImageData> preview,
                                 DicomVolumeLoader::VolumeMetadata meta)
{
    m_volume      = preview;
    m_volumeMeta  = meta;
    m_previewActive = true;

    distributeVolume();
    broadcastPreset(m_currentPreset);
    refreshInfoPanel();

    // Annotate title bars to indicate preview mode
    statusBar()->showMessage(
        tr("Preview (every 4th slice)  —  full resolution loading in background…"));
}

void MainWindow::onVolumeReady(vtkSmartPointer<vtkImageData> volume,
                                DicomVolumeLoader::VolumeMetadata meta)
{
    m_volume        = volume;
    m_volumeMeta    = meta;
    m_previewActive = false;

    distributeVolume();
    broadcastPreset(m_currentPreset);
    refreshInfoPanel();
    setLoadingUiEnabled(false);

    // ── Phase 4: log DICOM load ───────────────────────────────────────────
    LoggerCore::instance().logDicomLoad(
        m_dicomFolder, meta.numSlices,
        meta.spacing[0], meta.spacing[1], meta.spacing[2]);

    // ── Phase 1: volume loaded — update button states ─────────────────────
    m_appState.setVolumeLoaded(true);
    updateButtonStates();

    // ── Phase 3: validate DICOM geometry ─────────────────────────────────
    {
        const ValidationResult vr = GeometryValidation::validateDicom(m_volume);
        if (vr.isError()) {
            QMessageBox::warning(this, tr("Validación DICOM"), vr.message);
            LoggerCore::instance().logValidation(
                QStringLiteral("DICOM"), vr.message, false);
        } else if (vr.isWarn()) {
            statusBar()->showMessage(tr("Advertencia DICOM: ") + vr.message);
            LoggerCore::instance().logValidation(
                QStringLiteral("DICOM"), vr.message, true);
        }
    }

    // If a project was being opened, apply its state now that the volume is ready
    if (m_pendingProjectState) {
        applyProjectState(*m_pendingProjectState);
        m_pendingProjectState.reset();
        statusBar()->showMessage(tr("Proyecto cargado: ") + m_projectFilePath);
        return;
    }

    statusBar()->showMessage(
        QString("Loaded  |  %1 slices  |  Spacing: %2 × %3 × %4 mm")
            .arg(meta.numSlices)
            .arg(meta.spacing[0], 0, 'f', 3)
            .arg(meta.spacing[1], 0, 'f', 3)
            .arg(meta.spacing[2], 0, 'f', 3));
}

void MainWindow::onLoadError(const QString& message)
{
    setLoadingUiEnabled(false);
    QMessageBox::critical(this, tr("DICOM Load Error"), message);
    statusBar()->showMessage(tr("Load failed — see error dialog."));
}

// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::distributeVolume()
{
    m_axialView->setVolume(m_volume);
    auto* sharedCursor = m_axialView->resliceCursor();
    m_coronalView->setVolume(m_volume);
    m_sagittalView->setVolume(m_volume);
    m_coronalView->setSharedResliceCursor(sharedCursor);
    m_sagittalView->setSharedResliceCursor(sharedCursor);

    syncResliceViews(nullptr);
    QTimer::singleShot(0, this, [this] {
        if (m_axialView)    m_axialView->resetCamera();
        if (m_coronalView)  m_coronalView->resetCamera();
        if (m_sagittalView) m_sagittalView->resetCamera();
    });
    refreshSegmentationOverlays();
}

// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::broadcastPreset(const WindowLevelPreset& preset)
{
    m_axialView->applyWindowLevel(preset);
    m_coronalView->applyWindowLevel(preset);
    m_sagittalView->applyWindowLevel(preset);
    statusBar()->showMessage(
        QString("Window: %1 | Level: %2  (%3)")
            .arg(preset.window)
            .arg(preset.level)
            .arg(preset.name));
}

// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::refreshInfoPanel()
{
    const auto& m = m_volumeMeta;

    auto toQ = [](const std::string& s) {
        return s.empty() ? QString("N/A") : QString::fromStdString(s);
    };

    m_lblPatientName->setText(toQ(m.patientName).replace('^', ' '));
    m_lblPatientId->setText(toQ(m.patientId));
    m_lblStudyDate->setText(toQ(m.studyDate));

    m_lblDimensions->setText(
        QString("%1 × %2 × %3")
            .arg(m.dimensions[0])
            .arg(m.dimensions[1])
            .arg(m.dimensions[2]));

    m_lblSpacing->setText(
        QString("%1 × %2 × %3")
            .arg(m.spacing[0], 0, 'f', 3)
            .arg(m.spacing[1], 0, 'f', 3)
            .arg(m.spacing[2], 0, 'f', 3));

    m_lblHuRange->setText(
        QString("[%1, %2]")
            .arg(static_cast<int>(m.scalarRange[0]))
            .arg(static_cast<int>(m.scalarRange[1])));

    m_lblNumSlices->setText(QString::number(m.numSlices));
}

// ─────────────────────────────────────────────────────────────────────────────
QColor MainWindow::maskColorForLabel(int label) const
{
    if (m_maskTable) {
        for (int row = 0; row < m_maskTable->rowCount(); ++row) {
            auto* item = m_maskTable->item(row, 0);
            if (item && item->data(Qt::UserRole).toInt() == label)
                return item->background().color();
        }
    }
    return meshLabelColor(label);
}

QColor MainWindow::objectColorForLabel(int label) const
{
    if (m_objectTable) {
        for (int row = 0; row < m_objectTable->rowCount(); ++row) {
            auto* item = m_objectTable->item(row, 0);
            if (item && item->data(Qt::UserRole + 1).toInt() == label)
                return item->background().color();
        }
    }
    if (label == kUpperCompositeLabel)
        return objectColorForLabel(5);
    if (label == kLowerCompositeLabel)
        return objectColorForLabel(6);
    if (label == 5 || label == 6)
        return maskColorForLabel(label);
    return meshLabelColor(label);
}

void MainWindow::applyMaskColorToAllViews(int label, const QColor& color)
{
    if (label <= 0) return;
    for (Mesh3DView* view : {m_mesh3DView, m_modelMaxillaView, m_modelUpperArchView,
                             m_modelMandibleView, m_modelLowerArchView,
                             m_modelMatchView, m_orientationView, m_osteotomyView,
                             m_repositionView, m_splintUpperView, m_splintLowerView,
                             m_splintView}) {
        if (!view) continue;
        view->setMeshColor(label, color);
        view->setMeshColor(objectActorKey(label), color);
    }
}

void MainWindow::applyObjectColorToAllViews(int label, const QColor& color)
{
    if (label <= 0) return;

    QVector<int> labels{label};
    const auto appendUnique = [&labels](int v) {
        if (v > 0 && !labels.contains(v)) labels.push_back(v);
    };

    // ORIENTACION renders the composite meshes. If the user recolors the base
    // bone or its registered STL, keep the corresponding composite in sync so
    // the visible object changes immediately and survives view rebuilds.
    if (label == 5 || label == kUpperArchLabel)
        appendUnique(kUpperCompositeLabel);
    else if (label == 6 || label == kLowerArchLabel)
        appendUnique(kLowerCompositeLabel);

    if (m_objectTable) {
        const QSignalBlocker blocker(m_objectTable);
        for (int row = 0; row < m_objectTable->rowCount(); ++row) {
            auto* item = m_objectTable->item(row, 0);
            if (!item) continue;
            const int itemLabel = item->data(Qt::UserRole + 1).toInt();
            if (itemLabel != label && !labels.contains(itemLabel)) continue;
            item->setBackground(QBrush(color));
        }
    }

    for (Mesh3DView* view : {m_mesh3DView, m_modelMaxillaView, m_modelUpperArchView,
                             m_modelMandibleView, m_modelLowerArchView,
                             m_modelMatchView, m_orientationView, m_osteotomyView,
                             m_repositionView, m_splintUpperView, m_splintLowerView,
                             m_splintView}) {
        if (!view) continue;
        for (int l : labels) {
            view->setMeshColor(objectActorKey(l), color);
            view->setMeshColor(l, color);
        }
    }
}

void MainWindow::syncResliceViews(MPRView*)
{
    if (!m_volume) return;

    m_axialView->syncFromCursor();
    m_coronalView->syncFromCursor();
    m_sagittalView->syncFromCursor();
    refreshMeasurementOverlays();

    m_axialView->render();
    m_coronalView->render();
    m_sagittalView->render();
}

void MainWindow::showMprPresetContextMenu(MPRView*, const QPoint& globalPos)
{
    if (!m_volume)
        return;

    QMenu menu(this);
    menu.setStyleSheet(
        "QMenu { background:#2c2c2e; color:#f5f5f7; border:1px solid #3a3a3c; border-radius:12px; padding:6px; }"
        "QMenu::item { padding:7px 26px 7px 18px; border-radius:8px; }"
        "QMenu::item:selected { background:#0a84ff; color:#ffffff; }");

    auto* boneAct = menu.addAction(tr("Hueso"));
    auto* softAct = menu.addAction(tr("Tejidos blandos"));
    auto* brainAct = menu.addAction(tr("Cerebro"));

    QAction* selected = menu.exec(globalPos);
    if (!selected)
        return;

    if (selected == boneAct) {
        onBoneWindow();
    } else if (selected == softAct) {
        onSoftTissueWindow();
    } else if (selected == brainAct) {
        onBrainWindow();
    }
}

QWidget* MainWindow::viewAtCursor() const
{
    const QPoint globalPos = QCursor::pos();
    QWidget* leaf = QApplication::widgetAt(globalPos);

    const std::array<QWidget*, 19> views = {
        static_cast<QWidget*>(m_axialView),
        static_cast<QWidget*>(m_coronalView),
        static_cast<QWidget*>(m_sagittalView),
        static_cast<QWidget*>(m_mesh3DView),
        static_cast<QWidget*>(m_modelMaxillaView),
        static_cast<QWidget*>(m_modelUpperArchView),
        static_cast<QWidget*>(m_modelMandibleView),
        static_cast<QWidget*>(m_modelLowerArchView),
        static_cast<QWidget*>(m_modelMatchView),
        static_cast<QWidget*>(m_orientationView),
        static_cast<QWidget*>(m_osteotomyView),
        static_cast<QWidget*>(m_biteSegmentView),
        static_cast<QWidget*>(m_biteScanView),
        static_cast<QWidget*>(m_biteRegistrationView),
        static_cast<QWidget*>(m_repositionView),
        static_cast<QWidget*>(m_splintUpperView),
        static_cast<QWidget*>(m_splintLowerView),
        static_cast<QWidget*>(m_splintView),
        static_cast<QWidget*>(m_guideView)
    };

    for (QWidget* view : views) {
        if (!view || !view->isVisible()) continue;
        if (leaf && (leaf == view || view->isAncestorOf(leaf))) return view;

        const QRect globalRect(view->mapToGlobal(QPoint(0, 0)), view->size());
        if (globalRect.contains(globalPos)) return view;
    }

    return nullptr;
}

bool MainWindow::toggleViewUnderCursor()
{
    if (m_viewFullScreen) {
        exitViewFullScreen();
        return true;
    }

    QWidget* view = viewAtCursor();
    if (!view) return false;

    toggleViewFullScreen(view);
    return true;
}

void MainWindow::toggleViewFullScreen(QWidget* view)
{
    if (!view) return;

    if (m_viewFullScreen) {
        exitViewFullScreen();
        return;
    }

    m_viewFullScreen = true;
    m_fullScreenView = view;
    m_windowStateBeforeFullScreen = windowState();
    if (m_mprSplitter) {
        m_splitterSizesBeforeFullScreen = m_mprSplitter->sizes();
    }

    const std::array<QWidget*, 18> views = {
        static_cast<QWidget*>(m_axialView),
        static_cast<QWidget*>(m_coronalView),
        static_cast<QWidget*>(m_sagittalView),
        static_cast<QWidget*>(m_mesh3DView),
        static_cast<QWidget*>(m_modelMaxillaView),
        static_cast<QWidget*>(m_modelUpperArchView),
        static_cast<QWidget*>(m_modelMandibleView),
        static_cast<QWidget*>(m_modelLowerArchView),
        static_cast<QWidget*>(m_modelMatchView),
        static_cast<QWidget*>(m_orientationView),
        static_cast<QWidget*>(m_osteotomyView),
        static_cast<QWidget*>(m_biteSegmentView),
        static_cast<QWidget*>(m_biteScanView),
        static_cast<QWidget*>(m_biteRegistrationView),
        static_cast<QWidget*>(m_repositionView),
        static_cast<QWidget*>(m_splintUpperView),
        static_cast<QWidget*>(m_splintLowerView),
        static_cast<QWidget*>(m_splintView)
    };

    for (QWidget* candidate : views) {
        if (!candidate) continue;
        const bool active = candidate == view;
        candidate->setVisible(active);
        if (auto* mpr = qobject_cast<MPRView*>(candidate)) {
            mpr->setFullScreenActive(active);
        } else if (auto* mesh = qobject_cast<Mesh3DView*>(candidate)) {
            mesh->setFullScreenActive(active);
        }
    }

    if ((view == m_axialView || view == m_coronalView ||
         view == m_sagittalView || view == m_mesh3DView) &&
        m_axialView && m_axialView->parentWidget()) {
        if (auto* grid = qobject_cast<QGridLayout*>(m_axialView->parentWidget()->layout())) {
            grid->addWidget(view, 0, 0, 2, 2);
            grid->setRowStretch(0, 1);
            grid->setRowStretch(1, 1);
            grid->setColumnStretch(0, 1);
            grid->setColumnStretch(1, 1);
            grid->invalidate();
        }
    }

    // MODELOS workspace: each pair lives in its own page (1 row, 2 cols)
    const bool isModelView = (view == m_modelMaxillaView   || view == m_modelUpperArchView ||
                              view == m_modelMandibleView  || view == m_modelLowerArchView);
    if (isModelView && view->parentWidget()) {
        if (auto* grid = qobject_cast<QGridLayout*>(view->parentWidget()->layout())) {
            grid->addWidget(view, 0, 0, 1, 2);
            grid->setColumnStretch(0, 1);
            grid->setColumnStretch(1, 1);
            grid->invalidate();
        }
    }

    if (view == m_modelMatchView && m_modelStepStack) {
        m_modelStepStack->hide();
    }

    const bool isBiteTopView = (view == m_biteSegmentView || view == m_biteScanView);
    if (isBiteTopView && view->parentWidget()) {
        if (auto* grid = qobject_cast<QGridLayout*>(view->parentWidget()->layout())) {
            grid->addWidget(view, 0, 0, 1, 2);
            grid->setColumnStretch(0, 1);
            grid->setColumnStretch(1, 1);
            grid->invalidate();
        }
    }
    if (view == m_biteRegistrationView && m_biteSegmentView && m_biteSegmentView->parentWidget()) {
        m_biteSegmentView->parentWidget()->hide();
    }

    if (m_infoPanel) m_infoPanel->hide();
    if (m_leftMenuPanel) m_leftMenuPanel->hide();
    for (QToolBar* toolbar : findChildren<QToolBar*>()) {
        toolbar->show();
    }
    menuBar()->hide();
    statusBar()->hide();

    showFullScreen();
    if (auto* mpr = qobject_cast<MPRView*>(view)) {
        mpr->resetCamera();
    } else if (auto* mesh = qobject_cast<Mesh3DView*>(view)) {
        mesh->resetCamera();
        mesh->render();
    }
}

void MainWindow::exitViewFullScreen()
{
    if (!m_viewFullScreen) return;

    for (MPRView* view : {m_axialView, m_coronalView, m_sagittalView}) {
        if (!view) continue;
        view->show();
        view->setFullScreenActive(false);
    }
    if (m_mesh3DView) {
        m_mesh3DView->show();
        m_mesh3DView->setFullScreenActive(false);
    }
    for (Mesh3DView* view : {m_modelMaxillaView, m_modelUpperArchView,
                              m_modelMandibleView, m_modelLowerArchView,
                              m_modelMatchView}) {
        if (!view) continue;
        view->show();
        view->setFullScreenActive(false);
    }
    if (m_orientationView) {
        m_orientationView->show();
        m_orientationView->setFullScreenActive(false);
    }
    if (m_osteotomyView) {
        m_osteotomyView->show();
        m_osteotomyView->setFullScreenActive(false);
    }
    for (Mesh3DView* view : {m_biteSegmentView, m_biteScanView, m_biteRegistrationView}) {
        if (!view) continue;
        view->show();
        view->setFullScreenActive(false);
    }
    if (m_biteSegmentView && m_biteSegmentView->parentWidget()) {
        m_biteSegmentView->parentWidget()->show();
    }
    if (m_repositionView) {
        m_repositionView->show();
        m_repositionView->setFullScreenActive(false);
    }
    for (Mesh3DView* view : {m_splintUpperView, m_splintLowerView, m_splintView}) {
        if (!view) continue;
        view->show();
        view->setFullScreenActive(false);
    }

    if (m_axialView && m_axialView->parentWidget()) {
        if (auto* grid = qobject_cast<QGridLayout*>(m_axialView->parentWidget()->layout())) {
            grid->addWidget(m_axialView,    0, 0);
            grid->addWidget(m_coronalView,  0, 1);
            grid->addWidget(m_sagittalView, 1, 0);
            grid->addWidget(m_mesh3DView,   1, 1);
            grid->setRowStretch(0, 1);
            grid->setRowStretch(1, 1);
            grid->setColumnStretch(0, 1);
            grid->setColumnStretch(1, 1);
            grid->invalidate();
        }
    }
    // Reset both pair pages (each has its own 1-row grid)
    if (m_modelMaxillaView && m_modelMaxillaView->parentWidget()) {
        if (auto* grid = qobject_cast<QGridLayout*>(m_modelMaxillaView->parentWidget()->layout())) {
            grid->addWidget(m_modelMaxillaView, 0, 0);
            grid->addWidget(m_modelUpperArchView, 0, 1);
            grid->setColumnStretch(0, 1);
            grid->setColumnStretch(1, 1);
            grid->invalidate();
        }
    }
    if (m_modelMandibleView && m_modelMandibleView->parentWidget()) {
        if (auto* grid = qobject_cast<QGridLayout*>(m_modelMandibleView->parentWidget()->layout())) {
            grid->addWidget(m_modelMandibleView, 0, 0);
            grid->addWidget(m_modelLowerArchView, 0, 1);
            grid->setColumnStretch(0, 1);
            grid->setColumnStretch(1, 1);
            grid->invalidate();
        }
    }
    if (m_modelStepStack) {
        m_modelStepStack->setVisible(!(m_upperCompositeMesh && m_lowerCompositeMesh));
    }
    if (m_biteSegmentView && m_biteSegmentView->parentWidget()) {
        if (auto* grid = qobject_cast<QGridLayout*>(m_biteSegmentView->parentWidget()->layout())) {
            grid->addWidget(m_biteSegmentView, 0, 0);
            grid->addWidget(m_biteScanView, 0, 1);
            grid->setColumnStretch(0, 1);
            grid->setColumnStretch(1, 1);
            grid->invalidate();
        }
    }

    if (m_infoPanel) m_infoPanel->show();
    if (m_leftMenuPanel) m_leftMenuPanel->show();
    for (QToolBar* toolbar : findChildren<QToolBar*>()) {
        toolbar->show();
    }
    menuBar()->hide();
    statusBar()->show();

    showNormal();
    setWindowState(m_windowStateBeforeFullScreen & ~Qt::WindowFullScreen);
    if (m_mprSplitter && !m_splitterSizesBeforeFullScreen.isEmpty()) {
        m_mprSplitter->setSizes(m_splitterSizesBeforeFullScreen);
    }

    m_viewFullScreen = false;
    m_fullScreenView = nullptr;
    syncResliceViews(nullptr);
    if (m_mesh3DView) m_mesh3DView->render();
    for (Mesh3DView* v : {m_modelMaxillaView, m_modelUpperArchView,
                          m_modelMandibleView, m_modelLowerArchView,
                          m_modelMatchView})
        if (v) v->render();
    if (m_orientationView) m_orientationView->render();
    if (m_osteotomyView) m_osteotomyView->render();
    for (Mesh3DView* v : {m_biteSegmentView, m_biteScanView, m_biteRegistrationView})
        if (v) v->render();
    if (m_repositionView) m_repositionView->render();
    for (Mesh3DView* v : {m_splintUpperView, m_splintLowerView, m_splintView})
        if (v) v->render();
}

void MainWindow::deactivateLassoTools()
{
    cancelBoneCavityFill();
    if (m_lassoAddButton && m_lassoAddButton->isChecked()) {
        m_lassoAddButton->setChecked(false);
    }
    if (m_lassoSubButton && m_lassoSubButton->isChecked()) {
        m_lassoSubButton->setChecked(false);
    }

    if (m_axialView)    m_axialView->setLassoMode(false, true);
    if (m_coronalView)  m_coronalView->setLassoMode(false, true);
    if (m_sagittalView) m_sagittalView->setLassoMode(false, true);
    if (m_mesh3DView)   m_mesh3DView->setLassoEraseMode(false);
}

void MainWindow::setMeasurementTool(MeasurementToolMode mode)
{
    if (m_airwayPointCaptureStep > 0 && mode != MeasurementToolMode::Cursor) {
        m_airwayPointCaptureStep = 0;
        for (MPRView* view : {m_axialView, m_coronalView, m_sagittalView}) {
            if (view) view->setMeasurementPickingEnabled(false);
        }
        closeAirwayDialog();
    }
    deactivateLassoTools();
    m_dentalPointSet = DentalPointSet::None;
    if (m_mesh3DView) m_mesh3DView->setPointPickMode(false);

    m_measurementToolMode = mode;
    m_activeMeasurementTool.reset();

    switch (mode) {
        case MeasurementToolMode::Distance:
            m_activeMeasurementTool = std::make_unique<DistanceMeasurementTool>();
            break;
        case MeasurementToolMode::Angle:
            m_activeMeasurementTool = std::make_unique<AngleMeasurementTool>();
            break;
        case MeasurementToolMode::Annotation:
            m_activeMeasurementTool = std::make_unique<AnnotationTool>();
            break;
        case MeasurementToolMode::Area:
            m_activeMeasurementTool = std::make_unique<AreaMeasurementTool>();
            break;
        case MeasurementToolMode::ROICircle:
            m_activeMeasurementTool = std::make_unique<ROIStatisticsTool>(
                MeasurementROIShape::Circle);
            break;
        case MeasurementToolMode::ROIRectangle:
            m_activeMeasurementTool = std::make_unique<ROIStatisticsTool>(
                MeasurementROIShape::Rectangle);
            break;
        case MeasurementToolMode::PerpendicularDistance:
            m_activeMeasurementTool = std::make_unique<PerpendicularDistanceTool>();
            break;
        case MeasurementToolMode::Cursor:
            break;
    }

    if (m_activeMeasurementTool) {
        m_activeMeasurementTool->setImageData(m_volume);
    }

    const bool picking = m_activeMeasurementTool != nullptr;
    for (MPRView* view : {m_axialView, m_coronalView, m_sagittalView}) {
        if (view) {
            view->setMeasurementPickingEnabled(picking);
            view->setMeasurementFinishRequiresDoubleRightClick(
                mode == MeasurementToolMode::Area);
        }
    }

    statusBar()->showMessage(
        picking
            ? QString("Herramienta activa: %1").arg(m_activeMeasurementTool->name())
            : QString("Cursor normal"));
    refreshMeasurementOverlays();
}

void MainWindow::handleMeasurementPoint(MPRView* source, double x, double y, double z)
{
    if (source && m_fillBoneCavityLabel > 0) {
        fillBoneCavityAt({x, y, z});
        return;
    }
    if (m_airwayPointCaptureStep > 0) {
        if (m_airwayPointCaptureStep == 1) {
            m_airwayPoint1 = QVector3D(x, y, z);
            m_airwayPointCaptureStep = 2;
            statusBar()->showMessage(tr("Punto 1 (Nasofaringe) seleccionado. Ahora haga clic en el LÍMITE INFERIOR (Tráquea)..."));
            showAirwayDialog(2);
        } else if (m_airwayPointCaptureStep == 2) {
            m_airwayPoint2 = QVector3D(x, y, z);
            m_airwayPointCaptureStep = 0;
            for (MPRView* view : {m_axialView, m_coronalView, m_sagittalView}) {
                if (view) {
                    view->setMeasurementPickingEnabled(false);
                }
            }
            statusBar()->showMessage(tr("Límites de vía aérea definidos. Iniciando segmentación..."));
            closeAirwayDialog();
            if (m_aiSegmentationService) {
                m_aiSegmentationService->setAirwaySeeds({m_airwayPoint1.x(), m_airwayPoint1.y(), m_airwayPoint1.z()},
                                                        {m_airwayPoint2.x(), m_airwayPoint2.y(), m_airwayPoint2.z()});
                m_pendingAutoMeshLabel = 4; // Auto-generate mesh label 4 (via aerea)
                startAISegmentation(SegmentationTarget::Airway);
            }
        }
        return;
    }

    if (!source || !m_activeMeasurementTool) return;

    m_activeMeasurementTool->setImageData(m_volume);
    const std::array<double, 3> point = {x, y, z};
    auto measurement = m_activeMeasurementTool->addPoint(
        point,
        toMeasurementView(source->orientation()),
        source->currentSlice(),
        this);

    if (measurement.has_value()) {
        const QString id = m_measurementManager.addMeasurement(std::move(*measurement));
        statusBar()->showMessage(QString("Medicion creada: %1").arg(id));
        return;
    }

    refreshMeasurementOverlays();
    const QString closeHint =
        m_measurementToolMode == MeasurementToolMode::Area
            ? QString("doble click derecho para cerrar")
            : QString("click derecho para cerrar");
    statusBar()->showMessage(
        m_activeMeasurementTool->canFinish()
            ? QString("%1: punto %2  |  %3")
                  .arg(m_activeMeasurementTool->name())
                  .arg(m_activeMeasurementTool->pendingPointCount())
                  .arg(closeHint)
            : QString("%1: punto %2 / %3")
                  .arg(m_activeMeasurementTool->name())
                  .arg(m_activeMeasurementTool->pendingPointCount())
                  .arg(m_activeMeasurementTool->requiredPointCount()));
}

void MainWindow::finishMeasurementTool(MPRView*)
{
    if (m_fillBoneCavityLabel > 0) {
        cancelBoneCavityFill();
        return;
    }
    if (!m_activeMeasurementTool || !m_activeMeasurementTool->canFinish()) return;

    auto measurement = m_activeMeasurementTool->finish(this);
    if (!measurement.has_value()) return;

    const QString id = m_measurementManager.addMeasurement(std::move(*measurement));
    statusBar()->showMessage(QString("Medicion creada: %1").arg(id));
}

std::optional<Measurement> MainWindow::activeMeasurementPreview() const
{
    if (!m_activeMeasurementTool || m_activeMeasurementTool->pendingPointCount() == 0) {
        return std::nullopt;
    }

    Measurement preview;
    preview.viewOrientation = m_activeMeasurementTool->pendingView();
    preview.sliceIndex = m_activeMeasurementTool->pendingSliceIndex();
    preview.pointsPhysical = m_activeMeasurementTool->pendingPoints();
    preview.visible = true;

    switch (m_measurementToolMode) {
        case MeasurementToolMode::Distance:
            preview.type = MeasurementType::Distance;
            break;
        case MeasurementToolMode::Angle:
            preview.type = MeasurementType::Angle;
            break;
        case MeasurementToolMode::Area:
            preview.type = MeasurementType::Area;
            break;
        case MeasurementToolMode::ROICircle:
            preview.type = MeasurementType::ROIStatistics;
            preview.roiShape = MeasurementROIShape::Circle;
            break;
        case MeasurementToolMode::ROIRectangle:
            preview.type = MeasurementType::ROIStatistics;
            preview.roiShape = MeasurementROIShape::Rectangle;
            break;
        case MeasurementToolMode::PerpendicularDistance:
            preview.type = MeasurementType::PerpendicularDistance;
            break;
        case MeasurementToolMode::Annotation:
        case MeasurementToolMode::Cursor:
            return std::nullopt;
    }

    return preview;
}

void MainWindow::refreshMeasurementTable()
{
    if (!m_measurementTable) return;

    const QSignalBlocker blocker(m_measurementTable);
    const auto& measurements = m_measurementManager.measurements();
    m_measurementTable->setRowCount(static_cast<int>(measurements.size()));

    for (int row = 0; row < static_cast<int>(measurements.size()); ++row) {
        const Measurement& m   = measurements[static_cast<size_t>(row)];
        const QString      id  = QString::fromStdString(m.id);

        // Col 0 = Tipo  (carries id in UserRole for deletion)
        // Col 1 = Vista, Col 2 = Valor, Col 3 = Texto
        auto setItem = [&](int col, const QString& text) {
            auto* item = new QTableWidgetItem(text);
            item->setData(Qt::UserRole, id);   // id on every cell so deleteSelected works on any col
            m_measurementTable->setItem(row, col, item);
        };
        setItem(0, typeLabel(m.type));
        setItem(1, viewLabel(m.viewOrientation));
        setItem(2, valueLabel(m));
        setItem(3, QString::fromStdString(m.text));
    }
}

void MainWindow::refreshMeasurementOverlays()
{
    if (!m_axialView || !m_coronalView || !m_sagittalView) return;

    const auto& measurements = m_measurementManager.measurements();
    const bool visible = m_measurementManager.allVisible();
    const auto preview = activeMeasurementPreview();
    m_axialView->setMeasurements(measurements, visible);
    m_coronalView->setMeasurements(measurements, visible);
    m_sagittalView->setMeasurements(measurements, visible);
    m_axialView->setMeasurementPreview(preview);
    m_coronalView->setMeasurementPreview(preview);
    m_sagittalView->setMeasurementPreview(preview);
}

void MainWindow::deleteSelectedMeasurement()
{
    if (!m_measurementTable) return;
    const int row = m_measurementTable->currentRow();
    if (row < 0) return;

    QTableWidgetItem* item = m_measurementTable->item(row, 0);
    if (!item) return;
    m_measurementManager.removeMeasurement(item->data(Qt::UserRole).toString());
}

void MainWindow::saveMeasurements()
{
    const QString filePath = QFileDialog::getSaveFileName(
        this,
        tr("Guardar mediciones"),
        QString(),
        tr("JSON (*.json)"));
    if (filePath.isEmpty()) return;

    const auto& meta = m_volumeMeta;
    QString error;
    const bool ok = MeasurementSerialization::save(
        filePath,
        QString::fromStdString(meta.studyInstanceUid),
        QString::fromStdString(meta.seriesInstanceUid),
        m_measurementManager.measurements(),
        &error);

    if (!ok) {
        QMessageBox::critical(this, tr("Error"), error);
        return;
    }
    statusBar()->showMessage(QString("Mediciones guardadas: %1").arg(filePath));
}

void MainWindow::loadMeasurements()
{
    const QString filePath = QFileDialog::getOpenFileName(
        this,
        tr("Cargar mediciones"),
        QString(),
        tr("JSON (*.json)"));
    if (filePath.isEmpty()) return;

    MeasurementDocument document;
    QString error;
    if (!MeasurementSerialization::load(filePath, document, &error)) {
        QMessageBox::critical(this, tr("Error"), error);
        return;
    }

    const auto& meta = m_volumeMeta;
    const QString currentStudy = QString::fromStdString(meta.studyInstanceUid);
    const QString currentSeries = QString::fromStdString(meta.seriesInstanceUid);
    if (!currentStudy.isEmpty() && !document.studyInstanceUid.isEmpty() &&
        currentStudy != document.studyInstanceUid) {
        QMessageBox::warning(
            this,
            tr("Serie diferente"),
            tr("El JSON pertenece a otro StudyInstanceUID."));
    } else if (!currentSeries.isEmpty() && !document.seriesInstanceUid.isEmpty() &&
               currentSeries != document.seriesInstanceUid) {
        QMessageBox::warning(
            this,
            tr("Serie diferente"),
            tr("El JSON pertenece a otro SeriesInstanceUID."));
    }

    m_measurementManager.replaceMeasurements(std::move(document.measurements));
    statusBar()->showMessage(QString("Mediciones cargadas: %1").arg(filePath));
}

void MainWindow::showSegmentationBackendInfo()
{
    QMessageBox::information(
        this,
        tr("Backend de segmentacion"),
        tr("La segmentacion ya no depende de Slicer.exe.\n\n"
           "Dental IA usa DentalSegmentator standalone con nnU-Net/PyTorch y los pesos "
           "oficiales instalados en este equipo (sin conexion a internet).\n\n"
           "Python debe tener SimpleITK, numpy, torch y nnunetv2. Puede definir "
           "DENTALSEGMENTATOR_PYTHON para seleccionar el Python de ese entorno, "
           "DENTALSEGMENTATOR_MODEL_DIR para los pesos y DENTALSEGMENTATOR_DEVICE "
           "como cuda o cpu.\n\n"
           "Tejido blando sigue usando segmentacion local por HU porque DentalSegmentator "
           "no entrega esa estructura."));
}

void MainWindow::startAISegmentation(SegmentationTarget target)
{
    cancelBoneCavityFill();
    if (!m_volume || !m_aiSegmentationService) {
        QMessageBox::warning(this, tr("Segmentacion IA"), tr("Primero cargue un volumen DICOM."));
        return;
    }
    if (target != SegmentationTarget::Airway) {
        m_aiSegmentationService->clearAirwaySeed();
    }
    if (m_previewActive || m_loadInProgress) {
        QMessageBox::warning(
            this,
            tr("Segmentacion IA"),
            tr("Espere a que termine de cargar el TAC completo antes de segmentar."));
        return;
    }

    m_segmentationOutputDir =
        QDir(m_cacheDir).filePath("AISegmentation/" +
                                  QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));
    QDir().mkpath(m_segmentationOutputDir);

    if (m_progressBar) {
        m_progressBar->setVisible(true);
        m_progressBar->setValue(0);
        m_progressBar->setFormat(tr("Segmentacion IA  %p%"));
    }

    statusBar()->showMessage(tr("Segmentando el TAC cargado sin Slicer.exe..."));
    // Progress dialog: stage, percentage (or busy bar), elapsed time and Cancelar.
    closeSegmentationProgress();
    auto* dialog = new SegmentationProgressDialog(this);
    m_segmentationProgressDialog = dialog;
    connect(m_aiSegmentationService, &AISegmentationService::statusChanged, dialog, &SegmentationProgressDialog::setStage);
    connect(m_aiSegmentationService, &AISegmentationService::progressChanged, dialog, &SegmentationProgressDialog::setProgress);
    connect(dialog, &SegmentationProgressDialog::cancelRequested, this, [this] {
        if (m_aiSegmentationService)
            m_aiSegmentationService->cancel();
    });
    dialog->show();
    m_aiSegmentationService->segment(m_volume, m_segmentationOutputDir, target);
}

// ─────────────────────────────────────────────────────────────────────────────
// mergeIntoLabelmap
//
// Fuses `src` into `dst` without destroying existing labels:
//   - If dst is null  → dst = src (first segmentation).
//   - If dimensions match → merge non-zero voxels in src.
//   - Soft tissue (label 2) only fills empty voxels so it does not erase
//     DentalSegmentator labels already present in the same labelmap.
//   - If dimensions differ → dst = src (incompatible volumes; warn in status).
//
// Returns true if a merge (not a simple replace) was performed.
// ─────────────────────────────────────────────────────────────────────────────
static bool mergeIntoLabelmap(vtkSmartPointer<vtkImageData>& dst,
                               vtkSmartPointer<vtkImageData>  src)
{
    if (!dst) { dst = src; return false; }

    int dDims[3], sDims[3];
    dst->GetDimensions(dDims);
    src->GetDimensions(sDims);

    if (dDims[0] != sDims[0] || dDims[1] != sDims[1] || dDims[2] != sDims[2]) {
        // Incompatible grids — just replace
        dst = src;
        return false;
    }

    const vtkIdType nPts = src->GetNumberOfPoints();
    for (vtkIdType i = 0; i < nPts; ++i) {
        const double v = src->GetPointData()->GetScalars()->GetTuple1(i);
        if (v == 0.0) continue;

        const int incomingLabel = static_cast<int>(std::lround(v));
        const double existing = dst->GetPointData()->GetScalars()->GetTuple1(i);

        // Soft tissue is a broad envelope. Preserve already segmented
        // anatomical labels and only add soft tissue where the labelmap is empty.
        if (incomingLabel == 2 && existing != 0.0) continue;

        dst->GetPointData()->GetScalars()->SetTuple1(i, v);
    }
    dst->Modified();
    return true;
}

void MainWindow::closeSegmentationProgress()
{
    if (!m_segmentationProgressDialog)
        return;
    m_segmentationProgressDialog->hide();
    m_segmentationProgressDialog->deleteLater();
    m_segmentationProgressDialog = nullptr;
}

void MainWindow::onSegmentationCancelled()
{
    closeSegmentationProgress();
    if (m_progressBar) m_progressBar->setVisible(false);
    if (m_aiSegmentationService)
        m_aiSegmentationService->clearAirwaySeed();
    statusBar()->showMessage(tr("Segmentación cancelada. La segmentación actual no cambió."));
}

void MainWindow::onSegmentationFinished(const QString& outputSegmentationPath)
{
    closeSegmentationProgress();
    QString error;
    auto newLabelmap = SegmentationImporter::importLabelmap(outputSegmentationPath, &error);
    if (!newLabelmap) {
        onSegmentationError(error);
        return;
    }

    // ── Determine which labels the new segmentation introduces ────────────
    // Do this BEFORE merging so we know the incoming set precisely.
    const std::vector<int> newLabels = presentSegmentationLabels(newLabelmap);

    // ── Merge into the existing labelmap (keeps other labels intact) ──────
    const bool merged = mergeIntoLabelmap(m_segmentationLabelmap, newLabelmap);
    Q_UNUSED(merged)

    refreshSegmentationOverlays();
    if (m_progressBar) m_progressBar->setVisible(false);
    statusBar()->showMessage(tr("Segmentacion importada."));

    // Phase 1 + Phase 4
    m_appState.setSegmentationDone(true);
    updateButtonStates();
    LoggerCore::instance().logSegmentation(
        QStringLiteral("bone/tissue"),
        outputSegmentationPath,
        true);

    // ── Decide which labels to (re)generate meshes for ────────────────────
    const int pendingLabel = m_pendingAutoMeshLabel;
    m_pendingAutoMeshLabel = -1;

    std::vector<int> labelsToGenerate;
    if (pendingLabel <= -2) {
        // "Dental IA" or "Segmentación Automática": generate all labels found in the new segmentation
        labelsToGenerate = newLabels;
    } else if (pendingLabel > 0) {
        labelsToGenerate.push_back(pendingLabel);
    }

    // NOTE: We intentionally do NOT clear the mask table or 3D view here.
    // addMaskEntry() already does insert-or-update, and addMesh() replaces
    // the actor for an existing label. Running a second segmentation (e.g.
    // "Tejido") therefore adds its labels alongside the existing ones.

    for (int currentLabel : labelsToGenerate) {
        QString meshError;
        const int smoothingIterations = (pendingLabel == -2)
            ? smoothingIterationsForPreset(MeshSmoothingPreset::Optimal)
            : smoothingIterationsForPreset(MeshSmoothingPreset::Moderate);
        m_maskSmoothingIterations[currentLabel] = smoothingIterations;
        auto mesh = MeshGenerator::generateMesh(
            m_segmentationLabelmap, currentLabel, true, smoothingIterations, &meshError);
        if (!mesh) {
            statusBar()->showMessage(
                tr("Malla %1: %2").arg(meshLabelName(currentLabel), meshError));
        } else {
            addMaskEntry(meshLabelName(currentLabel), meshLabelColor(currentLabel),
                         currentLabel);
            publishSegmentationMesh(currentLabel, mesh);
            statusBar()->showMessage(
                tr("Malla 3D generada: %1").arg(meshLabelName(currentLabel)));
        }
    }

    if (m_aiSegmentationService) {
        m_aiSegmentationService->clearAirwaySeed();
    }
    syncModelViews();
    updateButtonStates();
}

void MainWindow::onSegmentationError(const QString& error)
{
    closeSegmentationProgress();
    if (m_progressBar) m_progressBar->setVisible(false);
    if (m_aiSegmentationService) {
        m_aiSegmentationService->clearAirwaySeed();
    }
    QMessageBox::critical(this, tr("Segmentacion IA"), error);
    statusBar()->showMessage(tr("Segmentacion IA fallida."));
}

void MainWindow::refreshSegmentationOverlays()
{
    const bool visible = m_toggleSegmentationOverlayAct
        ? m_toggleSegmentationOverlayAct->isChecked()
        : true;

    for (MPRView* view : {m_axialView, m_coronalView, m_sagittalView}) {
        if (!view) continue;
        view->setSegmentationLabelmap(m_segmentationLabelmap);
        view->setSegmentationHiddenLabels(m_hiddenMaskLabels);
        view->setSegmentationOpacity(0.45);
        view->setSegmentationVisible(visible && m_segmentationLabelmap != nullptr);
        // Without a render the slice keeps showing the previous overlay until the user interacts.
        if (m_volume) view->render();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// addMaskEntry — inserts or updates a row in the dynamic Máscaras table.
//   Col 0  color swatch  (click → QColorDialog)  stores label in UserRole
//   Col 1  name          (double-click to edit)
//   Col 2  visible       (checkbox)
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::addMaskEntry(const QString& name, const QColor& color, int label)
{
    if (!m_maskTable) return;

    // If the label already exists, update it instead of adding a duplicate
    for (int row = 0; row < m_maskTable->rowCount(); ++row) {
        auto* item = m_maskTable->item(row, 0);
        if (item && item->data(Qt::UserRole).toInt() == label) {
            const QColor finalColor = item->background().color().isValid()
                ? item->background().color()
                : color;
            item->setBackground(QBrush(finalColor));
            if (auto* nm = m_maskTable->item(row, 1)) nm->setText(name);
            if (auto* vis = m_maskTable->item(row, 2)) {
                vis->setCheckState(m_hiddenMaskLabels.count(label) ? Qt::Unchecked : Qt::Checked);
            }
            applyMaskColorToAllViews(label, finalColor);
            return;
        }
    }

    const int row = m_maskTable->rowCount();
    m_maskTable->insertRow(row);
    m_maskTable->setRowHeight(row, 24);

    // Col 0 — color swatch (not directly editable; handled via itemClicked)
    auto* colorItem = new QTableWidgetItem;
    colorItem->setBackground(QBrush(color));
    colorItem->setData(Qt::UserRole, label);
    colorItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    m_maskTable->setItem(row, 0, colorItem);

    // Col 1 — name (editable on double-click)
    auto* nameItem = new QTableWidgetItem(name);
    nameItem->setData(Qt::UserRole, label);
    nameItem->setFlags(Qt::ItemIsEditable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    m_maskTable->setItem(row, 1, nameItem);

    // Col 2 — visible checkbox
    auto* visItem = new QTableWidgetItem;
    visItem->setCheckState(m_hiddenMaskLabels.count(label) ? Qt::Unchecked : Qt::Checked);
    visItem->setData(Qt::UserRole, label);
    visItem->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    m_maskTable->setItem(row, 2, visItem);
    applyMaskColorToAllViews(label, color);
}

void MainWindow::setMaskVisible(int label, bool visible)
{
    if (label <= 0) return;
    if (visible) {
        m_hiddenMaskLabels.erase(label);
    } else {
        m_hiddenMaskLabels.insert(label);
    }

    if (m_maskTable) {
        for (int row = 0; row < m_maskTable->rowCount(); ++row) {
            auto* idItem = m_maskTable->item(row, 0);
            if (!idItem || idItem->data(Qt::UserRole).toInt() != label) continue;
            if (auto* visItem = m_maskTable->item(row, 2)) {
                const QSignalBlocker blocker(m_maskTable);
                visItem->setCheckState(visible ? Qt::Checked : Qt::Unchecked);
            }
            break;
        }
    }

    for (Mesh3DView* view : {m_mesh3DView, m_modelMaxillaView, m_modelUpperArchView,
                             m_modelMandibleView, m_modelLowerArchView,
                             m_modelMatchView, m_orientationView, m_osteotomyView,
                             m_repositionView, m_splintUpperView, m_splintLowerView,
                             m_splintView}) {
        if (view) view->setMeshVisible(label, visible);
    }
    syncVisibilityPanelToAllViews();
    refreshSegmentationOverlays();
}

int MainWindow::selectedMaskLabel() const
{
    if (!m_maskTable || m_maskTable->rowCount() == 0) return -1;
    const int row = m_maskTable->currentRow();
    if (row < 0) return -1;

    auto* item = m_maskTable->item(row, 0);
    if (!item) item = m_maskTable->item(row, 1);
    return item ? item->data(Qt::UserRole).toInt() : -1;
}

bool MainWindow::hasDerivedBonePlanning() const
{
    if (m_upperCompositeMesh || m_lowerCompositeMesh ||
        m_leFortSegmentMesh || m_bssoDistalMesh || m_genioBodyMesh) return true;
    for (int label : {kUpperCompositeLabel, kLowerCompositeLabel, kLeFortSegLabel,
                      kBssoDistalLabel, kGenioBodyLabel, kGenioSegmentLabel})
        if (objectEntryExists(label)) return true;
    return false;
}

void MainWindow::cancelBoneCavityFill()
{
    const bool wasActive = m_fillBoneCavityLabel > 0;
    m_fillBoneCavityLabel = -1;
    if (m_fillBoneCavityAct) {
        const QSignalBlocker blocker(m_fillBoneCavityAct);
        m_fillBoneCavityAct->setChecked(false);
    }
    if (wasActive) {
        for (MPRView* view : {m_axialView, m_coronalView, m_sagittalView})
            if (view) view->setMeasurementPickingEnabled(false);
        statusBar()->showMessage(tr("Selección de cavidad finalizada."));
    }
}

void MainWindow::beginBoneCavityFill()
{
    int label = selectedMaskLabel();
    if (m_objectTable && m_objectTable->selectionModel()) {
        const auto rows = m_objectTable->selectionModel()->selectedRows();
        if (!rows.isEmpty()) {
            auto* item = m_objectTable->item(rows.front().row(), 0);
            if (item) label = item->data(Qt::UserRole + 1).toInt();
        }
    }
    cancelBoneCavityFill();
    auto* segmentationService = qobject_cast<StandaloneDentalSegmentatorService*>(m_aiSegmentationService);
    if (!m_volume || !m_segmentationLabelmap || m_loadInProgress ||
        (segmentationService && segmentationService->isRunning()) ||
        (m_boneSplitter && m_boneSplitter->isRunning())) {
        statusBar()->showMessage(tr("Cargue el TAC y espere a que termine la segmentación."));
        return;
    }
    if (hasDerivedBonePlanning()) {
        QMessageBox::information(this, tr("Rellenar cavidad"),
            tr("Esta planeación ya contiene compuestos u osteotomías. El relleno debe realizarse "
               "en la segmentación inicial, antes de crear esos modelos. No se modificó la planeación actual."));
        return;
    }
    if (label != 1 && label != 5 && label != 6) {
        statusBar()->showMessage(tr("Seleccione la máscara Hueso, Maxilar o Mandíbula antes de rellenar."));
        return;
    }
    m_airwayPointCaptureStep = 0;
    closeAirwayDialog();
    setMeasurementTool(MeasurementToolMode::Cursor);
    m_fillBoneCavityLabel = label;
    {
        const QSignalBlocker blocker(m_fillBoneCavityAct);
        m_fillBoneCavityAct->setChecked(true);
    }
    for (MPRView* view : {m_axialView, m_coronalView, m_sagittalView})
        if (view) view->setMeasurementPickingEnabled(true);
    statusBar()->showMessage(tr("%1: seleccione el interior de la cavidad en un corte 2D.").arg(meshLabelName(label)));
}

void MainWindow::fillBoneCavityAt(const std::array<double, 3>& point)
{
    const int label = m_fillBoneCavityLabel;
    cancelBoneCavityFill();
    if (label < 0 || !m_segmentationLabelmap || hasDerivedBonePlanning()) return;
    auto original = m_segmentationLabelmap;
    const auto originalTime = original->GetMTime();
    auto input = vtkSmartPointer<vtkImageData>::New();
    input->DeepCopy(original);
    struct Candidate {
        BoneCavityFillResult fill;
        vtkSmartPointer<vtkPolyData> mesh;
    };
    const int iterations = m_maskSmoothingIterations.value(label, 20);
    QProgressDialog progress(tr("Analizando cavidad…"), QString(), 0, 0, this);
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setCancelButton(nullptr);
    progress.setMinimumDuration(0);
    QFutureWatcher<Candidate> watcher;
    connect(&watcher, &QFutureWatcher<Candidate>::finished, &progress, &QDialog::accept);
    watcher.setFuture(QtConcurrent::run([input, label, point, iterations] {
        Candidate candidate;
        try {
            candidate.fill = fillEnclosedBoneCavity(input, label, point);
            if (candidate.fill.labelmap)
                candidate.mesh = MeshGenerator::generateMesh(candidate.fill.labelmap, label, true,
                                                              iterations, &candidate.fill.error);
        } catch (const std::exception& e) {
            candidate.fill.error = QString::fromUtf8(e.what());
        }
        return candidate;
    }));
    if (!watcher.isFinished()) progress.exec();
    watcher.waitForFinished();
    if (progress.wasCanceled()) {
        statusBar()->showMessage(tr("Análisis cancelado. La máscara no cambió."));
        return;
    }
    const auto candidate = watcher.result();
    if (!candidate.fill.labelmap || !candidate.mesh) {
        QMessageBox::information(this, tr("Rellenar cavidad"), candidate.fill.error);
        return;
    }

    QDialog preview(this);
    preview.setObjectName("BoneCavityPreview");
    preview.setStyleSheet(
        "QDialog#BoneCavityPreview { background:#1c1c1e; }"
        "QDialog#BoneCavityPreview QLabel, QDialog#BoneCavityPreview QCheckBox { color:#f5f5f7; }");
    preview.setWindowTitle(tr("Previsualización del relleno"));
    preview.resize(std::min(1100, width() - 60), std::min(760, height() - 60));
    auto* layout = new QVBoxLayout(&preview);
    auto* warning = new QLabel(tr("Relleno geométrico: %1 mm³. No representa una nueva detección de tejido. "
                                  "Revise que no incluya cavidades anatómicas que deban conservarse.")
        .arg(QLocale(QLocale::Spanish, QLocale::Colombia).toString(candidate.fill.addedVolumeMm3, 'f', 1)), &preview);
    warning->setWordWrap(true);
    layout->addWidget(warning);
    auto* compare = new QCheckBox(tr("Mostrar relleno propuesto"), &preview);
    compare->setObjectName("BoneCavityCompare");
    compare->setChecked(true);
    layout->addWidget(compare);
    auto* grid = new QGridLayout;
    grid->setRowStretch(0, 1);
    grid->setRowStretch(1, 1);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    layout->addLayout(grid, 1);
    auto cursor = vtkSmartPointer<vtkResliceCursor>::New();
    cursor->SetImage(m_volume);
    cursor->SetCenter(point[0], point[1], point[2]);
    QVector<MPRView*> views;
    for (auto orientation : {MPROrientation::Axial, MPROrientation::Coronal, MPROrientation::Sagittal}) {
        auto* view = new MPRView(orientation, &preview);
        if (auto* renderWidget = view->findChild<QVTKOpenGLNativeWidget*>())
            renderWidget->setMinimumSize(160, 100);
        view->setMinimumSize(180, 140);
        view->setVolume(m_volume);
        view->setSharedResliceCursor(cursor);
        view->setSegmentationLabelmap(candidate.fill.labelmap);
        std::set<int> hidden {1, 2, 3, 4, 5, 6, 7, 8};
        hidden.erase(label);
        view->setSegmentationHiddenLabels(hidden);
        view->setSegmentationVisible(true);
        view->setSegmentationOpacity(0.55);
        grid->addWidget(view, views.size() / 2, views.size() % 2);
        views.append(view);
    }
    for (auto* view : views)
        connect(view, &MPRView::reslicePositionChanged, &preview, [views](MPRView* source) {
            for (auto* other : views) if (other != source) other->syncFromCursor();
        });
    auto* meshView = new Mesh3DView(&preview);
    if (auto* renderWidget = meshView->findChild<QVTKOpenGLNativeWidget*>())
        renderWidget->setMinimumSize(160, 100);
    meshView->setMinimumSize(180, 140);
    meshView->addMesh(label, candidate.mesh, meshLabelName(label));
    meshView->setMeshColor(label, maskColorForLabel(label));
    meshView->setStandardView(0);
    grid->addWidget(meshView, 1, 1);
    const auto originalMesh = MeshGenerator::generateMesh(original, label, true, iterations, nullptr);
    connect(compare, &QCheckBox::toggled, &preview, [=](bool filled) {
        for (auto* view : views) {
            view->setSegmentationLabelmap(filled ? candidate.fill.labelmap : original);
            view->render();
        }
        meshView->addMesh(label, filled ? candidate.mesh : originalMesh, meshLabelName(label));
        meshView->setMeshColor(label, maskColorForLabel(label));
        meshView->render();
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, &preview);
    buttons->button(QDialogButtonBox::Apply)->setText(tr("Aplicar relleno"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("Cancelar"));
    buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, &preview, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &preview, &QDialog::reject);
    layout->addWidget(buttons);
    QTimer::singleShot(0, &preview, [views, meshView, cursor, point] {
        cursor->SetCenter(point[0], point[1], point[2]);
        for (auto* view : views) {
            view->syncFromCursor();
            view->render();
        }
        meshView->render();
    });
    if (preview.exec() != QDialog::Accepted) {
        statusBar()->showMessage(tr("Relleno cancelado. La máscara no cambió."));
        return;
    }
    if (m_segmentationLabelmap != original || original->GetMTime() != originalTime || hasDerivedBonePlanning()) {
        QMessageBox::warning(this, tr("Rellenar cavidad"), tr("La segmentación cambió durante la revisión. Vuelva a seleccionar la cavidad."));
        return;
    }
    pushLabelmapUndo();
    m_segmentationLabelmap = candidate.fill.labelmap;
    refreshSegmentationOverlays();
    refreshEditedSegmentationMesh(label);
    if (candidate.fill.replacedSoftTissue) refreshEditedSegmentationMesh(2);
    m_appState.setProjectDirty(true);
    statusBar()->showMessage(tr("Cavidad rellenada en %1 (%2 vóxeles). Ctrl+Z permite deshacer.")
        .arg(meshLabelName(label)).arg(candidate.fill.addedVoxels));
}

int MainWindow::smoothingIterationsForPreset(MeshSmoothingPreset smoothing) const
{
    switch (smoothing) {
        case MeshSmoothingPreset::Light:    return 10;
        case MeshSmoothingPreset::Moderate: return 30;
        case MeshSmoothingPreset::Optimal:  return 70;
    }
    return 20;
}

QString MainWindow::smoothingNameForPreset(MeshSmoothingPreset smoothing) const
{
    switch (smoothing) {
        case MeshSmoothingPreset::Light:    return tr("Leve");
        case MeshSmoothingPreset::Moderate: return tr("Moderado");
        case MeshSmoothingPreset::Optimal:  return tr("Optimo");
    }
    return tr("Moderado");
}

bool MainWindow::chooseMeshSmoothingPreset(MeshSmoothingPreset* smoothing) const
{
    if (!smoothing) return false;

    QMessageBox box(const_cast<MainWindow*>(this));
    box.setWindowTitle(tr("Calcular objeto 3D"));
    box.setText(tr("Seleccione el nivel de suavizado del objeto."));
    box.setIcon(QMessageBox::Question);
    auto* lightButton = box.addButton(tr("Leve"), QMessageBox::AcceptRole);
    auto* moderateButton = box.addButton(tr("Moderado"), QMessageBox::AcceptRole);
    auto* optimalButton = box.addButton(tr("Optimo"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();

    if (box.clickedButton() == lightButton) {
        *smoothing = MeshSmoothingPreset::Light;
        return true;
    }
    if (box.clickedButton() == moderateButton) {
        *smoothing = MeshSmoothingPreset::Moderate;
        return true;
    }
    if (box.clickedButton() == optimalButton) {
        *smoothing = MeshSmoothingPreset::Optimal;
        return true;
    }
    return false;
}

void MainWindow::updateObjectAppearanceControls()
{
    if (!m_objectOpacitySlider || !m_objectOnTopCheck) return;
    const auto* item = m_objectTable && m_objectTable->selectionModel()->hasSelection()
        ? m_objectTable->item(m_objectTable->currentRow(), 0) : nullptr;
    const QSignalBlocker opacityBlocker(m_objectOpacitySlider);
    const QSignalBlocker topBlocker(m_objectOnTopCheck);
    const double opacity = item && item->data(kObjectOpacityRole).isValid()
        ? item->data(kObjectOpacityRole).toDouble() : 1.0;
    const int percent = qRound(opacity * 100.0);
    m_objectOpacitySlider->setEnabled(item != nullptr);
    m_objectOpacitySlider->setValue(percent);
    m_objectOpacityValue->setText(QString("%1 %").arg(percent));
    m_objectOpacityValue->setEnabled(item != nullptr);
    m_objectOnTopCheck->setEnabled(item != nullptr);
    m_objectOnTopCheck->setChecked(item && item->data(kObjectOnTopRole).toBool());
}

void MainWindow::setObjectDisplayOptions(int label, double opacity, bool alwaysOnTop)
{
    if (!m_objectTable || !std::isfinite(opacity)) return;
    opacity = std::clamp(opacity, 0.0, 1.0);
    for (int row = 0; row < m_objectTable->rowCount(); ++row) {
        auto* item = m_objectTable->item(row, 0);
        if (!item || item->data(Qt::UserRole + 1).toInt() != label) continue;
        const QSignalBlocker blocker(m_objectTable);
        item->setData(kObjectOpacityRole, opacity);
        item->setData(kObjectOnTopRole, alwaysOnTop);
        applyObjectDisplayOptionsToAllViews(label, opacity, alwaysOnTop);
        updateObjectAppearanceControls();
        return;
    }
}

void MainWindow::applyObjectDisplayOptionsToAllViews(int label, double opacity, bool alwaysOnTop)
{
    if (label <= 0) return;
    for (Mesh3DView* view : {m_mesh3DView, m_modelMaxillaView, m_modelUpperArchView,
                           m_modelMandibleView, m_modelLowerArchView, m_modelMatchView,
                           m_orientationView, m_osteotomyView, m_biteSegmentView,
                           m_biteScanView, m_biteRegistrationView, m_repositionView,
                           m_splintUpperView, m_splintLowerView, m_splintView}) {
        if (!view) continue;
        view->setMeshDisplayOptions(objectActorKey(label), opacity, alwaysOnTop);
        view->setMeshDisplayOptions(label, opacity, alwaysOnTop);
    }
}

bool MainWindow::objectEntryExists(int label) const
{
    if (!m_objectTable || label <= 0) return false;
    for (int row = 0; row < m_objectTable->rowCount(); ++row) {
        auto* item = m_objectTable->item(row, 0);
        if (item && item->data(Qt::UserRole + 1).toInt() == label)
            return true;
    }
    return false;
}

bool MainWindow::objectEntryVisible(int label) const
{
    if (!m_objectTable || label <= 0) return true;
    for (int row = 0; row < m_objectTable->rowCount(); ++row) {
        auto* item = m_objectTable->item(row, 0);
        if (!item || item->data(Qt::UserRole + 1).toInt() != label) continue;
        auto* vis = m_objectTable->item(row, 2);
        return !vis || vis->checkState() == Qt::Checked;
    }
    return true;
}

void MainWindow::syncVisibilityPanelToAllViews()
{
    ensureOsteotomyMeshesPresent();

    const QVector<Mesh3DView*> views {
        m_mesh3DView, m_modelMaxillaView, m_modelUpperArchView,
        m_modelMandibleView, m_modelLowerArchView, m_modelMatchView,
        m_orientationView, m_osteotomyView, m_biteSegmentView,
        m_biteScanView, m_biteRegistrationView, m_repositionView,
        m_splintUpperView, m_splintLowerView, m_splintView
    };

    std::map<int, bool> maskVisible;
    if (m_maskTable) {
        for (int row = 0; row < m_maskTable->rowCount(); ++row) {
            auto* idItem = m_maskTable->item(row, 0);
            auto* visItem = m_maskTable->item(row, 2);
            if (!idItem) continue;
            const int label = idItem->data(Qt::UserRole).toInt();
            if (label <= 0) continue;
            const bool visible = !visItem || visItem->checkState() == Qt::Checked;
            maskVisible[label] = visible;
            for (Mesh3DView* view : views) {
                if (view) view->setMeshVisible(label, visible);
            }
        }
    }

    if (m_objectTable) {
        for (int row = 0; row < m_objectTable->rowCount(); ++row) {
            auto* idItem = m_objectTable->item(row, 0);
            auto* visItem = m_objectTable->item(row, 2);
            if (!idItem) continue;
            const int actorKey = idItem->data(Qt::UserRole).toInt();
            const int label = idItem->data(Qt::UserRole + 1).toInt();
            const bool visible = !visItem || visItem->checkState() == Qt::Checked;
            for (Mesh3DView* view : views) {
                if (!view) continue;
                view->setMeshVisible(actorKey, visible);
                if (label > 0) {
                    // A mask surface is hidden only while its object is shown in that view;
                    // hiding the object shows the mask again instead of emptying the scene.
                    const auto mask = maskVisible.find(label);
                    view->setMeshVisible(label, mask != maskVisible.end()
                        ? mask->second && !(visible && view->meshData(actorKey))
                        : visible && !view->meshData(actorKey));
                }
                if (idItem->data(kObjectOpacityRole).isValid()) {
                    const double opacity = idItem->data(kObjectOpacityRole).toDouble();
                    const bool onTop = idItem->data(kObjectOnTopRole).toBool();
                    view->setMeshDisplayOptions(actorKey, opacity, onTop);
                    if (label > 0) view->setMeshDisplayOptions(label, opacity, onTop);
                }
            }
        }
    }
    // MODELOS views show the pair being registered (CT bone and scan, or its composite): the object and
    // mask lists must not hide them, or the scan would stand alone under the adjustment gizmo.
    for (Mesh3DView* view : {m_modelMatchView, m_modelMaxillaView, m_modelMandibleView}) {
        if (!view) continue;
        for (int bone : {5, 6}) {
            // One copy of the bone: its object when the view has one, otherwise the mask surface.
            const bool hasObject = view->meshData(objectActorKey(bone)) != nullptr;
            if (hasObject)
                view->setMeshVisible(objectActorKey(bone), true);
            if (view->meshData(bone))
                view->setMeshVisible(bone, !hasObject);
        }
        for (int key : {objectActorKey(kUpperArchLabel), objectActorKey(kLowerArchLabel),
                        objectActorKey(kUpperCompositeLabel), objectActorKey(kLowerCompositeLabel)})
            if (view->meshData(key))
                view->setMeshVisible(key, true);
    }
    syncRepositionSelectionVisibility();
}

void MainWindow::publishCompositeMeshesToSceneViews()
{
    auto publish = [this](Mesh3DView* view, int label, vtkPolyData* mesh) {
        if (!view || !mesh || mesh->GetNumberOfPoints() <= 0) return;
        const int actorKey = objectActorKey(label);
        view->addMesh(actorKey, mesh, meshLabelName(label));
        view->setMeshColor(actorKey, objectColorForLabel(label));
        view->setMeshOpacity(actorKey, 1.0);
        view->setMeshVisible(actorKey, objectEntryVisible(label));
    };

    for (Mesh3DView* view : {m_mesh3DView, m_modelMatchView, m_repositionView,
                             m_splintUpperView, m_splintLowerView, m_splintView}) {
        publish(view, kUpperCompositeLabel, m_upperCompositeMesh);
        publish(view, kLowerCompositeLabel, m_lowerCompositeMesh);
    }

    publish(m_modelMaxillaView, kUpperCompositeLabel, m_upperCompositeMesh);
    publish(m_modelMandibleView, kLowerCompositeLabel, m_lowerCompositeMesh);

    const bool osteotomyHasSegments =
        (m_leFortCranialMesh && m_leFortCranialMesh->GetNumberOfPoints() > 0) ||
        (m_leFortSegmentMesh && m_leFortSegmentMesh->GetNumberOfPoints() > 0) ||
        (m_bssoDistalMesh && m_bssoDistalMesh->GetNumberOfPoints() > 0) ||
        (m_bssoRightProximalMesh && m_bssoRightProximalMesh->GetNumberOfPoints() > 0) ||
        (m_bssoLeftProximalMesh && m_bssoLeftProximalMesh->GetNumberOfPoints() > 0) ||
        (m_genioBodyMesh && m_genioBodyMesh->GetNumberOfPoints() > 0) ||
        (m_genioSegmentMesh && m_genioSegmentMesh->GetNumberOfPoints() > 0);
    const bool osteotomyActive = m_viewModeStack && m_viewModeStack->currentIndex() == 3;
    if (osteotomyActive && !osteotomyHasSegments) {
        publish(m_osteotomyView, kUpperCompositeLabel, m_upperCompositeMesh);
        publish(m_osteotomyView, kLowerCompositeLabel, m_lowerCompositeMesh);
    }

    syncVisibilityPanelToAllViews();
}

void MainWindow::ensureOsteotomyMeshesPresent()
{
    if (!m_osteotomyView || !m_viewModeStack || m_viewModeStack->currentIndex() != 3)
        return;

    auto add = [this](int label, vtkPolyData* mesh, double opacity = 1.0) {
        if (!mesh || mesh->GetNumberOfPoints() <= 0) return;
        const int actorKey = (label == kBssoGuideLabel) ? label : objectActorKey(label);
        m_osteotomyView->addMesh(actorKey, mesh, meshLabelName(label));
        m_osteotomyView->setMeshColor(actorKey, objectColorForLabel(label));
        m_osteotomyView->setMeshOpacity(actorKey, opacity);
    };

    add(kUpperCompositeLabel, m_upperCompositeMesh);
    add(kLowerCompositeLabel, m_lowerCompositeMesh);
    add(kLeFortCranialLabel, m_leFortCranialMesh);
    add(kLeFortSegLabel, m_leFortSegmentMesh);
    const bool hasGenioSegments =
        m_genioBodyMesh && m_genioBodyMesh->GetNumberOfPoints() > 0 &&
        m_genioSegmentMesh && m_genioSegmentMesh->GetNumberOfPoints() > 0;
    if (hasGenioSegments) {
        add(kGenioBodyLabel, m_genioBodyMesh);
        add(kGenioSegmentLabel, m_genioSegmentMesh);
    } else {
        add(kBssoDistalLabel, m_bssoDistalMesh);
    }
    add(kBssoProximalRightLabel, m_bssoRightProximalMesh);
    add(kBssoProximalLeftLabel, m_bssoLeftProximalMesh);
    if (!m_bssoRightProximalMesh && !m_bssoLeftProximalMesh)
        add(kBssoProximalLabel, m_bssoProximalMesh);
    add(kBssoGuideLabel, m_bssoGuideVisualMesh, 0.35);
}

void MainWindow::addObjectEntry(const QString& name, const QColor& color, int label)
{
    if (!m_objectTable || label <= 0) return;
    const int actorKey = objectActorKey(label);

    for (int row = 0; row < m_objectTable->rowCount(); ++row) {
        auto* item = m_objectTable->item(row, 0);
        if (item && item->data(Qt::UserRole + 1).toInt() == label) {
            const QColor finalColor = item->background().color().isValid()
                ? item->background().color()
                : color;
            item->setBackground(QBrush(finalColor));
            if (auto* nm = m_objectTable->item(row, 1)) nm->setText(name);
            applyObjectColorToAllViews(label, finalColor);
            syncVisibilityPanelToAllViews();
            return;
        }
    }

    const int row = m_objectTable->rowCount();
    m_objectTable->insertRow(row);
    m_objectTable->setRowHeight(row, 24);

    auto* colorItem = new QTableWidgetItem;
    colorItem->setBackground(QBrush(color));
    colorItem->setData(Qt::UserRole, actorKey);
    colorItem->setData(Qt::UserRole + 1, label);
    colorItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    m_objectTable->setItem(row, 0, colorItem);

    auto* nameItem = new QTableWidgetItem(name);
    nameItem->setData(Qt::UserRole, actorKey);
    nameItem->setData(Qt::UserRole + 1, label);
    nameItem->setFlags(Qt::ItemIsEditable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    m_objectTable->setItem(row, 1, nameItem);

    auto* visItem = new QTableWidgetItem;
    visItem->setCheckState(Qt::Checked);
    visItem->setData(Qt::UserRole, actorKey);
    visItem->setData(Qt::UserRole + 1, label);
    visItem->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    m_objectTable->setItem(row, 2, visItem);
    applyObjectColorToAllViews(label, color);
    syncVisibilityPanelToAllViews();
}

void MainWindow::removeObjectEntry(int label)
{
    if (!m_objectTable) return;
    if (label == m_mandibleMovement.targetLabel) {
        m_mandibleMovement = {};
        m_mandibleMovementResetMatrix.clear();
        updateMandibleMovementSummary();
    }
    for (int row = m_objectTable->rowCount() - 1; row >= 0; --row) {
        auto* item = m_objectTable->item(row, 0);
        if (item && item->data(Qt::UserRole + 1).toInt() == label) {
            m_objectTable->removeRow(row);
            return;
        }
    }
}

void MainWindow::deleteSelectedMask()
{
    if (!m_maskTable || m_maskTable->rowCount() == 0) {
        statusBar()->showMessage(tr("No hay mascaras para eliminar."));
        return;
    }

    const int row = m_maskTable->currentRow();
    if (row < 0) {
        statusBar()->showMessage(tr("Selecciona una mascara en la tabla."));
        return;
    }

    auto* item = m_maskTable->item(row, 0);
    if (!item) item = m_maskTable->item(row, 1);
    const int label = item ? item->data(Qt::UserRole).toInt() : -1;
    if (label <= 0) {
        statusBar()->showMessage(tr("Mascara seleccionada invalida."));
        return;
    }

    bool changed = false;
    if (m_segmentationLabelmap) {
        pushLabelmapUndo();
        int extent[6] = {};
        m_segmentationLabelmap->GetExtent(extent);
        for (int z = extent[4]; z <= extent[5]; ++z) {
            for (int y = extent[2]; y <= extent[3]; ++y) {
                for (int x = extent[0]; x <= extent[1]; ++x) {
                    const int voxelLabel = static_cast<int>(std::lround(
                        m_segmentationLabelmap->GetScalarComponentAsDouble(x, y, z, 0)));
                    if (voxelLabel != label) continue;
                    m_segmentationLabelmap->SetScalarComponentFromDouble(x, y, z, 0, 0.0);
                    changed = true;
                }
            }
        }

        if (!changed && !m_labelmapUndoStack.isEmpty()) {
            m_labelmapUndoStack.pop();
        }
    }

    const int actorKey = objectActorKey(label);
    for (Mesh3DView* view : {m_mesh3DView, m_modelMaxillaView, m_modelUpperArchView,
                             m_modelMandibleView, m_modelLowerArchView,
                             m_modelMatchView, m_orientationView, m_osteotomyView,
                             m_repositionView, m_splintUpperView, m_splintLowerView,
                             m_splintView}) {
        if (!view) continue;
        view->removeMesh(label);
        view->removeMesh(actorKey);
    }
    removeObjectEntry(label);

    if (label == 5) {
        m_upperCompositeMesh = nullptr;
        m_appState.setUpperCompositeReady(false);
        removeObjectEntry(kUpperCompositeLabel);
        const int compositeKey = objectActorKey(kUpperCompositeLabel);
        for (Mesh3DView* view : {m_mesh3DView, m_modelMatchView, m_orientationView,
                                 m_osteotomyView, m_repositionView, m_splintUpperView,
                                 m_splintLowerView, m_splintView}) {
            if (view) view->removeMesh(compositeKey);
        }
    } else if (label == 6) {
        m_lowerCompositeMesh = nullptr;
        m_appState.setLowerCompositeReady(false);
        removeObjectEntry(kLowerCompositeLabel);
        const int compositeKey = objectActorKey(kLowerCompositeLabel);
        for (Mesh3DView* view : {m_mesh3DView, m_modelMatchView, m_orientationView,
                                 m_osteotomyView, m_repositionView, m_splintUpperView,
                                 m_splintLowerView, m_splintView}) {
            if (view) view->removeMesh(compositeKey);
        }
    }

    m_hiddenMaskLabels.erase(label);
    m_maskTable->removeRow(row);
    refreshSegmentationOverlays();
    syncModelViews();
    updateButtonStates();

    statusBar()->showMessage(
        tr("Mascara eliminada: %1").arg(meshLabelName(label)));
}

// ─────────────────────────────────────────────────────────────────────────────
// onLassoEdit
//
// Slot called when the user finishes a lasso stroke on any MPR view.
// 1. Identifies which label is currently selected in the Máscaras table.
// 2. Delegates the pixel-level edit to SegmentationMaskEditor::applyLasso().
// 3. Refreshes segmentation overlays immediately.
// 4. Regenerates the 3D mesh for the modified label so the 3D view stays in
//    sync without any additional user action.
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::onLassoEdit(MPRView* source,
                             QVector<QPointF> vtkDisplayPoints,
                             bool addMode)
{
    if (!m_segmentationLabelmap || !source) return;

    auto preselectedRow = [](QTableWidget* table) -> int {
        if (!table || !table->selectionModel()) return -1;
        const QModelIndexList rows = table->selectionModel()->selectedRows();
        return rows.isEmpty() ? -1 : rows.first().row();
    };
    auto preselectedTableLabel = [](QTableWidget* table, int row, int role) -> int {
        if (!table || row < 0 || row >= table->rowCount()) return -1;
        auto* item = table->item(row, 0);
        if (!item) item = table->item(row, 1);
        return item ? item->data(role).toInt() : -1;
    };
    int preselectedLabel = preselectedTableLabel(m_objectTable, preselectedRow(m_objectTable), Qt::UserRole + 1);
    if (!isEditableSegmentationLabel(preselectedLabel))
        preselectedLabel = preselectedTableLabel(m_maskTable, preselectedRow(m_maskTable), Qt::UserRole);

    // ── Identify target label from the selected row in the mask table ─────
    int label = preselectedLabel;
    if (m_maskTable) {
        const int row = m_maskTable->currentRow();
        if (row >= 0) {
            auto* item = m_maskTable->item(row, 0);
            if (item) label = item->data(Qt::UserRole).toInt();
        }
        // If nothing is selected, fall back to the first entry in the table
        if (label <= 0 && m_maskTable->rowCount() > 0) {
            auto* item = m_maskTable->item(0, 0);
            if (item) label = item->data(Qt::UserRole).toInt();
        }
    }
    if (label <= 0) {
        statusBar()->showMessage(tr("Lasso: selecciona una máscara en la tabla primero."));
        return;
    }

    // ── Save undo snapshot before modifying the labelmap ─────────────────
    auto selectedRow = [](QTableWidget* table) -> int {
        if (!table || !table->selectionModel()) return -1;
        const QModelIndexList rows = table->selectionModel()->selectedRows();
        return rows.isEmpty() ? -1 : rows.first().row();
    };
    auto tableLabel = [](QTableWidget* table, int row, int role) -> int {
        if (!table || row < 0 || row >= table->rowCount()) return -1;
        auto* item = table->item(row, 0);
        if (!item) item = table->item(row, 1);
        return item ? item->data(role).toInt() : -1;
    };

    int explicitLabel = tableLabel(m_objectTable, selectedRow(m_objectTable), Qt::UserRole + 1);
    if (!isEditableSegmentationLabel(explicitLabel))
        explicitLabel = tableLabel(m_maskTable, selectedRow(m_maskTable), Qt::UserRole);
    if (!isEditableSegmentationLabel(explicitLabel)) {
        statusBar()->showMessage(tr("Lasso: selecciona primero una mascara u objeto anatomico editable."));
        return;
    }
    label = explicitLabel;

    pushLabelmapUndo();

    // ── Apply the lasso to the labelmap ───────────────────────────────────
    const bool changed = SegmentationMaskEditor::applyLasso(
        m_segmentationLabelmap,
        source->renderer(),
        vtkDisplayPoints,
        source->orientation(),
        source->currentSlice(),
        label,
        addMode);

    if (!changed) {
        // Nothing was modified — discard the snapshot we just pushed
        if (!m_labelmapUndoStack.isEmpty()) m_labelmapUndoStack.pop();
        return;
    }

    // ── Refresh 2-D overlays on all slices immediately ────────────────────
    refreshSegmentationOverlays();

    const QString derivedNote = refreshEditedSegmentationMesh(label);
    statusBar()->showMessage(
        tr("Lasso %1: máscara actualizada.")
            .arg(addMode ? tr("agregar") : tr("quitar")) + derivedNote);
}

void MainWindow::publishSegmentationMesh(int label, vtkSmartPointer<vtkPolyData> mesh)
{
    if (label <= 0) return;
    const int actorKey = objectActorKey(label);
    const QVector<Mesh3DView*> views {
        m_mesh3DView, m_modelMaxillaView, m_modelUpperArchView,
        m_modelMandibleView, m_modelLowerArchView, m_modelMatchView,
        m_orientationView, m_osteotomyView, m_biteSegmentView,
        m_biteScanView, m_biteRegistrationView, m_repositionView,
        m_splintUpperView, m_splintLowerView, m_splintView
    };
    const bool hasObject = objectEntryExists(label) || std::any_of(views.begin(), views.end(),
        [actorKey](Mesh3DView* view) { return view && view->meshData(actorKey); });
    Mesh3DView* dedicated = label == 5 ? m_modelMaxillaView : label == 6 ? m_modelMandibleView : nullptr;

    // A mask and its calculated object are aliases of the same source geometry.
    // Update only those aliases, never baked composites or moved surgical segments.
    for (Mesh3DView* view : views) {
        if (!view) continue;
        if (!mesh) {
            view->removeMesh(label);
            view->removeMesh(actorKey);
            continue;
        }
        const bool primary = view == m_mesh3DView || view == dedicated;
        if (hasObject && (primary || view->meshData(actorKey))) {
            view->addMesh(actorKey, mesh, meshLabelName(label));
            view->setMeshColor(actorKey, objectColorForLabel(label));
            view->setMeshVisible(actorKey, objectEntryVisible(label));
        }
        if (primary || view->meshData(label)) {
            view->addMesh(label, mesh, meshLabelName(label));
            view->setMeshColor(label, maskColorForLabel(label));
            view->setMeshVisible(label, !m_hiddenMaskLabels.count(label) &&
                !(hasObject && objectEntryVisible(label) && view->meshData(actorKey)));
        }
    }
}

QString MainWindow::refreshEditedSegmentationMesh(int label)
{

    // ── Regenerate the 3-D mesh for this label (synchronous) ─────────────
    QString meshError;
    const int smoothingIterations = m_maskSmoothingIterations.value(label, 20);
    auto mesh = MeshGenerator::generateMesh(
        m_segmentationLabelmap, label, true, smoothingIterations, &meshError);
    publishSegmentationMesh(label, mesh);
    QString derivedNote;
    if (mesh) {
        if (label == 5 && m_upperCompositeMesh) {
            m_upperCompositeMesh = nullptr;
            m_appState.setUpperCompositeReady(false);
            removeObjectEntry(kUpperCompositeLabel);
            const int compositeKey = objectActorKey(kUpperCompositeLabel);
            for (Mesh3DView* view : {m_mesh3DView, m_modelMatchView, m_orientationView,
                                     m_osteotomyView, m_repositionView, m_splintUpperView,
                                     m_splintLowerView, m_splintView}) {
                if (view) view->removeMesh(compositeKey);
            }
            derivedNote = tr(" Recalcula el compuesto maxilar.");
        } else if (label == 6 && m_lowerCompositeMesh) {
            m_lowerCompositeMesh = nullptr;
            m_appState.setLowerCompositeReady(false);
            removeObjectEntry(kLowerCompositeLabel);
            const int compositeKey = objectActorKey(kLowerCompositeLabel);
            for (Mesh3DView* view : {m_mesh3DView, m_modelMatchView, m_orientationView,
                                     m_osteotomyView, m_repositionView, m_splintUpperView,
                                     m_splintLowerView, m_splintView}) {
                if (view) view->removeMesh(compositeKey);
            }
            derivedNote = tr(" Recalcula el compuesto mandibular.");
        }

        syncModelViews();
        updateButtonStates();
    }

    return derivedNote;
}

// ─────────────────────────────────────────────────────────────────────────────
// pushLabelmapUndo — deep-copies the current labelmap onto the undo stack.
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::onSurfaceMeshEdited(int meshLabel, vtkSmartPointer<vtkPolyData> newMesh)
{
    if (!newMesh || newMesh->GetNumberOfPoints() == 0) return;

    const int semanticLabel = (meshLabel >= 1000) ? (meshLabel - 1000) : meshLabel;
    if (semanticLabel <= 0) return;

    auto baked = vtkSmartPointer<vtkPolyData>::New();
    baked->DeepCopy(newMesh);

    if (semanticLabel == kUpperArchLabel) {
        m_upperArchMesh = baked;
    } else if (semanticLabel == kLowerArchLabel) {
        m_lowerArchMesh = baked;
    } else if (semanticLabel == kUpperCompositeLabel) {
        m_upperCompositeMesh = baked;
    } else if (semanticLabel == kLowerCompositeLabel) {
        m_lowerCompositeMesh = baked;
    }

    if (semanticLabel == 5 || semanticLabel == 6) {
        publishSegmentationMesh(semanticLabel, baked);
    }

    syncModelViews();
    updateButtonStates();
}

void MainWindow::pushLabelmapUndo()
{
    if (!m_segmentationLabelmap) return;
    auto copy = vtkSmartPointer<vtkImageData>::New();
    copy->DeepCopy(m_segmentationLabelmap);
    if (m_labelmapUndoStack.size() >= kMaxUndo2D)
        m_labelmapUndoStack.removeFirst();
    m_labelmapUndoStack.push(copy);
}

// ─────────────────────────────────────────────────────────────────────────────
// undoLastEdit
//
// Priority order:
//   1. Labelmap edits (2-D lasso) – most commonly the last action when
//      doing slice-level corrections, and restores both overlays and mesh.
//   2. Mesh surface edits (3-D lasso) – visual-only undo.
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::undoLastEdit()
{
    // ── Priority 0: dental registration points ────────────────────────────
    auto pointsForSet = [this](DentalPointSet set) -> QVector<QVector3D>* {
        switch (set) {
            case DentalPointSet::MaxillaBone:  return &m_maxillaBonePoints;
            case DentalPointSet::UpperArch:    return &m_upperArchPoints;
            case DentalPointSet::MandibleBone: return &m_mandibleBonePoints;
            case DentalPointSet::LowerArch:    return &m_lowerArchPoints;
            case DentalPointSet::None:         return nullptr;
        }
        return nullptr;
    };

    DentalPointSet undoSet = m_dentalPointSet;
    if (undoSet == DentalPointSet::None && m_viewModeStack && m_viewModeStack->currentIndex() == 1) {
        const int step = m_modelStepStack ? m_modelStepStack->currentIndex() : 0;
        if (step == 1) {
            if (!m_lowerArchPoints.isEmpty()) undoSet = DentalPointSet::LowerArch;
            else if (!m_mandibleBonePoints.isEmpty()) undoSet = DentalPointSet::MandibleBone;
        } else {
            if (!m_upperArchPoints.isEmpty()) undoSet = DentalPointSet::UpperArch;
            else if (!m_maxillaBonePoints.isEmpty()) undoSet = DentalPointSet::MaxillaBone;
        }
    }

    if (undoSet != DentalPointSet::None) {
        QVector<QVector3D>* pts = pointsForSet(undoSet);
        if (pts && !pts->isEmpty()) {
            pts->removeLast();
            rebuildDentalPointMarkers();
            updateDentalPointStatus();
            updateButtonStates();
            statusBar()->showMessage(tr("Ctrl+Z: último punto de registro eliminado."));
            return;
        }
    }

    if (!m_labelmapUndoStack.isEmpty()) {
        // Restore previous labelmap
        m_segmentationLabelmap = m_labelmapUndoStack.pop();
        refreshSegmentationOverlays();

        // Regenerate all visible meshes so 3-D stays in sync
        if (m_mesh3DView && m_maskTable) {
            for (int row = 0; row < m_maskTable->rowCount(); ++row) {
                auto* item = m_maskTable->item(row, 0);
                if (!item) continue;
                const int lbl = item->data(Qt::UserRole).toInt();
                if (lbl <= 0) continue;
                QString err;
                const int smoothingIterations = m_maskSmoothingIterations.value(lbl, 20);
                auto mesh = MeshGenerator::generateMesh(
                    m_segmentationLabelmap, lbl, true, smoothingIterations, &err);
                publishSegmentationMesh(lbl, mesh);
            }
        }
        statusBar()->showMessage(tr("Ctrl+Z: edición de máscara deshecha."));

        syncModelViews();
    } else if (m_mesh3DView && m_mesh3DView->canUndo()) {
        m_mesh3DView->undo();
        statusBar()->showMessage(tr("Ctrl+Z: recorte 3D deshecho."));

    } else {
        statusBar()->showMessage(tr("Nada que deshacer."));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::generateSelectedMesh()
{
    int label = selectedMaskLabel();
    if (label <= 0) {
        label = m_structureCombo ? m_structureCombo->currentData().toInt() : 1;
    }

    calculateObjectFromMask(label);
}

vtkSmartPointer<vtkPolyData> MainWindow::loadStlMesh(const QString& filePath, QString* error) const
{
    auto reader = vtkSmartPointer<vtkSTLReader>::New();
    reader->SetFileName(filePath.toLocal8Bit().constData());
    reader->Update();

    vtkPolyData* output = reader->GetOutput();
    if (!output || output->GetNumberOfPoints() == 0 || output->GetNumberOfCells() == 0) {
        if (error) *error = tr("El STL no tiene geometria valida.");
        return nullptr;
    }

    auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
    clean->SetInputData(output);
    clean->Update();

    auto mesh = vtkSmartPointer<vtkPolyData>::New();
    mesh->DeepCopy(clean->GetOutput());
    return mesh;
}

vtkSmartPointer<vtkPolyData> MainWindow::transformMesh(
    vtkPolyData* mesh,
    const QVector<QVector3D>& moving,
    const QVector<QVector3D>& fixed,
    QString* error,
    vtkMatrix4x4* outputMatrix,
    double* landmarkRms) const
{
    if (outputMatrix) outputMatrix->Identity();
    if (landmarkRms) *landmarkRms = 0.0;

    if (!mesh || mesh->GetNumberOfPoints() == 0) {
        if (error) *error = tr("No hay malla STL para registrar.");
        return nullptr;
    }
    const int pairCount = std::min(moving.size(), fixed.size());
    if (pairCount < 3) {
        if (error) {
            *error = tr("Se necesitan al menos 3 pares de puntos correspondientes.");
        }
        return nullptr;
    }

    auto source = vtkSmartPointer<vtkPoints>::New();
    auto target = vtkSmartPointer<vtkPoints>::New();
    for (int i = 0; i < pairCount; ++i) {
        source->InsertNextPoint(moving[i].x(), moving[i].y(), moving[i].z());
        target->InsertNextPoint(fixed[i].x(), fixed[i].y(), fixed[i].z());
    }
    if (moving.size() != fixed.size()) {
        qInfo().noquote() << QString("Landmarks: conteo desigual moving=%1 fixed=%2; usando %3 pares.")
            .arg(moving.size())
            .arg(fixed.size())
            .arg(pairCount);
    }

    auto landmark = vtkSmartPointer<vtkLandmarkTransform>::New();
    landmark->SetSourceLandmarks(source);
    landmark->SetTargetLandmarks(target);
    landmark->SetModeToRigidBody();
    landmark->Update();

    double sumSq = 0.0;
    double maxErr = 0.0;
    double in[3] = {};
    double outPt[3] = {};
    double tgt[3] = {};
    for (int i = 0; i < pairCount; ++i) {
        in[0] = moving[i].x(); in[1] = moving[i].y(); in[2] = moving[i].z();
        tgt[0] = fixed[i].x(); tgt[1] = fixed[i].y(); tgt[2] = fixed[i].z();
        landmark->TransformPoint(in, outPt);
        const double dx = outPt[0] - tgt[0];
        const double dy = outPt[1] - tgt[1];
        const double dz = outPt[2] - tgt[2];
        const double e = std::sqrt(dx * dx + dy * dy + dz * dz);
        sumSq += e * e;
        maxErr = std::max(maxErr, e);
    }
    const double rms = std::sqrt(sumSq / static_cast<double>(pairCount));
    qInfo().noquote() << QString("Landmarks: pares=%1 RMS=%2 mm max=%3 mm")
        .arg(pairCount)
        .arg(rms, 0, 'f', 3)
        .arg(maxErr, 0, 'f', 3);
    qInfo().noquote() << "Matriz inicial landmarks:";
    qInfo().noquote() << matrixToText(landmark->GetMatrix());

    if (outputMatrix)
        outputMatrix->DeepCopy(landmark->GetMatrix());
    if (landmarkRms)
        *landmarkRms = rms;

    auto filter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
    filter->SetInputData(mesh);
    filter->SetTransform(landmark);
    filter->Update();

    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(filter->GetOutput());
    return out;
}

void MainWindow::logRegistrationDiagnostics(const QString& name, vtkPolyData* bone, vtkPolyData* arch) const
{
    auto boundsText = [](vtkPolyData* mesh) {
        if (!mesh || mesh->GetNumberOfPoints() == 0) return QStringLiteral("<empty>");
        double b[6] = {};
        mesh->GetBounds(b);
        return QString("[%1, %2] x [%3, %4] x [%5, %6]")
            .arg(b[0], 0, 'f', 2).arg(b[1], 0, 'f', 2)
            .arg(b[2], 0, 'f', 2).arg(b[3], 0, 'f', 2)
            .arg(b[4], 0, 'f', 2).arg(b[5], 0, 'f', 2);
    };
    auto centroidText = [](vtkPolyData* mesh) {
        if (!mesh || mesh->GetNumberOfPoints() == 0) return QStringLiteral("<empty>");
        double p[3] = {};
        double c[3] = {};
        const vtkIdType step = std::max<vtkIdType>(1, mesh->GetNumberOfPoints() / 20000);
        vtkIdType count = 0;
        for (vtkIdType i = 0; i < mesh->GetNumberOfPoints(); i += step) {
            mesh->GetPoint(i, p);
            c[0] += p[0]; c[1] += p[1]; c[2] += p[2];
            ++count;
        }
        if (count > 0) {
            c[0] /= static_cast<double>(count);
            c[1] /= static_cast<double>(count);
            c[2] /= static_cast<double>(count);
        }
        return QString("(%1, %2, %3)")
            .arg(c[0], 0, 'f', 2).arg(c[1], 0, 'f', 2).arg(c[2], 0, 'f', 2);
    };
    auto diagLength = [](vtkPolyData* mesh) {
        if (!mesh || mesh->GetNumberOfPoints() == 0) return 0.0;
        double b[6] = {};
        mesh->GetBounds(b);
        const double dx = b[1] - b[0];
        const double dy = b[3] - b[2];
        const double dz = b[5] - b[4];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };

    const QString spacing = QString("%1 x %2 x %3 mm")
        .arg(m_volumeMeta.spacing[0], 0, 'f', 3)
        .arg(m_volumeMeta.spacing[1], 0, 'f', 3)
        .arg(m_volumeMeta.spacing[2], 0, 'f', 3);

    qInfo().noquote() << "\n=== Registro TAC-STL:" << name << "===";
    qInfo().noquote() << "Bounds TAC:" << boundsText(bone);
    qInfo().noquote() << "Bounds STL:" << boundsText(arch);
    qInfo().noquote() << "Centroide TAC:" << centroidText(bone);
    qInfo().noquote() << "Centroide STL:" << centroidText(arch);
    qInfo().noquote() << "Spacing DICOM:" << spacing;
    const double boneDiag = diagLength(bone);
    const double archDiag = diagLength(arch);
    const double scaleRatio = boneDiag > 0.0 ? archDiag / boneDiag : 0.0;
    qInfo().noquote() << QString("Escala relativa STL/TAC: %1 (diag STL %2 mm / diag TAC %3 mm)")
        .arg(scaleRatio, 0, 'f', 3)
        .arg(archDiag, 0, 'f', 2)
        .arg(boneDiag, 0, 'f', 2);
    if (scaleRatio > 4.0 || scaleRatio < 0.05)
        qWarning().noquote() << "Advertencia: escala STL/TAC atipica; revise si el STL esta en mm.";
    qInfo().noquote() << "Matriz STL actual: geometria horneada en vtkPolyData; no hay actor transform activo.";
}

vtkSmartPointer<vtkPolyData> MainWindow::refineArchWithIcp(
    vtkPolyData* arch,
    vtkPolyData* bone,
    bool upperArch,
    QString* report,
    QString* error,
    vtkMatrix4x4* outputMatrix) const
{
    if (outputMatrix) outputMatrix->Identity();

    if (!arch || !bone || arch->GetNumberOfPoints() == 0 || bone->GetNumberOfPoints() == 0) {
        if (error) *error = tr("No hay mallas validas para ICP.");
        return nullptr;
    }

    auto cleanTri = [](vtkPolyData* input) -> vtkSmartPointer<vtkPolyData> {
        auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
        clean->SetInputData(input);
        clean->Update();

        auto tri = vtkSmartPointer<vtkTriangleFilter>::New();
        tri->SetInputConnection(clean->GetOutputPort());
        tri->Update();

        auto out = vtkSmartPointer<vtkPolyData>::New();
        out->DeepCopy(tri->GetOutput());
        return out;
    };

    auto cropByBox = [](vtkPolyData* input, const double box[6]) -> vtkSmartPointer<vtkPolyData> {
        if (!input || input->GetNumberOfCells() == 0) return nullptr;
        auto polys = vtkSmartPointer<vtkCellArray>::New();
        auto ids = vtkSmartPointer<vtkIdList>::New();
        double p[3] = {};
        double c[3] = {};

        for (vtkIdType cellId = 0; cellId < input->GetNumberOfCells(); ++cellId) {
            input->GetCellPoints(cellId, ids);
            const vtkIdType n = ids->GetNumberOfIds();
            if (n < 3) continue;
            c[0] = c[1] = c[2] = 0.0;
            for (vtkIdType i = 0; i < n; ++i) {
                input->GetPoint(ids->GetId(i), p);
                c[0] += p[0]; c[1] += p[1]; c[2] += p[2];
            }
            c[0] /= static_cast<double>(n);
            c[1] /= static_cast<double>(n);
            c[2] /= static_cast<double>(n);

            const bool inside = c[0] >= box[0] && c[0] <= box[1] &&
                                c[1] >= box[2] && c[1] <= box[3] &&
                                c[2] >= box[4] && c[2] <= box[5];
            if (inside) polys->InsertNextCell(ids);
        }

        auto out = vtkSmartPointer<vtkPolyData>::New();
        out->SetPoints(input->GetPoints());
        out->SetPolys(polys);
        auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
        clean->SetInputData(out);
        clean->Update();

        auto result = vtkSmartPointer<vtkPolyData>::New();
        result->DeepCopy(clean->GetOutput());
        return result;
    };

    auto archTri = cleanTri(arch);
    auto boneTri = cleanTri(bone);
    if (!archTri || !boneTri) {
        if (error) *error = tr("No se pudieron limpiar las mallas para ICP.");
        return nullptr;
    }

    double archBounds[6] = {};
    archTri->GetBounds(archBounds);
    const double dx = std::max(1.0, archBounds[1] - archBounds[0]);
    const double dy = std::max(1.0, archBounds[3] - archBounds[2]);
    const double dz = std::max(1.0, archBounds[5] - archBounds[4]);
    const double maxRange = std::max({dx, dy, dz});

    const double xyPad = std::clamp(maxRange * 0.18, 12.0, 28.0);
    const double zBelow = upperArch ? std::clamp(maxRange * 0.08, 5.0, 10.0)
                                    : std::clamp(maxRange * 0.22, 14.0, 30.0);
    const double zAbove = upperArch ? std::clamp(maxRange * 0.22, 14.0, 30.0)
                                    : std::clamp(maxRange * 0.08, 5.0, 10.0);
    double roi[6] = {
        archBounds[0] - xyPad, archBounds[1] + xyPad,
        archBounds[2] - xyPad, archBounds[3] + xyPad,
        archBounds[4] - zBelow, archBounds[5] + zAbove
    };

    auto archRoi = cropByBox(archTri, roi);
    auto boneRoi = cropByBox(boneTri, roi);
    if (!archRoi || !boneRoi ||
        archRoi->GetNumberOfPoints() < 50 || boneRoi->GetNumberOfPoints() < 50) {
        if (error) {
            *error = tr("ROI insuficiente para ICP. Revise puntos iniciales o STL importado.");
        }
        return archTri;
    }

    auto icp = vtkSmartPointer<vtkIterativeClosestPointTransform>::New();
    icp->SetSource(archRoi);
    icp->SetTarget(boneRoi);
    icp->GetLandmarkTransform()->SetModeToRigidBody();
    icp->StartByMatchingCentroidsOff();
    icp->CheckMeanDistanceOn();
    icp->SetMaximumNumberOfIterations(80);
    icp->SetMaximumMeanDistance(0.0005);
    icp->Update();
    if (outputMatrix)
        outputMatrix->DeepCopy(icp->GetMatrix());

    auto fullFilter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
    fullFilter->SetInputData(archTri);
    fullFilter->SetTransform(icp);
    fullFilter->Update();

    auto roiFilter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
    roiFilter->SetInputData(archRoi);
    roiFilter->SetTransform(icp);
    roiFilter->Update();

    auto distance = vtkSmartPointer<vtkImplicitPolyDataDistance>::New();
    distance->SetInput(boneRoi);

    vtkPolyData* registeredRoi = roiFilter->GetOutput();
    const vtkIdType total = registeredRoi ? registeredRoi->GetNumberOfPoints() : 0;
    const vtkIdType step = std::max<vtkIdType>(1, total / 25000);
    double p[3] = {};
    double sum = 0.0;
    double sumSq = 0.0;
    double maxDist = 0.0;
    vtkIdType count = 0;
    vtkIdType within1 = 0;
    vtkIdType within2 = 0;
    for (vtkIdType i = 0; i < total; i += step) {
        registeredRoi->GetPoint(i, p);
        const double d = std::abs(distance->EvaluateFunction(p));
        sum += d;
        sumSq += d * d;
        maxDist = std::max(maxDist, d);
        if (d <= 1.0) ++within1;
        if (d <= 2.0) ++within2;
        ++count;
    }

    const double mean = count > 0 ? sum / static_cast<double>(count) : 0.0;
    const double rmse = count > 0 ? std::sqrt(sumSq / static_cast<double>(count)) : 0.0;
    const double pct1 = count > 0 ? 100.0 * static_cast<double>(within1) / static_cast<double>(count) : 0.0;
    const double pct2 = count > 0 ? 100.0 * static_cast<double>(within2) / static_cast<double>(count) : 0.0;
    const QString grade = rmse < 0.75 ? tr("excelente") : (rmse <= 1.5 ? tr("aceptable") : tr("revisar manualmente"));

    if (report) {
        *report = tr("ICP ROI: RMSE %1 mm | media %2 mm | max %3 mm | <=1mm %4% | <=2mm %5% | %6")
            .arg(rmse, 0, 'f', 3)
            .arg(mean, 0, 'f', 3)
            .arg(maxDist, 0, 'f', 3)
            .arg(pct1, 0, 'f', 1)
            .arg(pct2, 0, 'f', 1)
            .arg(grade);
    }

    qInfo().noquote() << "ICP matriz:";
    if (auto* m = icp->GetMatrix()) {
        for (int r = 0; r < 4; ++r) {
            qInfo().noquote() << QString("%1 %2 %3 %4")
                .arg(m->GetElement(r, 0), 0, 'f', 6)
                .arg(m->GetElement(r, 1), 0, 'f', 6)
                .arg(m->GetElement(r, 2), 0, 'f', 6)
                .arg(m->GetElement(r, 3), 0, 'f', 6);
        }
    }
    qInfo().noquote() << (report ? *report : QString());

    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(fullFilter->GetOutput());
    return out;
}

vtkSmartPointer<vtkPolyData> MainWindow::appendMeshes(
    const QVector<vtkSmartPointer<vtkPolyData>>& meshes) const
{
    auto append = vtkSmartPointer<vtkAppendPolyData>::New();
    int count = 0;
    for (const auto& mesh : meshes) {
        if (!mesh || mesh->GetNumberOfPoints() == 0) continue;
        append->AddInputData(mesh);
        ++count;
    }
    if (count == 0) return nullptr;

    append->Update();
    auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
    clean->SetInputConnection(append->GetOutputPort());
    clean->Update();

    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(clean->GetOutput());
    return out;
}

vtkSmartPointer<vtkPolyData> MainWindow::booleanComposite(vtkPolyData* bone, vtkPolyData* arch, bool upperArch) const
{
    if (!bone || !arch) return nullptr;

    auto cleanOutput = [](vtkPolyData* input) -> vtkSmartPointer<vtkPolyData> {
        if (!input || input->GetNumberOfPoints() == 0 || input->GetNumberOfCells() == 0)
            return nullptr;
        auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
        clean->SetInputData(input);
        clean->Update();

        auto tri = vtkSmartPointer<vtkTriangleFilter>::New();
        tri->SetInputConnection(clean->GetOutputPort());
        tri->Update();

        auto out = vtkSmartPointer<vtkPolyData>::New();
        out->DeepCopy(tri->GetOutput());
        if (out->GetNumberOfPoints() == 0 || out->GetNumberOfCells() == 0)
            return nullptr;
        return out;
    };

    auto cleanedBone = cleanOutput(bone);
    auto cleanedArch = cleanOutput(arch);
    if (!cleanedBone || !cleanedArch) return nullptr;

    auto largestRegion = [](vtkPolyData* input) -> vtkSmartPointer<vtkPolyData> {
        if (!input || input->GetNumberOfPoints() == 0 || input->GetNumberOfCells() == 0)
            return nullptr;
        auto connectivity = vtkSmartPointer<vtkPolyDataConnectivityFilter>::New();
        connectivity->SetInputData(input);
        connectivity->SetExtractionModeToLargestRegion();
        connectivity->Update();

        auto out = vtkSmartPointer<vtkPolyData>::New();
        out->DeepCopy(connectivity->GetOutput());
        if (out->GetNumberOfPoints() == 0 || out->GetNumberOfCells() == 0)
            return nullptr;
        return out;
    };

    auto cropArchByHeight = [&](vtkPolyData* input) -> vtkSmartPointer<vtkPolyData> {
        if (!input || input->GetNumberOfPoints() == 0 || input->GetNumberOfCells() == 0)
            return nullptr;

        double bounds[6] = {};
        input->GetBounds(bounds);
        const double zMin = bounds[4];
        const double zMax = bounds[5];
        const double zRange = std::max(1.0, zMax - zMin);
        const double keepFraction = upperArch ? 0.82 : 0.70;
        const double cutZ = upperArch
            ? zMin + zRange * keepFraction
            : zMax - zRange * keepFraction;

        auto polys = vtkSmartPointer<vtkCellArray>::New();
        auto ids = vtkSmartPointer<vtkIdList>::New();
        double p[3] = {};
        double centroidZ = 0.0;

        for (vtkIdType cellId = 0; cellId < input->GetNumberOfCells(); ++cellId) {
            input->GetCellPoints(cellId, ids);
            const vtkIdType n = ids->GetNumberOfIds();
            if (n < 3) continue;

            centroidZ = 0.0;
            for (vtkIdType i = 0; i < n; ++i) {
                input->GetPoint(ids->GetId(i), p);
                centroidZ += p[2];
            }
            centroidZ /= static_cast<double>(n);

            const bool keep = upperArch ? (centroidZ <= cutZ) : (centroidZ >= cutZ);
            if (keep)
                polys->InsertNextCell(ids);
        }

        auto cropped = vtkSmartPointer<vtkPolyData>::New();
        cropped->SetPoints(input->GetPoints());
        cropped->SetPolys(polys);
        return cropped;
    };

    auto triBone = vtkSmartPointer<vtkTriangleFilter>::New();
    triBone->SetInputData(cleanedBone);
    triBone->Update();

    auto triArch = vtkSmartPointer<vtkTriangleFilter>::New();
    triArch->SetInputData(cleanedArch);
    triArch->Update();

    auto croppedArch = cropArchByHeight(triArch->GetOutput());
    auto mainArch = largestRegion(croppedArch);
    if (mainArch) {
        triArch->SetInputData(mainArch);
        triArch->Update();
    }

    auto boneTri = triBone->GetOutput();
    auto archTri = triArch->GetOutput();
    if (!boneTri || !archTri ||
        boneTri->GetNumberOfPoints() == 0 || archTri->GetNumberOfPoints() == 0)
        return nullptr;

    auto boneDistance = vtkSmartPointer<vtkImplicitPolyDataDistance>::New();
    boneDistance->SetInput(boneTri);

    double boneBounds[6] = {};
    boneTri->GetBounds(boneBounds);

    double archBoundsForSize[6] = {};
    archTri->GetBounds(archBoundsForSize);
    const double archSx = std::max(1.0, archBoundsForSize[1] - archBoundsForSize[0]);
    const double archSy = std::max(1.0, archBoundsForSize[3] - archBoundsForSize[2]);
    const double archSz = std::max(1.0, archBoundsForSize[5] - archBoundsForSize[4]);
    const double archMaxRange = std::max({archSx, archSy, archSz});

    const double keepArchDistance = std::clamp(archMaxRange * 0.13, 10.0, 22.0);
    const double keepPad = keepArchDistance + 2.0;
    for (int i = 0; i < 3; ++i) {
        boneBounds[i * 2] -= keepPad;
        boneBounds[i * 2 + 1] += keepPad;
    }

    auto inBoneRegion = [&](const double p[3]) {
        return p[0] >= boneBounds[0] && p[0] <= boneBounds[1] &&
               p[1] >= boneBounds[2] && p[1] <= boneBounds[3] &&
               p[2] >= boneBounds[4] && p[2] <= boneBounds[5];
    };

    auto filteredArchPolys = vtkSmartPointer<vtkCellArray>::New();
    auto ids = vtkSmartPointer<vtkIdList>::New();
    double p[3] = {};
    double centroid[3] = {};

    for (vtkIdType cellId = 0; cellId < archTri->GetNumberOfCells(); ++cellId) {
        archTri->GetCellPoints(cellId, ids);
        const vtkIdType n = ids->GetNumberOfIds();
        if (n < 3) continue;

        centroid[0] = centroid[1] = centroid[2] = 0.0;
        for (vtkIdType i = 0; i < n; ++i) {
            archTri->GetPoint(ids->GetId(i), p);
            centroid[0] += p[0];
            centroid[1] += p[1];
            centroid[2] += p[2];
        }
        centroid[0] /= static_cast<double>(n);
        centroid[1] /= static_cast<double>(n);
        centroid[2] /= static_cast<double>(n);

        bool keep = inBoneRegion(centroid) &&
                    std::abs(boneDistance->EvaluateFunction(centroid)) <= keepArchDistance;
        if (!keep) {
            for (vtkIdType i = 0; i < n; ++i) {
                archTri->GetPoint(ids->GetId(i), p);
                if (inBoneRegion(p) &&
                    std::abs(boneDistance->EvaluateFunction(p)) <= keepArchDistance) {
                    keep = true;
                    break;
                }
            }
        }
        if (keep) {
            filteredArchPolys->InsertNextCell(ids);
        }
    }

    auto filteredArch = vtkSmartPointer<vtkPolyData>::New();
    filteredArch->SetPoints(archTri->GetPoints());
    filteredArch->SetPolys(filteredArchPolys);
    auto archForComposite = cleanOutput(largestRegion(filteredArch));
    if (!archForComposite) {
        archForComposite = cleanOutput(archTri);
    }
    if (!archForComposite) return nullptr;

    // Fast clinical composite: remove the CT/bone surface occupied by the cast,
    // then append the registered STL. This avoids visual/mesh overlap without
    // voxelizing the whole skull, which is too slow for dense dental scans.
    auto archDistance = vtkSmartPointer<vtkImplicitPolyDataDistance>::New();
    archDistance->SetInput(archForComposite);

    double archBounds[6] = {};
    archForComposite->GetBounds(archBounds);
    const double sx = std::max(1.0, archBounds[1] - archBounds[0]);
    const double sy = std::max(1.0, archBounds[3] - archBounds[2]);
    const double sz = std::max(1.0, archBounds[5] - archBounds[4]);
    const double maxRange = std::max({sx, sy, sz});
    const double trimDistance = upperArch
        ? std::clamp(maxRange * 0.045, 3.0, 5.5)
        : std::clamp(maxRange * 0.070, 5.0, 8.5);
    const double xyPad = upperArch
        ? std::clamp(maxRange * 0.035, 2.5, 5.0)
        : std::clamp(maxRange * 0.050, 4.0, 8.0);
    const double zPadLow = upperArch
        ? std::clamp(maxRange * 0.025, 1.5, 3.0)
        : std::clamp(maxRange * 0.040, 2.5, 5.0);
    const double zPadHigh = upperArch
        ? std::clamp(maxRange * 0.030, 2.0, 3.8)
        : std::clamp(maxRange * 0.055, 3.5, 6.5);
    const double xMin = archBounds[0] - xyPad;
    const double xMax = archBounds[1] + xyPad;
    const double yMin = archBounds[2] - xyPad;
    const double yMax = archBounds[3] + xyPad;
    const double zMin = archBounds[4] - zPadLow;
    const double zMax = archBounds[5] + zPadHigh;

    auto inArchTrimBox = [&](const double p[3]) {
        return p[0] >= xMin && p[0] <= xMax &&
               p[1] >= yMin && p[1] <= yMax &&
               p[2] >= zMin && p[2] <= zMax;
    };

    auto keptPolys = vtkSmartPointer<vtkCellArray>::New();

    for (vtkIdType cellId = 0; cellId < boneTri->GetNumberOfCells(); ++cellId) {
        boneTri->GetCellPoints(cellId, ids);
        const vtkIdType n = ids->GetNumberOfIds();
        if (n < 3) continue;

        centroid[0] = centroid[1] = centroid[2] = 0.0;
        for (vtkIdType i = 0; i < n; ++i) {
            boneTri->GetPoint(ids->GetId(i), p);
            centroid[0] += p[0];
            centroid[1] += p[1];
            centroid[2] += p[2];
        }
        centroid[0] /= static_cast<double>(n);
        centroid[1] /= static_cast<double>(n);
        centroid[2] /= static_cast<double>(n);

        bool remove = false;
        if (inArchTrimBox(centroid)) {
            remove = archDistance->EvaluateFunction(centroid) <= trimDistance;
        }
        if (!remove) {
            for (vtkIdType i = 0; i < n; ++i) {
                boneTri->GetPoint(ids->GetId(i), p);
                if (inArchTrimBox(p) &&
                    archDistance->EvaluateFunction(p) <= trimDistance) {
                    remove = true;
                    break;
                }
            }
        }
        if (!remove) {
            keptPolys->InsertNextCell(ids);
        }
    }

    auto trimmedBone = vtkSmartPointer<vtkPolyData>::New();
    trimmedBone->SetPoints(boneTri->GetPoints());
    trimmedBone->SetPolys(keptPolys);

    auto append = vtkSmartPointer<vtkAppendPolyData>::New();
    append->AddInputData(trimmedBone);
    append->AddInputData(archForComposite);
    append->Update();

    return cleanOutput(append->GetOutput());
}

vtkSmartPointer<vtkPolyData> MainWindow::meshForAnatomicLabel(int label) const
{
    if (m_mesh3DView) {
        if (auto objectMesh = m_mesh3DView->meshData(objectActorKey(label))) return objectMesh;
        if (auto maskMesh = m_mesh3DView->meshData(label)) return maskMesh;
    }
    if (!m_segmentationLabelmap) return nullptr;

    QString error;
    const int smoothingIterations = m_maskSmoothingIterations.value(label, 20);
    auto generated = MeshGenerator::generateMesh(
        m_segmentationLabelmap, label, true, smoothingIterations, &error);
    return generated;
}

void MainWindow::importUpperArchStl()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Importar arco superior STL"), QString(), tr("STL (*.stl)"));
    if (path.isEmpty()) return;

    QString error;
    auto mesh = loadStlMesh(path, &error);
    if (!mesh) {
        QMessageBox::critical(this, tr("STL superior"), error);
        return;
    }

    // Phase 3: validate mesh scale
    {
        const ValidationResult vr = GeometryValidation::validateMeshScale(mesh, QStringLiteral("STL arco"));
        if (vr.isError()) {
            QMessageBox::warning(this, tr("STL superior — Escala"),
                tr("La malla tiene una escala inusual:\n%1\n\n"
                   "Verifique que el STL esté en milímetros.").arg(vr.message));
            LoggerCore::instance().logValidation(
                QStringLiteral("STL_upper"), vr.message, false);
            return;
        }
        if (vr.isWarn()) {
            statusBar()->showMessage(tr("Advertencia escala STL sup: ") + vr.message);
            LoggerCore::instance().logValidation(
                QStringLiteral("STL_upper"), vr.message, true);
        }
    }

    // Phase 4: log STL load
    LoggerCore::instance().logStlLoad(path, QStringLiteral("upper_arch"));

    m_upperArchMesh = mesh;
    m_upperArchOriginalMesh = vtkSmartPointer<vtkPolyData>::New();
    m_upperArchOriginalMesh->DeepCopy(mesh);
    m_upperArchPoints.clear();
    m_upperArchRegistrationMatrix = identityMatrix();
    m_upperRegistrationReport.clear();
    m_upperRegistrationCalculated = false;
    m_upperRegResult = {};

    const QColor color = objectColorForLabel(kUpperArchLabel);
    const int actorKey = objectActorKey(kUpperArchLabel);
    if (m_mesh3DView) {
        m_mesh3DView->addMesh(actorKey, mesh, meshLabelName(kUpperArchLabel));
        m_mesh3DView->setMeshColor(actorKey, color);
        m_mesh3DView->setMeshOpacity(actorKey, 1.0);
    }
    addObjectEntry(meshLabelName(kUpperArchLabel), color, kUpperArchLabel);

    // Phase 1
    m_appState.setUpperArchImported(true);
    m_appState.setUpperRegistered(false);
    updateButtonStates();

    // Refresh individual views + match preview (bone + arch together)
    syncModelViews();
    statusBar()->showMessage(tr("Arco superior STL importado. Marque Pts Sup para registrar."));
}

void MainWindow::importLowerArchStl()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Importar arco inferior STL"), QString(), tr("STL (*.stl)"));
    if (path.isEmpty()) return;

    QString error;
    auto mesh = loadStlMesh(path, &error);
    if (!mesh) {
        QMessageBox::critical(this, tr("STL inferior"), error);
        return;
    }

    // Phase 3: validate mesh scale
    {
        const ValidationResult vr = GeometryValidation::validateMeshScale(mesh, QStringLiteral("STL arco"));
        if (vr.isError()) {
            QMessageBox::warning(this, tr("STL inferior — Escala"),
                tr("La malla tiene una escala inusual:\n%1\n\n"
                   "Verifique que el STL esté en milímetros.").arg(vr.message));
            LoggerCore::instance().logValidation(
                QStringLiteral("STL_lower"), vr.message, false);
            return;
        }
        if (vr.isWarn()) {
            statusBar()->showMessage(tr("Advertencia escala STL inf: ") + vr.message);
            LoggerCore::instance().logValidation(
                QStringLiteral("STL_lower"), vr.message, true);
        }
    }

    // Phase 4: log STL load
    LoggerCore::instance().logStlLoad(path, QStringLiteral("lower_arch"));

    m_lowerArchMesh = mesh;
    m_lowerArchOriginalMesh = vtkSmartPointer<vtkPolyData>::New();
    m_lowerArchOriginalMesh->DeepCopy(mesh);
    m_lowerArchPoints.clear();
    m_lowerArchRegistrationMatrix = identityMatrix();
    m_lowerRegistrationReport.clear();
    m_lowerRegistrationCalculated = false;
    m_lowerRegResult = {};

    const QColor color = objectColorForLabel(kLowerArchLabel);
    const int actorKey = objectActorKey(kLowerArchLabel);
    if (m_mesh3DView) {
        m_mesh3DView->addMesh(actorKey, mesh, meshLabelName(kLowerArchLabel));
        m_mesh3DView->setMeshColor(actorKey, color);
        m_mesh3DView->setMeshOpacity(actorKey, 1.0);
    }
    addObjectEntry(meshLabelName(kLowerArchLabel), color, kLowerArchLabel);

    // Phase 1
    m_appState.setLowerArchImported(true);
    m_appState.setLowerRegistered(false);
    updateButtonStates();

    // Refresh individual views + match preview (bone + arch together)
    syncModelViews();
    statusBar()->showMessage(tr("Arco inferior STL importado. Marque Pts Inf para registrar."));
}

void MainWindow::setDentalPointCapture(DentalPointSet set)
{
    deactivateLassoTools();
    m_dentalPointSet = set;
    const bool boneMode = set == DentalPointSet::MaxillaBone || set == DentalPointSet::MandibleBone;
    const bool archMode = set == DentalPointSet::UpperArch || set == DentalPointSet::LowerArch;
    if (m_mesh3DView) m_mesh3DView->setPointPickMode(set != DentalPointSet::None);
    // Each model view activates point-pick only when its anatomic role is selected
    if (m_modelMaxillaView)   m_modelMaxillaView->setPointPickMode(set == DentalPointSet::MaxillaBone);
    if (m_modelUpperArchView) m_modelUpperArchView->setPointPickMode(set == DentalPointSet::UpperArch);
    if (m_modelMandibleView)  m_modelMandibleView->setPointPickMode(set == DentalPointSet::MandibleBone);
    if (m_modelLowerArchView) m_modelLowerArchView->setPointPickMode(set == DentalPointSet::LowerArch);
    updateDentalPointStatus();
    updateModelWorkflowUi();
}

void MainWindow::onDentalPointPicked(int actorLabel, double x, double y, double z)
{
    if (m_dentalPointSet == DentalPointSet::None) return;

    const bool maxillaActor = actorLabel == 5 || actorLabel == objectActorKey(5);
    const bool mandibleActor = actorLabel == 6 || actorLabel == objectActorKey(6);
    const bool upperActor = actorLabel == objectActorKey(kUpperArchLabel);
    const bool lowerActor = actorLabel == objectActorKey(kLowerArchLabel);

    QVector<QVector3D>* target = nullptr;
    QColor markerColor(80, 220, 120);
    bool accepted = false;
    switch (m_dentalPointSet) {
        case DentalPointSet::MaxillaBone:
            accepted = maxillaActor;
            target = &m_maxillaBonePoints;
            markerColor = QColor(80, 220, 120);
            break;
        case DentalPointSet::UpperArch:
            accepted = upperActor;
            target = &m_upperArchPoints;
            markerColor = QColor(255, 170, 55);
            break;
        case DentalPointSet::MandibleBone:
            accepted = mandibleActor;
            target = &m_mandibleBonePoints;
            markerColor = QColor(90, 190, 255);
            break;
        case DentalPointSet::LowerArch:
            accepted = lowerActor;
            target = &m_lowerArchPoints;
            markerColor = QColor(255, 210, 75);
            break;
        case DentalPointSet::None:
            break;
    }

    if (!accepted || !target) {
        statusBar()->showMessage(tr("Punto rechazado: seleccione la superficie correcta para este paso."));
        return;
    }

    target->append(QVector3D(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)));
    if (m_mesh3DView) m_mesh3DView->addPointMarker(x, y, z, markerColor);
    if (maxillaActor && m_modelMaxillaView)
        m_modelMaxillaView->addPointMarker(x, y, z, markerColor);
    if (mandibleActor && m_modelMandibleView)
        m_modelMandibleView->addPointMarker(x, y, z, markerColor);
    if (upperActor && m_modelUpperArchView)
        m_modelUpperArchView->addPointMarker(x, y, z, markerColor);
    if (lowerActor && m_modelLowerArchView)
        m_modelLowerArchView->addPointMarker(x, y, z, markerColor);
    updateDentalPointStatus();
    updateButtonStates();
}

void MainWindow::updateDentalPointStatus()
{
    QString mode;
    switch (m_dentalPointSet) {
        case DentalPointSet::MaxillaBone: mode = tr("Capturando puntos del maxilar"); break;
        case DentalPointSet::UpperArch: mode = tr("Capturando puntos del arco superior"); break;
        case DentalPointSet::MandibleBone: mode = tr("Capturando puntos de la mandibula"); break;
        case DentalPointSet::LowerArch: mode = tr("Capturando puntos del arco inferior"); break;
        case DentalPointSet::None: mode = tr("Captura de puntos desactivada"); break;
    }
    statusBar()->showMessage(
        tr("%1 | Max:%2 Sup:%3 Mand:%4 Inf:%5")
            .arg(mode)
            .arg(m_maxillaBonePoints.size())
            .arg(m_upperArchPoints.size())
            .arg(m_mandibleBonePoints.size())
            .arg(m_lowerArchPoints.size()));
}

// ─────────────────────────────────────────────────────────────────────────────
// rebuildDentalPointMarkers
//
// Clears all point markers in every model view and redraws them from the four
// registration-point vectors.  Used by Ctrl+Z (undo last point) and by
// clearDentalRegistrationPoints().
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::rebuildDentalPointMarkers()
{
    // Clear all markers everywhere
    for (Mesh3DView* v : {m_mesh3DView, m_modelMaxillaView, m_modelUpperArchView,
                          m_modelMandibleView, m_modelLowerArchView, m_modelMatchView})
        if (v) v->clearPointMarkers();

    const QColor cMax(80, 220, 120);   // green  – maxilar bone
    const QColor cUAr(255, 170, 55);   // orange – upper arch
    const QColor cMan(90, 190, 255);   // blue   – mandible bone
    const QColor cLAr(255, 210, 75);   // yellow – lower arch

    for (const auto& p : m_maxillaBonePoints) {
        if (m_mesh3DView)      m_mesh3DView->addPointMarker(p.x(), p.y(), p.z(), cMax);
        if (m_modelMaxillaView) m_modelMaxillaView->addPointMarker(p.x(), p.y(), p.z(), cMax);
    }
    for (const auto& p : m_upperArchPoints) {
        if (m_mesh3DView)        m_mesh3DView->addPointMarker(p.x(), p.y(), p.z(), cUAr);
        if (m_modelUpperArchView) m_modelUpperArchView->addPointMarker(p.x(), p.y(), p.z(), cUAr);
    }
    for (const auto& p : m_mandibleBonePoints) {
        if (m_mesh3DView)       m_mesh3DView->addPointMarker(p.x(), p.y(), p.z(), cMan);
        if (m_modelMandibleView) m_modelMandibleView->addPointMarker(p.x(), p.y(), p.z(), cMan);
    }
    for (const auto& p : m_lowerArchPoints) {
        if (m_mesh3DView)        m_mesh3DView->addPointMarker(p.x(), p.y(), p.z(), cLAr);
        if (m_modelLowerArchView) m_modelLowerArchView->addPointMarker(p.x(), p.y(), p.z(), cLAr);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// showFinalCompositeView
//
// Called after both upper and lower composite models are generated.
// Hides the step-pair widgets and shows only the match-preview view
// (retitled "MODELOS COMPUESTOS") with both composites rendered in it.
// The "Crear Modelo Compuesto" button is repurposed to "Exportar STL".
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::showFinalCompositeView(bool advanceToOrientation)
{
    // Keep the MODELOS match view updated (available if user returns to MODELOS tab)
    if (m_modelStepStack) m_modelStepStack->setVisible(false);
    if (m_modelMatchView) {
        m_modelMatchView->clearMeshes();
        m_modelMatchView->setTitle(tr("MODELOS COMPUESTOS"));
        if (m_upperCompositeMesh) {
            const int k = objectActorKey(kUpperCompositeLabel);
            m_modelMatchView->addMesh(k, m_upperCompositeMesh, meshLabelName(kUpperCompositeLabel));
            m_modelMatchView->setMeshColor(k, objectColorForLabel(kUpperCompositeLabel));
        }
        if (m_lowerCompositeMesh) {
            const int k = objectActorKey(kLowerCompositeLabel);
            m_modelMatchView->addMesh(k, m_lowerCompositeMesh, meshLabelName(kLowerCompositeLabel));
            m_modelMatchView->setMeshColor(k, objectColorForLabel(kLowerCompositeLabel));
        }
    }
    publishCompositeMeshesToSceneViews();
    if (m_compositeButton) {
        m_compositeButton->setText(tr("Exportar STL compuesto"));
        disconnect(m_compositeButton, &QPushButton::clicked,
                   this, &MainWindow::createDentalCompositeModels);
        connect(m_compositeButton, &QPushButton::clicked,
                this, &MainWindow::exportDentalCompositeStl,
                Qt::UniqueConnection);
    }

    if (!advanceToOrientation) {
        statusBar()->showMessage(
            tr("Modelos compuestos cargados. Puede revisarlos o exportarlos desde MODELOS."));
        return;
    }

    // Transition to the PLAN/Orientación workspace (page 2 of m_viewModeStack)
    for (auto* tab : findChildren<QToolButton*>(QStringLiteral("MT"))) {
        if (tab && tab->text() == tr("ORIENTACION")) {
            tab->click();
            break;
        }
    }
    setOrientationWorkspace(true);

    // Sequential Frankfurt buttons: only PorionD is enabled on entry.
    // Remaining buttons unlock via updateFrankfurtPointStatus() as points are placed.
    if (m_frankfortPorionDAct)  m_frankfortPorionDAct->setEnabled(true);
    if (m_frankfortPorionIAct)  m_frankfortPorionIAct->setEnabled(false);
    if (m_frankfortOrbitalDAct) m_frankfortOrbitalDAct->setEnabled(false);
    if (m_frankfortOrbitalIAct) m_frankfortOrbitalIAct->setEnabled(false);
    if (m_alignFrankfurtAct)    m_alignFrankfurtAct->setEnabled(false);
    if (m_midlineGizmoAct)      m_midlineGizmoAct->setEnabled(false);
    if (m_midlineAcceptGizmoAct) m_midlineAcceptGizmoAct->setEnabled(false);
    if (m_saveOrientationAct)   m_saveOrientationAct->setEnabled(false);
    if (m_exportOrientedAct)    m_exportOrientedAct->setEnabled(false);

    statusBar()->showMessage(
        tr("Modelos compuestos listos. Marque los 4 puntos de Frankfort para alinear el craneo."));
}

// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::onOrientationPointPicked(int actorLabel, double x, double y, double z)
{
    onFrankfurtPointPicked(actorLabel, x, y, z);
}

// Frankfurt plane landmark capture: called by m_orientationView's pointPicked signal.
void MainWindow::onFrankfurtPointPicked(int /*actorLabel*/, double x, double y, double z)
{
    if (m_frankfurtCapturingIdx < 0 || m_frankfurtCapturingIdx > 3) return;

    while (m_frankfurtPoints.size() <= m_frankfurtCapturingIdx)
        m_frankfurtPoints.append(QVector3D{});

    m_frankfurtPoints[m_frankfurtCapturingIdx] = QVector3D(
        static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));

    static const QColor kColors[4] = { Qt::red, Qt::blue, Qt::green, Qt::cyan };
    if (m_orientationView)
        m_orientationView->addPointMarker(x, y, z, kColors[m_frankfurtCapturingIdx]);

    // Mark the corresponding button as "placed"
    const QAction* const kFrankActs[4] = {
        m_frankfortPorionDAct, m_frankfortPorionIAct,
        m_frankfortOrbitalDAct, m_frankfortOrbitalIAct
    };
    if (auto* act = const_cast<QAction*>(kFrankActs[m_frankfurtCapturingIdx]))
        act->setChecked(true);

    m_frankfurtCapturingIdx = -1;
    if (m_orientationView) m_orientationView->setPointPickMode(false);
    updateFrankfurtPointStatus();
}

// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::updateFrankfurtPointStatus()
{
    const int n = static_cast<int>(m_frankfurtPoints.size());
    QAction* const pointActions[4] = {
        m_frankfortPorionDAct, m_frankfortPorionIAct,
        m_frankfortOrbitalDAct, m_frankfortOrbitalIAct
    };
    const QVector<QAction*> orientationActions = {
        m_frankfortPorionDAct, m_frankfortPorionIAct,
        m_frankfortOrbitalDAct, m_frankfortOrbitalIAct,
        m_alignFrankfurtAct, m_midlineGizmoAct, m_midlineAcceptGizmoAct,
        m_saveOrientationAct, m_exportOrientedAct
    };
    clearGuidedActionStates(orientationActions);

    // Sequential unlock: each button enabled only after the previous point is placed
    if (m_frankfortPorionIAct)  m_frankfortPorionIAct->setEnabled(n >= 1);
    if (m_frankfortOrbitalDAct) m_frankfortOrbitalDAct->setEnabled(n >= 2);
    if (m_frankfortOrbitalIAct) m_frankfortOrbitalIAct->setEnabled(n >= 3);
    if (m_alignFrankfurtAct)    m_alignFrankfurtAct->setEnabled(n >= 4);

    for (int i = 0; i < std::min(n, 4); ++i)
        setGuidedDone(pointActions[i]);

    if (n < 4) {
        setGuidedNext(pointActions[n]);
    } else if (m_midlineAcceptGizmoAct && m_midlineAcceptGizmoAct->isEnabled()) {
        setGuidedNext(m_midlineAcceptGizmoAct);
    } else if (m_midlineGizmoAct && m_midlineGizmoAct->isEnabled()) {
        setGuidedNext(m_midlineGizmoAct);
    } else if (m_saveOrientationAct && m_saveOrientationAct->isEnabled()) {
        setGuidedNext(m_saveOrientationAct);
    } else if (m_alignFrankfurtAct && m_alignFrankfurtAct->isEnabled()) {
        setGuidedNext(m_alignFrankfurtAct);
    }

    const QString msg = (n >= 4)
        ? tr("4 puntos de Frankfort marcados. Listo para alinear.")
        : tr("Marque: ") +
          QStringList({tr("Porion D"), tr("Porion I"), tr("Orbital D"), tr("Orbital I")})
              .mid(n).join(QStringLiteral(", "));
    statusBar()->showMessage(msg);
    updateOrientationGuide();
}

// ─────────────────────────────────────────────────────────────────────────────
// Gizmo feedback: called when the user stops the orientation-view gizmo.
// The upper composite has already been baked into newMesh; apply the same
// transform matrix to the lower composite so both move in sync.
void MainWindow::onOrientationGizmoUpdated(int meshLabel, vtkSmartPointer<vtkPolyData> /*newMesh*/)
{
    if (!m_orientationView) return;

    // Get the accumulated transform from the gizmo before the view clears it
    auto mat = m_orientationView->lastGizmoTransformMatrix();

    if (meshLabel == kOrientGizmoTempLabel) {
        // Combined-mesh gizmo: apply the matrix to each composite individually
        auto applyMatrix = [&](vtkSmartPointer<vtkPolyData>& mesh) {
            if (!mesh || !mat) return;
            auto t = vtkSmartPointer<vtkTransform>::New();
            t->SetMatrix(mat);
            auto f = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
            f->SetTransform(t);
            f->SetInputData(mesh);
            f->Update();
            mesh->DeepCopy(f->GetOutput());
        };
        applyMatrix(m_upperCompositeMesh);
        applyMatrix(m_lowerCompositeMesh);

        // Transform Frankfurt landmark points too
        if (mat) {
            for (auto& pt : m_frankfurtPoints) {
                double in[4]  = { pt.x(), pt.y(), pt.z(), 1.0 };
                double out[4] = {};
                mat->MultiplyPoint(in, out);
                pt = QVector3D(float(out[0]), float(out[1]), float(out[2]));
            }
        }
    }

    // Rebuild the orientation view: remove combined temp mesh, restore individual composites
    m_orientationView->clearMeshes();
    m_orientationView->clearPointMarkers();
    if (m_upperCompositeMesh) {
        const int k = objectActorKey(kUpperCompositeLabel);
        m_orientationView->addMesh(k, m_upperCompositeMesh, meshLabelName(kUpperCompositeLabel));
        m_orientationView->setMeshColor(k, objectColorForLabel(kUpperCompositeLabel));
    }
    if (m_lowerCompositeMesh) {
        const int k = objectActorKey(kLowerCompositeLabel);
        m_orientationView->addMesh(k, m_lowerCompositeMesh, meshLabelName(kLowerCompositeLabel));
        m_orientationView->setMeshColor(k, objectColorForLabel(kLowerCompositeLabel));
    }
    // Re-add transformed landmark markers
    static const QColor kColors[4] = { Qt::red, Qt::blue, Qt::green, Qt::cyan };
    for (int i = 0; i < static_cast<int>(m_frankfurtPoints.size()); ++i)
        m_orientationView->addPointMarker(m_frankfurtPoints[i].x(),
                                          m_frankfurtPoints[i].y(),
                                          m_frankfurtPoints[i].z(), kColors[i]);

    publishCompositeMeshesToSceneViews();
    LoggerCore::instance().logCustom(QStringLiteral("MIDLINE"),
        QStringLiteral("Orientation gizmo applied to both composites."));
    statusBar()->showMessage(tr("Ajuste de linea media aplicado a ambos modelos."));
}

// ─────────────────────────────────────────────────────────────────────────────
// Computes the rotation that makes the Frankfurt plane horizontal (normal → Z+)
// and applies it to both composite meshes in-place.
void MainWindow::alignFrankfurtPlane()
{
    if (m_frankfurtPoints.size() < 4) {
        QMessageBox::warning(this, tr("Plano de Frankfort"),
            tr("Necesita marcar los 4 puntos (Porion D, Porion I, Orbital D, Orbital I) primero."));
        return;
    }

    // Points: P0=PorionD, P1=PorionI, P2=OrbitalD, P3=OrbitalI
    // Best-fit plane normal via Newell's method (polygon order: P0→P2→P3→P1)
    // This gives a convex quadrilateral covering the Frankfurt plane.
    const QVector3D pts[4] = {
        m_frankfurtPoints[0],  // Porion D
        m_frankfurtPoints[2],  // Orbital D  (right side together)
        m_frankfurtPoints[3],  // Orbital I  (left side together)
        m_frankfurtPoints[1],  // Porion I
    };
    QVector3D normal(0.0f, 0.0f, 0.0f);
    for (int i = 0; i < 4; ++i) {
        const QVector3D& curr = pts[i];
        const QVector3D& next = pts[(i + 1) % 4];
        normal += QVector3D(
            (curr.y() - next.y()) * (curr.z() + next.z()),
            (curr.z() - next.z()) * (curr.x() + next.x()),
            (curr.x() - next.x()) * (curr.y() + next.y()));
    }
    normal.normalize();

    // We want to align 'normal' with (0, 0, 1) — make the plane horizontal
    const QVector3D target(0.0f, 0.0f, 1.0f);
    const float cosA = QVector3D::dotProduct(normal, target);
    const float angle = static_cast<float>(std::acos(std::clamp(static_cast<double>(cosA), -1.0, 1.0)));

    // Rotation axis = normal × target
    QVector3D axis = QVector3D::crossProduct(normal, target);
    const float axisLen = axis.length();

    if (axisLen < 1e-6f) {
        axis = (cosA < 0.0f) ? QVector3D(1.0f, 0.0f, 0.0f)
                             : QVector3D(0.0f, 0.0f, 1.0f);
    } else {
        axis.normalize();
    }

    // Compute the composite bounding-box center (pivot for rotation)
    double bounds[6] = { 1e30, -1e30, 1e30, -1e30, 1e30, -1e30 };
    auto expandBounds = [&](vtkPolyData* pd) {
        if (!pd) return;
        double b[6];
        pd->GetBounds(b);
        bounds[0] = std::min(bounds[0], b[0]); bounds[1] = std::max(bounds[1], b[1]);
        bounds[2] = std::min(bounds[2], b[2]); bounds[3] = std::max(bounds[3], b[3]);
        bounds[4] = std::min(bounds[4], b[4]); bounds[5] = std::max(bounds[5], b[5]);
    };
    expandBounds(m_upperCompositeMesh);
    expandBounds(m_lowerCompositeMesh);
    const double cx = (bounds[0]+bounds[1])*0.5;
    const double cy = (bounds[2]+bounds[3])*0.5;
    const double cz = (bounds[4]+bounds[5])*0.5;

    // Build VTK transform: RotateWXYZ(degrees, axis) around center
    const float angleDeg = angle * 180.0f / static_cast<float>(M_PI);
    auto t = vtkSmartPointer<vtkTransform>::New();
    t->Translate( cx,  cy,  cz);
    t->RotateWXYZ(angleDeg, axis.x(), axis.y(), axis.z());
    t->Translate(-cx, -cy, -cz);

    auto transformPoint = [](vtkTransform* tx, const QVector3D& pt) {
        double in[4]  = { pt.x(), pt.y(), pt.z(), 1.0 };
        double out[4] = {};
        tx->GetMatrix()->MultiplyPoint(in, out);
        return QVector3D(static_cast<float>(out[0]),
                         static_cast<float>(out[1]),
                         static_cast<float>(out[2]));
    };

    QVector<QVector3D> leveledPoints = m_frankfurtPoints;
    for (auto& pt : leveledPoints)
        pt = transformPoint(t, pt);

    QVector3D leftRight = leveledPoints[1] - leveledPoints[0]; // Porion I - Porion D
    leftRight.setZ(0.0f);
    double yawDeg = 0.0;
    if (leftRight.length() >= 1e-6f) {
        leftRight.normalize();
        double yawRad = std::atan2(static_cast<double>(leftRight.y()),
                                   static_cast<double>(leftRight.x()));
        if (yawRad > M_PI * 0.5)
            yawRad -= M_PI;
        else if (yawRad < -M_PI * 0.5)
            yawRad += M_PI;
        yawDeg = -yawRad * 180.0 / M_PI;
    }

    auto yawTransform = vtkSmartPointer<vtkTransform>::New();
    yawTransform->Translate( cx,  cy,  cz);
    yawTransform->RotateZ(yawDeg);
    yawTransform->Translate(-cx, -cy, -cz);

    // Apply to both composite meshes in-place
    auto applyTransform = [&](vtkSmartPointer<vtkPolyData>& mesh, vtkTransform* tx) {
        if (!mesh) return;
        auto f = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
        f->SetTransform(tx);
        f->SetInputData(mesh);
        f->Update();
        auto updated = vtkSmartPointer<vtkPolyData>::New();
        updated->DeepCopy(f->GetOutput());
        mesh = updated;
    };
    applyTransform(m_upperCompositeMesh, t);
    applyTransform(m_lowerCompositeMesh, t);
    applyTransform(m_upperCompositeMesh, yawTransform);
    applyTransform(m_lowerCompositeMesh, yawTransform);

    // Transform the landmark points too
    for (auto& pt : m_frankfurtPoints) {
        pt = transformPoint(t, pt);
        pt = transformPoint(yawTransform, pt);
    }

    // Repopulate orientation view
    if (m_orientationView) {
        m_orientationView->clearMeshes();
        m_orientationView->clearPointMarkers();
        if (m_upperCompositeMesh) {
            const int k = objectActorKey(kUpperCompositeLabel);
            m_orientationView->addMesh(k, m_upperCompositeMesh,
                                       meshLabelName(kUpperCompositeLabel));
            m_orientationView->setMeshColor(k, objectColorForLabel(kUpperCompositeLabel));
        }
        if (m_lowerCompositeMesh) {
            const int k = objectActorKey(kLowerCompositeLabel);
            m_orientationView->addMesh(k, m_lowerCompositeMesh,
                                       meshLabelName(kLowerCompositeLabel));
            m_orientationView->setMeshColor(k, objectColorForLabel(kLowerCompositeLabel));
        }

        // Add translucent Frankfurt plane disc as visual indicator.
        // m_frankfurtPoints have already been transformed in-place above —
        // use their average directly as the disc center.
        const QVector3D planeCenter = (m_frankfurtPoints[0] + m_frankfurtPoints[1]
                                      + m_frankfurtPoints[2] + m_frankfurtPoints[3]) / 4.0f;

        const double diagHalf = std::sqrt(
            std::pow(bounds[1]-bounds[0],2) +
            std::pow(bounds[3]-bounds[2],2) +
            std::pow(bounds[5]-bounds[4],2)) * 0.4;

        auto disc = vtkSmartPointer<vtkRegularPolygonSource>::New();
        disc->SetNumberOfSides(80);
        disc->SetRadius(diagHalf);
        disc->SetCenter(planeCenter.x(), planeCenter.y(), planeCenter.z());
        disc->SetNormal(0.0, 0.0, 1.0);  // already aligned to Z after rotation
        disc->GeneratePolygonOn();
        disc->Update();

        const int discLabel = -100;  // special label for the plane indicator
        m_orientationView->addMesh(discLabel, disc->GetOutput(),
                                   tr("Plano Frankfort"));
        m_orientationView->setMeshColor(discLabel, QColor(255, 230, 0));
        m_orientationView->setMeshOpacity(discLabel, 0.28);

        auto makeTubeLine = [](const QVector3D& a, const QVector3D& b, double radius) {
            auto line = vtkSmartPointer<vtkLineSource>::New();
            line->SetPoint1(a.x(), a.y(), a.z());
            line->SetPoint2(b.x(), b.y(), b.z());
            line->Update();

            auto tube = vtkSmartPointer<vtkTubeFilter>::New();
            tube->SetInputConnection(line->GetOutputPort());
            tube->SetRadius(radius);
            tube->SetNumberOfSides(16);
            tube->Update();

            auto out = vtkSmartPointer<vtkPolyData>::New();
            out->DeepCopy(tube->GetOutput());
            return out;
        };

        const double lineRadius = std::max(0.35, diagHalf * 0.004);
        auto horizontalLine = makeTubeLine(
            QVector3D(planeCenter.x(), planeCenter.y() - static_cast<float>(diagHalf), planeCenter.z()),
            QVector3D(planeCenter.x(), planeCenter.y() + static_cast<float>(diagHalf), planeCenter.z()),
            lineRadius);

        const int lineLabel = -101;
        m_orientationView->addMesh(lineLabel, horizontalLine, tr("Horizontal Frankfort"));
        m_orientationView->setMeshColor(lineLabel, QColor(80, 255, 120));
        m_orientationView->setMeshOpacity(lineLabel, 1.0);

        auto rightFrankfortLine = makeTubeLine(m_frankfurtPoints[0], m_frankfurtPoints[2],
                                               lineRadius * 1.25);
        auto leftFrankfortLine = makeTubeLine(m_frankfurtPoints[1], m_frankfurtPoints[3],
                                              lineRadius * 1.25);
        m_orientationView->addMesh(-102, rightFrankfortLine, tr("Frankfort derecho"));
        m_orientationView->setMeshColor(-102, QColor(255, 230, 0));
        m_orientationView->setMeshOpacity(-102, 1.0);
        m_orientationView->addMesh(-103, leftFrankfortLine, tr("Frankfort izquierdo"));
        m_orientationView->setMeshColor(-103, QColor(255, 230, 0));
        m_orientationView->setMeshOpacity(-103, 1.0);

        // Re-add transformed landmark markers
        static const QColor kColors[4] = { Qt::red, Qt::blue, Qt::green, Qt::cyan };
        for (int i = 0; i < static_cast<int>(m_frankfurtPoints.size()); ++i)
            m_orientationView->addPointMarker(m_frankfurtPoints[i].x(),
                                              m_frankfurtPoints[i].y(),
                                              m_frankfurtPoints[i].z(),
                                              kColors[i]);

        // Lateral standard view: Z is vertical on screen and world Y is true horizontal.
        m_orientationView->setStandardView(1);
    }

    publishCompositeMeshesToSceneViews();

    // Enable midline gizmo and save/continue now that Frankfurt plane is set
    if (m_midlineGizmoAct)    m_midlineGizmoAct->setEnabled(true);
    if (m_saveOrientationAct) m_saveOrientationAct->setEnabled(true);
    if (m_exportOrientedAct)  m_exportOrientedAct->setEnabled(true);
    updateFrankfurtPointStatus();

    LoggerCore::instance().logCustom(QStringLiteral("FRANKFORT"),
        QStringLiteral("Alignment applied: level=%1 deg, yaw=%2 deg, axis=(%3,%4,%5)")
            .arg(angleDeg, 0, 'f', 2)
            .arg(yawDeg, 0, 'f', 2)
            .arg(axis.x(), 0, 'f', 4).arg(axis.y(), 0, 'f', 4).arg(axis.z(), 0, 'f', 4));
    statusBar()->showMessage(tr("Frankfort alineado. Ajuste linea media con el gizmo o guarde la orientacion."));
}

// ─────────────────────────────────────────────────────────────────────────────
// LE FORT I — OSTEOTOMY WORKSPACE
// ─────────────────────────────────────────────────────────────────────────────

QWidget* MainWindow::buildOsteotomyWizardPanel(QWidget* parent)
{
    auto* panel = new QWidget(parent);
    panel->setFixedWidth(340);
    panel->setStyleSheet(
        "QWidget { background:#1f1f21; color:#f5f5f7; font-size:11px; }"
        "QLabel#WizardHeader { background:#242426; color:#f5f5f7; font-weight:700; padding:9px; border-bottom:1px solid #2c2c2e; }"
        "QLabel#WizardSection { color:#0a84ff; font-weight:700; padding:6px 2px; }"
        "QPushButton { background:#2c2c2e; border:1px solid #3a3a3c; border-radius:10px; color:#f5f5f7; padding:6px 10px; }"
        "QPushButton:hover { border-color:#5a5a5c; background:#3a3a3c; }"
        "QPushButton:checked { background:#0a84ff; border:1px solid #0a84ff; color:#ffffff; font-weight:700; }"
        "QPushButton:disabled { color:#636366; background:#242426; border-color:#2c2c2e; }"
        "QGroupBox { border:1px solid #2c2c2e; border-radius:12px; margin-top:8px; padding-top:8px; color:#f5f5f7; background:#242426; }"
        "QGroupBox::title { subcontrol-origin: margin; left:8px; padding:0 4px; color:#0a84ff; }"
        "QTableWidget { background:#1f1f21; color:#f5f5f7; border:1px solid #2c2c2e; gridline-color:#2c2c2e; }"
        "QHeaderView::section { background:#2c2c2e; color:#f5f5f7; border:0; padding:4px; font-weight:bold; border-bottom:1px solid #3a3a3c; }"
        "QDoubleSpinBox { background:#2c2c2e; color:#f5f5f7; border:1px solid #3a3a3c; padding:4px; border-radius:10px; }");

    auto* root = new QVBoxLayout(panel);
    root->setContentsMargins(3, 3, 3, 3);
    root->setSpacing(6);

    auto* header = new QLabel(tr("Planear osteotomia"), panel);
    header->setObjectName(QStringLiteral("WizardHeader"));
    header->setAlignment(Qt::AlignCenter);
    root->addWidget(header);

    auto* progress = new QGridLayout();
    progress->setContentsMargins(4, 6, 4, 4);
    progress->setHorizontalSpacing(8);
    static const QStringList stepNames = {
        tr("Tipo de\nosteotomia"),
        tr("Hueso"),
        tr("Puntos\nanatomicos"),
        tr("Ajuste\ndel corte"),
        tr("Finalizar")
    };
    for (int i = 0; i < 5; ++i) {
        m_leFortStepDots[i] = new QLabel(QStringLiteral("●"), panel);
        m_leFortStepDots[i]->setAlignment(Qt::AlignCenter);
        m_leFortStepDots[i]->setMinimumHeight(22);
        m_leFortStepTexts[i] = new QLabel(stepNames[i], panel);
        m_leFortStepTexts[i]->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
        m_leFortStepTexts[i]->setWordWrap(true);
        progress->addWidget(m_leFortStepDots[i], 0, i);
        progress->addWidget(m_leFortStepTexts[i], 1, i);
    }
    root->addLayout(progress);

    auto* title = new QLabel(tr("Le Fort I"), panel);
    title->setObjectName(QStringLiteral("WizardSection"));
    root->addWidget(title);

    m_leFortWizardStack = new QStackedWidget(panel);
    root->addWidget(m_leFortWizardStack, 1);

    auto makePage = [&]() {
        auto* page = new QWidget(m_leFortWizardStack);
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(8);
        return std::pair<QWidget*, QVBoxLayout*>(page, layout);
    };

    {
        auto [page, layout] = makePage();
        layout->addWidget(new QLabel(tr("Tipo de osteotomia:"), page));
        m_leFortSelectTypeBtn = new QPushButton(tr("Le Fort I"), page);
        m_leFortSelectTypeBtn->setCheckable(true);
        m_leFortSelectTypeBtn->setChecked(true);
        m_leFortSelectTypeBtn->setMinimumHeight(34);
        connect(m_leFortSelectTypeBtn, &QPushButton::clicked, this, [this] {
            setLeFortWizardStep(1);
        });
        layout->addWidget(m_leFortSelectTypeBtn);
        layout->addStretch(1);
        m_leFortWizardStack->addWidget(page);
    }

    {
        auto [page, layout] = makePage();
        layout->addWidget(new QLabel(tr("Hueso a cortar:"), page));
        m_leFortSelectBoneBtn = new QPushButton(tr("Maxilar / compuesto superior"), page);
        m_leFortSelectBoneBtn->setCheckable(true);
        m_leFortSelectBoneBtn->setChecked(true);
        connect(m_leFortSelectBoneBtn, &QPushButton::clicked, this, [this] {
            m_leFortTargetLabel = kUpperCompositeLabel;
            if (m_leFortTargMaxAct) m_leFortTargMaxAct->setChecked(true);
            setLeFortWizardStep(2);
        });
        layout->addWidget(m_leFortSelectBoneBtn);
        auto* hint = new QLabel(tr("Le Fort I usa el compuesto maxilar como hueso activo."), page);
        hint->setWordWrap(true);
        hint->setStyleSheet("color:#98989d;");
        layout->addWidget(hint);
        layout->addStretch(1);
        m_leFortWizardStack->addWidget(page);
    }

    {
        auto [page, layout] = makePage();
        layout->addWidget(new QLabel(tr("Punto a indicar:"), page));
        m_leFortPointPrompt = new QLabel(page);
        m_leFortPointPrompt->setMinimumHeight(48);
        m_leFortPointPrompt->setFrameShape(QFrame::StyledPanel);
        m_leFortPointPrompt->setStyleSheet("background:#242426; color:#f5f5f7; padding:8px; border:1px solid #3a3a3c; border-radius:10px;");
        layout->addWidget(m_leFortPointPrompt);

        static const QStringList pointNames = {
            tr("Point 1 - Piriforme Der"),
            tr("Point 2 - Piriforme Izq"),
            tr("Point 3 - Pilax Der"),
            tr("Point 4 - Pilax Izq")
        };
        for (int i = 0; i < 4; ++i) {
            auto* btn = new QPushButton(pointNames[i], page);
            btn->setCheckable(true);
            connect(btn, &QPushButton::clicked, this, [this, i] {
                startLeFortPointCapture(i);
            });
            m_leFortPointButtons[i] = btn;
            layout->addWidget(btn);
        }

        auto* preview = new QLabel(tr("Marque los cuatro puntos en una vista frontal."), page);
        preview->setAlignment(Qt::AlignCenter);
        preview->setWordWrap(true);
        preview->setMinimumHeight(95);
        preview->setStyleSheet("background:#1f3b57; border:1px solid #0a84ff; color:#f5f5f7; border-radius:10px; padding:8px;");
        layout->addWidget(preview);
        layout->addStretch(1);
        m_leFortWizardStack->addWidget(page);
    }

    {
        auto [page, layout] = makePage();
        layout->addWidget(new QLabel(tr("Ajuste interactivo"), page));
        auto* toolRow = new QHBoxLayout();
        m_leFortTranslateBtn = new QPushButton(tr("Trasladar"), page);
        m_leFortRotateBtn = new QPushButton(tr("Rotar"), page);
        m_leFortResizeBtn = new QPushButton(tr("Escalar"), page);
        for (auto* btn : {m_leFortTranslateBtn, m_leFortRotateBtn, m_leFortResizeBtn}) {
            connect(btn, &QPushButton::clicked, this, [this] {
                if (m_leFortAdjustPlaneAct && m_leFortAdjustPlaneAct->isEnabled())
                    m_leFortAdjustPlaneAct->trigger();
            });
            toolRow->addWidget(btn);
        }
        layout->addLayout(toolRow);

        auto* formBox = new QGroupBox(tr("Propiedades"), page);
        auto* form = new QFormLayout(formBox);
        form->setContentsMargins(12, 10, 12, 10);
        auto makeSpin = [&](double min, double max, double value) {
            auto* spin = new QDoubleSpinBox(formBox);
            spin->setDecimals(1);
            spin->setRange(min, max);
            spin->setSingleStep(1.0);
            spin->setSuffix(tr(" mm"));
            spin->setValue(value);
            spin->setMinimumWidth(105);
            connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged),
                    this, [this](double) { applyLeFortGuideSpinValues(); });
            return spin;
        };
        m_leFortWidthSpin = makeSpin(20.0, 240.0, m_leFortGuideWidthMm);
        m_leFortThicknessSpin = makeSpin(0.2, 5.0, m_leFortCutThicknessMm);
        m_leFortExtRightSpin = makeSpin(0.0, 100.0, m_leFortGuideExtensionRightMm);
        m_leFortExtLeftSpin = makeSpin(0.0, 100.0, m_leFortGuideExtensionLeftMm);
        form->addRow(tr("Ancho:"), m_leFortWidthSpin);
        form->addRow(tr("Grosor:"), m_leFortThicknessSpin);
        form->addRow(tr("Extension derecha:"), m_leFortExtRightSpin);
        form->addRow(tr("Extension izquierda:"), m_leFortExtLeftSpin);
        layout->addWidget(formBox);
        layout->addStretch(1);
        m_leFortWizardStack->addWidget(page);
    }

    {
        auto [page, layout] = makePage();
        layout->addWidget(new QLabel(tr("Objetos creados:"), page));
        m_leFortFinalizeTable = new QTableWidget(0, 2, page);
        m_leFortFinalizeTable->setHorizontalHeaderLabels({tr("Nombre"), tr("Visible")});
        m_leFortFinalizeTable->horizontalHeader()->setStretchLastSection(true);
        m_leFortFinalizeTable->verticalHeader()->setVisible(false);
        m_leFortFinalizeTable->setSelectionMode(QAbstractItemView::NoSelection);
        m_leFortFinalizeTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        layout->addWidget(m_leFortFinalizeTable, 1);

        auto* nextBox = new QGroupBox(tr("Siguiente paso:"), page);
        auto* nextLayout = new QVBoxLayout(nextBox);
        m_leFortNextAnotherOsteotomy = new QRadioButton(tr("Crear otra osteotomia"), nextBox);
        nextLayout->addWidget(m_leFortNextAnotherOsteotomy);
        m_leFortNextOcclusion = new QRadioButton(tr("Registro oclusal"), nextBox);
        m_leFortNextOcclusion->setChecked(true);
        nextLayout->addWidget(m_leFortNextOcclusion);
        m_leFortNextReposition = new QRadioButton(tr("Reposicion"), nextBox);
        nextLayout->addWidget(m_leFortNextReposition);
        layout->addWidget(nextBox);
        m_leFortWizardStack->addWidget(page);
    }

    m_leFortWizardStatus = new QLabel(panel);
    m_leFortWizardStatus->setWordWrap(true);
    m_leFortWizardStatus->setMinimumHeight(42);
    m_leFortWizardStatus->setStyleSheet("color:#d1d1d6; padding:4px;");
    root->addWidget(m_leFortWizardStatus);

    auto* nav = new QHBoxLayout();
    nav->addStretch(1);
    m_leFortBackBtn = new QPushButton(tr("Atras"), panel);
    m_leFortNextBtn = new QPushButton(tr("Siguiente"), panel);
    nav->addWidget(m_leFortBackBtn);
    nav->addWidget(m_leFortNextBtn);
    root->addLayout(nav);

    connect(m_leFortBackBtn, &QPushButton::clicked, this, [this] {
        setLeFortWizardStep(std::max(0, m_leFortWizardStep - 1));
    });
    connect(m_leFortNextBtn, &QPushButton::clicked, this, [this] {
        if (m_leFortWizardStep == 0) {
            setLeFortWizardStep(1);
        } else if (m_leFortWizardStep == 1) {
            m_leFortTargetLabel = kUpperCompositeLabel;
            if (m_leFortTargMaxAct) m_leFortTargMaxAct->setChecked(true);
            setLeFortWizardStep(2);
        } else if (m_leFortWizardStep == 2) {
            setLeFortWizardStep(3);
        } else if (m_leFortWizardStep == 3) {
            if (m_osteotomyView && m_osteotomyView->hasGizmo())
                m_osteotomyView->stopGizmo();
            executeLeFortSplit();
            if (m_leFortCranialMesh && m_leFortSegmentMesh)
                setLeFortWizardStep(4);
        } else {
            if (m_leFortNextAnotherOsteotomy && m_leFortNextAnotherOsteotomy->isChecked()) {
                setLeFortWizardStep(0);
                statusBar()->showMessage(tr("Le Fort I finalizado. Puede crear otra osteotomia."));
            } else if (m_leFortNextOcclusion && m_leFortNextOcclusion->isChecked()) {
                setBiteRegistrationWorkspace(true);
            } else {
                setRepositionWorkspace(true);
            }
        }
    });

    return panel;
}

void MainWindow::setOsteotomyWorkspace(bool enabled)
{
    if (m_viewModeStack && enabled) {
        m_viewModeStack->setCurrentIndex(3);
        // Let Qt process the show event so the QVTKOpenGLNativeWidget initialises
        // its GL context before we attempt the first render.
        QApplication::processEvents();
    }

    if (!enabled || !m_osteotomyView) return;
    m_osteotomyView->show();
    m_osteotomyView->setFullScreenActive(false);

    // Reset transient picking state.  Do not clear already-created surgical
    // segments here: users can return to OSTEOTOMIA after saving/loading or
    // after toggling object visibility, and the Project Manager state should
    // keep driving what is displayed.
    m_leFortPoints.clear();
    m_leFortPointSet.fill(false);
    m_leFortCapturingIdx = -1;
    m_leFortWizardStep = 0;
    m_leFortPlaneVisualMesh = nullptr;
    m_leFortTargetLabel  = kUpperCompositeLabel;
    if (m_leFortTargMaxAct)  m_leFortTargMaxAct->setChecked(true);
    // Uncheck landmark buttons
    if (m_leFortPirDACt)   m_leFortPirDACt->setChecked(false);
    if (m_leFortPirIAct)   m_leFortPirIAct->setChecked(false);
    if (m_leFortPilaxDAct) m_leFortPilaxDAct->setChecked(false);
    if (m_leFortPilaxIAct) m_leFortPilaxIAct->setChecked(false);
    if (m_bssoRamusRightAct) m_bssoRamusRightAct->setChecked(false);
    if (m_bssoBodyRightAct)  m_bssoBodyRightAct->setChecked(false);
    if (m_bssoPlaneRightAct) m_bssoPlaneRightAct->setChecked(false);
    if (m_bssoRamusLeftAct)  m_bssoRamusLeftAct->setChecked(false);
    if (m_bssoBodyLeftAct)   m_bssoBodyLeftAct->setChecked(false);
    if (m_bssoPlaneLeftAct)  m_bssoPlaneLeftAct->setChecked(false);
    if (m_genioApicalRightAct) m_genioApicalRightAct->setChecked(false);
    if (m_genioBasalRightAct)  m_genioBasalRightAct->setChecked(false);
    if (m_genioApicalLeftAct)  m_genioApicalLeftAct->setChecked(false);
    if (m_genioBasalLeftAct)   m_genioBasalLeftAct->setChecked(false);

    auto sceneMesh = [&](int label) -> vtkSmartPointer<vtkPolyData> {
        if (!m_mesh3DView) return nullptr;
        if (auto mesh = m_mesh3DView->meshData(objectActorKey(label))) return mesh;
        if (auto mesh = m_mesh3DView->meshData(label)) return mesh;
        return nullptr;
    };
    auto deepCopyMesh = [](vtkPolyData* src) -> vtkSmartPointer<vtkPolyData> {
        if (!src || src->GetNumberOfPoints() <= 0) return nullptr;
        auto out = vtkSmartPointer<vtkPolyData>::New();
        out->DeepCopy(src);
        return out;
    };
    auto restorePersistentMesh = [&](vtkSmartPointer<vtkPolyData>& dst, int label) {
        if (auto mesh = sceneMesh(label)) {
            dst = deepCopyMesh(mesh);
        } else if (!objectEntryExists(label)) {
            dst = nullptr;
        }
    };

    if (!m_upperCompositeMesh)
        m_upperCompositeMesh = deepCopyMesh(sceneMesh(kUpperCompositeLabel));
    if (!m_lowerCompositeMesh)
        m_lowerCompositeMesh = deepCopyMesh(sceneMesh(kLowerCompositeLabel));
    restorePersistentMesh(m_leFortCranialMesh, kLeFortCranialLabel);
    restorePersistentMesh(m_leFortSegmentMesh, kLeFortSegLabel);
    restorePersistentMesh(m_bssoDistalMesh, kBssoDistalLabel);
    restorePersistentMesh(m_bssoProximalMesh, kBssoProximalLabel);
    restorePersistentMesh(m_bssoRightProximalMesh, kBssoProximalRightLabel);
    restorePersistentMesh(m_bssoLeftProximalMesh, kBssoProximalLeftLabel);
    restorePersistentMesh(m_genioBodyMesh, kGenioBodyLabel);
    restorePersistentMesh(m_genioSegmentMesh, kGenioSegmentLabel);

    m_osteotomyView->clearMeshes();
    m_osteotomyView->clearPointMarkers();

    const bool hasUpper = (m_upperCompositeMesh != nullptr &&
                           m_upperCompositeMesh->GetNumberOfPoints() > 0);
    const bool hasLower = (m_lowerCompositeMesh != nullptr &&
                           m_lowerCompositeMesh->GetNumberOfPoints() > 0);
    const bool hasBilateralBssoSegments =
        m_bssoDistalMesh && m_bssoDistalMesh->GetNumberOfPoints() > 0 &&
        m_bssoRightProximalMesh && m_bssoRightProximalMesh->GetNumberOfPoints() > 0 &&
        m_bssoLeftProximalMesh && m_bssoLeftProximalMesh->GetNumberOfPoints() > 0;
    if (hasBilateralBssoSegments) {
        m_leFortTargetLabel = kLowerCompositeLabel;
        if (m_leFortTargMandAct) m_leFortTargMandAct->setChecked(true);
    }

    auto addOsteotomyObject = [&](int label, vtkPolyData* mesh) -> bool {
        if (!mesh || mesh->GetNumberOfPoints() <= 0) return false;
        const int actorKey = objectActorKey(label);
        m_osteotomyView->addMesh(actorKey, mesh, meshLabelName(label));
        m_osteotomyView->setMeshColor(actorKey, objectColorForLabel(label));
        m_osteotomyView->setMeshVisible(actorKey, objectEntryVisible(label));
        m_osteotomyView->setMeshOpacity(actorKey, 1.0);
        return true;
    };

    bool hasPersistentSegment = false;
    hasPersistentSegment |= addOsteotomyObject(kLeFortCranialLabel, m_leFortCranialMesh);
    hasPersistentSegment |= addOsteotomyObject(kLeFortSegLabel, m_leFortSegmentMesh);
    const bool hasGenioSegments =
        m_genioBodyMesh && m_genioBodyMesh->GetNumberOfPoints() > 0 &&
        m_genioSegmentMesh && m_genioSegmentMesh->GetNumberOfPoints() > 0;
    if (hasGenioSegments) {
        hasPersistentSegment |= addOsteotomyObject(kGenioBodyLabel, m_genioBodyMesh);
        hasPersistentSegment |= addOsteotomyObject(kGenioSegmentLabel, m_genioSegmentMesh);
    } else {
        hasPersistentSegment |= addOsteotomyObject(kBssoDistalLabel, m_bssoDistalMesh);
    }
    hasPersistentSegment |= addOsteotomyObject(kBssoProximalRightLabel, m_bssoRightProximalMesh);
    hasPersistentSegment |= addOsteotomyObject(kBssoProximalLeftLabel, m_bssoLeftProximalMesh);
    if (!m_bssoRightProximalMesh && !m_bssoLeftProximalMesh)
        hasPersistentSegment |= addOsteotomyObject(kBssoProximalLabel, m_bssoProximalMesh);

    if (hasUpper && !hasPersistentSegment) {
        const int k = objectActorKey(kUpperCompositeLabel);
        m_osteotomyView->addMesh(k, m_upperCompositeMesh, meshLabelName(kUpperCompositeLabel));
        m_osteotomyView->setMeshColor(k, objectColorForLabel(kUpperCompositeLabel));
        m_osteotomyView->setMeshVisible(k, objectEntryVisible(kUpperCompositeLabel));
        m_osteotomyView->setMeshOpacity(k, 1.0);
    }
    if (hasLower) {
        const int k = objectActorKey(kLowerCompositeLabel);
        m_osteotomyView->addMesh(k, m_lowerCompositeMesh, meshLabelName(kLowerCompositeLabel));
        m_osteotomyView->setMeshColor(k, objectColorForLabel(kLowerCompositeLabel));
        m_osteotomyView->setMeshVisible(k, objectEntryVisible(kLowerCompositeLabel));
        m_osteotomyView->setMeshOpacity(k, 1.0);
    }

    const bool hasMesh = hasUpper || hasLower || hasPersistentSegment;
    if (!hasMesh) {
        statusBar()->showMessage(tr("Osteotomia: no hay modelos compuestos visibles para cargar."));
        m_osteotomyView->render();
    }
    if (!hasUpper && hasLower) {
        m_leFortTargetLabel = kLowerCompositeLabel;
        if (m_leFortTargMandAct) m_leFortTargMandAct->setChecked(true);
    }
    if (m_leFortPirDACt)    m_leFortPirDACt->setEnabled(hasMesh);
    if (m_leFortPirIAct)    m_leFortPirIAct->setEnabled(hasMesh);
    if (m_leFortPilaxDAct)  m_leFortPilaxDAct->setEnabled(hasMesh);
    if (m_leFortPilaxIAct)  m_leFortPilaxIAct->setEnabled(hasMesh);
    if (m_leFortTargMaxAct) m_leFortTargMaxAct->setEnabled(hasUpper);
    if (m_leFortTargMandAct)m_leFortTargMandAct->setEnabled(hasLower);
    if (m_leFortAdjustPlaneAct)  m_leFortAdjustPlaneAct->setEnabled(false);
    if (m_leFortGuidePropsAct)   m_leFortGuidePropsAct->setEnabled(false);
    if (m_leFortAcceptPlaneAct)  m_leFortAcceptPlaneAct->setEnabled(false);
    if (m_leFortSplitAct)        m_leFortSplitAct->setEnabled(false);
    if (m_leFortExportAct)       m_leFortExportAct->setEnabled(false);
    // Reset stored plane state (zero normal = "not yet defined")
    m_leFortPlaneNormal = QVector3D(0, 0, 0);
    m_leFortPlaneCenter = QVector3D(0, 0, 0);
    m_leFortPlaneRadius = 0.0;
    m_leFortGuideAxis = QVector3D(0, 0, 0);
    m_leFortGuideCoreLengthMm = 80.0;
    m_bssoPoints.clear();
    m_bssoPointSet.fill(false);
    m_bssoCapturingIdx = -1;
    m_bssoGuideVisualMesh = nullptr;
    m_bssoGuideReady = false;
    m_bssoActiveLeftSide = false;
    m_bssoSideSplitDone.fill(false);
    m_genioPoints.clear();
    m_genioPointSet.fill(false);
    m_genioCapturingIdx = -1;
    m_genioPlaneVisualMesh = nullptr;
    m_genioPlaneNormal = QVector3D(0, 0, 0);
    m_genioPlaneCenter = QVector3D(0, 0, 0);
    m_genioGuideAxis = QVector3D(0, 0, 0);
    m_genioGuideDepthAxis = QVector3D(0, 0, 0);
    if (m_bssoAutoGuideAct)    m_bssoAutoGuideAct->setEnabled(hasLower);
    if (m_bssoLeftGuideAct)    m_bssoLeftGuideAct->setEnabled(hasLower);
    if (m_bssoRamusRightAct)   m_bssoRamusRightAct->setEnabled(hasLower);
    if (m_bssoBodyRightAct)    m_bssoBodyRightAct->setEnabled(hasLower);
    if (m_bssoPlaneRightAct)   m_bssoPlaneRightAct->setEnabled(hasLower);
    if (m_bssoRamusLeftAct)    m_bssoRamusLeftAct->setEnabled(hasLower);
    if (m_bssoBodyLeftAct)     m_bssoBodyLeftAct->setEnabled(hasLower);
    if (m_bssoPlaneLeftAct)    m_bssoPlaneLeftAct->setEnabled(hasLower);
    if (m_bssoAdjustGuideAct)  m_bssoAdjustGuideAct->setEnabled(false);
    if (m_bssoGuidePropsAct)   m_bssoGuidePropsAct->setEnabled(false);
    if (m_bssoAcceptGuideAct)  m_bssoAcceptGuideAct->setEnabled(false);
    if (m_genioApicalRightAct) m_genioApicalRightAct->setEnabled(hasLower);
    if (m_genioBasalRightAct)  m_genioBasalRightAct->setEnabled(hasLower);
    if (m_genioApicalLeftAct)  m_genioApicalLeftAct->setEnabled(hasLower);
    if (m_genioBasalLeftAct)   m_genioBasalLeftAct->setEnabled(hasLower);
    if (m_genioAdjustPlaneAct) m_genioAdjustPlaneAct->setEnabled(false);
    if (m_genioAcceptPlaneAct) m_genioAcceptPlaneAct->setEnabled(false);

    updateLeFortPointStatus();
    updateBssoPointStatus();
    updateGenioPointStatus();
    updateOsteotomyWorkflowUi();
    setLeFortWizardStep(0);
    startOsteotomyWizard();
    syncVisibilityPanelToAllViews();

    // resetCamera fits all actors, then setStandardView adjusts orientation.
    // QVTKOpenGLNativeWidget initialises its GL context lazily on the first
    // paint event.  A single processEvents() round is not always sufficient
    // (the widget may not have received its first expose yet).  We therefore
    // do an immediate attempt and then schedule a deferred render ~100 ms later
    // so that by then the context is guaranteed to be live.
    m_osteotomyView->resetCamera();
    QApplication::processEvents();
    m_osteotomyView->setStandardView(0);
    m_osteotomyView->render();

    QTimer::singleShot(120, this, [this] {
        if (m_osteotomyView) {
            m_osteotomyView->resetCamera();
            m_osteotomyView->setStandardView(0);
            syncVisibilityPanelToAllViews();
            m_osteotomyView->render();
        }
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// Receives a picked point from m_osteotomyView, stores it at the active index,
// adds a colour-coded sphere marker, and updates the plane disc.
void MainWindow::setLeFortWizardStep(int step)
{
    m_leFortWizardStep = std::clamp(step, 0, 4);
    if (m_leFortWizardStack)
        m_leFortWizardStack->setCurrentIndex(m_leFortWizardStep);

    if (m_osteotomyView && (m_leFortWizardStep == 2 || m_leFortWizardStep == 4))
        m_osteotomyView->setStandardView(0);

    if (m_leFortWizardStep == 4)
        refreshLeFortFinalizeTable();

    updateLeFortWizardUi();

    if (m_leFortWizardStep == 2) {
        const int next = static_cast<int>(
            std::find(m_leFortPointSet.begin(), m_leFortPointSet.end(), false) -
            m_leFortPointSet.begin());
        if (next >= 0 && next < 4 && m_leFortCapturingIdx < 0)
            startLeFortPointCapture(next);
    }
}

void MainWindow::updateLeFortWizardUi()
{
    const int pointCount = static_cast<int>(
        std::count(m_leFortPointSet.begin(), m_leFortPointSet.end(), true));
    const bool hasUpper = m_upperCompositeMesh && m_upperCompositeMesh->GetNumberOfPoints() > 0;
    const bool hasPlane = !m_leFortPlaneNormal.isNull();
    const bool hasSplit = m_leFortCranialMesh && m_leFortSegmentMesh;

    const bool complete[5] = {
        m_leFortWizardStep > 0,
        m_leFortWizardStep > 1 && hasUpper,
        pointCount >= 4,
        m_leFortWizardStep > 3 || hasSplit,
        hasSplit
    };

    for (int i = 0; i < 5; ++i) {
        if (!m_leFortStepDots[i] || !m_leFortStepTexts[i]) continue;
        const bool active = (i == m_leFortWizardStep);
        const QString color = complete[i] ? QStringLiteral("#34c759")
                            : active      ? QStringLiteral("#0a84ff")
                                          : QStringLiteral("#98989d");
        m_leFortStepDots[i]->setText(complete[i] ? QStringLiteral("✓") : QStringLiteral("●"));
        m_leFortStepDots[i]->setStyleSheet(
            QStringLiteral("font-size:24px; font-weight:700; color:%1;").arg(color));
        m_leFortStepTexts[i]->setStyleSheet(
            QStringLiteral("font-size:10px; color:%1; font-weight:%2;")
                .arg(active ? QStringLiteral("#0a84ff") : (complete[i] ? QStringLiteral("#f5f5f7") : QStringLiteral("#98989d")))
                .arg(active ? QStringLiteral("700") : QStringLiteral("400")));
    }

    if (m_leFortSelectTypeBtn)
        m_leFortSelectTypeBtn->setChecked(true);
    if (m_leFortSelectBoneBtn) {
        m_leFortSelectBoneBtn->setChecked(m_leFortTargetLabel == kUpperCompositeLabel);
        m_leFortSelectBoneBtn->setEnabled(hasUpper);
    }

    const QStringList pointNames = {
        tr("LeFort 1 - Point 1 of 4"),
        tr("LeFort 1 - Point 2 of 4"),
        tr("LeFort 1 - Point 3 of 4"),
        tr("LeFort 1 - Point 4 of 4")
    };
    int firstMissing = -1;
    for (int i = 0; i < 4; ++i) {
        if (!m_leFortPointSet[i]) { firstMissing = i; break; }
    }
    if (m_leFortPointPrompt) {
        m_leFortPointPrompt->setText(firstMissing < 0
            ? tr("Point indication complete.")
            : pointNames[firstMissing]);
    }
    for (int i = 0; i < 4; ++i) {
        auto* btn = m_leFortPointButtons[i];
        if (!btn) continue;
        const bool placed = m_leFortPointSet[i];
        const bool active = (m_leFortCapturingIdx == i);
        btn->setChecked(placed || active);
        btn->setEnabled(hasUpper && (placed || i == firstMissing));
        btn->setStyleSheet(active
            ? QStringLiteral("background:#1f3b57; border:1px solid #0a84ff; color:#ffffff; font-weight:700;")
            : placed
                ? QStringLiteral("background:#24342a; border:1px solid #34c759; color:#b8f2c2; font-weight:700;")
                : QString());
    }

    if (m_leFortWidthSpin) {
        QSignalBlocker b(m_leFortWidthSpin);
        m_leFortWidthSpin->setValue(m_leFortGuideWidthMm);
    }
    if (m_leFortThicknessSpin) {
        QSignalBlocker b(m_leFortThicknessSpin);
        m_leFortThicknessSpin->setValue(m_leFortCutThicknessMm);
    }
    if (m_leFortExtRightSpin) {
        QSignalBlocker b(m_leFortExtRightSpin);
        m_leFortExtRightSpin->setValue(m_leFortGuideExtensionRightMm);
    }
    if (m_leFortExtLeftSpin) {
        QSignalBlocker b(m_leFortExtLeftSpin);
        m_leFortExtLeftSpin->setValue(m_leFortGuideExtensionLeftMm);
    }

    const bool canAdjust = hasPlane;
    if (m_leFortTranslateBtn) m_leFortTranslateBtn->setEnabled(canAdjust);
    if (m_leFortRotateBtn)    m_leFortRotateBtn->setEnabled(canAdjust);
    if (m_leFortResizeBtn)    m_leFortResizeBtn->setEnabled(canAdjust);

    if (m_leFortBackBtn)
        m_leFortBackBtn->setEnabled(m_leFortWizardStep > 0);
    if (m_leFortNextBtn) {
        bool canNext = true;
        QString text = tr("Siguiente");
        if (m_leFortWizardStep == 1) canNext = hasUpper;
        if (m_leFortWizardStep == 2) canNext = (pointCount >= 4 && hasPlane);
        if (m_leFortWizardStep == 3) canNext = hasPlane;
        if (m_leFortWizardStep == 4) text = tr("Finalizar");
        m_leFortNextBtn->setText(text);
        m_leFortNextBtn->setEnabled(canNext);
    }

    if (m_leFortWizardStatus) {
        QString msg;
        if (!hasUpper) {
            msg = tr("Cree primero el modelo compuesto maxilar antes de planear Le Fort I.");
        } else if (m_leFortWizardStep == 0) {
            msg = tr("Seleccione el tipo de osteotomia.");
        } else if (m_leFortWizardStep == 1) {
            msg = tr("Seleccione el maxilar como hueso activo.");
        } else if (m_leFortWizardStep == 2) {
            msg = tr("Puntos marcados: %1/4. Use la vista frontal para ubicar los landmarks.").arg(pointCount);
        } else if (m_leFortWizardStep == 3) {
            msg = tr("Ajuste la guia, grosor y extensiones. Next aplica la osteotomia.");
        } else {
            msg = tr("Revise los nuevos segmentos y continue con el siguiente paso clinico.");
        }
        m_leFortWizardStatus->setText(msg);
    }
}

void MainWindow::refreshLeFortFinalizeTable()
{
    if (!m_leFortFinalizeTable) return;
    m_leFortFinalizeTable->setRowCount(0);
    auto addRow = [&](const QString& name, const QColor& color, bool visible) {
        const int row = m_leFortFinalizeTable->rowCount();
        m_leFortFinalizeTable->insertRow(row);
        auto* nameItem = new QTableWidgetItem(name);
        nameItem->setBackground(color);
        m_leFortFinalizeTable->setItem(row, 0, nameItem);
        auto* visibleItem = new QTableWidgetItem(visible ? tr("yes") : tr("no"));
        visibleItem->setTextAlignment(Qt::AlignCenter);
        m_leFortFinalizeTable->setItem(row, 1, visibleItem);
    };
    if (m_leFortCranialMesh)
        addRow(meshLabelName(kLeFortCranialLabel), meshLabelColor(kLeFortCranialLabel), true);
    if (m_leFortSegmentMesh)
        addRow(meshLabelName(kLeFortSegLabel), meshLabelColor(kLeFortSegLabel), true);
    if (m_lowerCompositeMesh)
        addRow(meshLabelName(kLowerCompositeLabel), objectColorForLabel(kLowerCompositeLabel), true);
    m_leFortFinalizeTable->resizeColumnsToContents();
}

void MainWindow::startLeFortPointCapture(int index)
{
    if (index < 0 || index > 3 || !m_osteotomyView) return;

    m_bssoCapturingIdx = -1;
    m_leFortCapturingIdx = index;
    m_osteotomyView->setPointPickMode(true);

    QAction* const acts[4] = {
        m_leFortPirDACt, m_leFortPirIAct,
        m_leFortPilaxDAct, m_leFortPilaxIAct
    };
    for (int i = 0; i < 4; ++i) {
        if (acts[i]) acts[i]->setChecked(i == index);
    }

    if (m_leFortWizardStack && m_leFortWizardStep != 2) {
        m_leFortWizardStep = 2;
        m_leFortWizardStack->setCurrentIndex(2);
    }
    updateLeFortWizardUi();

    const QStringList prompts = {
        tr("Marque piriforme derecho en el modelo."),
        tr("Marque piriforme izquierdo en el modelo."),
        tr("Marque pilar maxilomalar derecho."),
        tr("Marque pilar maxilomalar izquierdo.")
    };
    statusBar()->showMessage(prompts[index]);
}

void MainWindow::applyLeFortGuideSpinValues()
{
    if (!m_leFortWidthSpin || !m_leFortThicknessSpin ||
        !m_leFortExtRightSpin || !m_leFortExtLeftSpin) {
        return;
    }

    m_leFortGuideWidthMm = m_leFortWidthSpin->value();
    m_leFortCutThicknessMm = m_leFortThicknessSpin->value();
    m_leFortGuideExtensionRightMm = m_leFortExtRightSpin->value();
    m_leFortGuideExtensionLeftMm = m_leFortExtLeftSpin->value();

    if (!m_leFortPlaneNormal.isNull()) {
        m_leFortPlaneVisualMesh = buildLeFortCutGuideMesh();
        rebuildLeFortPlaneFromState();
    }
}

void MainWindow::onLeFortPointPicked(int /*actorLabel*/, double x, double y, double z)
{
    if (m_genioCapturingIdx >= 0) {
        onGenioPointPicked(0, x, y, z);
        return;
    }

    if (m_bssoCapturingIdx >= 0) {
        onBssoPointPicked(0, x, y, z);
        return;
    }

    if (m_leFortCapturingIdx < 0 || m_leFortCapturingIdx > 3) return;

    while (m_leFortPoints.size() <= m_leFortCapturingIdx)
        m_leFortPoints.append(QVector3D{});
    const int placedIdx = m_leFortCapturingIdx;
    m_leFortPoints[m_leFortCapturingIdx] =
        QVector3D(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
    m_leFortPointSet[static_cast<size_t>(m_leFortCapturingIdx)] = true;

    static const QColor kColors[4] = { Qt::red, Qt::blue, QColor(0,200,80), Qt::cyan };
    if (m_osteotomyView)
        m_osteotomyView->addPointMarker(x, y, z, kColors[m_leFortCapturingIdx]);

    // Mark the corresponding button as "placed"
    const QAction* const kLeFortActs[4] = {
        m_leFortPirDACt, m_leFortPirIAct,
        m_leFortPilaxDAct, m_leFortPilaxIAct
    };
    if (auto* act = const_cast<QAction*>(kLeFortActs[m_leFortCapturingIdx]))
        act->setChecked(true);

    m_leFortCapturingIdx = -1;
    if (m_osteotomyView) m_osteotomyView->setPointPickMode(false);

    updateLeFortPointStatus();
    const int pointCount = static_cast<int>(
        std::count(m_leFortPointSet.begin(), m_leFortPointSet.end(), true));
    if (pointCount >= 4)
        updateLeFortPlaneDisc();
    updateLeFortWizardUi();

    if (m_leFortWizardStep == 2 && pointCount < 4) {
        for (int i = placedIdx + 1; i < 4; ++i) {
            if (!m_leFortPointSet[i]) {
                startLeFortPointCapture(i);
                return;
            }
        }
        for (int i = 0; i < 4; ++i) {
            if (!m_leFortPointSet[i]) {
                startLeFortPointCapture(i);
                return;
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::setBssoActiveSide(bool leftSide)
{
    m_bssoActiveLeftSide = leftSide;
    if (m_bssoAutoGuideAct) {
        const QSignalBlocker blocker(m_bssoAutoGuideAct);
        m_bssoAutoGuideAct->setChecked(!leftSide);
    }
    if (m_bssoLeftGuideAct) {
        const QSignalBlocker blocker(m_bssoLeftGuideAct);
        m_bssoLeftGuideAct->setChecked(leftSide);
    }
    updateOsteotomyWorkflowUi();
}

void MainWindow::updateOsteotomyWorkflowUi()
{
    auto showOnly = [](QAction* action, bool visible, bool enabled) {
        if (!action) return;
        action->setVisible(visible);
        action->setEnabled(visible && enabled);
        const auto objects = action->associatedObjects();
        for (QObject* object : objects) {
            if (auto* widget = qobject_cast<QWidget*>(object))
                widget->setVisible(visible);
        }
    };

    if (m_osteotomyWizard) {
        // The wizard panel drives the osteotomies; the ribbon keeps the export only.
        for (QAction* action : {m_leFortPirDACt, m_leFortPirIAct, m_leFortPilaxDAct, m_leFortPilaxIAct,
                                m_leFortAdjustPlaneAct, m_leFortGuidePropsAct, m_leFortAcceptPlaneAct, m_leFortSplitAct,
                                m_leFortTargMaxAct, m_leFortTargMandAct, m_bssoAutoGuideAct, m_bssoLeftGuideAct,
                                m_bssoRamusRightAct, m_bssoBodyRightAct, m_bssoPlaneRightAct, m_bssoRamusLeftAct,
                                m_bssoBodyLeftAct, m_bssoPlaneLeftAct, m_bssoAdjustGuideAct, m_bssoGuidePropsAct,
                                m_bssoAcceptGuideAct, m_genioApicalRightAct, m_genioBasalRightAct, m_genioApicalLeftAct,
                                m_genioBasalLeftAct, m_genioAdjustPlaneAct, m_genioAcceptPlaneAct})
            showOnly(action, false, false);
        showOnly(m_leFortExportAct, true, m_leFortCranialMesh && m_leFortSegmentMesh);
        for (QLabel* label : {m_leFortCutLabel, m_bssoStatusLabel, m_genioStatusLabel})
            if (label) label->setVisible(false);
        return;
    }

    const bool hasUpper = m_upperCompositeMesh && m_upperCompositeMesh->GetNumberOfPoints() > 0;
    const bool hasLower = m_lowerCompositeMesh && m_lowerCompositeMesh->GetNumberOfPoints() > 0;
    const bool bssoMode = m_leFortTargetLabel == kLowerCompositeLabel;
    const bool hasBilateralBssoSegments =
        m_bssoDistalMesh && m_bssoDistalMesh->GetNumberOfPoints() > 0 &&
        m_bssoRightProximalMesh && m_bssoRightProximalMesh->GetNumberOfPoints() > 0 &&
        m_bssoLeftProximalMesh && m_bssoLeftProximalMesh->GetNumberOfPoints() > 0;
    const bool hasGenioSegments =
        m_genioBodyMesh && m_genioBodyMesh->GetNumberOfPoints() > 0 &&
        m_genioSegmentMesh && m_genioSegmentMesh->GetNumberOfPoints() > 0;
    const bool genioMode = bssoMode && (hasBilateralBssoSegments || hasGenioSegments);
    const int leFortPoints = static_cast<int>(
        std::count(m_leFortPointSet.begin(), m_leFortPointSet.end(), true));
    const bool leFortPlaneReady = !m_leFortPlaneNormal.isNull();
    const int genioPoints = static_cast<int>(
        std::count(m_genioPointSet.begin(), m_genioPointSet.end(), true));
    const bool genioPlaneReady = !m_genioPlaneNormal.isNull() &&
        m_genioPlaneVisualMesh && m_genioPlaneVisualMesh->GetNumberOfPoints() > 0;
    const int sideBase = m_bssoActiveLeftSide ? 3 : 0;
    int bssoSidePoints = 0;
    for (int i = 0; i < 3; ++i) {
        if (m_bssoPointSet[static_cast<size_t>(sideBase + i)])
            ++bssoSidePoints;
    }
    const bool bssoGuideReady =
        m_bssoGuideVisualMesh && m_bssoGuideVisualMesh->GetNumberOfPoints() > 0;
    const QVector<QAction*> osteotomyActions = {
        m_leFortPirDACt, m_leFortPirIAct, m_leFortPilaxDAct, m_leFortPilaxIAct,
        m_leFortAdjustPlaneAct, m_leFortGuidePropsAct, m_leFortAcceptPlaneAct,
        m_leFortSplitAct, m_leFortExportAct,
        m_bssoRamusRightAct, m_bssoBodyRightAct, m_bssoPlaneRightAct,
        m_bssoRamusLeftAct, m_bssoBodyLeftAct, m_bssoPlaneLeftAct,
        m_bssoAdjustGuideAct, m_bssoGuidePropsAct, m_bssoAcceptGuideAct,
        m_genioApicalRightAct, m_genioBasalRightAct,
        m_genioApicalLeftAct, m_genioBasalLeftAct,
        m_genioAdjustPlaneAct, m_genioAcceptPlaneAct
    };
    clearGuidedActionStates(osteotomyActions);

    if (m_leFortCutLabel)  m_leFortCutLabel->setVisible(!bssoMode);
    if (m_bssoStatusLabel) m_bssoStatusLabel->setVisible(bssoMode && !genioMode);
    if (m_genioStatusLabel) m_genioStatusLabel->setVisible(genioMode);

    showOnly(m_leFortPirDACt,       !bssoMode, hasUpper);
    showOnly(m_leFortPirIAct,       !bssoMode, hasUpper);
    showOnly(m_leFortPilaxDAct,     !bssoMode, hasUpper);
    showOnly(m_leFortPilaxIAct,     !bssoMode, hasUpper);
    showOnly(m_leFortAdjustPlaneAct,!bssoMode, hasUpper && leFortPlaneReady);
    showOnly(m_leFortGuidePropsAct, !bssoMode, hasUpper && leFortPlaneReady);
    showOnly(m_leFortAcceptPlaneAct,!bssoMode, m_leFortAcceptPlaneAct && m_leFortAcceptPlaneAct->isEnabled());
    showOnly(m_leFortExportAct,     !bssoMode, m_leFortCranialMesh && m_leFortSegmentMesh);

    // The maxilla/mandible target actions are used as internal workflow state.
    // The visible ribbon stays focused on the current surgical step.
    showOnly(m_leFortTargMaxAct,    false, false);
    showOnly(m_leFortTargMandAct,   false, false);

    showOnly(m_bssoAutoGuideAct,    false, false);
    showOnly(m_bssoLeftGuideAct,    false, false);
    showOnly(m_bssoRamusRightAct,   bssoMode && !genioMode && !m_bssoActiveLeftSide, hasLower);
    showOnly(m_bssoBodyRightAct,    bssoMode && !genioMode && !m_bssoActiveLeftSide, hasLower);
    showOnly(m_bssoPlaneRightAct,   bssoMode && !genioMode && !m_bssoActiveLeftSide, hasLower);
    showOnly(m_bssoRamusLeftAct,    bssoMode && !genioMode &&  m_bssoActiveLeftSide, hasLower);
    showOnly(m_bssoBodyLeftAct,     bssoMode && !genioMode &&  m_bssoActiveLeftSide, hasLower);
    showOnly(m_bssoPlaneLeftAct,    bssoMode && !genioMode &&  m_bssoActiveLeftSide, hasLower);
    showOnly(m_bssoAdjustGuideAct,  bssoMode && !genioMode, hasLower && bssoGuideReady && bssoSidePoints >= 3);
    showOnly(m_bssoGuidePropsAct,   bssoMode && !genioMode, hasLower && bssoGuideReady && bssoSidePoints >= 3);
    showOnly(m_bssoAcceptGuideAct,  bssoMode && !genioMode, m_bssoAcceptGuideAct && m_bssoAcceptGuideAct->isEnabled());

    showOnly(m_genioApicalRightAct, genioMode, hasLower);
    showOnly(m_genioBasalRightAct,  genioMode, hasLower);
    showOnly(m_genioApicalLeftAct,  genioMode, hasLower);
    showOnly(m_genioBasalLeftAct,   genioMode, hasLower);
    showOnly(m_genioAdjustPlaneAct, genioMode, hasLower && genioPlaneReady && genioPoints >= 4);
    showOnly(m_genioAcceptPlaneAct, genioMode, m_genioAcceptPlaneAct && m_genioAcceptPlaneAct->isEnabled());

    if (m_leFortSplitAct) {
        if (genioMode) {
            showOnly(m_leFortSplitAct, true, hasLower && genioPlaneReady && genioPoints >= 4);
            m_leFortSplitAct->setText(tr("Dividir\nMentón"));
            m_leFortSplitAct->setToolTip(tr("Dividir segmento de menton usando el plano definido por los 4 puntos."));
        } else {
            showOnly(m_leFortSplitAct, true, true);
            m_leFortSplitAct->setText(tr("Dividir"));
        }
        if (bssoMode && !genioMode) {
            m_leFortSplitAct->setEnabled(hasLower && m_bssoGuideReady && bssoSidePoints >= 3);
            m_leFortSplitAct->setToolTip(m_bssoActiveLeftSide
                ? tr("Dividir mandibula usando el plano BSSO izquierdo.")
                : tr("Dividir mandibula usando el plano BSSO derecho."));
        } else if (!bssoMode) {
            m_leFortSplitAct->setEnabled(hasUpper && leFortPoints >= 4 && leFortPlaneReady);
            m_leFortSplitAct->setToolTip(tr("Aplicar osteotomia Le Fort I al maxilar."));
        }
    }

    QAction* const leFortPointActions[4] = {
        m_leFortPirDACt, m_leFortPirIAct, m_leFortPilaxDAct, m_leFortPilaxIAct
    };
    QAction* const bssoPointActions[6] = {
        m_bssoRamusRightAct, m_bssoBodyRightAct, m_bssoPlaneRightAct,
        m_bssoRamusLeftAct,  m_bssoBodyLeftAct,  m_bssoPlaneLeftAct
    };
    QAction* const genioPointActions[4] = {
        m_genioApicalRightAct, m_genioBasalRightAct,
        m_genioApicalLeftAct,  m_genioBasalLeftAct
    };

    if (!bssoMode) {
        for (int i = 0; i < 4; ++i) {
            if (m_leFortPointSet[static_cast<size_t>(i)])
                setGuidedDone(leFortPointActions[i]);
        }
        if (m_leFortCranialMesh && m_leFortSegmentMesh)
            setGuidedDone(m_leFortSplitAct);

        int nextPoint = -1;
        for (int i = 0; i < 4; ++i) {
            if (!m_leFortPointSet[static_cast<size_t>(i)]) {
                nextPoint = i;
                break;
            }
        }
        if (hasUpper && nextPoint >= 0) {
            setGuidedNext(leFortPointActions[nextPoint],
                          tr("Le Fort I: marque el siguiente punto de referencia."));
        } else if (m_leFortAcceptPlaneAct && m_leFortAcceptPlaneAct->isEnabled()) {
            setGuidedNext(m_leFortAcceptPlaneAct, tr("Confirme el plano Le Fort I ajustado."));
        } else if (m_leFortAdjustPlaneAct && m_leFortAdjustPlaneAct->isEnabled() &&
                   !(m_leFortCranialMesh && m_leFortSegmentMesh)) {
            setGuidedNext(m_leFortAdjustPlaneAct, tr("Ajuste el plano Le Fort I antes de dividir."));
        } else if (m_leFortSplitAct && m_leFortSplitAct->isEnabled() &&
                   !(m_leFortCranialMesh && m_leFortSegmentMesh)) {
            setGuidedNext(m_leFortSplitAct, tr("Divida el maxilar con el plano Le Fort I."));
        } else if (m_leFortExportAct && m_leFortExportAct->isEnabled()) {
            setGuidedNext(m_leFortExportAct, tr("Segmentos Le Fort listos para exportar o continuar."));
        }
        return;
    }

    if (genioMode) {
        for (int i = 0; i < 4; ++i) {
            if (m_genioPointSet[static_cast<size_t>(i)])
                setGuidedDone(genioPointActions[i]);
        }
        if (hasGenioSegments)
            setGuidedDone(m_leFortSplitAct);

        int nextPoint = -1;
        for (int i = 0; i < 4; ++i) {
            if (!m_genioPointSet[static_cast<size_t>(i)]) {
                nextPoint = i;
                break;
            }
        }
        if (hasLower && nextPoint >= 0) {
            setGuidedNext(genioPointActions[nextPoint],
                          tr("Menton: marque el siguiente punto del plano."));
        } else if (m_genioAcceptPlaneAct && m_genioAcceptPlaneAct->isEnabled()) {
            setGuidedNext(m_genioAcceptPlaneAct, tr("Confirme el plano de menton ajustado."));
        } else if (m_genioAdjustPlaneAct && m_genioAdjustPlaneAct->isEnabled() && !hasGenioSegments) {
            setGuidedNext(m_genioAdjustPlaneAct, tr("Ajuste el plano de menton antes de dividir."));
        } else if (m_leFortSplitAct && m_leFortSplitAct->isEnabled() && !hasGenioSegments) {
            setGuidedNext(m_leFortSplitAct, tr("Divida el menton con el plano definido."));
        } else if (m_leFortExportAct && m_leFortExportAct->isEnabled()) {
            setGuidedNext(m_leFortExportAct, tr("Segmentos de menton listos para exportar o continuar."));
        }
        return;
    }

    for (int i = 0; i < 3; ++i) {
        if (m_bssoPointSet[static_cast<size_t>(sideBase + i)])
            setGuidedDone(bssoPointActions[sideBase + i]);
    }
    if (m_bssoSideSplitDone[static_cast<size_t>(m_bssoActiveLeftSide ? 1 : 0)])
        setGuidedDone(m_leFortSplitAct);

    int nextBssoPoint = -1;
    for (int i = 0; i < 3; ++i) {
        if (!m_bssoPointSet[static_cast<size_t>(sideBase + i)]) {
            nextBssoPoint = sideBase + i;
            break;
        }
    }
    if (hasLower && nextBssoPoint >= 0) {
        setGuidedNext(bssoPointActions[nextBssoPoint],
                      m_bssoActiveLeftSide
                          ? tr("BSSO izquierda: marque el siguiente punto del plano.")
                          : tr("BSSO derecha: marque el siguiente punto del plano."));
    } else if (m_bssoAcceptGuideAct && m_bssoAcceptGuideAct->isEnabled()) {
        setGuidedNext(m_bssoAcceptGuideAct, tr("Confirme la guia BSSO ajustada."));
    } else if (m_bssoAdjustGuideAct && m_bssoAdjustGuideAct->isEnabled() &&
               !m_bssoSideSplitDone[static_cast<size_t>(m_bssoActiveLeftSide ? 1 : 0)]) {
        setGuidedNext(m_bssoAdjustGuideAct, tr("Ajuste la guia BSSO antes de dividir."));
    } else if (m_leFortSplitAct && m_leFortSplitAct->isEnabled() &&
               !m_bssoSideSplitDone[static_cast<size_t>(m_bssoActiveLeftSide ? 1 : 0)]) {
        setGuidedNext(m_leFortSplitAct,
                      m_bssoActiveLeftSide
                          ? tr("Divida la sagital izquierda.")
                          : tr("Divida la sagital derecha."));
    } else if (!m_bssoActiveLeftSide && m_bssoSideSplitDone[0] && !m_bssoSideSplitDone[1]) {
        setGuidedNext(m_bssoRamusLeftAct, tr("Continue con la sagital izquierda."));
    } else if (hasBilateralBssoSegments) {
        setGuidedNext(m_genioApicalRightAct, tr("BSSO lista. Puede continuar con menton si aplica."));
    }
}

// BSSO planning guide: one Z-shaped plate for the active mandibular side.
void MainWindow::onBssoPointPicked(int /*actorLabel*/, double x, double y, double z)
{
    if (m_bssoCapturingIdx < 0 || m_bssoCapturingIdx > 5) return;
    setBssoActiveSide(m_bssoCapturingIdx >= 3);

    while (m_bssoPoints.size() <= m_bssoCapturingIdx)
        m_bssoPoints.append(QVector3D{});

    m_bssoPoints[m_bssoCapturingIdx] =
        QVector3D(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
    m_bssoPointSet[static_cast<size_t>(m_bssoCapturingIdx)] = true;

    static const QColor kColors[6] = {
        QColor(245, 245, 210), QColor(210, 245, 160), QColor(170, 230, 255),
        QColor(245, 245, 210), QColor(210, 245, 160), QColor(170, 230, 255)
    };
    if (m_osteotomyView)
        m_osteotomyView->addPointMarker(x, y, z, kColors[m_bssoCapturingIdx]);

    QAction* const acts[6] = {
        m_bssoRamusRightAct, m_bssoBodyRightAct, m_bssoPlaneRightAct,
        m_bssoRamusLeftAct,  m_bssoBodyLeftAct,  m_bssoPlaneLeftAct
    };
    if (auto* act = acts[m_bssoCapturingIdx])
        act->setChecked(true);

    m_bssoCapturingIdx = -1;
    if (m_osteotomyView) m_osteotomyView->setPointPickMode(false);

    updateBssoPointStatus();
    const int sideBase = m_bssoActiveLeftSide ? 3 : 0;
    int sideCount = 0;
    for (int i = 0; i < 3; ++i) {
        if (m_bssoPointSet[static_cast<size_t>(sideBase + i)])
            ++sideCount;
    }
    if (sideCount >= 3) {
        updateBssoGuide();
        statusBar()->showMessage(m_bssoActiveLeftSide
            ? tr("Plano BSSO izquierdo generado desde rama, cuerpo y basal. Ajuste o divida.")
            : tr("Plano BSSO derecho generado desde rama, cuerpo y basal. Ajuste o divida."));
    } else {
        m_bssoGuideReady = false;
        if (m_osteotomyView) {
            m_osteotomyView->removeMesh(kBssoGuideLabel);
            m_osteotomyView->render();
        }
    }
}

void MainWindow::updateBssoPointStatus()
{
    const int sideBase = m_bssoActiveLeftSide ? 3 : 0;
    int sideCount = 0;
    for (int i = 0; i < 3; ++i) {
        if (m_bssoPointSet[static_cast<size_t>(sideBase + i)])
            ++sideCount;
    }

    if (m_bssoStatusLabel) {
        m_bssoStatusLabel->setText(m_bssoActiveLeftSide
            ? tr("BSSO Izq  %1/3 pts").arg(sideCount)
            : tr("BSSO Der  %1/3 pts").arg(sideCount));
    }

    const bool hasMandible = m_lowerCompositeMesh && m_lowerCompositeMesh->GetNumberOfPoints() > 0;
    const bool hasGuide = m_bssoGuideVisualMesh && m_bssoGuideVisualMesh->GetNumberOfPoints() > 0;
    if (m_bssoAutoGuideAct)   m_bssoAutoGuideAct->setEnabled(hasMandible);
    if (m_bssoLeftGuideAct)   m_bssoLeftGuideAct->setEnabled(hasMandible);
    if (m_bssoRamusRightAct)  m_bssoRamusRightAct->setEnabled(hasMandible);
    if (m_bssoBodyRightAct)   m_bssoBodyRightAct->setEnabled(hasMandible);
    if (m_bssoPlaneRightAct)  m_bssoPlaneRightAct->setEnabled(hasMandible);
    if (m_bssoRamusLeftAct)   m_bssoRamusLeftAct->setEnabled(hasMandible);
    if (m_bssoBodyLeftAct)    m_bssoBodyLeftAct->setEnabled(hasMandible);
    if (m_bssoPlaneLeftAct)   m_bssoPlaneLeftAct->setEnabled(hasMandible);
    if (m_bssoAdjustGuideAct) m_bssoAdjustGuideAct->setEnabled(hasGuide);
    if (m_bssoGuidePropsAct)  m_bssoGuidePropsAct->setEnabled(hasGuide);
    if (m_leFortSplitAct && hasGuide) {
        m_leFortSplitAct->setEnabled(true);
        m_leFortSplitAct->setToolTip(m_bssoActiveLeftSide
            ? tr("Dividir mandibula usando la guia BSSO izquierda.")
            : tr("Dividir mandibula usando la guia BSSO derecha."));
    }
    updateOsteotomyWorkflowUi();
}

void MainWindow::onGenioPointPicked(int /*actorLabel*/, double x, double y, double z)
{
    if (m_genioCapturingIdx < 0 || m_genioCapturingIdx > 3) return;

    while (m_genioPoints.size() <= m_genioCapturingIdx)
        m_genioPoints.append(QVector3D{});

    m_genioPoints[m_genioCapturingIdx] =
        QVector3D(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
    m_genioPointSet[static_cast<size_t>(m_genioCapturingIdx)] = true;

    static const QColor kColors[4] = {
        QColor(245, 245, 210), QColor(255, 170, 80),
        QColor(170, 230, 255), QColor(130, 230, 140)
    };
    if (m_osteotomyView)
        m_osteotomyView->addPointMarker(x, y, z, kColors[m_genioCapturingIdx]);

    QAction* const acts[4] = {
        m_genioApicalRightAct, m_genioBasalRightAct,
        m_genioApicalLeftAct,  m_genioBasalLeftAct
    };
    if (auto* act = acts[m_genioCapturingIdx])
        act->setChecked(true);

    m_genioCapturingIdx = -1;
    if (m_osteotomyView) m_osteotomyView->setPointPickMode(false);

    updateGenioPointStatus();
    const int count = static_cast<int>(
        std::count(m_genioPointSet.begin(), m_genioPointSet.end(), true));
    if (count >= 4) {
        updateGenioPlaneGuide();
        statusBar()->showMessage(
            tr("Plano de menton generado desde apical/basal derecho e izquierdo. Ajuste si es necesario."));
    } else if (m_osteotomyView) {
        m_osteotomyView->removeMesh(kGenioPlaneLabel);
        m_osteotomyView->removeMesh(kGenioCutLineLabel);
        m_osteotomyView->render();
    }
}

void MainWindow::updateGenioPointStatus()
{
    const int n = static_cast<int>(
        std::count(m_genioPointSet.begin(), m_genioPointSet.end(), true));
    if (m_genioStatusLabel)
        m_genioStatusLabel->setText(tr("Mentón  %1/4 pts").arg(n));

    const bool hasMandible =
        (m_bssoDistalMesh && m_bssoDistalMesh->GetNumberOfPoints() > 0) ||
        (m_lowerCompositeMesh && m_lowerCompositeMesh->GetNumberOfPoints() > 0);
    const bool hasPlane = m_genioPlaneVisualMesh &&
        m_genioPlaneVisualMesh->GetNumberOfPoints() > 0;

    if (m_genioApicalRightAct) m_genioApicalRightAct->setEnabled(hasMandible);
    if (m_genioBasalRightAct)  m_genioBasalRightAct->setEnabled(hasMandible);
    if (m_genioApicalLeftAct)  m_genioApicalLeftAct->setEnabled(hasMandible);
    if (m_genioBasalLeftAct)   m_genioBasalLeftAct->setEnabled(hasMandible);
    if (m_genioAdjustPlaneAct) m_genioAdjustPlaneAct->setEnabled(hasPlane && n >= 4);
    if (m_genioAcceptPlaneAct && !m_genioAcceptPlaneAct->isEnabled())
        m_genioAcceptPlaneAct->setEnabled(false);

    updateOsteotomyWorkflowUi();
}

vtkSmartPointer<vtkPolyData> MainWindow::buildGenioGuideMesh() const
{
    if (m_genioPlaneNormal.isNull())
        return nullptr;

    QVector3D normal = m_genioPlaneNormal.normalized();
    QVector3D axis = projectedInPlaneAxis(m_genioGuideAxis, normal);
    QVector3D depthAxis = projectedInPlaneAxis(m_genioGuideDepthAxis, normal);
    if (std::abs(QVector3D::dotProduct(axis, depthAxis)) > 0.25f) {
        depthAxis = QVector3D::crossProduct(normal, axis);
        if (depthAxis.lengthSquared() < 1e-8f)
            depthAxis = fallbackInPlaneAxis(normal);
        else
            depthAxis.normalize();
    }

    const double length = std::max(20.0, m_genioGuideLengthMm);
    const double width = std::max(8.0, m_genioGuideWidthMm);
    const QVector3D halfLength = axis * static_cast<float>(length * 0.5);
    const QVector3D halfWidth = depthAxis * static_cast<float>(width * 0.5);

    auto points = vtkSmartPointer<vtkPoints>::New();
    points->InsertNextPoint((m_genioPlaneCenter - halfLength - halfWidth).x(),
                            (m_genioPlaneCenter - halfLength - halfWidth).y(),
                            (m_genioPlaneCenter - halfLength - halfWidth).z());
    points->InsertNextPoint((m_genioPlaneCenter + halfLength - halfWidth).x(),
                            (m_genioPlaneCenter + halfLength - halfWidth).y(),
                            (m_genioPlaneCenter + halfLength - halfWidth).z());
    points->InsertNextPoint((m_genioPlaneCenter + halfLength + halfWidth).x(),
                            (m_genioPlaneCenter + halfLength + halfWidth).y(),
                            (m_genioPlaneCenter + halfLength + halfWidth).z());
    points->InsertNextPoint((m_genioPlaneCenter - halfLength + halfWidth).x(),
                            (m_genioPlaneCenter - halfLength + halfWidth).y(),
                            (m_genioPlaneCenter - halfLength + halfWidth).z());

    auto polys = vtkSmartPointer<vtkCellArray>::New();
    polys->InsertNextCell(4);
    polys->InsertCellPoint(0);
    polys->InsertCellPoint(1);
    polys->InsertCellPoint(2);
    polys->InsertCellPoint(3);

    auto mesh = vtkSmartPointer<vtkPolyData>::New();
    mesh->SetPoints(points);
    mesh->SetPolys(polys);
    mesh->Modified();
    return mesh;
}

void MainWindow::updateGenioPlaneGuide()
{
    if (!m_osteotomyView) return;

    m_osteotomyView->removeMesh(kGenioPlaneLabel);
    m_osteotomyView->removeMesh(kGenioCutLineLabel);

    const int n = static_cast<int>(
        std::count(m_genioPointSet.begin(), m_genioPointSet.end(), true));
    if (n < 4 || m_genioPoints.size() < 4 ||
        !std::all_of(m_genioPointSet.begin(), m_genioPointSet.end(),
                     [](bool v) { return v; })) {
        m_genioPlaneNormal = QVector3D(0, 0, 0);
        m_genioPlaneCenter = QVector3D(0, 0, 0);
        m_genioPlaneVisualMesh = nullptr;
        m_osteotomyView->render();
        return;
    }

    const QVector3D apicalRight = m_genioPoints[0];
    const QVector3D basalRight = m_genioPoints[1];
    const QVector3D apicalLeft = m_genioPoints[2];
    const QVector3D basalLeft = m_genioPoints[3];
    const QVector3D ordered[4] = { apicalRight, basalRight, basalLeft, apicalLeft };

    QVector3D normal(0, 0, 0);
    for (int i = 0; i < 4; ++i) {
        const QVector3D& c = ordered[i];
        const QVector3D& nx = ordered[(i + 1) % 4];
        normal += QVector3D((c.y() - nx.y()) * (c.z() + nx.z()),
                            (c.z() - nx.z()) * (c.x() + nx.x()),
                            (c.x() - nx.x()) * (c.y() + nx.y()));
    }
    if (normal.lengthSquared() < 1e-8f) {
        normal = QVector3D::crossProduct(basalRight - apicalRight,
                                         apicalLeft - apicalRight);
    }
    if (normal.lengthSquared() < 1e-8f) {
        QMessageBox::warning(this, tr("Mentón"),
            tr("Los 4 puntos del menton no definen un plano estable. Separe mas los puntos."));
        return;
    }
    normal.normalize();

    const QVector3D center = (apicalRight + basalRight + apicalLeft + basalLeft) / 4.0f;
    const QVector3D rightMid = (apicalRight + basalRight) * 0.5f;
    const QVector3D leftMid = (apicalLeft + basalLeft) * 0.5f;
    const QVector3D apicalMid = (apicalRight + apicalLeft) * 0.5f;
    const QVector3D basalMid = (basalRight + basalLeft) * 0.5f;

    QVector3D axis = projectedInPlaneAxis(leftMid - rightMid, normal);
    QVector3D depthAxis = projectedInPlaneAxis(basalMid - apicalMid, normal);
    if (std::abs(QVector3D::dotProduct(axis, depthAxis)) > 0.25f) {
        depthAxis = QVector3D::crossProduct(normal, axis);
        if (QVector3D::dotProduct(depthAxis, basalMid - apicalMid) < 0.0f)
            depthAxis = -depthAxis;
        if (depthAxis.lengthSquared() < 1e-8f)
            depthAxis = fallbackInPlaneAxis(normal);
        else
            depthAxis.normalize();
    }

    m_genioPlaneNormal = normal;
    m_genioPlaneCenter = center;
    m_genioGuideAxis = axis;
    m_genioGuideDepthAxis = depthAxis;
    m_genioGuideLengthMm = std::max(45.0, static_cast<double>((leftMid - rightMid).length()) + 24.0);
    m_genioGuideWidthMm = std::max(18.0, static_cast<double>((basalMid - apicalMid).length()) + 12.0);

    m_genioPlaneVisualMesh = buildGenioGuideMesh();
    rebuildGenioPlaneFromState();
    if (m_genioAdjustPlaneAct) m_genioAdjustPlaneAct->setEnabled(true);
    updateOsteotomyWorkflowUi();
}

void MainWindow::rebuildGenioPlaneFromState()
{
    if (!m_osteotomyView) return;

    m_osteotomyView->removeMesh(kGenioPlaneLabel);
    m_osteotomyView->removeMesh(kGenioCutLineLabel);

    if (m_genioPlaneNormal.isNull()) {
        m_osteotomyView->render();
        return;
    }

    vtkSmartPointer<vtkPolyData> planeMesh = m_genioPlaneVisualMesh;
    if (!planeMesh || planeMesh->GetNumberOfPoints() == 0) {
        planeMesh = buildGenioGuideMesh();
        m_genioPlaneVisualMesh = planeMesh;
    }

    if (planeMesh) {
        m_osteotomyView->addMesh(kGenioPlaneLabel, planeMesh, tr("Guia de menton"));
        m_osteotomyView->setMeshColor(kGenioPlaneLabel, QColor(90, 150, 55));
        m_osteotomyView->setMeshOpacity(kGenioPlaneLabel, 0.62);
    }

    vtkPolyData* targetMesh =
        (m_bssoDistalMesh && m_bssoDistalMesh->GetNumberOfCells() > 0)
            ? m_bssoDistalMesh.Get()
            : m_lowerCompositeMesh.Get();
    if (targetMesh && targetMesh->GetNumberOfCells() > 0) {
        auto plane = vtkSmartPointer<vtkPlane>::New();
        plane->SetNormal(m_genioPlaneNormal.x(), m_genioPlaneNormal.y(), m_genioPlaneNormal.z());
        plane->SetOrigin(m_genioPlaneCenter.x(), m_genioPlaneCenter.y(), m_genioPlaneCenter.z());

        auto cutter = vtkSmartPointer<vtkCutter>::New();
        cutter->SetCutFunction(plane);
        cutter->SetInputData(targetMesh);
        cutter->Update();

        if (cutter->GetOutput()->GetNumberOfCells() > 0) {
            auto tube = vtkSmartPointer<vtkTubeFilter>::New();
            tube->SetInputConnection(cutter->GetOutputPort());
            tube->SetRadius(std::max(0.35, m_genioCutThicknessMm * 0.5));
            tube->SetNumberOfSides(12);
            tube->Update();

            auto cutLine = vtkSmartPointer<vtkPolyData>::New();
            cutLine->DeepCopy(tube->GetOutput());
            m_osteotomyView->addMesh(kGenioCutLineLabel, cutLine, tr("Linea menton"));
            m_osteotomyView->setMeshColor(kGenioCutLineLabel, QColor(185, 240, 80));
            m_osteotomyView->setMeshOpacity(kGenioCutLineLabel, 1.0);
        }
    }

    m_osteotomyView->render();
}

vtkSmartPointer<vtkPolyData> MainWindow::buildBssoGuideMesh() const
{
    if (!m_lowerCompositeMesh || m_lowerCompositeMesh->GetNumberOfPoints() == 0)
        return nullptr;

    const int activeBase = m_bssoActiveLeftSide ? 3 : 0;
    if (m_bssoPoints.size() <= activeBase + 2)
        return nullptr;
    for (int i = 0; i < 3; ++i) {
        if (!m_bssoPointSet[static_cast<size_t>(activeBase + i)])
            return nullptr;
    }

    auto points = vtkSmartPointer<vtkPoints>::New();
    auto polys = vtkSmartPointer<vtkCellArray>::New();

    const QVector3D rama = m_bssoPoints[activeBase + 0];
    const QVector3D cuerpo = m_bssoPoints[activeBase + 1];
    const QVector3D basal = m_bssoPoints[activeBase + 2];

    QVector3D axisLong = cuerpo - rama;
    if (axisLong.lengthSquared() < 1e-8f)
        axisLong = QVector3D(0.0f, 0.0f, 1.0f);
    axisLong.normalize();

    QVector3D axisBasal = basal - ((rama + cuerpo) * 0.5f);
    axisBasal -= axisLong * QVector3D::dotProduct(axisBasal, axisLong);
    if (axisBasal.lengthSquared() < 1e-8f)
        axisBasal = fallbackInPlaneAxis(axisLong);
    axisBasal.normalize();

    QVector3D normal = QVector3D::crossProduct(axisLong, axisBasal);
    if (normal.lengthSquared() < 1e-8f)
        normal = QVector3D(1.0f, 0.0f, 0.0f);
    normal.normalize();
    axisBasal = QVector3D::crossProduct(normal, axisLong).normalized();

    // Ensure normal points medially (inwards)
    double mandBounds[6] = {};
    m_lowerCompositeMesh->GetBounds(mandBounds);
    const double mandCenterX = (mandBounds[0] + mandBounds[1]) * 0.5;
    const QVector3D center = (rama + cuerpo + basal) / 3.0f;
    QVector3D centralDirection =
        center.x() < static_cast<float>(mandCenterX)
            ? QVector3D(1.0f, 0.0f, 0.0f)
            : QVector3D(-1.0f, 0.0f, 0.0f);
    if (QVector3D::dotProduct(normal, centralDirection) < 0.0f) {
        normal = -normal;
    }

    const double pickedLong = std::max({static_cast<double>((rama - cuerpo).length()),
                                        static_cast<double>((rama - basal).length()),
                                        static_cast<double>((cuerpo - basal).length())});
    const double widthMm = std::max(m_bssoGuideLengthMm, static_cast<double>((basal - center).length()) * 2.0 + 22.0);
    const QVector3D halfWidth = axisBasal * static_cast<float>(widthMm * 0.5);

    // Flange depth: top flange medially/lingually (+normal), bottom flange laterally/buccally (-normal)
    const float flangeDepth = 12.0f;
    const QVector3D p_top_end = rama + normal * flangeDepth;
    const QVector3D p_bottom_end = basal - normal * flangeDepth;

    std::array<QVector3D, 5> path = {
        p_top_end,
        rama,
        cuerpo,
        basal,
        p_bottom_end
    };

    for (int i = 0; i < 5; ++i) {
        QVector3D b = path[i] - halfWidth;
        QVector3D f = path[i] + halfWidth;
        points->InsertNextPoint(b.x(), b.y(), b.z()); // Index 2*i
        points->InsertNextPoint(f.x(), f.y(), f.z()); // Index 2*i + 1
    }

    for (int i = 0; i < 4; ++i) {
        polys->InsertNextCell(4);
        polys->InsertCellPoint(2 * i);         // B_i
        polys->InsertCellPoint(2 * (i+1));     // B_{i+1}
        polys->InsertCellPoint(2 * (i+1) + 1); // F_{i+1}
        polys->InsertCellPoint(2 * i + 1);     // F_i
    }

    auto mesh = vtkSmartPointer<vtkPolyData>::New();
    mesh->SetPoints(points);
    mesh->SetPolys(polys);
    mesh->Modified();
    return mesh;
}

void MainWindow::updateBssoGuide()
{
    if (!m_osteotomyView || !m_lowerCompositeMesh) return;

    m_osteotomyView->removeMesh(kBssoGuideLabel);
    m_bssoGuideVisualMesh = buildBssoGuideMesh();
    if (!m_bssoGuideVisualMesh || m_bssoGuideVisualMesh->GetNumberOfPoints() == 0) {
        updateBssoPointStatus();
        return;
    }

    const QColor guideColor = objectColorForLabel(kBssoGuideLabel);
    const QString sideName = m_bssoActiveLeftSide ? tr("izquierda") : tr("derecha");
    m_osteotomyView->addMesh(kBssoGuideLabel, m_bssoGuideVisualMesh,
                             tr("Guia BSSO %1").arg(sideName));
    m_osteotomyView->setMeshColor(kBssoGuideLabel, guideColor);
    m_osteotomyView->setMeshOpacity(kBssoGuideLabel, 0.55);
    addObjectEntry(meshLabelName(kBssoGuideLabel), guideColor, kBssoGuideLabel);
    m_bssoGuideReady = true;
    updateBssoPointStatus();
    m_osteotomyView->render();
    statusBar()->showMessage(tr("Guia BSSO %1 lista. Ajustela con el gizmo si es necesario.").arg(sideName));
}

void MainWindow::showBssoGuidePropertiesDialog()
{
    if (!m_bssoGuideVisualMesh) {
        updateBssoGuide();
        if (!m_bssoGuideVisualMesh) return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Propiedades BSSO"));
    dialog.setStyleSheet(styleSheet());

    auto* layout = new QFormLayout(&dialog);
    layout->setContentsMargins(16, 14, 16, 12);
    layout->setSpacing(8);

    auto makeSpin = [&](double value, double min, double max) {
        auto* spin = new QDoubleSpinBox(&dialog);
        spin->setDecimals(1);
        spin->setRange(min, max);
        spin->setSingleStep(1.0);
        spin->setSuffix(tr(" mm"));
        spin->setValue(value);
        spin->setMinimumWidth(120);
        return spin;
    };

    auto* lengthSpin = makeSpin(m_bssoGuideLengthMm, 20.0, 140.0);
    auto* heightSpin = makeSpin(m_bssoGuideHeightMm, 30.0, 150.0);
    auto* wingSpin = makeSpin(m_bssoGuideWingMm, 5.0, 90.0);
    auto* thicknessSpin = makeSpin(m_bssoCutThicknessMm, 0.2, 5.0);

    layout->addRow(tr("Length:"), lengthSpin);
    layout->addRow(tr("Height:"), heightSpin);
    layout->addRow(tr("Medial extension:"), wingSpin);
    layout->addRow(tr("Thickness:"), thicknessSpin);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         Qt::Horizontal, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addRow(buttons);

    if (dialog.exec() != QDialog::Accepted)
        return;

    m_bssoGuideLengthMm = lengthSpin->value();
    m_bssoGuideHeightMm = heightSpin->value();
    m_bssoGuideWingMm = wingSpin->value();
    m_bssoCutThicknessMm = thicknessSpin->value();
    updateBssoGuide();
    statusBar()->showMessage(tr("BSSO: propiedades de guia actualizadas."));
}

void MainWindow::updateLeFortPointStatus()
{
    const int n = static_cast<int>(
        std::count(m_leFortPointSet.begin(), m_leFortPointSet.end(), true));
    if (m_leFortCutLabel)
        m_leFortCutLabel->setText(tr("Le Fort I  %1/4 pts").arg(n));
    // Le Fort I needs the four clinical landmarks before exposing the guide
    // or the split action.
    if (m_leFortSplitAct)
        m_leFortSplitAct->setEnabled(m_leFortTargetLabel == kUpperCompositeLabel &&
                                     n >= 4 && !m_leFortPlaneNormal.isNull());

    static const char* const kNames[4] =
        { "Piriforme Der", "Piriforme Izq", "Pilax Der", "Pilax Izq" };
    const QString msg = (n >= 4)
        ? tr("4 puntos Le Fort I definidos. Listo para dividir.")
        : tr("Le Fort I — marque: ") +
          QStringList({tr("Piriforme Der"), tr("Piriforme Izq"),
                       tr("Pilax Der"),     tr("Pilax Izq")}).mid(n).join(", ");
    statusBar()->showMessage(msg);
    updateLeFortWizardUi();
    updateOsteotomyWorkflowUi();
    Q_UNUSED(kNames)
}

// ─────────────────────────────────────────────────────────────────────────────
// Computes the best-fit plane from the available landmarks (3+) via Newell's
// method and redraws the translucent disc + cutter intersection line.
vtkSmartPointer<vtkPolyData> MainWindow::buildLeFortCutGuideMesh() const
{
    if (m_leFortPlaneNormal.isNull())
        return nullptr;

    QVector3D normal = m_leFortPlaneNormal.normalized();
    QVector3D axis = projectedInPlaneAxis(m_leFortGuideAxis, normal);
    QVector3D depthAxis = QVector3D::crossProduct(normal, axis);
    if (depthAxis.lengthSquared() < 1e-8f)
        depthAxis = fallbackInPlaneAxis(normal);
    else
        depthAxis.normalize();

    const double width = std::max(5.0, m_leFortGuideWidthMm);
    const double extRight = std::max(0.0, m_leFortGuideExtensionRightMm);
    const double extLeft = std::max(0.0, m_leFortGuideExtensionLeftMm);

    auto points = vtkSmartPointer<vtkPoints>::New();
    auto polys = vtkSmartPointer<vtkCellArray>::New();

    // Check if we have all 4 landmarks defined
    bool allFourSet = true;
    for (size_t i = 0; i < 4; ++i) {
        if (!m_leFortPointSet[i]) {
            allFourSet = false;
            break;
        }
    }

    if (allFourSet && m_leFortPoints.size() >= 4) {
        // Landmarks: 0=PirD, 1=PirI, 2=PilaxD, 3=PilaxI
        QVector3D p_pir_right = m_leFortPoints[0];
        QVector3D p_pir_left = m_leFortPoints[1];
        QVector3D p_pilar_right = m_leFortPoints[2];
        QVector3D p_pilar_left = m_leFortPoints[3];

        QVector3D dir_left = (p_pilar_left - p_pir_left);
        if (dir_left.lengthSquared() > 1e-8f) dir_left.normalize();
        QVector3D p_ext_left = p_pilar_left + dir_left * static_cast<float>(extLeft);

        QVector3D dir_right = (p_pilar_right - p_pir_right);
        if (dir_right.lengthSquared() > 1e-8f) dir_right.normalize();
        QVector3D p_ext_right = p_pilar_right + dir_right * static_cast<float>(extRight);

        std::array<QVector3D, 6> path = {
            p_ext_right,
            p_pilar_right,
            p_pir_right,
            p_pir_left,
            p_pilar_left,
            p_ext_left
        };

        const QVector3D halfDepth = depthAxis * static_cast<float>(width * 0.5);

        for (int i = 0; i < 6; ++i) {
            QVector3D b = path[i] - halfDepth;
            QVector3D f = path[i] + halfDepth;
            points->InsertNextPoint(b.x(), b.y(), b.z()); // Index 2*i
            points->InsertNextPoint(f.x(), f.y(), f.z()); // Index 2*i + 1
        }

        for (int i = 0; i < 5; ++i) {
            polys->InsertNextCell(4);
            polys->InsertCellPoint(2 * i);         // B_i
            polys->InsertCellPoint(2 * (i+1));     // B_{i+1}
            polys->InsertCellPoint(2 * (i+1) + 1); // F_{i+1}
            polys->InsertCellPoint(2 * i + 1);     // F_i
        }
    } else {
        // Fallback to single rectangle if not all landmarks are set
        const double coreLength = std::max(20.0, m_leFortGuideCoreLengthMm);
        const QVector3D rightEnd =
            m_leFortPlaneCenter - axis * static_cast<float>(coreLength * 0.5 + extRight);
        const QVector3D leftEnd =
            m_leFortPlaneCenter + axis * static_cast<float>(coreLength * 0.5 + extLeft);
        const QVector3D halfDepth = depthAxis * static_cast<float>(width * 0.5);

        points->InsertNextPoint((rightEnd - halfDepth).x(), (rightEnd - halfDepth).y(), (rightEnd - halfDepth).z());
        points->InsertNextPoint((leftEnd  - halfDepth).x(), (leftEnd  - halfDepth).y(), (leftEnd  - halfDepth).z());
        points->InsertNextPoint((leftEnd  + halfDepth).x(), (leftEnd  + halfDepth).y(), (leftEnd  + halfDepth).z());
        points->InsertNextPoint((rightEnd + halfDepth).x(), (rightEnd + halfDepth).y(), (rightEnd + halfDepth).z());

        polys->InsertNextCell(4);
        polys->InsertCellPoint(0);
        polys->InsertCellPoint(1);
        polys->InsertCellPoint(2);
        polys->InsertCellPoint(3);
    }

    auto mesh = vtkSmartPointer<vtkPolyData>::New();
    mesh->SetPoints(points);
    mesh->SetPolys(polys);
    mesh->Modified();
    return mesh;
}

void MainWindow::showLeFortGuidePropertiesDialog()
{
    if (m_leFortPlaneNormal.isNull()) {
        QMessageBox::information(this, tr("Le Fort I"),
                                 tr("Primero defina el plano con los 4 puntos Le Fort I."));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Propiedades Le Fort I"));
    dialog.setStyleSheet(styleSheet());

    auto* layout = new QFormLayout(&dialog);
    layout->setContentsMargins(16, 14, 16, 12);
    layout->setSpacing(8);

    auto makeSpin = [&](double value, double min, double max) {
        auto* spin = new QDoubleSpinBox(&dialog);
        spin->setDecimals(1);
        spin->setRange(min, max);
        spin->setSingleStep(1.0);
        spin->setSuffix(tr(" mm"));
        spin->setValue(value);
        spin->setMinimumWidth(120);
        return spin;
    };

    auto* widthSpin = makeSpin(m_leFortGuideWidthMm, 20.0, 240.0);
    auto* thicknessSpin = makeSpin(m_leFortCutThicknessMm, 0.2, 5.0);
    auto* rightSpin = makeSpin(m_leFortGuideExtensionRightMm, 0.0, 80.0);
    auto* leftSpin = makeSpin(m_leFortGuideExtensionLeftMm, 0.0, 80.0);

    layout->addRow(tr("Width:"), widthSpin);
    layout->addRow(tr("Thickness:"), thicknessSpin);
    layout->addRow(tr("Extension - Right:"), rightSpin);
    layout->addRow(tr("Extension - Left:"), leftSpin);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         Qt::Horizontal, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addRow(buttons);

    if (dialog.exec() != QDialog::Accepted)
        return;

    m_leFortGuideWidthMm = widthSpin->value();
    m_leFortCutThicknessMm = thicknessSpin->value();
    m_leFortGuideExtensionRightMm = rightSpin->value();
    m_leFortGuideExtensionLeftMm = leftSpin->value();
    m_leFortPlaneVisualMesh = buildLeFortCutGuideMesh();

    rebuildLeFortPlaneFromState();
    statusBar()->showMessage(
        tr("Le Fort I: guia actualizada (%1 mm ancho, %2 mm grosor).")
            .arg(m_leFortGuideWidthMm, 0, 'f', 1)
            .arg(m_leFortCutThicknessMm, 0, 'f', 1));
    updateLeFortWizardUi();
}

void MainWindow::updateLeFortPlaneDisc()
{
    const int availableCount = static_cast<int>(
        std::count(m_leFortPointSet.begin(), m_leFortPointSet.end(), true));
    if (!m_osteotomyView) return;

    m_osteotomyView->removeMesh(kLeFortPlaneLabel);
    m_osteotomyView->removeMesh(kLeFortCutLineLabel);

    if (availableCount < 4) {
        m_leFortPlaneNormal = QVector3D(0, 0, 0);
        m_leFortPlaneCenter = QVector3D(0, 0, 0);
        m_leFortPlaneVisualMesh = nullptr;
        if (m_leFortAdjustPlaneAct) m_leFortAdjustPlaneAct->setEnabled(false);
        if (m_leFortGuidePropsAct)  m_leFortGuidePropsAct->setEnabled(false);
        if (m_leFortSplitAct)       m_leFortSplitAct->setEnabled(false);
        m_osteotomyView->render();
        return;
    }

    QVector<QVector3D> availablePoints;
    availablePoints.reserve(4);
    for (int i = 0; i < 4 && i < m_leFortPoints.size(); ++i) {
        if (m_leFortPointSet[i])
            availablePoints.append(m_leFortPoints[i]);
    }
    const int n = static_cast<int>(availablePoints.size());
    const bool allFour = n >= 4 &&
        std::all_of(m_leFortPointSet.begin(), m_leFortPointSet.end(),
                    [](bool v) { return v; });
    if (!allFour) return;

    // ── Normal from available points ─────────────────────────────────────────
    // With 3 points: cross product; with 4: Newell on convex quad
    QVector3D normal(0, 0, 0);
    if (allFour) {
        // Polygon order: PirD → PilaxD → PilaxI → PirI (convex quad)
        const QVector3D poly[4] = {
            m_leFortPoints[0], m_leFortPoints[2],
            m_leFortPoints[3], m_leFortPoints[1]
        };
        for (int i = 0; i < 4; ++i) {
            const QVector3D& c = poly[i];
            const QVector3D& nx = poly[(i + 1) % 4];
            normal += QVector3D((c.y()-nx.y())*(c.z()+nx.z()),
                                (c.z()-nx.z())*(c.x()+nx.x()),
                                (c.x()-nx.x())*(c.y()+nx.y()));
        }
    } else {
        // 3 points: cross product of two edges from the first point
        const QVector3D v1 = availablePoints[1] - availablePoints[0];
        const QVector3D v2 = availablePoints[2] - availablePoints[0];
        normal = QVector3D::crossProduct(v1, v2);
    }
    normal.normalize();
    if (normal.z() < 0) normal = -normal;   // always point cranially (toward +Z)

    // Centroid of available points
    QVector3D center;
    for (int i = 0; i < n; ++i) center += availablePoints[i];
    center /= static_cast<float>(n);

    // Store as current plane state (used by split and by gizmo accept)
    m_leFortPlaneNormal = normal;
    m_leFortPlaneCenter = center;

    // ── Disc radius = skull width × 0.75 (extends well beyond the model) ────
    if (allFour) {
        const QVector3D rightMid = (m_leFortPoints[0] + m_leFortPoints[2]) * 0.5f;
        const QVector3D leftMid = (m_leFortPoints[1] + m_leFortPoints[3]) * 0.5f;
        m_leFortGuideAxis = projectedInPlaneAxis(leftMid - rightMid, normal);
        m_leFortGuideCoreLengthMm = std::max(20.0, static_cast<double>((leftMid - rightMid).length()));
    } else {
        double maxDist = 0.0;
        QVector3D axisCandidate;
        for (int i = 0; i < n; ++i) {
            for (int j = i + 1; j < n; ++j) {
                const QVector3D delta = availablePoints[j] - availablePoints[i];
                const double dist = delta.length();
                if (dist > maxDist) {
                    maxDist = dist;
                    axisCandidate = delta;
                }
            }
        }
        m_leFortGuideAxis = projectedInPlaneAxis(axisCandidate, normal);
        m_leFortGuideCoreLengthMm = std::max(20.0, maxDist);
    }
    m_leFortPlaneRadius = std::sqrt(
        std::pow(m_leFortGuideCoreLengthMm * 0.5 +
                     std::max(m_leFortGuideExtensionLeftMm, m_leFortGuideExtensionRightMm), 2.0) +
        std::pow(m_leFortGuideWidthMm * 0.5, 2.0));

    // ── Translucent plane disc (golden-brown, matches reference look) ────────
    m_leFortPlaneVisualMesh = buildLeFortCutGuideMesh();
    if (m_leFortPlaneVisualMesh) {
        m_osteotomyView->addMesh(kLeFortPlaneLabel, m_leFortPlaneVisualMesh, tr("Guia Le Fort I"));
        m_osteotomyView->setMeshColor(kLeFortPlaneLabel, QColor(35, 95, 38));
        m_osteotomyView->setMeshOpacity(kLeFortPlaneLabel, 0.88);
    }

    // ── Cutter intersection line on the target mesh ──────────────────────────
    vtkPolyData* targetMesh = (m_leFortTargetLabel == kUpperCompositeLabel)
        ? m_upperCompositeMesh.Get() : m_lowerCompositeMesh.Get();
    if (targetMesh && targetMesh->GetNumberOfCells() > 0) {
        auto plane = vtkSmartPointer<vtkPlane>::New();
        plane->SetNormal(normal.x(), normal.y(), normal.z());
        plane->SetOrigin(center.x(), center.y(), center.z());

        auto cutter = vtkSmartPointer<vtkCutter>::New();
        cutter->SetCutFunction(plane);
        cutter->SetInputData(targetMesh);
        cutter->Update();

        if (cutter->GetOutput()->GetNumberOfCells() > 0) {
            const double lineRadius = std::max(0.35, m_leFortCutThicknessMm * 0.5);

            auto tube = vtkSmartPointer<vtkTubeFilter>::New();
            tube->SetInputConnection(cutter->GetOutputPort());
            tube->SetRadius(lineRadius);
            tube->SetNumberOfSides(12);
            tube->Update();

            auto cutLine = vtkSmartPointer<vtkPolyData>::New();
            cutLine->DeepCopy(tube->GetOutput());
            m_osteotomyView->addMesh(kLeFortCutLineLabel, cutLine, tr("Linea de corte"));
            m_osteotomyView->setMeshColor(kLeFortCutLineLabel, QColor(120, 220, 80));
            m_osteotomyView->setMeshOpacity(kLeFortCutLineLabel, 1.0);
        }
    }

    // Enable plane gizmo and split only after the four Le Fort landmarks define the plane.
    if (m_leFortAdjustPlaneAct) m_leFortAdjustPlaneAct->setEnabled(true);
    if (m_leFortGuidePropsAct)  m_leFortGuidePropsAct->setEnabled(true);
    if (m_leFortSplitAct) m_leFortSplitAct->setEnabled(true);

    m_osteotomyView->render();
}

// ─────────────────────────────────────────────────────────────────────────────
// Redraws the cut-plane disc and intersection line from m_leFortPlaneNormal/Center.
// Called after the gizmo is accepted to visualise the adjusted position without
// recomputing from landmarks.
void MainWindow::rebuildLeFortPlaneFromState()
{
    if (!m_osteotomyView) return;

    m_osteotomyView->removeMesh(kLeFortPlaneLabel);
    m_osteotomyView->removeMesh(kLeFortCutLineLabel);

    const QVector3D& normal = m_leFortPlaneNormal;
    const QVector3D& center = m_leFortPlaneCenter;

    // Disc radius — same logic as updateLeFortPlaneDisc
    vtkSmartPointer<vtkPolyData> planeMesh = m_leFortPlaneVisualMesh;
    if (!planeMesh || planeMesh->GetNumberOfPoints() == 0) {
        planeMesh = buildLeFortCutGuideMesh();
        m_leFortPlaneVisualMesh = planeMesh;
    }

    if (planeMesh) {
        m_osteotomyView->addMesh(kLeFortPlaneLabel, planeMesh, tr("Guia Le Fort I"));
        m_osteotomyView->setMeshColor(kLeFortPlaneLabel, QColor(35, 95, 38));
        m_osteotomyView->setMeshOpacity(kLeFortPlaneLabel, 0.88);
    }

    // Cut-line on target mesh
    vtkPolyData* targetMesh = (m_leFortTargetLabel == kUpperCompositeLabel)
        ? m_upperCompositeMesh.Get() : m_lowerCompositeMesh.Get();
    if (targetMesh && targetMesh->GetNumberOfCells() > 0) {
        auto plane = vtkSmartPointer<vtkPlane>::New();
        plane->SetNormal(normal.x(), normal.y(), normal.z());
        plane->SetOrigin(center.x(), center.y(), center.z());

        auto cutter = vtkSmartPointer<vtkCutter>::New();
        cutter->SetCutFunction(plane);
        cutter->SetInputData(targetMesh);
        cutter->Update();

        if (cutter->GetOutput()->GetNumberOfCells() > 0) {
            const double lineRadius = std::max(0.35, m_leFortCutThicknessMm * 0.5);

            auto tube = vtkSmartPointer<vtkTubeFilter>::New();
            tube->SetInputConnection(cutter->GetOutputPort());
            tube->SetRadius(lineRadius);
            tube->SetNumberOfSides(12);
            tube->Update();

            auto cutLine = vtkSmartPointer<vtkPolyData>::New();
            cutLine->DeepCopy(tube->GetOutput());
            m_osteotomyView->addMesh(kLeFortCutLineLabel, cutLine, tr("Linea de corte"));
            m_osteotomyView->setMeshColor(kLeFortCutLineLabel, QColor(120, 220, 80));
            m_osteotomyView->setMeshOpacity(kLeFortCutLineLabel, 1.0);
        }
    }

    m_osteotomyView->render();
}

// ─────────────────────────────────────────────────────────────────────────────
// Called when the plane gizmo is accepted in the osteotomy workspace.
// Extracts the accumulated transform, updates m_leFortPlaneNormal/Center, and
// redraws the plane disc + cut line at the new position.
void MainWindow::onOsteotomyGizmoUpdated(int meshLabel, vtkSmartPointer<vtkPolyData> newMesh)
{
    if (meshLabel == kOsteotomyGuideActorKey || meshLabel == kOsteotomyGuideLeftActorKey) {
        applyOsteotomyGizmo(m_osteotomyView ? m_osteotomyView->lastGizmoTransformMatrix() : nullptr);
        return;
    }
    if (meshLabel == kBssoGuideLabel) {
        if (newMesh && newMesh->GetNumberOfPoints() > 0) {
            m_bssoGuideVisualMesh = vtkSmartPointer<vtkPolyData>::New();
            m_bssoGuideVisualMesh->DeepCopy(newMesh);
        }
        if (m_bssoAdjustGuideAct) m_bssoAdjustGuideAct->setEnabled(true);
        if (m_bssoGuidePropsAct)  m_bssoGuidePropsAct->setEnabled(true);
        if (m_bssoAcceptGuideAct) m_bssoAcceptGuideAct->setEnabled(false);
        m_bssoGuideReady = true;
        if (m_leFortSplitAct) {
            m_leFortSplitAct->setEnabled(true);
            m_leFortSplitAct->setToolTip(m_bssoActiveLeftSide
                ? tr("Dividir mandibula usando el plano BSSO izquierdo.")
                : tr("Dividir mandibula usando el plano BSSO derecho."));
        }
        if (m_osteotomyView) m_osteotomyView->render();
        updateOsteotomyWorkflowUi();
        statusBar()->showMessage(tr("Guias BSSO ajustadas."));
        return;
    }

    if (meshLabel == kGenioPlaneLabel) {
        auto mat = m_osteotomyView ? m_osteotomyView->lastGizmoTransformMatrix() : nullptr;
        if (mat) {
            double c[4] = { m_genioPlaneCenter.x(), m_genioPlaneCenter.y(),
                            m_genioPlaneCenter.z(), 1.0 };
            double cOut[4] = {};
            mat->MultiplyPoint(c, cOut);
            m_genioPlaneCenter = QVector3D(static_cast<float>(cOut[0]),
                                           static_cast<float>(cOut[1]),
                                           static_cast<float>(cOut[2]));

            double n[4] = { m_genioPlaneNormal.x(), m_genioPlaneNormal.y(),
                            m_genioPlaneNormal.z(), 0.0 };
            double nOut[4] = {};
            mat->MultiplyPoint(n, nOut);
            m_genioPlaneNormal = QVector3D(static_cast<float>(nOut[0]),
                                           static_cast<float>(nOut[1]),
                                           static_cast<float>(nOut[2])).normalized();

            double a[4] = { m_genioGuideAxis.x(), m_genioGuideAxis.y(),
                            m_genioGuideAxis.z(), 0.0 };
            double aOut[4] = {};
            mat->MultiplyPoint(a, aOut);
            m_genioGuideAxis = projectedInPlaneAxis(
                QVector3D(static_cast<float>(aOut[0]),
                          static_cast<float>(aOut[1]),
                          static_cast<float>(aOut[2])),
                m_genioPlaneNormal);

            double d[4] = { m_genioGuideDepthAxis.x(), m_genioGuideDepthAxis.y(),
                            m_genioGuideDepthAxis.z(), 0.0 };
            double dOut[4] = {};
            mat->MultiplyPoint(d, dOut);
            m_genioGuideDepthAxis = projectedInPlaneAxis(
                QVector3D(static_cast<float>(dOut[0]),
                          static_cast<float>(dOut[1]),
                          static_cast<float>(dOut[2])),
                m_genioPlaneNormal);
        }

        if (newMesh && newMesh->GetNumberOfPoints() > 0) {
            m_genioPlaneVisualMesh = vtkSmartPointer<vtkPolyData>::New();
            m_genioPlaneVisualMesh->DeepCopy(newMesh);

            QVector3D visualCenter;
            QVector3D visualNormal;
            if (fitPlaneFromPolyData(newMesh, m_genioPlaneNormal,
                                     &visualCenter, &visualNormal, nullptr)) {
                m_genioPlaneCenter = visualCenter;
                m_genioPlaneNormal = visualNormal;
            }
        }

        if (m_genioAdjustPlaneAct) m_genioAdjustPlaneAct->setEnabled(true);
        if (m_genioAcceptPlaneAct) m_genioAcceptPlaneAct->setEnabled(false);
        rebuildGenioPlaneFromState();
        updateOsteotomyWorkflowUi();
        statusBar()->showMessage(tr("Plano de menton ajustado."));
        return;
    }

    if (meshLabel != kLeFortPlaneLabel || !m_osteotomyView) return;

    // Retrieve the accumulated gizmo transform matrix
    auto mat = m_osteotomyView->lastGizmoTransformMatrix();
    if (mat) {
        // Transform plane center (homogeneous point — w=1 picks up translation)
        double c[4] = { m_leFortPlaneCenter.x(), m_leFortPlaneCenter.y(),
                        m_leFortPlaneCenter.z(), 1.0 };
        double cOut[4];
        mat->MultiplyPoint(c, cOut);
        m_leFortPlaneCenter = QVector3D(
            static_cast<float>(cOut[0]),
            static_cast<float>(cOut[1]),
            static_cast<float>(cOut[2]));

        // Transform plane normal (direction vector — w=0 suppresses translation)
        double n[4] = { m_leFortPlaneNormal.x(), m_leFortPlaneNormal.y(),
                        m_leFortPlaneNormal.z(), 0.0 };
        double nOut[4];
        mat->MultiplyPoint(n, nOut);
        m_leFortPlaneNormal = QVector3D(
            static_cast<float>(nOut[0]),
            static_cast<float>(nOut[1]),
            static_cast<float>(nOut[2])).normalized();

        double a[4] = { m_leFortGuideAxis.x(), m_leFortGuideAxis.y(),
                        m_leFortGuideAxis.z(), 0.0 };
        double aOut[4];
        mat->MultiplyPoint(a, aOut);
        m_leFortGuideAxis = projectedInPlaneAxis(
            QVector3D(static_cast<float>(aOut[0]),
                      static_cast<float>(aOut[1]),
                      static_cast<float>(aOut[2])),
            m_leFortPlaneNormal);
    }

    if (newMesh && newMesh->GetNumberOfPoints() > 0) {
        m_leFortPlaneVisualMesh = vtkSmartPointer<vtkPolyData>::New();
        m_leFortPlaneVisualMesh->DeepCopy(newMesh);

        QVector3D visualCenter;
        QVector3D visualNormal;
        double visualRadius = 0.0;
        if (fitPlaneFromPolyData(newMesh, m_leFortPlaneNormal,
                                 &visualCenter, &visualNormal, &visualRadius)) {
            m_leFortPlaneCenter = visualCenter;
            m_leFortPlaneNormal = visualNormal;
            if (std::isfinite(visualRadius) && visualRadius > 0.1)
                m_leFortPlaneRadius = visualRadius;
        } else {
            double maxRadius2 = 0.0;
            const double cx = m_leFortPlaneCenter.x();
            const double cy = m_leFortPlaneCenter.y();
            const double cz = m_leFortPlaneCenter.z();
            for (vtkIdType i = 0; i < newMesh->GetNumberOfPoints(); ++i) {
                double p[3] = {0.0, 0.0, 0.0};
                newMesh->GetPoint(i, p);
                const double dx = p[0] - cx;
                const double dy = p[1] - cy;
                const double dz = p[2] - cz;
                maxRadius2 = std::max(maxRadius2, dx * dx + dy * dy + dz * dz);
            }
            const double adjustedRadius = std::sqrt(maxRadius2);
            if (std::isfinite(adjustedRadius) && adjustedRadius > 0.1)
                m_leFortPlaneRadius = adjustedRadius;
        }
    } else if (mat && m_leFortPlaneRadius > 0.0) {
        const double sx = std::sqrt(std::pow(mat->GetElement(0, 0), 2.0) +
                                    std::pow(mat->GetElement(1, 0), 2.0) +
                                    std::pow(mat->GetElement(2, 0), 2.0));
        const double sy = std::sqrt(std::pow(mat->GetElement(0, 1), 2.0) +
                                    std::pow(mat->GetElement(1, 1), 2.0) +
                                    std::pow(mat->GetElement(2, 1), 2.0));
        const double sz = std::sqrt(std::pow(mat->GetElement(0, 2), 2.0) +
                                    std::pow(mat->GetElement(1, 2), 2.0) +
                                    std::pow(mat->GetElement(2, 2), 2.0));
        const double scale = std::max(sx, std::max(sy, sz));
        if (std::isfinite(scale) && scale > 0.0)
            m_leFortPlaneRadius *= scale;
    }

    // Re-enable landmark buttons and adjust-plane button; hide accept
    if (m_leFortPirDACt)        m_leFortPirDACt->setEnabled(true);
    if (m_leFortPirIAct)        m_leFortPirIAct->setEnabled(true);
    if (m_leFortPilaxDAct)      m_leFortPilaxDAct->setEnabled(true);
    if (m_leFortPilaxIAct)      m_leFortPilaxIAct->setEnabled(true);
    if (m_leFortAdjustPlaneAct) m_leFortAdjustPlaneAct->setEnabled(true);
    if (m_leFortGuidePropsAct)  m_leFortGuidePropsAct->setEnabled(true);
    if (m_leFortAcceptPlaneAct) m_leFortAcceptPlaneAct->setEnabled(false);
    const int leFortPointCount = static_cast<int>(
        std::count(m_leFortPointSet.begin(), m_leFortPointSet.end(), true));
    if (m_leFortSplitAct && leFortPointCount >= 4)
        m_leFortSplitAct->setEnabled(true);

    // Rebuild disc and cut-line at the new plane position
    rebuildLeFortPlaneFromState();

    statusBar()->showMessage(
        tr("Plano ajustado. Haga clic en 'Dividir' para aplicar la osteotomia."));
    updateLeFortWizardUi();
    updateOsteotomyWorkflowUi();
}

// ─────────────────────────────────────────────────────────────────────────────
// Clips the selected mesh at the plane defined by the 4 cephalometric landmarks.
void MainWindow::executeOsteotomySplit()
{
    const bool hasGenioSegmentsReady =
        m_leFortTargetLabel == kLowerCompositeLabel &&
        !m_genioPlaneNormal.isNull() &&
        m_genioPlaneVisualMesh &&
        m_genioPlaneVisualMesh->GetNumberOfPoints() > 0;
    if (hasGenioSegmentsReady) {
        executeGenioSplit();
        return;
    }

    if (m_leFortTargetLabel == kLowerCompositeLabel &&
        m_bssoGuideReady && m_bssoGuideVisualMesh &&
        m_bssoGuideVisualMesh->GetNumberOfPoints() > 0) {
        executeBssoSplit();
        return;
    }

    executeLeFortSplit();
}

void MainWindow::executeGenioSplit()
{
    const int pointCount = static_cast<int>(
        std::count(m_genioPointSet.begin(), m_genioPointSet.end(), true));
    if (pointCount < 4 || m_genioPlaneNormal.isNull()) {
        QMessageBox::warning(this, tr("Mentón"),
            tr("Marque apical derecho, basal derecho, apical izquierdo y basal izquierdo antes de dividir."));
        return;
    }

    vtkPolyData* targetMesh =
        (m_bssoDistalMesh && m_bssoDistalMesh->GetNumberOfCells() > 0)
            ? m_bssoDistalMesh.Get()
            : m_lowerCompositeMesh.Get();
    if (!targetMesh || targetMesh->GetNumberOfCells() == 0) {
        QMessageBox::warning(this, tr("Mentón"),
            tr("No hay segmento mandibular disponible para dividir el menton."));
        return;
    }

    QVector3D planeNormal = m_genioPlaneNormal.normalized();
    QVector3D planeOrigin = m_genioPlaneCenter;
    if (m_genioPlaneVisualMesh && m_genioPlaneVisualMesh->GetNumberOfPoints() > 0) {
        QVector3D visualCenter;
        QVector3D visualNormal;
        if (fitPlaneFromPolyData(m_genioPlaneVisualMesh, planeNormal,
                                 &visualCenter, &visualNormal, nullptr)) {
            planeOrigin = visualCenter;
            planeNormal = visualNormal;
        }
    }
    if (planeNormal.lengthSquared() < 1e-8f) {
        QMessageBox::warning(this, tr("Mentón"),
            tr("El plano de menton no es valido. Ajuste o marque nuevamente los 4 puntos."));
        return;
    }

    QVector3D basalDirection = m_genioGuideDepthAxis;
    if (basalDirection.lengthSquared() < 1e-8f && m_genioPoints.size() >= 4) {
        const QVector3D apicalMid = (m_genioPoints[0] + m_genioPoints[2]) * 0.5f;
        const QVector3D basalMid = (m_genioPoints[1] + m_genioPoints[3]) * 0.5f;
        basalDirection = basalMid - apicalMid;
    }
    if (basalDirection.lengthSquared() < 1e-8f)
        basalDirection = QVector3D(0.0f, 0.0f, -1.0f);
    basalDirection.normalize();

    const double kerfMm = std::clamp(m_genioCutThicknessMm, 0.2, 5.0);
    const double halfKerf = kerfMm * 0.5;

    auto clipPositive = [](vtkPolyData* input,
                           const QVector3D& normal,
                           const QVector3D& origin) -> vtkSmartPointer<vtkPolyData> {
        auto plane = vtkSmartPointer<vtkPlane>::New();
        plane->SetNormal(normal.x(), normal.y(), normal.z());
        plane->SetOrigin(origin.x(), origin.y(), origin.z());

        auto clip = vtkSmartPointer<vtkClipPolyData>::New();
        clip->SetInputData(input);
        clip->SetClipFunction(plane);
        clip->GenerateClippedOutputOff();
        clip->Update();

        auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
        clean->SetInputData(clip->GetOutput());
        clean->Update();

        auto normals = vtkSmartPointer<vtkPolyDataNormals>::New();
        normals->SetInputData(clean->GetOutput());
        normals->ConsistencyOn();
        normals->SplittingOff();
        normals->Update();

        auto out = vtkSmartPointer<vtkPolyData>::New();
        out->DeepCopy(normals->GetOutput());
        return out;
    };

    auto centroid = [](vtkPolyData* mesh) -> QVector3D {
        if (!mesh || mesh->GetNumberOfPoints() == 0)
            return QVector3D();
        double p[3] = {};
        QVector3D c;
        const vtkIdType step = std::max<vtkIdType>(1, mesh->GetNumberOfPoints() / 20000);
        vtkIdType count = 0;
        for (vtkIdType i = 0; i < mesh->GetNumberOfPoints(); i += step) {
            mesh->GetPoint(i, p);
            c += QVector3D(static_cast<float>(p[0]),
                           static_cast<float>(p[1]),
                           static_cast<float>(p[2]));
            ++count;
        }
        return count > 0 ? c / static_cast<float>(count) : QVector3D();
    };

    const QVector3D positiveOrigin = planeOrigin + planeNormal * static_cast<float>(halfKerf);
    const QVector3D negativeOrigin = planeOrigin - planeNormal * static_cast<float>(halfKerf);
    auto sideA = clipPositive(targetMesh, planeNormal, positiveOrigin);
    auto sideB = clipPositive(targetMesh, -planeNormal, negativeOrigin);

    if (!sideA || sideA->GetNumberOfCells() == 0 ||
        !sideB || sideB->GetNumberOfCells() == 0) {
        QMessageBox::warning(this, tr("Mentón"),
            tr("El plano de menton no atraviesa correctamente la mandibula. Ajustelo y vuelva a dividir."));
        return;
    }

    const double scoreA = QVector3D::dotProduct(centroid(sideA), basalDirection);
    const double scoreB = QVector3D::dotProduct(centroid(sideB), basalDirection);
    vtkSmartPointer<vtkPolyData> chinMesh = scoreA >= scoreB ? sideA : sideB;
    vtkSmartPointer<vtkPolyData> bodyMesh = scoreA >= scoreB ? sideB : sideA;

    if (!chinMesh || chinMesh->GetNumberOfCells() == 0 ||
        !bodyMesh || bodyMesh->GetNumberOfCells() == 0) {
        QMessageBox::warning(this, tr("Mentón"),
            tr("La division del menton genero un segmento vacio. Revise el plano."));
        return;
    }

    m_genioBodyMesh = vtkSmartPointer<vtkPolyData>::New();
    m_mandibleMovement = {};
    m_mandibleMovementResetMatrix.clear();
    m_genioBodyMesh->DeepCopy(bodyMesh);
    m_genioSegmentMesh = vtkSmartPointer<vtkPolyData>::New();
    m_genioSegmentMesh->DeepCopy(chinMesh);

    if (m_osteotomyView) {
        m_osteotomyView->clearMeshes();
        m_osteotomyView->clearPointMarkers();

        auto addToOsteotomy = [this](int label, vtkPolyData* mesh, double opacity = 1.0) {
            if (!mesh || mesh->GetNumberOfPoints() <= 0) return;
            const int key = objectActorKey(label);
            m_osteotomyView->addMesh(key, mesh, meshLabelName(label));
            m_osteotomyView->setMeshColor(key, objectColorForLabel(label));
            m_osteotomyView->setMeshOpacity(key, opacity);
            m_osteotomyView->setMeshVisible(key, objectEntryVisible(label));
        };

        addToOsteotomy(kLeFortCranialLabel, m_leFortCranialMesh);
        addToOsteotomy(kLeFortSegLabel, m_leFortSegmentMesh);
        addToOsteotomy(kUpperCompositeLabel, m_upperCompositeMesh);
        addToOsteotomy(kGenioBodyLabel, m_genioBodyMesh);
        addToOsteotomy(kGenioSegmentLabel, m_genioSegmentMesh);
        addToOsteotomy(kBssoProximalRightLabel, m_bssoRightProximalMesh);
        addToOsteotomy(kBssoProximalLeftLabel, m_bssoLeftProximalMesh);

        // After the genioplasty split the planning guide should no longer be visible;
        // the operative scene should show only the resulting bone segments.
        m_osteotomyView->removeMesh(kGenioPlaneLabel);
        m_osteotomyView->removeMesh(kGenioCutLineLabel);
        m_osteotomyView->render();
    }

    if (m_mesh3DView) {
        m_mesh3DView->addMesh(objectActorKey(kGenioBodyLabel), m_genioBodyMesh,
                              meshLabelName(kGenioBodyLabel));
        m_mesh3DView->setMeshColor(objectActorKey(kGenioBodyLabel),
                                   meshLabelColor(kGenioBodyLabel));
        m_mesh3DView->addMesh(objectActorKey(kGenioSegmentLabel), m_genioSegmentMesh,
                              meshLabelName(kGenioSegmentLabel));
        m_mesh3DView->setMeshColor(objectActorKey(kGenioSegmentLabel),
                                   meshLabelColor(kGenioSegmentLabel));
    }

    m_bssoDistalMesh = nullptr;   // superseded by genio body (212)
    removeObjectEntry(kBssoDistalLabel);
    removeObjectEntry(kGenioPlaneLabel);
    removeObjectEntry(kGenioCutLineLabel);
    removeObjectEntry(kGenioBodyLabel);
    removeObjectEntry(kGenioSegmentLabel);
    addObjectEntry(meshLabelName(kGenioBodyLabel), meshLabelColor(kGenioBodyLabel), kGenioBodyLabel);
    addObjectEntry(meshLabelName(kGenioSegmentLabel), meshLabelColor(kGenioSegmentLabel), kGenioSegmentLabel);
    syncVisibilityPanelToAllViews();

    LoggerCore::instance().logCustom(QStringLiteral("Menton"),
        QStringLiteral("Split: kerf_mm=%1, body_cells=%2, chin_cells=%3, origin=(%4,%5,%6)")
            .arg(kerfMm, 0, 'f', 1)
            .arg(m_genioBodyMesh->GetNumberOfCells())
            .arg(m_genioSegmentMesh->GetNumberOfCells())
            .arg(planeOrigin.x(), 0, 'f', 3)
            .arg(planeOrigin.y(), 0, 'f', 3)
            .arg(planeOrigin.z(), 0, 'f', 3));

    updateOsteotomyWorkflowUi();
    statusBar()->showMessage(
        tr("Osteotomia de menton aplicada con corte de %1 mm. Segmentos creados: mandibula post-menton y segmento menton.")
            .arg(kerfMm, 0, 'f', 1));
}

void MainWindow::executeBssoSplit()
{
    const bool leftSide = m_bssoActiveLeftSide;
    const int sideIndex = leftSide ? 1 : 0;
    const QString sideName = leftSide ? tr("izquierda") : tr("derecha");

    if (m_bssoSideSplitDone[static_cast<size_t>(sideIndex)]) {
        QMessageBox::information(this, tr("BSSO"),
            tr("La sagital %1 ya fue dividida. Use reset/recalcule la mandibula si desea repetir ese lado.")
                .arg(sideName));
        return;
    }

    vtkPolyData* targetMesh = (m_bssoDistalMesh && m_bssoDistalMesh->GetNumberOfCells() > 0)
        ? m_bssoDistalMesh.Get()
        : m_lowerCompositeMesh.Get();
    if (!targetMesh || targetMesh->GetNumberOfCells() == 0) {
        QMessageBox::warning(this, tr("BSSO"),
            tr("No hay modelo mandibular compuesto disponible para dividir."));
        return;
    }

    if (!m_bssoGuideVisualMesh || m_bssoGuideVisualMesh->GetNumberOfPoints() == 0) {
        QMessageBox::warning(this, tr("BSSO"),
            tr("Primero cree o ajuste la guia BSSO."));
        return;
    }

    double mandBounds[6] = {};
    targetMesh->GetBounds(mandBounds);

    const int p0 = leftSide ? 3 : 0;
    const int p1 = leftSide ? 4 : 1;
    const int p2 = leftSide ? 5 : 2;
    if (!m_bssoPointSet[static_cast<size_t>(p0)] ||
        !m_bssoPointSet[static_cast<size_t>(p1)] ||
        !m_bssoPointSet[static_cast<size_t>(p2)] ||
        m_bssoPoints.size() <= p2) {
        QMessageBox::warning(this, tr("BSSO"),
            tr("Marque 3 puntos en la sagital %1 para definir el plano de corte.")
                .arg(sideName));
        return;
    }

    const QVector3D a = m_bssoPoints[p0];
    const QVector3D b = m_bssoPoints[p1];
    const QVector3D c = m_bssoPoints[p2];
    QVector3D planeNormal = QVector3D::crossProduct(b - a, c - a);
    if (planeNormal.lengthSquared() < 1e-8f) {
        QMessageBox::warning(this, tr("BSSO"),
            tr("Los 3 puntos de la sagital %1 estan casi alineados. Marque un tercer punto mas separado.")
                .arg(sideName));
        return;
    }
    planeNormal.normalize();

    QVector3D planeOrigin = (a + b + c) / 3.0f;
    const double mandCenterX = (mandBounds[0] + mandBounds[1]) * 0.5;
    QVector3D centralDirection =
        planeOrigin.x() < static_cast<float>(mandCenterX)
            ? QVector3D(1.0f, 0.0f, 0.0f)
            : QVector3D(-1.0f, 0.0f, 0.0f);

    // If the user adjusted the BSSO plane with the gizmo, use the adjusted
    // visual plane for the actual split. This keeps preview and cut identical.
    if (m_bssoGuideVisualMesh && m_bssoGuideVisualMesh->GetNumberOfPoints() > 0) {
        QVector3D visualCenter;
        QVector3D visualNormal;
        if (fitPlaneFromPolyData(m_bssoGuideVisualMesh, planeNormal,
                                 &visualCenter, &visualNormal, nullptr)) {
            planeOrigin = visualCenter;
            planeNormal = visualNormal;
            centralDirection =
                planeOrigin.x() < static_cast<float>(mandCenterX)
                    ? QVector3D(1.0f, 0.0f, 0.0f)
                    : QVector3D(-1.0f, 0.0f, 0.0f);
        }
    }
    if (QVector3D::dotProduct(planeNormal, centralDirection) < 0.0f)
        planeNormal = -planeNormal;

    const double kerfMm = std::clamp(m_bssoCutThicknessMm, 0.2, 5.0);
    const double halfKerf = kerfMm * 0.5;

    auto clipPositive = [](vtkPolyData* input,
                           const QVector3D& normal,
                           const QVector3D& origin) -> vtkSmartPointer<vtkPolyData> {
        auto plane = vtkSmartPointer<vtkPlane>::New();
        plane->SetNormal(normal.x(), normal.y(), normal.z());
        plane->SetOrigin(origin.x(), origin.y(), origin.z());

        auto clip = vtkSmartPointer<vtkClipPolyData>::New();
        clip->SetInputData(input);
        clip->SetClipFunction(plane);
        clip->GenerateClippedOutputOff();
        clip->Update();

        auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
        clean->SetInputData(clip->GetOutput());
        clean->Update();

        auto normals = vtkSmartPointer<vtkPolyDataNormals>::New();
        normals->SetInputData(clean->GetOutput());
        normals->ConsistencyOn();
        normals->SplittingOff();
        normals->Update();

        auto out = vtkSmartPointer<vtkPolyData>::New();
        out->DeepCopy(normals->GetOutput());
        return out;
    };

    const QVector3D inner = planeOrigin + planeNormal * static_cast<float>(halfKerf);
    const QVector3D outer = planeOrigin - planeNormal * static_cast<float>(halfKerf);

    auto central = clipPositive(targetMesh, planeNormal, inner);
    auto newProximal = clipPositive(targetMesh, -planeNormal, outer);

    if (newProximal && newProximal->GetNumberOfCells() > 0) {
        if (leftSide) {
            m_bssoLeftProximalMesh = vtkSmartPointer<vtkPolyData>::New();
            m_bssoLeftProximalMesh->DeepCopy(newProximal);
        } else {
            m_bssoRightProximalMesh = vtkSmartPointer<vtkPolyData>::New();
            m_bssoRightProximalMesh->DeepCopy(newProximal);
        }
    }

    auto append = vtkSmartPointer<vtkAppendPolyData>::New();
    if (m_bssoRightProximalMesh && m_bssoRightProximalMesh->GetNumberOfCells() > 0)
        append->AddInputData(m_bssoRightProximalMesh);
    if (m_bssoLeftProximalMesh && m_bssoLeftProximalMesh->GetNumberOfCells() > 0)
        append->AddInputData(m_bssoLeftProximalMesh);
    append->Update();

    auto proxNormals = vtkSmartPointer<vtkPolyDataNormals>::New();
    proxNormals->SetInputData(append->GetOutput());
    proxNormals->ConsistencyOn();
    proxNormals->SplittingOff();
    proxNormals->Update();

    m_bssoDistalMesh = vtkSmartPointer<vtkPolyData>::New();
    m_mandibleMovement = {};
    m_mandibleMovementResetMatrix.clear();
    m_bssoDistalMesh->DeepCopy(central);
    m_bssoProximalMesh = vtkSmartPointer<vtkPolyData>::New();
    m_bssoProximalMesh->DeepCopy(proxNormals->GetOutput());

    if (!m_bssoDistalMesh || m_bssoDistalMesh->GetNumberOfCells() == 0 ||
        !newProximal || newProximal->GetNumberOfCells() == 0 ||
        !m_bssoProximalMesh || m_bssoProximalMesh->GetNumberOfCells() == 0) {
        QMessageBox::warning(this, tr("BSSO"),
            tr("La guia BSSO %1 no atraviesa correctamente la mandibula. Ajuste la guia y vuelva a dividir.")
                .arg(sideName));
        return;
    }

    m_bssoSideSplitDone[static_cast<size_t>(sideIndex)] = true;

    if (m_osteotomyView) {
        m_osteotomyView->clearMeshes();
        m_osteotomyView->clearPointMarkers();

        if (m_upperCompositeMesh && m_upperCompositeMesh->GetNumberOfPoints() > 0) {
            const int upperKey = objectActorKey(kUpperCompositeLabel);
            m_osteotomyView->addMesh(upperKey, m_upperCompositeMesh, meshLabelName(kUpperCompositeLabel));
            m_osteotomyView->setMeshColor(upperKey, objectColorForLabel(kUpperCompositeLabel));
            m_osteotomyView->setMeshVisible(upperKey, objectEntryVisible(kUpperCompositeLabel));
        }

        const int distalKey = objectActorKey(kBssoDistalLabel);
        m_osteotomyView->addMesh(distalKey, m_bssoDistalMesh, meshLabelName(kBssoDistalLabel));
        m_osteotomyView->setMeshColor(distalKey, objectColorForLabel(kBssoDistalLabel));
        if (m_bssoRightProximalMesh && m_bssoRightProximalMesh->GetNumberOfCells() > 0) {
            const int proxRightKey = objectActorKey(kBssoProximalRightLabel);
            m_osteotomyView->addMesh(proxRightKey, m_bssoRightProximalMesh,
                                     meshLabelName(kBssoProximalRightLabel));
            m_osteotomyView->setMeshColor(proxRightKey, objectColorForLabel(kBssoProximalRightLabel));
        }
        if (m_bssoLeftProximalMesh && m_bssoLeftProximalMesh->GetNumberOfCells() > 0) {
            const int proxLeftKey = objectActorKey(kBssoProximalLeftLabel);
            m_osteotomyView->addMesh(proxLeftKey, m_bssoLeftProximalMesh,
                                     meshLabelName(kBssoProximalLeftLabel));
            m_osteotomyView->setMeshColor(proxLeftKey, objectColorForLabel(kBssoProximalLeftLabel));
        }

        m_osteotomyView->addMesh(kBssoGuideLabel, m_bssoGuideVisualMesh, meshLabelName(kBssoGuideLabel));
        m_osteotomyView->setMeshColor(kBssoGuideLabel, objectColorForLabel(kBssoGuideLabel));
        m_osteotomyView->setMeshOpacity(kBssoGuideLabel, 0.35);
        ensureOsteotomyMeshesPresent();
        syncVisibilityPanelToAllViews();
        m_osteotomyView->render();
    }

    if (m_mesh3DView) {
        m_mesh3DView->addMesh(objectActorKey(kBssoDistalLabel), m_bssoDistalMesh,
                              meshLabelName(kBssoDistalLabel));
        m_mesh3DView->setMeshColor(objectActorKey(kBssoDistalLabel), meshLabelColor(kBssoDistalLabel));
        if (m_bssoRightProximalMesh && m_bssoRightProximalMesh->GetNumberOfCells() > 0) {
            m_mesh3DView->addMesh(objectActorKey(kBssoProximalRightLabel), m_bssoRightProximalMesh,
                                  meshLabelName(kBssoProximalRightLabel));
            m_mesh3DView->setMeshColor(objectActorKey(kBssoProximalRightLabel),
                                       meshLabelColor(kBssoProximalRightLabel));
        }
        if (m_bssoLeftProximalMesh && m_bssoLeftProximalMesh->GetNumberOfCells() > 0) {
            m_mesh3DView->addMesh(objectActorKey(kBssoProximalLeftLabel), m_bssoLeftProximalMesh,
                                  meshLabelName(kBssoProximalLeftLabel));
            m_mesh3DView->setMeshColor(objectActorKey(kBssoProximalLeftLabel),
                                       meshLabelColor(kBssoProximalLeftLabel));
        }
    }

    removeObjectEntry(kBssoDistalLabel);
    removeObjectEntry(kBssoProximalLabel);
    removeObjectEntry(kBssoProximalRightLabel);
    removeObjectEntry(kBssoProximalLeftLabel);
    addObjectEntry(meshLabelName(kBssoDistalLabel), meshLabelColor(kBssoDistalLabel), kBssoDistalLabel);
    if (m_bssoRightProximalMesh && m_bssoRightProximalMesh->GetNumberOfCells() > 0)
        addObjectEntry(meshLabelName(kBssoProximalRightLabel),
                       meshLabelColor(kBssoProximalRightLabel), kBssoProximalRightLabel);
    if (m_bssoLeftProximalMesh && m_bssoLeftProximalMesh->GetNumberOfCells() > 0)
        addObjectEntry(meshLabelName(kBssoProximalLeftLabel),
                       meshLabelColor(kBssoProximalLeftLabel), kBssoProximalLeftLabel);
    syncVisibilityPanelToAllViews();

    LoggerCore::instance().logCustom(QStringLiteral("BSSO"),
        QStringLiteral("Split side=%1: kerf_mm=%2, distal=%3, proximal_total=%4, plane_x=%5")
            .arg(leftSide ? QStringLiteral("left") : QStringLiteral("right"))
            .arg(kerfMm, 0, 'f', 1)
            .arg(m_bssoDistalMesh->GetNumberOfCells())
            .arg(m_bssoProximalMesh->GetNumberOfCells())
            .arg(planeOrigin.x(), 0, 'f', 3));

    const int otherSideIndex = leftSide ? 0 : 1;
    if (!m_bssoSideSplitDone[static_cast<size_t>(otherSideIndex)]) {
        setBssoActiveSide(!leftSide);
        m_bssoGuideReady = false;
        if (m_osteotomyView) {
            m_osteotomyView->removeMesh(kBssoGuideLabel);
            m_osteotomyView->render();
        }
        updateBssoPointStatus();
        statusBar()->showMessage(
            tr("BSSO %1 aplicada con corte de %2 mm. Ahora marque 3 puntos y divida el lado %3.")
                .arg(sideName)
                .arg(kerfMm, 0, 'f', 1)
                .arg(m_bssoActiveLeftSide ? tr("izquierdo") : tr("derecho")));
    } else {
        m_leFortTargetLabel = kLowerCompositeLabel;
        if (m_leFortTargMandAct) m_leFortTargMandAct->setChecked(true);
        m_bssoGuideReady = false;
        m_bssoGuideVisualMesh = nullptr;
        removeObjectEntry(kBssoGuideLabel);
        if (m_osteotomyView)
            m_osteotomyView->removeMesh(kBssoGuideLabel);
        syncVisibilityPanelToAllViews();
        updateOsteotomyWorkflowUi();
        updateBssoPointStatus();
        statusBar()->showMessage(
            tr("BSSO bilateral completada. Ahora marque apical/basal derecho e izquierdo para el menton."));
    }
}

void MainWindow::executeLeFortSplit()
{
    const int pointCount = static_cast<int>(
        std::count(m_leFortPointSet.begin(), m_leFortPointSet.end(), true));
    if (pointCount < 4 || m_leFortPlaneNormal.isNull()) {
        QMessageBox::warning(this, tr("Le Fort I"),
            tr("Marque los 4 puntos Le Fort I antes de dividir."));
        return;
    }

    vtkPolyData* targetMesh = (m_leFortTargetLabel == kUpperCompositeLabel)
        ? m_upperCompositeMesh.Get() : m_lowerCompositeMesh.Get();
    if (!targetMesh) {
        QMessageBox::warning(this, tr("Le Fort I"),
            tr("El modelo seleccionado no esta disponible."));
        return;
    }

    // Use the stored plane state (initialised from landmarks, possibly adjusted by gizmo)
    const QVector3D& normal = m_leFortPlaneNormal;
    const QVector3D& center = m_leFortPlaneCenter;

    if (normal.isNull()) {
        QMessageBox::warning(this, tr("Le Fort I"),
            tr("El plano de corte no esta definido. Marque los puntos primero."));
        return;
    }

    statusBar()->showMessage(tr("Aplicando osteotomia Le Fort I…"));
    QApplication::processEvents();

    const QVector3D splitNormal = normal.normalized();
    const double kOsteotomyKerfMm = std::clamp(m_leFortCutThicknessMm, 0.2, 5.0);
    const double kHalfKerfMm = kOsteotomyKerfMm * 0.5;

    const QVector3D cranialPlaneOrigin = center + splitNormal * static_cast<float>(kHalfKerfMm);
    const QVector3D segmentPlaneOrigin = center - splitNormal * static_cast<float>(kHalfKerfMm);

    auto cranialPlane = vtkSmartPointer<vtkPlane>::New();
    cranialPlane->SetNormal(splitNormal.x(), splitNormal.y(), splitNormal.z());
    cranialPlane->SetOrigin(cranialPlaneOrigin.x(),
                            cranialPlaneOrigin.y(),
                            cranialPlaneOrigin.z());

    auto cranialClipper = vtkSmartPointer<vtkClipPolyData>::New();
    cranialClipper->SetInputData(targetMesh);
    cranialClipper->SetClipFunction(cranialPlane);
    cranialClipper->GenerateClippedOutputOff();
    cranialClipper->Update();

    auto segmentPlane = vtkSmartPointer<vtkPlane>::New();
    segmentPlane->SetNormal(splitNormal.x(), splitNormal.y(), splitNormal.z());
    segmentPlane->SetOrigin(segmentPlaneOrigin.x(),
                            segmentPlaneOrigin.y(),
                            segmentPlaneOrigin.z());

    auto segmentClipper = vtkSmartPointer<vtkClipPolyData>::New();
    segmentClipper->SetInputData(targetMesh);
    segmentClipper->SetClipFunction(segmentPlane);
    segmentClipper->GenerateClippedOutputOn();
    segmentClipper->Update();

    auto recomputeNormals = [](vtkPolyData* in) -> vtkSmartPointer<vtkPolyData> {
        auto norms = vtkSmartPointer<vtkPolyDataNormals>::New();
        norms->SetInputData(in);
        norms->ConsistencyOn(); norms->SplittingOff();
        norms->Update();
        auto out = vtkSmartPointer<vtkPolyData>::New();
        out->DeepCopy(norms->GetOutput());
        return out;
    };

    m_leFortCranialMesh = recomputeNormals(cranialClipper->GetOutput());        // positive side, 0.5 mm above plane
    m_leFortSegmentMesh = recomputeNormals(segmentClipper->GetClippedOutput()); // negative side, 0.5 mm below plane

    if (!m_leFortCranialMesh || m_leFortCranialMesh->GetNumberOfCells() == 0 ||
        !m_leFortSegmentMesh || m_leFortSegmentMesh->GetNumberOfCells() == 0) {
        QMessageBox::warning(this, tr("Le Fort I"),
            tr("El plano no atraviesa el modelo. Reposicione los puntos."));
        m_leFortCranialMesh = m_leFortSegmentMesh = nullptr;
        return;
    }

    // ── Repopulate osteotomy view with the two coloured segments ────────────
    m_osteotomyView->clearMeshes();
    m_osteotomyView->clearPointMarkers();

    const int cranialActorKey = objectActorKey(kLeFortCranialLabel);
    const int segActorKey     = objectActorKey(kLeFortSegLabel);

    m_osteotomyView->addMesh(cranialActorKey, m_leFortCranialMesh,
                             meshLabelName(kLeFortCranialLabel));
    m_osteotomyView->setMeshColor(cranialActorKey, objectColorForLabel(kLeFortCranialLabel));

    m_osteotomyView->addMesh(segActorKey, m_leFortSegmentMesh,
                             meshLabelName(kLeFortSegLabel));
    m_osteotomyView->setMeshColor(segActorKey, objectColorForLabel(kLeFortSegLabel));

    // Keep the non-cut composite visible alongside
    const int otherLabel = (m_leFortTargetLabel == kUpperCompositeLabel)
                           ? kLowerCompositeLabel : kUpperCompositeLabel;
    vtkPolyData* otherMesh = (otherLabel == kLowerCompositeLabel)
                             ? m_lowerCompositeMesh.Get() : m_upperCompositeMesh.Get();
    if (otherMesh) {
        const int k = objectActorKey(otherLabel);
        m_osteotomyView->addMesh(k, otherMesh, meshLabelName(otherLabel));
        m_osteotomyView->setMeshColor(k, objectColorForLabel(otherLabel));
        m_osteotomyView->setMeshVisible(k, objectEntryVisible(otherLabel));
    }

    // ── Register in the object panel so the user can toggle/recolor them ────
    // Remove stale entries first (in case the user re-runs the split)
    if (m_mesh3DView) {
        m_mesh3DView->addMesh(cranialActorKey, m_leFortCranialMesh,
                              meshLabelName(kLeFortCranialLabel));
        m_mesh3DView->setMeshColor(cranialActorKey, objectColorForLabel(kLeFortCranialLabel));
        m_mesh3DView->addMesh(segActorKey, m_leFortSegmentMesh,
                              meshLabelName(kLeFortSegLabel));
        m_mesh3DView->setMeshColor(segActorKey, objectColorForLabel(kLeFortSegLabel));
    }

    removeObjectEntry(kLeFortCranialLabel);
    removeObjectEntry(kLeFortSegLabel);
    addObjectEntry(meshLabelName(kLeFortCranialLabel),
                   meshLabelColor(kLeFortCranialLabel), kLeFortCranialLabel);
    addObjectEntry(meshLabelName(kLeFortSegLabel),
                   meshLabelColor(kLeFortSegLabel), kLeFortSegLabel);
    syncVisibilityPanelToAllViews();

    if (m_leFortExportAct) m_leFortExportAct->setEnabled(true);

    statusBar()->showMessage(
        tr("Le Fort I: corte de %1 mm | Base craneal = %2 celdas  |  Segmento = %3 celdas")
            .arg(kOsteotomyKerfMm, 0, 'f', 1)
            .arg(m_leFortCranialMesh->GetNumberOfCells())
            .arg(m_leFortSegmentMesh->GetNumberOfCells()));

    LoggerCore::instance().logCustom(QStringLiteral("LEFORT_I"),
        QStringLiteral("Split: kerf_mm=%1, cranial=%2, segment=%3")
            .arg(kOsteotomyKerfMm, 0, 'f', 1)
            .arg(m_leFortCranialMesh->GetNumberOfCells())
            .arg(m_leFortSegmentMesh->GetNumberOfCells()));

    setLeFortWizardStep(4);
    if (m_lowerCompositeMesh && m_lowerCompositeMesh->GetNumberOfPoints() > 0) {
        m_leFortTargetLabel = kLowerCompositeLabel;
        if (m_leFortTargMandAct) {
            const QSignalBlocker blocker(m_leFortTargMandAct);
            m_leFortTargMandAct->setChecked(true);
        }
        setBssoActiveSide(false);
        updateBssoPointStatus();
        statusBar()->showMessage(
            tr("Le Fort I aplicado. Continue con BSSO derecha: marque 3 puntos para el plano sagital."));
    } else {
        updateOsteotomyWorkflowUi();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Exports the three segments (base craneal, segmento Le Fort I, mandíbula)
// as separate binary STL files into a user-chosen directory.
void MainWindow::exportLeFortSegments()
{
    if (!m_leFortCranialMesh || !m_leFortSegmentMesh) {
        QMessageBox::warning(this, tr("Le Fort I"),
            tr("Primero ejecute la division Le Fort I con el boton 'Dividir'."));
        return;
    }

    const QString dir = QFileDialog::getExistingDirectory(
        this,
        tr("Carpeta para exportar segmentos Le Fort I"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
    if (dir.isEmpty()) return;

    QStringList failed;
    int openMeshes = 0;
    auto writeStl = [&failed, &openMeshes](vtkPolyData* mesh, const QString& path) {
        auto w = vtkSmartPointer<vtkSTLWriter>::New();
        w->SetFileTypeToBinary();
        w->SetFileName(QFile::encodeName(path).constData());
        w->SetInputData(mesh);
        if (w->Write() == 0)
            failed << QFileInfo(path).fileName();
        else if (!MeshRepairCore::Analyze(mesh).Valid())
            ++openMeshes;
    };

    writeStl(m_leFortCranialMesh,  dir + "/01_base_craneal.stl");
    writeStl(m_leFortSegmentMesh,  dir + "/02_segmento_lefort_I.stl");
    // Export the non-cut composite (mandible if upper was cut, or upper if lower was cut)
    const int otherLabel = (m_leFortTargetLabel == kUpperCompositeLabel)
                           ? kLowerCompositeLabel : kUpperCompositeLabel;
    vtkPolyData* otherMesh = (otherLabel == kLowerCompositeLabel)
                             ? m_lowerCompositeMesh.Get() : m_upperCompositeMesh.Get();
    if (otherMesh)
        writeStl(otherMesh, dir + "/03_" + meshLabelName(otherLabel).replace(' ', '_') + ".stl");

    if (!failed.isEmpty()) {
        QMessageBox::warning(this, tr("Le Fort I"), tr("No se pudieron escribir: %1").arg(failed.join(QStringLiteral(", "))));
        return;
    }
    statusBar()->showMessage(openMeshes == 0
        ? tr("Segmentos Le Fort I exportados en: %1 · STL válidos.").arg(dir)
        : tr("Segmentos Le Fort I exportados en: %1 · %2 malla(s) abiertas: repárelas antes de imprimir.").arg(dir).arg(openMeshes));
    LoggerCore::instance().logCustom(QStringLiteral("LEFORT_I_EXPORT"), dir);
}

// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::exportOrientedCompositeStl()
{
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Exportar modelo compuesto orientado"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        tr("STL files (*.stl)"));
    if (path.isEmpty()) return;

    // Merge both composites (if available) into a single STL
    auto append = vtkSmartPointer<vtkAppendPolyData>::New();
    bool hasData = false;
    if (m_upperCompositeMesh) { append->AddInputData(m_upperCompositeMesh); hasData = true; }
    if (m_lowerCompositeMesh) { append->AddInputData(m_lowerCompositeMesh); hasData = true; }
    if (!hasData) {
        QMessageBox::warning(this, tr("Export"), tr("No hay modelos compuestos para exportar."));
        return;
    }
    append->Update();

    auto normals = vtkSmartPointer<vtkPolyDataNormals>::New();
    normals->SetInputConnection(append->GetOutputPort());
    normals->ConsistencyOn();
    normals->SplittingOff();
    normals->AutoOrientNormalsOn();
    normals->Update();

    auto writer = vtkSmartPointer<vtkSTLWriter>::New();
    writer->SetFileName(path.toStdString().c_str());
    writer->SetInputConnection(normals->GetOutputPort());
    writer->SetFileTypeToBinary();
    if (writer->Write() == 0) {
        QMessageBox::critical(this, tr("Export"), tr("Error al escribir el archivo STL."));
        return;
    }

    LoggerCore::instance().logExport(path, QStringLiteral("oriented_composite"));
    statusBar()->showMessage(tr("Modelo orientado exportado: %1").arg(path));
}

// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::clearDentalRegistrationPoints()
{
    m_maxillaBonePoints.clear();
    m_upperArchPoints.clear();
    m_mandibleBonePoints.clear();
    m_lowerArchPoints.clear();
    m_dentalPointSet = DentalPointSet::None;
    if (m_mesh3DView) {
        m_mesh3DView->setPointPickMode(false);
        m_mesh3DView->clearPointMarkers();
    }
    for (Mesh3DView* v : {m_modelMaxillaView, m_modelUpperArchView,
                          m_modelMandibleView, m_modelLowerArchView,
                          m_modelMatchView}) {
        if (!v) continue;
        v->setPointPickMode(false);
        v->clearPointMarkers();
    }
    updateButtonStates();
    statusBar()->showMessage(tr("Puntos de registro limpiados."));
}

void MainWindow::resetDentalArchTransforms()
{
    commitActiveDentalGizmos(false);

    bool resetAny = false;
    if (m_upperArchOriginalMesh) {
        m_upperArchMesh = vtkSmartPointer<vtkPolyData>::New();
        m_upperArchMesh->DeepCopy(m_upperArchOriginalMesh);
        const int actorKey = objectActorKey(kUpperArchLabel);
        const QColor color = objectColorForLabel(kUpperArchLabel);
        for (Mesh3DView* v : {m_mesh3DView, m_modelUpperArchView, m_modelMatchView}) {
            if (!v) continue;
            v->addMesh(actorKey, m_upperArchMesh, meshLabelName(kUpperArchLabel));
            v->setMeshColor(actorKey, color);
            v->setMeshOpacity(actorKey, 1.0);
        }
        m_upperArchPoints.clear();
        resetAny = true;
    }
    if (m_lowerArchOriginalMesh) {
        m_lowerArchMesh = vtkSmartPointer<vtkPolyData>::New();
        m_lowerArchMesh->DeepCopy(m_lowerArchOriginalMesh);
        const int actorKey = objectActorKey(kLowerArchLabel);
        const QColor color = objectColorForLabel(kLowerArchLabel);
        for (Mesh3DView* v : {m_mesh3DView, m_modelLowerArchView, m_modelMatchView}) {
            if (!v) continue;
            v->addMesh(actorKey, m_lowerArchMesh, meshLabelName(kLowerArchLabel));
            v->setMeshColor(actorKey, color);
            v->setMeshOpacity(actorKey, 1.0);
        }
        m_lowerArchPoints.clear();
        resetAny = true;
    }
    m_upperCompositeMesh = nullptr;
    m_lowerCompositeMesh = nullptr;
    m_upperArchRegistrationMatrix = identityMatrix();
    m_lowerArchRegistrationMatrix = identityMatrix();
    m_upperRegistrationReport.clear();
    m_lowerRegistrationReport.clear();
    m_upperRegistrationCalculated = false;
    m_lowerRegistrationCalculated = false;
    m_upperRegResult = {};
    m_lowerRegResult = {};

    // Phase 1: update state after reset
    m_appState.setUpperRegistered(false);
    m_appState.setLowerRegistered(false);
    m_appState.setUpperCompositeReady(false);
    m_appState.setLowerCompositeReady(false);
    updateButtonStates();

    clearDentalRegistrationPoints();
    syncModelViews();
    statusBar()->showMessage(resetAny
        ? tr("STL dentales restaurados a geometria original.")
        : tr("No hay STL original para restaurar."));
}

void MainWindow::alignUpperArchToMaxilla()
{
    if (!m_upperArchMesh) {
        QMessageBox::warning(this, tr("Match superior"), tr("Importe primero el STL del arco superior."));
        return;
    }
    auto maxilla = meshForAnatomicLabel(5);
    if (!maxilla) {
        QMessageBox::warning(this, tr("Match superior"), tr("Calcule o genere primero la malla del maxilar."));
        return;
    }
    logRegistrationDiagnostics(tr("Maxilar / arco superior"), maxilla, m_upperArchMesh);

    QString error;
    auto landmarkMatrix = identityMatrix();
    auto icpMatrix = identityMatrix();
    double landmarkRms = 0.0;
    auto transformed = transformMesh(m_upperArchMesh, m_upperArchPoints, m_maxillaBonePoints,
                                     &error, landmarkMatrix, &landmarkRms);
    if (!transformed) {
        QMessageBox::warning(this, tr("Match superior"), error);
        return;
    }
    QString icpReport;
    QString icpError;
    auto refined = refineArchWithIcp(transformed, maxilla, true, &icpReport, &icpError, icpMatrix);
    if (refined) {
        transformed = refined;
    } else if (!icpError.isEmpty()) {
        statusBar()->showMessage(tr("ICP superior omitido: %1").arg(icpError));
    }

    auto priorMatrix = m_upperArchRegistrationMatrix
        ? TransformCore::CloneMatrix(m_upperArchRegistrationMatrix)
        : TransformCore::IdentityMatrix();
    auto finalMatrix = TransformCore::ComposeTransforms(
        {priorMatrix.GetPointer(), landmarkMatrix.GetPointer(), icpMatrix.GetPointer()});
    m_upperArchRegistrationMatrix = finalMatrix;
    m_upperRegistrationCalculated = true;
    m_upperRegistrationReport =
        tr("Arcada: maxilar\n"
           "Target TAC: Maxilar segmentado (label 5)\n"
           "Landmark RMS: %1 mm\n"
           "%2\n\n"
           "Matriz inicial landmarks:\n%3\n\n"
           "Matriz ICP:\n%4\n\n"
           "Matriz final T_maxilla_STL_to_CT:\n%5\n")
            .arg(landmarkRms, 0, 'f', 3)
            .arg(icpReport.isEmpty() ? tr("ICP ROI: omitido o sin reporte") : icpReport)
            .arg(matrixToText(landmarkMatrix))
            .arg(matrixToText(icpMatrix))
            .arg(matrixToText(finalMatrix));
    qInfo().noquote() << "Matriz final T_maxilla_STL_to_CT:";
    qInfo().noquote() << matrixToText(finalMatrix);

    if (m_upperArchOriginalMesh) {
        if (auto resolved = TransformCore::ApplyTransformToPolyData(m_upperArchOriginalMesh, finalMatrix))
            transformed = resolved;
    }

    // ── Phase 5: compute point-to-surface metrics ──────────────────────────
    double meanDist = 0.0, maxDist = 0.0, p95Dist = 0.0;
    RegistrationResult::computeMetrics(transformed, maxilla, meanDist, maxDist, p95Dist);

    // ── Phase 5: metrics in the status bar (no confirmation dialog: the fine
    // adjustment and the mandatory composite review follow) ────────────────
    statusBar()->showMessage(
        tr("Registro superior aplicado: LM-RMS %1 mm · media %2 mm · P95 %3 mm · máx %4 mm.")
            .arg(landmarkRms, 0, 'f', 2).arg(meanDist, 0, 'f', 2).arg(p95Dist, 0, 'f', 2).arg(maxDist, 0, 'f', 2));

    // ── Accept: apply transformed mesh ────────────────────────────────────
    m_upperArchMesh = transformed;

    // ── Phase 5: store result ──────────────────────────────────────────────
    m_upperRegResult.landmarkRms  = landmarkRms;
    m_upperRegResult.meanDistance = meanDist;
    m_upperRegResult.maxDistance  = maxDist;
    m_upperRegResult.p95Distance  = p95Dist;
    m_upperRegResult.accepted     = true;
    m_upperRegResult.report       = m_upperRegistrationReport;
    m_upperRegResult.matrix       = TransformCore::MatrixToVector(finalMatrix);

    // ── Phase 4: log accepted registration ────────────────────────────────
    LoggerCore::instance().logRegistration(
        QStringLiteral("upper"), landmarkRms, meanDist, p95Dist, true);

    const int actorKey = objectActorKey(kUpperArchLabel);
    const QColor color = objectColorForLabel(kUpperArchLabel);
    if (m_mesh3DView) {
        m_mesh3DView->addMesh(actorKey, transformed, meshLabelName(kUpperArchLabel));
        m_mesh3DView->setMeshColor(actorKey, color);
        m_mesh3DView->setMeshOpacity(actorKey, 1.0);
    }
    if (m_modelUpperArchView) {
        m_modelUpperArchView->addMesh(actorKey, transformed, meshLabelName(kUpperArchLabel));
        m_modelUpperArchView->setMeshColor(actorKey, color);
    }
    addObjectEntry(tr("Arco superior STL registrado"), color, kUpperArchLabel);
    m_maxillaBonePoints.clear();
    m_upperArchPoints.clear();
    if (m_mesh3DView) m_mesh3DView->clearPointMarkers();
    for (Mesh3DView* v : {m_modelMaxillaView, m_modelUpperArchView,
                          m_modelMandibleView, m_modelLowerArchView,
                          m_modelMatchView})
        if (v) v->clearPointMarkers();
    setDentalPointCapture(DentalPointSet::None);

    // ── Phase 1: mark upper as registered ─────────────────────────────────
    m_appState.setUpperRegistered(true);
    updateButtonStates();

    syncModelViews();
    startDentalAdjustmentGizmo(actorKey);
}

void MainWindow::alignLowerArchToMandible()
{
    if (!m_lowerArchMesh) {
        QMessageBox::warning(this, tr("Match inferior"), tr("Importe primero el STL del arco inferior."));
        return;
    }
    auto mandible = meshForAnatomicLabel(6);
    if (!mandible) {
        QMessageBox::warning(this, tr("Match inferior"), tr("Calcule o genere primero la malla de la mandibula."));
        return;
    }
    logRegistrationDiagnostics(tr("Mandibula / arco inferior"), mandible, m_lowerArchMesh);

    QString error;
    auto landmarkMatrix = identityMatrix();
    auto icpMatrix = identityMatrix();
    double landmarkRms = 0.0;
    auto transformed = transformMesh(m_lowerArchMesh, m_lowerArchPoints, m_mandibleBonePoints,
                                     &error, landmarkMatrix, &landmarkRms);
    if (!transformed) {
        QMessageBox::warning(this, tr("Match inferior"), error);
        return;
    }
    QString icpReport;
    QString icpError;
    auto refined = refineArchWithIcp(transformed, mandible, false, &icpReport, &icpError, icpMatrix);
    if (refined) {
        transformed = refined;
    } else if (!icpError.isEmpty()) {
        statusBar()->showMessage(tr("ICP inferior omitido: %1").arg(icpError));
    }

    auto priorMatrix = m_lowerArchRegistrationMatrix
        ? TransformCore::CloneMatrix(m_lowerArchRegistrationMatrix)
        : TransformCore::IdentityMatrix();
    auto finalMatrix = TransformCore::ComposeTransforms(
        {priorMatrix.GetPointer(), landmarkMatrix.GetPointer(), icpMatrix.GetPointer()});
    m_lowerArchRegistrationMatrix = finalMatrix;
    m_lowerRegistrationCalculated = true;
    m_lowerRegistrationReport =
        tr("Arcada: mandibula\n"
           "Target TAC: Mandibula segmentada (label 6)\n"
           "Landmark RMS: %1 mm\n"
           "%2\n\n"
           "Matriz inicial landmarks:\n%3\n\n"
           "Matriz ICP:\n%4\n\n"
           "Matriz final T_mandible_STL_to_CT:\n%5\n")
            .arg(landmarkRms, 0, 'f', 3)
            .arg(icpReport.isEmpty() ? tr("ICP ROI: omitido o sin reporte") : icpReport)
            .arg(matrixToText(landmarkMatrix))
            .arg(matrixToText(icpMatrix))
            .arg(matrixToText(finalMatrix));
    qInfo().noquote() << "Matriz final T_mandible_STL_to_CT:";
    qInfo().noquote() << matrixToText(finalMatrix);

    if (m_lowerArchOriginalMesh) {
        if (auto resolved = TransformCore::ApplyTransformToPolyData(m_lowerArchOriginalMesh, finalMatrix))
            transformed = resolved;
    }

    // ── Phase 5: compute point-to-surface metrics ──────────────────────────
    double meanDist = 0.0, maxDist = 0.0, p95Dist = 0.0;
    RegistrationResult::computeMetrics(transformed, mandible, meanDist, maxDist, p95Dist);

    // ── Phase 5: metrics in the status bar (no confirmation dialog: the fine
    // adjustment and the mandatory composite review follow) ────────────────
    statusBar()->showMessage(
        tr("Registro inferior aplicado: LM-RMS %1 mm · media %2 mm · P95 %3 mm · máx %4 mm.")
            .arg(landmarkRms, 0, 'f', 2).arg(meanDist, 0, 'f', 2).arg(p95Dist, 0, 'f', 2).arg(maxDist, 0, 'f', 2));

    // ── Accept: apply transformed mesh ────────────────────────────────────
    m_lowerArchMesh = transformed;

    // ── Phase 5: store result ──────────────────────────────────────────────
    m_lowerRegResult.landmarkRms  = landmarkRms;
    m_lowerRegResult.meanDistance = meanDist;
    m_lowerRegResult.maxDistance  = maxDist;
    m_lowerRegResult.p95Distance  = p95Dist;
    m_lowerRegResult.accepted     = true;
    m_lowerRegResult.report       = m_lowerRegistrationReport;
    m_lowerRegResult.matrix       = TransformCore::MatrixToVector(finalMatrix);

    // ── Phase 4: log accepted registration ────────────────────────────────
    LoggerCore::instance().logRegistration(
        QStringLiteral("lower"), landmarkRms, meanDist, p95Dist, true);

    const int actorKey = objectActorKey(kLowerArchLabel);
    const QColor color = objectColorForLabel(kLowerArchLabel);
    if (m_mesh3DView) {
        m_mesh3DView->addMesh(actorKey, transformed, meshLabelName(kLowerArchLabel));
        m_mesh3DView->setMeshColor(actorKey, color);
        m_mesh3DView->setMeshOpacity(actorKey, 1.0);
    }
    if (m_modelLowerArchView) {
        m_modelLowerArchView->addMesh(actorKey, transformed, meshLabelName(kLowerArchLabel));
        m_modelLowerArchView->setMeshColor(actorKey, color);
    }
    addObjectEntry(tr("Arco inferior STL registrado"), color, kLowerArchLabel);
    m_mandibleBonePoints.clear();
    m_lowerArchPoints.clear();
    if (m_mesh3DView) m_mesh3DView->clearPointMarkers();
    for (Mesh3DView* v : {m_modelMaxillaView, m_modelUpperArchView,
                          m_modelMandibleView, m_modelLowerArchView,
                          m_modelMatchView})
        if (v) v->clearPointMarkers();
    setDentalPointCapture(DentalPointSet::None);

    // ── Phase 1: mark lower as registered ─────────────────────────────────
    m_appState.setLowerRegistered(true);
    updateButtonStates();

    syncModelViews();
    startDentalAdjustmentGizmo(actorKey);
}

void MainWindow::alignBothDentalArches()
{
    const bool canUpper = m_upperArchMesh && meshForAnatomicLabel(5) &&
                          m_upperArchPoints.size() >= 3 &&
                          m_upperArchPoints.size() == m_maxillaBonePoints.size();
    const bool canLower = m_lowerArchMesh && meshForAnatomicLabel(6) &&
                          m_lowerArchPoints.size() >= 3 &&
                          m_lowerArchPoints.size() == m_mandibleBonePoints.size();

    if (!canUpper && !canLower) {
        QMessageBox::warning(this, tr("Match ambos"),
            tr("Marque al menos 3 pares de puntos homologos para maxilar y/o mandibula antes de registrar."));
        return;
    }

    if (canUpper) {
        if (m_modelStepStack) m_modelStepStack->setCurrentIndex(0);
        alignUpperArchToMaxilla();
    }
    if (canLower) {
        if (m_modelStepStack) m_modelStepStack->setCurrentIndex(1);
        alignLowerArchToMandible();
    }
    statusBar()->showMessage(tr("Registro TAC-STL por arcada terminado."));
}

void MainWindow::startDentalAdjustmentGizmo(int actorKey)
{
    const bool upper = actorKey == objectActorKey(kUpperArchLabel);
    const bool lower = actorKey == objectActorKey(kLowerArchLabel);
    if (!upper && !lower) return;

    commitActiveDentalGizmos(false);
    setDentalPointCapture(DentalPointSet::None);
    deactivateLassoTools();

    if (m_modelStepStack)
        m_modelStepStack->setCurrentIndex(upper ? 0 : 1);

    syncModelViews();

    Mesh3DView* targetView = m_modelMatchView ? m_modelMatchView : m_mesh3DView;
    if (!targetView || !targetView->meshData(actorKey)) {
        statusBar()->showMessage(tr("No se encontro el STL registrado en la vista de match."));
        return;
    }

    targetView->startGizmo(actorKey);
    if (!targetView->hasGizmo()) {
        statusBar()->showMessage(tr("No se pudo activar el gizmo del STL."));
        return;
    }

    if (m_viewFullScreen && m_fullScreenView != targetView)
        exitViewFullScreen();
    if (!m_viewFullScreen)
        toggleViewFullScreen(targetView);

    m_dentalGizmoActive = true;
    updateButtonStates();
    statusBar()->showMessage(upper
        ? tr("Ajuste manual activo para STL superior. Mueva/rote el gizmo y pulse Aceptar Ajuste o Crear Comp.")
        : tr("Ajuste manual activo para STL inferior. Mueva/rote el gizmo y pulse Aceptar Ajuste o Crear Comp."));
}

void MainWindow::commitActiveDentalGizmos(bool showReview)
{
    bool committed = false;
    for (Mesh3DView* view : {m_mesh3DView, m_modelMaxillaView, m_modelUpperArchView,
                             m_modelMandibleView, m_modelLowerArchView, m_modelMatchView}) {
        if (view && view->hasGizmo()) {
            view->stopGizmo();
            committed = true;
        }
    }

    m_dentalGizmoActive = false;
    updateButtonStates();
    if (!committed) return;

    statusBar()->showMessage(tr("Ajuste manual del STL aplicado."));

    // Show registration review only when in the MODELOS workspace and the user
    // just completed a dental arch registration (no composite yet for that jaw).
    const int step = m_modelStepStack ? m_modelStepStack->currentIndex() : -1;
    const bool upperJustRegistered = (step == 0)
        && m_upperRegistrationCalculated
        && m_upperArchMesh
        && !m_upperCompositeMesh;
    const bool lowerJustRegistered = (step == 1)
        && m_lowerRegistrationCalculated
        && m_lowerArchMesh
        && !m_lowerCompositeMesh;

    if (!showReview) return;

    if (upperJustRegistered)
        showRegistrationReview(0);
    else if (lowerJustRegistered)
        showRegistrationReview(1);
}

void MainWindow::onDentalGizmoMeshUpdated(int actorKey, vtkSmartPointer<vtkPolyData> newMesh)
{
    if (!newMesh) return;

    const bool upper = actorKey == objectActorKey(kUpperArchLabel);
    const bool lower = actorKey == objectActorKey(kLowerArchLabel);
    if (!upper && !lower) return;

    auto baked = vtkSmartPointer<vtkPolyData>::New();
    baked->DeepCopy(newMesh);

    const int objectLabel = upper ? kUpperArchLabel : kLowerArchLabel;
    const QColor color = meshLabelColor(objectLabel);
    auto* sourceView = qobject_cast<Mesh3DView*>(sender());
    auto manualDelta = sourceView
        ? sourceView->lastGizmoTransformMatrix()
        : TransformCore::IdentityMatrix();
    QString manualReport;
    const bool hasRigidManualDelta =
        manualDelta &&
        !TransformCore::IsIdentity(manualDelta) &&
        TransformCore::ValidateRigidTransform(manualDelta, &manualReport, 2e-3);

    if (upper) {
        m_upperArchMesh = baked;
        if (hasRigidManualDelta && m_upperArchRegistrationMatrix) {
            m_upperArchRegistrationMatrix = TransformCore::ComposeTransforms(
                {m_upperArchRegistrationMatrix.GetPointer(), manualDelta.GetPointer()});
            m_upperRegResult.matrix = TransformCore::MatrixToVector(m_upperArchRegistrationMatrix);
        } else if (manualDelta && !TransformCore::IsIdentity(manualDelta)) {
            qWarning().noquote() << "Ajuste manual superior no rigido; no se agrego a la matriz:"
                                 << manualReport;
        }
        m_upperCompositeMesh = nullptr;
        m_appState.setUpperCompositeReady(false);
        removeObjectEntry(kUpperCompositeLabel);
        if (m_upperRegistrationCalculated)
            m_upperRegistrationReport += tr("\nAjuste manual con gizmo aplicado sobre la malla STL registrada.\n");
    } else {
        m_lowerArchMesh = baked;
        if (hasRigidManualDelta && m_lowerArchRegistrationMatrix) {
            m_lowerArchRegistrationMatrix = TransformCore::ComposeTransforms(
                {m_lowerArchRegistrationMatrix.GetPointer(), manualDelta.GetPointer()});
            m_lowerRegResult.matrix = TransformCore::MatrixToVector(m_lowerArchRegistrationMatrix);
        } else if (manualDelta && !TransformCore::IsIdentity(manualDelta)) {
            qWarning().noquote() << "Ajuste manual inferior no rigido; no se agrego a la matriz:"
                                 << manualReport;
        }
        m_lowerCompositeMesh = nullptr;
        m_appState.setLowerCompositeReady(false);
        removeObjectEntry(kLowerCompositeLabel);
        if (m_lowerRegistrationCalculated)
            m_lowerRegistrationReport += tr("\nAjuste manual con gizmo aplicado sobre la malla STL registrada.\n");
    }

    for (Mesh3DView* view : {m_mesh3DView,
                             upper ? m_modelUpperArchView : m_modelLowerArchView,
                             m_modelMatchView}) {
        if (!view) continue;
        view->addMesh(actorKey, baked, meshLabelName(objectLabel));
        view->setMeshColor(actorKey, color);
        view->setMeshOpacity(actorKey, 1.0);
        view->removeMesh(objectActorKey(upper ? kUpperCompositeLabel : kLowerCompositeLabel));
    }

    addObjectEntry(upper ? tr("Arco superior STL registrado") : tr("Arco inferior STL registrado"),
                   color, objectLabel);
    updateButtonStates();
}

void MainWindow::setObjectEntryVisible(int label, bool visible)
{
    const int actorKey = objectActorKey(label);
    if (m_mesh3DView) {
        m_mesh3DView->setMeshVisible(actorKey, visible);
        m_mesh3DView->setMeshVisible(label, visible);
    }
    for (Mesh3DView* v : {m_modelMaxillaView, m_modelUpperArchView,
                          m_modelMandibleView, m_modelLowerArchView,
                          m_modelMatchView, m_orientationView, m_osteotomyView,
                          m_biteSegmentView, m_biteScanView,
                          m_biteRegistrationView, m_repositionView}) {
        if (!v) continue;
        v->setMeshVisible(actorKey, visible);
        v->setMeshVisible(label, visible);
    }

    if (!m_objectTable) return;
    for (int row = 0; row < m_objectTable->rowCount(); ++row) {
        auto* idItem = m_objectTable->item(row, 0);
        if (!idItem || idItem->data(Qt::UserRole + 1).toInt() != label) continue;
        if (auto* visItem = m_objectTable->item(row, 2)) {
            const QSignalBlocker blocker(m_objectTable);
            visItem->setCheckState(visible ? Qt::Checked : Qt::Unchecked);
        }
        break;
    }
}

vtkSmartPointer<vtkPolyData> MainWindow::resolvedUpperArchWorldMesh(QString* error) const
{
    if (!m_upperRegistrationCalculated) {
        if (error) *error = tr("El STL superior aun no tiene registro calculado.");
        return nullptr;
    }
    if (!m_upperArchOriginalMesh) {
        if (error) *error = tr("No existe STL superior original para resolver la transformacion.");
        return nullptr;
    }
    QString transformReport;
    if (!TransformCore::ValidateRigidTransform(m_upperArchRegistrationMatrix, &transformReport)) {
        if (error) *error = tr("La matriz superior no es una transformacion rigida valida: %1").arg(transformReport);
        return nullptr;
    }
    if (m_upperArchMesh) {
        auto world = vtkSmartPointer<vtkPolyData>::New();
        world->DeepCopy(m_upperArchMesh);
        return world;
    }
    auto world = TransformCore::ApplyTransformToPolyData(m_upperArchOriginalMesh, m_upperArchRegistrationMatrix);
    if (!world && error) *error = tr("No se pudo aplicar T_maxilla_STL_to_CT.");
    return world;
}

vtkSmartPointer<vtkPolyData> MainWindow::resolvedLowerArchWorldMesh(QString* error) const
{
    if (!m_lowerRegistrationCalculated) {
        if (error) *error = tr("El STL inferior aun no tiene registro calculado.");
        return nullptr;
    }
    if (!m_lowerArchOriginalMesh) {
        if (error) *error = tr("No existe STL inferior original para resolver la transformacion.");
        return nullptr;
    }
    QString transformReport;
    if (!TransformCore::ValidateRigidTransform(m_lowerArchRegistrationMatrix, &transformReport)) {
        if (error) *error = tr("La matriz inferior no es una transformacion rigida valida: %1").arg(transformReport);
        return nullptr;
    }
    if (m_lowerArchMesh) {
        auto world = vtkSmartPointer<vtkPolyData>::New();
        world->DeepCopy(m_lowerArchMesh);
        return world;
    }
    auto world = TransformCore::ApplyTransformToPolyData(m_lowerArchOriginalMesh, m_lowerArchRegistrationMatrix);
    if (!world && error) *error = tr("No se pudo aplicar T_mandible_STL_to_CT.");
    return world;
}

void MainWindow::createDentalCompositeModels()
{
    if (m_compositeInProgress) {
        statusBar()->showMessage(tr("Ya hay un modelo compuesto calculandose."));
        return;
    }

    // Bake any pending manual STL adjustment before resolving composite input.
    commitActiveDentalGizmos(false);

    // Determine which step is active: 0 = upper pair, 1 = lower pair
    const int currentStep = m_modelStepStack ? m_modelStepStack->currentIndex() : 0;

    vtkSmartPointer<vtkPolyData> bone;
    vtkSmartPointer<vtkPolyData> arch;
    QString archError;

    if (currentStep == 0) {
        bone = meshForAnatomicLabel(5);
        arch = resolvedUpperArchWorldMesh(&archError);
        if (!bone || !arch) {
            QMessageBox::warning(this, tr("Modelo compuesto"),
                archError.isEmpty()
                    ? tr("Falta registrar el arco superior sobre el maxilar antes de crear el modelo compuesto.")
                    : archError);
            return;
        }
    } else {
        bone = meshForAnatomicLabel(6);
        arch = resolvedLowerArchWorldMesh(&archError);
        if (!bone || !arch) {
            QMessageBox::warning(this, tr("Modelo compuesto"),
                archError.isEmpty()
                    ? tr("Falta registrar el arco inferior sobre la mandibula antes de crear el modelo compuesto.")
                    : archError);
            return;
        }
    }

    QString dicomReport;
    if (!GeometryValidation::ValidateDicomGeometry(m_volume, &dicomReport)) {
        QMessageBox::warning(this, tr("Modelo compuesto"),
            tr("La geometria DICOM no paso validacion basica:\n%1").arg(dicomReport));
        return;
    }

    QString scaleReport;
    GeometryValidation::ValidateCompositeInputs(
        bone, arch, currentStep == 0 ? tr("Maxilar") : tr("Mandibula"), &scaleReport);
    qInfo().noquote() << "Validacion previa compuesto:";
    qInfo().noquote() << scaleReport;

    // Registrar → Ajuste fino → Bloque → Revisar (MainWindowComposite.cpp): the
    // composite is computed from the cutting block and stored only after review.
    startCompositeBlockStage(currentStep);
}

// ─────────────────────────────────────────────────────────────────────────────
// Called after the dental registration gizmo is committed.
// Collapses the step-pair panel and expands the match view to full height so
// the user can review the bone+STL alignment before continuing.
// Each jaw proceeds through its own cutting block and mandatory composite review.
void MainWindow::showRegistrationReview(int step)
{
    if (!m_modelMatchView) return;

    // ── Populate match view with bone + registered arch ───────────────────
    m_modelMatchView->clearMeshes();
    if (step == 0) {
        // Maxilla bone
        if (auto maxilla = meshForAnatomicLabel(5)) {
            m_modelMatchView->addMesh(5, maxilla, meshLabelName(5));
            m_modelMatchView->setMeshColor(5, objectColorForLabel(5));
        }
        // Registered upper arch
        if (m_upperArchMesh) {
            const int k = objectActorKey(kUpperArchLabel);
            m_modelMatchView->addMesh(k, m_upperArchMesh, meshLabelName(kUpperArchLabel));
            m_modelMatchView->setMeshColor(k, objectColorForLabel(kUpperArchLabel));
        }
        m_modelMatchView->setTitle(tr("MATCH MAXILAR — Revise y pulse Continuar"));
    } else {
        // Mandible bone
        if (auto mandible = meshForAnatomicLabel(6)) {
            m_modelMatchView->addMesh(6, mandible, meshLabelName(6));
            m_modelMatchView->setMeshColor(6, objectColorForLabel(6));
        }
        // Registered lower arch
        if (m_lowerArchMesh) {
            const int k = objectActorKey(kLowerArchLabel);
            m_modelMatchView->addMesh(k, m_lowerArchMesh, meshLabelName(kLowerArchLabel));
            m_modelMatchView->setMeshColor(k, objectColorForLabel(kLowerArchLabel));
        }
        m_modelMatchView->setTitle(tr("MATCH MANDIBULAR — Revise y pulse Continuar"));
    }
    m_modelMatchView->resetCamera();

    // ── Expand match view to full height ─────────────────────────────────
    if (m_modelStepStack) m_modelStepStack->setVisible(false);

    // ── Rewire the composite button as "Continuar" ────────────────────────
    if (!m_compositeButton) return;
    disconnect(m_compositeButton, nullptr, nullptr, nullptr);
    m_compositeButton->setEnabled(true);

    if (m_modelStepStack) m_modelStepStack->setCurrentIndex(step);
    connect(m_compositeButton, &QPushButton::clicked, this, &MainWindow::createDentalCompositeModels);
    updateModelWorkflowUi();
}

void MainWindow::onDentalCompositeFinished(int step, vtkSmartPointer<vtkPolyData> mesh)
{
    m_compositeInProgress = false;
    if (m_progressBar) {
        m_progressBar->setRange(0, 100);
        m_progressBar->setVisible(false);
    }
    if (m_viewFullScreen)
        exitViewFullScreen();
    if (m_compositeButton) {
        m_compositeButton->setEnabled(true);
        m_compositeButton->setText(tr("Crear Modelo Compuesto"));
    }

    if (!mesh) {
        QMessageBox::warning(this, tr("Modelo compuesto"),
            step == 0
                ? tr("No se pudo calcular la union booleana para el par superior.")
                : tr("No se pudo calcular la union booleana para el par inferior."));
        return;
    }

    if (step == 0) {
        m_upperCompositeMesh = mesh;
        const int actorKey = objectActorKey(kUpperCompositeLabel);
        const QColor color = objectColorForLabel(kUpperCompositeLabel);
        if (m_mesh3DView) {
            m_mesh3DView->addMesh(actorKey, m_upperCompositeMesh, meshLabelName(kUpperCompositeLabel));
            m_mesh3DView->setMeshColor(actorKey, color);
            m_mesh3DView->setMeshOpacity(actorKey, 1.0);
        }
        if (m_modelMaxillaView) {
            m_modelMaxillaView->addMesh(actorKey, m_upperCompositeMesh, meshLabelName(kUpperCompositeLabel));
            m_modelMaxillaView->setMeshColor(actorKey, color);
        }
        addObjectEntry(meshLabelName(kUpperCompositeLabel), color, kUpperCompositeLabel);
        setMaskVisible(5, false);
        setObjectEntryVisible(5, false);
        setObjectEntryVisible(kUpperArchLabel, false);

        // Phase 1 + Phase 4
        m_appState.setUpperCompositeReady(true);
        updateButtonStates();
        LoggerCore::instance().logCompositeCreation(
            QStringLiteral("upper"), m_upperCompositeMesh->GetNumberOfCells());

        if (m_autoCreateBothComposites) {
            // Chained auto-create: silently advance to lower without review
            if (m_modelStepStack) m_modelStepStack->setCurrentIndex(1);
            syncModelViews();
            createDentalCompositeModels();
            return;
        }
        if (m_modelStepStack)
            m_modelStepStack->setCurrentIndex(1);
        syncModelViews();
        updateModelWorkflowUi();
        statusBar()->showMessage(tr("Modelo compuesto superior creado. Ahora registra el arco inferior."));
        return;
    }

    m_lowerCompositeMesh = mesh;
    const int actorKey = objectActorKey(kLowerCompositeLabel);
    const QColor color = objectColorForLabel(kLowerCompositeLabel);
    if (m_mesh3DView) {
        m_mesh3DView->addMesh(actorKey, m_lowerCompositeMesh, meshLabelName(kLowerCompositeLabel));
        m_mesh3DView->setMeshColor(actorKey, color);
        m_mesh3DView->setMeshOpacity(actorKey, 1.0);
    }
    if (m_modelMandibleView) {
        m_modelMandibleView->addMesh(actorKey, m_lowerCompositeMesh, meshLabelName(kLowerCompositeLabel));
        m_modelMandibleView->setMeshColor(actorKey, color);
    }
    addObjectEntry(meshLabelName(kLowerCompositeLabel), color, kLowerCompositeLabel);
    setMaskVisible(6, false);
    setObjectEntryVisible(6, false);
    setObjectEntryVisible(kLowerArchLabel, false);

    // Phase 1 + Phase 4
    m_appState.setLowerCompositeReady(true);
    updateButtonStates();
    LoggerCore::instance().logCompositeCreation(
        QStringLiteral("lower"), m_lowerCompositeMesh->GetNumberOfCells());

    m_autoCreateBothComposites = false;
    showFinalCompositeView(true);
    statusBar()->showMessage(tr("Modelos compuestos superior e inferior completados."));
}

void MainWindow::exportDentalRegistrationPackage()
{
    const QString dirPath = QFileDialog::getExistingDirectory(
        this, tr("Exportar paquete TAC-STL"), QString());
    if (dirPath.isEmpty()) return;

    QDir dir(dirPath);
    auto writeStl = [this](vtkPolyData* mesh, const QString& path) -> bool {
        if (!mesh || mesh->GetNumberOfPoints() == 0) return false;
        auto writer = vtkSmartPointer<vtkSTLWriter>::New();
        writer->SetFileName(path.toLocal8Bit().constData());
        writer->SetInputData(mesh);
        writer->SetFileTypeToBinary();
        if (writer->Write() == 0) {
            QMessageBox::warning(this, tr("Exportar registro"),
                                 tr("No se pudo escribir: %1").arg(path));
            return false;
        }
        return true;
    };

    QString upperError;
    QString lowerError;
    auto upperWorld = resolvedUpperArchWorldMesh(&upperError);
    auto lowerWorld = resolvedLowerArchWorldMesh(&lowerError);

    bool wroteMesh = false;
    if (upperWorld)
        wroteMesh |= writeStl(upperWorld, dir.filePath(QStringLiteral("STL_maxilar_alineado.stl")));
    if (lowerWorld)
        wroteMesh |= writeStl(lowerWorld, dir.filePath(QStringLiteral("STL_mandibular_alineado.stl")));

    auto maxillaMesh = meshForAnatomicLabel(5);
    auto mandibleMesh = meshForAnatomicLabel(6);
    QVector<vtkSmartPointer<vtkPolyData>> compositeParts;
    if (maxillaMesh) compositeParts.append(maxillaMesh);
    if (mandibleMesh) compositeParts.append(mandibleMesh);
    if (upperWorld) compositeParts.append(upperWorld);
    if (lowerWorld) compositeParts.append(lowerWorld);
    if (auto composite = appendMeshes(compositeParts))
        wroteMesh |= writeStl(composite, dir.filePath(QStringLiteral("modelo_compuesto_TAC_STL.stl")));

    auto writeMatrix = [&](const QString& fileName, vtkMatrix4x4* matrix) {
        QFile f(dir.filePath(fileName));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        QTextStream out(&f);
        out << matrixToText(matrix) << '\n';
    };
    writeMatrix(QStringLiteral("T_maxilla_STL_to_CT.txt"), m_upperArchRegistrationMatrix);
    writeMatrix(QStringLiteral("T_mandible_STL_to_CT.txt"), m_lowerArchRegistrationMatrix);

    QFile report(dir.filePath(QStringLiteral("registro_TAC_STL_reporte.txt")));
    if (report.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&report);
        out << "MODELO COMPUESTO TAC + STL\n";
        out << "Fecha: " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n\n";
        out << "Spacing DICOM (mm): "
            << m_volumeMeta.spacing[0] << " x "
            << m_volumeMeta.spacing[1] << " x "
            << m_volumeMeta.spacing[2] << "\n\n";

        out << "Bounds TAC maxilar: " << meshBoundsText(maxillaMesh) << "\n";
        out << "Bounds TAC mandibula: " << meshBoundsText(mandibleMesh) << "\n";
        out << "Bounds STL maxilar alineado: " << meshBoundsText(upperWorld) << "\n";
        out << "Bounds STL mandibular alineado: " << meshBoundsText(lowerWorld) << "\n\n";
        out << "Centroide TAC maxilar: " << meshCentroidText(maxillaMesh) << "\n";
        out << "Centroide TAC mandibula: " << meshCentroidText(mandibleMesh) << "\n";
        out << "Centroide STL maxilar alineado: " << meshCentroidText(upperWorld) << "\n";
        out << "Centroide STL mandibular alineado: " << meshCentroidText(lowerWorld) << "\n";
        const double upperBoneDiag = meshDiagonalLength(maxillaMesh);
        const double lowerBoneDiag = meshDiagonalLength(mandibleMesh);
        const double upperScale = upperBoneDiag > 0.0
            ? meshDiagonalLength(upperWorld) / upperBoneDiag
            : 0.0;
        const double lowerScale = lowerBoneDiag > 0.0
            ? meshDiagonalLength(lowerWorld) / lowerBoneDiag
            : 0.0;
        out << "Escala relativa STL/TAC maxilar: " << upperScale << "\n";
        out << "Escala relativa STL/TAC mandibular: " << lowerScale << "\n\n";

        out << "=== REGISTRO MAXILAR ===\n";
        out << (m_upperRegistrationReport.isEmpty()
                    ? QStringLiteral("Sin registro maxilar calculado en esta sesion.\n")
                    : m_upperRegistrationReport) << "\n";
        out << "=== REGISTRO MANDIBULAR ===\n";
        out << (m_lowerRegistrationReport.isEmpty()
                    ? QStringLiteral("Sin registro mandibular calculado en esta sesion.\n")
                    : m_lowerRegistrationReport) << "\n";
        out << "Archivos STL exportados:\n";
        out << "- STL_maxilar_alineado.stl\n";
        out << "- STL_mandibular_alineado.stl\n";
        out << "- modelo_compuesto_TAC_STL.stl\n";
        if (!upperError.isEmpty()) out << "Aviso superior: " << upperError << "\n";
        if (!lowerError.isEmpty()) out << "Aviso inferior: " << lowerError << "\n";
    }

    if (!wroteMesh) {
        QMessageBox::warning(this, tr("Exportar registro"),
                             tr("No hay STL alineados ni mallas TAC para exportar."));
        return;
    }
    LoggerCore::instance().logExport(dirPath, QStringLiteral("registration_package"));
    statusBar()->showMessage(tr("Paquete TAC-STL exportado: %1").arg(QFileInfo(dirPath).absoluteFilePath()));
}

void MainWindow::exportDentalCompositeStl()
{
    QVector<vtkSmartPointer<vtkPolyData>> parts;
    if (m_upperCompositeMesh) {
        parts.append(m_upperCompositeMesh);
    } else if (auto maxilla = meshForAnatomicLabel(5)) {
        QString err;
        if (auto upperWorld = resolvedUpperArchWorldMesh(&err)) {
            parts.append(maxilla);
            parts.append(upperWorld);
        }
    }

    if (m_lowerCompositeMesh) {
        parts.append(m_lowerCompositeMesh);
    } else if (auto mandible = meshForAnatomicLabel(6)) {
        QString err;
        if (auto lowerWorld = resolvedLowerArchWorldMesh(&err)) {
            parts.append(mandible);
            parts.append(lowerWorld);
        }
    }

    auto finalMesh = appendMeshes(parts);
    if (!finalMesh) {
        QMessageBox::warning(this, tr("Exportar STL"), tr("No hay modelos compuestos para exportar."));
        return;
    }

    const QString path = QFileDialog::getSaveFileName(
        this, tr("Exportar modelo compuesto STL"), QStringLiteral("modelo_compuesto.stl"), tr("STL (*.stl)"));
    if (path.isEmpty()) return;

    // Offers a voxel union into a single closed printable mesh (MainWindowComposite.cpp).
    writeCompositeStl(parts, finalMesh, path);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Z &&
            (key->modifiers() & Qt::ControlModifier)) {
            undoLastEdit();
            key->accept();
            return true;
        }
        // The sculpting shortcuts come first while the guide is being edited.
        if (handleGuideSculptKey(key)) {
            key->accept();
            return true;
        }
        if (key->key() == Qt::Key_Space &&
            key->modifiers() == Qt::NoModifier &&
            toggleViewUnderCursor()) {
            key->accept();
            return true;
        }
        if (key->key() == Qt::Key_Tab &&
            key->modifiers() == Qt::NoModifier) {
            if (auto* meshView = qobject_cast<Mesh3DView*>(viewAtCursor())) {
                meshView->cycleStandardView();
                key->accept();
                return true;
            }
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && m_fillBoneCavityLabel > 0) {
        cancelBoneCavityFill();
        event->accept();
        return;
    }
    if (handleGuideSculptKey(event)) {
        event->accept();
        return;
    }
    // ── Ctrl+Z: undo ──────────────────────────────────────────────────────
    if (event->key() == Qt::Key_Z &&
        (event->modifiers() & Qt::ControlModifier)) {
        undoLastEdit();
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_Escape && m_viewFullScreen) {
        exitViewFullScreen();
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_Space &&
        event->modifiers() == Qt::NoModifier &&
        toggleViewUnderCursor()) {
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_Tab &&
        event->modifiers() == Qt::NoModifier) {
        if (auto* meshView = qobject_cast<Mesh3DView*>(viewAtCursor())) {
            meshView->cycleStandardView();
            event->accept();
            return;
        }
    }

    QMainWindow::keyPressEvent(event);
}

void MainWindow::onBoneWindow()        { broadcastPreset(WindowLevelPresets::Bone); }
void MainWindow::onSoftTissueWindow()  { broadcastPreset(WindowLevelPresets::SoftTissue); }
void MainWindow::onLungWindow()        { broadcastPreset(WindowLevelPresets::Lung); }
void MainWindow::onBrainWindow()       { broadcastPreset(WindowLevelPresets::Brain); }

// ─────────────────────────────────────────────────────────────────────────────
// onBoneSplitFinished
//
// Called when the Python bone-splitter script finishes successfully.
// The output NRRD contains the same labelmap with label 1 (Hueso) replaced by:
//   5 → Maxilar
//   6 → Mandíbula
//
// Steps:
//   1. Import the new labelmap.
//   2. Remove the old "Hueso" entry from the mask table and 3D view.
//   3. Register Maxilar (5) and Mandíbula (6) as new mask entries.
//   4. Generate 3D meshes for both new labels.
//   5. Refresh 2D overlays.
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::onBoneSplitFinished(const QString& outputPath)
{
    if (m_progressBar) m_progressBar->setVisible(false);

    // ── 1. Import the output labelmap ─────────────────────────────────────
    QString importError;
    auto newLabelmap = SegmentationImporter::importLabelmap(outputPath, &importError);
    if (!newLabelmap) {
        QMessageBox::critical(this, tr("Max/Mand"),
            tr("No se pudo importar el resultado de la división:\n%1").arg(importError));
        statusBar()->showMessage(tr("Max/Mand: error al importar resultado."));
        return;
    }

    // Save an undo snapshot before replacing the labelmap
    pushLabelmapUndo();
    m_segmentationLabelmap = newLabelmap;

    // ── 2. Remove "Hueso" (label 1) from the mask table and 3D view ──────
    if (m_maskTable) {
        for (int row = m_maskTable->rowCount() - 1; row >= 0; --row) {
            auto* item = m_maskTable->item(row, 0);
            if (item && item->data(Qt::UserRole).toInt() == 1) {
                m_maskTable->removeRow(row);
                break;
            }
        }
    }
    if (m_mesh3DView) m_mesh3DView->removeMesh(1);

    // ── 3. Register new mask entries (update if they already exist) ───────
    addMaskEntry(meshLabelName(5), meshLabelColor(5), 5);
    addMaskEntry(meshLabelName(6), meshLabelColor(6), 6);

    // ── 4. Generate 3D meshes for Maxilar and Mandíbula ───────────────────
    for (int label : {5, 6}) {
        QString meshError;
        const int smoothingIterations = smoothingIterationsForPreset(MeshSmoothingPreset::Optimal);
        m_maskSmoothingIterations[label] = smoothingIterations;
        auto mesh = MeshGenerator::generateMesh(
            m_segmentationLabelmap, label, true, smoothingIterations, &meshError);
        if (mesh) {
            publishSegmentationMesh(label, mesh);
        } else if (!meshError.isEmpty()) {
            statusBar()->showMessage(
                tr("Max/Mand: error generando malla %1 — %2")
                    .arg(meshLabelName(label), meshError));
        }
    }

    // ── 5. Refresh 2D segmentation overlays ──────────────────────────────
    refreshSegmentationOverlays();
    syncModelViews();

    // Phase 1: bone split makes segmentation "done" + re-enable split
    m_appState.setSegmentationDone(true);
    updateButtonStates();

    statusBar()->showMessage(
        tr("Max/Mand: hueso dividido en Maxilar y Mandíbula correctamente."));
}

// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::onBoneSplitError(const QString& error)
{
    if (m_progressBar) m_progressBar->setVisible(false);
    QMessageBox::critical(this, tr("Max/Mand — Error"), error);
    statusBar()->showMessage(tr("Max/Mand: operación fallida."));
}

void MainWindow::showAirwayDialog(int step)
{
    if (!m_airwayDialog) {
        m_airwayDialog = new QDialog(this, Qt::Dialog | Qt::WindowTitleHint | Qt::CustomizeWindowHint);
        m_airwayDialog->setWindowTitle(tr("Segmentar vía aérea"));
        m_airwayDialog->setModal(false); // Modeless, so user can interact with 2D slices
        m_airwayDialog->setMinimumWidth(340);
        m_airwayDialog->setStyleSheet(styleSheet());

        auto* layout = new QVBoxLayout(m_airwayDialog);
        layout->setContentsMargins(20, 18, 20, 16);
        layout->setSpacing(12);

        m_airwayDialogLabel = new QLabel(m_airwayDialog);
        m_airwayDialogLabel->setWordWrap(true);
        m_airwayDialogLabel->setStyleSheet("font-size:12px; color:#f5f5f7;");
        layout->addWidget(m_airwayDialogLabel);

        auto* buttonBox = new QHBoxLayout();
        auto* cancelBtn = new QPushButton(tr("Cancelar"), m_airwayDialog);
        buttonBox->addStretch();
        buttonBox->addWidget(cancelBtn);
        layout->addLayout(buttonBox);

        connect(cancelBtn, &QPushButton::clicked, this, [this] {
            m_airwayPointCaptureStep = 0;
            for (MPRView* view : {m_axialView, m_coronalView, m_sagittalView}) {
                if (view) view->setMeasurementPickingEnabled(false);
            }
            statusBar()->showMessage(tr("Segmentación de vía aérea cancelada."));
            closeAirwayDialog();
        });

        connect(m_airwayDialog, &QDialog::rejected, this, [this] {
            m_airwayPointCaptureStep = 0;
            for (MPRView* view : {m_axialView, m_coronalView, m_sagittalView}) {
                if (view) view->setMeasurementPickingEnabled(false);
            }
            statusBar()->showMessage(tr("Segmentación de vía aérea cancelada."));
        });
    }

    if (step == 1) {
        m_airwayDialogLabel->setText(tr("Paso 1 de 2: Indique el límite superior (nasofaringe) en la imagen."));
    } else if (step == 2) {
        m_airwayDialogLabel->setText(tr("Paso 2 de 2: Indique el límite inferior (tráquea) en la imagen."));
    }

    m_airwayDialog->show();
    m_airwayDialog->raise();
    m_airwayDialog->activateWindow();
}

void MainWindow::closeAirwayDialog()
{
    if (m_airwayDialog) {
        m_airwayDialog->close();
    }
}
