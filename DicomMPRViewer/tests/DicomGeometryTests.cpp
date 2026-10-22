#include "DicomGeometryCore.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

DicomSpacingInput axial(double pixel, double slice)
{
    DicomSpacingInput input;
    input.pixelSpacingX = pixel;
    input.pixelSpacingY = pixel;
    input.sliceDeltasMm = {slice, slice, slice, slice};
    input.hasImageOrientation = true;
    input.imageOrientation = {1, 0, 0, 0, 1, 0};
    return input;
}

void testUniformSpacingIsUsedAsMeasured()
{
    const auto decision = decideDicomSpacing(axial(0.377, 0.700));
    require(decision.status == DicomSpacingStatus::Reliable, "a uniform axial series was not accepted");
    require(std::abs(decision.spacingMm[0] - 0.377) < 1e-9, "x spacing changed");
    require(std::abs(decision.spacingMm[2] - 0.700) < 1e-9, "z spacing is not the median delta");
    require(!decision.gantryTilt, "an axial IOP was called tilted");
}

void testSliceVariationOffersAResample()
{
    auto input = axial(0.4, 0.70);
    input.sliceDeltasMm = {0.70, 0.70, 0.72, 0.70};
    const auto decision = decideDicomSpacing(input);
    require(decision.status == DicomSpacingStatus::OfferResample, "a 2 % slice variation did not ask to resample");
    require(decision.sliceVariation > 0.01, "the variation was not reported");
    require(std::abs(decision.spacingMm[2] - 0.70) < 1e-9, "the offered spacing is not the measured median");
    require(std::abs(decision.spacingMm[2] - 1.0) > 1e-6, "the variation path substituted 1 mm");
}

void testGantryTiltOffersAResample()
{
    auto input = axial(0.4, 0.6);
    input.gantryTiltDegrees = 8.0;
    const auto decision = decideDicomSpacing(input);
    require(decision.status == DicomSpacingStatus::OfferResample, "a gantry tilt tag did not ask to resample");
    require(decision.gantryTilt, "the tilt flag stayed off");

    auto oblique = axial(0.4, 0.6);
    oblique.gantryTiltDegrees = 0.0;
    oblique.imageOrientation = {1, 0, 0, 0, std::cos(0.2), -std::sin(0.2)};
    const auto fromIop = decideDicomSpacing(oblique);
    require(fromIop.gantryTilt && fromIop.status == DicomSpacingStatus::OfferResample,
            "a tilted IOP was treated as axial");
}

void testMissingSpacingBlocksAndDoesNotUseOneMillimetre()
{
    const auto decision = decideDicomSpacing({});
    require(decision.status == DicomSpacingStatus::Blocked, "missing spacing did not block automatic segmentation");
    require(decision.spacingMm[0] == 0.0 && decision.spacingMm[1] == 0.0 && decision.spacingMm[2] == 0.0,
            "missing spacing was filled in");
    for (double value : decision.spacingMm)
        require(std::abs(value - 1.0) > 1e-9, "missing spacing used 1 mm");

    DicomSpacingInput readerDefault;
    readerDefault.imageSpacingX = 1.0;
    readerDefault.imageSpacingY = 1.0;
    readerDefault.imageSpacingZ = 1.0;
    const auto ignored = decideDicomSpacing(readerDefault);
    require(ignored.status == DicomSpacingStatus::Blocked, "a reader's 1 mm default was treated as measured");
    require(ignored.spacingMm[2] != 1.0, "the 1 mm reader default became the slice spacing");
}
} // namespace

int main()
{
    const std::pair<const char*, void (*)()> tests[] = {
        {"uniform spacing is used as measured", testUniformSpacingIsUsedAsMeasured},
        {"slice variation offers a resample", testSliceVariationOffersAResample},
        {"gantry tilt offers a resample", testGantryTiltOffersAResample},
        {"missing spacing blocks and does not use 1 mm", testMissingSpacingBlocksAndDoesNotUseOneMillimetre},
    };
    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.second();
            std::cout << "PASS " << test.first << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << test.first << ": " << error.what() << '\n';
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
