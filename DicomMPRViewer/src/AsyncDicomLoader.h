#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include <vtkSmartPointer.h>

#include "DicomSeriesIndexer.h"
#include "DicomVolumeLoader.h"

class vtkImageData;

// ─────────────────────────────────────────────────────────────────────────────
// Qt meta-type registrations for cross-thread signals.
// These must appear before the first use and in a header so all TUs see them.
// ─────────────────────────────────────────────────────────────────────────────
Q_DECLARE_METATYPE(vtkSmartPointer<vtkImageData>)
Q_DECLARE_METATYPE(DicomVolumeLoader::VolumeMetadata)

// ─────────────────────────────────────────────────────────────────────────────
// AsyncDicomLoader
//
// Worker QObject that must be moved to a dedicated QThread:
//
//   m_thread = new QThread(this);
//   m_loader = new AsyncDicomLoader();          // no parent — moves to thread
//   m_loader->moveToThread(m_thread);
//   connect(m_thread, &QThread::finished, m_loader, &QObject::deleteLater);
//   m_thread->start();
//
// Call startLoad() from the main thread via QMetaObject::invokeMethod or
// through a connected signal.  Results come back as Qt signals (queued).
//
// Multi-series handling:
//   • If a folder contains exactly one image series, startLoad() runs the
//     full pipeline automatically.
//   • If multiple series are found, startLoad() emits seriesFound() and
//     returns.  The main thread shows a picker and calls loadSeries() with
//     the user's choice.
// ─────────────────────────────────────────────────────────────────────────────
class AsyncDicomLoader : public QObject
{
    Q_OBJECT

public:
    explicit AsyncDicomLoader(QObject* parent = nullptr);

public slots:
    // ── Entry points (called from main thread via queued connection) ────────

    // Full pipeline: index folder → check cache → load (with preview).
    void startLoad(const QString& folderPath, const QString& cacheDir);

    // Load a specific already-indexed series (used when the user picks one
    // from SeriesSelectionDialog after a multi-series seriesFound() signal).
    void loadSeries(const SeriesInfo& series, const QString& cacheDir);

signals:
    // Informational — update status bar / progress bar in the UI.
    void statusChanged(const QString& message);
    void progressChanged(int percent);        // 0–100

    // Emitted after indexing when multiple series are found.
    // If there is exactly one series, this signal is NOT emitted
    // (the pipeline continues automatically).
    void seriesFound(const QVector<SeriesInfo>& series);

    // Downsampled preview (every 4th slice) — available quickly so the
    // user sees something while the full volume loads.
    void previewReady(vtkSmartPointer<vtkImageData> preview,
                      DicomVolumeLoader::VolumeMetadata meta);

    // Full-resolution volume — replaces the preview.
    void volumeReady(vtkSmartPointer<vtkImageData> volume,
                     DicomVolumeLoader::VolumeMetadata meta);

    void errorOccurred(const QString& message);
};
