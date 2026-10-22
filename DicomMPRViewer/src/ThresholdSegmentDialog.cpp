#include "ThresholdSegmentDialog.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

namespace
{
struct Preset
{
    const char* name;
    double minHu;
    double maxHu;
    bool fillHoles;
    bool keepLargest;
};

const Preset kPresets[] = {
    {"Personalizado", 0, 0, false, false},
    {"Hueso (TC)", 150, 3071, false, true},
    {"Tejido blando (TC)", -205, 235, true, true},
};

int clampHu(double value, double minimum, double maximum)
{
    return static_cast<int>(std::lround(std::clamp(value, minimum, maximum)));
}
}

class HuRangeWidget : public QWidget
{
public:
    explicit HuRangeWidget(const HuHistogram& histogram, QWidget* parent = nullptr)
        : QWidget(parent)
        , m_histogram(histogram)
    {
        setMinimumHeight(120);
        setMinimumWidth(320);
        setMouseTracking(true);
        setCursor(Qt::SplitHCursor);
    }

    void setRange(double minHu, double maxHu)
    {
        m_min = minHu;
        m_max = maxHu;
        update();
    }

    std::function<void(double, double)> onChanged;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);
        const QRect plot = rect().adjusted(8, 8, -8, -8);
        painter.fillRect(rect(), QColor(28, 28, 30));
        painter.fillRect(plot, QColor(18, 18, 20));
        if (m_histogram.counts.empty() || plot.width() < 2) return;

        const auto peak = std::max<std::int64_t>(1, *std::max_element(m_histogram.counts.begin(),
                                                                       m_histogram.counts.end()));
        const int bins = static_cast<int>(m_histogram.counts.size());
        for (int i = 0; i < bins; ++i) {
            const double hu = m_histogram.minimum + (i + 0.5) * m_histogram.binWidth;
            const int x0 = plot.left() + i * plot.width() / bins;
            const int x1 = plot.left() + (i + 1) * plot.width() / bins;
            const int height = static_cast<int>(plot.height() * m_histogram.counts[static_cast<size_t>(i)] / peak);
            const bool inside = hu >= m_min && hu <= m_max;
            painter.fillRect(QRect(x0, plot.bottom() - height, std::max(1, x1 - x0), height),
                             inside ? QColor(48, 209, 88) : QColor(120, 120, 128));
        }
        painter.setPen(QColor(48, 209, 88));
        painter.drawLine(QPoint(huToX(m_min), plot.top()), QPoint(huToX(m_min), plot.bottom()));
        painter.drawLine(QPoint(huToX(m_max), plot.top()), QPoint(huToX(m_max), plot.bottom()));
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        const int x = static_cast<int>(event->position().x());
        const int minX = huToX(m_min);
        const int maxX = huToX(m_max);
        if (std::abs(x - minX) <= 8) m_drag = Drag::Min;
        else if (std::abs(x - maxX) <= 8) m_drag = Drag::Max;
        else if (x > minX && x < maxX) {
            m_drag = Drag::Window;
            m_grab = xToHu(x) - m_min;
        } else {
            m_drag = x < minX ? Drag::Min : Drag::Max;
        }
        mouseMoveEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (m_drag == Drag::None || !(event->buttons() & Qt::LeftButton)) return;
        const double hu = xToHu(static_cast<int>(event->position().x()));
        double minHu = m_min;
        double maxHu = m_max;
        if (m_drag == Drag::Min) minHu = std::min(hu, maxHu);
        else if (m_drag == Drag::Max) maxHu = std::max(hu, minHu);
        else {
            const double width = maxHu - minHu;
            minHu = std::clamp(hu - m_grab, m_histogram.minimum, m_histogram.maximum - width);
            maxHu = minHu + width;
        }
        minHu = std::clamp(minHu, m_histogram.minimum, m_histogram.maximum);
        maxHu = std::clamp(maxHu, m_histogram.minimum, m_histogram.maximum);
        if (onChanged) onChanged(minHu, maxHu);
    }

    void mouseReleaseEvent(QMouseEvent*) override { m_drag = Drag::None; }

