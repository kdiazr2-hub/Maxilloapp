#include "GuidePlanCore.h"

#include "ObjectLabels.h"
#include "OsteotomyCore.h"

#include <QJsonArray>

#include <algorithm>

namespace
{
QJsonArray pointJson(const std::array<double, 3>& p) { return QJsonArray{p[0], p[1], p[2]}; }

std::array<double, 3> pointFromJson(const QJsonArray& a, const std::array<double, 3>& fallback)
{
    return a.size() == 3 ? std::array<double, 3>{a[0].toDouble(), a[1].toDouble(), a[2].toDouble()} : fallback;
}

QString shapeName(GuideFigureShape shape)
{
    switch (shape) {
    case GuideFigureShape::Cylinder: return QStringLiteral("cylinder");
    case GuideFigureShape::Box: return QStringLiteral("box");
    case GuideFigureShape::Sphere: return QStringLiteral("sphere");
    case GuideFigureShape::Mesh: return QStringLiteral("mesh");
    case GuideFigureShape::CurvedTube: return QStringLiteral("curvedTube");
    }
    return QStringLiteral("cylinder");
}

GuideFigureShape shapeFromName(const QString& name)
{
    if (name == QStringLiteral("box"))
        return GuideFigureShape::Box;
    if (name == QStringLiteral("sphere"))
        return GuideFigureShape::Sphere;
    if (name == QStringLiteral("mesh"))
        return GuideFigureShape::Mesh;
    if (name == QStringLiteral("curvedTube"))
        return GuideFigureShape::CurvedTube;
    return GuideFigureShape::Cylinder;
}
// The Le Fort guide's holes: readable names in the file, and nothing guessed when a name is unknown.
QString pillarKey(LeFortPillar pillar)
{
    switch (pillar) {
    case LeFortPillar::PillarRight: return QStringLiteral("pillarRight");
    case LeFortPillar::PiriformRight: return QStringLiteral("piriformRight");
    case LeFortPillar::PiriformLeft: return QStringLiteral("piriformLeft");
    case LeFortPillar::PillarLeft: return QStringLiteral("pillarLeft");
    }
    return {};
}
bool pillarFromKey(const QString& key, LeFortPillar& pillar)
{
    for (const LeFortPillar candidate : {LeFortPillar::PillarRight, LeFortPillar::PiriformRight,
                                         LeFortPillar::PiriformLeft, LeFortPillar::PillarLeft})
        if (key == pillarKey(candidate)) {
            pillar = candidate;
            return true;
        }
    return false;
}
} // namespace

