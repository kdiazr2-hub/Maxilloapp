// ─────────────────────────────────────────────────────────────────────────────
// MainWindow — GUIAS: surgical guides, the step after FERULA.
//
// The panel walks the steps of 3-matic's Design tab: choose the guide (Le Fort
// or chin — each wraps its own models in their planned position), mark the
// support region, place the saw slots on the planned osteotomies, add Boolean
// figures (cylinders, boxes, spheres or imported STL shapes, added or
// subtracted), drill the fixation holes, and build. The geometry is all in the
// cores (`WrapCore`, `GuideBaseCore`, `GuideDesignCore`); this file only
// collects what the user decides into a `GuidePlan` and shows it.
// ─────────────────────────────────────────────────────────────────────────────

#include "MainWindow.h"

#include "Mesh3DView.h"
#include "CranioPalette.h"
#include "GuideSculptCore.h"
#include "LeFortGuideCore.h"
#include "LeFortHoleCore.h"
#include "LeFortMotionCore.h"
#include "MeshRepairCore.h"
#include "ObjectLabels.h"
#include "PlateCore.h"
#include "SplintHeightmapGenerator.h"
#include "WrapCore.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTextStream>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <vtkIdList.h>
#include <vtkImplicitPolyDataDistance.h>
#include <vtkMatrix4x4.h>
#include <vtkPointData.h>
#include <vtkPointLocator.h>
#include <vtkPolyData.h>
#include <vtkUnsignedCharArray.h>
#include <vtkSTLReader.h>
#include <vtkSTLWriter.h>
#include <vtkStaticCellLocator.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>

#include <algorithm>
#include <limits>
#include <utility>

// Defined in MainWindowModels.cpp.
QString guidedSidePanelStyle(const QString& objectName);
// Defined in MainWindow.cpp.
QString meshLabelName(int label);

namespace
{
constexpr double kGuideGapClosingMm = 6.0; // the envelope bridges the maxilla's perforations (2026-10-06)
constexpr int kModeNone = 0;
constexpr int kModeRegion = 1;
constexpr int kModeSlotEnds = 2;
constexpr int kModeHoles = 3;
constexpr int kModeFigure = 4;
constexpr int kModeSculpt = 5;
constexpr int kModeTrim = 6;
constexpr int kModeTube = 7;
constexpr int kModePlateHoles = 8; // plate screw holes, on the planned bone
constexpr int kModeMoveHoles = 9;  // drag the Le Fort guide's drill sites over the bone
constexpr int kModeMarkHoles = 10; // click the Le Fort guide's drill sites on the bone
// Editable-point groups of the guide's drill sites, one per support verdict (Ok, Warning, Rejected).
constexpr int kHoleGroupBase = 10;
constexpr int kPlateDesignConventional = 0;
constexpr int kPlateDesignThreePsi = 1;
constexpr int kPlateDesignSplintless = 2;
constexpr int kPlatePillarCount = 4;

QString platePillarName(int pillar)
{
    switch (pillar) {
    case 0: return QCoreApplication::translate("MainWindow", "Pilar nasomaxilar derecho");
    case 1: return QCoreApplication::translate("MainWindow", "Pilar maxilomalar derecho");
    case 2: return QCoreApplication::translate("MainWindow", "Pilar nasomaxilar izquierdo");
    case 3: return QCoreApplication::translate("MainWindow", "Pilar maxilomalar izquierdo");
    default: return QCoreApplication::translate("MainWindow", "Pilares completos");
    }
}

// The EDITAR palette, in the order the buttons sit in the grid.
constexpr int kToolSmooth = 0;
constexpr int kToolWax = 1;
constexpr int kToolAdd = 2;
constexpr int kToolRemove = 3;
constexpr int kToolFlatten = 4;
constexpr int kToolTrim = 5;
constexpr int kToolUndo = 6;
constexpr int kToolRedo = 7;
constexpr int kIconSmooth = kToolSmooth;
constexpr int kIconWax = kToolWax;
constexpr int kIconAdd = kToolAdd;
constexpr int kIconRemove = kToolRemove;
constexpr int kIconFlatten = kToolFlatten;
constexpr int kIconTrim = kToolTrim;
constexpr int kIconUndo = kToolUndo;
constexpr int kIconRedo = kToolRedo;

// The palette icons, drawn here rather than shipped as files: a clay ball for the brushes that work it,
// a spatula for the plane, a blade for the trim and two curved arrows for the history. Same idea as the
// sculpting palettes these tools come from, but our own drawing.
QPixmap sculptPixmap(int kind, int size)
{
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const double u = size / 48.0; // the drawing is laid out on a 48 x 48 grid
    const QRectF ball(7.0 * u, 7.0 * u, 34.0 * u, 34.0 * u);
    const auto clay = [&](const QColor& light, const QColor& dark) {
        QRadialGradient gradient(ball.center() - QPointF(ball.width() * 0.25, ball.height() * 0.28),
                                 ball.width() * 1.05);
        gradient.setColorAt(0.0, light);
        gradient.setColorAt(1.0, dark);
        painter.setPen(Qt::NoPen);
        painter.setBrush(gradient);
        painter.drawEllipse(ball);
    };
    const auto stroke = [&](const QColor& color, double width) {
        QPen pen(color);
        pen.setWidthF(width);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
    };

    switch (kind) {
    case kIconSmooth: {
        clay(QColor(226, 230, 238), QColor(118, 126, 140));
        QPainterPath clip;
        clip.addEllipse(ball);
        painter.setClipPath(clip);
        // Three ripples, each flatter than the one above it: the surface being smoothed out.
        for (int row = 0; row < 3; ++row) {
            const double y = ball.top() + ball.height() * (0.28 + 0.22 * row);
            const double amplitude = 4.5 * u * (1.0 - 0.45 * row);
            QPainterPath wave;
            wave.moveTo(ball.left() - u, y);
            for (int seg = 0; seg < 4; ++seg) {
                const double x0 = ball.left() - u + ball.width() * 0.3 * seg;
                const double x1 = x0 + ball.width() * 0.3;
                wave.quadTo(0.5 * (x0 + x1), y + (seg % 2 == 0 ? -amplitude : amplitude), x1, y);
            }
            stroke(QColor(44, 50, 62), 2.6 * u);
            painter.drawPath(wave);
        }
        painter.setClipping(false);
        break;
    }
    case kIconWax: {
        // A flame over a ball of wax that is running off it.
        QPainterPath flame;
        flame.moveTo(24.0 * u, 0.5 * u);
        flame.cubicTo(34.0 * u, 8.0 * u, 31.0 * u, 13.0 * u, 24.0 * u, 15.0 * u);
        flame.cubicTo(17.0 * u, 13.0 * u, 14.0 * u, 8.0 * u, 24.0 * u, 0.5 * u);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 214, 80));
        painter.drawPath(flame);
        clay(QColor(255, 198, 126), QColor(206, 96, 12));
        painter.setBrush(QColor(224, 124, 30));
        painter.drawEllipse(QPointF(17.0 * u, 42.0 * u), 3.6 * u, 4.8 * u);
        painter.drawEllipse(QPointF(31.0 * u, 40.5 * u), 2.8 * u, 3.8 * u);
        break;
    }
    case kIconAdd:
    case kIconRemove: {
        clay(QColor(196, 202, 214), QColor(96, 104, 118));
        const QColor sign = kind == kIconAdd ? QColor(60, 220, 100) : QColor(255, 82, 72);
        painter.setPen(Qt::NoPen);
        painter.setBrush(sign);
        const QPointF center = ball.center();
        const double arm = 12.0 * u, thick = 4.4 * u;
        QPainterPath cross;
        cross.addRoundedRect(QRectF(center.x() - arm, center.y() - thick, 2 * arm, 2 * thick), thick, thick);
        if (kind == kIconAdd)
            cross.addRoundedRect(QRectF(center.x() - thick, center.y() - arm, 2 * thick, 2 * arm), thick, thick);
        painter.drawPath(cross.simplified());
        break;
    }
    case kIconFlatten: {
        // The surface, already flat, and the spatula riding over it.
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(92, 99, 112));
        painter.drawRoundedRect(QRectF(2.0 * u, 36.0 * u, 44.0 * u, 9.0 * u), 2.5 * u, 2.5 * u);
        painter.save();
        painter.translate(24.0 * u, 26.0 * u);
        painter.rotate(-14.0);
        painter.setBrush(QColor(232, 236, 244));
        painter.drawRoundedRect(QRectF(-19.0 * u, 0.0, 38.0 * u, 6.0 * u), 2.5 * u, 2.5 * u);
        painter.setBrush(QColor(150, 158, 172));
        painter.drawRoundedRect(QRectF(8.0 * u, -17.0 * u, 5.5 * u, 18.0 * u), 2.5 * u, 2.5 * u);
        painter.restore();
        break;
    }
    case kIconTrim: {
        // The dashed line the user draws, and the blade cutting along it.
        QPen dashed(QColor(170, 178, 190));
        dashed.setWidthF(2.8 * u);
        dashed.setStyle(Qt::DashLine);
        dashed.setCapStyle(Qt::FlatCap);
        painter.setPen(dashed);
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(QPointF(2.0 * u, 40.0 * u), QPointF(46.0 * u, 40.0 * u));
        QPainterPath blade;
        blade.moveTo(31.0 * u, 2.0 * u);
        blade.lineTo(39.0 * u, 8.0 * u);
        blade.lineTo(17.0 * u, 34.0 * u);
        blade.lineTo(11.0 * u, 30.0 * u);
        blade.closeSubpath();
        painter.setPen(QPen(QColor(24, 28, 36, 150), 1.2 * u));
        painter.setBrush(QColor(236, 240, 248));
        painter.drawPath(blade);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(80, 86, 98));
        painter.save();
        painter.translate(35.0 * u, 5.0 * u);
        painter.rotate(-54.0);
        painter.drawRoundedRect(QRectF(-5.0 * u, -4.0 * u, 12.0 * u, 8.0 * u), 2.0 * u, 2.0 * u);
        painter.restore();
        break;
    }
    case kIconUndo:
    case kIconRedo: {
        painter.save();
        if (kind == kIconRedo) { // the same arrow, mirrored
            painter.translate(size, 0);
            painter.scale(-1.0, 1.0);
        }
        const QRectF circle(9.0 * u, 11.0 * u, 30.0 * u, 28.0 * u);
        QPainterPath arc;
        arc.arcMoveTo(circle, 150.0);
        arc.arcTo(circle, 150.0, -235.0);
        stroke(QColor(232, 236, 244), 3.6 * u);
        painter.drawPath(arc);
        // The head sits on the free end of the arc, pointing the way it came from.
        const QPointF tip = arc.pointAtPercent(0.0);
        const QPointF back = arc.pointAtPercent(0.08);
        QPointF along = tip - back;
        const double length = std::hypot(along.x(), along.y());
        if (length > 1e-6) {
            along /= length;
            const QPointF side(-along.y(), along.x());
            QPainterPath head;
            head.moveTo(tip + along * 9.5 * u);
            head.lineTo(tip - along * 2.5 * u + side * 7.0 * u);
            head.lineTo(tip - along * 2.5 * u - side * 7.0 * u);
            head.closeSubpath();
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(232, 236, 244));
            painter.drawPath(head);
        }
        painter.restore();
        break;
    }
    default:
        break;
    }
    return pixmap;
}

QIcon sculptIcon(int kind)
{
    QIcon icon;
    for (int size : {28, 56})
        icon.addPixmap(sculptPixmap(kind, size));
    return icon;
}

constexpr int kGuideFigureActorBase = -560; // one actor per figure, counting down

const QColor kGuideColor(214, 226, 240);
const QColor kWrapColor(120, 196, 214);  // the envelope: its own layer, teal so it is not mistaken for bone
const QColor kPaintColor(10, 132, 255);  // the brushed region on the envelope
const QColor kRegionColor(10, 132, 255);
const QColor kSlotEndColor(10, 132, 255);
const QColor kHoleColor(52, 199, 89);
const QColor kSubtractColor(255, 69, 58);
const QColor kAddColor(48, 209, 88);
const QColor kPlateColor(176, 184, 196);     // titanium
const QColor kPlateHoleColor(255, 159, 10);  // holes of the plate being marked
const QColor kPredictiveColor(191, 90, 242); // where the guide drills them, before the cut

int figureActorKey(size_t index) { return kGuideFigureActorBase - static_cast<int>(index); }

bool identityFigureMatrix(const std::array<double, 16>& matrix)
{
    const auto identity = SplintDesignCore::IdentityMatrix();
    for (size_t i = 0; i < matrix.size(); ++i)
        if (std::abs(matrix[i] - identity[i]) > 1e-6)
            return false;
    return true;
}

QString figureName(const GuideFigure& figure)
{
    const QString op = figure.operation == GuideFigureOperation::Add ? QObject::tr("Unir") : QObject::tr("Restar");
    switch (figure.shape) {
    case GuideFigureShape::Cylinder:
        return QObject::tr("%1 · Cilindro Ø%2 × %3 mm").arg(op).arg(figure.diameterMm, 0, 'f', 1).arg(figure.lengthMm, 0, 'f', 1);
    case GuideFigureShape::Box:
        return QObject::tr("%1 · Caja %2 × %3 × %4 mm")
            .arg(op)
            .arg(figure.widthMm, 0, 'f', 1)
            .arg(figure.heightMm, 0, 'f', 1)
            .arg(figure.depthMm, 0, 'f', 1);
    case GuideFigureShape::Sphere:
        return QObject::tr("%1 · Esfera Ø%2 mm").arg(op).arg(figure.diameterMm, 0, 'f', 1);
    case GuideFigureShape::Mesh:
        return figure.sourceLabel != 0
            ? QObject::tr("%1 · Copia de %2").arg(op, meshLabelName(figure.sourceLabel))
            : QObject::tr("%1 · %2").arg(op, QFileInfo(figure.sourcePath).fileName());
    case GuideFigureShape::CurvedTube:
        return QObject::tr("%1 · Tubo curvo Ø%2 mm").arg(op).arg(figure.diameterMm, 0, 'f', 1);
    }
    return op;
}

// An STL read and centred on its own middle, so its frame can be placed like any primitive's.
vtkSmartPointer<vtkPolyData> loadFigureMesh(const QString& path)
{
    if (path.isEmpty() || !QFileInfo::exists(path))
        return nullptr;
    auto reader = vtkSmartPointer<vtkSTLReader>::New();
    reader->SetFileName(path.toUtf8().constData());
    reader->Update();
    vtkPolyData* mesh = reader->GetOutput();
    if (!mesh || mesh->GetNumberOfPolys() == 0)
        return nullptr;
    double b[6] = {};
    mesh->GetBounds(b);
    auto center = vtkSmartPointer<vtkTransform>::New();
    center->Translate(-0.5 * (b[0] + b[1]), -0.5 * (b[2] + b[3]), -0.5 * (b[4] + b[5]));
    auto filter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
    filter->SetInputData(mesh);
    filter->SetTransform(center);
    filter->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(filter->GetOutput());
    return out;
}
} // namespace

