#pragma once

#include <QColor>

namespace CranioPalette {

inline constexpr const char* AppBackground = "#1c1c1e";
inline constexpr const char* PanelDark     = "#242426";
inline constexpr const char* PanelMid      = "#2c2c2e";
inline constexpr const char* AccentDark    = "#0066cc";
inline constexpr const char* Accent        = "#0a84ff";
inline constexpr const char* AccentLight   = "#1f3b57";
inline constexpr const char* Text          = "#f5f5f7";
inline constexpr const char* MutedText     = "#98989d";

inline QColor bone()          { return QColor(214, 205, 190); }
inline QColor boneLight()     { return QColor(232, 225, 214); }
inline QColor boneDark()      { return QColor(185, 169, 153); }
inline QColor softTissue()    { return QColor(202, 180, 168); }
inline QColor dental()        { return QColor(238, 228, 204); }
inline QColor mandible()      { return QColor(222, 216, 207); }
inline QColor maxilla()       { return QColor(205, 191, 173); }
inline QColor osteotomy()     { return QColor(188, 162, 146); }
inline QColor guide()         { return QColor(126, 118, 93); }
inline QColor canal()         { return QColor(170, 134, 118); }
inline QColor fallback()      { return QColor(196, 180, 166); }
// Planning marks: bone to take out, and the state of a drill site.
inline QColor resection()     { return QColor(255, 69, 58); }
inline QColor holeSound()     { return QColor(191, 90, 242); }
inline QColor holeWarning()   { return QColor(255, 159, 10); }
inline QColor holeRefused()   { return QColor(255, 69, 58); }

} // namespace CranioPalette
