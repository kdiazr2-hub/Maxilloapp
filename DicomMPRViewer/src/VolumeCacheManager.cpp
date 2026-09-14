#include "VolumeCacheManager.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <vtkMetaImageReader.h>
#include <vtkMetaImageWriter.h>
#include <vtkSmartPointer.h>

// ─────────────────────────────────────────────────────────────────────────────
VolumeCacheManager::VolumeCacheManager(const QString& cacheDir)
    : m_cacheDir(cacheDir)
{}

// ─────────────────────────────────────────────────────────────────────────────
QString VolumeCacheManager::resolvedCacheDir() const
{
    if (!m_cacheDir.isEmpty()) return m_cacheDir;
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
           + QStringLiteral("/DicomCache");
}

QString VolumeCacheManager::cacheDirectory() const
{
    return resolvedCacheDir();
}

// ─────────────────────────────────────────────────────────────────────────────
// MD5 of the SeriesInstanceUID → 32-char hex string used as the filename stem.
// SeriesInstanceUIDs can be arbitrarily long and contain dots, which are valid
// in filenames but awkward; the hash produces a clean, fixed-length key.
// ─────────────────────────────────────────────────────────────────────────────
QString VolumeCacheManager::cacheKey(const SeriesInfo& series) const
{
    const QByteArray uid =
        series.seriesInstanceUid.toUtf8().isEmpty()
            ? QByteArray("unknown")
            : series.seriesInstanceUid.toUtf8();
    return QString::fromLatin1(
        QCryptographicHash::hash(uid, QCryptographicHash::Md5).toHex());
}

QString VolumeCacheManager::cacheMhaPath(const SeriesInfo& series) const
{
    return resolvedCacheDir() + QChar('/') + cacheKey(series) + QStringLiteral(".mha");
}

QString VolumeCacheManager::cacheJsonPath(const SeriesInfo& series) const
{
    return resolvedCacheDir() + QChar('/') + cacheKey(series) + QStringLiteral(".json");
}

// ─────────────────────────────────────────────────────────────────────────────
// Sum of all individual DICOM file sizes — fast to compute (stat() only),
// changes when any file is overwritten or replaced.
// ─────────────────────────────────────────────────────────────────────────────
qint64 VolumeCacheManager::totalFileSize(const SeriesInfo& series) const
{
    qint64 total = 0;
    for (const QString& p : series.filePaths)
        total += QFileInfo(p).size();
    return total;
}

// ─────────────────────────────────────────────────────────────────────────────
bool VolumeCacheManager::hasCachedVolume(const SeriesInfo& series) const
{
    const QString mhaPath  = cacheMhaPath(series);
    const QString jsonPath = cacheJsonPath(series);

    if (!QFileInfo::exists(mhaPath) || !QFileInfo::exists(jsonPath))
        return false;

    // Load + validate JSON metadata
    QFile f(jsonPath);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (doc.isNull() || !doc.isObject()) return false;
    const QJsonObject obj = doc.object();

    // Compare stored values with current series state
    if (obj["seriesInstanceUid"].toString() != series.seriesInstanceUid) return false;
    if (obj["fileCount"].toInt()           != series.fileCount)          return false;
    if (obj["totalFileSize"].toVariant().toLongLong() != totalFileSize(series)) return false;

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
vtkSmartPointer<vtkImageData>
VolumeCacheManager::loadFromCache(const SeriesInfo& series) const
{
    const QString mhaPath = cacheMhaPath(series);
    if (!QFileInfo::exists(mhaPath)) return nullptr;

    auto reader = vtkSmartPointer<vtkMetaImageReader>::New();
    reader->SetFileName(mhaPath.toLocal8Bit().constData());
    reader->Update();

    if (reader->GetErrorCode() != 0) return nullptr;

    vtkImageData* raw = reader->GetOutput();
    if (!raw || raw->GetNumberOfPoints() == 0) return nullptr;

    auto out = vtkSmartPointer<vtkImageData>::New();
    out->DeepCopy(raw);
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
void VolumeCacheManager::saveToCache(const SeriesInfo& series,
                                     vtkImageData* volume) const
{
    if (!volume) return;

    const QString dir = resolvedCacheDir();
    QDir().mkpath(dir);   // ensure directory exists

    // ── Write .mha (uncompressed for fast read) ───────────────────────────
    const QString mhaPath = cacheMhaPath(series);
    auto writer = vtkSmartPointer<vtkMetaImageWriter>::New();
    writer->SetFileName(mhaPath.toLocal8Bit().constData());
    writer->SetInputData(volume);
    writer->SetCompression(false);  // uncompressed = fast read, ~200 MB for typical CT
    writer->Write();

    if (writer->GetErrorCode() != 0) return;  // write failed; don't write JSON

    // ── Write .json (validation metadata) ────────────────────────────────
    int dims[3] = {};
    double sp[3] = {};
    volume->GetDimensions(dims);
    volume->GetSpacing(sp);

    QJsonObject obj;
    obj["seriesInstanceUid"] = series.seriesInstanceUid;
    obj["fileCount"]         = series.fileCount;
    obj["totalFileSize"]     = QString::number(totalFileSize(series));
    obj["dimX"] = dims[0];
    obj["dimY"] = dims[1];
    obj["dimZ"] = dims[2];
    obj["spX"]  = sp[0];
    obj["spY"]  = sp[1];
    obj["spZ"]  = sp[2];

    QFile f(cacheJsonPath(series));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(obj).toJson());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void VolumeCacheManager::clearCache() const
{
    QDir dir(resolvedCacheDir());   // non-const: remove() requires non-const QDir
    if (!dir.exists()) return;

    const QStringList files = dir.entryList(
        {QStringLiteral("*.mha"), QStringLiteral("*.json")}, QDir::Files);
    for (const QString& f : files)
        dir.remove(f);
}