QWidget* MainWindow::buildGuideControlPanel(QWidget* parent)
{
    auto* scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setFixedWidth(320);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* panel = new QWidget();
    panel->setObjectName(QStringLiteral("GuideControlPanel"));
    panel->setStyleSheet(guidedSidePanelStyle(panel->objectName()) +
                         QStringLiteral("#GuideControlPanel QPushButton { background:#292b30; color:#f5f5f7;"
                                        "  border:1px solid #3a3d43; border-radius:6px; padding:8px 10px;"
                                        "  min-height:20px; font-size:12px; text-align:left; }"
                                        "#GuideControlPanel QPushButton:hover { background:#34373d; border-color:#4b4e55; }"
                                        "#GuideControlPanel QPushButton:pressed { background:#0a84ff;"
                                        "  border-color:#64d2ff; color:#ffffff; }"
                                        "#GuideControlPanel QPushButton:checked { background:#0a84ff;"
                                        "  border-color:#64d2ff; color:#ffffff; font-weight:700; }"
                                        "#GuideControlPanel QPushButton:focus { border-color:#0a84ff; }"
                                        "#GuideControlPanel QPushButton:disabled { background:#202226;"
                                        "  border-color:#2c2e33; color:#6e6e73; }"
                                        "#GuideControlPanel QPushButton#GuideFold { background:transparent;"
                                        "  border:none; color:#8e8e93; font-weight:700; text-align:left;"
                                        "  padding:6px 0 2px 0; }"
                                        "#GuideControlPanel QPushButton#GuideFold:checked { background:transparent;"
                                        "  border:none; color:#c7c7cc; }"
                                        "#GuideControlPanel QListWidget { background:#202226; color:#f5f5f7;"
                                        "  border:1px solid #3a3d43; border-radius:4px; font-size:11px; }"
                                        "#GuideControlPanel QComboBox { background:#292b30; color:#f5f5f7;"
                                        "  border:1px solid #3a3d43; border-radius:6px; padding:5px 8px; }"
                                        "#GuideControlPanel QToolButton { background:#292b30; color:#f5f5f7;"
                                        "  border:1px solid #3a3d43; border-radius:6px; }"
                                        "#GuideControlPanel QToolButton:hover { background:#34373d; }"
                                        "#GuideControlPanel QToolButton:pressed { background:#0a84ff;"
                                        "  border-color:#64d2ff; }"
                                        "#GuideControlPanel QToolButton:checked { background:#0a84ff;"
                                        "  border-color:#64d2ff; }"
                                        "#GuideControlPanel QToolButton:focus { border-color:#0a84ff; }"
                                        "#GuideControlPanel QToolButton:disabled { background:#202226;"
                                        "  border-color:#2c2e33; color:#6e6e73; }"
                                        "#GuideControlPanel QToolButton#GuideMode { font-size:10px;"
                                        "  padding:3px 7px; }"
                                        "#GuideControlPanel QCheckBox { color:#c7c7cc; font-size:11px; }"
                                        "#GuideControlPanel QLabel { color:#c7c7cc; font-size:11px; }"));
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(6);

    m_guidePanelTitle = new QLabel(tr("Guía de corte"), panel);
    m_guidePanelTitle->setObjectName(QStringLiteral("GuidedPanelTitle"));
    layout->addWidget(m_guidePanelTitle);
    m_guideHintLabel = new QLabel(panel);
    m_guideHintLabel->setWordWrap(true);
    m_guideHintLabel->setStyleSheet(QStringLiteral("color:#f5f5f7; font-size:11px; padding:2px 0 6px 0;"));
    layout->addWidget(m_guideHintLabel);

    // A numbered step: its own widget, so the panel can show only the steps that are reachable.
    const auto step = [&](const QString& text) {
        auto* holder = new QWidget(panel);
        auto* box = new QVBoxLayout(holder);
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(5);
        auto* label = new QLabel(text, holder);
        label->setObjectName(QStringLiteral("GuidedPanelSection"));
        box->addWidget(label);
        layout->addWidget(holder);
        return box;
    };
    // Everything that is not a step folds away: the panel stays short and nothing is lost.
    const auto fold = [&](const QString& text) {
        auto* holder = new QWidget(panel);
        auto* box = new QVBoxLayout(holder);
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(4);
        auto* header = new QPushButton(QStringLiteral("▸ ") + text, holder);
        header->setObjectName(QStringLiteral("GuideFold"));
        header->setCheckable(true);
        auto* body = new QWidget(holder);
        auto* inner = new QVBoxLayout(body);
        inner->setContentsMargins(0, 0, 0, 0);
        inner->setSpacing(5);
        body->setVisible(false);
        connect(header, &QPushButton::toggled, body, [header, body, text](bool on) {
            header->setText((on ? QStringLiteral("▾ ") : QStringLiteral("▸ ")) + text);
            body->setVisible(on);
        });
        box->addWidget(header);
        box->addWidget(body);
        layout->addWidget(holder);
        return std::pair<QWidget*, QVBoxLayout*>{holder, inner};
    };
    const auto spin = [&](double value, double lo, double hi, double stepSize) {
        auto* box = new QDoubleSpinBox(panel);
        box->setRange(lo, hi);
        box->setSingleStep(stepSize);
        box->setDecimals(2);
        box->setValue(value);
        box->setSuffix(tr(" mm"));
        return box;
    };

    // ── 1. Guide type and envelope ────────────────────────────────────────
    auto* typeBox = step(tr("1. TIPO DE GUÍA"));
    m_guideTypeSection = typeBox->parentWidget();
    m_guideTypeCombo = new QComboBox(panel);
    m_guideTypeCombo->addItem(tr("Guía de corte + placa Le Fort I"), static_cast<int>(GuideType::LeFort));
    m_guideTypeCombo->addItem(tr("Guía de mentón"), static_cast<int>(GuideType::Chin));
    connect(m_guideTypeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { setGuideType(static_cast<GuideType>(m_guideTypeCombo->currentData().toInt())); });
    typeBox->addWidget(m_guideTypeCombo);
    m_guideSourcesLabel = new QLabel(panel);
    m_guideSourcesLabel->setWordWrap(true);
    typeBox->addWidget(m_guideSourcesLabel);
    m_guideWrapButton = new QPushButton(tr("Calcular envolvente"), panel);
    connect(m_guideWrapButton, &QPushButton::clicked, this, &MainWindow::computeGuideWrap);
    typeBox->addWidget(m_guideWrapButton);

    // ── Layers: one compact row of toggles ────────────────────────────────
    m_guideLayersSection = new QWidget(panel);
    auto* layerRow = new QHBoxLayout(m_guideLayersSection);
    layerRow->setContentsMargins(0, 2, 0, 2);
    layerRow->setSpacing(6);
    const auto layer = [&](const QString& text) {
        auto* check = new QCheckBox(text, m_guideLayersSection);
        check->setChecked(true);
        connect(check, &QCheckBox::toggled, this, [this](bool) { applyGuideLayers(); });
        layerRow->addWidget(check);
        return check;
    };
    m_guideShowModelsCheck = layer(tr("Hueso"));
    m_guideShowWrapCheck = layer(tr("Envolvente"));
    m_guideShowGuideCheck = layer(tr("Guía"));
    m_guideShowFiguresCheck = layer(tr("Figuras"));
    layerRow->addStretch(1);
    layout->addWidget(m_guideLayersSection);

    // The cutting guide is built as a guided sequence on the pre-operative anatomy.
    auto* automaticGuideBox = step(tr("PASO ACTUAL"));
    m_guideAutomaticSection = automaticGuideBox->parentWidget();
    m_guideWorkflowLabel = new QLabel(panel);
    m_guideWorkflowLabel->setWordWrap(true);
    m_guideWorkflowLabel->setStyleSheet(QStringLiteral("color:#f5f5f7; font-weight:700; padding:3px 0;"));
    automaticGuideBox->addWidget(m_guideWorkflowLabel);
    auto* workflowRow = new QHBoxLayout();
    workflowRow->setSpacing(4);
    m_guideWorkflowBackButton = new QPushButton(tr("Anterior"), panel);
    connect(m_guideWorkflowBackButton, &QPushButton::clicked, this, &MainWindow::retreatGuideWorkflow);
    workflowRow->addWidget(m_guideWorkflowBackButton);
    m_guideWorkflowNextButton = new QPushButton(tr("Continuar"), panel);
    connect(m_guideWorkflowNextButton, &QPushButton::clicked, this, &MainWindow::advanceGuideWorkflow);
    workflowRow->addWidget(m_guideWorkflowNextButton);
    automaticGuideBox->addLayout(workflowRow);
    // Le Fort: once the envelope exists the guide can be laid out from the movement itself — the band an
    // impaction takes out, both slits, the drill sites — instead of painted by hand (user's choice,
    // 2026-10-05). The hand-drawn steps stay as the alternative and to retouch it.
    // The Le Fort guide assistant (spec asistente-guia-lefort, after the user's own case): 1 roots, 2 band,
    // 3 holes, 4 guides. Each step is computed and editable.
    m_guideRootsButton = new QPushButton(tr("1 · Medir raíces y ápices"), panel);
    m_guideRootsButton->setToolTip(tr("Longitud de las raíces del canino y del primer molar y distancia de cada ápice "
                                      "al corte. Avisa si algún ápice queda a menos de 5 mm."));
    connect(m_guideRootsButton, &QPushButton::clicked, this, &MainWindow::analyzeGuideRoots);
    automaticGuideBox->addWidget(m_guideRootsButton);
    m_guideRootsLabel = new QLabel(panel);
    m_guideRootsLabel->setWordWrap(true);
    m_guideRootsLabel->setStyleSheet(QStringLiteral("color:%1; font-size:11px;").arg(CranioPalette::MutedText));
    automaticGuideBox->addWidget(m_guideRootsLabel);
    m_guideBandBox = new QWidget(panel);
    {
        auto* bandForm = new QFormLayout(m_guideBandBox);
        bandForm->setContentsMargins(0, 0, 0, 0);
        const QString names[] = {tr("Pilar D"), tr("Piriforme D"), tr("Piriforme I"), tr("Pilar I")};
        for (size_t i = 0; i < m_guideBandSpins.size(); ++i) {
            auto* spin = new QDoubleSpinBox(m_guideBandBox);
            spin->setRange(-10.0, 15.0);
            spin->setDecimals(1);
            spin->setSingleStep(0.5);
            spin->setSuffix(QStringLiteral(" mm"));
            connect(spin, &QDoubleSpinBox::editingFinished, this, &MainWindow::applyGuideBandHeights);
            m_guideBandSpins[i] = spin;
            bandForm->addRow(names[i], spin);
        }
        auto* reset = new QPushButton(tr("Restablecer (movimiento)"), m_guideBandBox);
        connect(reset, &QPushButton::clicked, this, &MainWindow::resetGuideBandHeights);
        bandForm->addRow(reset);
    }
    automaticGuideBox->addWidget(new QLabel(tr("2 · Franja a quitar (overlap)"), panel));
    automaticGuideBox->addWidget(m_guideBandBox);
    m_guideCaseEdit = new QLineEdit(panel);
    m_guideCaseEdit->setPlaceholderText(tr("Número de caso (se graba en las guías)"));
    connect(m_guideCaseEdit, &QLineEdit::editingFinished, this,
            [this] { m_guidePlan.caseLabel = m_guideCaseEdit->text().trimmed(); });
    automaticGuideBox->addWidget(m_guideCaseEdit);
    // The holes come first, as markers on the bone the surgeon can move; only «Aceptar orificios» puts them in
    // the guide and builds it (user's request, 2026-10-05).
    m_guideGenerateButton = new QPushButton(tr("3 · Proponer orificios"), panel);
    m_guideGenerateButton->setToolTip(tr("Calcula la franja de hueso a quitar según el movimiento del Le Fort "
                                         "(impactación o descenso) y propone los orificios en hueso con buen soporte. "
                                         "Todavía no se colocan en la guía: puede moverlos antes de aceptarlos."));
    connect(m_guideGenerateButton, &QPushButton::clicked, this, &MainWindow::generateLeFortGuide);
    automaticGuideBox->addWidget(m_guideGenerateButton);
    // Or the surgeon marks them: each click on the bone is a drill site, given to the nearest pillar and to its side
    // of the cut, and the guide is built round them (user's request, 2026-10-06).
    m_guideMarkHolesButton = new QPushButton(tr("Marcar orificios"), panel);
    m_guideMarkHolesButton->setCheckable(true);
    m_guideMarkHolesButton->setToolTip(tr("Haga clic sobre el hueso en cada pilar para colocar un orificio. "
                                          "La guía se construye desde ellos, con un margen lateral."));
    connect(m_guideMarkHolesButton, &QPushButton::toggled, this,
            [this](bool on) { setGuidePointMode(on ? kModeMarkHoles : kModeNone); });
    automaticGuideBox->addWidget(m_guideMarkHolesButton);
    m_guideClearHolesButton = new QPushButton(tr("Borrar orificios"), panel);
    connect(m_guideClearHolesButton, &QPushButton::clicked, this, [this] {
        m_guidePlan.lefortHoles.clear();
        m_guideLeFortMissing.clear();
        showGuideLeFortHoles();
        if (m_guideReportLabel)
            m_guideReportLabel->setText(guideLeFortHoleReport());
        updateGuideUi();
    });
    automaticGuideBox->addWidget(m_guideClearHolesButton);
    m_guideMoveHoleButton = new QPushButton(tr("Mover orificio"), panel);
    m_guideMoveHoleButton->setCheckable(true);
    m_guideMoveHoleButton->setToolTip(tr("Arrastre un orificio sobre el hueso. Se rechaza en el borde óseo, cerca "
                                         "del corte o en la franja; en hueso delgado se acepta con aviso."));
    connect(m_guideMoveHoleButton, &QPushButton::toggled, this,
            [this](bool on) { setGuidePointMode(on ? kModeMoveHoles : kModeNone); });
    automaticGuideBox->addWidget(m_guideMoveHoleButton);
    m_guideAcceptHolesButton = new QPushButton(tr("Aceptar orificios y crear guía"), panel);
    m_guideAcceptHolesButton->setToolTip(tr("Coloca los orificios en la guía como camisas de broca y la construye."));
    connect(m_guideAcceptHolesButton, &QPushButton::clicked, this, &MainWindow::acceptLeFortHoles);
    automaticGuideBox->addWidget(m_guideAcceptHolesButton);

    // ── Custom plates: define the definitive holes before the pre-operative guide ──
    auto* plateBox = step(tr("1. DISEÑO · POSICIÓN DEFINITIVA"));
    m_guidePlateSection = plateBox->parentWidget();
    auto* plateForm = new QFormLayout();
    m_guidePlateTemplateCombo = new QComboBox(panel);
    m_guidePlateTemplateCombo->addItem(tr("Convencionales · 4 placas"), kPlateDesignConventional);
    m_guidePlateTemplateCombo->addItem(tr("Splintless · 3 PSI"), kPlateDesignThreePsi);
    m_guidePlateTemplateCombo->addItem(tr("Splintless · monobloque"), kPlateDesignSplintless);
    connect(m_guidePlateTemplateCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { clearGuidePlateHoles(); });
    plateForm->addRow(tr("Diseño:"), m_guidePlateTemplateCombo);
    plateBox->addLayout(plateForm);
    m_guidePlateStepLabel = new QLabel(panel);
    m_guidePlateStepLabel->setWordWrap(true);
    plateBox->addWidget(m_guidePlateStepLabel);
    m_guidePlateViewButton = new QPushButton(tr("Ver posición planificada"), panel);
    m_guidePlateViewButton->setCheckable(true);
    connect(m_guidePlateViewButton, &QPushButton::toggled, this, &MainWindow::setGuidePlateView);
    plateBox->addWidget(m_guidePlateViewButton);
    m_guidePlateHolesButton = new QPushButton(tr("Marcar puntos del pilar"), panel);
    m_guidePlateHolesButton->setCheckable(true);
    connect(m_guidePlateHolesButton, &QPushButton::toggled, this, [this](bool on) {
        if (!on) {
            setGuidePointMode(kModeNone);
            return;
        }
        setGuidePlateView(true); // holes are marked on the bone where the plate will sit
        setGuidePointMode(m_guidePlannedView ? kModePlateHoles : kModeNone);
    });
    plateBox->addWidget(m_guidePlateHolesButton);
    auto* plateHoleRow = new QHBoxLayout();
    plateHoleRow->setSpacing(4);
    m_guidePlateArmButton = new QPushButton(tr("Aceptar pilar y continuar"), panel);
    connect(m_guidePlateArmButton, &QPushButton::clicked, this, &MainWindow::startGuidePlateArm);
    plateHoleRow->addWidget(m_guidePlateArmButton);
    auto* clearPlateHoles = new QPushButton(tr("Borrar agujeros"), panel);
    connect(clearPlateHoles, &QPushButton::clicked, this, &MainWindow::clearGuidePlateHoles);
    plateHoleRow->addWidget(clearPlateHoles);
    plateBox->addLayout(plateHoleRow);
    m_guidePlateCreateButton = new QPushButton(tr("Crear placas"), panel);
    connect(m_guidePlateCreateButton, &QPushButton::clicked, this, &MainWindow::createGuidePlate);
    plateBox->addWidget(m_guidePlateCreateButton);
    m_guidePlateList = new QListWidget(panel);
    m_guidePlateList->setMaximumHeight(64);
    plateBox->addWidget(m_guidePlateList);
    auto* plateRow = new QHBoxLayout();
    plateRow->setSpacing(4);
    auto* removePlate = new QPushButton(tr("Borrar placa"), panel);
    connect(removePlate, &QPushButton::clicked, this, &MainWindow::removeGuidePlate);
    plateRow->addWidget(removePlate);
    m_guidePlateExportButton = new QPushButton(tr("Exportar placas"), panel);
    connect(m_guidePlateExportButton, &QPushButton::clicked, this, &MainWindow::exportGuidePlates);
    plateRow->addWidget(m_guidePlateExportButton);
    plateBox->addLayout(plateRow);
    m_guidePlateCheckLabel = new QLabel(panel);
    m_guidePlateCheckLabel->setWordWrap(true);
    plateBox->addWidget(m_guidePlateCheckLabel);
    // ── 2-4. Support zones and subnasal bridge ────────────────────────────
    auto* regionBox = step(tr("ZONA DE APOYO"));
    m_guideRegionSection = regionBox->parentWidget();
    m_guideRegionButton = new QPushButton(tr("Pintar zona"), panel);
    m_guideRegionButton->setCheckable(true);
    connect(m_guideRegionButton, &QPushButton::toggled, this,
            [this](bool on) { setGuidePointMode(on ? kModeRegion : kModeNone); });
    regionBox->addWidget(m_guideRegionButton);
    auto* regionForm = new QFormLayout();
    m_guideBrushSpin = spin(4.0, 0.5, 15.0, 0.5);
    regionForm->addRow(tr("Pincel:"), m_guideBrushSpin);
    regionBox->addLayout(regionForm);
    auto* clearRegion = new QPushButton(tr("Borrar zona"), panel);
    connect(clearRegion, &QPushButton::clicked, this, &MainWindow::clearGuideRegion);
    regionBox->addWidget(clearRegion);

    // ── 6. Saw slots ──────────────────────────────────────────────────────
    auto* slotBox = step(tr("6. HENDIDURAS DE CORTE"));
    m_guideSlotSection = slotBox->parentWidget();
    m_guideCutList = new QListWidget(panel);
    m_guideCutList->setMaximumHeight(72);
    connect(m_guideCutList, &QListWidget::itemChanged, this, [this](QListWidgetItem*) { updateGuideUi(); });
    slotBox->addWidget(m_guideCutList);
    m_guideSlotEndsButton = new QPushButton(tr("Marcar extremos"), panel);
    m_guideSlotEndsButton->setCheckable(true);
    connect(m_guideSlotEndsButton, &QPushButton::toggled, this,
            [this](bool on) { setGuidePointMode(on ? kModeSlotEnds : kModeNone); });
    slotBox->addWidget(m_guideSlotEndsButton);
    auto* clearEnds = new QPushButton(tr("Borrar extremos"), panel);
    connect(clearEnds, &QPushButton::clicked, this, &MainWindow::clearGuideSlotEnds);
    slotBox->addWidget(clearEnds);

    // ── 5. Fixation holes ─────────────────────────────────────────────────
    auto* holeBox = step(tr("5. PERFORACIONES"));
    m_guideHoleSection = holeBox->parentWidget();
    m_guideHoleButton = new QPushButton(tr("Marcar agujeros"), panel);
    m_guideHoleButton->setCheckable(true);
    connect(m_guideHoleButton, &QPushButton::toggled, this,
            [this](bool on) { setGuidePointMode(on ? kModeHoles : kModeNone); });
    holeBox->addWidget(m_guideHoleButton);
    auto* holeForm = new QFormLayout();
    m_guideHoleDiameterSpin = spin(2.0, 0.5, 6.0, 0.1);
    holeForm->addRow(tr("Diámetro:"), m_guideHoleDiameterSpin);
    holeBox->addLayout(holeForm);
    auto* clearHoles = new QPushButton(tr("Borrar"), panel);
    connect(clearHoles, &QPushButton::clicked, this, &MainWindow::clearGuideHoles);
    holeBox->addWidget(clearHoles);

    // ── Boolean figures: a tool for special cases, folded away ────────────
    const auto [figuresHolder, figureBox] = fold(tr("Figuras"));
    m_guideFiguresSection = figuresHolder;
    auto* figureForm = new QFormLayout();
    m_guideFigureShapeCombo = new QComboBox(panel);
    m_guideFigureShapeCombo->addItem(tr("Cilindro"), static_cast<int>(GuideFigureShape::Cylinder));
    m_guideFigureShapeCombo->addItem(tr("Caja"), static_cast<int>(GuideFigureShape::Box));
    m_guideFigureShapeCombo->addItem(tr("Esfera"), static_cast<int>(GuideFigureShape::Sphere));
    connect(m_guideFigureShapeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { updateGuideFigureInputs(); });
    figureForm->addRow(tr("Figura:"), m_guideFigureShapeCombo);
    m_guideFigureOperationCombo = new QComboBox(panel);
    m_guideFigureOperationCombo->addItem(tr("Restar (ranura, orificio)"), static_cast<int>(GuideFigureOperation::Subtract));
    m_guideFigureOperationCombo->addItem(tr("Unir (añadir material)"), static_cast<int>(GuideFigureOperation::Add));
    figureForm->addRow(tr("Operación:"), m_guideFigureOperationCombo);
    m_guideFigureDiameterSpin = spin(3.0, 0.2, 40.0, 0.1);
    m_guideFigureLengthSpin = spin(12.0, 0.2, 80.0, 0.5);
    m_guideFigureWidthSpin = spin(12.0, 0.2, 80.0, 0.5);
    m_guideFigureHeightSpin = spin(1.0, 0.1, 80.0, 0.1);
    m_guideFigureDepthSpin = spin(12.0, 0.2, 80.0, 0.5);
    figureForm->addRow(tr("Diámetro:"), m_guideFigureDiameterSpin);
    figureForm->addRow(tr("Longitud:"), m_guideFigureLengthSpin);
    figureForm->addRow(tr("Ancho:"), m_guideFigureWidthSpin);
    figureForm->addRow(tr("Alto:"), m_guideFigureHeightSpin);
    figureForm->addRow(tr("Profundidad:"), m_guideFigureDepthSpin);
    figureBox->addLayout(figureForm);
    m_guidePlaceFigureButton = new QPushButton(tr("Colocar figura"), panel);
    m_guidePlaceFigureButton->setCheckable(true);
    connect(m_guidePlaceFigureButton, &QPushButton::toggled, this,
            [this](bool on) { setGuidePointMode(on ? kModeFigure : kModeNone); });
    figureBox->addWidget(m_guidePlaceFigureButton);
    auto* importFigure = new QPushButton(tr("Importar figura STL…"), panel);
    connect(importFigure, &QPushButton::clicked, this, &MainWindow::importGuideFigure);
    figureBox->addWidget(importFigure);

    auto* connectorForm = new QFormLayout();
    m_guideSplintCopyCombo = new QComboBox(panel);
    m_guideSplintCopyCombo->addItem(tr("Férula intermedia"), kIntermediateSplintLabel);
    m_guideSplintCopyCombo->addItem(tr("Férula final"), kFinalSplintLabel);
    connectorForm->addRow(tr("Copia integrada:"), m_guideSplintCopyCombo);
    auto* addSplintCopy = new QPushButton(tr("Añadir copia de férula"), panel);
    connect(addSplintCopy, &QPushButton::clicked, this, &MainWindow::addGuideSplintCopy);
    connectorForm->addRow(addSplintCopy);
    m_guideTubeDiameterSpin = spin(3.0, 1.0, 8.0, 0.25);
    connectorForm->addRow(tr("Diámetro del tubo:"), m_guideTubeDiameterSpin);
    figureBox->addLayout(connectorForm);
    m_guideTubeButton = new QPushButton(tr("Colocar tubo curvo (3 puntos)"), panel);
    m_guideTubeButton->setCheckable(true);
    connect(m_guideTubeButton, &QPushButton::toggled, this,
            [this](bool on) { setGuidePointMode(on ? kModeTube : kModeNone); });
    figureBox->addWidget(m_guideTubeButton);
    m_guideFigureList = new QListWidget(panel);
    m_guideFigureList->setMaximumHeight(80);
    connect(m_guideFigureList, &QListWidget::currentRowChanged, this, [this](int) { updateGuideUi(); });
    figureBox->addWidget(m_guideFigureList);
    m_guideMoveFigureButton = new QPushButton(tr("Mover figura (gizmo)"), panel);
    m_guideMoveFigureButton->setCheckable(true);
    connect(m_guideMoveFigureButton, &QPushButton::toggled, this, &MainWindow::setGuideFigureGizmo);
    figureBox->addWidget(m_guideMoveFigureButton);
    auto* removeFigure = new QPushButton(tr("Borrar figura"), panel);
    connect(removeFigure, &QPushButton::clicked, this, &MainWindow::removeGuideFigure);
    figureBox->addWidget(removeFigure);

    // ── 7. Build ──────────────────────────────────────────────────────────
    auto* buildBox = step(tr("7. CREAR GUÍA"));
    m_guideBuildSection = buildBox->parentWidget();
    auto* guideForm = new QFormLayout();
    m_guideThicknessSpin = spin(2.5, 0.5, 10.0, 0.1);
    guideForm->addRow(tr("Espesor:"), m_guideThicknessSpin);
    buildBox->addLayout(guideForm);
    m_guideBuildButton = new QPushButton(tr("Crear guía"), panel);
    connect(m_guideBuildButton, &QPushButton::clicked, this, &MainWindow::buildGuideMesh);
    buildBox->addWidget(m_guideBuildButton);

    // ── 6. Edit: the finished guide as clay ───────────────────────────────
    auto* editBox = step(tr("7. EDITAR"));
    m_guideEditSection = editBox->parentWidget();
    m_guideEditButton = new QPushButton(tr("Editar la guía"), panel);
    m_guideEditButton->setCheckable(true);
    connect(m_guideEditButton, &QPushButton::toggled, this, &MainWindow::setGuideEditActive);
    editBox->addWidget(m_guideEditButton);

    // The palette: one square button per tool, as Freeform's tool tray.
    m_guideSculptPalette = new QWidget(panel);
    auto* palette = new QGridLayout(m_guideSculptPalette);
    palette->setContentsMargins(0, 2, 0, 2);
    palette->setSpacing(4);
    const std::vector<std::tuple<int, QString, QString>> tools = {
        {kIconSmooth, tr("Suavizar"), QStringLiteral("1")},
        {kIconWax, tr("Cera caliente"), QStringLiteral("2")},
        {kIconAdd, tr("Añadir material"), QStringLiteral("3")},
        {kIconRemove, tr("Quitar material"), QStringLiteral("4")},
        {kIconFlatten, tr("Aplanar"), QStringLiteral("5")},
        {kIconTrim, tr("Recortar"), QStringLiteral("6")},
        {kIconUndo, tr("Deshacer"), QStringLiteral("Ctrl+Z")},
        {kIconRedo, tr("Rehacer"), QStringLiteral("Ctrl+Y")},
    };
    for (int index = 0; index < static_cast<int>(tools.size()); ++index) {
        const auto& [icon, name, shortcut] = tools[static_cast<size_t>(index)];
        auto* button = new QToolButton(m_guideSculptPalette);
        button->setIcon(sculptIcon(icon));
        button->setIconSize(QSize(28, 28));
        button->setFixedSize(40, 40);
        button->setToolTip(QStringLiteral("%1 (%2)").arg(name, shortcut));
        button->setCheckable(index < kToolUndo);
        connect(button, &QToolButton::clicked, this, [this, index] {
            if (index == kToolUndo)
                guideSculptUndo();
            else if (index == kToolRedo)
                guideSculptRedo();
            else
                setGuideSculptTool(index);
        });
        palette->addWidget(button, index / 4, index % 4);
        m_guideSculptTools.append(button);
    }
    editBox->addWidget(m_guideSculptPalette);

    // The contextual bar: only what the active tool uses.
    m_guideSculptBar = new QWidget(panel);
    auto* bar = new QVBoxLayout(m_guideSculptBar);
    bar->setContentsMargins(0, 0, 0, 0);
    bar->setSpacing(4);

    m_guideSculptSizeRow = new QWidget(m_guideSculptBar);
    auto* sizeRow = new QHBoxLayout(m_guideSculptSizeRow);
    sizeRow->setContentsMargins(0, 0, 0, 0);
    sizeRow->setSpacing(4);
    sizeRow->addWidget(new QLabel(tr("Tamaño"), m_guideSculptSizeRow));
    m_guideSculptSizeSpin = new QDoubleSpinBox(m_guideSculptSizeRow);
    m_guideSculptSizeSpin->setRange(0.5, 20.0);
    m_guideSculptSizeSpin->setSingleStep(0.5);
    m_guideSculptSizeSpin->setDecimals(1);
    m_guideSculptSizeSpin->setValue(3.0);
    m_guideSculptSizeSpin->setSuffix(tr(" mm"));
    sizeRow->addWidget(m_guideSculptSizeSpin, 1);
    for (const auto& [text, deltaMm] : {std::pair{QStringLiteral("−"), -0.5}, std::pair{QStringLiteral("+"), 0.5}}) {
        auto* nudge = new QToolButton(m_guideSculptSizeRow);
        nudge->setText(text);
        nudge->setFixedSize(24, 24);
        connect(nudge, &QToolButton::clicked, this, [this, deltaMm] { nudgeGuideBrushSize(deltaMm); });
        sizeRow->addWidget(nudge);
    }
    bar->addWidget(m_guideSculptSizeRow);

    m_guideSculptLevelRow = new QWidget(m_guideSculptBar);
    auto* levelRow = new QHBoxLayout(m_guideSculptLevelRow);
    levelRow->setContentsMargins(0, 0, 0, 0);
    levelRow->setSpacing(4);
    m_guideSculptLevelLabel = new QLabel(tr("Nivel"), m_guideSculptLevelRow);
    levelRow->addWidget(m_guideSculptLevelLabel);
    m_guideSculptLevelSlider = new QSlider(Qt::Horizontal, m_guideSculptLevelRow);
    m_guideSculptLevelSlider->setRange(0, 100);
    m_guideSculptLevelSlider->setValue(50);
    levelRow->addWidget(m_guideSculptLevelSlider, 1);
    bar->addWidget(m_guideSculptLevelRow);

    m_guideSculptModeRow = new QWidget(m_guideSculptBar);
    auto* modeRow = new QHBoxLayout(m_guideSculptModeRow);
    modeRow->setContentsMargins(0, 0, 0, 0);
    modeRow->setSpacing(4);
    for (int index = 0; index < 4; ++index) {
        auto* mode = new QToolButton(m_guideSculptModeRow);
        mode->setObjectName(QStringLiteral("GuideMode"));
        mode->setCheckable(true);
        connect(mode, &QToolButton::clicked, this, [this, index] {
            switch (m_guideSculptTool) {
            case kToolSmooth: m_guideSmoothScope = index; break;
            case kToolWax: m_guideWaxMode = index; break;
            case kToolFlatten: m_guideFlattenMode = index; break;
            default: break;
            }
            updateGuideSculptBar();
        });
        modeRow->addWidget(mode);
        m_guideSculptModes.append(mode);
    }
    modeRow->addStretch(1);
    bar->addWidget(m_guideSculptModeRow);

    m_guideSculptTrimRow = new QWidget(m_guideSculptBar);
    auto* trimRow = new QHBoxLayout(m_guideSculptTrimRow);
    trimRow->setContentsMargins(0, 0, 0, 0);
    trimRow->setSpacing(4);
    m_guideTrimInvertCheck = new QCheckBox(tr("Invertir"), m_guideSculptTrimRow);
    trimRow->addWidget(m_guideTrimInvertCheck);
    auto* trimApply = new QPushButton(tr("Aplicar"), m_guideSculptTrimRow);
    connect(trimApply, &QPushButton::clicked, this, &MainWindow::applyGuideTrim);
    trimRow->addWidget(trimApply);
    auto* trimClear = new QPushButton(tr("Limpiar"), m_guideSculptTrimRow);
    connect(trimClear, &QPushButton::clicked, this, &MainWindow::clearGuideTrim);
    trimRow->addWidget(trimClear);
    bar->addWidget(m_guideSculptTrimRow);
    editBox->addWidget(m_guideSculptBar);

    // ── 7. Export ─────────────────────────────────────────────────────────
    auto* exportBox = step(tr("8. EXPORTAR"));
    m_guideExportSection = exportBox->parentWidget();
    m_guideThicknessCheck = new QCheckBox(tr("Mapa de espesor"), panel);
    connect(m_guideThicknessCheck, &QCheckBox::toggled, this, [this](bool) { applyGuideThicknessColors(); });
    exportBox->addWidget(m_guideThicknessCheck);
    m_guideExportButton = new QPushButton(tr("Exportar STL"), panel);
    connect(m_guideExportButton, &QPushButton::clicked, this, &MainWindow::exportGuideStl);
    exportBox->addWidget(m_guideExportButton);

    // ── Advanced: the numbers that have a sensible default ────────────────
    const auto [advancedHolder, advancedBox] = fold(tr("Avanzado · guía"));
    m_guideAdvancedSection = advancedHolder;
    auto* advancedForm = new QFormLayout();
    // 2.5 mm: segmented maxilla is perforated, and the guide's envelope must bridge those holes or the guide
    // follows them down and breaks off short of the pillar (user's case, 2026-10-05).
    m_guideGapSpin = spin(kGuideGapClosingMm, 0.0, 10.0, 0.1);
    m_guideDetailSpin = spin(0.25, 0.1, 1.0, 0.05);
    m_guideClearanceSpin = spin(0.1, 0.0, 2.0, 0.05);
    m_guideBladeSpin = spin(0.6, 0.2, 2.0, 0.1);
    m_guideMarginSpin = spin(2.0, 0.0, 10.0, 0.5);
    m_guideCornerSpin = spin(4.0, 0.0, 10.0, 0.5);
    // The rim finish of a printed guide pad: thinner and rounded towards the edge.
    m_guideTaperSpin = spin(5.0, 0.0, 15.0, 0.5);
    m_guideEdgeFractionSpin = new QDoubleSpinBox(panel);
    m_guideEdgeFractionSpin->setRange(10.0, 100.0);
    m_guideEdgeFractionSpin->setSingleStep(5.0);
    m_guideEdgeFractionSpin->setDecimals(0);
    m_guideEdgeFractionSpin->setValue(40.0);
    m_guideEdgeFractionSpin->setSuffix(tr(" %"));
    m_guideEdgeRoundSpin = spin(1.5, 0.0, 4.0, 0.1);
    m_guideWrapOpacitySpin = new QDoubleSpinBox(panel);
    m_guideWrapOpacitySpin->setRange(0.1, 1.0);
    m_guideWrapOpacitySpin->setSingleStep(0.1);
    m_guideWrapOpacitySpin->setValue(0.6);
    connect(m_guideWrapOpacitySpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double) { applyGuideLayers(); });
    advancedForm->addRow(tr("Cierre de huecos:"), m_guideGapSpin);
    advancedForm->addRow(tr("Detalle:"), m_guideDetailSpin);
    advancedForm->addRow(tr("Holgura:"), m_guideClearanceSpin);
    advancedForm->addRow(tr("Hoja de sierra:"), m_guideBladeSpin);
    advancedForm->addRow(tr("Margen al borde:"), m_guideMarginSpin);
    advancedForm->addRow(tr("Redondeo de esquinas:"), m_guideCornerSpin);
    advancedForm->addRow(tr("Afinado hacia el borde:"), m_guideTaperSpin);
    advancedForm->addRow(tr("Espesor en el borde:"), m_guideEdgeFractionSpin);
    advancedForm->addRow(tr("Redondeo del borde:"), m_guideEdgeRoundSpin);
    advancedForm->addRow(tr("Opacidad envolvente:"), m_guideWrapOpacitySpin);
    advancedBox->addLayout(advancedForm);

    // Plate dimensions belong to the separate final-position plate module.
    const auto [plateAdvancedHolder, plateAdvancedBox] = fold(tr("Avanzado · placa"));
    m_guidePlateAdvancedSection = plateAdvancedHolder;
    auto* plateAdvancedForm = new QFormLayout();
    const PlateParams plateDefaults;
    const SleeveParams sleeveDefaults;
    m_guidePlateThicknessSpin = spin(plateDefaults.thicknessMm, 0.6, 2.5, 0.1);
    m_guidePlateMinCutSpin = spin(plateDefaults.minCutDistanceMm, 0.0, 10.0, 0.5);
    m_guideSleeveBoreSpin = spin(sleeveDefaults.boreDiameterMm, 0.8, 4.0, 0.1);
    m_guideSleeveOuterSpin = spin(sleeveDefaults.outerDiameterMm, 2.0, 8.0, 0.1);
    m_guideSleeveHeightSpin = spin(sleeveDefaults.heightMm, 1.0, 10.0, 0.5);
    plateAdvancedForm->addRow(tr("Espesor de placa:"), m_guidePlateThicknessSpin);
    plateAdvancedForm->addRow(tr("Agujero-osteotomía mín.:"), m_guidePlateMinCutSpin);
    plateAdvancedForm->addRow(tr("Camisa, orificio:"), m_guideSleeveBoreSpin);
    plateAdvancedForm->addRow(tr("Camisa, exterior:"), m_guideSleeveOuterSpin);
    plateAdvancedForm->addRow(tr("Camisa, altura:"), m_guideSleeveHeightSpin);
    for (QDoubleSpinBox* box : {m_guidePlateThicknessSpin, m_guidePlateMinCutSpin})
        connect(box, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) {
            m_guidePlateMeshes.clear(); // rebuilt with the new numbers when next shown
            refreshGuidePlates();
        });
    plateAdvancedBox->addLayout(plateAdvancedForm);

    m_guideReportLabel = new QLabel(panel);
    m_guideReportLabel->setWordWrap(true);
    m_guideReportLabel->setObjectName(QStringLiteral("MutedText"));
    layout->addWidget(m_guideReportLabel);
    layout->addStretch(1);
    scroll->setWidget(panel);
    updateGuideFigureInputs();
    return scroll;
}