private:
    int huToX(double hu) const
    {
        const QRect plot = rect().adjusted(8, 8, -8, -8);
        const double span = std::max(1e-6, m_histogram.maximum - m_histogram.minimum);
        const double t = (hu - m_histogram.minimum) / span;
        return plot.left() + static_cast<int>(std::lround(t * plot.width()));
    }

    double xToHu(int x) const
    {
        const QRect plot = rect().adjusted(8, 8, -8, -8);
        const double span = std::max(1e-6, m_histogram.maximum - m_histogram.minimum);
        const double t = std::clamp((x - plot.left()) / static_cast<double>(std::max(1, plot.width())), 0.0, 1.0);
        return m_histogram.minimum + t * span;
    }

    enum class Drag { None, Min, Max, Window };
    HuHistogram m_histogram;
    double m_min = 0;
    double m_max = 0;
    Drag m_drag = Drag::None;
    double m_grab = 0;
};

ThresholdSegmentDialog::ThresholdSegmentDialog(bool bone, const HuHistogram& histogram, QWidget* parent)
    : QDialog(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint)
    , m_bone(bone)
    , m_histogram(histogram)
{
    setWindowTitle(tr("Segmentar"));
    setModal(false);
    setMinimumWidth(380);
    if (parent) setStyleSheet(parent->styleSheet());

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(8);

    auto* choiceRow = new QHBoxLayout();
    m_boneChoice = new QRadioButton(tr("Hueso"), this);
    m_boneChoice->setObjectName(QStringLiteral("thresholdBoneChoice"));
    m_softChoice = new QRadioButton(tr("Tejido blando"), this);
    m_softChoice->setObjectName(QStringLiteral("thresholdSoftChoice"));
    choiceRow->addWidget(m_boneChoice);
    choiceRow->addWidget(m_softChoice);
    choiceRow->addStretch(1);
    layout->addLayout(choiceRow);

    m_hint = new QLabel(this);
    m_hint->setWordWrap(true);
    layout->addWidget(m_hint);

    m_histogramView = new HuRangeWidget(histogram, this);
    layout->addWidget(m_histogramView);

    const int dataMin = histogram.voxels > 0 ? static_cast<int>(std::floor(histogram.minimum)) : -1000;
    const int dataMax = histogram.voxels > 0 ? static_cast<int>(std::ceil(histogram.maximum)) : 3000;

    auto* rangeRow = new QHBoxLayout();
    rangeRow->addWidget(new QLabel(tr("Mínimo"), this));
    m_minBox = new QSpinBox(this);
    m_minBox->setObjectName(QStringLiteral("thresholdMinHu"));
    m_minBox->setRange(dataMin, dataMax);
    m_minBox->setSuffix(tr(" HU"));
    rangeRow->addWidget(m_minBox);
    rangeRow->addWidget(new QLabel(tr("Máximo"), this));
    m_maxBox = new QSpinBox(this);
    m_maxBox->setObjectName(QStringLiteral("thresholdMaxHu"));
    m_maxBox->setRange(dataMin, dataMax);
    m_maxBox->setSuffix(tr(" HU"));
    rangeRow->addWidget(m_maxBox);
    layout->addLayout(rangeRow);

    m_preset = new QComboBox(this);
    m_preset->setObjectName(QStringLiteral("thresholdPreset"));
    for (const auto& preset : kPresets)
        m_preset->addItem(tr(preset.name));
    layout->addWidget(m_preset);

    m_fillHoles = new QCheckBox(tr("Rellenar huecos"), this);
    m_fillHoles->setObjectName(QStringLiteral("thresholdFillHoles"));
    m_keepLargest = new QCheckBox(tr("Conservar el mayor"), this);
    m_keepLargest->setObjectName(QStringLiteral("thresholdKeepLargest"));
    auto* options = new QHBoxLayout();
    options->addWidget(m_fillHoles);
    options->addWidget(m_keepLargest);
    layout->addLayout(options);

    auto* note = new QLabel(tr("Rellenar huecos y conservar el mayor se aplican al crear el objeto. "
                               "Conservar el mayor quita las piezas sueltas del borde del estudio."), this);
    note->setWordWrap(true);
    layout->addWidget(note);

    auto* buttons = new QHBoxLayout();
    auto* cancel = new QPushButton(tr("Cancelar"), this);
    m_create = new QPushButton(this);
    m_create->setObjectName(QStringLiteral("thresholdCreate"));
    m_create->setDefault(true);
    buttons->addStretch(1);
    buttons->addWidget(cancel);
    buttons->addWidget(m_create);
    layout->addLayout(buttons);

    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    timer->setInterval(40);
    connect(timer, &QTimer::timeout, this, &ThresholdSegmentDialog::emitRange);

    m_histogramView->onChanged = [this, timer](double minHu, double maxHu) {
        setRange(minHu, maxHu, true);
        timer->start();
    };
    connect(m_minBox, qOverload<int>(&QSpinBox::valueChanged), this, [this, timer](int) {
        if (m_updating) return;
        setRange(m_minBox->value(), m_maxBox->value(), true);
        timer->start();
    });
    connect(m_maxBox, qOverload<int>(&QSpinBox::valueChanged), this, [this, timer](int) {
        if (m_updating) return;
        setRange(m_minBox->value(), m_maxBox->value(), true);
        timer->start();
    });
    connect(m_preset, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, timer](int index) {
        if (m_updating || index <= 0) return;
        if (index == 1 || index == 2)
            setBoneMode(index == 1);
        timer->start();
    });
    connect(m_boneChoice, &QRadioButton::toggled, this, [this, timer](bool on) {
        if (!on || m_updating) return;
        setBoneMode(true);
        timer->start();
    });
    connect(m_softChoice, &QRadioButton::toggled, this, [this, timer](bool on) {
        if (!on || m_updating) return;
        setBoneMode(false);
        timer->start();
    });
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_create, &QPushButton::clicked, this, [this, timer] {
        timer->stop();
        emitRange();
        accept();
    });

    setBoneMode(bone);
}

