#include "SplintDesignCore.h"

#include <QJsonDocument>

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using namespace SplintDesignCore;

constexpr int kUpper = 206;
constexpr int kInitialLower = -1001;
constexpr int kFinalLower = -1002;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::string s(const QString& text)
{
    return text.toStdString();
}

SplintHeightmapParams customParams()
{
    SplintHeightmapParams p;
    p.edgeOffsetMm = 2.5;
    p.filletMm = 0.7;
    p.clearanceMm = 0.25;
    p.minFeatureMm = 0.6;
    p.impressionUpper = false;
    p.impressionLower = true;
    p.minThicknessMm = 1.2;
    p.maxThicknessMm = 3.4;
    p.computeThickness = false;
    p.roundingFactorForSplintSide = 0.8;
    p.convexHullFixDistance = 1.1;
    p.reduceThreshold = 1.3;
    p.smoothThreshold = 0.15;
    p.innerSmoothSigma = 22.0;
    p.convexHullSmoothSigma = 9.0;
    p.gridResolutionMm = 0.3;
    p.undercutUpper = true;
    p.undercutLower = true;
    p.undercutAngleUpperDeg = 7.5;
    p.undercutAngleLowerDeg = 40.0;
    p.undercutDirectionUV = {0.2, 0.98};
    return p;
}

bool sameParams(const SplintHeightmapParams& a, const SplintHeightmapParams& b)
{
    return a.edgeOffsetMm == b.edgeOffsetMm && a.filletMm == b.filletMm && a.clearanceMm == b.clearanceMm &&
           a.minFeatureMm == b.minFeatureMm && a.impressionUpper == b.impressionUpper &&
           a.impressionLower == b.impressionLower && a.minThicknessMm == b.minThicknessMm &&
           a.maxThicknessMm == b.maxThicknessMm && a.computeThickness == b.computeThickness &&
           a.roundingFactorForSplintSide == b.roundingFactorForSplintSide &&
           a.convexHullFixDistance == b.convexHullFixDistance && a.reduceThreshold == b.reduceThreshold &&
           a.smoothThreshold == b.smoothThreshold && a.innerSmoothSigma == b.innerSmoothSigma &&
           a.convexHullSmoothSigma == b.convexHullSmoothSigma && a.gridResolutionMm == b.gridResolutionMm &&
           a.undercutUpper == b.undercutUpper && a.undercutLower == b.undercutLower &&
           a.undercutAngleUpperDeg == b.undercutAngleUpperDeg && a.undercutAngleLowerDeg == b.undercutAngleLowerDeg &&
           a.undercutDirectionUV == b.undercutDirectionUV;
}

void testDefaults()
{
    const auto designs = DefaultDesigns(kUpper, kInitialLower, kFinalLower);
    require(designs.size() == 2, "expected two default designs");
    require(designs[0].name == QStringLiteral("Intermedia") && designs[0].label == kIntermediateLabel &&
                designs[0].builtIn && designs[0].lowerSource == kInitialLower,
            "wrong intermediate default");
    require(designs[1].name == QStringLiteral("Final") && designs[1].label == kFinalLabel &&
                designs[1].builtIn && designs[1].lowerSource == kFinalLower,
            "wrong final default");
    require(designs[0].id != designs[1].id, "default ids are not unique");
}