namespace GuidePlanCore
{
std::vector<int> SourceLabelsFor(GuideType type)
{
    return type == GuideType::Chin ? std::vector<int>{kGenioSegmentLabel, kGenioBodyLabel}
                                   : std::vector<int>{kLeFortSegLabel, kLeFortCranialLabel};
}

QJsonObject ToJson(const GuidePlan& plan)
{
    QJsonArray sources;
    for (int label : plan.sourceLabels)
        sources.append(label);

    QJsonArray slotArray;
    for (const GuideSlot& slot : plan.slotPlan) {
        QJsonObject o{{QStringLiteral("path"), OsteotomyCore::PathToJson(slot.path)}};
        if (slot.hasExtent) {
            o[QStringLiteral("start")] = pointJson(slot.start);
            o[QStringLiteral("end")] = pointJson(slot.end);
        }
        slotArray.append(o);
    }

    QJsonArray figures;
    for (const GuideFigure& figure : plan.figures) {
        QJsonArray matrix;
        for (double v : figure.matrix)
            matrix.append(v);
        QJsonObject o{{QStringLiteral("shape"), shapeName(figure.shape)},
                      {QStringLiteral("operation"),
                       figure.operation == GuideFigureOperation::Add ? QStringLiteral("add") : QStringLiteral("subtract")},
                      {QStringLiteral("matrix"), matrix},
                      {QStringLiteral("diameterMm"), figure.diameterMm},
                      {QStringLiteral("lengthMm"), figure.lengthMm},
                      {QStringLiteral("widthMm"), figure.widthMm},
                      {QStringLiteral("heightMm"), figure.heightMm},
                      {QStringLiteral("depthMm"), figure.depthMm}};
        if (!figure.sourcePath.isEmpty())
            o[QStringLiteral("sourcePath")] = figure.sourcePath;
        if (figure.sourceLabel != 0)
            o[QStringLiteral("sourceLabel")] = figure.sourceLabel;
        if (!figure.controlPoints.empty()) {
            QJsonArray points;
            for (const auto& point : figure.controlPoints)
                points.append(pointJson(point));
            o[QStringLiteral("controlPoints")] = points;
        }
        figures.append(o);
    }

    QJsonArray holes;
    for (const GuideFixationHole& hole : plan.holes)
        holes.append(QJsonObject{{QStringLiteral("center"), pointJson(hole.center)},
                                 {QStringLiteral("axis"), pointJson(hole.axis)},
                                 {QStringLiteral("diameterMm"), hole.diameterMm}});

    QJsonObject out{
        {QStringLiteral("type"), plan.type == GuideType::Chin ? QStringLiteral("chin") : QStringLiteral("leFort")},
        {QStringLiteral("name"), plan.name},
        {QStringLiteral("sourceLabels"), sources},
        {QStringLiteral("wrap"), QJsonObject{{QStringLiteral("gapClosingMm"), plan.wrap.gapClosingMm},
                                             {QStringLiteral("smallestDetailMm"), plan.wrap.smallestDetailMm},
                                             {QStringLiteral("smoothingIterations"), plan.wrap.smoothingIterations}}},
        {QStringLiteral("design"),
         QJsonObject{{QStringLiteral("thicknessMm"), plan.design.base.thicknessMm},
                     {QStringLiteral("clearanceMm"), plan.design.base.clearanceMm},
                     {QStringLiteral("smallestDetailMm"), plan.design.base.smallestDetailMm},
                     {QStringLiteral("smoothingIterations"), plan.design.base.smoothingIterations},
                     {QStringLiteral("cornerRadiusMm"), plan.design.base.cornerRadiusMm},
                     {QStringLiteral("edgeTaperMm"), plan.design.base.edgeTaperMm},
                     {QStringLiteral("edgeThicknessFraction"), plan.design.base.edgeThicknessFraction},
                     {QStringLiteral("edgeRoundMm"), plan.design.base.edgeRoundMm},
                     {QStringLiteral("bladeThicknessMm"), plan.design.slot.bladeThicknessMm},
                     {QStringLiteral("slotExtensionMm"), plan.design.slot.extensionMm},
                     {QStringLiteral("holeLengthMm"), plan.design.holeLengthMm},
                     {QStringLiteral("edgeMarginMm"), plan.design.edgeMarginMm}}},
        {QStringLiteral("contour"), GuideBaseCore::ContourToJson(plan.contour)},
        {QStringLiteral("paint"), GuideBaseCore::PaintToJson(plan.paint)},
        {QStringLiteral("workflowStep"), static_cast<int>(plan.workflowStep)},
        {QStringLiteral("rightPaintEnd"), plan.rightPaintEnd},
        {QStringLiteral("leftPaintEnd"), plan.leftPaintEnd},
        {QStringLiteral("slots"), slotArray},
        {QStringLiteral("holes"), holes},
        {QStringLiteral("figures"), figures}};
    if (!plan.plates.empty()) {
        QJsonArray plates;
        for (const PlateDesign& plate : plan.plates)
            plates.append(PlateCore::ToJson(plate));
        out[QStringLiteral("plates")] = plates;
    }
    if (!plan.lefortHoles.empty()) {
        QJsonArray holes;
        for (const LeFortProposedHole& hole : plan.lefortHoles)
            holes.append(QJsonObject{
                {QStringLiteral("center"), pointJson(hole.center)},
                {QStringLiteral("axis"), pointJson(hole.axis)},
                {QStringLiteral("pillar"), pillarKey(hole.pillar)},
                {QStringLiteral("side"), hole.side == LeFortCutSide::Cranial ? QStringLiteral("cranial") : QStringLiteral("segment")},
                {QStringLiteral("origin"), hole.origin == LeFortHoleOrigin::Manual ? QStringLiteral("manual") : QStringLiteral("auto")}});
        out[QStringLiteral("lefortHoles")] = holes;
    }
    if (!plan.bandHeights.empty()) {
        QJsonArray heights;
        for (const double height : plan.bandHeights)
            heights.append(height);
        out[QStringLiteral("bandHeights")] = heights;
    }
    if (!plan.caseLabel.isEmpty())
        out[QStringLiteral("caseLabel")] = plan.caseLabel;
    if (plan.assistant)
        out[QStringLiteral("assistant")] = true;
    out[QStringLiteral("plate")] = PlateCore::ParamsToJson(plan.plate);
    if (!plan.foramina.empty()) {
        QJsonArray foramina;
        for (const GuideForamen& foramen : plan.foramina)
            foramina.append(QJsonObject{{QStringLiteral("center"), pointJson(foramen.center)},
                                        {QStringLiteral("side"), foramen.right ? QStringLiteral("right") : QStringLiteral("left")}});
        out[QStringLiteral("foramina")] = foramina;
    }
    out[QStringLiteral("sleeve")] = QJsonObject{{QStringLiteral("screwDiameterMm"), plan.sleeve.screwDiameterMm},
                                                {QStringLiteral("boreDiameterMm"), plan.sleeve.boreDiameterMm},
                                                {QStringLiteral("outerDiameterMm"), plan.sleeve.outerDiameterMm},
                                                {QStringLiteral("heightMm"), plan.sleeve.heightMm}};
    return out;
}

GuidePlan FromJson(const QJsonObject& object)
{
    GuidePlan plan;
    plan.type = object.value(QStringLiteral("type")).toString() == QStringLiteral("chin") ? GuideType::Chin
                                                                                          : GuideType::LeFort;
    plan.name = object.value(QStringLiteral("name")).toString();
    for (const QJsonValue& label : object.value(QStringLiteral("sourceLabels")).toArray())
        plan.sourceLabels.push_back(label.toInt());

    const QJsonObject wrap = object.value(QStringLiteral("wrap")).toObject();
    plan.wrap.gapClosingMm = wrap.value(QStringLiteral("gapClosingMm")).toDouble(plan.wrap.gapClosingMm);
    plan.wrap.smallestDetailMm = wrap.value(QStringLiteral("smallestDetailMm")).toDouble(plan.wrap.smallestDetailMm);
    plan.wrap.smoothingIterations =
        wrap.value(QStringLiteral("smoothingIterations")).toInt(plan.wrap.smoothingIterations);

    const QJsonObject design = object.value(QStringLiteral("design")).toObject();
    plan.design.base.thicknessMm = design.value(QStringLiteral("thicknessMm")).toDouble(plan.design.base.thicknessMm);
    plan.design.base.clearanceMm = design.value(QStringLiteral("clearanceMm")).toDouble(plan.design.base.clearanceMm);
    plan.design.base.smallestDetailMm =
        design.value(QStringLiteral("smallestDetailMm")).toDouble(plan.design.base.smallestDetailMm);
    plan.design.base.smoothingIterations =
        design.value(QStringLiteral("smoothingIterations")).toInt(plan.design.base.smoothingIterations);
    plan.design.base.cornerRadiusMm =
        design.value(QStringLiteral("cornerRadiusMm")).toDouble(plan.design.base.cornerRadiusMm);
    plan.design.base.edgeTaperMm = design.value(QStringLiteral("edgeTaperMm")).toDouble(plan.design.base.edgeTaperMm);
    plan.design.base.edgeThicknessFraction =
        design.value(QStringLiteral("edgeThicknessFraction")).toDouble(plan.design.base.edgeThicknessFraction);
    plan.design.base.edgeRoundMm = design.value(QStringLiteral("edgeRoundMm")).toDouble(plan.design.base.edgeRoundMm);
    plan.design.slot.bladeThicknessMm =
        design.value(QStringLiteral("bladeThicknessMm")).toDouble(plan.design.slot.bladeThicknessMm);
    plan.design.slot.extensionMm = design.value(QStringLiteral("slotExtensionMm")).toDouble(plan.design.slot.extensionMm);
    plan.design.slot.smallestDetailMm = plan.design.base.smallestDetailMm;
    plan.design.holeLengthMm = design.value(QStringLiteral("holeLengthMm")).toDouble(plan.design.holeLengthMm);
    plan.design.edgeMarginMm = design.value(QStringLiteral("edgeMarginMm")).toDouble(plan.design.edgeMarginMm);

    plan.contour = GuideBaseCore::ContourFromJson(object.value(QStringLiteral("contour")).toArray());
    plan.paint = GuideBaseCore::PaintFromJson(object.value(QStringLiteral("paint")).toArray());
    const int workflowStep = object.value(QStringLiteral("workflowStep")).toInt(
        static_cast<int>(GuideWorkflowStep::Envelope));
    plan.workflowStep = static_cast<GuideWorkflowStep>(
        std::clamp(workflowStep, static_cast<int>(GuideWorkflowStep::Envelope),
                   static_cast<int>(GuideWorkflowStep::Complete)));
    plan.rightPaintEnd = std::clamp(object.value(QStringLiteral("rightPaintEnd")).toInt(), 0,
                                   static_cast<int>(plan.paint.size()));
    plan.leftPaintEnd = std::clamp(object.value(QStringLiteral("leftPaintEnd")).toInt(),
                                  plan.rightPaintEnd, static_cast<int>(plan.paint.size()));

    for (const QJsonValue& value : object.value(QStringLiteral("slots")).toArray()) {
        const QJsonObject o = value.toObject();
        GuideSlot slot;
        slot.path = OsteotomyCore::PathFromJson(o.value(QStringLiteral("path")).toObject());
        if (o.contains(QStringLiteral("start")) && o.contains(QStringLiteral("end"))) {
            slot.start = pointFromJson(o.value(QStringLiteral("start")).toArray(), slot.start);
            slot.end = pointFromJson(o.value(QStringLiteral("end")).toArray(), slot.end);
            slot.hasExtent = true;
        }
        plan.slotPlan.push_back(slot);
    }

    for (const QJsonValue& value : object.value(QStringLiteral("holes")).toArray()) {
        const QJsonObject o = value.toObject();
        GuideFixationHole hole;
        hole.center = pointFromJson(o.value(QStringLiteral("center")).toArray(), hole.center);
        hole.axis = pointFromJson(o.value(QStringLiteral("axis")).toArray(), hole.axis);
        hole.diameterMm = o.value(QStringLiteral("diameterMm")).toDouble(hole.diameterMm);
        plan.holes.push_back(hole);
    }

    for (const QJsonValue& value : object.value(QStringLiteral("figures")).toArray()) {
        const QJsonObject o = value.toObject();
        GuideFigure figure;
        figure.shape = shapeFromName(o.value(QStringLiteral("shape")).toString());
        figure.operation = o.value(QStringLiteral("operation")).toString() == QStringLiteral("add")
                               ? GuideFigureOperation::Add
                               : GuideFigureOperation::Subtract;
        const QJsonArray matrix = o.value(QStringLiteral("matrix")).toArray();
        if (matrix.size() == 16)
            for (int i = 0; i < 16; ++i)
                figure.matrix[static_cast<size_t>(i)] = matrix[i].toDouble();
        figure.diameterMm = o.value(QStringLiteral("diameterMm")).toDouble(figure.diameterMm);
        figure.lengthMm = o.value(QStringLiteral("lengthMm")).toDouble(figure.lengthMm);
        figure.widthMm = o.value(QStringLiteral("widthMm")).toDouble(figure.widthMm);
        figure.heightMm = o.value(QStringLiteral("heightMm")).toDouble(figure.heightMm);
        figure.depthMm = o.value(QStringLiteral("depthMm")).toDouble(figure.depthMm);
        figure.sourcePath = o.value(QStringLiteral("sourcePath")).toString();
        figure.sourceLabel = o.value(QStringLiteral("sourceLabel")).toInt();
        for (const QJsonValue& point : o.value(QStringLiteral("controlPoints")).toArray())
            figure.controlPoints.push_back(pointFromJson(point.toArray(), {0.0, 0.0, 0.0}));
        plan.figures.push_back(figure);
    }
    for (const QJsonValue& value : object.value(QStringLiteral("plates")).toArray())
        plan.plates.push_back(PlateCore::FromJson(value.toObject()));
    for (const QJsonValue& value : object.value(QStringLiteral("lefortHoles")).toArray()) {
        const QJsonObject o = value.toObject();
        const QString side = o.value(QStringLiteral("side")).toString();
        const QJsonArray center = o.value(QStringLiteral("center")).toArray();
        LeFortProposedHole hole;
        if (!pillarFromKey(o.value(QStringLiteral("pillar")).toString(), hole.pillar) ||
            (side != QStringLiteral("cranial") && side != QStringLiteral("segment")) || center.size() != 3)
            continue; // a hole the file does not describe is left out, not guessed
        hole.side = side == QStringLiteral("cranial") ? LeFortCutSide::Cranial : LeFortCutSide::Segment;
        hole.center = pointFromJson(center, {0.0, 0.0, 0.0});
        hole.axis = pointFromJson(o.value(QStringLiteral("axis")).toArray(), {0.0, 0.0, 1.0});
        hole.origin = o.value(QStringLiteral("origin")).toString() == QStringLiteral("manual") ? LeFortHoleOrigin::Manual
                                                                                              : LeFortHoleOrigin::Auto;
        plan.lefortHoles.push_back(hole);
    }
    for (const QJsonValue& value : object.value(QStringLiteral("bandHeights")).toArray())
        plan.bandHeights.push_back(value.toDouble());
    plan.caseLabel = object.value(QStringLiteral("caseLabel")).toString();
    plan.assistant = object.value(QStringLiteral("assistant")).toBool(false);
    for (const QJsonValue& value : object.value(QStringLiteral("foramina")).toArray()) {
        const QJsonObject o = value.toObject();
        const QJsonArray center = o.value(QStringLiteral("center")).toArray();
        if (center.size() != 3)
            continue;
        plan.foramina.push_back({pointFromJson(center, {0.0, 0.0, 0.0}),
                                 o.value(QStringLiteral("side")).toString() != QStringLiteral("left")});
    }
    plan.plate = PlateCore::ParamsFromJson(object.value(QStringLiteral("plate")).toObject());
    const QJsonObject sleeve = object.value(QStringLiteral("sleeve")).toObject();
    plan.sleeve.boreDiameterMm = sleeve.value(QStringLiteral("boreDiameterMm")).toDouble(plan.sleeve.boreDiameterMm);
    // Saved before the screw was chosen: a 2.0 mm screw and its pilot drill, whatever bore the plan had (it came
    // out at 2.0 mm on the real case and the screw did not bite; user's request, 2026-10-06).
    if (sleeve.contains(QStringLiteral("screwDiameterMm"))) {
        plan.sleeve.screwDiameterMm = sleeve.value(QStringLiteral("screwDiameterMm")).toDouble(2.0);
    } else {
        plan.sleeve.screwDiameterMm = 2.0;
        plan.sleeve.boreDiameterMm = PlateCore::PilotDrillFor(2.0);
    }
    plan.sleeve.outerDiameterMm = sleeve.value(QStringLiteral("outerDiameterMm")).toDouble(plan.sleeve.outerDiameterMm);
    plan.sleeve.heightMm = sleeve.value(QStringLiteral("heightMm")).toDouble(plan.sleeve.heightMm);
    return plan;
}
}