void MainWindow::setGuidesWorkspace(bool enabled, bool plateWorkspace)
{
    if (m_viewModeStack && enabled)
        m_viewModeStack->setCurrentIndex(7);
    if (!enabled) {
        if (m_guideSculptActive)
            setGuideEditActive(false);
        setGuidePointMode(kModeNone);
        if (m_guideMoveFigureButton && m_guideMoveFigureButton->isChecked())
            m_guideMoveFigureButton->setChecked(false);
        return;
    }
    m_guidePlateWorkspace = plateWorkspace;
    if (m_guidePanelTitle)
        m_guidePanelTitle->setText(plateWorkspace ? tr("Placa personalizada") : tr("Guía de corte"));
    if (m_guideView)
        m_guideView->setTitle(plateWorkspace ? tr("PLACA PERSONALIZADA · LE FORT DEFINITIVO")
                                             : tr("GUÍA DE CORTE · ANATOMÍA PREOPERATORIA"));
    // Older plans stored copied splints with an identity matrix even though
    // their Le Fort had already moved. Migrate those copies once on entry.
    if (m_guidePlan.type == GuideType::LeFort) {
        const auto movedLeFort = repositionMeshForLabel(kLeFortSegLabel);
        const auto guideLeFort = guideSourceMeshForLabel(kLeFortSegLabel);
        const auto toPreoperative = SplintDesignCore::RigidMotion(movedLeFort, guideLeFort, 0.2);
        if (toPreoperative)
            for (GuideFigure& figure : m_guidePlan.figures)
                if (figure.shape == GuideFigureShape::Mesh &&
                    (figure.sourceLabel == kIntermediateSplintLabel || figure.sourceLabel == kFinalSplintLabel) &&
                    identityFigureMatrix(figure.matrix))
                    figure.matrix = *toPreoperative;
    }
    refreshGuideSources();
    refreshGuideCutList();
    refreshGuideFigureList();
    refreshGuidePlates();
    // The plate step always shows the Le Fort where the plan puts it — not only when a guide happens to be in
    // memory (after reopening a project it is not, and the step then showed the segment without its movement).
    const auto leFortSegment = repositionMeshForLabel(kLeFortSegLabel);
    setGuidePlateView(plateWorkspace && leFortSegment && leFortSegment->GetNumberOfPolys() > 0);
    syncGuideView();
    if (!plateWorkspace && m_guideWrapMesh)
        activateGuideWorkflowStep();
    updateGuideUi();
}

void MainWindow::setGuideType(GuideType type)
{
    if (m_guidePlan.type == type && !m_guidePlan.sourceLabels.empty())
        return;
    m_guidePlan.type = type;
    m_guidePlan.sourceLabels = GuidePlanCore::SourceLabelsFor(type);
    if (type != GuideType::LeFort && m_guidePlannedView) {
        m_guidePlannedView = false; // plates are a Le Fort thing
        if (m_guidePointMode == kModePlateHoles)
            setGuidePointMode(kModeNone);
    }
    // A different guide sits on different models: its envelope and region start again.
    m_guideWrapMesh = nullptr;
    m_guidePrepared = GuidePreparation{};
    m_guideMesh = nullptr;
    m_guidePlan.contour.clear();
    m_guidePlan.paint.clear();
    m_guidePlan.holes.clear();
    m_guidePlan.slotPlan.clear();
    m_guidePlan.workflowStep = GuideWorkflowStep::Envelope;
    m_guidePlan.rightPaintEnd = 0;
    m_guidePlan.leftPaintEnd = 0;
    m_guidePendingEnds.clear();
    refreshGuideSources();
    refreshGuideCutList();
    syncGuideView();
    updateGuideUi();
}

vtkSmartPointer<vtkPolyData> MainWindow::guideSourceMeshForLabel(int label) const
{
    // The Le Fort cutting path is defined at osteotomy time. Its guide must be
    // designed in that same pre-reposition frame, even after the segment moves.
    if (m_guidePlan.type == GuideType::LeFort) {
        const auto original = m_repositionOriginalMeshes.find(label);
        if (original != m_repositionOriginalMeshes.end() && original->second &&
            original->second->GetNumberOfPolys() > 0)
            return original->second;
    }
    return repositionMeshForLabel(label);
}

void MainWindow::refreshGuideSources()
{
    if (m_guidePlan.sourceLabels.empty())
        m_guidePlan.sourceLabels = GuidePlanCore::SourceLabelsFor(m_guidePlan.type);
    if (!m_guideSourcesLabel)
        return;
    QStringList parts;
    for (int label : GuidePlanCore::SourceLabelsFor(m_guidePlan.type)) {
        const auto mesh = guideSourceMeshForLabel(label);
        const bool present = mesh && mesh->GetNumberOfPolys() > 0;
        parts << (present ? QStringLiteral("✓ %1").arg(meshLabelName(label))
                          : tr("✗ %1 (falta)").arg(meshLabelName(label)));
    }
    const QString frame = m_guidePlan.type == GuideType::LeFort
        ? tr("posición preoperatoria, antes de Reposición")
        : tr("posición planificada");
    m_guideSourcesLabel->setText(tr("Envolvente sobre los modelos en %1:\n%2")
                                     .arg(frame, parts.join(QStringLiteral("\n"))));
}

void MainWindow::refreshGuideCutList()
{
    if (!m_guideCutList)
        return;
    // The osteotomy being planned counts too, so a guide can be designed before the cut is executed.
    if (m_ostWizard.path.valid && m_ostWizard.type != 1)
        rememberOsteotomyCut(tr("Trayectoria actual"), m_ostWizard.path,
                             m_ostWizard.type == 2 ? GuideType::Chin : GuideType::LeFort);

    QSet<QString> checked;
    for (int row = 0; row < m_guideCutList->count(); ++row)
        if (m_guideCutList->item(row)->checkState() == Qt::Checked)
            checked.insert(m_guideCutList->item(row)->text());

    QSignalBlocker blocker(m_guideCutList);
    m_guideCutList->clear();
    for (size_t i = 0; i < m_guideCuts.size(); ++i) {
        if (m_guideCuts[i].type != m_guidePlan.type)
            continue; // a Le Fort guide only offers Le Fort cuts, a chin guide the genioplasty
        auto* item = new QListWidgetItem(m_guideCuts[i].name, m_guideCutList);
        item->setData(Qt::UserRole, static_cast<int>(i));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(checked.contains(m_guideCuts[i].name) ? Qt::Checked : Qt::Unchecked);
    }
}

void MainWindow::rememberOsteotomyCut(const QString& name, const OsteotomyPath& path, GuideType type)
{
    if (!path.valid)
        return;
    for (auto& cut : m_guideCuts) {
        // The same cut under another name (a slot piece of it, a reloaded plan) is not a second osteotomy.
        if (cut.name == name || cut.path.points == path.points) {
            cut.path = path;
            cut.type = type;
            return;
        }
    }
    m_guideCuts.push_back({name, path, type});
}

void MainWindow::computeGuideWrap()
{
    std::vector<vtkPolyData*> meshes;
    QStringList missing;
    for (int label : GuidePlanCore::SourceLabelsFor(m_guidePlan.type)) {
        const auto mesh = guideSourceMeshForLabel(label);
        if (mesh && mesh->GetNumberOfPolys() > 0)
            meshes.push_back(mesh);
        else
            missing << meshLabelName(label);
    }
    if (!missing.isEmpty()) {
        QMessageBox::warning(this, tr("Guías"),
                             tr("Faltan modelos para esta guía: %1. Realice la osteotomía correspondiente primero.")
                                 .arg(missing.join(QStringLiteral(", "))));
        return;
    }
    m_guidePlan.sourceLabels = GuidePlanCore::SourceLabelsFor(m_guidePlan.type);
    m_guidePlan.wrap.gapClosingMm = m_guideGapSpin->value();
    m_guidePlan.wrap.smallestDetailMm = m_guideDetailSpin->value();
    m_guidePlan.wrap.smoothingIterations = std::max(m_guidePlan.wrap.smoothingIterations, 30);
    m_guidePlan.design.base.smallestDetailMm = m_guideDetailSpin->value();
    m_guidePlan.design.base.thicknessMm = m_guideThicknessSpin->value();
    m_guidePlan.design.base.clearanceMm = m_guideClearanceSpin->value();

    statusBar()->showMessage(tr("Guías: calculando la envolvente…"));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const WrapResult wrap = WrapCore::Wrap(meshes, m_guidePlan.wrap);
    GuidePreparation prepared;
    if (wrap.ok)
        prepared = GuideDesignCore::Prepare(wrap.mesh, m_guidePlan.design); // measured once, reused while marking
    QApplication::restoreOverrideCursor();
    if (!wrap.ok) {
        QMessageBox::warning(this, tr("Guías"), wrap.error);
        return;
    }
    m_guideWrapMesh = wrap.mesh;
    m_guidePrepared = prepared;
    // The bone itself, so nothing built on the wrap can end up inside it. Baked once with the envelope: it is
    // the same rasterisation, just without the closing and without contouring a mesh from it.
    m_guidePlan.design.bone = ImplicitCore::BakeMeshField(meshes, m_guidePlan.wrap.smallestDetailMm, 8.0);
    refreshGuideBand(); // a reopened project shows its band again: it is never saved, only recomputed
    if (m_guidePlan.workflowStep == GuideWorkflowStep::Envelope) {
        // Starting the assistant is an explicit restart: legacy automatic geometry
        // must not leak into the manually painted guide.
        m_guidePlan.contour.clear();
        m_guidePlan.paint.clear();
        m_guidePlan.holes.clear();
        m_guidePlan.slotPlan.clear();
        m_guidePlan.rightPaintEnd = 0;
        m_guidePlan.leftPaintEnd = 0;
        m_guideMesh = nullptr;
        m_guidePlan.workflowStep = GuideWorkflowStep::PaintRight;
    }
    if (m_guidePrepared.ok) {
        for (GuideFigure& figure : m_guidePlan.figures) {
            if (figure.shape != GuideFigureShape::CurvedTube || figure.controlPoints.size() < 3)
                continue;
            const auto& start = figure.controlPoints.front();
            const auto& end = figure.controlPoints.back();
            const double chord = std::hypot(std::hypot(end[0] - start[0], end[1] - start[1]), end[2] - start[2]);
            const double minimumBulge = std::max({8.0, 2.5 * figure.diameterMm, 0.25 * chord});
            figure.controlPoints = GuideDesignCore::OutwardTubeControlPoints(
                figure.controlPoints, GuideDesignCore::SurfaceNormalAt(m_guidePrepared, start), minimumBulge);
        }
    }
    repaintGuideWrap();
    // The envelope replaces the bone while designing: bone layers hidden, envelope opaque.
    if (m_guideShowModelsCheck) {
        QSignalBlocker blocker(m_guideShowModelsCheck);
        m_guideShowModelsCheck->setChecked(false);
    }
    if (m_guideShowWrapCheck) {
        QSignalBlocker blocker(m_guideShowWrapCheck);
        m_guideShowWrapCheck->setChecked(true);
    }
    if (m_guideWrapOpacitySpin) {
        QSignalBlocker blocker(m_guideWrapOpacitySpin);
        m_guideWrapOpacitySpin->setValue(1.0);
    }
    syncGuideView();
    activateGuideWorkflowStep();
    updateGuideUi();
    if (m_guideReportLabel)
        m_guideReportLabel->setText(wrap.report);
    statusBar()->showMessage(wrap.report);
}

bool MainWindow::guideWorkflowStepComplete() const
{
    const auto hasPaintSince = [this](int begin) {
        begin = std::clamp(begin, 0, static_cast<int>(m_guidePlan.paint.size()));
        return std::any_of(m_guidePlan.paint.begin() + begin, m_guidePlan.paint.end(),
                           [](const GuideBrushStroke& stroke) { return !stroke.erase; });
    };
    switch (m_guidePlan.workflowStep) {
    case GuideWorkflowStep::Envelope:
        return m_guideWrapMesh && m_guideWrapMesh->GetNumberOfPolys() > 0;
    case GuideWorkflowStep::PaintRight:
        return hasPaintSince(0);
    case GuideWorkflowStep::PaintLeft:
        return hasPaintSince(m_guidePlan.rightPaintEnd);
    case GuideWorkflowStep::PaintBridge:
        return hasPaintSince(m_guidePlan.leftPaintEnd);
    case GuideWorkflowStep::Holes:
        return m_guidePlan.type == GuideType::LeFort && !m_guidePlan.plates.empty()
                   ? !guidePredictiveHoles().empty()
                   : m_guidePlan.holes.size() >= 2;
    case GuideWorkflowStep::Slots:
        return std::any_of(m_guidePlan.slotPlan.begin(), m_guidePlan.slotPlan.end(),
                           [](const GuideSlot& slot) { return slot.hasExtent; });
    case GuideWorkflowStep::Build:
    case GuideWorkflowStep::Complete:
        return m_guideMesh && m_guideMesh->GetNumberOfPolys() > 0;
    }
    return false;
}

void MainWindow::activateGuideWorkflowStep()
{
    switch (m_guidePlan.workflowStep) {
    case GuideWorkflowStep::PaintRight:
    case GuideWorkflowStep::PaintLeft:
        if (m_guideBrushSpin) m_guideBrushSpin->setValue(4.0);
        setGuidePointMode(kModeRegion);
        break;
    case GuideWorkflowStep::PaintBridge:
        if (m_guideBrushSpin) m_guideBrushSpin->setValue(2.0);
        setGuidePointMode(kModeRegion);
        break;
    case GuideWorkflowStep::Holes:
        setGuidePointMode(m_guidePlan.type == GuideType::LeFort && !m_guidePlan.plates.empty() ? kModeNone
                                                                                              : kModeHoles);
        break;
    case GuideWorkflowStep::Slots:
        if (m_guideCutList && m_guideCutList->currentRow() < 0 && m_guideCutList->count() > 0)
            m_guideCutList->setCurrentRow(0);
        setGuidePointMode(kModeSlotEnds);
        break;
    default:
        setGuidePointMode(kModeNone);
        break;
    }
    if (m_guideView && m_guidePlan.type == GuideType::LeFort)
        m_guideView->setStandardView(0);
}

void MainWindow::advanceGuideWorkflow()
{
    if (!guideWorkflowStepComplete()) {
        QString message;
        switch (m_guidePlan.workflowStep) {
        case GuideWorkflowStep::PaintRight: message = tr("Pinte primero la zona de apoyo derecha."); break;
        case GuideWorkflowStep::PaintLeft: message = tr("Pinte primero la zona de apoyo izquierda."); break;
        case GuideWorkflowStep::PaintBridge: message = tr("Una las dos zonas por debajo de la espina nasal."); break;
        case GuideWorkflowStep::Holes:
            message = m_guidePlan.type == GuideType::LeFort
                          ? tr("Planifique primero las placas; sus perforaciones se transferirán a la guía.")
                          : tr("Coloque al menos dos perforaciones de fijación.");
            break;
        case GuideWorkflowStep::Slots: message = tr("Marque los dos extremos de una hendidura de corte."); break;
        default: message = tr("Complete el paso actual antes de continuar."); break;
        }
        statusBar()->showMessage(message);
        return;
    }
    if (m_guidePlan.workflowStep == GuideWorkflowStep::PaintRight)
        m_guidePlan.rightPaintEnd = static_cast<int>(m_guidePlan.paint.size());
    else if (m_guidePlan.workflowStep == GuideWorkflowStep::PaintLeft)
        m_guidePlan.leftPaintEnd = static_cast<int>(m_guidePlan.paint.size());

    const int next = std::min(static_cast<int>(GuideWorkflowStep::Build),
                              static_cast<int>(m_guidePlan.workflowStep) + 1);
    m_guidePlan.workflowStep = static_cast<GuideWorkflowStep>(next);
    activateGuideWorkflowStep();
    updateGuideUi();
}

void MainWindow::retreatGuideWorkflow()
{
    const int previous = std::max(static_cast<int>(GuideWorkflowStep::Envelope),
                                  static_cast<int>(m_guidePlan.workflowStep) - 1);
    m_guidePlan.workflowStep = static_cast<GuideWorkflowStep>(previous);
    activateGuideWorkflowStep();
    updateGuideUi();
}

void MainWindow::setGuidePointMode(int mode)
{
    // Everything but the plate holes is marked on the bone before the cut: leave the planned view first.
    if (m_guidePlannedView && mode != kModeNone && mode != kModePlateHoles) {
        m_guidePlannedView = false;
        if (m_guidePlateViewButton) {
            QSignalBlocker blocker(m_guidePlateViewButton);
            m_guidePlateViewButton->setChecked(false);
        }
        syncGuideView();
    }
    m_guidePointMode = mode;
    if (mode != kModeSlotEnds)
        m_guidePendingEnds.clear();
    if (mode != kModeTube)
        m_guidePendingTubePoints.clear();
    const std::vector<std::pair<QPushButton*, int>> owners = {{m_guideRegionButton, kModeRegion},
                                                              {m_guideSlotEndsButton, kModeSlotEnds},
                                                              {m_guideHoleButton, kModeHoles},
                                                              {m_guidePlaceFigureButton, kModeFigure},
                                                              {m_guideTubeButton, kModeTube},
                                                              {m_guidePlateHolesButton, kModePlateHoles},
                                                              {m_guideMoveHoleButton, kModeMoveHoles},
                                                              {m_guideMarkHolesButton, kModeMarkHoles}};
    for (const auto& [button, owned] : owners) {
        if (!button)
            continue;
        QSignalBlocker blocker(button);
        button->setChecked(mode == owned);
    }
    if (mode != kModeSculpt && mode != kModeTrim)
        for (QToolButton* tool : m_guideSculptTools) {
            if (!tool->isCheckable())
                continue;
            QSignalBlocker blocker(tool);
            tool->setChecked(false);
        }
    if (m_guideView) {
        // The region and the sculpting tools use the surface brush; the other modes pick points.
        m_guideView->setSurfaceBrushMode(mode == kModeRegion || mode == kModeSculpt);
        m_guideView->setPointPickMode(mode == kModeSlotEnds || mode == kModeHoles || mode == kModeFigure ||
                                      mode == kModeTrim || mode == kModeTube || mode == kModePlateHoles ||
                                      mode == kModeMarkHoles);
        for (size_t i = 0; i < m_guidePlan.figures.size(); ++i)
            m_guideView->setMeshPickable(figureActorKey(i), mode == kModeTube &&
                                                               m_guidePlan.figures[i].sourceLabel != 0);
        // The drill sites are dragged over the surface; no new ones are added by clicking.
        m_guideView->setPointEditMode(mode == kModeMoveHoles, -1);
        showGuideLeFortHoles();
    }
    updateGuideUi();
}

void MainWindow::onGuideSurfaceBrushed(double x, double y, double z, Qt::KeyboardModifiers modifiers)
{
    if (m_guidePointMode == kModeSculpt) {
        onGuideSculptBrushed(x, y, z, modifiers);
        return;
    }
    if (m_guidePointMode != kModeRegion || !m_guideWrapMesh)
        return;
    GuideBrushStroke stroke;
    stroke.center = {x, y, z};
    stroke.radiusMm = m_guideBrushSpin ? m_guideBrushSpin->value() : 4.0;
    stroke.erase = modifiers.testFlag(Qt::ControlModifier);
    // Dragging reports many positions: a dab only when the brush has moved a fraction of its size.
    if (!m_guidePlan.paint.empty()) {
        const GuideBrushStroke& last = m_guidePlan.paint.back();
        const double moved = std::hypot(std::hypot(x - last.center[0], y - last.center[1]), z - last.center[2]);
        if (last.erase == stroke.erase && std::abs(last.radiusMm - stroke.radiusMm) < 1e-9 &&
            moved < 0.3 * stroke.radiusMm)
            return;
    }
    m_guidePlan.paint.push_back(stroke);

    // Colour the envelope under the dab right away.
    auto* colors = vtkUnsignedCharArray::SafeDownCast(m_guideWrapMesh->GetPointData()->GetArray("GuidePaint"));
    if (!colors) {
        repaintGuideWrap();
    } else {
        auto locator = vtkSmartPointer<vtkPointLocator>::New();
        locator->SetDataSet(m_guideWrapMesh);
        locator->BuildLocator();
        auto ids = vtkSmartPointer<vtkIdList>::New();
        locator->FindPointsWithinRadius(stroke.radiusMm, stroke.center.data(), ids);
        const QColor color = stroke.erase ? kWrapColor : kPaintColor;
        for (vtkIdType i = 0; i < ids->GetNumberOfIds(); ++i) {
            const vtkIdType id = ids->GetId(i);
            colors->SetTypedTuple(id, std::array<unsigned char, 3>{static_cast<unsigned char>(color.red()),
                                                                   static_cast<unsigned char>(color.green()),
                                                                   static_cast<unsigned char>(color.blue())}
                                          .data());
        }
        colors->Modified();
    }
    if (m_guideView)
        m_guideView->render();
}

void MainWindow::onGuideBrushRadiusDragged(double deltaYPixels)
{
    if (m_guidePointMode == kModeSculpt) {
        nudgeGuideBrushSize(-0.05 * deltaYPixels); // Alt + dragging up grows the sculpting brush
        return;
    }
    if (m_guidePointMode != kModeRegion || !m_guideBrushSpin)
        return;
    // Dragging up grows the brush.
    m_guideBrushSpin->setValue(std::clamp(m_guideBrushSpin->value() - 0.05 * deltaYPixels, 0.5, 15.0));
    statusBar()->showMessage(tr("Tamaño del pincel: %1 mm").arg(m_guideBrushSpin->value(), 0, 'f', 1));
}

void MainWindow::onGuideBrushFinished()
{
    if (m_guidePointMode == kModeSculpt) {
        onGuideSculptFinished();
        return;
    }
    updateGuideUi();
}

