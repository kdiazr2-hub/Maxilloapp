#pragma once

#include <array>
#include <string>
#include <vector>

// Slice spacing taken from DICOM tags, in millimetres. A missing tag stays 0.
// imageSpacing* is the spacing a reader already attached to the pixels. A value of
// exactly 1 mm there is ignored: that is the silent default this decision exists to stop.
struct DicomSpacingInput
{
    std::vector<double> sliceDeltasMm;
    double pixelSpacingX = 0.0;
    double pixelSpacingY = 0.0;
    double spacingBetweenSlices = 0.0;
    double sliceThickness = 0.0;
    double gantryTiltDegrees = 0.0;
    std::array<double, 6> imageOrientation = {};
    bool hasImageOrientation = false;
    double imageSpacingX = 0.0;
    double imageSpacingY = 0.0;
    double imageSpacingZ = 0.0;
};

enum class DicomSpacingStatus
{
    Reliable,
    OfferResample,
    Blocked
};

struct DicomSpacingDecision
{
    DicomSpacingStatus status = DicomSpacingStatus::Blocked;
    std::array<double, 3> spacingMm = {0.0, 0.0, 0.0}; // x, y, z. Never a silent 1 mm.
    double sliceVariation = 0.0;                       // (max - min) / median of the slice deltas
    bool gantryTilt = false;
    std::string message;
};

// Reliable spacing is used as measured. Variation above 1 % or a tilted gantry / non-axial
// orientation asks for a resample and still returns the measured spacing. With nothing
// reliable to measure, automatic segmentation is blocked and the spacing stays 0.
DicomSpacingDecision decideDicomSpacing(const DicomSpacingInput& input);
