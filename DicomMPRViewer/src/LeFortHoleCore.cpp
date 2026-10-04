#include "LeFortHoleCore.h"

namespace LeFortHoleCore
{
LeFortHoleSupport Support(const std::array<double, 3>&, const std::array<double, 3>&, const LeFortHoleContext&)
{
    LeFortHoleSupport support;
    support.verdict = LeFortSupportVerdict::Ok;
    return support;
}
}