void MainWindow::repaintGuideWrap()
{
    if (!m_guideWrapMesh || m_guideWrapMesh->GetNumberOfPoints() == 0)
        return;
    auto colors = vtkSmartPointer<vtkUnsignedCharArray>::New();
    colors->SetName("GuidePaint");
    colors->SetNumberOfComponents(3);
    colors->SetNumberOfTuples(m_guideWrapMesh->GetNumberOfPoints());
    const std::array<unsigned char, 3> base{static_cast<unsigned char>(kWrapColor.red()),
                                            static_cast<unsigned char>(kWrapColor.green()),
                                            static_cast<unsigned char>(kWrapColor.blue())};
    for (vtkIdType id = 0; id < m_guideWrapMesh->GetNumberOfPoints(); ++id)
        colors->SetTypedTuple(id, base.data());
    if (!m_guidePlan.paint.empty()) {
        auto locator = vtkSmartPointer<vtkPointLocator>::New();
        locator->SetDataSet(m_guideWrapMesh);
        locator->BuildLocator();
        auto ids = vtkSmartPointer<vtkIdList>::New();
        const std::array<unsigned char, 3> painted{static_cast<unsigned char>(kPaintColor.red()),
                                                   static_cast<unsigned char>(kPaintColor.green()),
                                                   static_cast<unsigned char>(kPaintColor.blue())};
        for (const GuideBrushStroke& stroke : m_guidePlan.paint) { // in order: erasing and repainting both count
            locator->FindPointsWithinRadius(stroke.radiusMm, stroke.center.data(), ids);
            for (vtkIdType i = 0; i < ids->GetNumberOfIds(); ++i)
                colors->SetTypedTuple(ids->GetId(i), stroke.erase ? base.data() : painted.data());
        }
    }
    m_guideWrapMesh->GetPointData()->RemoveArray("GuidePaint");
    m_guideWrapMesh->GetPointData()->SetScalars(colors);
}

void MainWindow::applyGuideLayers()
{
    if (!m_guideView)
        return;
    const auto on = [](QCheckBox* check) { return !check || check->isChecked(); };
    for (int label : GuidePlanCore::SourceLabelsFor(m_guidePlan.type))
        m_guideView->setMeshVisible(objectActorKey(label), on(m_guideShowModelsCheck));
    m_guideView->setMeshVisible(kGuideWrapActorKey, on(m_guideShowWrapCheck));
    m_guideView->setMeshOpacity(kGuideWrapActorKey, m_guideWrapOpacitySpin ? m_guideWrapOpacitySpin->value() : 0.6);
    m_guideView->setMeshVisible(objectActorKey(kGuideMeshLabel), on(m_guideShowGuideCheck));
    m_guideView->setMeshVisible(kGuideBandActorKey, on(m_guideShowModelsCheck) || on(m_guideShowGuideCheck));
    for (size_t i = 0; i < m_guidePlan.figures.size(); ++i)
        m_guideView->setMeshVisible(figureActorKey(i), on(m_guideShowFiguresCheck));
    m_guideView->render();
}

void MainWindow::onGuidePointPicked(int, double x, double y, double z)
{
    const std::array<double, 3> point{x, y, z};
    const auto normalAt = [this](const std::array<double, 3>& p) {
        return m_guidePrepared.ok ? GuideDesignCore::SurfaceNormalAt(m_guidePrepared, p)
                                  : std::array<double, 3>{0.0, 0.0, 1.0};
    };
    if (m_guidePointMode == kModeMarkHoles) {
        LeFortHoleContext context;
        QString error;
        if (!guideLeFortHoleContext(context, &error)) {
            statusBar()->showMessage(error, 8000);
            return;
        }
        const LeFortProposedHole hole = LeFortHoleCore::AddHole(point, context);
        if (hole.support.verdict == LeFortSupportVerdict::Rejected) {
            statusBar()->showMessage(tr("Orificio no colocado: %1").arg(hole.support.reason), 10000);
            return;
        }
        m_guidePlan.lefortHoles.push_back(hole);
        statusBar()->showMessage(hole.support.verdict == LeFortSupportVerdict::Warning
                                     ? tr("Orificio %1 colocado con aviso: %2").arg(m_guidePlan.lefortHoles.size()).arg(hole.support.reason)
                                     : tr("Orificio %1 colocado: %2 mm de hueso.")
                                           .arg(m_guidePlan.lefortHoles.size())
                                           .arg(hole.support.thicknessMm, 0, 'f', 1),
                                 10000);
        showGuideLeFortHoles();
        if (m_guideReportLabel)
            m_guideReportLabel->setText(guideLeFortHoleReport());
        updateGuideUi();
        return;
    }
    switch (m_guidePointMode) {
    case kModeRegion:
        m_guidePlan.contour.push_back(point);
        break;
    case kModeSlotEnds: {
        const auto* item = m_guideCutList ? m_guideCutList->currentItem() : nullptr;
        if (!item) {
            statusBar()->showMessage(tr("Guías: elija primero la osteotomía en la lista de ranuras."));
            return;
        }
        m_guidePendingEnds.push_back(point);
        if (m_guidePendingEnds.size() < 2)
            break;
        GuideSlot slot;
        slot.path = m_guideCuts[static_cast<size_t>(item->data(Qt::UserRole).toInt())].path;
        slot.start = m_guidePendingEnds[0];
        slot.end = m_guidePendingEnds[1];
        slot.hasExtent = true;
        m_guidePlan.slotPlan.push_back(slot);
        m_guidePendingEnds.clear();
        m_guideCutList->currentItem()->setCheckState(Qt::Checked);
        break;
    }
    case kModeTrim:
        m_guideTrimPoints.push_back(point);
        break;
    case kModePlateHoles: {
        // Picked on the bone in its planned position; the screw goes in along the surface normal there.
        PlateHole hole;
        hole.center = point;
        // The screw's axis — and so the drill's vector and the guide's sleeve — is the bone's own normal, not
        // the envelope's, whose gradient swings tens of degrees where the wrap is coarse.
        hole.axis = m_guidePlannedPrepared.ok ? GuideDesignCore::SurfaceNormalAt(m_guidePlannedPrepared, point)
                                              : std::array<double, 3>{0.0, 0.0, 1.0};
        if (m_guidePlannedKeepOut.bone)
            hole.axis = PlateCore::BoneNormalAt(*m_guidePlannedKeepOut.bone, point, hole.axis);
        // A screw needs bone all round its head and room to the osteotomy. One on a bony margin holds nothing,
        // and the guide's sleeve would stand on air there (user's rule, 2026-09-20).
        const HoleSeat seat = guidePlateHoleSeat(hole.center, hole.axis);
        if (!seat.ok) {
            statusBar()->showMessage(seat.reason, 6000);
            if (m_guidePlateCheckLabel) {
                m_guidePlateCheckLabel->setText(QStringLiteral("⚠ ") + seat.reason);
                m_guidePlateCheckLabel->setStyleSheet(QStringLiteral("color:#c87e1e; font-size:11px;"));
            }
            break;
        }
        m_guidePendingPlateHoles.push_back(hole);
        break;
    }
    case kModeHoles: {
        GuideFixationHole hole;
        hole.center = point;
        hole.axis = normalAt(point);
        hole.diameterMm = m_guideHoleDiameterSpin ? m_guideHoleDiameterSpin->value() : 2.0;
        m_guidePlan.holes.push_back(hole);
        break;
    }
    case kModeFigure: {
        // Seated on the surface, its z axis along the normal and centred in the guide wall, so a figure that
        // is longer than the wall goes through it.
        GuideFigure figure = guideFigureFromInputs();
        const auto normal = normalAt(point);
        const double wallMiddle = m_guideClearanceSpin->value() + 0.5 * m_guideThicknessSpin->value();
        const std::array<double, 3> center{x + normal[0] * wallMiddle, y + normal[1] * wallMiddle,
                                           z + normal[2] * wallMiddle};
        figure.matrix = GuideDesignCore::FrameAt(center, normal);
        m_guidePlan.figures.push_back(figure);
        refreshGuideFigureList();
        if (m_guideFigureList)
            m_guideFigureList->setCurrentRow(static_cast<int>(m_guidePlan.figures.size()) - 1);
        syncGuideView();
        break;
    }
    case kModeTube: {
        m_guidePendingTubePoints.push_back(point);
        if (m_guidePendingTubePoints.size() < 3)
            break;
        GuideFigure tube;
        tube.shape = GuideFigureShape::CurvedTube;
        tube.operation = GuideFigureOperation::Add;
        tube.diameterMm = m_guideTubeDiameterSpin ? m_guideTubeDiameterSpin->value() : 3.0;
        const auto outward = normalAt(m_guidePendingTubePoints.front());
        const auto& start = m_guidePendingTubePoints.front();
        const auto& end = m_guidePendingTubePoints.back();
        const double chord = std::hypot(std::hypot(end[0] - start[0], end[1] - start[1]), end[2] - start[2]);
        double minimumBulge = std::max({8.0, 2.5 * tube.diameterMm, 0.25 * chord});
        const auto leFort = m_guidePlan.type == GuideType::LeFort
            ? guideSourceMeshForLabel(kLeFortSegLabel) : vtkSmartPointer<vtkPolyData>{};
        for (int attempt = 0; attempt < 10; ++attempt) {
            tube.controlPoints = GuideDesignCore::OutwardTubeControlPoints(
                m_guidePendingTubePoints, outward, minimumBulge);
            if (!leFort || leFort->GetNumberOfPolys() == 0)
                break;

            auto distance = vtkSmartPointer<vtkImplicitPolyDataDistance>::New();
            distance->SetInput(leFort);
            const auto centerline = GuideDesignCore::CurvedTubeCenterline(tube);
            double closest = std::numeric_limits<double>::max();
            // The ends intentionally meet the guide and splint. The remaining
            // centreline must clear the bone by the tube radius plus 2 mm.
            const size_t margin = std::max<size_t>(3, centerline.size() / 10);
            for (size_t i = margin; i + margin < centerline.size(); ++i) {
                double sample[3] = {centerline[i][0], centerline[i][1], centerline[i][2]};
                closest = std::min(closest, std::abs(distance->EvaluateFunction(sample)));
            }
            if (closest >= 0.5 * tube.diameterMm + 2.0)
                break;
            minimumBulge += std::max(3.0, 0.08 * chord);
        }
        m_guidePlan.figures.push_back(tube);
        m_guidePendingTubePoints.clear();
        refreshGuideFigureList();
        if (m_guideFigureList)
            m_guideFigureList->setCurrentRow(static_cast<int>(m_guidePlan.figures.size()) - 1);
        syncGuideView();
        break;
    }
    default:
        return;
    }
    rebuildGuideMarkers();
    updateGuideUi();
}

void MainWindow::updateGuideFigureInputs()
{
    if (!m_guideFigureShapeCombo)
        return;
    const auto shape = static_cast<GuideFigureShape>(m_guideFigureShapeCombo->currentData().toInt());
    const auto show = [](QDoubleSpinBox* box, bool visible) {
        if (box)
            box->setVisible(visible);
    };
    const bool cylinder = shape == GuideFigureShape::Cylinder;
    const bool box = shape == GuideFigureShape::Box;
    const bool sphere = shape == GuideFigureShape::Sphere;
    show(m_guideFigureDiameterSpin, cylinder || sphere);
    show(m_guideFigureLengthSpin, cylinder);
    show(m_guideFigureWidthSpin, box);
    show(m_guideFigureHeightSpin, box);
    show(m_guideFigureDepthSpin, box);
    // The labels of hidden rows go with them.
    for (auto* spinBox : {m_guideFigureDiameterSpin, m_guideFigureLengthSpin, m_guideFigureWidthSpin,
                          m_guideFigureHeightSpin, m_guideFigureDepthSpin}) {
        if (!spinBox || !spinBox->parentWidget())
            continue;
        const auto labels = spinBox->parentWidget()->findChildren<QLabel*>();
        for (QLabel* label : labels)
            if (label->buddy() == spinBox)
                label->setVisible(spinBox->isVisibleTo(spinBox->parentWidget()));
    }
}

GuideFigure MainWindow::guideFigureFromInputs() const
{
    GuideFigure figure;
    figure.shape = static_cast<GuideFigureShape>(m_guideFigureShapeCombo->currentData().toInt());
    figure.operation = static_cast<GuideFigureOperation>(m_guideFigureOperationCombo->currentData().toInt());
    figure.diameterMm = m_guideFigureDiameterSpin->value();
    figure.lengthMm = m_guideFigureLengthSpin->value();
    figure.widthMm = m_guideFigureWidthSpin->value();
    figure.heightMm = m_guideFigureHeightSpin->value();
    figure.depthMm = m_guideFigureDepthSpin->value();
    return figure;
}

void MainWindow::refreshGuideFigureList()
{
    if (!m_guideFigureList)
        return;
    const int current = m_guideFigureList->currentRow();
    QSignalBlocker blocker(m_guideFigureList);
    m_guideFigureList->clear();
    for (const GuideFigure& figure : m_guidePlan.figures)
        m_guideFigureList->addItem(figureName(figure));
    if (current >= 0 && current < m_guideFigureList->count())
        m_guideFigureList->setCurrentRow(current);
}

void MainWindow::importGuideFigure()
{
    const QString path =
        QFileDialog::getOpenFileName(this, tr("Importar figura"), QString(), tr("STL (*.stl)"));
    if (path.isEmpty())
        return;
    const auto mesh = loadFigureMesh(path);
    if (!mesh) {
        QMessageBox::warning(this, tr("Guías"), tr("No se pudo leer %1.").arg(path));
        return;
    }
    GuideFigure figure;
    figure.shape = GuideFigureShape::Mesh;
    figure.operation = static_cast<GuideFigureOperation>(m_guideFigureOperationCombo->currentData().toInt());
    figure.mesh = mesh;
    figure.sourcePath = path;
    // Placed at the middle of the marked region when there is one; the gizmo takes it from there.
    std::array<double, 3> center{0.0, 0.0, 0.0};
    if (!m_guidePlan.contour.empty()) {
        for (const auto& p : m_guidePlan.contour)
            for (int a = 0; a < 3; ++a)
                center[static_cast<size_t>(a)] += p[static_cast<size_t>(a)] / static_cast<double>(m_guidePlan.contour.size());
    } else if (m_guideWrapMesh) {
        double b[6] = {};
        m_guideWrapMesh->GetBounds(b);
        center = {0.5 * (b[0] + b[1]), 0.5 * (b[2] + b[3]), 0.5 * (b[4] + b[5])};
    }
    const auto normal = m_guidePrepared.ok ? GuideDesignCore::SurfaceNormalAt(m_guidePrepared, center)
                                           : std::array<double, 3>{0.0, 0.0, 1.0};
    figure.matrix = GuideDesignCore::FrameAt(center, normal);
    m_guidePlan.figures.push_back(figure);
    refreshGuideFigureList();
    if (m_guideFigureList)
        m_guideFigureList->setCurrentRow(static_cast<int>(m_guidePlan.figures.size()) - 1);
    syncGuideView();
    updateGuideUi();
    statusBar()->showMessage(tr("Guías: figura importada; muévala con «Mover figura (gizmo)»."));
}

void MainWindow::resolveGuideFigureMesh(GuideFigure& figure)
{
    if (figure.shape != GuideFigureShape::Mesh || figure.mesh)
        return;
    if (figure.sourceLabel != 0) {
        const auto source = repositionMeshForLabel(figure.sourceLabel);
        if (source && source->GetNumberOfPolys() > 0) {
            figure.mesh = vtkSmartPointer<vtkPolyData>::New();
            figure.mesh->DeepCopy(source);
        }
        return;
    }
    figure.mesh = loadFigureMesh(figure.sourcePath);
}

void MainWindow::addGuideSplintCopy()
{
    const int label = m_guideSplintCopyCombo ? m_guideSplintCopyCombo->currentData().toInt() : 0;
    const auto source = repositionMeshForLabel(label);
    if (!source || source->GetNumberOfPolys() == 0) {
        QMessageBox::warning(this, tr("Guías"), tr("Primero cree la férula seleccionada."));
        return;
    }
    GuideFigure copy;
    copy.shape = GuideFigureShape::Mesh;
    copy.operation = GuideFigureOperation::Add;
    copy.sourceLabel = label;
    copy.mesh = vtkSmartPointer<vtkPolyData>::New();
    copy.mesh->DeepCopy(source);
    if (m_guidePlan.type == GuideType::LeFort) {
        const auto movedLeFort = repositionMeshForLabel(kLeFortSegLabel);
        const auto guideLeFort = guideSourceMeshForLabel(kLeFortSegLabel);
        const auto toPreoperative = SplintDesignCore::RigidMotion(movedLeFort, guideLeFort, 0.2);
        if (!toPreoperative) {
            QMessageBox::warning(
                this, tr("Guías"),
                tr("No se pudo alinear la férula con el Le Fort preoperatorio. Revise que el segmento conserve su geometría rígida."));
            return;
        }
        copy.matrix = *toPreoperative;
    }
    m_guidePlan.figures.push_back(copy);
    refreshGuideFigureList();
    if (m_guideFigureList)
        m_guideFigureList->setCurrentRow(static_cast<int>(m_guidePlan.figures.size()) - 1);
    syncGuideView();
    updateGuideUi();
    statusBar()->showMessage(
        m_guidePlan.type == GuideType::LeFort
            ? tr("Guías: la copia de la férula quedó encajada con el Le Fort preoperatorio. Únala con tubos curvos.")
            : tr("Guías: se añadió una copia independiente de la férula. Únala con tubos curvos."));
}

void MainWindow::removeGuideFigure()
{
    const int row = m_guideFigureList ? m_guideFigureList->currentRow() : -1;
    if (row < 0 || row >= static_cast<int>(m_guidePlan.figures.size()))
        return;
    if (m_guideMoveFigureButton && m_guideMoveFigureButton->isChecked())
        m_guideMoveFigureButton->setChecked(false);
    m_guidePlan.figures.erase(m_guidePlan.figures.begin() + row);
    refreshGuideFigureList();
    syncGuideView();
    updateGuideUi();
}

void MainWindow::setGuideFigureGizmo(bool active)
{
    if (!m_guideView)
        return;
    if (!active) {
        if (m_guideView->hasGizmo())
            m_guideView->stopGizmo(); // emits gizmoMeshUpdated → onGuideGizmoUpdated
        return;
    }
    const int row = m_guideFigureList ? m_guideFigureList->currentRow() : -1;
    if (row < 0 || row >= static_cast<int>(m_guidePlan.figures.size())) {
        QSignalBlocker blocker(m_guideMoveFigureButton);
        m_guideMoveFigureButton->setChecked(false);
        statusBar()->showMessage(tr("Guías: elija la figura en la lista para moverla."));
        return;
    }
    setGuidePointMode(kModeNone);
    m_guideView->startGizmo(figureActorKey(static_cast<size_t>(row)));
    statusBar()->showMessage(tr("Guías: mueva y gire la figura; vuelva a pulsar «Mover figura» para fijarla."));
}

void MainWindow::onGuideGizmoUpdated(int label, vtkSmartPointer<vtkPolyData>)
{
    for (size_t i = 0; i < m_guidePlan.figures.size(); ++i) {
        if (figureActorKey(i) != label)
            continue;
        // The gizmo's matrix is what it applied since it started: put the figure's frame through it.
        auto current = vtkSmartPointer<vtkMatrix4x4>::New();
        current->DeepCopy(m_guidePlan.figures[i].matrix.data());
        auto moved = vtkSmartPointer<vtkMatrix4x4>::New();
        vtkMatrix4x4::Multiply4x4(m_guideView->lastGizmoTransformMatrix(), current, moved);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                m_guidePlan.figures[i].matrix[static_cast<size_t>(4 * r + c)] = moved->GetElement(r, c);
        syncGuideView();
        return;
    }
}

void MainWindow::rebuildGuideMarkers()
{
    if (!m_guideView)
        return;
    m_guideView->clearPointMarkers();
    if (m_guidePlannedView) {
        // The planned bone: accepted pillars plus the pillar currently being marked.
        for (const auto& pillar : m_guidePendingPlatePillars)
            for (const PlateHole& hole : pillar)
                m_guideView->addPointMarker(hole.center[0], hole.center[1], hole.center[2], QColor(48, 209, 88));
        for (const PlateHole& hole : m_guidePendingPlateHoles)
            m_guideView->addPointMarker(hole.center[0], hole.center[1], hole.center[2], kPlateHoleColor);
        m_guideView->removeOverlay(kGuideRegionOverlayKey);
        m_guideView->render();
        return;
    }
    // Editing handles are contextual. Keeping every historical point visible
    // made a finished cutting guide look covered in unrelated colored spheres.
    if (m_guidePointMode == kModeRegion)
        for (const auto& p : m_guidePlan.contour)
            m_guideView->addPointMarker(p[0], p[1], p[2], kRegionColor);
    if (m_guidePointMode == kModeTrim)
        for (const auto& p : m_guideTrimPoints)
            m_guideView->addPointMarker(p[0], p[1], p[2], kSubtractColor);
    if (m_guidePointMode == kModeSlotEnds) {
        for (const auto& slot : m_guidePlan.slotPlan)
            if (slot.hasExtent) {
                m_guideView->addPointMarker(slot.start[0], slot.start[1], slot.start[2], kSlotEndColor);
                m_guideView->addPointMarker(slot.end[0], slot.end[1], slot.end[2], kSlotEndColor);
            }
        for (const auto& p : m_guidePendingEnds)
            m_guideView->addPointMarker(p[0], p[1], p[2], kSlotEndColor);
    }
    if (m_guidePointMode == kModeTube)
        for (const auto& p : m_guidePendingTubePoints)
            m_guideView->addPointMarker(p[0], p[1], p[2], kAddColor);
    if (m_guidePointMode == kModeHoles)
        for (const auto& hole : m_guidePlan.holes)
            m_guideView->addPointMarker(hole.center[0], hole.center[1], hole.center[2], kHoleColor);

    // While marking, the outline is drawn the way the guide will be cut: rounded and laid on the surface.
    vtkSmartPointer<vtkPolyData> outline;
    if (m_guidePrepared.ok && m_guidePlan.paint.empty() && GuideBaseCore::ContourValid(m_guidePlan.contour)) {
        GuideBaseParams params = m_guidePlan.design.base;
        params.cornerRadiusMm = m_guideCornerSpin ? m_guideCornerSpin->value() : params.cornerRadiusMm;
        const GuideRegion region = GuideBaseCore::MakeRegion(m_guidePrepared.wrapField, m_guidePlan.contour, params);
        if (region.valid)
            outline = GuideBaseCore::RegionOutline(region, 0.3);
    }
    if (outline && outline->GetNumberOfLines() > 0)
        m_guideView->setOverlayPolyline(kGuideRegionOverlayKey, outline, kRegionColor, 3.0);
    else if (m_guidePlan.contour.size() >= 2)
        m_guideView->setOverlayPolyline(kGuideRegionOverlayKey, GuideBaseCore::ContourPolyline(m_guidePlan.contour),
                                        kRegionColor, 2.0);
    else
        m_guideView->removeOverlay(kGuideRegionOverlayKey);
    m_guideView->render();
}

void MainWindow::clearGuideRegion()
{
    m_guidePlan.contour.clear();
    m_guidePlan.paint.clear();
    m_guidePlan.rightPaintEnd = 0;
    m_guidePlan.leftPaintEnd = 0;
    if (m_guideWrapMesh)
        m_guidePlan.workflowStep = GuideWorkflowStep::PaintRight;
    repaintGuideWrap();
    if (m_guideView)
        m_guideView->render();
    rebuildGuideMarkers();
    activateGuideWorkflowStep();
    updateGuideUi();
}

void MainWindow::clearGuideSlotEnds()
{
    m_guidePlan.slotPlan.clear();
    m_guidePendingEnds.clear();
    if (m_guideWrapMesh)
        m_guidePlan.workflowStep = GuideWorkflowStep::Slots;
    rebuildGuideMarkers();
    updateGuideUi();
}

void MainWindow::clearGuideHoles()
{
    m_guidePlan.holes.clear();
    if (m_guideWrapMesh)
        m_guidePlan.workflowStep = GuideWorkflowStep::Holes;
    rebuildGuideMarkers();
    updateGuideUi();
}

std::vector<GuideSlot> MainWindow::guideChosenSlots() const
{
    // Only the osteotomies ticked in the list get a slot, with the ends the user placed when there are any.
    std::vector<GuideSlot> chosen;
    for (int row = 0; row < (m_guideCutList ? m_guideCutList->count() : 0); ++row) {
        auto* item = m_guideCutList->item(row);
        if (item->checkState() != Qt::Checked)
            continue;
        const OsteotomyPath& path = m_guideCuts[static_cast<size_t>(item->data(Qt::UserRole).toInt())].path;
        bool placed = false;
        for (const GuideSlot& slot : m_guidePlan.slotPlan)
            if (slot.hasExtent && slot.path.points == path.points) {
                chosen.push_back(slot);
                placed = true;
            }
        if (!placed) {
            GuideSlot slot;
            slot.path = path; // no ends marked: the whole region, still short of the rim
            chosen.push_back(slot);
        }
    }
    // The second slit of an impaction runs along the band's upper edge. Its pieces follow that edge, not a
    // remembered cut, so they are taken while the band they came from is still the movement's: a change of
    // movement drops them until the guide is generated again.
    if (m_guidePlan.type == GuideType::LeFort && !chosen.empty()) {
        const LeFortBandProfile band = guideLeFortBand();
        if (band.ok && !band.spans.empty())
            for (const GuideSlot& slot : m_guidePlan.slotPlan)
                if (slot.hasExtent && slot.path.points == band.upperCut.points)
                    chosen.push_back(slot);
    }
    return chosen;
}