void testAddCopyRenameRemove()
{
    auto designs = DefaultDesigns(kUpper, kInitialLower, kFinalLower);
    designs[0].upperPoints = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}};
    designs[0].params = customParams();

    int index = -1;
    QString error;
    require(AddDesign(designs, QStringLiteral("final"), kUpper, kFinalLower, &index, &error), s(error));
    require(designs[static_cast<size_t>(index)].name == QStringLiteral("final 2"), "duplicate name not numbered");
    require(designs[static_cast<size_t>(index)].label == kFirstExtraLabel, "first extra label not used");

    require(CopyDesign(designs, 0, &index, &error), s(error));
    const SplintDesign& copy = designs[static_cast<size_t>(index)];
    require(copy.name == QStringLiteral("Intermedia (copia)"), "wrong copy name: " + s(copy.name));
    require(copy.label == kFirstExtraLabel + 1 && !copy.builtIn, "copy label or built-in flag wrong");
    require(copy.id != designs[0].id, "copy kept the id");
    require(copy.upperPoints == designs[0].upperPoints && sameParams(copy.params, designs[0].params),
            "copy lost points or parameters");

    require(!RenameDesign(designs, index, QStringLiteral("FINAL"), &error), "case-insensitive duplicate accepted");
    require(!RenameDesign(designs, index, QStringLiteral("   "), &error), "empty name accepted");
    require(RenameDesign(designs, index, QStringLiteral("  Prueba  "), &error) &&
                designs[static_cast<size_t>(index)].name == QStringLiteral("Prueba"),
            "rename failed or not trimmed");
    require(RenameDesign(designs, 0, QStringLiteral("Intermedia 1"), &error), "built-in rename failed");

    require(!RemoveDesign(designs, 0, &error) && error.contains(QStringLiteral("no se pueden borrar")),
            "built-in design removed");
    require(!RemoveDesign(designs, 99, &error), "invalid index accepted");
    const int extra = IndexOfLabel(designs, kFirstExtraLabel);
    require(RemoveDesign(designs, extra, &error), s(error));
    require(NextFreeLabel(designs) == kFirstExtraLabel, "freed label not reused");
}

void testLabelExhaustion()
{
    auto designs = DefaultDesigns(kUpper, kInitialLower, kFinalLower);
    QString error;
    int added = 0;
    while (AddDesign(designs, QStringLiteral("Extra"), kUpper, kInitialLower, nullptr, &error))
        ++added;
    require(added == kLastExtraLabel - kFirstExtraLabel + 1, "wrong number of extra designs");
    require(error.contains(QStringLiteral("máximo")), "exhaustion not reported");
    require(!CopyDesign(designs, 0, nullptr, &error), "copy beyond the label range accepted");
    require(designs.back().name == QStringLiteral("Extra %1").arg(added), "names not unique");
}

void testJsonRoundTrip()
{
    auto designs = DefaultDesigns(kUpper, kInitialLower, kFinalLower);
    int index = -1;
    QString error;
    require(AddDesign(designs, QStringLiteral("Diseño ñ"), 203, 204, &index, &error), s(error));
    SplintDesign& d = designs[static_cast<size_t>(index)];
    d.upperPoints = {{1.25, -2.5, 3.125}, {4, 5, 6}, {7, 8, 9.5}};
    d.lowerPoints = {{-1, -2, -3}, {0.1, 0.2, 0.3}, {10, 20, 30}, {1, 1, 1}};
    d.params = customParams();
    d.editedContourUV = {{{0, 0}, {10, 0}, {10, 5.5}, {0, 5}}};
    d.editedContourFrame.origin = {1, 2, 3};
    d.editedContourFrame.axisU = {0, 1, 0};
    d.editedContourFrame.axisV = {-1, 0, 0};
    d.editedContourFrame.anteriorResolved = true;

    // Through text, as in the project file.
    const QByteArray text = QJsonDocument(DesignsToJson(designs)).toJson();
    const auto loaded = DesignsFromJson(QJsonDocument::fromJson(text).array(),
                                        DefaultDesigns(kUpper, kInitialLower, kFinalLower));
    require(loaded.size() == 3, "wrong number of loaded designs");
    const SplintDesign& r = loaded[2];
    require(r.id == d.id && r.name == d.name && r.label == d.label && !r.builtIn, "identity not preserved");
    require(r.upperSource == 203 && r.lowerSource == 204, "sources not preserved");
    require(r.upperPoints == d.upperPoints && r.lowerPoints == d.lowerPoints, "points not preserved");
    require(sameParams(r.params, d.params), "parameters not preserved");
    require(r.editedContourUV == d.editedContourUV, "edited contour not preserved");
    require(SameFrame(r.editedContourFrame, d.editedContourFrame, 1e-9, 1.0 - 1e-12) &&
                r.editedContourFrame.anteriorResolved,
            "contour frame not preserved");
}

