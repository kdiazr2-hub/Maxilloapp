#include "GuidePlanCore.h"

#include "OsteotomyCore.h"
#include "SplintTestGeometry.h"

#include <QJsonDocument>

#include <cmath>
#include <functional>
#include <iostream>
#include <string>

namespace
{
using namespace splinttest;

GuidePlan samplePlan()
{
    GuidePlan plan;
    plan.name = QStringLiteral("Guía Le Fort");
    plan.sourceLabels = {206, 203};
    plan.wrap.gapClosingMm = 1.8;
    plan.wrap.smallestDetailMm = 0.28;
    plan.design.base.thicknessMm = 2.6;
    plan.design.base.clearanceMm = 0.15;
    plan.design.slot.bladeThicknessMm = 0.8;
    plan.design.edgeMarginMm = 2.5;
    plan.design.holeLengthMm = 24.0;
    plan.contour = {{-15.0, 0.0, 2.0}, {15.0, 0.0, 2.0}, {15.0, 0.0, 18.0}, {-15.0, 0.0, 18.0}};

    GuideSlot placed;
    placed.path = OsteotomyCore::LeFortPath({{{-10.0, 5.0, 9.4}, {10.0, 5.0, 9.6}, {-20.0, -5.0, 9.2},
                                              {20.0, -5.0, 9.3}}});
    placed.start = {-5.0, 1.4, 9.3};
    placed.end = {5.0, 1.4, 9.5};
    placed.hasExtent = true;
    GuideSlot wholeRegion;
    wholeRegion.path = placed.path;
    plan.slotPlan = {placed, wholeRegion};

    plan.holes = {{{-10.0, 0.0, 14.0}, {0.0, 1.0, 0.0}, 2.0}, {{10.0, 0.0, 14.0}, {0.0, 1.0, 0.0}, 2.5}};

    plan.type = GuideType::Chin;
    plan.design.base.cornerRadiusMm = 4.5;
    plan.design.base.edgeTaperMm = 5.5;
    plan.design.base.edgeThicknessFraction = 0.35;
    plan.design.base.edgeRoundMm = 0.8;
    GuideFigure box;
    box.shape = GuideFigureShape::Box;
    box.operation = GuideFigureOperation::Subtract;
    box.widthMm = 8.0;
    box.heightMm = 0.8;
    box.depthMm = 10.0;
    box.matrix = GuideDesignCore::FrameAt({1.0, 2.0, 3.0}, {0.0, 1.0, 0.0});
    GuideFigure stl;
    stl.shape = GuideFigureShape::Mesh;
    stl.operation = GuideFigureOperation::Add;
    stl.sourcePath = QStringLiteral("C:/casos/tope.stl");
    stl.sourceLabel = 215;
    GuideFigure tube;
    tube.shape = GuideFigureShape::CurvedTube;
    tube.operation = GuideFigureOperation::Add;
    tube.diameterMm = 3.2;
    tube.controlPoints = {{{0.0, 0.0, 0.0}, {2.0, 4.0, 6.0}, {5.0, 7.0, 8.0}}};
    plan.figures = {box, stl, tube};
    plan.paint = {{{1.0, 2.0, 3.0}, 4.0, false}, {{1.5, 2.0, 3.0}, 2.5, true}};
    plan.workflowStep = GuideWorkflowStep::Holes;
    plan.rightPaintEnd = 1;
    plan.leftPaintEnd = 2;
    return plan;
}

void testRoundTrip()
{
    const GuidePlan plan = samplePlan();
    const GuidePlan back = GuidePlanCore::FromJson(GuidePlanCore::ToJson(plan));

    require(back.name == plan.name && back.sourceLabels == plan.sourceLabels, "name or sources lost");
    require(std::abs(back.wrap.gapClosingMm - 1.8) < 1e-9 && std::abs(back.wrap.smallestDetailMm - 0.28) < 1e-9,
            "wrap parameters lost");
    require(std::abs(back.design.base.thicknessMm - 2.6) < 1e-9 &&
                std::abs(back.design.base.clearanceMm - 0.15) < 1e-9 &&
                std::abs(back.design.slot.bladeThicknessMm - 0.8) < 1e-9 &&
                std::abs(back.design.edgeMarginMm - 2.5) < 1e-9 &&
                std::abs(back.design.holeLengthMm - 24.0) < 1e-9,
            "design parameters lost");
    require(back.contour == plan.contour, "the marked region was not kept");

    require(back.slotPlan.size() == 2, "the slots were lost");
    require(back.slotPlan[0].hasExtent && back.slotPlan[0].start == plan.slotPlan[0].start &&
                back.slotPlan[0].end == plan.slotPlan[0].end,
            "the placed ends of the slot were lost");
    require(!back.slotPlan[1].hasExtent, "a slot without ends came back with them");
    // The osteotomy comes back as the same cut: same field at the landmarks.
    for (const OstPoint3& point : {OstPoint3{-10.0, 5.0, 9.4}, OstPoint3{20.0, -5.0, 9.3}, OstPoint3{0.0, 0.0, 15.0}})
        require(std::abs(OsteotomyCore::PathField(back.slotPlan[0].path, point) -
                         OsteotomyCore::PathField(plan.slotPlan[0].path, point)) < 1e-6,
                "the reloaded slot does not follow the same cut");
    require(back.slotPlan[0].path.valid, "the reloaded path is not valid");

    require(back.type == GuideType::Chin, "the guide type was lost");
    require(back.paint.size() == 2 && back.paint[1].erase && std::abs(back.paint[1].radiusMm - 2.5) < 1e-9 &&
                back.paint[0].center == plan.paint[0].center,
            "the brushed region was lost");
    require(back.workflowStep == GuideWorkflowStep::Holes && back.rightPaintEnd == 1 && back.leftPaintEnd == 2,
            "the guided workflow position was lost");
    require(std::abs(back.design.base.cornerRadiusMm - 4.5) < 1e-9, "the outline rounding was lost");
    require(std::abs(back.design.base.edgeTaperMm - 5.5) < 1e-9 &&
                std::abs(back.design.base.edgeThicknessFraction - 0.35) < 1e-9 &&
                std::abs(back.design.base.edgeRoundMm - 0.8) < 1e-9,
            "the rim finish was lost");
    require(back.figures.size() == 3 && back.figures[0].shape == GuideFigureShape::Box &&
                back.figures[0].operation == GuideFigureOperation::Subtract &&
                back.figures[0].matrix == plan.figures[0].matrix && std::abs(back.figures[0].heightMm - 0.8) < 1e-9,
            "the box figure was lost");
    require(back.figures[1].shape == GuideFigureShape::Mesh && back.figures[1].operation == GuideFigureOperation::Add &&
                back.figures[1].sourcePath == plan.figures[1].sourcePath && back.figures[1].sourceLabel == 215,
            "the copied mesh source was lost");
    require(back.figures[2].shape == GuideFigureShape::CurvedTube &&
                std::abs(back.figures[2].diameterMm - 3.2) < 1e-9 &&
                back.figures[2].controlPoints == plan.figures[2].controlPoints,
            "the curved tube was lost");
    require(GuidePlanCore::SourceLabelsFor(GuideType::LeFort) == std::vector<int>{206, 205} &&
                GuidePlanCore::SourceLabelsFor(GuideType::Chin) == std::vector<int>{213, 212},
            "the guide types do not wrap their own models");
    require(back.holes.size() == 2 && back.holes[0].center == plan.holes[0].center &&
                back.holes[1].axis == plan.holes[1].axis && std::abs(back.holes[1].diameterMm - 2.5) < 1e-9,
            "the fixation holes were lost");
}

void testEmptyAndDefaults()
{
    // An empty object gives a usable plan with the defaults, so an older project just opens without guides.
    const GuidePlan empty = GuidePlanCore::FromJson({});
    require(empty.contour.empty() && empty.slotPlan.empty() && empty.holes.empty() && empty.figures.empty(),
            "an empty plan is not empty");
    require(empty.type == GuideType::LeFort, "an older plan did not default to a Le Fort guide");
    require(empty.workflowStep == GuideWorkflowStep::Envelope && empty.rightPaintEnd == 0 && empty.leftPaintEnd == 0,
            "an older plan did not start at the envelope step");
    require(empty.design.base.thicknessMm > 0.0 && empty.design.edgeMarginMm > 0.0,
            "the defaults were lost on an empty plan");

    // It survives a real JSON document, not just the in-memory object.
    const QJsonObject json = GuidePlanCore::ToJson(samplePlan());
    const QByteArray text = QJsonDocument(json).toJson(QJsonDocument::Compact);
    const GuidePlan back = GuidePlanCore::FromJson(QJsonDocument::fromJson(text).object());
    require(back.slotPlan.size() == 2 && back.holes.size() == 2 && back.contour.size() == 4,
            "the plan did not survive a JSON document");
    require(back.name == QStringLiteral("Guía Le Fort"), "the accented name did not survive");
}
} // namespace

int main()
{
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"round trip", testRoundTrip},
        {"empty and defaults", testEmptyAndDefaults},
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