void MainWindow::buildGuideMesh()
{
    if (m_guideSculptEdited &&
        QMessageBox::question(this, tr("Guías"),
                              tr("Se perderán las ediciones de la guía. ¿Crear la guía de nuevo?")) !=
            QMessageBox::Yes)
        return;
    if (!m_guideWrapMesh || m_guideWrapMesh->GetNumberOfPolys() == 0) {
        QMessageBox::warning(this, tr("Guías"), tr("Calcule primero la envolvente."));
        return;
    }
    QString error;
    const bool brushed = GuideBaseCore::PaintValid(m_guidePlan.paint);
    if (!brushed && !GuideBaseCore::ContourValid(m_guidePlan.contour, &error)) {
        QMessageBox::warning(this, tr("Guías"), tr("Pinte con el pincel la zona de apoyo de la guía."));
        return;
    }
    if (m_guideMoveFigureButton && m_guideMoveFigureButton->isChecked())
        m_guideMoveFigureButton->setChecked(false); // fix the figure being moved first
    m_guidePlan.design.base.thicknessMm = m_guideThicknessSpin->value();
    m_guidePlan.design.base.clearanceMm = m_guideClearanceSpin->value();
    m_guidePlan.design.base.smallestDetailMm = m_guideDetailSpin->value();
    m_guidePlan.design.base.edgeTaperMm = m_guideTaperSpin->value();
    m_guidePlan.design.base.edgeThicknessFraction = m_guideEdgeFractionSpin->value() / 100.0;
    m_guidePlan.design.base.edgeRoundMm = m_guideEdgeRoundSpin->value();
    if (m_guideCornerSpin)
        m_guidePlan.design.base.cornerRadiusMm = m_guideCornerSpin->value();
    m_guidePlan.design.slot.bladeThicknessMm = m_guideBladeSpin->value();
    m_guidePlan.design.slot.smallestDetailMm = m_guideDetailSpin->value();
    m_guidePlan.design.edgeMarginMm = m_guideMarginSpin->value();

    const std::vector<GuideSlot> chosen = guideChosenSlots();
    // Imported figures and copied splints are reloaded when a project is opened.
    for (GuideFigure& figure : m_guidePlan.figures)
        resolveGuideFigureMesh(figure);

    QApplication::setOverrideCursor(Qt::WaitCursor);
    statusBar()->showMessage(tr("Guías: construyendo…"));
    if (!m_guidePrepared.ok)
        m_guidePrepared = GuideDesignCore::Prepare(m_guideWrapMesh, m_guidePlan.design);
    GuideDesignResult result;
    if (m_guidePrepared.ok) {
        // Kept for the edit session: its keep-out field is built from this very region and these slots.
        m_guideRegion =
            brushed ? GuideBaseCore::MakeBrushRegion(m_guidePrepared.wrapField, m_guidePlan.paint, m_guidePlan.design.base)
                    : GuideBaseCore::MakeRegion(m_guidePrepared.wrapField, m_guidePlan.contour, m_guidePlan.design.base);
        m_guideBuiltSlots = chosen;
        // A drill sleeve at every predictive hole of the plates, carved with everything else.
        m_guidePlan.sleeve = guideSleeveParams();
        m_guideBuiltFigures = guideFiguresWithSleeves();
        // The Le Fort guide is laid on the envelope and nothing else: its inner face is the envelope plus the
        // clearance (user's request, 2026-10-06: "la guía debe crearse sobre el envolvente, no sobre el hueso").
        // The envelope no longer sinks into the bone it wraps, so the bone clip only made the guide follow the
        // segmentation's holes. The bone field stays for judging the drill sites.
        GuideDesignParams design = m_guidePlan.design;
        if (m_guidePlan.type == GuideType::LeFort)
            design.bone.reset();
        result = GuideDesignCore::Build(m_guidePrepared, m_guideRegion, chosen, m_guidePlan.holes,
                                        m_guideBuiltFigures, design);
    }
    QApplication::restoreOverrideCursor();
    if (!m_guidePrepared.ok) {
        QMessageBox::warning(this, tr("Guías"), m_guidePrepared.error);
        return;
    }
    if (!result.ok) {
        QMessageBox::warning(this, tr("Guías"), result.error);
        return;
    }
    m_guideMesh = result.mesh;
    m_guidePlan.workflowStep = GuideWorkflowStep::Complete;
    m_guideSculptEdited = false;
    if (m_guideSculptActive)
        setGuideEditActive(false); // the clay is stale: the guide was carved again
    if (!objectEntryExists(kGuideMeshLabel))
        addObjectEntry(tr("Guía quirúrgica"), kGuideColor, kGuideMeshLabel);
    setRepositionMeshForLabel(kGuideMeshLabel, m_guideMesh);
    syncGuideView();
    applyGuideThicknessColors();
    updateGuideUi();
    updateButtonStates();
    QString report = result.report;
    // The assistant's Le Fort guides are two by design, right and left.
    const int expectedPieces = m_guidePlan.type == GuideType::LeFort && !m_guidePlan.lefortHoles.empty() ? 2 : 1;
    if (result.pieces > expectedPieces)
        report += QStringLiteral(" ") + tr("Atención: la guía quedó en %1 piezas; suba el margen al borde, acorte "
                                           "las ranuras o revise las figuras restadas.").arg(result.pieces);
    if (m_guideReportLabel)
        m_guideReportLabel->setText(report);
    statusBar()->showMessage(report);
}

void MainWindow::applyGuideThicknessColors()
{
    if (!m_guideView || !m_guideMesh)
        return;
    const int key = objectActorKey(kGuideMeshLabel);
    const bool on = m_guideThicknessCheck && m_guideThicknessCheck->isChecked();
    if (on) {
        const double nominal = m_guidePlan.design.base.thicknessMm;
        SplintHeightmapGenerator::ApplyThicknessColors(m_guideMesh, 0.6 * nominal, 1.4 * nominal);
        m_guideView->addMesh(key, m_guideMesh, meshLabelName(kGuideMeshLabel));
    }
    m_guideView->setMeshScalarColoring(key, on);
    m_guideView->render();
}

void MainWindow::exportGuideStl()
{
    if (!m_guideMesh || m_guideMesh->GetNumberOfPolys() == 0) {
        QMessageBox::warning(this, tr("Guías"), tr("Primero cree la guía."));
        return;
    }
    // The assistant's Le Fort guides are two: one STL each, in a folder.
    if (m_guidePlan.type == GuideType::LeFort && !m_guidePlan.lefortHoles.empty()) {
        const QString folder = QFileDialog::getExistingDirectory(this, tr("Carpeta para las guías DER e IZQ"));
        if (folder.isEmpty())
            return;
        QString report;
        if (!exportLeFortGuideFiles(folder, &report)) {
            QMessageBox::warning(this, tr("Guías"), report);
            return;
        }
        statusBar()->showMessage(report);
        return;
    }
    const QString suggested = m_guidePlan.type == GuideType::Chin ? QStringLiteral("guia_menton.stl")
                                                                  : QStringLiteral("guia_lefort.stl");
    const QString path = QFileDialog::getSaveFileName(this, tr("Exportar guía"), suggested, tr("STL (*.stl)"));
    if (path.isEmpty())
        return;
    vtkSmartPointer<vtkPolyData> output = m_guideMesh;
    QString validation;
    const MeshCheck check = MeshRepairCore::Analyze(m_guideMesh);
    if (check.Valid()) {
        validation = tr("STL válido: %1").arg(check.Summary());
    } else {
        const MeshRepairResult repaired = MeshRepairCore::Repair(m_guideMesh);
        output = repaired.ok ? repaired.mesh : m_guideMesh;
        validation = repaired.ok ? repaired.report : tr("STL con avisos: %1").arg(repaired.after.Summary());
    }
    auto writer = vtkSmartPointer<vtkSTLWriter>::New();
    writer->SetFileName(path.toUtf8().constData());
    writer->SetInputData(output);
    writer->SetFileTypeToBinary();
    if (writer->Write() != 1) {
        QMessageBox::warning(this, tr("Guías"), tr("No se pudo escribir %1.").arg(path));
        return;
    }
    statusBar()->showMessage(tr("Guía exportada: %1 · %2").arg(path, validation));
}

void MainWindow::syncGuideView()
{
    if (!m_guideView)
        return;
    const bool gizmoRunning = m_guideView->hasGizmo();
    if (gizmoRunning)
        return; // rebuilding the scene would drop the figure being moved
    m_guideView->clearMeshes(true);
    if (m_guidePlannedView) {
        // The bone where the plan puts it, with the plates on it: this is what the surgeon screws together.
        for (int label : {kLeFortCranialLabel, kLeFortSegLabel}) {
            const auto mesh = repositionMeshForLabel(label);
            if (!mesh || mesh->GetNumberOfPolys() == 0)
                continue;
            const int key = objectActorKey(label);
            m_guideView->addMesh(key, mesh, meshLabelName(label));
            m_guideView->setMeshColor(key, objectColorForLabel(label));
            m_guideView->setMeshOpacity(key, 1.0);
        }
        for (size_t i = 0; i < m_guidePlateMeshes.size(); ++i) {
            if (!m_guidePlateMeshes[i] || i >= m_guidePlan.plates.size())
                continue;
            const int key = kGuidePlateActorBase - static_cast<int>(i);
            m_guideView->addMesh(key, m_guidePlateMeshes[i], m_guidePlan.plates[i].name);
            m_guideView->setMeshColor(key, kPlateColor);
            m_guideView->setMeshPickable(key, false); // holes are picked on the bone
        }
        rebuildGuideMarkers();
        return;
    }
    // Le Fort uses the pre-reposition frame so its saved osteotomy and slot coincide.
    // Other guide types keep their planned-position sources.
    for (int label : GuidePlanCore::SourceLabelsFor(m_guidePlan.type)) {
        const auto mesh = guideSourceMeshForLabel(label);
        if (!mesh || mesh->GetNumberOfPolys() == 0)
            continue;
        const int key = objectActorKey(label);
        m_guideView->addMesh(key, mesh, meshLabelName(label));
        m_guideView->setMeshColor(key, objectColorForLabel(label));
        m_guideView->setMeshOpacity(key, 1.0);
    }
    if (m_guideWrapMesh && m_guideWrapMesh->GetNumberOfPolys() > 0) {
        if (!m_guideWrapMesh->GetPointData()->GetArray("GuidePaint"))
            repaintGuideWrap();
        m_guideView->addMesh(kGuideWrapActorKey, m_guideWrapMesh, tr("Envolvente"));
        m_guideView->setMeshColor(kGuideWrapActorKey, kWrapColor);
        m_guideView->setMeshScalarColoring(kGuideWrapActorKey, true); // teal, with the brushed region in blue
        m_guideView->setMeshOpacity(kGuideWrapActorKey, m_guideWrapOpacitySpin ? m_guideWrapOpacitySpin->value() : 0.6);
    }
    if (m_guideMesh && m_guideMesh->GetNumberOfPolys() > 0) {
        const int key = objectActorKey(kGuideMeshLabel);
        m_guideView->addMesh(key, m_guideMesh, meshLabelName(kGuideMeshLabel));
        m_guideView->setMeshColor(key, kGuideColor);
    }
    if (m_guidePlan.type == GuideType::LeFort && m_guideBandMesh && m_guideBandMesh->GetNumberOfPolys() > 0) {
        // The bone to take out, drawn over the guide so it reads through it.
        m_guideView->addMesh(kGuideBandActorKey, m_guideBandMesh, tr("Franja a quitar"));
        m_guideView->setMeshColor(kGuideBandActorKey, CranioPalette::resection());
        m_guideView->setMeshDisplayOptions(kGuideBandActorKey, 0.85, true);
        m_guideView->setMeshPickable(kGuideBandActorKey, false);
    }
    for (size_t i = 0; i < m_guidePlan.figures.size(); ++i) {
        GuideFigure& figure = m_guidePlan.figures[i];
        resolveGuideFigureMesh(figure);
        const auto preview = GuideDesignCore::FigurePreview(figure);
        if (!preview || preview->GetNumberOfPolys() == 0)
            continue;
        const int key = figureActorKey(i);
        m_guideView->addMesh(key, preview, figureName(figure));
        m_guideView->setMeshColor(key, figure.operation == GuideFigureOperation::Add ? kAddColor : kSubtractColor);
        m_guideView->setMeshOpacity(key, 0.55);
        m_guideView->setMeshPickable(key, m_guidePointMode == kModeTube && figure.sourceLabel != 0);
    }
    rebuildGuideMarkers();
    applyGuideLayers();
}

void MainWindow::updateGuideUi()
{
    const bool hasWrap = m_guideWrapMesh && m_guideWrapMesh->GetNumberOfPolys() > 0;
    const bool hasRegion = GuideBaseCore::PaintValid(m_guidePlan.paint) || GuideBaseCore::ContourValid(m_guidePlan.contour);
    const bool hasGuide = m_guideMesh && m_guideMesh->GetNumberOfPolys() > 0;
    const auto plateSegment = repositionMeshForLabel(kLeFortSegLabel);
    const auto plateCranium = repositionMeshForLabel(kLeFortCranialLabel);
    const bool hasPlateAnatomy = plateSegment && plateSegment->GetNumberOfPolys() > 0 &&
                                 plateCranium && plateCranium->GetNumberOfPolys() > 0;
    const bool figureSelected = m_guideFigureList && m_guideFigureList->currentRow() >= 0;
    const auto showSection = [](QWidget* section, bool visible) {
        if (section)
            section->setVisible(visible);
    };
    const bool leFort = m_guidePlan.type == GuideType::LeFort;
    const bool hasPlates = !m_guidePlan.plates.empty();
    const int pendingPlateHoles = static_cast<int>(m_guidePendingPlateHoles.size());
    const int acceptedPillars = static_cast<int>(m_guidePendingPlatePillars.size());
    const GuideWorkflowStep workflow = hasWrap ? m_guidePlan.workflowStep : GuideWorkflowStep::Envelope;
    // GUIAS and PLACAS share the renderer and project data, but each module
    // presents only the controls for its own clinical product.
    showSection(m_guideTypeSection, !m_guidePlateWorkspace && workflow == GuideWorkflowStep::Envelope);
    showSection(m_guideAutomaticSection, !m_guidePlateWorkspace);
    showSection(m_guidePlateSection, m_guidePlateWorkspace && leFort);
    showSection(m_guideAdvancedSection, !m_guidePlateWorkspace && workflow == GuideWorkflowStep::Complete);
    showSection(m_guidePlateAdvancedSection, m_guidePlateWorkspace);
    if (m_guideGenerateButton)
        m_guideGenerateButton->setVisible(!m_guidePlateWorkspace && leFort && hasWrap);
    const bool hasSites = leFort && !m_guidePlan.lefortHoles.empty();
    if (m_guideMoveHoleButton)
        m_guideMoveHoleButton->setVisible(!m_guidePlateWorkspace && hasWrap && hasSites);
    if (m_guideMarkHolesButton)
        m_guideMarkHolesButton->setVisible(!m_guidePlateWorkspace && leFort && hasWrap);
    if (m_guideClearHolesButton)
        m_guideClearHolesButton->setVisible(!m_guidePlateWorkspace && hasWrap && hasSites);
    if (m_guideAcceptHolesButton) {
        // Also after a proposal that found nothing: the surgeon is never left without a way on (user's case,
        // 2026-10-05). Such a guide has only its positioning screws.
        const bool proposed = hasSites || (leFort && !m_guideLeFortMissing.isEmpty());
        m_guideAcceptHolesButton->setVisible(!m_guidePlateWorkspace && hasWrap && proposed);
        m_guideAcceptHolesButton->setText(!hasSites ? tr("4 · Crear guías sin orificios de placa")
                                          : hasGuide ? tr("4 · Reconstruir guías con los orificios")
                                                     : tr("4 · Aceptar orificios y crear guías"));
    }
    const bool leFortAssistant = !m_guidePlateWorkspace && leFort && hasWrap;
    if (m_guideRootsButton)
        m_guideRootsButton->setVisible(leFortAssistant);
    if (m_guideRootsLabel)
        m_guideRootsLabel->setVisible(leFortAssistant && !m_guideRootsLabel->text().isEmpty());
    if (m_guideBandBox)
        m_guideBandBox->setVisible(leFortAssistant);
    if (m_guideCaseEdit) {
        m_guideCaseEdit->setVisible(leFortAssistant);
        if (!m_guideCaseEdit->hasFocus() && m_guideCaseEdit->text() != m_guidePlan.caseLabel)
            m_guideCaseEdit->setText(m_guidePlan.caseLabel);
    }
    if (m_guidePlateStepLabel) {
        const QString stepText = acceptedPillars < kPlatePillarCount
                                     ? tr("%1 de 4 · %2").arg(acceptedPillars + 1).arg(platePillarName(acceptedPillars))
                                     : tr("4 de 4 · Pilares completos");
        m_guidePlateStepLabel->setText(stepText +
            (acceptedPillars < kPlatePillarCount
                 ? tr("\nMarque al menos cuatro puntos de arriba abajo: dos en cráneo y dos en Le Fort.")
                 : tr("\nYa puede generar el diseño seleccionado.")));
    }
    if (m_guidePlateArmButton) {
        m_guidePlateArmButton->setText(acceptedPillars + 1 < kPlatePillarCount
                                           ? tr("Aceptar pilar y continuar")
                                           : acceptedPillars < kPlatePillarCount ? tr("Aceptar último pilar")
                                                                                 : tr("Pilares completos"));
        m_guidePlateArmButton->setEnabled(acceptedPillars < kPlatePillarCount && pendingPlateHoles >= 4 && hasPlateAnatomy);
    }
    if (m_guidePlateViewButton) m_guidePlateViewButton->setEnabled(hasPlateAnatomy);
    if (m_guidePlateHolesButton) {
        m_guidePlateHolesButton->setEnabled(hasPlateAnatomy && acceptedPillars < kPlatePillarCount);
        m_guidePlateHolesButton->setText(acceptedPillars < kPlatePillarCount
                                             ? tr("Marcar · %1").arg(platePillarName(acceptedPillars))
                                             : tr("Marcación terminada"));
    }
    if (m_guidePlateCreateButton) {
        const int design = m_guidePlateTemplateCombo ? m_guidePlateTemplateCombo->currentData().toInt()
                                                      : kPlateDesignConventional;
        m_guidePlateCreateButton->setText(design == kPlateDesignConventional ? tr("Crear 4 placas")
                                             : design == kPlateDesignThreePsi ? tr("Crear 3 PSI")
                                                                               : tr("Crear PSI monobloque"));
        m_guidePlateCreateButton->setEnabled(hasPlateAnatomy && acceptedPillars == kPlatePillarCount &&
                                             pendingPlateHoles == 0);
    }
    if (m_guidePlateExportButton) m_guidePlateExportButton->setEnabled(hasPlates);
    showSection(m_guideLayersSection, !m_guidePlateWorkspace && hasWrap && !m_guidePlannedView);
    const bool paintStep = workflow == GuideWorkflowStep::PaintRight ||
                           workflow == GuideWorkflowStep::PaintLeft ||
                           workflow == GuideWorkflowStep::PaintBridge;
    showSection(m_guideRegionSection, !m_guidePlateWorkspace && hasWrap && paintStep);
    showSection(m_guideHoleSection, !m_guidePlateWorkspace && workflow == GuideWorkflowStep::Holes &&
                                          !(leFort && hasPlates));
    showSection(m_guideSlotSection, !m_guidePlateWorkspace && workflow == GuideWorkflowStep::Slots);
    showSection(m_guideFiguresSection, !m_guidePlateWorkspace && workflow == GuideWorkflowStep::Complete);
    showSection(m_guideBuildSection, !m_guidePlateWorkspace &&
        (workflow == GuideWorkflowStep::Build || workflow == GuideWorkflowStep::Complete));
    if (m_guideBuildButton)
        m_guideBuildButton->setText(hasGuide ? tr("Reconstruir guía") : tr("Crear guía"));
    showSection(m_guideEditSection, !m_guidePlateWorkspace && hasGuide && workflow == GuideWorkflowStep::Complete);
    showSection(m_guideExportSection, !m_guidePlateWorkspace && hasGuide && workflow == GuideWorkflowStep::Complete);
    updateGuideSculptBar();
    if (m_guideRegionButton) m_guideRegionButton->setEnabled(hasWrap);
    if (m_guideSlotEndsButton) m_guideSlotEndsButton->setEnabled(hasWrap && m_guideCutList && m_guideCutList->count() > 0);
    if (m_guideHoleButton) m_guideHoleButton->setEnabled(hasWrap);
    if (m_guidePlaceFigureButton) m_guidePlaceFigureButton->setEnabled(hasWrap);
    if (m_guideMoveFigureButton) m_guideMoveFigureButton->setEnabled(figureSelected);
    if (m_guideBuildButton) {
        const bool hasDrillHoles = leFort && hasPlates ? !guidePredictiveHoles().empty()
                                                       : m_guidePlan.holes.size() >= 2;
        m_guideBuildButton->setEnabled(hasWrap && hasRegion && hasDrillHoles &&
            std::any_of(m_guidePlan.slotPlan.begin(), m_guidePlan.slotPlan.end(),
                        [](const GuideSlot& slot) { return slot.hasExtent; }));
    }
    if (m_guideExportButton) m_guideExportButton->setEnabled(hasGuide);
    if (m_guideThicknessCheck) m_guideThicknessCheck->setEnabled(hasGuide);

    if (m_guideWorkflowLabel) {
        QString text;
        switch (workflow) {
        case GuideWorkflowStep::Envelope: text = tr("1 de 7 · Calcular modelo envolvente"); break;
        case GuideWorkflowStep::PaintRight: text = tr("2 de 7 · Pintar apoyo derecho"); break;
        case GuideWorkflowStep::PaintLeft: text = tr("3 de 7 · Pintar apoyo izquierdo"); break;
        case GuideWorkflowStep::PaintBridge: text = tr("4 de 7 · Unión bajo la espina nasal"); break;
        case GuideWorkflowStep::Holes:
            text = leFort && hasPlates ? tr("5 de 7 · Verificar perforaciones de las placas")
                                       : tr("5 de 7 · Colocar perforaciones");
            break;
        case GuideWorkflowStep::Slots: text = tr("6 de 7 · Marcar hendiduras de corte"); break;
        case GuideWorkflowStep::Build: text = tr("7 de 7 · Crear la guía"); break;
        case GuideWorkflowStep::Complete: text = tr("Guía creada · lista para revisar"); break;
        }
        m_guideWorkflowLabel->setText(text);
    }
    if (m_guideWorkflowBackButton) {
        m_guideWorkflowBackButton->setVisible(workflow != GuideWorkflowStep::Envelope);
        m_guideWorkflowBackButton->setEnabled(hasWrap);
    }
    if (m_guideWorkflowNextButton) {
        const bool canAdvance = workflow >= GuideWorkflowStep::PaintRight && workflow <= GuideWorkflowStep::Slots;
        m_guideWorkflowNextButton->setVisible(canAdvance);
        m_guideWorkflowNextButton->setEnabled(canAdvance && guideWorkflowStepComplete());
    }
    if (m_guideRegionButton) {
        if (workflow == GuideWorkflowStep::PaintRight)
            m_guideRegionButton->setText(tr("Pintar apoyo derecho"));
        else if (workflow == GuideWorkflowStep::PaintLeft)
            m_guideRegionButton->setText(tr("Pintar apoyo izquierdo"));
        else if (workflow == GuideWorkflowStep::PaintBridge)
            m_guideRegionButton->setText(tr("Pintar unión subnasal"));
    }

    if (!m_guideHintLabel)
        return;
    QString hint;
    if (m_guidePointMode == kModePlateHoles)
        hint = tr("%1: marque de arriba abajo, cruzando el corte, y acepte el pilar para continuar.")
                   .arg(platePillarName(acceptedPillars));
    else if (m_guidePlateWorkspace && !hasPlateAnatomy)
        hint = tr("Complete la osteotomía y la reposición del Le Fort antes de planificar los implantes.");
    else if (m_guidePlateWorkspace && m_guidePlannedView)
        hint = tr("Le Fort definitivo: marque los agujeros de arriba abajo, cruzando el borde de la osteotomía.");
    else if (!hasWrap)
        hint = m_guidePlan.type == GuideType::Chin ? tr("Calcule la envolvente del mentón y la mandíbula.")
                                                   : tr("Calcule la envolvente del Le Fort y el cráneo.");
    else if (workflow == GuideWorkflowStep::PaintRight)
        hint = tr("Pinte únicamente el apoyo sobre el lado derecho del Le Fort. Ctrl borra.");
    else if (workflow == GuideWorkflowStep::PaintLeft)
        hint = tr("Pinte únicamente el apoyo sobre el lado izquierdo del Le Fort. Ctrl borra.");
    else if (workflow == GuideWorkflowStep::PaintBridge)
        hint = tr("Con el pincel pequeño una ambos apoyos por debajo de la espina nasal.");
    else if (workflow == GuideWorkflowStep::Holes)
        hint = leFort && hasPlates
                   ? tr("Las perforaciones predictivas son las mismas de las placas y ya están incorporadas a la guía.")
                   : tr("Marque al menos dos perforaciones de fijación sobre las zonas pintadas.");
    else if (workflow == GuideWorkflowStep::Slots)
        hint = m_guidePendingEnds.empty() ? tr("Seleccione Le Fort I y marque el inicio de la hendidura.")
                                          : tr("Marque el final de la hendidura sobre la osteotomía.");
    else if (workflow == GuideWorkflowStep::Build)
        hint = tr("Revise que las tres zonas formen una sola región y pulse «Crear guía».");
    else if (workflow == GuideWorkflowStep::Complete && !m_guideSculptActive)
        hint = tr("Revise la guía, suavícela si hace falta y exporte el STL.");
    else if (m_guidePointMode == kModeSlotEnds)
        hint = m_guidePendingEnds.empty() ? tr("Elija la osteotomía y marque el inicio de la ranura.")
                                          : tr("Marque el final de la ranura.");
    else if (m_guidePointMode == kModeFigure)
        hint = tr("Haga clic donde quiera la figura.");
    else if (m_guidePointMode == kModeTube)
        hint = m_guidePendingTubePoints.empty()
            ? tr("Marque el inicio del tubo sobre la guía.")
            : (m_guidePendingTubePoints.size() == 1 ? tr("Marque el punto que define la curva.")
                                                    : tr("Marque el final sobre la copia de la férula."));
    else if (m_guidePointMode == kModeHoles)
        hint = tr("Haga clic en cada agujero de fijación.");
    else if (m_guidePointMode == kModeTrim)
        hint = tr("Marque el contorno del recorte y pulse Intro.");
    else if (m_guidePointMode == kModeSculpt)
        hint = tr("Arrastre sobre la guía · Ctrl invierte · Alt + arrastre cambia el tamaño.");
    else if (!hasGuide)
        hint = tr("Ajuste el espesor y pulse «Crear guía».");
    else
        hint = tr("Revise el mapa de espesor y exporte el STL.");
    m_guideHintLabel->setText(hint);
}

