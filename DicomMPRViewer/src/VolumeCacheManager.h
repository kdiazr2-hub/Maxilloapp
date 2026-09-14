#pragma once

#include <QString>

#include <vtkImageData.h>
#include <vtkSmartPointer.h>

#include "DicomSeriesIndexer.h"   // SeriesInfo
#include "DicomVolumeLoader.h"    // VolumeMetadata

// ─────────────────────────────────────────────────────────────────────────────
// VolumeCacheManager
//
// Stores and retrieves pre-processed vtkImageData volumes on disk using the
// MetaImage (.mha) format so that re-opening the same DICOM study is fast.
//
// Cache location:
//   {cacheDir}/{md5(seriesUID)}.mha   – the vtkImageData as a flat binary
//   {cacheDir}/{md5(seriesUID)}.json  – validation metadata (file count,
//                                       total file size, dimensions, spacing)
//
// Invalidation strategy:
//   A cached volume is considered valid when:
//     • The JSON metadata file exists alongside the .mha file
//     • SeriesInstanceUID matches
//     • File count in DICOM folder matches
//     • Sum of individual DICOM file sizes matches (cheap file-size check,
//       detects modifications without reading pixel data)
//     • Volume dimensions and spacing match the values stored in JSON
//
// Thread-safe: all operations are self-contained; no shared mutable state.
// Call from any thread (including worker threads).
// ─────────────────────────────────────────────────────────────────────────────
class VolumeCacheManager
{
public:
    // cacheDir defaults to QStandardPaths::AppLocalDataLocation + "/DicomCache"
    explicit VolumeCacheManager(const QString& cacheDir = QString());

    // Returns true if a valid, up-to-date cache file exists for this series.
    bool hasCachedVolume(const SeriesInfo& series) const;

    // Reads the cached volume. Returns nullptr if missing or invalid.
    vtkSmartPointer<vtkImageData> loadFromCache(const SeriesInfo& series) const;

    // Saves volume + validation metadata to cache.
    // The write is best-effort: if it fails the caller still has the volume.
    void saveToCache(const SeriesInfo& series, vtkImageData* volume) const;

    // Deletes all files in the cache directory.
    void clearCache() const;

    // Expose the resolved cache directory path (useful for diagnostics / README).
    QString cacheDirectory() const;

private:
    QString resolvedCacheDir() const;
    QString cacheKey(const SeriesInfo& series) const;
    QString cacheMhaPath(const SeriesInfo& series) const;
    QString cacheJsonPath(const SeriesInfo& series) const;
    qint64  totalFileSize(const SeriesInfo& series) const;

    QString m_cacheDir;
};
