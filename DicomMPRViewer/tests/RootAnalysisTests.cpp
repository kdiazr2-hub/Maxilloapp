#include "RootAnalysisCore.h"

#include "OsteotomyCore.h"
#include "SplintTestGeometry.h"

#include <vtkAppendPolyData.h>
#include <vtkPolyData.h>

#include <cmath>
#include <functional>
#include <iostream>
#include <string>

namespace
{
using namespace splinttest;
using Vec3 = std::array<double, 3>;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

// The Le Fort cut at z = 9, its points pilar D, piriforme D, piriforme I, pilar I whatever the sort.
OsteotomyPath cut() { return OsteotomyCore::LeFortPath({{{-10.0, 5.0, 9.0}, {10.0, 5.0, 9.0}, {-20.0, -5.0, 9.0}, {20.0, -5.0, 9.0}}}); }

// Upper teeth as root columns 3 mm wide, cusps at z = −12, apices at the given heights. Per side, from the
// midline out: central incisor (x 3), canine (x 11, the longest), premolar (x 16), first molar (x 22).
struct Root
{
    double x;
    double apex;
};
const Root kRoots[] = {{3.0, 2.0}, {11.0, 6.0}, {16.0, 3.0}, {22.0, -1.0}};

vtkSmartPointer<vtkPolyData> teeth()
{
    auto append = vtkSmartPointer<vtkAppendPolyData>::New();
    for (const double side : {-1.0, 1.0})
        for (const Root& root : kRoots) {
            const double x = side * root.x;
            append->AddInputData(boxMesh({x - 1.5, x + 1.5, -1.5, 1.5, -12.0, root.apex}, false, false));
        }
    append->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(append->GetOutput());
    return out;
}

const RootApex& named(const RootAnalysis& analysis, RootTooth tooth)
{
    const int index = analysis.named[static_cast<size_t>(tooth)];
    if (index < 0) {
        std::string found;
        for (const RootApex& apex : analysis.apices)
            found += " (" + std::to_string(apex.apex[0]) + ", " + std::to_string(apex.apex[1]) + ", " +
                     std::to_string(apex.apex[2]) + ")";
        throw std::runtime_error("a tooth was not identified: " + RootAnalysisCore::ToothName(tooth).toStdString() +
                                 "; apices:" + found);
    }
    return analysis.apices[static_cast<size_t>(index)];
}

// spec §Acceptance 1: canine and first molar per side, with root length and apex-to-cut distance.
void testCaninesAndFirstMolarsAreMeasured()
{
    const OsteotomyPath path = cut();
    const RootAnalysis analysis = RootAnalysisCore::Analyze(teeth(), path);
    require(analysis.ok, "the analysis failed: " + analysis.error.toStdString());
    const double rightSign = path.points.front()[0] < 0.0 ? -1.0 : 1.0; // the side of pilar D
    struct Expect
    {
        RootTooth tooth;
        double x, apex;
    };
    const Expect expected[] = {{RootTooth::CanineRight, rightSign * 11.0, 6.0},
                               {RootTooth::CanineLeft, -rightSign * 11.0, 6.0},
                               {RootTooth::FirstMolarRight, rightSign * 22.0, -1.0},
                               {RootTooth::FirstMolarLeft, -rightSign * 22.0, -1.0}};
    for (const Expect& e : expected) {
        const RootApex& apex = named(analysis, e.tooth);
        const std::string name = RootAnalysisCore::ToothName(e.tooth).toStdString();
        require(std::abs(apex.apex[0] - e.x) <= 1.6, name + " is not the root at x " + std::to_string(e.x) + ": " +
                                                         std::to_string(apex.apex[0]));
        require(std::abs(apex.lengthMm - (e.apex + 12.0)) <= 0.3,
                name + " length " + std::to_string(apex.lengthMm) + " instead of " + std::to_string(e.apex + 12.0));
        require(std::abs(apex.cutDistanceMm - (9.0 - e.apex)) <= 0.3,
                name + " distance to the cut " + std::to_string(apex.cutDistanceMm));
        require(std::abs(apex.onCut[2] - 9.0) <= 0.3, name + "'s measure does not end on the cut");
    }
}

// spec §Acceptance 2: every apex nearer the cut than 5 mm is flagged and reported, named or not.
void testApicesNearerThanFiveMillimetresAreFlagged()
{
    const RootAnalysis analysis = RootAnalysisCore::Analyze(teeth(), cut());
    require(analysis.ok, analysis.error.toStdString());
    int flagged = 0;
    for (const RootApex& apex : analysis.apices) {
        const bool near = apex.cutDistanceMm < 5.0;
        require(apex.tooClose == near, "an apex at " + std::to_string(apex.cutDistanceMm) + " mm was misjudged");
        flagged += near ? 1 : 0;
    }
    require(flagged == 2, "the two canines (3 mm from the cut) should be flagged, got " + std::to_string(flagged));
    require(analysis.warnings.size() == 2 && analysis.warnings.front().contains(QStringLiteral("Canino")) &&
                analysis.report.contains(QStringLiteral("mínimo 5.0")),
            "the report does not warn about them");
}

// The molars behind the first one overlap it in a frontal view: a second molar 12 mm behind and a third one,
// unerupted and high in the tuberosity, 22 mm behind, at the same left-right position. The first molar is still
// the root under the pillar, and its apex is its own (user's case, 2026-10-05: the "first molar" apices came out
// above the cut, at −2.8 and −4.1 mm).
void testTheMolarsBehindDoNotStandInForTheFirst()
{
    auto append = vtkSmartPointer<vtkAppendPolyData>::New();
    append->AddInputData(teeth());
    for (const double side : {-1.0, 1.0}) {
        const double x = side * 22.0;
        append->AddInputData(boxMesh({x - 1.5, x + 1.5, -13.5, -10.5, -12.0, 4.0}, false, false));  // second molar
        append->AddInputData(boxMesh({x - 1.5, x + 1.5, -23.5, -20.5, 2.0, 13.0}, false, false));   // third, high
    }
    append->Update();
    const RootAnalysis analysis = RootAnalysisCore::Analyze(append->GetOutput(), cut());
    require(analysis.ok, analysis.error.toStdString());
    for (const RootTooth tooth : {RootTooth::FirstMolarRight, RootTooth::FirstMolarLeft}) {
        const RootApex& apex = named(analysis, tooth);
        const std::string name = RootAnalysisCore::ToothName(tooth).toStdString();
        require(std::abs(apex.apex[1]) <= 1.6 && std::abs(apex.apex[2] - (-1.0)) <= 0.3,
                name + " took another molar's apex: (" + std::to_string(apex.apex[0]) + ", " +
                    std::to_string(apex.apex[1]) + ", " + std::to_string(apex.apex[2]) + ")");
        require(std::abs(apex.cutDistanceMm - 10.0) <= 0.3, name + " distance " + std::to_string(apex.cutDistanceMm));
    }
    for (const RootTooth tooth : {RootTooth::CanineRight, RootTooth::CanineLeft})
        require(std::abs(named(analysis, tooth).cutDistanceMm - 3.0) <= 0.3, "a canine moved");
}

// The report gives, for each canine and first molar, how far its apex is from the osteotomy, and says plainly
// when the cut runs through the root.
void testTheReportStatesApexToOsteotomy()
{
    auto append = vtkSmartPointer<vtkAppendPolyData>::New();
    append->AddInputData(teeth());
    append->AddInputData(boxMesh({-12.5, -9.5, -1.5, 1.5, -12.0, 11.0}, false, false)); // a canine above the cut
    append->Update();
    const RootAnalysis analysis = RootAnalysisCore::Analyze(append->GetOutput(), cut());
    require(analysis.ok, analysis.error.toStdString());
    require(analysis.report.contains(QStringLiteral("ápice a 10.0 mm por debajo de la osteotomía")),
            "the first molars' apex-to-osteotomy distance is not stated: " + analysis.report.toStdString());
    require(analysis.report.contains(QStringLiteral("por encima de la osteotomía")) &&
                analysis.report.contains(QStringLiteral("cruza la raíz")),
            "a root the cut goes through is not reported as such: " + analysis.report.toStdString());
}

void testNoTeethIsAnErrorNotACrash()
{
    const RootAnalysis analysis = RootAnalysisCore::Analyze(nullptr, cut());
    require(!analysis.ok && !analysis.error.isEmpty(), "missing teeth must be explained");
}
} // namespace

int main()
{
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"canines and first molars are measured", testCaninesAndFirstMolarsAreMeasured},
        {"apices nearer than five millimetres are flagged", testApicesNearerThanFiveMillimetresAreFlagged},
        {"no teeth is an error, not a crash", testNoTeethIsAnErrorNotACrash},
        {"the molars behind do not stand in for the first", testTheMolarsBehindDoNotStandInForTheFirst},
        {"the report states apex to osteotomy", testTheReportStatesApexToOsteotomy},
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