QJsonObject MainWindow::guidePlanJson() const
{
    if (m_guidePlan.contour.empty() && m_guidePlan.paint.empty() && m_guidePlan.figures.empty() &&
        m_guidePlan.plates.empty() && !m_guideWrapMesh)
        return {};
    return GuidePlanCore::ToJson(m_guidePlan);
}

void MainWindow::restoreGuidePlan(const ProjectState& state)
{
    m_guideCuts.clear();
    for (const QJsonValue& value : state.osteotomyPlan.value(QStringLiteral("executedCuts")).toArray()) {
        const QJsonObject cut = value.toObject();
        rememberOsteotomyCut(cut.value(QStringLiteral("name")).toString(),
                             OsteotomyCore::PathFromJson(cut.value(QStringLiteral("path")).toObject()),
                             cut.value(QStringLiteral("type")).toString() == QStringLiteral("chin") ? GuideType::Chin
                                                                                                   : GuideType::LeFort);
    }
    const bool hasLeFortCut = std::any_of(m_guideCuts.begin(), m_guideCuts.end(),
                                          [](const GuideCutOption& cut) { return cut.type == GuideType::LeFort; });
    if (!hasLeFortCut) {
        const OsteotomyPath recovered = recoveredLeFortPath();
        if (recovered.valid)
            rememberOsteotomyCut(tr("Le Fort I"), recovered, GuideType::LeFort);
    }
    if (state.guidesPlan.isEmpty()) {
        m_guidePlan = GuidePlan{};
        m_guideWrapMesh = nullptr;
        m_guideMesh = nullptr;
        m_guidePrepared = GuidePreparation{};
        refreshGuideSources();
        refreshGuideCutList();
        refreshGuideFigureList();
        updateGuideUi();
        return;
    }
    m_guidePlan = GuidePlanCore::FromJson(state.guidesPlan);
    m_guideWrapMesh = nullptr;
    m_guideMesh = nullptr;
    m_guidePrepared = GuidePreparation{};
    // Plates reload as plans; their meshes are rebuilt on the planned bone when they are next shown.
    m_guidePlannedPrepared = GuidePreparation{};
    m_guidePlateMeshes.clear();
    m_guidePendingPlateHoles.clear();
    m_guidePendingPlatePillars.clear();
    m_guidePlannedView = false;
    if (m_guidePlateThicknessSpin) m_guidePlateThicknessSpin->setValue(m_guidePlan.plate.thicknessMm);
    if (m_guidePlateMinCutSpin) m_guidePlateMinCutSpin->setValue(m_guidePlan.plate.minCutDistanceMm);
    if (m_guideSleeveBoreSpin) m_guideSleeveBoreSpin->setValue(m_guidePlan.sleeve.boreDiameterMm);
    if (m_guideSleeveOuterSpin) m_guideSleeveOuterSpin->setValue(m_guidePlan.sleeve.outerDiameterMm);
    if (m_guideSleeveHeightSpin) m_guideSleeveHeightSpin->setValue(m_guidePlan.sleeve.heightMm);
    // The cuts the saved slots follow are offered again in the list.
    int index = 1;
    for (const GuideSlot& slot : m_guidePlan.slotPlan)
        rememberOsteotomyCut(tr("Osteotomía %1").arg(index++), slot.path, m_guidePlan.type);
    QStringList missingFiles;
    for (GuideFigure& figure : m_guidePlan.figures) {
        resolveGuideFigureMesh(figure);
        if (figure.shape == GuideFigureShape::Mesh && !figure.mesh) {
            missingFiles << (figure.sourceLabel != 0 ? meshLabelName(figure.sourceLabel) : figure.sourcePath);
        }
    }
    if (m_guideTypeCombo) {
        QSignalBlocker blocker(m_guideTypeCombo);
        m_guideTypeCombo->setCurrentIndex(m_guideTypeCombo->findData(static_cast<int>(m_guidePlan.type)));
    }
    // Le Fort guides saved with the old 1.5 mm closing get the new one: their envelope kept the bone's holes.
    if (m_guideGapSpin)
        m_guideGapSpin->setValue(m_guidePlan.type == GuideType::LeFort
                                     ? std::max(m_guidePlan.wrap.gapClosingMm, kGuideGapClosingMm)
                                     : m_guidePlan.wrap.gapClosingMm);
    if (m_guideDetailSpin) m_guideDetailSpin->setValue(m_guidePlan.wrap.smallestDetailMm);
    if (m_guideCornerSpin) m_guideCornerSpin->setValue(m_guidePlan.design.base.cornerRadiusMm);
    if (m_guideBrushSpin && !m_guidePlan.paint.empty()) m_guideBrushSpin->setValue(m_guidePlan.paint.back().radiusMm);
    if (m_guideThicknessSpin) m_guideThicknessSpin->setValue(m_guidePlan.design.base.thicknessMm);
    if (m_guideClearanceSpin) m_guideClearanceSpin->setValue(m_guidePlan.design.base.clearanceMm);
    if (m_guideBladeSpin) m_guideBladeSpin->setValue(m_guidePlan.design.slot.bladeThicknessMm);
    if (m_guideMarginSpin) m_guideMarginSpin->setValue(m_guidePlan.design.edgeMarginMm);
    if (m_guideTaperSpin) m_guideTaperSpin->setValue(m_guidePlan.design.base.edgeTaperMm);
    if (m_guideEdgeFractionSpin) m_guideEdgeFractionSpin->setValue(100.0 * m_guidePlan.design.base.edgeThicknessFraction);
    if (m_guideEdgeRoundSpin) m_guideEdgeRoundSpin->setValue(m_guidePlan.design.base.edgeRoundMm);
    refreshGuideSources();
    refreshGuideCutList();
    refreshGuideFigureList();
    refreshGuidePlates();
    updateGuideUi();
    if (!missingFiles.isEmpty())
        statusBar()->showMessage(tr("Guías: no se encontraron las figuras importadas %1.")
                                     .arg(missingFiles.join(QStringLiteral(", "))));
}

