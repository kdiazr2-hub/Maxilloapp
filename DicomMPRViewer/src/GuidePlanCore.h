#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// GuidePlanCore
//
// Everything the user decided about a surgical guide, as plain data: which
// models the wrap was built from, the marked region, which osteotomies get a
// slot and where its ends were placed, the fixation holes and the parameters.
// The meshes are not in here — they are rebuilt from the plan — so it saves and
// reloads as one optional `guidesPlan` key in the project.
// No Qt Widgets.
// ─────────────────────────────────────────────────────────────────────────────

#include "GuideDesignCore.h"
#include "WrapCore.h"

#include <QJsonObject>
#include <QString>

#include <vector>

// Which guide is being designed; each takes its own models for the envelope.
enum class GuideType
{
    LeFort, // Le Fort I segment + cranial base, in their planned position
    Chin    // chin segment + mandible after the genioplasty
};

struct GuidePlan
{
    GuideType type = GuideType::LeFort;
    QString name;
    std::vector<int> sourceLabels; // object labels the wrap was built from
    WrapParams wrap;
    GuideDesignParams design;
    GuideContour contour;   // region marked with points (older plans)
    GuideBrushPaint paint;  // region painted with the brush; used when not empty
    std::vector<GuideSlot> slotPlan; // not "slots": Qt defines that as a keyword macro
    std::vector<GuideFixationHole> holes;
    // Boolean tools. Imported figures keep only their file here: the UI reloads the mesh from `sourcePath`.
    std::vector<GuideFigure> figures;
};

namespace GuidePlanCore
{
// Object labels a guide type wraps.
std::vector<int> SourceLabelsFor(GuideType type);
QJsonObject ToJson(const GuidePlan& plan);
GuidePlan FromJson(const QJsonObject& object);
}
