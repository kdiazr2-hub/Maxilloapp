#include "DicomVolumeLoader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

#include <vtkDICOMImageReader.h>
#include <vtkImageData.h>
#include <vtkImageShiftScale.h>
#include <vtkSmartPointer.h>

#ifdef HAVE_GDCM
#  include <gdcmDirectory.h>
#  include <gdcmImageReader.h>
#  include <gdcmImage.h>
#  include <gdcmPixelFormat.h>
#  include <gdcmScanner.h>
#  include <gdcmTag.h>
#  include <vtkType.h>
#endif

// ─────────────────────────────────────────────────────────────────────────────
vtkSmartPointer<vtkImageData>
DicomVolumeLoader::load(const std::string& folderPath)
{
    m_meta = VolumeMetadata{};
    if (folderPath.empty()) {
        m_meta.errorMessage = "No folder path provided.";
        return nullptr;
    }
#ifdef HAVE_GDCM
    return loadWithGDCM(folderPath);
#else
    return loadWithVTK(folderPath);
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
#ifdef HAVE_GDCM

// Parse component idx from a DICOM DS value like "1.2\3.4\5.6"
static double parseDS(const char* str, int idx)
{
    if (!str) return 0.0;
    const char* p = str;
    for (int i = 0; i < idx; ++i) {
        p = std::strchr(p, '\\');
        if (!p) return 0.0;
        ++p;
    }
    return std::atof(p);
}

// ─────────────────────────────────────────────────────────────────────────────
static bool parseDSVector(const char* str, double* out, int count)
{
    if (!str || !out || count <= 0) return false;

    const char* p = str;
    for (int i = 0; i < count; ++i) {
        char* end = nullptr;
        out[i] = std::strtod(p, &end);
        if (end == p) return false;

        if (i + 1 < count) {
            p = std::strchr(end, '\\');
            if (!p) return false;
            ++p;
        }
    }
    return true;
}

static std::array<double, 3> sliceNormalFromIOP(const char* iop)
{
    double dc[6] = {};
    if (!parseDSVector(iop, dc, 6)) {
        return {0.0, 0.0, 1.0};
    }

    const std::array<double, 3> row = {dc[0], dc[1], dc[2]};
    const std::array<double, 3> col = {dc[3], dc[4], dc[5]};
    std::array<double, 3> normal = {
        row[1] * col[2] - row[2] * col[1],
        row[2] * col[0] - row[0] * col[2],
        row[0] * col[1] - row[1] * col[0]
    };

    const double len = std::sqrt(normal[0] * normal[0] +
                                 normal[1] * normal[1] +
                                 normal[2] * normal[2]);
    if (len <= 1e-9) {
        return {0.0, 0.0, 1.0};
    }

    normal[0] /= len;
    normal[1] /= len;
    normal[2] /= len;
    return normal;
}

static bool projectedSlicePosition(const char* ipp,
                                   const std::array<double, 3>& normal,
                                   double& position)
{
    double origin[3] = {};
    if (!parseDSVector(ipp, origin, 3)) return false;
    position = origin[0] * normal[0] +
               origin[1] * normal[1] +
               origin[2] * normal[2];
    return true;
}

static double medianSpacing(std::vector<double> deltas)
{
    deltas.erase(
        std::remove_if(deltas.begin(), deltas.end(),
                       [](double d) { return d <= 1e-6 || !std::isfinite(d); }),
        deltas.end());
    if (deltas.empty()) return 0.0;

    std::sort(deltas.begin(), deltas.end());
    return deltas[deltas.size() / 2];
}

static bool plausibleCtSpacing(double spacing)
{
    return spacing > 0.0 && spacing < 20.0;
}

vtkSmartPointer<vtkImageData>
DicomVolumeLoader::loadWithGDCM(const std::string& folderPath)
{
    // ── 1. Collect all files ───────────────────────────────────────────────
    gdcm::Directory dir;
    if (dir.Load(folderPath, true) == 0) {
        m_meta.errorMessage = "No files found in folder or subfolders: " + folderPath;
        return nullptr;
    }
    gdcm::Directory::FilenamesType allFiles = dir.GetFilenames();

    // ── 2. Scan ALL metadata tags from DICOM headers ───────────────────────
    // gdcm::Scanner reads the uncompressed DICOM header — works for JPEG2000,
    // JPEG-LS, and all other transfer syntaxes without decompressing pixels.
    gdcm::Scanner sc;
    sc.AddTag(gdcm::Tag(0x0020, 0x000d)); // Study Instance UID
    sc.AddTag(gdcm::Tag(0x0020, 0x000e)); // Series Instance UID
    sc.AddTag(gdcm::Tag(0x0020, 0x0032)); // Image Position Patient (IPP)
    sc.AddTag(gdcm::Tag(0x0020, 0x0037)); // Image Orientation Patient (IOP)
    sc.AddTag(gdcm::Tag(0x0020, 0x0013)); // Instance Number
    sc.AddTag(gdcm::Tag(0x0020, 0x1041)); // Slice Location
    sc.AddTag(gdcm::Tag(0x0028, 0x0010)); // Rows
    sc.AddTag(gdcm::Tag(0x0028, 0x0011)); // Columns
    sc.AddTag(gdcm::Tag(0x0028, 0x0030)); // Pixel Spacing  "rowSp\colSp"
    sc.AddTag(gdcm::Tag(0x0028, 0x0031)); // Imager Pixel Spacing (fallback)
    sc.AddTag(gdcm::Tag(0x0018, 0x0088)); // Spacing Between Slices
    sc.AddTag(gdcm::Tag(0x0018, 0x0050)); // Slice Thickness
    sc.AddTag(gdcm::Tag(0x0028, 0x1053)); // Rescale Slope
    sc.AddTag(gdcm::Tag(0x0028, 0x1052)); // Rescale Intercept
    sc.AddTag(gdcm::Tag(0x0010, 0x0010)); // Patient Name
    sc.AddTag(gdcm::Tag(0x0010, 0x0020)); // Patient ID
    sc.AddTag(gdcm::Tag(0x0008, 0x0020)); // Study Date
    sc.Scan(allFiles);

    auto tagVal = [&](const std::string& f, gdcm::Tag t) -> const char* {
        return sc.GetValue(f.c_str(), t);
    };
    // ── 3. Sort slices by IPP using Scanner (no pixel decompression needed) ─
    // Strategy:
    //   a) Project Image Position Patient onto the IOP slice normal
    //   b) Fallback: Instance Number
    //   c) Fallback: Slice Location
    //   d) Fallback: filename sort

    std::map<std::string, gdcm::Directory::FilenamesType> filesBySeries;
    gdcm::Directory::FilenamesType filesWithoutSeries;
    for (const auto& f : allFiles) {
        const bool hasImageGeometry =
            tagVal(f, gdcm::Tag(0x0028, 0x0010)) &&
            tagVal(f, gdcm::Tag(0x0028, 0x0011));
        if (!hasImageGeometry) continue;

        const char* uid = tagVal(f, gdcm::Tag(0x0020, 0x000e));
        if (uid && *uid) {
            filesBySeries[uid].push_back(f);
        } else {
            filesWithoutSeries.push_back(f);
        }
    }

    gdcm::Directory::FilenamesType selectedFiles;
    for (const auto& series : filesBySeries) {
        if (series.second.size() > selectedFiles.size()) {
            selectedFiles = series.second;
        }
    }
    if (selectedFiles.empty()) selectedFiles = filesWithoutSeries;

    if (selectedFiles.empty()) {
        m_meta.errorMessage =
            "No valid DICOM image series found in folder or subfolders: " + folderPath;
        return nullptr;
    }
    allFiles = selectedFiles;

    using FileZ = std::pair<double, std::string>;
    const auto normal = sliceNormalFromIOP(
        tagVal(allFiles[0], gdcm::Tag(0x0020, 0x0037)));

    std::vector<FileZ> byPosition;
    byPosition.reserve(allFiles.size());

    bool allHaveIPP = true;
    for (const auto& f : allFiles) {
        const char* ipp = tagVal(f, gdcm::Tag(0x0020, 0x0032));
        double position = 0.0;
        if (!projectedSlicePosition(ipp, normal, position)) {
            allHaveIPP = false;
            break;
        }
        byPosition.push_back({ position, f });
    }

    gdcm::Directory::FilenamesType sortedFiles;
    double zSp = 0.0;

    if (allHaveIPP && byPosition.size() == allFiles.size()) {
        std::sort(byPosition.begin(), byPosition.end());
        for (const auto& p : byPosition) sortedFiles.push_back(p.second);

        std::vector<double> deltas;
        deltas.reserve(byPosition.size());
        for (size_t i = 1; i < byPosition.size(); ++i) {
            deltas.push_back(std::abs(byPosition[i].first - byPosition[i-1].first));
        }
        zSp = medianSpacing(std::move(deltas));
    }

    // Fallback: Instance Number
    if (sortedFiles.empty()) {
        std::vector<FileZ> byInst;
        byInst.reserve(allFiles.size());
        bool ok = true;
        for (const auto& f : allFiles) {
            const char* v = tagVal(f, gdcm::Tag(0x0020, 0x0013));
            if (!v) { ok = false; break; }
            byInst.push_back({ static_cast<double>(std::atoi(v)), f });
        }
        if (ok && !byInst.empty()) {
            std::sort(byInst.begin(), byInst.end());
            for (const auto& p : byInst) sortedFiles.push_back(p.second);
        }
    }

    // Fallback: Slice Location
    if (sortedFiles.empty()) {
        std::vector<FileZ> bySL;
        bySL.reserve(allFiles.size());
        bool ok = true;
        for (const auto& f : allFiles) {
            const char* v = tagVal(f, gdcm::Tag(0x0020, 0x1041));
            if (!v) { ok = false; break; }
            bySL.push_back({ std::atof(v), f });
        }
        if (ok && !bySL.empty()) {
            std::sort(bySL.begin(), bySL.end());
            for (const auto& p : bySL) sortedFiles.push_back(p.second);
        }
    }

    // Fallback: filename sort
    if (sortedFiles.empty()) {
        sortedFiles = allFiles;
        std::sort(sortedFiles.begin(), sortedFiles.end());
    }

    if (sortedFiles.empty()) {
        m_meta.errorMessage = "No valid DICOM slices in: " + folderPath;
        return nullptr;
    }

    // ── 4. XY spacing from Pixel Spacing tag ──────────────────────────────
    double xSp = 0.0, ySp = 0.0;
    bool xySpacingFromTags = false;
    const char* ps = tagVal(sortedFiles[0], gdcm::Tag(0x0028, 0x0030));
    if (!ps) ps = tagVal(sortedFiles[0], gdcm::Tag(0x0028, 0x0031));
    if (ps) {
        ySp = parseDS(ps, 0); // row spacing → Y
        xSp = parseDS(ps, 1); // col spacing → X
        if (xSp <= 0.0) xSp = ySp;
        if (ySp <= 0.0) ySp = xSp;
        xySpacingFromTags = xSp > 0.0 && ySp > 0.0;
    }
    if (xSp <= 0.0) xSp = 1.0;
    if (ySp <= 0.0) ySp = 1.0;

    // ── 5. Z spacing fallback chain ────────────────────────────────────────
    if (!plausibleCtSpacing(zSp)) {
        const char* s = tagVal(sortedFiles[0], gdcm::Tag(0x0018, 0x0088));
        const double candidate = s ? std::atof(s) : 0.0;
        if (plausibleCtSpacing(candidate)) zSp = candidate;
    }
    if (!plausibleCtSpacing(zSp)) {
        const char* s = tagVal(sortedFiles[0], gdcm::Tag(0x0018, 0x0050));
        const double candidate = s ? std::atof(s) : 0.0;
        if (plausibleCtSpacing(candidate)) zSp = candidate;
    }
    if (!plausibleCtSpacing(zSp) && plausibleCtSpacing(zSp / 1000.0)) zSp /= 1000.0;
    if (!plausibleCtSpacing(zSp)) zSp = 1.0;

    // ── 6. Origin from IPP of first sorted slice ───────────────────────────
    double ox = 0.0, oy = 0.0, oz = 0.0;
    if (const char* ipp = tagVal(sortedFiles[0], gdcm::Tag(0x0020, 0x0032))) {
        ox = parseDS(ipp, 0);
        oy = parseDS(ipp, 1);
        oz = parseDS(ipp, 2);
    }

    // ── 7. Read first file for pixel format / dimensions ──────────────────
    gdcm::ImageReader firstReader;
    firstReader.SetFileName(sortedFiles[0].c_str());
    if (!firstReader.Read()) {
        m_meta.errorMessage = "GDCM cannot read: " + sortedFiles[0];
        return nullptr;
    }
    const gdcm::Image& first = firstReader.GetImage();
    const unsigned int* dims = first.GetDimensions();
    unsigned int cols    = dims[0];
    unsigned int rows    = dims[1];
    unsigned int nSlices = static_cast<unsigned int>(sortedFiles.size());

    const double* imageSpacing = first.GetSpacing();
    if (imageSpacing) {
        if (!xySpacingFromTags) {
            if (plausibleCtSpacing(imageSpacing[0])) xSp = imageSpacing[0];
            if (plausibleCtSpacing(imageSpacing[1])) ySp = imageSpacing[1];
        }
        if (std::abs(zSp - 1.0) < 1e-6 &&
            std::abs(imageSpacing[2] - 1.0) > 1e-6 &&
            plausibleCtSpacing(imageSpacing[2])) {
            zSp = imageSpacing[2];
        }
    }

    int vtkType = VTK_SHORT;
    switch (first.GetPixelFormat().GetScalarType()) {
        case gdcm::PixelFormat::UINT8:   vtkType = VTK_UNSIGNED_CHAR;  break;
        case gdcm::PixelFormat::INT8:    vtkType = VTK_SIGNED_CHAR;    break;
        case gdcm::PixelFormat::UINT16:  vtkType = VTK_UNSIGNED_SHORT; break;
        case gdcm::PixelFormat::INT16:   vtkType = VTK_SHORT;          break;
        case gdcm::PixelFormat::UINT32:  vtkType = VTK_UNSIGNED_INT;   break;
        case gdcm::PixelFormat::INT32:   vtkType = VTK_INT;            break;
        case gdcm::PixelFormat::FLOAT32: vtkType = VTK_FLOAT;          break;
        case gdcm::PixelFormat::FLOAT64: vtkType = VTK_DOUBLE;         break;
        default: break;
    }

    // ── 8. Build vtkImageData with correct geometry ───────────────────────
    auto rawData = vtkSmartPointer<vtkImageData>::New();
    rawData->SetDimensions(static_cast<int>(cols),
                           static_cast<int>(rows),
                           static_cast<int>(nSlices));
    rawData->SetSpacing(xSp, ySp, zSp);
    rawData->SetOrigin(ox, oy, oz);
    rawData->AllocateScalars(vtkType, 1);

    // ── 9. Fill volume slice by slice ─────────────────────────────────────
    size_t sliceBytes = first.GetBufferLength();
    char*  buf        = static_cast<char*>(rawData->GetScalarPointer());

    for (unsigned int i = 0; i < nSlices; ++i) {
        gdcm::ImageReader reader;
        reader.SetFileName(sortedFiles[i].c_str());
        if (!reader.Read()) {
            std::memset(buf + i * sliceBytes, 0, sliceBytes);
            continue;
        }
        reader.GetImage().GetBuffer(buf + i * sliceBytes);
    }

    // ── 10. Rescale Slope/Intercept → Hounsfield Units ────────────────────
    double slope     = 1.0;
    double intercept = 0.0;
    if (const char* s = tagVal(sortedFiles[0], gdcm::Tag(0x0028, 0x1053))) slope     = std::atof(s);
    if (const char* s = tagVal(sortedFiles[0], gdcm::Tag(0x0028, 0x1052))) intercept = std::atof(s);

    // vtkImageShiftScale formula: out = (in + Shift) * Scale
    // HU = pixel * slope + intercept → Scale=slope, Shift=intercept/slope
    auto ss = vtkSmartPointer<vtkImageShiftScale>::New();
    ss->SetInputData(rawData);
    ss->SetScale(slope);
    ss->SetShift(intercept / slope);
    ss->SetOutputScalarTypeToShort();
    ss->ClampOverflowOn();
    ss->Update();

    // ── 11. Metadata ───────────────────────────────────────────────────────
    auto safeStr = [](const char* s) -> std::string { return s ? s : ""; };
    m_meta.patientName = safeStr(tagVal(sortedFiles[0], gdcm::Tag(0x0010, 0x0010)));
    m_meta.patientId   = safeStr(tagVal(sortedFiles[0], gdcm::Tag(0x0010, 0x0020)));
    m_meta.studyDate   = safeStr(tagVal(sortedFiles[0], gdcm::Tag(0x0008, 0x0020)));
    m_meta.studyInstanceUid  = safeStr(tagVal(sortedFiles[0], gdcm::Tag(0x0020, 0x000d)));
    m_meta.seriesInstanceUid = safeStr(tagVal(sortedFiles[0], gdcm::Tag(0x0020, 0x000e)));

    populateMetadata(ss->GetOutput());
    m_meta.valid = true;

    auto out = vtkSmartPointer<vtkImageData>::New();
    out->DeepCopy(ss->GetOutput());
    return out;
}
#endif // HAVE_GDCM

// ─────────────────────────────────────────────────────────────────────────────
// loadFromFiles — public entry point
// ─────────────────────────────────────────────────────────────────────────────
vtkSmartPointer<vtkImageData>
DicomVolumeLoader::loadFromFiles(const std::vector<std::string>& sortedFiles,
                                  ProgressCallback progressCb)
{
    m_meta = VolumeMetadata{};
    if (sortedFiles.empty()) {
        m_meta.errorMessage = "No files provided.";
        return nullptr;
    }
#ifdef HAVE_GDCM
    return loadFromFilesGDCM(sortedFiles, std::move(progressCb));
#else
    // Without GDCM: derive folder from first file and use vtkDICOMImageReader.
    const size_t sep = sortedFiles[0].find_last_of("/\\");
    const std::string folder =
        (sep != std::string::npos) ? sortedFiles[0].substr(0, sep) : ".";
    return loadWithVTK(folder);
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
#ifdef HAVE_GDCM
// loadFromFilesGDCM — core pixel-loading logic with progress reporting.
// Receives an already-sorted file list (supplied by DicomSeriesIndexer or
// by loadWithGDCM after its own sort).  Does steps 4-11 from loadWithGDCM.
// ─────────────────────────────────────────────────────────────────────────────
vtkSmartPointer<vtkImageData>
DicomVolumeLoader::loadFromFilesGDCM(const std::vector<std::string>& sortedFiles,
                                      ProgressCallback progressCb)
{
    // ── Scan tag metadata (header-only; fast for any transfer syntax) ──────
    gdcm::Scanner sc;
    sc.AddTag(gdcm::Tag(0x0020, 0x000d)); // Study Instance UID
    sc.AddTag(gdcm::Tag(0x0020, 0x000e)); // Series Instance UID
    sc.AddTag(gdcm::Tag(0x0020, 0x0032)); // Image Position Patient
    sc.AddTag(gdcm::Tag(0x0020, 0x0037)); // Image Orientation Patient
    sc.AddTag(gdcm::Tag(0x0028, 0x0030)); // Pixel Spacing
    sc.AddTag(gdcm::Tag(0x0028, 0x0031)); // Imager Pixel Spacing (fallback)
    sc.AddTag(gdcm::Tag(0x0018, 0x0088)); // Spacing Between Slices
    sc.AddTag(gdcm::Tag(0x0018, 0x0050)); // Slice Thickness
    sc.AddTag(gdcm::Tag(0x0028, 0x1053)); // Rescale Slope
    sc.AddTag(gdcm::Tag(0x0028, 0x1052)); // Rescale Intercept
    sc.AddTag(gdcm::Tag(0x0010, 0x0010)); // Patient Name
    sc.AddTag(gdcm::Tag(0x0010, 0x0020)); // Patient ID
    sc.AddTag(gdcm::Tag(0x0008, 0x0020)); // Study Date

    const gdcm::Directory::FilenamesType gdcmFiles(
        sortedFiles.begin(), sortedFiles.end());
    sc.Scan(gdcmFiles);

    auto tagVal = [&](const std::string& f, gdcm::Tag t) -> const char* {
        return sc.GetValue(f.c_str(), t);
    };

    // ── XY spacing ─────────────────────────────────────────────────────────
    double xSp = 0.0, ySp = 0.0;
    bool xySpacingFromTags = false;
    const char* ps = tagVal(sortedFiles[0], gdcm::Tag(0x0028, 0x0030));
    if (!ps) ps = tagVal(sortedFiles[0], gdcm::Tag(0x0028, 0x0031));
    if (ps) {
        ySp = parseDS(ps, 0);
        xSp = parseDS(ps, 1);
        if (xSp <= 0.0) xSp = ySp;
        if (ySp <= 0.0) ySp = xSp;
        xySpacingFromTags = xSp > 0.0 && ySp > 0.0;
    }
    if (xSp <= 0.0) xSp = 1.0;
    if (ySp <= 0.0) ySp = 1.0;

    // ── Z spacing: project IPP onto the slice normal, use median delta ──────
    double zSp = 0.0;
    if (sortedFiles.size() >= 2) {
        const auto normal = sliceNormalFromIOP(
            tagVal(sortedFiles[0], gdcm::Tag(0x0020, 0x0037)));

        std::vector<double> deltas;
        deltas.reserve(sortedFiles.size() - 1);
        double prevPos = 0.0;
        bool   prevOk  = false;
        for (const auto& f : sortedFiles) {
            double pos = 0.0;
            if (projectedSlicePosition(tagVal(f, gdcm::Tag(0x0020, 0x0032)),
                                        normal, pos)) {
                if (prevOk) deltas.push_back(std::abs(pos - prevPos));
                prevPos = pos;
                prevOk  = true;
            }
        }
        zSp = medianSpacing(std::move(deltas));
    }

    // ── Z spacing fallback chain ────────────────────────────────────────────
    if (!plausibleCtSpacing(zSp)) {
        const char* s = tagVal(sortedFiles[0], gdcm::Tag(0x0018, 0x0088));
        const double c = s ? std::atof(s) : 0.0;
        if (plausibleCtSpacing(c)) zSp = c;
    }
    if (!plausibleCtSpacing(zSp)) {
        const char* s = tagVal(sortedFiles[0], gdcm::Tag(0x0018, 0x0050));
        const double c = s ? std::atof(s) : 0.0;
        if (plausibleCtSpacing(c)) zSp = c;
    }
    if (!plausibleCtSpacing(zSp) && plausibleCtSpacing(zSp / 1000.0)) zSp /= 1000.0;
    if (!plausibleCtSpacing(zSp)) zSp = 1.0;

    // ── Origin from IPP of first sorted slice ───────────────────────────────
    double ox = 0.0, oy = 0.0, oz = 0.0;
    if (const char* ipp = tagVal(sortedFiles[0], gdcm::Tag(0x0020, 0x0032))) {
        ox = parseDS(ipp, 0);
        oy = parseDS(ipp, 1);
        oz = parseDS(ipp, 2);
    }

    // ── Read first file: pixel format & dimensions ──────────────────────────
    gdcm::ImageReader firstReader;
    firstReader.SetFileName(sortedFiles[0].c_str());
    if (!firstReader.Read()) {
        m_meta.errorMessage = "GDCM cannot read: " + sortedFiles[0];
        return nullptr;
    }
    const gdcm::Image& first = firstReader.GetImage();
    const unsigned int* dims = first.GetDimensions();
    const unsigned int cols    = dims[0];
    const unsigned int rows    = dims[1];
    const unsigned int nSlices = static_cast<unsigned int>(sortedFiles.size());

    // Allow image-embedded spacing to fill gaps left by tag parsing
    if (const double* sp = first.GetSpacing()) {
        if (!xySpacingFromTags) {
            if (plausibleCtSpacing(sp[0])) xSp = sp[0];
            if (plausibleCtSpacing(sp[1])) ySp = sp[1];
        }
        if (std::abs(zSp - 1.0) < 1e-6 &&
            plausibleCtSpacing(sp[2]) &&
            std::abs(sp[2] - 1.0) > 1e-6) {
            zSp = sp[2];
        }
    }

    int vtkType = VTK_SHORT;
    switch (first.GetPixelFormat().GetScalarType()) {
        case gdcm::PixelFormat::UINT8:   vtkType = VTK_UNSIGNED_CHAR;  break;
        case gdcm::PixelFormat::INT8:    vtkType = VTK_SIGNED_CHAR;    break;
        case gdcm::PixelFormat::UINT16:  vtkType = VTK_UNSIGNED_SHORT; break;
        case gdcm::PixelFormat::INT16:   vtkType = VTK_SHORT;          break;
        case gdcm::PixelFormat::UINT32:  vtkType = VTK_UNSIGNED_INT;   break;
        case gdcm::PixelFormat::INT32:   vtkType = VTK_INT;            break;
        case gdcm::PixelFormat::FLOAT32: vtkType = VTK_FLOAT;          break;
        case gdcm::PixelFormat::FLOAT64: vtkType = VTK_DOUBLE;         break;
        default: break;
    }

    // ── Build vtkImageData ──────────────────────────────────────────────────
    auto rawData = vtkSmartPointer<vtkImageData>::New();
    rawData->SetDimensions(static_cast<int>(cols),
                            static_cast<int>(rows),
                            static_cast<int>(nSlices));
    rawData->SetSpacing(xSp, ySp, zSp);
    rawData->SetOrigin(ox, oy, oz);
    rawData->AllocateScalars(vtkType, 1);

    // ── Fill slices (the slow part — reports progress) ──────────────────────
    const size_t sliceBytes = first.GetBufferLength();
    char* buf = static_cast<char*>(rawData->GetScalarPointer());

    for (unsigned int i = 0; i < nSlices; ++i) {
        gdcm::ImageReader reader;
        reader.SetFileName(sortedFiles[i].c_str());
        if (!reader.Read()) {
            std::memset(buf + i * sliceBytes, 0, sliceBytes);
        } else {
            reader.GetImage().GetBuffer(buf + i * sliceBytes);
        }
        if (progressCb)
            progressCb(static_cast<int>(i + 1), static_cast<int>(nSlices));
    }

    // ── Apply Rescale Slope / Intercept → Hounsfield Units ─────────────────
    double slope     = 1.0;
    double intercept = 0.0;
    if (const char* s = tagVal(sortedFiles[0], gdcm::Tag(0x0028, 0x1053))) slope     = std::atof(s);
    if (const char* s = tagVal(sortedFiles[0], gdcm::Tag(0x0028, 0x1052))) intercept = std::atof(s);

    auto ss = vtkSmartPointer<vtkImageShiftScale>::New();
    ss->SetInputData(rawData);
    ss->SetScale(slope);
    ss->SetShift(intercept / slope);
    ss->SetOutputScalarTypeToShort();
    ss->ClampOverflowOn();
    ss->Update();

    // ── Metadata ────────────────────────────────────────────────────────────
    auto safeStr = [](const char* s) -> std::string { return s ? s : ""; };
    m_meta.patientName       = safeStr(tagVal(sortedFiles[0], gdcm::Tag(0x0010, 0x0010)));
    m_meta.patientId         = safeStr(tagVal(sortedFiles[0], gdcm::Tag(0x0010, 0x0020)));
    m_meta.studyDate         = safeStr(tagVal(sortedFiles[0], gdcm::Tag(0x0008, 0x0020)));
    m_meta.studyInstanceUid  = safeStr(tagVal(sortedFiles[0], gdcm::Tag(0x0020, 0x000d)));
    m_meta.seriesInstanceUid = safeStr(tagVal(sortedFiles[0], gdcm::Tag(0x0020, 0x000e)));

    populateMetadata(ss->GetOutput());
    m_meta.valid = true;

    auto out = vtkSmartPointer<vtkImageData>::New();
    out->DeepCopy(ss->GetOutput());
    return out;
}
#endif // HAVE_GDCM (loadFromFilesGDCM)

// ─────────────────────────────────────────────────────────────────────────────
vtkSmartPointer<vtkImageData>
DicomVolumeLoader::loadWithVTK(const std::string& folderPath)
{
    auto reader = vtkSmartPointer<vtkDICOMImageReader>::New();
    reader->SetDirectoryName(folderPath.c_str());
    reader->GlobalWarningDisplayOff();
    reader->Update();

    if (reader->GetErrorCode() != 0) {
        m_meta.errorMessage =
            "vtkDICOMImageReader failed for: " + folderPath +
            "\nRebuild with -DUSE_GDCM=ON for compressed DICOM support.";
        return nullptr;
    }
    vtkImageData* raw = reader->GetOutput();
    if (!raw || raw->GetNumberOfPoints() == 0) {
        m_meta.errorMessage = "No valid pixel data in: " + folderPath;
        return nullptr;
    }
    auto safeStr = [](const char* s) { return s ? std::string(s) : std::string{}; };
    m_meta.patientName = safeStr(reader->GetPatientName());
    m_meta.patientId   = safeStr(reader->GetStudyID());
    populateMetadata(raw);
    m_meta.valid = true;
    auto out = vtkSmartPointer<vtkImageData>::New();
    out->DeepCopy(raw);
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
void DicomVolumeLoader::populateMetadata(vtkImageData* img)
{
    img->GetDimensions(m_meta.dimensions.data());
    img->GetSpacing(m_meta.spacing.data());
    img->GetOrigin(m_meta.origin.data());
    img->GetScalarRange(m_meta.scalarRange.data());
    m_meta.numSlices = m_meta.dimensions[2];
}
