#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// GuideEngraveCore
//
// Text in relief on a guide (spec asistente-guia-lefort: the case number and DER/IZQ, as on the user's printed
// guides). The text is VTK's font (`vtkVectorText`) rasterised and stacked into a closed solid, sunk into the guide's
// wall and standing `reliefMm` proud of it, and handed to `GuideDesignCore` as an added mesh figure, so it is
// carved into the same field as everything else.
//
// No Qt Widgets.
// ─────────────────────────────────────────────────────────────────────────────

#include "GuideDesignCore.h"

#include <QString>
#include <vtkSmartPointer.h>

#include <array>

class vtkPolyData;

namespace GuideEngraveCore
{
// The text as a closed solid in its own frame: centred on the origin in x (reading direction) and y (up),
// `heightMm` tall, from z = −`sinkMm` (inside the wall) to z = +`reliefMm` (proud of it).
vtkSmartPointer<vtkPolyData> TextSolid(const QString& text, double heightMm, double reliefMm, double sinkMm = 0.8);
// How wide the text comes out at that height.
double TextWidth(const QString& text, double heightMm);
// The text as an added mesh figure at `center` on the guide's outer face: read along `readingAxis`, standing
// along `outward`.
GuideFigure TextFigure(const QString& text, const std::array<double, 3>& center, const std::array<double, 3>& readingAxis,
                       const std::array<double, 3>& outward, double heightMm, double reliefMm);
}
