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
// midline out: central incisor (x 3), canine (x 11, the longest), premolar (x 15), first molar (x 19).
struct Root
{
    double x;
    double apex;
};
const Root kRoots[] = {{3.0, 2.0}, {11.0, 6.0}, {15.0, 3.0}, {19.0, -1.0}};

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
    if (index < 0)
        throw std::runtime_error("a tooth was not identified: " + RootAnalysisCore::ToothName(tooth).toStdString());
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
                               {RootTooth::FirstMolarRight, rightSign * 19.0, -1.0},
                               {RootTooth::FirstMolarLeft, -rightSign * 19.0, -1.0}};
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
    require(analysis.warnings.size() == 2 && analysis.warnings.front().contains(QStringLiteral("5.0")),
            "the report does not warn about them");
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
