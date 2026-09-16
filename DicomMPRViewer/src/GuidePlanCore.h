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

struct GuidePlan
{
    QString name;
    std::vector<int> sourceLabels; // object labels the wrap was built from
    WrapParams wrap;
    GuideDesignParams design;
    GuideContour contour;
    std::vector<GuideSlot> slotPlan; // not "slots": Qt defines that as a keyword macro
    std::vector<GuideFixationHole> holes;
};

namespace GuidePlanCore
{
QJsonObject ToJson(const GuidePlan& plan);
GuidePlan FromJson(const QJsonObject& object);
}
