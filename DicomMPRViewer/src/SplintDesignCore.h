#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// SplintDesignCore
//
// Named splint designs (ProPlan "Splint Design"): each keeps its source parts,
// guide points, parameters and edited contour. "Intermedia" (label 215) and
// "Final" (216) always exist; extra designs take labels 230–299.
// No Qt Widgets; JSON is stored in the .maxilloproject file.
// ─────────────────────────────────────────────────────────────────────────────

#include "SplintHeightmapGenerator.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <vector>

struct SplintDesign
{
    QString id;
    QString name;
    int label = -1;
    bool builtIn = false;
    int upperSource = 0; // source choice values owned by the UI
    int lowerSource = 0;
    std::vector<SplintPoint3> upperPoints;
    std::vector<SplintPoint3> lowerPoints;
    SplintHeightmapParams params;
    // Empty = automatic contour. An edited contour is only valid in the frame
    // it was edited in.
    std::vector<SplintContourUV> editedContourUV;
    SplintOcclusalFrame editedContourFrame;

    // Extras (bevel, wire holes, bracket margins) and the settings for new ones.
    SplintExtras extras;
    double wireHoleDiameterMm = 1.0;
    SplintHoleOrientation wireHoleOrientation = SplintHoleOrientation::SurfaceNormal;
    double bracketBrushRadiusMm = 1.5;
    // Signature of the source meshes the created splint was built from
    // (empty = unknown); a different current signature marks it outdated.
    QString createdSourceKey;
};

namespace SplintDesignCore
{
inline constexpr int kIntermediateLabel = 215;
inline constexpr int kFinalLabel = 216;
inline constexpr int kFirstExtraLabel = 230;
inline constexpr int kLastExtraLabel = 299;

std::vector<SplintDesign> DefaultDesigns(int upperSource, int intermediateLowerSource, int finalLowerSource);

int IndexOfLabel(const std::vector<SplintDesign>& designs, int label);
int NextFreeLabel(const std::vector<SplintDesign>& designs); // -1 when the range is full
// Case-insensitive unique name; ignoreIndex is the design being renamed.
QString UniqueName(const std::vector<SplintDesign>& designs, const QString& base, int ignoreIndex = -1);

bool AddDesign(std::vector<SplintDesign>& designs, const QString& name, int upperSource, int lowerSource,
               int* newIndex, QString* error);
bool CopyDesign(std::vector<SplintDesign>& designs, int index, int* newIndex, QString* error);
bool RenameDesign(std::vector<SplintDesign>& designs, int index, const QString& name, QString* error);
bool RemoveDesign(std::vector<SplintDesign>& designs, int index, QString* error);

bool SameFrame(const SplintOcclusalFrame& a, const SplintOcclusalFrame& b,
               double originToleranceMm = 0.05, double axisToleranceCos = 0.99995);

// Rigid motion (row-major 4x4) that maps `from` onto `to` when both have the
// same points in the same order (a repositioned copy); nullopt when the point
// counts differ or the change is not rigid within toleranceMm.
using SplintMatrix = std::array<double, 16>;
std::optional<SplintMatrix> RigidMotion(vtkPolyData* from, vtkPolyData* to, double toleranceMm = 0.05);
SplintMatrix IdentityMatrix();
SplintMatrix Compose(const SplintMatrix& second, const SplintMatrix& first); // second ∘ first
SplintPoint3 TransformPoint(const SplintMatrix& matrix, const SplintPoint3& point);
SplintPoint3 TransformDirection(const SplintMatrix& matrix, const SplintPoint3& direction);

// Bracket marks: a new mark is skipped when one lies within half the brush
// radius; unmarking removes the marks whose centre is within the radius.
bool AddBracketMark(SplintExtras& extras, const SplintPoint3& center, double radiusMm);
int RemoveBracketMarks(SplintExtras& extras, const SplintPoint3& center, double radiusMm);
// Recomputes every wire-hole axis for the design's orientation setting.
void ReorientWireHoles(SplintDesign& design, const SplintOcclusalFrame& frame);

QJsonObject ExtrasToJson(const SplintExtras& extras);
SplintExtras ExtrasFromJson(const QJsonObject& object);
QJsonObject ParamsToJson(const SplintHeightmapParams& params);
SplintHeightmapParams ParamsFromJson(const QJsonObject& object); // missing keys keep defaults
QJsonObject DesignToJson(const SplintDesign& design);
bool DesignFromJson(const QJsonObject& object, SplintDesign& design, QString* error);
QJsonArray DesignsToJson(const std::vector<SplintDesign>& designs);
// Invalid entries are skipped, built-in designs are always present (taken from
// defaults when missing), labels and names are made unique.
std::vector<SplintDesign> DesignsFromJson(const QJsonArray& array, const std::vector<SplintDesign>& defaults);
}
