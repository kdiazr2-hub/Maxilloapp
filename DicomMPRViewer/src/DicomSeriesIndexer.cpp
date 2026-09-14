#include "DicomSeriesIndexer.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <map>

#include <QDir>
#include <QFileInfo>

#ifdef HAVE_GDCM
#  include <gdcmDirectory.h>
#  include <gdcmScanner.h>
#  include <gdcmTag.h>
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Helpers (file-scope)
// ─────────────────────────────────────────────────────────────────────────────

// Parse the Nth backslash-separated DS component.
static double parseDSComp(const char* s, int idx)
{
    if (!s) return 0.0;
    const char* p = s;
    for (int i = 0; i < idx; ++i) {
        p = std::strchr(p, '\\');
        if (!p) return 0.0;
        ++p;
    }
    return std::atof(p);
}

// ─────────────────────────────────────────────────────────────────────────────
QVector<SeriesInfo> DicomSeriesIndexer::indexFolder(const QString& folderPath)
{
#ifdef HAVE_GDCM
    return indexWithGDCM(folderPath);
#else
    return indexWithFilenames(folderPath);
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
#ifdef HAVE_GDCM
QVector<SeriesInfo> DicomSeriesIndexer::indexWithGDCM(const QString& folderPath)
{
    QVector<SeriesInfo> result;

    gdcm::Directory dir;
    // recursive=true: handles studies stored in sub-folders
    if (dir.Load(folderPath.toStdString(), /*recursive=*/true) == 0)
        return result;

    const gdcm::Directory::FilenamesType& allFiles = dir.GetFilenames();
    if (allFiles.empty()) return result;

    // ── Scan headers (no pixel decode) ────────────────────────────────────
    gdcm::Scanner sc;
    sc.AddTag(gdcm::Tag(0x0020, 0x000e)); // Series Instance UID
    sc.AddTag(gdcm::Tag(0x0020, 0x000d)); // Study Instance UID
    sc.AddTag(gdcm::Tag(0x0020, 0x0032)); // Image Position Patient (IPP)
    sc.AddTag(gdcm::Tag(0x0020, 0x0013)); // Instance Number
    sc.AddTag(gdcm::Tag(0x0020, 0x1041)); // Slice Location
    sc.AddTag(gdcm::Tag(0x0028, 0x0010)); // Rows   – used to filter non-image objects
    sc.AddTag(gdcm::Tag(0x0028, 0x0011)); // Columns
    sc.AddTag(gdcm::Tag(0x0008, 0x103e)); // Series Description
    sc.AddTag(gdcm::Tag(0x0008, 0x0060)); // Modality
    sc.AddTag(gdcm::Tag(0x0010, 0x0010)); // Patient Name
    sc.AddTag(gdcm::Tag(0x0010, 0x0020)); // Patient ID
    sc.AddTag(gdcm::Tag(0x0008, 0x0020)); // Study Date
    sc.Scan(allFiles);

    auto get = [&](const std::string& f, gdcm::Tag t) -> const char* {
        return sc.GetValue(f.c_str(), t);
    };
    auto qs = [](const char* s) -> QString {
        return s ? QString::fromLatin1(s).trimmed() : QString{};
    };

    // ── Group pixel-bearing files by SeriesInstanceUID ─────────────────────
    // Map: seriesUID → vector of {sortKey, filePath}
    std::map<std::string, std::vector<std::pair<double, std::string>>> groups;

    for (const auto& f : allFiles) {
        // Skip non-image DICOM objects (no Rows/Columns = structural report, etc.)
        if (!get(f, gdcm::Tag(0x0028, 0x0010)) ||
            !get(f, gdcm::Tag(0x0028, 0x0011))) continue;

        const char* uid = get(f, gdcm::Tag(0x0020, 0x000e));
        const std::string key = (uid && *uid) ? uid : "__unknown__";

        // Sort key: IPP-Z (fastest proxy for axial CT) → InstanceNumber → SliceLoc
        double sortKey = 0.0;
        if (const char* ipp = get(f, gdcm::Tag(0x0020, 0x0032)))
            sortKey = parseDSComp(ipp, 2);
        else if (const char* inst = get(f, gdcm::Tag(0x0020, 0x0013)))
            sortKey = static_cast<double>(std::atoi(inst));
        else if (const char* sl = get(f, gdcm::Tag(0x0020, 0x1041)))
            sortKey = std::atof(sl);
        else
            sortKey = static_cast<double>(groups[key].size());

        groups[key].emplace_back(sortKey, f);
    }

    if (groups.empty()) return result;

    // ── Build one SeriesInfo per group ─────────────────────────────────────
    for (auto& [uid, files] : groups) {
        std::sort(files.begin(), files.end());

        SeriesInfo info;
        info.seriesInstanceUid = qs(uid.c_str());
        info.fileCount = static_cast<int>(files.size());

        for (const auto& [key, path] : files)
            info.filePaths.append(QString::fromLocal8Bit(path.c_str()));

        if (!files.empty()) {
            const std::string& first = files.front().second;
            info.studyInstanceUid  = qs(get(first, gdcm::Tag(0x0020, 0x000d)));
            info.seriesDescription = qs(get(first, gdcm::Tag(0x0008, 0x103e)));
            info.modality          = qs(get(first, gdcm::Tag(0x0008, 0x0060)));
            info.patientName       = qs(get(first, gdcm::Tag(0x0010, 0x0010)));
            info.patientId         = qs(get(first, gdcm::Tag(0x0010, 0x0020)));
            info.studyDate         = qs(get(first, gdcm::Tag(0x0008, 0x0020)));
        }

        result.append(std::move(info));
    }

    // Largest series first (the main CT series is usually the biggest)
    std::sort(result.begin(), result.end(),
        [](const SeriesInfo& a, const SeriesInfo& b) {
            return a.fileCount > b.fileCount;
        });

    return result;
}
#endif // HAVE_GDCM

// ─────────────────────────────────────────────────────────────────────────────
// Fallback: no GDCM — treat whole folder as one unnamed series,
// sorted by filename.
// ─────────────────────────────────────────────────────────────────────────────
QVector<SeriesInfo> DicomSeriesIndexer::indexWithFilenames(const QString& folderPath)
{
    QDir dir(folderPath);
    if (!dir.exists()) return {};

    const QStringList entries = dir.entryList(QDir::Files, QDir::Name);
    if (entries.isEmpty()) return {};

    SeriesInfo info;
    info.seriesInstanceUid = QStringLiteral("unknown");
    info.fileCount = entries.size();
    for (const QString& e : entries)
        info.filePaths.append(dir.filePath(e));

    return {std::move(info)};
}
