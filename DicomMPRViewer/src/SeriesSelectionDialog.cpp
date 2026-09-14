#include "SeriesSelectionDialog.h"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

// ─────────────────────────────────────────────────────────────────────────────
SeriesSelectionDialog::SeriesSelectionDialog(const QVector<SeriesInfo>& series,
                                             QWidget* parent)
    : QDialog(parent)
    , m_series(series)
{
    setWindowTitle(tr("Select DICOM Series"));
    setMinimumSize(700, 320);
    setStyleSheet(
        "QDialog { background:#1c1c1e; color:#f5f5f7; font-family:'Segoe UI Variable','Segoe UI'; }"
        "QLabel  { color:#d1d1d6; font-size:11px; }"
        "QTableWidget { background:#1f1f21; color:#f5f5f7; gridline-color:#2c2c2e;"
        "               font-size:11px; border:1px solid #2c2c2e; border-radius:12px; }"
        "QHeaderView::section { background:#2c2c2e; color:#f5f5f7; padding:6px; border:0;"
        "                      border-bottom:1px solid #3a3a3c; font-weight:bold; }"
        "QTableWidget::item:selected { background:#1f3b57; color:#ffffff; }"
        "QPushButton { background:#3a3a3c; color:#f5f5f7; padding:7px 20px;"
        "              border-radius:12px; font-weight:600; border:1px solid #4a4a4c; }"
        "QPushButton:hover { background:#48484a; }"
        "QPushButton:default { background:#0a84ff; border-color:#0a84ff; color:#ffffff; }"
        "QPushButton:default:hover { background:#1d9bf0; }");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto* hint = new QLabel(
        tr("Multiple series found. Select the series to load:"), this);
    layout->addWidget(hint);

    // ── Table ─────────────────────────────────────────────────────────────
    m_table = new QTableWidget(series.size(), 5, this);
    m_table->setHorizontalHeaderLabels(
        {tr("Modality"), tr("Description"), tr("Slices"),
         tr("Patient"), tr("Series UID (first 20)")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setStretchLastSection(true);

    for (int row = 0; row < series.size(); ++row) {
        const SeriesInfo& s = series[row];
        auto cell = [&](int col, const QString& text) {
            m_table->setItem(row, col, new QTableWidgetItem(text));
        };
        cell(0, s.modality.isEmpty()          ? tr("?") : s.modality);
        cell(1, s.seriesDescription.isEmpty() ? tr("(no description)") : s.seriesDescription);
        cell(2, QString::number(s.fileCount));
        cell(3, s.patientName.isEmpty() ? s.patientId : s.patientName);
        cell(4, s.seriesInstanceUid.left(20));
    }

    // Pre-select first row (largest series)
    m_table->selectRow(0);
    connect(m_table, &QTableWidget::cellDoubleClicked, this, &SeriesSelectionDialog::onAccept);
    layout->addWidget(m_table, 1);

    // ── Buttons ───────────────────────────────────────────────────────────
    auto* buttons = new QDialogButtonBox(this);
    auto* loadBtn   = buttons->addButton(tr("Load"), QDialogButtonBox::AcceptRole);
    loadBtn->setDefault(true);
    auto* cancelBtn = buttons->addButton(QDialogButtonBox::Cancel);
    Q_UNUSED(cancelBtn)
    connect(loadBtn,   &QPushButton::clicked, this, &SeriesSelectionDialog::onAccept);
    connect(buttons,   &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

// ─────────────────────────────────────────────────────────────────────────────
SeriesInfo SeriesSelectionDialog::selectedSeries() const
{
    const int row = m_table->currentRow();
    if (row >= 0 && row < m_series.size())
        return m_series[row];
    return m_series.isEmpty() ? SeriesInfo{} : m_series.first();
}

// ─────────────────────────────────────────────────────────────────────────────
void SeriesSelectionDialog::onAccept()
{
    accept();
}
