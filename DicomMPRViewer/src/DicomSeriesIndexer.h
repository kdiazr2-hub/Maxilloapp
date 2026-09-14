#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QMetaType>

// ─────────────────────────────────────────────────────────────────────────────
// SeriesInfo
//
// Describes one DICOM series found during folder indexing.
// filePaths is pre-sorted by ImagePositionPatient (projected onto the slice
// normal) or InstanceNumber — ready to pass to DicomVolumeLoader::loadFromFiles.
// ─────────────────────────────────────────────────────────────────────────────
struct SeriesInfo
{
    QString     seriesInstanceUid;
    QString     studyInstanceUid;
    QString     seriesDescription;
    QString     patientName;
    QString     patientId;
    QString     studyDate;
    QString     modality;
    int         fileCount = 0;
    QStringList filePaths;   // pre-sorted; ready for loadFromFiles()
};

Q_DECLARE_METATYPE(SeriesInfo)
Q_DECLARE_METATYPE(QVector<SeriesInfo>)

// ─────────────────────────────────────────────────────────────────────────────
// DicomSeriesIndexer
//
// Scans a folder (recursively) for DICOM files.
// Groups them by SeriesInstanceUID and pre-sorts each group's slices.
// No pixel decompression is performed — headers only.
//
// Thread-safe: stateless; call from any thread.
// ─────────────────────────────────────────────────────────────────────────────
class DicomSeriesIndexer
{
public:
    // Scans folderPath and returns all series found, sorted by file count
    // (largest — typically the primary CT series — comes first).
    QVector<SeriesInfo> indexFolder(const QString& folderPath);

private:
#ifdef HAVE_GDCM
    QVector<SeriesInfo> indexWithGDCM(const QString& folderPath);
#endif
    QVector<SeriesInfo> indexWithFilenames(const QString& folderPath);
};
