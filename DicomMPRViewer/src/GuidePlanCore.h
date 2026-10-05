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
#include "LeFortHoleCore.h"
#include "PlateCore.h"
#include "WrapCore.h"

#include <QJsonObject>
#include <QString>

#include <vector>

// Which guide is being designed; each takes its own models for the envelope.
enum class GuideType
{
    LeFort, // custom plates + cutting guide: plates on the planned bone, guide before repositioning
    Chin    // chin segment + mandible after the genioplasty
};

enum class GuideWorkflowStep
{
    Envelope = 0,
    PaintRight,
    PaintLeft,
    PaintBridge,
    Holes,
    Slots,
    Build,
    Complete
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
    GuideWorkflowStep workflowStep = GuideWorkflowStep::Envelope;
    int rightPaintEnd = 0; // exclusive paint index after the right support zone
    int leftPaintEnd = 0;  // exclusive paint index after the left support zone
    std::vector<GuideSlot> slotPlan; // not "slots": Qt defines that as a keyword macro
    std::vector<GuideFixationHole> holes;
    // Boolean tools. Imported figures reload from `sourcePath`; project-object copies reload from `sourceLabel`.
    // Curved tubes keep their three control points directly in the plan.
    std::vector<GuideFigure> figures;
    // Le Fort only: the patient-specific plates (on the planned bone) and the sleeves their predictive holes
    // put on the guide. Optional keys, so older plans load without them.
    std::vector<PlateDesign> plates;
    PlateParams plate;
    SleeveParams sleeve;
    // Le Fort only: the guide's own fixation holes, proposed by the app or moved by the surgeon, on the bone
    // before the cut. The plate phase takes its holes from here. Their support is not saved: it is judged again
    // whenever the guide is generated, since the movement may have changed. Optional key `lefortHoles`.
    std::vector<LeFortProposedHole> lefortHoles;
    // The surgeon's band heights, one per point of the Le Fort cut (pilar D, piriforme D, piriforme I, pilar I);
    // empty = the band comes from the movement. Optional key `bandHeights` (spec asistente-guia-lefort).
    std::vector<double> bandHeights;
    // The case number engraved on the guides. Optional key `caseLabel`.
    QString caseLabel;
};

namespace GuidePlanCore
{
// Object labels a guide type wraps.
std::vector<int> SourceLabelsFor(GuideType type);
QJsonObject ToJson(const GuidePlan& plan);
GuidePlan FromJson(const QJsonObject& object);
}
