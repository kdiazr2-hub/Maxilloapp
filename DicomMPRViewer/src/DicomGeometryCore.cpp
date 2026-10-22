#include "DicomGeometryCore.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace
{
bool plausible(double spacing)
{
    return std::isfinite(spacing) && spacing > 0.0 && spacing < 20.0;
}

// A reader that found no tag often reports 1 mm. That number is not a measurement.
bool measured(double spacing)
{
    return plausible(spacing) && std::abs(spacing - 1.0) > 1e-6;
}

double medianOf(std::vector<double> values)
{
    values.erase(std::remove_if(values.begin(), values.end(),
                                [](double value) { return !plausible(value); }),
                 values.end());
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

std::array<double, 3> sliceNormal(const std::array<double, 6>& iop)
{
    const std::array<double, 3> normal = {
        iop[1] * iop[5] - iop[2] * iop[4],
        iop[2] * iop[3] - iop[0] * iop[5],
        iop[0] * iop[4] - iop[1] * iop[3]};
    const double length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
    if (length <= 1e-9) return {0.0, 0.0, 1.0};
    return {normal[0] / length, normal[1] / length, normal[2] / length};
}
} // namespace

DicomSpacingDecision decideDicomSpacing(const DicomSpacingInput& input)
{
    DicomSpacingDecision decision;
    double x = plausible(input.pixelSpacingX) ? input.pixelSpacingX : 0.0;
    double y = plausible(input.pixelSpacingY) ? input.pixelSpacingY : 0.0;
    if (x <= 0.0 && measured(input.imageSpacingX)) x = input.imageSpacingX;
    if (y <= 0.0 && measured(input.imageSpacingY)) y = input.imageSpacingY;
    if (x <= 0.0) x = y;
    if (y <= 0.0) y = x;

    const double fromDeltas = medianOf(input.sliceDeltasMm);
    double z = fromDeltas;
    if (!plausible(z) && plausible(input.spacingBetweenSlices)) z = input.spacingBetweenSlices;
    if (!plausible(z) && plausible(input.sliceThickness)) z = input.sliceThickness;
    if (!plausible(z) && plausible(input.sliceThickness / 1000.0)) z = input.sliceThickness / 1000.0;
    if (!plausible(z) && measured(input.imageSpacingZ)) z = input.imageSpacingZ;

    std::vector<double> deltas = input.sliceDeltasMm;
    deltas.erase(std::remove_if(deltas.begin(), deltas.end(),
                                [](double value) { return !plausible(value); }),
                 deltas.end());
    if (deltas.size() >= 2 && plausible(fromDeltas)) {
        const auto ends = std::minmax_element(deltas.begin(), deltas.end());
        decision.sliceVariation = (*ends.second - *ends.first) / fromDeltas;
    }

    const bool tagTilt = std::isfinite(input.gantryTiltDegrees) && std::abs(input.gantryTiltDegrees) > 1.0;
    bool orientationTilt = false;
    if (input.hasImageOrientation) {
        const auto normal = sliceNormal(input.imageOrientation);
        orientationTilt = std::abs(normal[2]) < std::cos(1.0 * 3.14159265358979323846 / 180.0);
    }
    decision.gantryTilt = tagTilt || orientationTilt;

    if (!plausible(x) || !plausible(y) || !plausible(z)) {
        decision.status = DicomSpacingStatus::Blocked;
        decision.message = "No hay un espaciado fiable en este estudio. La segmentación automática queda "
                           "bloqueada. No se usará 1,0 mm.";
        return decision;
    }

    decision.spacingMm = {x, y, z};
    if (decision.sliceVariation > 0.01 || decision.gantryTilt) {
        decision.status = DicomSpacingStatus::OfferResample;
        std::ostringstream text;
        text.setf(std::ios::fixed);
        text.precision(1);
        if (decision.sliceVariation > 0.01)
            text << "El espaciado entre cortes varía un " << (decision.sliceVariation * 100.0) << " %. ";
        if (decision.gantryTilt)
            text << "La orientación de los cortes no es axial. ";
        text << "Se puede remuestrear a un espaciado uniforme de " << z << " mm. "
             << "Un remuestreo del volumen ya apilado no deshace la inclinación del gantry.";
        decision.message = text.str();
        return decision;
    }

    decision.status = DicomSpacingStatus::Reliable;
    return decision;
}
