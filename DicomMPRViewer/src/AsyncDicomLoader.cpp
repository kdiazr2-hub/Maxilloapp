#include "AsyncDicomLoader.h"

#include <vector>
#include <string>

#include <QStandardPaths>

#include <vtkImageData.h>
#include <vtkSmartPointer.h>

#include "DicomVolumeLoader.h"
#include "VolumeCacheManager.h"

// ─────────────────────────────────────────────────────────────────────────────
AsyncDicomLoader::AsyncDicomLoader(QObject* parent)
    : QObject(parent)
{}

// ─────────────────────────────────────────────────────────────────────────────
void AsyncDicomLoader::startLoad(const QString& folderPath,
                                  const QString& cacheDir)
{
    emit statusChanged(QStringLiteral("Indexing DICOM series…"));
    emit progressChanged(0);

    // ── Index folder (header-only, fast) ─────────────────────────────────
    DicomSeriesIndexer indexer;
    const QVector<SeriesInfo> series = indexer.indexFolder(folderPath);

    if (series.isEmpty()) {
        emit errorOccurred(QStringLiteral("No DICOM series found in: ") + folderPath);
        return;
    }

    emit progressChanged(5);

    if (series.size() == 1) {
        // Single series: proceed automatically
        loadSeries(series.first(), cacheDir);
    } else {
        // Multiple series: hand control to the main thread via seriesFound().
        // The main thread will call loadSeries() with the user's choice.
        emit seriesFound(series);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void AsyncDicomLoader::loadSeries(const SeriesInfo& series,
                                   const QString& cacheDir)
{
    const VolumeCacheManager cache(cacheDir);

    // ── 1. Cache check ────────────────────────────────────────────────────
    emit statusChanged(QStringLiteral("Checking cache…"));
    emit progressChanged(8);

    if (cache.hasCachedVolume(series)) {
        emit statusChanged(QStringLiteral("Loading from cache…"));
        emit progressChanged(15);

        auto cached = cache.loadFromCache(series);
        if (cached) {
            DicomVolumeLoader::VolumeMetadata meta;
            cached->GetDimensions(meta.dimensions.data());
            cached->GetSpacing(meta.spacing.data());
            cached->GetOrigin(meta.origin.data());
            cached->GetScalarRange(meta.scalarRange.data());
            meta.numSlices        = meta.dimensions[2];
            meta.patientName      = series.patientName.toStdString();
            meta.patientId        = series.patientId.toStdString();
            meta.studyDate        = series.studyDate.toStdString();
            meta.studyInstanceUid  = series.studyInstanceUid.toStdString();
            meta.seriesInstanceUid = series.seriesInstanceUid.toStdString();
            meta.valid            = true;

            emit progressChanged(100);
            emit statusChanged(QStringLiteral("Loaded from cache."));
            emit volumeReady(cached, meta);
            return;
        }
        // Cache read failed; fall through to full DICOM load
    }

    // ── 2. Convert QStringList → std::vector<std::string> ─────────────────
    std::vector<std::string> sortedFiles;
    sortedFiles.reserve(static_cast<size_t>(series.filePaths.size()));
    for (const QString& p : series.filePaths)
        sortedFiles.push_back(p.toStdString());

    // ── 3. Downsampled preview (every 4th slice, for large series only) ───
    //    Shows a coarse but fast image while the full volume loads.
    constexpr int previewStep      = 4;
    constexpr int minSlicesPreview = 40;

    if (series.fileCount >= minSlicesPreview) {
        emit statusChanged(QStringLiteral("Generating preview…"));
        emit progressChanged(10);

        std::vector<std::string> previewFiles;
        previewFiles.reserve(sortedFiles.size() / previewStep + 1);
        for (size_t i = 0; i < sortedFiles.size(); i += previewStep)
            previewFiles.push_back(sortedFiles[i]);

        DicomVolumeLoader previewLoader;
        // Progress: 10 → 28 %
        auto previewVol = previewLoader.loadFromFiles(
            previewFiles,
            [this](int done, int total) {
                const int pct = 10 + static_cast<int>(
                    18.0 * done / (total > 0 ? total : 1));
                emit progressChanged(pct);
            });

        if (previewVol) {
            DicomVolumeLoader::VolumeMetadata pm = previewLoader.metadata();
            // Fill in series-level identifiers not present in the preview files
            pm.patientName       = series.patientName.toStdString();
            pm.patientId         = series.patientId.toStdString();
            pm.studyDate         = series.studyDate.toStdString();
            pm.studyInstanceUid  = series.studyInstanceUid.toStdString();
            pm.seriesInstanceUid = series.seriesInstanceUid.toStdString();
            emit previewReady(previewVol, pm);
        }
    }

    // ── 4. Load full-resolution volume ────────────────────────────────────
    emit statusChanged(QStringLiteral("Loading volume…"));
    emit progressChanged(28);

    DicomVolumeLoader loader;
    // Progress: 28 → 92 %
    auto volume = loader.loadFromFiles(
        sortedFiles,
        [this](int done, int total) {
            const int pct = 28 + static_cast<int>(
                64.0 * done / (total > 0 ? total : 1));
            emit progressChanged(pct);
        });

    if (!volume || !loader.metadata().valid) {
        emit errorOccurred(QString::fromStdString(loader.metadata().errorMessage));
        return;
    }

    // Patch metadata with series-level fields from the indexer
    DicomVolumeLoader::VolumeMetadata meta = loader.metadata();
    if (meta.patientName.empty())
        meta.patientName = series.patientName.toStdString();
    if (meta.patientId.empty())
        meta.patientId = series.patientId.toStdString();
    if (meta.studyDate.empty())
        meta.studyDate = series.studyDate.toStdString();
    meta.studyInstanceUid  = series.studyInstanceUid.toStdString();
    meta.seriesInstanceUid = series.seriesInstanceUid.toStdString();

    // ── 5. Save to cache ──────────────────────────────────────────────────
    emit statusChanged(QStringLiteral("Saving to cache…"));
    emit progressChanged(93);
    cache.saveToCache(series, volume.Get());

    emit progressChanged(100);
    emit statusChanged(QStringLiteral("Preparing MPR views…"));
    emit volumeReady(volume, meta);
}