// ─────────────────────────────────────────────────────────────────────────────
// EDITAR: the finished guide as clay
//
// `GuideSculptCore` owns the grid and the tools; this half is the palette, the
// contextual bar under it and the wiring of the surface brush to whichever tool
// is active. The preview during a drag is the raw contour at ~10 Hz; letting go
// re-contours the guide properly and that mesh becomes `m_guideMesh`.
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::setGuideEditActive(bool active)
{
    if (!active) {
        m_guideSculptActive = false;
        setGuidePointMode(kModeNone);
        clearGuideTrim();
        if (m_guideSculptPreviewTimer)
            m_guideSculptPreviewTimer->stop();
        if (m_guideEditButton && m_guideEditButton->isChecked()) {
            QSignalBlocker blocker(m_guideEditButton);
            m_guideEditButton->setChecked(false);
        }
        if (m_guideShowModelsCheck) {
            QSignalBlocker blocker(m_guideShowModelsCheck);
            m_guideShowModelsCheck->setChecked(true);
        }
        if (m_guideWrapOpacitySpin) {
            QSignalBlocker blocker(m_guideWrapOpacitySpin);
            m_guideWrapOpacitySpin->setValue(1.0);
        }
        applyGuideLayers();
        updateGuideUi();
        return;
    }
    if (!m_guideMesh || m_guideMesh->GetNumberOfPolys() == 0) {
        QMessageBox::warning(this, tr("Guías"), tr("Primero cree la guía."));
        if (m_guideEditButton) {
            QSignalBlocker blocker(m_guideEditButton);
            m_guideEditButton->setChecked(false);
        }
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    statusBar()->showMessage(tr("Guías: preparando la edición…"));
    // The clay is the guide itself, with room around it for material to be added.
    const double detail = std::clamp(m_guidePlan.design.base.smallestDetailMm, 0.15, 0.5);
    QString error;
    const bool ready = m_guideSculpt.Reset(m_guideMesh, detail, 4.0, &error);
    if (ready) {
        SculptLimits limits;
        limits.wrapField = m_guidePrepared.wrapField;
        limits.clearanceMm = m_guidePlan.design.base.clearanceMm;
        // Whatever the plan cut away stays cut away: slots, holes and subtracted figures, baked once.
        double bounds[6] = {};
        m_guideMesh->GetBounds(bounds);
        for (int axis = 0; axis < 3; ++axis) {
            bounds[2 * axis] -= 5.0;
            bounds[2 * axis + 1] += 5.0;
        }
        const auto keepOut = GuideDesignCore::KeepOutNode(m_guidePrepared, m_guideRegion, m_guideBuiltSlots,
                                                          m_guidePlan.holes, m_guideBuiltFigures, m_guidePlan.design,
                                                          bounds);
        if (keepOut)
            limits.keepOut = ImplicitCore::BakeFunction(
                [keepOut](const std::array<double, 3>& p) { return ImplicitCore::Value(keepOut, p); }, bounds, detail);
        m_guideSculpt.SetLimits(limits);
    }
    QApplication::restoreOverrideCursor();
    if (!ready) {
        QMessageBox::warning(this, tr("Guías"), error.isEmpty() ? tr("No se pudo preparar la edición.") : error);
        if (m_guideEditButton) {
            QSignalBlocker blocker(m_guideEditButton);
            m_guideEditButton->setChecked(false);
        }
        return;
    }
    m_guideSculptActive = true;
    m_guideSculptEdited = false;
    if (!m_guideSculptPreviewTimer) {
        m_guideSculptPreviewTimer = new QTimer(this);
        m_guideSculptPreviewTimer->setInterval(100); // ~10 Hz while dragging
        connect(m_guideSculptPreviewTimer, &QTimer::timeout, this, [this] {
            if (!m_guideSculptDirty)
                return;
            m_guideSculptDirty = false;
            refreshGuideSculptMesh(true);
        });
    }
    // Only the guide matters while sculpting: the bone goes away and the envelope fades behind it.
    if (m_guideShowModelsCheck) {
        QSignalBlocker blocker(m_guideShowModelsCheck);
        m_guideShowModelsCheck->setChecked(false);
    }
    if (m_guideShowGuideCheck) {
        QSignalBlocker blocker(m_guideShowGuideCheck);
        m_guideShowGuideCheck->setChecked(true);
    }
    if (m_guideWrapOpacitySpin) {
        QSignalBlocker blocker(m_guideWrapOpacitySpin);
        m_guideWrapOpacitySpin->setValue(0.25);
    }
    applyGuideLayers();
    setGuideSculptTool(kToolSmooth);
    statusBar()->showMessage(tr("Guías: edición lista. Ctrl invierte, Alt + arrastre cambia el tamaño."));
}

void MainWindow::setGuideSculptTool(int tool)
{
    m_guideSculptTool = tool;
    for (int index = 0; index < m_guideSculptTools.size(); ++index) {
        QToolButton* button = m_guideSculptTools[index];
        if (!button->isCheckable())
            continue;
        QSignalBlocker blocker(button);
        button->setChecked(index == tool);
    }
    if (tool != kToolTrim)
        clearGuideTrim();
    setGuidePointMode(tool == kToolTrim ? kModeTrim : kModeSculpt);
    updateGuideSculptBar();
}

void MainWindow::updateGuideSculptBar()
{
    const bool editing = m_guideSculptActive;
    if (m_guideSculptPalette)
        m_guideSculptPalette->setVisible(editing);
    if (m_guideSculptBar)
        m_guideSculptBar->setVisible(editing);
    if (m_guideEditButton)
        m_guideEditButton->setText(editing ? tr("Terminar la edición") : tr("Editar la guía"));
    for (int index = 0; index < m_guideSculptTools.size(); ++index)
        m_guideSculptTools[index]->setEnabled(editing &&
                                              (index != kToolUndo || m_guideSculpt.CanUndo()) &&
                                              (index != kToolRedo || m_guideSculpt.CanRedo()));
    if (!editing)
        return;
    const bool trimming = m_guideSculptTool == kToolTrim;
    if (m_guideSculptSizeRow)
        m_guideSculptSizeRow->setVisible(!trimming);
    if (m_guideSculptTrimRow)
        m_guideSculptTrimRow->setVisible(trimming);
    // The level only means something where a tool reads it.
    const bool hasLevel = m_guideSculptTool == kToolSmooth || m_guideSculptTool == kToolWax ||
                          m_guideSculptTool == kToolFlatten;
    if (m_guideSculptLevelRow)
        m_guideSculptLevelRow->setVisible(hasLevel);
    if (m_guideSculptLevelLabel)
        m_guideSculptLevelLabel->setText(m_guideSculptTool == kToolWax ? tr("Calor") : tr("Nivel"));

    QStringList modes;
    int current = 0;
    switch (m_guideSculptTool) {
    case kToolSmooth:
        modes = {tr("Dentro"), tr("Alrededor")};
        current = m_guideSmoothScope;
        break;
    case kToolWax:
        modes = {tr("Derretir"), tr("Suavizar"), tr("Añadir"), tr("Quitar")};
        current = m_guideWaxMode;
        break;
    case kToolFlatten:
        modes = {tr("Aplanar"), tr("Rascar"), tr("Rellenar")};
        current = m_guideFlattenMode;
        break;
    default:
        break;
    }
    if (m_guideSculptModeRow)
        m_guideSculptModeRow->setVisible(!modes.isEmpty());
    for (int index = 0; index < m_guideSculptModes.size(); ++index) {
        QToolButton* mode = m_guideSculptModes[index];
        const bool used = index < modes.size();
        mode->setVisible(used);
        if (!used)
            continue;
        mode->setText(modes[index]);
        QSignalBlocker blocker(mode);
        mode->setChecked(index == current);
    }
}

void MainWindow::nudgeGuideBrushSize(double deltaMm)
{
    if (!m_guideSculptSizeSpin)
        return;
    m_guideSculptSizeSpin->setValue(std::clamp(m_guideSculptSizeSpin->value() + deltaMm, 0.5, 20.0));
    statusBar()->showMessage(tr("Tamaño del pincel: %1 mm").arg(m_guideSculptSizeSpin->value(), 0, 'f', 1));
}

void MainWindow::onGuideSculptBrushed(double x, double y, double z, Qt::KeyboardModifiers modifiers)
{
    if (!m_guideSculptActive || !m_guideSculpt.Ready())
        return;
    if (!m_guideSculptStroking) {
        m_guideSculpt.BeginStroke();
        m_guideSculptStroking = true;
        m_guideSculptHasLast = false;
        if (m_guideSculptPreviewTimer)
            m_guideSculptPreviewTimer->start();
    }
    SculptBrush brush;
    brush.center = {x, y, z};
    brush.previous = m_guideSculptLast;
    brush.hasPrevious = m_guideSculptHasLast;
    brush.radiusMm = m_guideSculptSizeSpin ? m_guideSculptSizeSpin->value() : 3.0;
    brush.level = m_guideSculptLevelSlider ? m_guideSculptLevelSlider->value() / 100.0 : 0.5;
    brush.scope = m_guideSmoothScope == 1 ? SmoothScope::Around : SmoothScope::Inside;
    brush.flatten = m_guideFlattenMode == 1   ? FlattenMode::Scrape
                    : m_guideFlattenMode == 2 ? FlattenMode::Fill
                                              : FlattenMode::Flatten;
    const bool invert = modifiers.testFlag(Qt::ControlModifier); // Ctrl turns a tool into its opposite
    switch (m_guideSculptTool) {
    case kToolSmooth:
        brush.tool = SculptTool::Smooth;
        break;
    case kToolWax: {
        brush.tool = SculptTool::HotWax;
        int wax = m_guideWaxMode;
        if (invert && wax == 2)
            wax = 3;
        else if (invert && wax == 3)
            wax = 2;
        brush.wax = wax == 1 ? HotWaxMode::Smooth : wax == 2 ? HotWaxMode::Add : wax == 3 ? HotWaxMode::Remove
                                                                                          : HotWaxMode::Melt;
        break;
    }
    case kToolAdd:
        brush.tool = invert ? SculptTool::Remove : SculptTool::Add;
        break;
    case kToolRemove:
        brush.tool = invert ? SculptTool::Add : SculptTool::Remove;
        break;
    case kToolFlatten:
        brush.tool = SculptTool::Flatten;
        break;
    default:
        return;
    }
    m_guideSculpt.ApplyBrush(brush);
    m_guideSculptLast = brush.center;
    m_guideSculptHasLast = true;
    m_guideSculptEdited = true;
    m_guideSculptDirty = true;
}

void MainWindow::onGuideSculptFinished()
{
    if (!m_guideSculptStroking)
        return;
    m_guideSculptStroking = false;
    m_guideSculptDirty = false;
    if (m_guideSculptPreviewTimer)
        m_guideSculptPreviewTimer->stop();
    m_guideSculpt.EndStroke();
    refreshGuideSculptMesh(false);
    updateGuideSculptBar();
}

void MainWindow::refreshGuideSculptMesh(bool fast)
{
    const auto mesh = m_guideSculpt.Contour(fast);
    if (!mesh || mesh->GetNumberOfPolys() == 0)
        return;
    m_guideMesh = mesh;
    if (m_guideView) {
        const int key = objectActorKey(kGuideMeshLabel);
        m_guideView->addMesh(key, m_guideMesh, meshLabelName(kGuideMeshLabel));
        m_guideView->setMeshColor(key, kGuideColor);
        m_guideView->render();
    }
    if (fast)
        return;
    setRepositionMeshForLabel(kGuideMeshLabel, m_guideMesh);
    applyGuideThicknessColors(); // the thickness map reads the edited guide
}

void MainWindow::applyGuideTrim()
{
    if (!m_guideSculptActive || !m_guideSculpt.Ready())
        return;
    if (m_guideTrimPoints.size() < 3) {
        statusBar()->showMessage(tr("Guías: marque al menos tres puntos del recorte."));
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    // Swept along the direction the camera looks in, so the polygon cuts what the user sees.
    const bool ok = m_guideSculpt.Trim(m_guideTrimPoints, m_guideView ? m_guideView->viewDirection()
                                                                      : std::array<double, 3>{0.0, 1.0, 0.0},
                                       m_guideTrimInvertCheck && m_guideTrimInvertCheck->isChecked(), &error);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        QMessageBox::warning(this, tr("Guías"), error);
        return;
    }
    m_guideSculptEdited = true;
    clearGuideTrim();
    refreshGuideSculptMesh(false);
    updateGuideSculptBar();
    statusBar()->showMessage(tr("Guías: recorte aplicado; se conservó la pieza mayor."));
}

void MainWindow::clearGuideTrim()
{
    m_guideTrimPoints.clear();
    rebuildGuideMarkers();
}

void MainWindow::guideSculptUndo()
{
    if (!m_guideSculptActive || !m_guideSculpt.Undo())
        return;
    refreshGuideSculptMesh(false);
    updateGuideSculptBar();
    statusBar()->showMessage(tr("Guías: edición deshecha."));
}

void MainWindow::guideSculptRedo()
{
    if (!m_guideSculptActive || !m_guideSculpt.Redo())
        return;
    refreshGuideSculptMesh(false);
    updateGuideSculptBar();
    statusBar()->showMessage(tr("Guías: edición rehecha."));
}

bool MainWindow::handleGuideSculptKey(QKeyEvent* event)
{
    if (!m_guideSculptActive || !event || !m_viewModeStack || m_viewModeStack->currentIndex() != 7)
        return false;
    const bool control = event->modifiers().testFlag(Qt::ControlModifier);
    switch (event->key()) {
    case Qt::Key_Z:
        if (!control)
            return false;
        guideSculptUndo();
        return true;
    case Qt::Key_Y:
        if (!control)
            return false;
        guideSculptRedo();
        return true;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        nudgeGuideBrushSize(0.5);
        return true;
    case Qt::Key_Minus:
        nudgeGuideBrushSize(-0.5);
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (m_guideSculptTool != kToolTrim)
            return false;
        applyGuideTrim();
        return true;
    case Qt::Key_Escape:
        if (m_guideSculptTool != kToolTrim || m_guideTrimPoints.empty())
            return false;
        clearGuideTrim();
        statusBar()->showMessage(tr("Guías: recorte descartado."));
        return true;
    default:
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// PLACAS A MEDIDA: patient-specific Le Fort plates and their predictive holes
//
// The plates are designed on the bone in its planned position (the cranial base
// and the repositioned Le Fort segment); the guide stays on the bone before the
// cut. `PlateCore` carries each segment hole back through the inverse of the
// segment's motion, and the guide gets a drill sleeve at every pre-operative
// hole, so drilling through the guide before the osteotomy is what puts the
// maxilla in its planned place when the plate is screwed on.
// ─────────────────────────────────────────────────────────────────────────────
PlateParams MainWindow::guidePlateParams() const
{
    PlateParams params = m_guidePlan.plate;
    if (m_guidePlateThicknessSpin)
        params.thicknessMm = m_guidePlateThicknessSpin->value();
    if (m_guidePlateMinCutSpin)
        params.minCutDistanceMm = m_guidePlateMinCutSpin->value();
    return params;
}

SleeveParams MainWindow::guideSleeveParams() const
{
    SleeveParams params = m_guidePlan.sleeve;
    if (m_guideSleeveBoreSpin)
        params.boreDiameterMm = m_guideSleeveBoreSpin->value();
    if (m_guideSleeveOuterSpin)
        params.outerDiameterMm = m_guideSleeveOuterSpin->value();
    if (m_guideSleeveHeightSpin)
        params.heightMm = m_guideSleeveHeightSpin->value();
    return params;
}

bool MainWindow::guideSegmentMotion(std::array<double, 16>& motion, QString* error) const
{
    motion = {1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0};
    const auto planned = repositionMeshForLabel(kLeFortSegLabel);
    if (!planned || planned->GetNumberOfPolys() == 0) {
        if (error)
            *error = tr("Falta el segmento Le Fort: realice primero la osteotomía.");
        return false;
    }
    const auto original = m_repositionOriginalMeshes.find(kLeFortSegLabel);
    if (original == m_repositionOriginalMeshes.end() || !original->second)
        return true; // not moved yet: the plan is the pre-operative position
    return PlateCore::RigidMotion(original->second, planned, motion, nullptr, error);
}

OsteotomyPath MainWindow::guideLeFortPath() const
{
    // The cut ticked for a slot if there is one, otherwise the first Le Fort cut remembered.
    if (m_guideCutList)
        for (int row = 0; row < m_guideCutList->count(); ++row) {
            const auto* item = m_guideCutList->item(row);
            const size_t index = static_cast<size_t>(item->data(Qt::UserRole).toInt());
            if (item->checkState() == Qt::Checked && index < m_guideCuts.size() &&
                m_guideCuts[index].type == GuideType::LeFort)
                return m_guideCuts[index].path;
        }
    for (const GuideCutOption& cut : m_guideCuts)
        if (cut.type == GuideType::LeFort && cut.path.valid)
            return cut.path;
    return recoveredLeFortPath();
}

std::vector<PredictiveHole> MainWindow::guidePredictiveHoles() const
{
    if (m_guidePlan.type != GuideType::LeFort || m_guidePlan.plates.empty())
        return {};
    std::array<double, 16> motion{};
    if (!guideSegmentMotion(motion))
        return {};
    // Any point of the segment before the cut tells which side of the path it is on.
    std::array<double, 3> probe{0.0, 0.0, 0.0};
    if (const auto before = guideSourceMeshForLabel(kLeFortSegLabel)) {
        double b[6] = {};
        before->GetBounds(b);
        probe = {0.5 * (b[0] + b[1]), 0.5 * (b[2] + b[3]), 0.5 * (b[4] + b[5])};
    }
    return PlateCore::PredictHoles(m_guidePlan.plates, motion, guideLeFortPath(), probe);
}

std::vector<GuideFigure> MainWindow::guideFiguresWithSleeves() const
{
    std::vector<GuideFigure> figures = m_guidePlan.figures;
    if (m_guidePlan.type != GuideType::LeFort)
        return figures;
    // The guide's own sites, proposed or moved. A project saved before them, with plates, keeps drilling the
    // plates' holes until its sites are proposed.
    const std::vector<PredictiveHole> sites = !m_guidePlan.lefortHoles.empty()
                                                  ? LeFortHoleCore::DrillSites(m_guidePlan.lefortHoles)
                                                  : guidePredictiveHoles();
    const auto sleeves = PlateCore::SleeveFigures(sites, guideSleeveParams());
    figures.insert(figures.end(), sleeves.begin(), sleeves.end());
    return figures;
}

bool MainWindow::prepareGuidePlannedBone()
{
    std::array<double, 16> motion{};
    QString error;
    if (!guideSegmentMotion(motion, &error)) {
        QMessageBox::warning(this, tr("Placas"), error);
        return false;
    }
    if (m_guidePlannedPrepared.ok && motion == m_guidePlannedMotion)
        return true;
    const auto cranial = repositionMeshForLabel(kLeFortCranialLabel);
    const auto segment = repositionMeshForLabel(kLeFortSegLabel);
    if (!cranial || cranial->GetNumberOfPolys() == 0) {
        QMessageBox::warning(this, tr("Placas"), tr("Falta la base craneal: realice primero la osteotomía Le Fort."));
        return false;
    }
    // The plate lies on the bone where the plan puts it, bent to it — on a regularised surface: a 3 mm closing
    // covers the perforations and thin walls of segmented maxilla, which a plate copied as ragged patches. The
    // arms still bridge the gap at the cut themselves: the bone query keeps them off anything further than 1 mm
    // from real bone or within 1.5 mm of the cut, which is where the closing fills in.
    WrapParams wrapParams;
    wrapParams.gapClosingMm = 3.0;
    wrapParams.smallestDetailMm = std::min(0.3, m_guideDetailSpin ? m_guideDetailSpin->value() : 0.3);
    GuideDesignParams prepareParams;
    prepareParams.base.smallestDetailMm = wrapParams.smallestDetailMm;
    prepareParams.base.thicknessMm = guidePlateParams().thicknessMm;
    prepareParams.base.clearanceMm = 0.0;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    statusBar()->showMessage(tr("Placas: midiendo el hueso en su posición planificada…"));
    const WrapResult wrap = WrapCore::Wrap({cranial.Get(), segment.Get()}, wrapParams);
    GuidePreparation prepared;
    if (wrap.ok)
        prepared = GuideDesignCore::Prepare(wrap.mesh, prepareParams);
    // Where no plate may go: the same envelope it is laid on, and that envelope with the Le Fort segment ALSO
    // where it was before the movement, so the union fills the space the movement vacated. An arm crossing the
    // cut is pulled taut over the second one, so it ramps across the step instead of dropping into the
    // osteotomy and getting in the way of the maxilla. Both have to be CLOSED bone: segmented maxilla is a
    // perforated shell around an open sinus, and a 0.5 mm closing of it came out shredded — arms passed
    // straight through the sinus and through the gap without the keep-out ever reporting bone.
    PlateKeepOut keepOut;
    if (prepared.ok) {
        // The bone material itself: exact on the surface, so the plate never ends up inside the maxilla.
        keepOut.bone = ImplicitCore::BakeMeshField({cranial.Get(), segment.Get()}, wrapParams.smallestDetailMm, 8.0);
        keepOut.boneAndGap = prepared.wrapField; // not moved yet: there is no gap to ramp over
        const auto original = m_repositionOriginalMeshes.find(kLeFortSegLabel);
        if (original != m_repositionOriginalMeshes.end() && original->second &&
            original->second->GetNumberOfPolys() > 0) {
            statusBar()->showMessage(tr("Placas: midiendo el espacio que deja el movimiento…"));
            const WrapResult both = WrapCore::Wrap({cranial.Get(), segment.Get(), original->second.Get()}, wrapParams);
            if (both.ok) {
                const GuidePreparation bothPrepared = GuideDesignCore::Prepare(both.mesh, prepareParams);
                if (bothPrepared.ok)
                    keepOut.boneAndGap = bothPrepared.wrapField;
            }
        }
    }
    QApplication::restoreOverrideCursor();
    if (!wrap.ok || !prepared.ok) {
        QMessageBox::warning(this, tr("Placas"), wrap.ok ? prepared.error : wrap.error);
        return false;
    }
    m_guidePlannedPrepared = prepared;
    m_guidePlannedKeepOut = keepOut;
    m_guidePlannedMotion = motion;
    m_guidePlateMeshes.clear(); // built on the previous plan: rebuilt on demand
    statusBar()->showMessage(tr("Placas: hueso planificado listo."));
    return true;
}

HoleSeat MainWindow::guidePlateHoleSeat(const std::array<double, 3>& center,
                                        const std::array<double, 3>& axis) const
{
    // Plain nearest-bone queries, with no cut margin: `CheckHoleSeat` measures the distance to the cut itself,
    // so a query that already hides the bone near the cut would report every hole there as "no bone".
    std::array<double, 16> motion{};
    guideSegmentMotion(motion);
    const auto cranial = repositionMeshForLabel(kLeFortCranialLabel);
    const PlateBoneQuery plannedBone =
        PlateCore::MakeBoneQuery(cranial, repositionMeshForLabel(kLeFortSegLabel), motion, {}, 0.0);
    // The ring is judged on the bone before the movement: that is where the drill goes, through the guide.
    const auto original = m_repositionOriginalMeshes.find(kLeFortSegLabel);
    const auto segmentBefore = original != m_repositionOriginalMeshes.end() ? original->second
                                                                           : repositionMeshForLabel(kLeFortSegLabel);
    const PlateBoneQuery preopBone =
        PlateCore::MakeBoneQuery(cranial, segmentBefore, {1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0,
                                                          0.0, 0.0, 0.0, 1.0},
                                 {}, 0.0);
    return PlateCore::CheckHoleSeat(center, axis, plannedBone, preopBone, guideLeFortPath(), motion,
                                    guidePlateParams());
}

PlateBuildResult MainWindow::buildGuidePlate(const PlateDesign& plate) const
{
    // The real bones in their planned position, kept clear of the osteotomy: each arm follows its own bone on
    // the anterior faces and bridges in front of the gap; the gap under each hole is measured on them too.
    std::array<double, 16> motion{};
    guideSegmentMotion(motion);
    const PlateParams params = guidePlateParams();
    // The query works on the centreline. Add half the strip width so the physical plate edge, not merely its
    // centre, remains two millimetres away from the Le Fort and cranial osteotomy borders.
    const double cutMargin = std::max(3.5, params.cutEdgeMarginMm) + 0.5 * params.widthMm;
    const PlateBoneQuery boneAt =
        PlateCore::MakeBoneQuery(repositionMeshForLabel(kLeFortCranialLabel), repositionMeshForLabel(kLeFortSegLabel),
                                 motion, guideLeFortPath(), cutMargin);
    return PlateCore::Build(m_guidePlannedPrepared, plate, params, boneAt, nullptr, m_guidePlannedKeepOut);
}

void MainWindow::ensureGuidePlateMeshes()
{
    if (m_guidePlateMeshes.size() == m_guidePlan.plates.size() || !m_guidePlannedPrepared.ok)
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    m_guidePlateMeshes.clear();
    for (const PlateDesign& plate : m_guidePlan.plates) {
        const PlateBuildResult built = buildGuidePlate(plate);
        m_guidePlateMeshes.push_back(built.ok ? built.mesh : vtkSmartPointer<vtkPolyData>{});
    }
    QApplication::restoreOverrideCursor();
}

void MainWindow::setGuidePlateView(bool planned)
{
    if (planned && !prepareGuidePlannedBone())
        planned = false;
    if (planned)
        ensureGuidePlateMeshes();
    else if (m_guidePointMode == kModePlateHoles)
        setGuidePointMode(kModeNone);
    m_guidePlannedView = planned;
    if (m_guidePlateViewButton) {
        QSignalBlocker blocker(m_guidePlateViewButton);
        m_guidePlateViewButton->setChecked(planned);
    }
    syncGuideView();
    updateGuideUi();
}

void MainWindow::startGuidePlateArm()
{
    if (m_guidePendingPlatePillars.size() >= kPlatePillarCount)
        return;
    if (m_guidePendingPlateHoles.size() < 4) {
        statusBar()->showMessage(tr("Placas: marque al menos cuatro puntos en este pilar, de arriba abajo."));
        return;
    }
    m_guidePendingPlatePillars.push_back(m_guidePendingPlateHoles);
    m_guidePendingPlateHoles.clear();
    if (m_guidePendingPlatePillars.size() == kPlatePillarCount) {
        setGuidePointMode(kModeNone);
        if (m_guidePlateHolesButton) {
            QSignalBlocker blocker(m_guidePlateHolesButton);
            m_guidePlateHolesButton->setChecked(false);
        }
    }
    rebuildGuideMarkers();
    updateGuideUi();
}

void MainWindow::clearGuidePlateHoles()
{
    m_guidePendingPlateHoles.clear();
    m_guidePendingPlatePillars.clear();
    rebuildGuideMarkers();
    updateGuideUi();
}

void MainWindow::createGuidePlate()
{
    if (m_guidePendingPlatePillars.size() != kPlatePillarCount) {
        QMessageBox::warning(this, tr("Placas"),
                             tr("Complete y acepte los cuatro pilares antes de crear el diseño."));
        return;
    }
    if (!prepareGuidePlannedBone())
        return;
    ensureGuidePlateMeshes(); // the list and the meshes stay in step

    const int design = m_guidePlateTemplateCombo ? m_guidePlateTemplateCombo->currentData().toInt()
                                                  : kPlateDesignConventional;
    std::vector<PlateDesign> assigned;
    const auto pillarPlate = [this](int pillar, const QString& name, PlateTemplate kind) {
        PlateDesign plate;
        plate.name = name;
        plate.side = pillar < 2 ? PlateSide::Right : PlateSide::Left;
        plate.kind = kind;
        plate.holes = m_guidePendingPlatePillars[static_cast<size_t>(pillar)];
        plate.struts = PlateCore::TemplateStruts(PlateTemplate::Paranasal,
                                                  static_cast<int>(plate.holes.size()), 0);
        return plate;
    };
    if (design == kPlateDesignConventional) {
        for (int pillar = 0; pillar < kPlatePillarCount; ++pillar) {
            assigned.push_back(pillarPlate(pillar, tr("Placa · %1").arg(platePillarName(pillar)),
                                                   PlateTemplate::Paranasal));
        }
    } else if (design == kPlateDesignThreePsi) {
        assigned.push_back(pillarPlate(1, tr("PSI maxilomalar derecho"), PlateTemplate::Splintless));
        PlateDesign paranasal;
        paranasal.name = tr("PSI paranasal bilateral");
        paranasal.side = PlateSide::Bilateral;
        paranasal.kind = PlateTemplate::Splintless;
        const auto& right = m_guidePendingPlatePillars[0];
        const auto& left = m_guidePendingPlatePillars[2];
        paranasal.holes.insert(paranasal.holes.end(), right.begin(), right.end());
        paranasal.holes.insert(paranasal.holes.end(), left.begin(), left.end());
        paranasal.struts = PlateCore::TemplateStruts(PlateTemplate::LShape,
                                                      static_cast<int>(right.size()), static_cast<int>(left.size()));
        assigned.push_back(std::move(paranasal));
        assigned.push_back(pillarPlate(3, tr("PSI maxilomalar izquierdo"), PlateTemplate::Splintless));
    } else {
        PlateDesign plate;
        plate.name = tr("Placa splintless Le Fort");
        plate.kind = PlateTemplate::Splintless;
        std::array<int, 4> counts{};
        for (int pillar = 0; pillar < kPlatePillarCount; ++pillar) {
            const auto& holes = m_guidePendingPlatePillars[static_cast<size_t>(pillar)];
            counts[static_cast<size_t>(pillar)] = static_cast<int>(holes.size());
            plate.holes.insert(plate.holes.end(), holes.begin(), holes.end());
        }
        plate.struts = PlateCore::SplintlessStruts(counts);
        assigned.push_back(std::move(plate));
    }
    vtkPolyData* cranialMesh = repositionMeshForLabel(kLeFortCranialLabel);
    vtkPolyData* segmentMesh = repositionMeshForLabel(kLeFortSegLabel);
    std::vector<PlateDesign> pillarChecks;
    for (int pillar = 0; pillar < kPlatePillarCount; ++pillar) {
        PlateDesign check;
        check.name = platePillarName(pillar);
        check.holes = m_guidePendingPlatePillars[static_cast<size_t>(pillar)];
        pillarChecks.push_back(std::move(check));
    }
    PlateCore::AssignBones(pillarChecks, cranialMesh, segmentMesh);
    for (const PlateDesign& plate : pillarChecks) {
        const int cranial = static_cast<int>(std::count_if(plate.holes.begin(), plate.holes.end(),
            [](const PlateHole& hole) { return hole.bone == PlateBone::Cranial; }));
        const int segment = static_cast<int>(std::count_if(plate.holes.begin(), plate.holes.end(),
            [](const PlateHole& hole) { return hole.bone == PlateBone::Segment; }));
        if (cranial < 2 || segment < 2) {
            QMessageBox::warning(this, tr("Placas"),
                                 tr("%1 necesita al menos dos puntos en cráneo y dos en Le Fort.")
                                     .arg(plate.name));
            return;
        }
    }
    PlateCore::AssignBones(assigned, cranialMesh, segmentMesh);

    QApplication::setOverrideCursor(Qt::WaitCursor);
    statusBar()->showMessage(design == kPlateDesignConventional ? tr("Placas: construyendo cuatro placas…")
                                     : design == kPlateDesignThreePsi ? tr("Placas: construyendo tres PSI…")
                                                                       : tr("Placas: construyendo PSI monobloque…"));
    std::vector<vtkSmartPointer<vtkPolyData>> meshes;
    QStringList reports;
    QString buildError;
    for (const PlateDesign& plate : assigned) {
        const PlateBuildResult built = buildGuidePlate(plate);
        if (!built.ok) {
            buildError = tr("%1: %2").arg(plate.name, built.error);
            break;
        }
        meshes.push_back(built.mesh);
        reports << built.report;
    }
    QApplication::restoreOverrideCursor();
    if (!buildError.isEmpty()) {
        QMessageBox::warning(this, tr("Placas"), buildError);
        return;
    }
    m_guidePlan.plate = guidePlateParams();
    m_guidePlan.plates.insert(m_guidePlan.plates.end(), assigned.begin(), assigned.end());
    m_guidePlateMeshes.insert(m_guidePlateMeshes.end(), meshes.begin(), meshes.end());
    // The guide is keyed to the definitive plate holes, so an existing guide is stale now.
    m_guideMesh = nullptr;
    m_guideBuiltFigures.clear();
    if (m_guidePlan.workflowStep == GuideWorkflowStep::Complete)
        m_guidePlan.workflowStep = GuideWorkflowStep::Build;
    m_guidePendingPlateHoles.clear();
    m_guidePendingPlatePillars.clear();
    setGuidePointMode(kModeNone);
    refreshGuidePlates();
    syncGuideView();
    if (m_guideReportLabel)
        m_guideReportLabel->setText(reports.join(QStringLiteral("\n")));
    statusBar()->showMessage(design == kPlateDesignConventional ? tr("Placas: cuatro placas creadas.")
                                     : design == kPlateDesignThreePsi ? tr("Placas: tres PSI creados.")
                                                                       : tr("Placas: PSI monobloque creado."));
}

void MainWindow::removeGuidePlate()
{
    const int row = m_guidePlateList ? m_guidePlateList->currentRow() : -1;
    if (row < 0 || row >= static_cast<int>(m_guidePlan.plates.size()))
        return;
    m_guidePlan.plates.erase(m_guidePlan.plates.begin() + row);
    if (m_guidePlateMeshes.size() > static_cast<size_t>(row))
        m_guidePlateMeshes.erase(m_guidePlateMeshes.begin() + row);
    m_guideMesh = nullptr;
    m_guideBuiltFigures.clear();
    if (m_guidePlan.workflowStep == GuideWorkflowStep::Complete)
        m_guidePlan.workflowStep = GuideWorkflowStep::Build;
    refreshGuidePlates();
    syncGuideView();
}

void MainWindow::refreshGuidePlates()
{
    if (m_guidePlateList) {
        const int current = m_guidePlateList->currentRow();
        QSignalBlocker blocker(m_guidePlateList);
        m_guidePlateList->clear();
        for (const PlateDesign& plate : m_guidePlan.plates) {
            int cranial = 0, segment = 0;
            for (const PlateHole& hole : plate.holes) {
                cranial += hole.bone == PlateBone::Cranial ? 1 : 0;
                segment += hole.bone == PlateBone::Segment ? 1 : 0;
            }
            const QString kind = plate.kind == PlateTemplate::Splintless ? tr("splintless")
                                 : plate.kind == PlateTemplate::LShape    ? tr("en L")
                                                                          : tr("convencional");
            m_guidePlateList->addItem(tr("%1 · %2 · %3 + %4 tornillos")
                                          .arg(plate.name, kind)
                                          .arg(cranial)
                                          .arg(segment));
        }
        if (current >= 0 && current < m_guidePlateList->count())
            m_guidePlateList->setCurrentRow(current);
    }
    if (m_guidePlateCheckLabel) {
        QString text;
        QString style = QStringLiteral("color:#8e8e93;");
        std::array<double, 16> motion{};
        QString error;
        bool moved = false;
        const QString motionText = guideMotionSummary(&moved);
        if (m_guidePlan.plates.empty()) {
            text = tr("Las placas se diseñan sobre el hueso en su posición planificada; la guía recibe una camisa "
                      "en cada agujero, en su posición antes del corte.");
        } else if (!guideSegmentMotion(motion, &error)) {
            text = error;
            style = QStringLiteral("color:#ff453a;");
        } else {
            const PlateCheck check = PlateCore::Check(m_guidePlan.plates, guidePredictiveHoles(), guidePlateParams());
            if (check.Ok()) {
                text = tr("✓ Sin avisos. %1 agujero(s) predictivo(s) irán a la guía.")
                           .arg(guidePredictiveHoles().size());
                style = QStringLiteral("color:#30d158;");
            } else {
                text = QStringLiteral("⚠ ") + check.warnings.join(QStringLiteral("\n⚠ "));
                style = QStringLiteral("color:#ff9f0a;");
            }
        }
        if (!motionText.isEmpty()) {
            text = motionText + QStringLiteral("\n") + text;
            if (!moved)
                style = QStringLiteral("color:#ff9f0a;");
        }
        m_guidePlateCheckLabel->setText(text);
        m_guidePlateCheckLabel->setStyleSheet(style + QStringLiteral(" font-size:11px;"));
    }
    updateGuideUi();
}

void MainWindow::exportGuidePlates()
{
    if (m_guidePlan.plates.empty()) {
        QMessageBox::warning(this, tr("Placas"), tr("Primero cree las placas."));
        return;
    }
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Carpeta para las placas"));
    if (folder.isEmpty())
        return;
    QString report;
    if (!exportGuidePlateFiles(folder, &report)) {
        QMessageBox::warning(this, tr("Placas"), report);
        return;
    }
    statusBar()->showMessage(report);
}

bool MainWindow::exportGuidePlateFiles(const QString& folder, QString* report)
{
    const auto fail = [report](const QString& message) {
        if (report)
            *report = message;
        return false;
    };
    if (m_guidePlan.plates.empty())
        return fail(tr("No hay placas que exportar."));
    if (!prepareGuidePlannedBone())
        return fail(tr("No se pudo medir el hueso planificado."));
    ensureGuidePlateMeshes();
    const QDir dir(folder);
    QStringList written;
    for (size_t i = 0; i < m_guidePlan.plates.size(); ++i) {
        const auto mesh = i < m_guidePlateMeshes.size() ? m_guidePlateMeshes[i] : nullptr;
        if (!mesh || mesh->GetNumberOfPolys() == 0)
            return fail(tr("La placa %1 no se pudo construir.").arg(i + 1));
        const QString side = m_guidePlan.plates[i].side == PlateSide::Left ? QStringLiteral("izquierda")
                             : m_guidePlan.plates[i].side == PlateSide::Bilateral ? QStringLiteral("bilateral")
                                                                                  : QStringLiteral("derecha");
        const QString name = QStringLiteral("placa_%1_%2.stl").arg(i + 1).arg(side);
        auto writer = vtkSmartPointer<vtkSTLWriter>::New();
        writer->SetFileName(dir.filePath(name).toUtf8().constData());
        writer->SetInputData(mesh);
        writer->SetFileTypeToBinary();
        if (writer->Write() != 1)
            return fail(tr("No se pudo escribir %1.").arg(dir.filePath(name)));
        written << name;
    }

    // The fabrication report: everything the lab and the surgeon need to check the plates against the guide.
    std::array<double, 16> motion{};
    guideSegmentMotion(motion);
    const PlateParams plate = guidePlateParams();
    const SleeveParams sleeve = guideSleeveParams();
    const auto holes = guidePredictiveHoles();
    const PlateCheck check = PlateCore::Check(m_guidePlan.plates, holes, plate);
    const auto xyz = [](const std::array<double, 3>& p) {
        return QStringLiteral("(%1, %2, %3)").arg(p[0], 0, 'f', 2).arg(p[1], 0, 'f', 2).arg(p[2], 0, 'f', 2);
    };
    QFile file(dir.filePath(QStringLiteral("informe_placas.txt")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
        return fail(tr("No se pudo escribir el informe."));
    QTextStream out(&file);
    out << "INFORME DE PLACAS A MEDIDA - LE FORT I\n";
    out << "Fecha: " << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")) << "\n\n";
    out << "Placas: espesor " << plate.thicknessMm << " mm, ancho " << plate.widthMm << " mm, agujero Ø"
        << plate.holeDiameterMm << " mm con avellanado Ø" << plate.countersinkDiameterMm << " x "
        << plate.countersinkDepthMm << " mm.\n";
    out << "Camisas de la guía: orificio Ø" << sleeve.boreDiameterMm << " mm, exterior Ø" << sleeve.outerDiameterMm
        << " mm, altura " << sleeve.heightMm << " mm.\n\n";
    out << "Movimiento planificado del segmento (preoperatorio -> planificado), matriz 4x4:\n";
    for (int r = 0; r < 4; ++r)
        out << "  " << motion[static_cast<size_t>(4 * r)] << "  " << motion[static_cast<size_t>(4 * r + 1)] << "  "
            << motion[static_cast<size_t>(4 * r + 2)] << "  " << motion[static_cast<size_t>(4 * r + 3)] << "\n";
    out << "\nAgujeros predictivos (mm, coordenadas del TAC):\n";
    for (const PredictiveHole& hole : holes) {
        out << "  Placa " << hole.plate + 1 << " (" << m_guidePlan.plates[static_cast<size_t>(hole.plate)].name
            << "), agujero " << hole.hole + 1 << " - "
            << (hole.bone == PlateBone::Segment ? "segmento Le Fort" : hole.bone == PlateBone::Cranial ? "cráneo"
                                                                                                        : "sin asignar")
            << "\n      planificado " << xyz(hole.plannedCenter) << "\n      en la guía " << xyz(hole.preopCenter)
            << "  eje " << xyz(hole.preopAxis) << "\n      a " << QString::number(hole.cutDistanceMm, 'f', 1)
            << " mm de la osteotomía\n";
    }
    out << "\nComprobaciones:\n";
    if (check.Ok())
        out << "  Sin avisos.\n";
    for (const QString& warning : check.warnings)
        out << "  - " << warning << "\n";
    out << "\nArchivos: " << written.join(QStringLiteral(", ")) << "\n";
    file.close();
    written << QStringLiteral("informe_placas.txt");
    if (report)
        *report = tr("Placas exportadas en %1: %2").arg(folder, written.join(QStringLiteral(", ")));
    return true;
}

namespace
{
QString holeLine(const LeFortProposedHole& hole)
{
    return LeFortHoleCore::PillarName(hole.pillar) + QStringLiteral(", ") + LeFortHoleCore::SideName(hole.side);
}
} // namespace

void MainWindow::generateLeFortGuide()
{
    if (m_guidePlan.type != GuideType::LeFort) {
        QMessageBox::warning(this, tr("Guía de corte"), tr("Seleccione la guía Le Fort I."));
        return;
    }
    if (m_guidePlannedView)
        setGuidePlateView(false); // the guide sits on the bone before the cut
    if (!m_guideWrapMesh || m_guideWrapMesh->GetNumberOfPolys() == 0 || !m_guidePrepared.ok) {
        computeGuideWrap();
        if (!m_guideWrapMesh || !m_guidePrepared.ok)
            return;
    }
    // The first molars anchor the maxillomalar holes: measure the roots now if step 1 was skipped.
    if (!m_guideRootAnalysis.ok && m_mesh3DView && m_mesh3DView->meshData(objectActorKey(kUpperTeethLabel)))
        analyzeGuideRoots();
    const OsteotomyPath path = guideLeFortPath();
    if (!path.valid) {
        QMessageBox::warning(this, tr("Guía de corte"),
                             path.error.isEmpty() ? tr("Falta la trayectoria de la osteotomía Le Fort.") : path.error);
        return;
    }
    LeFortHoleContext context;
    QString error;
    if (!guideLeFortHoleContext(context, &error)) {
        QMessageBox::warning(this, tr("Guía de corte"), error);
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    statusBar()->showMessage(tr("Guía de corte: buscando hueso con buen soporte para los orificios…"));
    // What the surgeon moved stays where it was put; the rest is proposed again on the current movement.
    std::vector<LeFortProposedHole> manual;
    for (const LeFortProposedHole& hole : m_guidePlan.lefortHoles)
        if (hole.origin == LeFortHoleOrigin::Manual)
            manual.push_back(hole);
    const LeFortProposal proposal = LeFortHoleCore::Propose(context, manual);
    QApplication::restoreOverrideCursor();
    m_guidePlan.lefortHoles = proposal.holes;
    m_guideLeFortMissing.clear();
    for (const LeFortMissingHoles& gap : proposal.missing)
        m_guideLeFortMissing << gap.reason;
    // Nothing is put in the guide yet: the band and the sites are shown on the bone, the sites draggable, and
    // a guide built from earlier sites no longer stands for them.
    m_guideMesh = nullptr;
    m_guideBuiltFigures.clear();
    refreshGuideBand();
    if (m_guideShowModelsCheck)
        m_guideShowModelsCheck->setChecked(true);
    if (m_guideShowWrapCheck)
        m_guideShowWrapCheck->setChecked(false);
    syncGuideView();
    setGuidePointMode(kModeMoveHoles);
    syncGuideBandSpins();
    const LeFortBandProfile band = guideLeFortBand();
    if (m_guideReportLabel)
        m_guideReportLabel->setText((band.noMotion ? QStringLiteral("⚠ ") : QString()) +
                                    (band.ok ? band.report : band.error) + QStringLiteral("\n") +
                                    guideLeFortHoleReport() + QStringLiteral("\n") +
                                    tr("Mueva los orificios que quiera y pulse «Aceptar orificios y crear guía»."));
    updateGuideUi();
}

void MainWindow::acceptLeFortHoles()
{
    if (m_guidePlan.type != GuideType::LeFort || (m_guidePlan.lefortHoles.empty() && m_guideLeFortMissing.isEmpty()))
        return;
    setGuidePointMode(kModeNone);
    layoutLeFortGuide(LeFortHoleCore::DrillSites(m_guidePlan.lefortHoles), guideLeFortBand(), guideLeFortHoleReport());
}

void MainWindow::layoutLeFortGuide(const std::vector<PredictiveHole>& drillSites, const LeFortBandProfile& band,
                                   const QString& holeReport)
{
    const OsteotomyPath path = guideLeFortPath();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    statusBar()->showMessage(tr("Guía de corte: trazando sobre la osteotomía preoperatoria…"));
    LeFortGuideParams params;
    params.sleeveOuterDiameterMm = guideSleeveParams().outerDiameterMm;
    // Two guides, right and left, engraved with the case number and their side (user's case, 2026-10-05).
    params.separateSides = true;
    params.extentFromHoles = true; // each guide ends a margin past its outermost hole (user's request, 2026-10-06)
    if (m_guideCaseEdit)
        m_guidePlan.caseLabel = m_guideCaseEdit->text().trimmed();
    params.caseLabel = m_guidePlan.caseLabel;
    params.labelWallMm = (m_guideClearanceSpin ? m_guideClearanceSpin->value() : m_guidePlan.design.base.clearanceMm) +
                         (m_guideThicknessSpin ? m_guideThicknessSpin->value() : m_guidePlan.design.base.thicknessMm);
    const LeFortGuideLayout layout = LeFortGuideCore::Layout(m_guidePrepared, m_guideWrapMesh, path, drillSites,
                                                             params, band.ok ? &band : nullptr);
    QApplication::restoreOverrideCursor();
    if (!layout.ok) {
        QMessageBox::warning(this, tr("Guía de corte"), layout.error);
        return;
    }
    // The plan is replaced by the laid-out guide; everything stays editable afterwards.
    m_guidePlan.contour.clear();
    m_guidePlan.paint = layout.paint;
    m_guidePlan.slotPlan = layout.slotPlan;
    m_guidePlan.holes.clear(); // every drill bore is a sleeve at a drill site
    // Regeneration is a fresh automatic cutting-guide layout. Saved projects
    // may still contain legacy sleeves, imported figures or connector tubes in
    // the old coordinate frame; keeping them is what made isolated pieces
    // reappear behind the skull after the support band had been corrected.
    m_guidePlan.figures = layout.figures; // the lattice cells; the sleeves are added at build time
    m_guideBuiltFigures.clear();
    refreshGuideFigureList();
    m_guidePendingEnds.clear();
    // The Le Fort cut is ticked so the slit pieces are carved.
    if (m_guideCutList)
        for (int row = 0; row < m_guideCutList->count(); ++row) {
            auto* item = m_guideCutList->item(row);
            const size_t index = static_cast<size_t>(item->data(Qt::UserRole).toInt());
            if (index < m_guideCuts.size() && m_guideCuts[index].path.points == path.points)
                item->setCheckState(Qt::Checked);
        }
    refreshGuideBand();
    repaintGuideWrap();
    rebuildGuideMarkers();
    buildGuideMesh();
    // Once generated, show the actual product against the bone. Leaving the
    // opaque envelope on top makes a correct thin guide look like scattered
    // fragments and hides its fit on the maxilla.
    if (m_guideMesh && m_guideMesh->GetNumberOfPolys() > 0) {
        if (m_guideShowModelsCheck)
            m_guideShowModelsCheck->setChecked(true);
        if (m_guideShowWrapCheck)
            m_guideShowWrapCheck->setChecked(false);
        if (m_guideShowGuideCheck)
            m_guideShowGuideCheck->setChecked(true);
        // The figures are the openwork cells subtracted from the guide: on real bone each preview cylinder
        // follows its own normal and they bristle over the guide (user's report, 2026-10-05). The guide
        // already carries them; the layer stays one click away.
        if (m_guideShowFiguresCheck)
            m_guideShowFiguresCheck->setChecked(false);
        applyGuideLayers();
    }
    // The movement first: it is what the guide was made from.
    QString movement = band.ok ? band.report : band.error;
    if (band.noMotion)
        movement = QStringLiteral("⚠ ") + movement;
    if (m_guideReportLabel && m_guideMesh)
        m_guideReportLabel->setText(movement + QStringLiteral("\n") + layout.report +
                                    (holeReport.isEmpty() ? QString() : QStringLiteral("\n") + holeReport) +
                                    QStringLiteral("\n") + m_guideReportLabel->text());
    showGuideLeFortHoles();
    updateGuideUi();
}

LeFortBandProfile MainWindow::guideLeFortBand() const
{
    std::array<double, 16> motion{};
    QString error;
    if (!guideSegmentMotion(motion, &error)) {
        LeFortBandProfile none;
        none.error = error;
        return none;
    }
    // The surgeon's own heights, when set, stand for the movement's (step 2 of the assistant).
    if (m_guidePlan.bandHeights.size() == 4)
        return LeFortMotionCore::BandFromHeights(guideLeFortPath(), m_guidePlan.bandHeights);
    return LeFortMotionCore::Band(guideLeFortPath(), motion);
}

bool MainWindow::guideLeFortHoleContext(LeFortHoleContext& context, QString* error) const
{
    if (!m_guidePlan.design.bone || !m_guidePrepared.ok || !m_guidePrepared.wrapField) {
        if (error)
            *error = tr("Calcule primero la envolvente del hueso antes del corte.");
        return false;
    }
    std::array<double, 16> motion{};
    if (!guideSegmentMotion(motion, error))
        return false;
    // The bone before the cut, where the guide drills, and the same bones where the plan puts them: the ring
    // of a segment hole is judged where the plate will hold it. No cut margin: Support measures the cut itself.
    const auto cranial = guideSourceMeshForLabel(kLeFortCranialLabel);
    const auto segmentBefore = guideSourceMeshForLabel(kLeFortSegLabel);
    const std::array<double, 16> still{1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0};
    context.bone = m_guidePlan.design.bone.get();
    context.plannedBone = PlateCore::MakeBoneQuery(repositionMeshForLabel(kLeFortCranialLabel),
                                                   repositionMeshForLabel(kLeFortSegLabel), motion, {}, 0.0);
    context.preopBone = PlateCore::MakeBoneQuery(cranial, segmentBefore, still, {}, 0.0);
    context.cut = guideLeFortPath();
    context.motion = motion;
    const LeFortBandProfile band = guideLeFortBand();
    if (band.ok)
        context.band = band;
    context.anterior = LeFortGuideCore::AnteriorDirection(*m_guidePrepared.wrapField, context.cut);
    context.params.seat = guidePlateParams();
    // The roots, when the segmentation separated the upper teeth: no screw is proposed or accepted into one
    // (user's case, 2026-10-05: holes proposed by the incisor roots).
    const vtkSmartPointer<vtkPolyData> teeth =
        m_mesh3DView ? m_mesh3DView->meshData(objectActorKey(kUpperTeethLabel)) : nullptr;
    if (!teeth || teeth->GetNumberOfPoints() == 0) {
        m_guideTeethField.reset();
        m_guideTeethFieldSource = nullptr;
    } else if (teeth.Get() != m_guideTeethFieldSource || teeth->GetMTime() != m_guideTeethFieldTime || !m_guideTeethField) {
        m_guideTeethField = ImplicitCore::BakeMeshField(teeth, 0.3, 4.0);
        m_guideTeethFieldSource = teeth;
        m_guideTeethFieldTime = teeth->GetMTime();
    }
    context.teeth = m_guideTeethField.get();
    // The maxillomalar buttress stands over the first molar: its place on the cut anchors that pillar's holes.
    if (m_guideRootAnalysis.ok) {
        for (const auto& [tooth, slot] : {std::pair{RootTooth::FirstMolarRight, 0}, std::pair{RootTooth::FirstMolarLeft, 3}}) {
            const int index = m_guideRootAnalysis.named[static_cast<size_t>(tooth)];
            if (index < 0)
                continue;
            context.anchors[static_cast<size_t>(slot)] = m_guideRootAnalysis.apices[static_cast<size_t>(index)].onCut;
            context.hasAnchor[static_cast<size_t>(slot)] = true;
        }
    }
    return true;
}

QString MainWindow::guideLeFortHoleReport() const
{
    QStringList lines;
    int warnings = 0, refused = 0, manual = 0;
    for (const LeFortProposedHole& hole : m_guidePlan.lefortHoles) {
        manual += hole.origin == LeFortHoleOrigin::Manual ? 1 : 0;
        if (hole.support.verdict == LeFortSupportVerdict::Warning) {
            ++warnings;
            lines << tr("⚠ %1: %2").arg(holeLine(hole), hole.support.reason);
        } else if (hole.support.verdict == LeFortSupportVerdict::Rejected) {
            ++refused;
            lines << tr("✖ %1: %2").arg(holeLine(hole), hole.support.reason);
        }
    }
    QString head = tr("Orificios: %1 (%2 movidos a mano)").arg(m_guidePlan.lefortHoles.size()).arg(manual);
    if (warnings > 0)
        head += tr(", %1 con aviso").arg(warnings);
    if (refused > 0)
        head += tr(", %1 sin soporte").arg(refused);
    head += QStringLiteral(".");
    lines.prepend(head);
    for (const QString& reason : m_guideLeFortMissing)
        lines << QStringLiteral("⚠ ") + reason;
    // The guide comes before the plates (user's decision, 2026-10-05): plates designed earlier are only checked.
    if (!m_guidePlan.plates.empty()) {
        const int unmatched = LeFortHoleCore::UnmatchedPlateHoles(m_guidePlan.lefortHoles, guidePredictiveHoles());
        if (unmatched > 0)
            lines << tr("⚠ %1 orificio(s) de las placas ya diseñadas no coinciden con los de la guía: "
                        "vuelva a diseñar las placas sobre estos orificios.").arg(unmatched);
    }
    return lines.join(QStringLiteral("\n"));
}

void MainWindow::refreshGuideBand()
{
    m_guideBandMesh = nullptr;
    // The heights shown are always the band drawn: the spins read 0.0 until «Proponer» before (user's case,
    // 2026-10-05), though the envelope had already given the band.
    syncGuideBandSpins();
    if (m_guidePlan.type != GuideType::LeFort)
        return;
    const LeFortBandProfile band = guideLeFortBand();
    if (!band.ok || band.spans.empty())
        return;
    // On the cranial bone before the cut: that is where the band is taken out.
    m_guideBandMesh = LeFortGuideCore::BandOnBone(guideSourceMeshForLabel(kLeFortCranialLabel), guideLeFortPath(), band);
}

void MainWindow::showGuideLeFortHoles()
{
    if (!m_guideView)
        return;
    for (int group = 0; group < 3; ++group) {
        m_guideHoleGroups[static_cast<size_t>(group)].clear();
        m_guideView->setEditablePoints(kHoleGroupBase + group, {}, CranioPalette::holeSound());
    }
    // Until they are accepted the proposed sites are always on show; once the guide carries them, only while
    // they are being moved.
    const bool guideBuilt = m_guideMesh && m_guideMesh->GetNumberOfPolys() > 0;
    if (m_guidePlan.type != GuideType::LeFort || m_guidePlan.lefortHoles.empty() ||
        (guideBuilt && m_guidePointMode != kModeMoveHoles && m_guidePointMode != kModeMarkHoles))
        return;
    // Saved projects keep where the holes are, not how sound they were: judge them again on this movement.
    LeFortHoleContext context;
    const bool judged = guideLeFortHoleContext(context);
    std::array<std::vector<std::array<double, 3>>, 3> centres;
    for (size_t i = 0; i < m_guidePlan.lefortHoles.size(); ++i) {
        LeFortProposedHole& hole = m_guidePlan.lefortHoles[i];
        if (judged)
            hole.support = LeFortHoleCore::Support(hole.center, hole.axis, context);
        const size_t group = hole.support.verdict == LeFortSupportVerdict::Ok        ? 0
                           : hole.support.verdict == LeFortSupportVerdict::Warning ? 1
                                                                                     : 2;
        m_guideHoleGroups[group].push_back(i);
        centres[group].push_back(hole.center);
    }
    m_guideView->setEditablePoints(kHoleGroupBase, centres[0], CranioPalette::holeSound(), 1.2);
    m_guideView->setEditablePoints(kHoleGroupBase + 1, centres[1], CranioPalette::holeWarning(), 1.2);
    m_guideView->setEditablePoints(kHoleGroupBase + 2, centres[2], CranioPalette::holeRefused(), 1.2);
    m_guideView->render();
}

void MainWindow::onGuideHoleDragged(int group, int, double x, double y, double z)
{
    if (group >= kHoleGroupBase && group < kHoleGroupBase + 3)
        m_guideHoleDragTarget = {x, y, z};
}

void MainWindow::onGuideHoleDropped(int group, int index)
{
    if (m_guidePointMode != kModeMoveHoles || group < kHoleGroupBase || group >= kHoleGroupBase + 3)
        return;
    const auto& members = m_guideHoleGroups[static_cast<size_t>(group - kHoleGroupBase)];
    if (index < 0 || static_cast<size_t>(index) >= members.size()) {
        showGuideLeFortHoles();
        return;
    }
    const size_t which = members[static_cast<size_t>(index)];
    LeFortHoleContext context;
    QString error;
    if (!guideLeFortHoleContext(context, &error)) {
        statusBar()->showMessage(error, 8000);
        showGuideLeFortHoles();
        return;
    }
    const LeFortProposedHole moved =
        LeFortHoleCore::MoveHole(m_guidePlan.lefortHoles[which], m_guideHoleDragTarget, context);
    if (moved.support.verdict == LeFortSupportVerdict::Rejected) {
        // A margin, the cut or the band: the hole stays where it was.
        statusBar()->showMessage(tr("Orificio no movido: %1").arg(moved.support.reason), 10000);
        showGuideLeFortHoles();
        return;
    }
    m_guidePlan.lefortHoles[which] = moved;
    statusBar()->showMessage(moved.support.verdict == LeFortSupportVerdict::Warning
                                 ? tr("Orificio movido con aviso: %1").arg(moved.support.reason)
                                 : tr("Orificio movido: %1 mm de hueso.").arg(moved.support.thicknessMm, 0, 'f', 1),
                             10000);
    // Before the sites are accepted only the markers move; after, the guide is built again with this hole in its
    // new place and the band and the other holes as they were.
    if (m_guideMesh && m_guideMesh->GetNumberOfPolys() > 0)
        layoutLeFortGuide(LeFortHoleCore::DrillSites(m_guidePlan.lefortHoles), guideLeFortBand(),
                          guideLeFortHoleReport());
    else
        showGuideLeFortHoles();
}

QString MainWindow::guideMotionSummary(bool* moved) const
{
    if (moved)
        *moved = false;
    std::array<double, 16> motion{};
    QString error;
    const auto segment = repositionMeshForLabel(kLeFortSegLabel);
    if (!segment || segment->GetNumberOfPolys() == 0)
        return {};
    if (!guideSegmentMotion(motion, &error))
        return tr("⚠ Movimiento del Le Fort: %1").arg(error);
    // Measured at the middle of the segment before the cut, in the patient's frame: forward is from the segment
    // towards the anterior cut points (the piriform ones), level; up is +Z.
    const auto before = guideSourceMeshForLabel(kLeFortSegLabel);
    double b[6] = {};
    (before ? before : segment)->GetBounds(b);
    const std::array<double, 3> middle{0.5 * (b[0] + b[1]), 0.5 * (b[2] + b[3]), 0.5 * (b[4] + b[5])};
    const std::array<double, 3> after = PlateCore::TransformPoint(motion, middle);
    const std::array<double, 3> shift{after[0] - middle[0], after[1] - middle[1], after[2] - middle[2]};
    std::array<double, 3> forward{0.0, -1.0, 0.0}; // DICOM LPS: anterior is -Y
    const OsteotomyPath path = guideLeFortPath();
    if (path.valid && path.points.size() == 4) {
        const auto& p = path.points;
        const double fx = 0.5 * (p[1][0] + p[2][0]) - middle[0], fy = 0.5 * (p[1][1] + p[2][1]) - middle[1];
        const double length = std::hypot(fx, fy);
        if (length > 1e-6)
            forward = {fx / length, fy / length, 0.0};
    }
    const double advance = shift[0] * forward[0] + shift[1] * forward[1];
    const double vertical = shift[2];
    const double lateral = shift[0] * forward[1] - shift[1] * forward[0];
    const double trace = motion[0] + motion[5] + motion[10];
    const double rotation = std::acos(std::clamp(0.5 * (trace - 1.0), -1.0, 1.0)) * 180.0 / 3.14159265358979323846;
    const bool anyMotion = std::hypot(std::hypot(shift[0], shift[1]), shift[2]) > 0.2 || rotation > 0.3;
    if (moved)
        *moved = anyMotion;
    if (!anyMotion)
        return tr("⚠ El Le Fort no tiene movimiento planificado: realice el avance en REPOSICIÓN y vuelva a este "
                  "paso.");
    return tr("Movimiento del Le Fort: %1 %2 mm, %3 %4 mm, lateral %5 mm, giro %6°.")
        .arg(advance >= 0.0 ? tr("avance") : tr("retroceso"))
        .arg(std::abs(advance), 0, 'f', 1)
        .arg(vertical >= 0.0 ? tr("ascenso") : tr("descenso"))
        .arg(std::abs(vertical), 0, 'f', 1)
        .arg(std::abs(lateral), 0, 'f', 1)
        .arg(rotation, 0, 'f', 1);
}

void MainWindow::analyzeGuideRoots()
{
    if (m_guidePlan.type != GuideType::LeFort)
        return;
    // The upper teeth before the cut: the object the segmentation gives them (labels are not moved by REPOSICIÓN).
    vtkSmartPointer<vtkPolyData> teeth = m_mesh3DView ? m_mesh3DView->meshData(objectActorKey(kUpperTeethLabel)) : nullptr;
    m_guideRootAnalysis = RootAnalysisCore::Analyze(teeth, guideLeFortPath());
    if (m_guideRootsLabel)
        m_guideRootsLabel->setText(m_guideRootAnalysis.ok ? m_guideRootAnalysis.report
                                                          : QStringLiteral("⚠ ") + m_guideRootAnalysis.error);
    if (m_guideView) {
        // What the surgeon measures: from the apex of each canine and first molar to the osteotomy, and nothing
        // else (the whole tooth drawn too hid the measure, user's case 2026-10-05); the apices too close in red.
        auto points = vtkSmartPointer<vtkPoints>::New();
        auto lines = vtkSmartPointer<vtkCellArray>::New();
        const auto segment = [&](const std::array<double, 3>& a, const std::array<double, 3>& b) {
            const vtkIdType ids[2] = {points->InsertNextPoint(a.data()), points->InsertNextPoint(b.data())};
            lines->InsertNextCell(2, ids);
        };
        if (m_guideRootAnalysis.ok)
            for (const int index : m_guideRootAnalysis.named) {
                if (index < 0)
                    continue;
                const RootApex& apex = m_guideRootAnalysis.apices[static_cast<size_t>(index)];
                segment(apex.apex, apex.onCut);
            }
        auto overlay = vtkSmartPointer<vtkPolyData>::New();
        overlay->SetPoints(points);
        overlay->SetLines(lines);
        if (lines->GetNumberOfCells() > 0)
            m_guideView->setOverlayPolyline(kGuideRootsOverlayKey, overlay, QColor(CranioPalette::Accent), 3.5);
        else
            m_guideView->removeOverlay(kGuideRootsOverlayKey);
        // The value beside each measurement, as the surgeon writes it on the reference image.
        std::vector<std::pair<std::array<double, 3>, QString>> labels;
        if (m_guideRootAnalysis.ok) {
            static const char* kShort[] = {"1.er molar D", "Canino D", "Canino I", "1.er molar I"};
            for (size_t t = 0; t < m_guideRootAnalysis.named.size(); ++t) {
                const int index = m_guideRootAnalysis.named[t];
                if (index < 0)
                    continue;
                const RootApex& apex = m_guideRootAnalysis.apices[static_cast<size_t>(index)];
                const std::array<double, 3> at{0.5 * (apex.apex[0] + apex.onCut[0]), 0.5 * (apex.apex[1] + apex.onCut[1]),
                                               0.5 * (apex.apex[2] + apex.onCut[2])};
                labels.push_back({at, apex.cutDistanceMm >= 0.0
                                          ? tr("%1: %2 mm").arg(QString::fromUtf8(kShort[t])).arg(apex.cutDistanceMm, 0, 'f', 1)
                                          : tr("%1: el corte cruza la raíz (%2 mm)")
                                                .arg(QString::fromUtf8(kShort[t]))
                                                .arg(-apex.cutDistanceMm, 0, 'f', 1)});
            }
        }
        m_guideView->setOverlayLabels(kGuideRootsOverlayKey, labels, QColor(CranioPalette::Accent));
        m_guideView->clearPointMarkers();
        if (m_guideRootAnalysis.ok)
            for (const int index : m_guideRootAnalysis.named) {
                if (index < 0)
                    continue;
                const RootApex& apex = m_guideRootAnalysis.apices[static_cast<size_t>(index)];
                m_guideView->addPointMarker(apex.apex[0], apex.apex[1], apex.apex[2],
                                            apex.tooClose ? CranioPalette::resection() : CranioPalette::holeSound());
            }
        m_guideView->render();
    }
    updateGuideUi();
}

void MainWindow::syncGuideBandSpins()
{
    const LeFortBandProfile band = guideLeFortBand();
    for (size_t i = 0; i < m_guideBandSpins.size(); ++i) {
        if (!m_guideBandSpins[i])
            continue;
        QSignalBlocker blocker(m_guideBandSpins[i]);
        m_guideBandSpins[i]->setValue(band.ok && i < band.heights.size() ? band.heights[i] : 0.0);
    }
}

void MainWindow::applyGuideBandHeights()
{
    std::vector<double> heights;
    for (QDoubleSpinBox* spin : m_guideBandSpins)
        heights.push_back(spin ? spin->value() : 0.0);
    // Only an actual change becomes the surgeon's band; the movement's own heights stay the movement's.
    const LeFortBandProfile current = guideLeFortBand();
    bool changed = !current.ok || current.heights.size() != heights.size();
    for (size_t i = 0; !changed && i < heights.size(); ++i)
        changed = std::abs(current.heights[i] - heights[i]) > 0.05;
    if (!changed)
        return;
    m_guidePlan.bandHeights = heights;
    refreshGuideBand();
    syncGuideView();
    if (m_guideMesh && m_guideMesh->GetNumberOfPolys() > 0 && !m_guidePlan.lefortHoles.empty())
        acceptLeFortHoles(); // the guides again, with the new band and slit
    else if (m_guideReportLabel)
        m_guideReportLabel->setText(guideLeFortBand().report);
    statusBar()->showMessage(tr("Franja ajustada por el cirujano."), 6000);
}

void MainWindow::resetGuideBandHeights()
{
    m_guidePlan.bandHeights.clear();
    syncGuideBandSpins();
    refreshGuideBand();
    syncGuideView();
    if (m_guideMesh && m_guideMesh->GetNumberOfPolys() > 0 && !m_guidePlan.lefortHoles.empty())
        acceptLeFortHoles();
    statusBar()->showMessage(tr("Franja restablecida desde el movimiento."), 6000);
}

bool MainWindow::exportLeFortGuideFiles(const QString& folder, QString* report)
{
    if (!m_guideMesh || m_guideMesh->GetNumberOfPolys() == 0) {
        if (report)
            *report = tr("Primero cree las guías.");
        return false;
    }
    const LeFortGuidePair pair = LeFortGuideCore::SplitBySide(m_guideMesh, guideLeFortPath());
    QStringList written;
    for (const auto& [mesh, name] : {std::pair{pair.right, QStringLiteral("guia_der.stl")},
                                     std::pair{pair.left, QStringLiteral("guia_izq.stl")}}) {
        if (!mesh || mesh->GetNumberOfPolys() == 0)
            continue;
        vtkSmartPointer<vtkPolyData> output = mesh;
        if (!MeshRepairCore::Analyze(mesh).Valid()) {
            const MeshRepairResult repaired = MeshRepairCore::Repair(mesh);
            if (repaired.ok)
                output = repaired.mesh;
        }
        auto writer = vtkSmartPointer<vtkSTLWriter>::New();
        const QString path = QDir(folder).filePath(name);
        writer->SetFileName(path.toUtf8().constData());
        writer->SetInputData(output);
        writer->SetFileTypeToBinary();
        if (writer->Write() != 1) {
            if (report)
                *report = tr("No se pudo escribir %1.").arg(path);
            return false;
        }
        written << name;
    }
    if (written.size() != 2) {
        if (report)
            *report = tr("Se esperaban dos guías (DER e IZQ) y salieron %1.").arg(written.size());
        return false;
    }
    if (report)
        *report = tr("Guías exportadas en %1: %2").arg(folder, written.join(QStringLiteral(", ")));
    return true;
}
