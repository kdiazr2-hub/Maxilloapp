#pragma once

#include <array>
#include <functional>
#include <string>
#include <vector>

#include <vtkImageData.h>
#include <vtkSmartPointer.h>

// ─────────────────────────────────────────────────────────────────────────────
// DicomVolumeLoader
//
// Abstraction layer for DICOM series loading.
//
// CURRENT BACKEND: GDCM (via vtkGDCMImageReader)
//   • Supports all standard transfer syntaxes including:
//     - Uncompressed (Implicit/Explicit Little/Big Endian)
//     - JPEG Baseline / Extended
//     - JPEG2000 (1.2.840.10008.1.2.4.90 / .91)
//     - JPEG-LS  (1.2.840.10008.1.2.4.80 / .81)
//     - RLE Lossless
//
// FALLBACK: vtkDICOMImageReader
//   Used if GDCM is not available at compile time.
//   Does NOT support JPEG2000 or JPEG-LS compressed files.
//
// TO SWITCH BACKEND: change only this class — no other files need modification.
// ─────────────────────────────────────────────────────────────────────────────

class DicomVolumeLoader
{
public:
    struct VolumeMetadata
    {
        std::array<int,    3> dimensions   = {0, 0, 0};
        std::array<double, 3> spacing      = {1, 1, 1};
        std::array<double, 3> origin       = {0, 0, 0};
        std::array<double, 2> scalarRange  = {0, 0};
        int                   numSlices    = 0;
        std::string           patientId;
        std::string           patientName;
        std::string           studyDate;
        std::string           studyInstanceUid;
        std::string           seriesInstanceUid;
        bool                  valid        = false;
        std::string           errorMessage;
    };

    // Called after each slice during loadFromFiles(); args: (done, total).
    using ProgressCallback = std::function<void(int, int)>;

    DicomVolumeLoader() = default;

    // Load a DICOM folder (self-indexing, synchronous, backward-compatible).
    vtkSmartPointer<vtkImageData> load(const std::string& folderPath);

    // Load from a pre-sorted file list (used by AsyncDicomLoader).
    // progressCb is called after every slice so the caller can update a
    // progress bar.  Pass nullptr to skip progress reporting.
    vtkSmartPointer<vtkImageData> loadFromFiles(
        const std::vector<std::string>& sortedFiles,
        ProgressCallback progressCb = nullptr);

    const VolumeMetadata& metadata() const { return m_meta; }

private:
    vtkSmartPointer<vtkImageData> loadWithGDCM(const std::string& folderPath);
    vtkSmartPointer<vtkImageData> loadWithVTK(const std::string& folderPath);
#ifdef HAVE_GDCM
    vtkSmartPointer<vtkImageData> loadFromFilesGDCM(
        const std::vector<std::string>& sortedFiles,
        ProgressCallback progressCb);
#endif
    void populateMetadata(vtkImageData* img);

    VolumeMetadata m_meta;
};