void ThresholdSegmentDialog::setBoneMode(bool bone)
{
    m_bone = bone;
    m_updating = true;
    m_boneChoice->setChecked(bone);
    m_softChoice->setChecked(!bone);
    m_updating = false;
    m_hint->setText(bone
        ? tr("Elija el rango en unidades Hounsfield. Los cortes muestran lo que entra. "
             "Al crear, el hueso se divide en maxilar y mandíbula.")
        : tr("Elija el rango en unidades Hounsfield. Los cortes muestran lo que entra. "
             "Al crear, se genera el tejido blando."));
    m_create->setText(bone ? tr("Crear y separar") : tr("Crear tejido"));
    applyPreset(bone ? 1 : 2);
}

ThresholdRequest ThresholdSegmentDialog::request() const
{
    ThresholdRequest request;
    request.minHu = m_minBox->value();
    request.maxHu = m_maxBox->value();
    request.fillHoles = m_fillHoles->isChecked();
    request.keepLargest = m_keepLargest->isChecked();
    request.label = m_bone ? 1 : 2;
    return request;
}

void ThresholdSegmentDialog::applyPreset(int index)
{
    if (index <= 0 || index >= static_cast<int>(std::size(kPresets))) return;
    const auto& preset = kPresets[index];
    const double minimum = m_histogram.voxels > 0 ? m_histogram.minimum : preset.minHu;
    const double maximum = m_histogram.voxels > 0 ? m_histogram.maximum : preset.maxHu;
    setRange(clampHu(preset.minHu, minimum, maximum), clampHu(preset.maxHu, minimum, maximum), false);
    m_fillHoles->setChecked(preset.fillHoles);
    m_keepLargest->setChecked(preset.keepLargest);
    QSignalBlocker blocker(m_preset);
    m_preset->setCurrentIndex(index);
}

void ThresholdSegmentDialog::setRange(double minHu, double maxHu, bool fromUser)
{
    if (minHu > maxHu) std::swap(minHu, maxHu);
    m_updating = true;
    m_minBox->setValue(static_cast<int>(std::lround(minHu)));
    m_maxBox->setValue(static_cast<int>(std::lround(maxHu)));
    m_histogramView->setRange(m_minBox->value(), m_maxBox->value());
    if (fromUser) {
        QSignalBlocker blocker(m_preset);
        m_preset->setCurrentIndex(0);
    }
    m_updating = false;
}

void ThresholdSegmentDialog::emitRange()
{
    emit rangeEdited(m_minBox->value(), m_maxBox->value());
}
