#pragma once

#include <QDialog>
#include <QVector>

#include "DicomSeriesIndexer.h"

QT_BEGIN_NAMESPACE
class QTableWidget;
class QLabel;
QT_END_NAMESPACE

// ─────────────────────────────────────────────────────────────────────────────
// SeriesSelectionDialog
//
// Shown when a folder contains more than one DICOM series.
// Displays Modality, Series Description, Slice Count, and Series UID.
// The user selects a row and clicks "Load".
// ─────────────────────────────────────────────────────────────────────────────
class SeriesSelectionDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SeriesSelectionDialog(const QVector<SeriesInfo>& series,
                                   QWidget* parent = nullptr);

    // Returns the series the user accepted.
    SeriesInfo selectedSeries() const;

private slots:
    void onAccept();

private:
    QVector<SeriesInfo> m_series;
    QTableWidget*       m_table = nullptr;
};
