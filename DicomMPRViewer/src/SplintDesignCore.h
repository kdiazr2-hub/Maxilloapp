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

QJsonObject ParamsToJson(const SplintHeightmapParams& params);
SplintHeightmapParams ParamsFromJson(const QJsonObject& object); // missing keys keep defaults
QJsonObject DesignToJson(const SplintDesign& design);
bool DesignFromJson(const QJsonObject& object, SplintDesign& design, QString* error);
QJsonArray DesignsToJson(const std::vector<SplintDesign>& designs);
// Invalid entries are skipped, built-in designs are always present (taken from
// defaults when missing), labels and names are made unique.
std::vector<SplintDesign> DesignsFromJson(const QJsonArray& array, const std::vector<SplintDesign>& defaults);
}