void testLegacyAndInvalidJson()
{
    const auto defaults = DefaultDesigns(kUpper, kInitialLower, kFinalLower);
    const auto empty = DesignsFromJson(QJsonArray{}, defaults);
    require(empty.size() == 2 && empty[0].label == kIntermediateLabel && empty[1].label == kFinalLabel,
            "missing designs do not fall back to defaults");

    const auto design = [](const QString& name, int label) {
        QJsonObject o;
        o[QStringLiteral("name")] = name;
        o[QStringLiteral("label")] = label;
        o[QStringLiteral("id")] = QStringLiteral("same-id");
        return o;
    };
    QJsonArray messy;
    messy.append(design(QStringLiteral("Final editada"), kFinalLabel));
    messy.append(design(QStringLiteral("Otra final"), kFinalLabel));      // duplicate built-in → dropped
    messy.append(design(QStringLiteral("A"), 500));                         // out of range → relabelled
    messy.append(design(QStringLiteral("a"), kFirstExtraLabel));            // duplicate name
    messy.append(design(QStringLiteral("B"), kFirstExtraLabel));            // duplicate label
    messy.append(QJsonObject{{QStringLiteral("label"), 231}});              // no name → skipped
    const auto loaded = DesignsFromJson(messy, defaults);

    require(loaded.size() == 5, "wrong number of designs after cleanup: " + std::to_string(loaded.size()));
    require(loaded[0].label == kIntermediateLabel && loaded[0].name == QStringLiteral("Intermedia"),
            "missing built-in not restored");
    require(loaded[1].label == kFinalLabel && loaded[1].name == QStringLiteral("Final editada") && loaded[1].builtIn,
            "saved built-in not kept");
    for (size_t i = 0; i < loaded.size(); ++i)
        for (size_t j = i + 1; j < loaded.size(); ++j) {
            require(loaded[i].label != loaded[j].label, "labels not unique");
            require(loaded[i].name.compare(loaded[j].name, Qt::CaseInsensitive) != 0, "names not unique");
            require(loaded[i].id != loaded[j].id, "ids not unique");
        }
    for (size_t i = 2; i < loaded.size(); ++i)
        require(loaded[i].label >= kFirstExtraLabel && loaded[i].label <= kLastExtraLabel, "extra label out of range");

    QJsonObject partial;
    partial[QStringLiteral("filletMm")] = 0.9;
    const SplintHeightmapParams p = ParamsFromJson(partial);
    SplintHeightmapParams expected;
    expected.filletMm = 0.9;
    require(sameParams(p, expected), "missing parameter keys do not keep defaults");
}

void testSameFrame()
{
    SplintOcclusalFrame a;
    SplintOcclusalFrame b = a;
    require(SameFrame(a, b), "identical frames differ");
    b.origin[0] += 1.0;
    require(!SameFrame(a, b), "moved frame considered equal");
    b = a;
    b.axisU = {0.995, 0.0998, 0.0};
    b.axisV = {-0.0998, 0.995, 0.0};
    require(!SameFrame(a, b), "rotated frame considered equal");
}
} // namespace

