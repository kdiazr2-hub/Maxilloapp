#include "GuidePlanCore.h"

#include "OsteotomyCore.h"

#include <QJsonArray>

namespace
{
QJsonArray pointJson(const std::array<double, 3>& p) { return QJsonArray{p[0], p[1], p[2]}; }

std::array<double, 3> pointFromJson(const QJsonArray& a, const std::array<double, 3>& fallback)
{
    return a.size() == 3 ? std::array<double, 3>{a[0].toDouble(), a[1].toDouble(), a[2].toDouble()} : fallback;
}
} // namespace

namespace GuidePlanCore
{
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

    QJsonArray holes;
    for (const GuideFixationHole& hole : plan.holes)
        holes.append(QJsonObject{{QStringLiteral("center"), pointJson(hole.center)},
                                 {QStringLiteral("axis"), pointJson(hole.axis)},
                                 {QStringLiteral("diameterMm"), hole.diameterMm}});

    return QJsonObject{
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
                     {QStringLiteral("bladeThicknessMm"), plan.design.slot.bladeThicknessMm},
                     {QStringLiteral("slotExtensionMm"), plan.design.slot.extensionMm},
                     {QStringLiteral("holeLengthMm"), plan.design.holeLengthMm},
                     {QStringLiteral("edgeMarginMm"), plan.design.edgeMarginMm}}},
        {QStringLiteral("contour"), GuideBaseCore::ContourToJson(plan.contour)},
        {QStringLiteral("slots"), slotArray},
        {QStringLiteral("holes"), holes}};
}

GuidePlan FromJson(const QJsonObject& object)
{
    GuidePlan plan;
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
    plan.design.slot.bladeThicknessMm =
        design.value(QStringLiteral("bladeThicknessMm")).toDouble(plan.design.slot.bladeThicknessMm);
    plan.design.slot.extensionMm = design.value(QStringLiteral("slotExtensionMm")).toDouble(plan.design.slot.extensionMm);
    plan.design.slot.smallestDetailMm = plan.design.base.smallestDetailMm;
    plan.design.holeLengthMm = design.value(QStringLiteral("holeLengthMm")).toDouble(plan.design.holeLengthMm);
    plan.design.edgeMarginMm = design.value(QStringLiteral("edgeMarginMm")).toDouble(plan.design.edgeMarginMm);

    plan.contour = GuideBaseCore::ContourFromJson(object.value(QStringLiteral("contour")).toArray());

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
    return plan;
}
}
