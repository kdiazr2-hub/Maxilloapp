#include "SegmentationProgressDialog.h"
#include "SegmentationProgressCore.h"
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

SegmentationProgressDialog::SegmentationProgressDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("Segmentación automática"));
    setWindowModality(Qt::WindowModal);
    setMinimumWidth(420);
    setMaximumWidth(600);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(14);
    m_stage = new QLabel(tr("Preparando segmentación..."), this);
    m_stage->setWordWrap(true);
    m_stage->setTextFormat(Qt::PlainText);
    m_bar = new QProgressBar(this);
    m_bar->setObjectName(QStringLiteral("segmentationProgress"));
    m_elapsed = new QLabel(this);
    m_cancel = new QPushButton(tr("Cancelar"), this);
    layout->addWidget(m_stage);
    layout->addWidget(m_bar);
    layout->addWidget(m_elapsed);
    layout->addWidget(m_cancel, 0, Qt::AlignRight);
    m_clock.start();
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] {
        m_elapsed->setText(tr("Tiempo transcurrido: %1").arg(SegmentationProgressCore::elapsedText(m_clock.elapsed())));
    });
    timer->start(250);
    m_elapsed->setText(tr("Tiempo transcurrido: 00:00:00"));
    connect(m_cancel, &QPushButton::clicked, this, &SegmentationProgressDialog::reject);
    setProgress(-1);
}

void SegmentationProgressDialog::setStage(const QString& text) { if (!m_cancelling) m_stage->setText(text.left(240)); }
void SegmentationProgressDialog::setProgress(int percent)
{
    m_bar->setRange(0, percent < 0 ? 0 : 100);
    if (percent >= 0) m_bar->setValue(percent);
}
void SegmentationProgressDialog::setCancelling()
{
    m_cancelling = true;
    m_cancel->setEnabled(false);
    m_stage->setText(tr("Cancelando. Esperando a que finalice la operación en curso..."));
    setProgress(-1);
}
void SegmentationProgressDialog::reject()
{
    if (m_cancelling) return;
    setCancelling();
    emit cancelRequested();
}