void testExtras()
{
    const auto absd = [](double v) { return v < 0.0 ? -v : v; };
    SplintDesign design = SplintDesignCore::DefaultDesigns(1, 2, 3).front();
    SplintExtras& e = design.extras;
    require(SplintDesignCore::AddBracketMark(e, {0.0, 0.0, 1.0}, 2.0), "first bracket mark rejected");
    require(!SplintDesignCore::AddBracketMark(e, {0.5, 0.0, 1.0}, 2.0), "mark within half the brush was added");
    require(SplintDesignCore::AddBracketMark(e, {1.5, 0.0, 1.0}, 2.0), "separate bracket mark rejected");
    require(SplintDesignCore::RemoveBracketMarks(e, {1.4, 0.0, 1.0}, 0.5) == 1 && e.bracketMarks.size() == 1,
            "unmarking removed the wrong marks");

    SplintOcclusalFrame frame;
    e.bevel = SplintBevel{{0.0, 20.0, 5.0}, {0.0, 22.0, -5.0}};
    SplintWireHole hole;
    hole.center = {0.0, 18.0, 0.0};
    hole.surfaceNormal = {0.0, 0.0, -1.0};
    hole.diameterMm = 1.2;
    e.wireHoles.push_back(hole);
    design.wireHoleOrientation = SplintHoleOrientation::Bevel;
    SplintDesignCore::ReorientWireHoles(design, frame);
    SplintPoint3 origin{}, normal{};
    require(SplintHeightmapGenerator::BevelPlane(*e.bevel, frame, origin, normal), "bevel plane rejected");
    const SplintPoint3& axis = e.wireHoles[0].axis;
    require(absd(axis[0] * normal[0] + axis[1] * normal[1] + axis[2] * normal[2]) < 1e-9 && axis[2] > 0.5,
            "holes were not re-oriented along the bevel");
    design.wireHoleOrientation = SplintHoleOrientation::SurfaceNormal;
    SplintDesignCore::ReorientWireHoles(design, frame);
    require(axis[2] > 0.999, "surface-normal hole must point to the maxilla");
    design.wireHoleOrientation = SplintHoleOrientation::Bevel;
    SplintDesignCore::ReorientWireHoles(design, frame);

    e.bracketOffsetMm = 0.75;
    design.wireHoleDiameterMm = 1.2;
    design.bracketBrushRadiusMm = 2.0;
    design.createdSourceKey = QStringLiteral("abc");
    SplintDesign loaded;
    QString error;
    require(SplintDesignCore::DesignFromJson(SplintDesignCore::DesignToJson(design), loaded, &error),
            "extras round trip failed: " + error.toStdString());
    require(loaded.extras.bevel && loaded.extras.bevel->first == e.bevel->first && loaded.extras.bevel->second == e.bevel->second,
            "bevel lost in JSON");
    require(loaded.extras.wireHoles.size() == 1 && loaded.extras.wireHoles[0].center == e.wireHoles[0].center &&
                loaded.extras.wireHoles[0].axis == e.wireHoles[0].axis &&
                loaded.extras.wireHoles[0].surfaceNormal == e.wireHoles[0].surfaceNormal &&
                loaded.extras.wireHoles[0].diameterMm == 1.2,
            "wire hole lost in JSON");
    require(loaded.extras.bracketMarks.size() == 1 && loaded.extras.bracketMarks[0].center == e.bracketMarks[0].center &&
                loaded.extras.bracketMarks[0].radiusMm == 2.0 && loaded.extras.bracketOffsetMm == 0.75,
            "bracket marks lost in JSON");
    require(loaded.wireHoleOrientation == SplintHoleOrientation::Bevel && loaded.wireHoleDiameterMm == 1.2 &&
                loaded.bracketBrushRadiusMm == 2.0 && loaded.createdSourceKey == QStringLiteral("abc"),
            "extras settings lost in JSON");

    // Designs saved before the extras existed load with defaults.
    QJsonObject legacy = SplintDesignCore::DesignToJson(SplintDesignCore::DefaultDesigns(1, 2, 3).front());
    for (const char* key : {"extras", "wireHoleDiameterMm", "wireHoleOrientation", "bracketBrushRadiusMm", "createdSourceKey"})
        legacy.remove(QString::fromLatin1(key));
    SplintDesign old;
    require(SplintDesignCore::DesignFromJson(legacy, old, &error), "legacy design rejected");
    require(!old.extras.bevel && old.extras.wireHoles.empty() && old.extras.bracketMarks.empty() &&
                old.wireHoleOrientation == SplintHoleOrientation::SurfaceNormal && old.wireHoleDiameterMm == 1.0 &&
                old.createdSourceKey.isEmpty(),
            "legacy design got unexpected extras");
}

int main()
{
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"defaults", testDefaults},
        {"add, copy, rename, remove", testAddCopyRenameRemove},
        {"label exhaustion", testLabelExhaustion},
        {"json round trip", testJsonRoundTrip},
        {"legacy and invalid json", testLegacyAndInvalidJson},
        {"same frame", testSameFrame},
        {"extras", testExtras},
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
