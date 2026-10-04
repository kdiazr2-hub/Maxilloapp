#include "LeFortMotionCore.h"

namespace LeFortMotionCore
{
LeFortBandProfile Band(const OsteotomyPath&, const std::array<double, 16>&, const LeFortBandParams&)
{
    return {};
}

double CutLength(const OsteotomyPath&)
{
    return 0.0;
}

std::array<double, 3> PointAlongCut(const OsteotomyPath&, double)
{
    return {0.0, 0.0, 0.0};
}
}
