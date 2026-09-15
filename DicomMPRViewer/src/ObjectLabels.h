#pragma once

// Object labels shared by the MainWindow translation units. Positive labels
// are table objects (actor key 1000 + label); negative ones are temporary
// scene actors.

inline constexpr int kUpperArchLabel = 201;
inline constexpr int kLowerArchLabel = 202;
inline constexpr int kUpperCompositeLabel  = 203;
inline constexpr int kLowerCompositeLabel  = 204;
inline constexpr int kLeFortCranialLabel   = 205;   // cranial base after Le Fort I split
inline constexpr int kLeFortSegLabel       = 206;   // Le Fort I segment after split
inline constexpr int kBssoGuideLabel       = 207;   // bilateral sagittal ramus guide object
inline constexpr int kBssoDistalLabel      = 208;   // distal tooth-bearing mandibular segment
inline constexpr int kBssoProximalLabel    = 209;   // proximal ramus segments
inline constexpr int kBssoProximalRightLabel = 210; // right proximal ramus segment
inline constexpr int kBssoProximalLeftLabel  = 211; // left proximal ramus segment
inline constexpr int kGenioBodyLabel       = 212;   // mandible after chin segment split
inline constexpr int kGenioSegmentLabel    = 213;   // chin segment after genioplasty split
inline constexpr int kBiteScanLabel        = 214;   // intraoperative/post-osteotomy bite scan
inline constexpr int kIntermediateSplintLabel = 215; // Le Fort moved + initial mandible
inline constexpr int kFinalSplintLabel        = 216; // Le Fort moved + final distal mandible
inline constexpr int kSplintInitialMandibleChoice = -1001;
inline constexpr int kSplintFinalMandibleChoice   = -1002;
inline constexpr int kSplintTestUpperChoice       = -1101; // "STL de prueba superior"
inline constexpr int kSplintTestLowerChoice       = -1102; // "STL de prueba inferior"
inline constexpr int kOrientGizmoTempLabel = -200;  // temporary combined mesh for orientation gizmo
inline constexpr int kLeFortPlaneLabel     = -300;
inline constexpr int kLeFortCutLineLabel   = -301;
inline constexpr int kGenioPlaneLabel      = -320;
inline constexpr int kGenioCutLineLabel    = -321;
inline constexpr int kSplintPreviewActorKey = -510;  // live height-map splint preview
inline constexpr int kSplintContourOverlayKey = 1;   // red contour in the occlusal views
inline constexpr int kCompositeBlockActorKey = -520; // blue cutting block in MODELOS
inline constexpr int kCompositeReviewActorKey = -521; // composite under review

// Splint extras in the splint views: wire-hole cylinders, bracket marks and
// the bevel line (overlay key).
inline constexpr int kSplintWireHolesActorKey = -511;
inline constexpr int kSplintBracketMarksActorKey = -512;
inline constexpr int kSplintBevelOverlayKey = 2;

// Osteotomy wizard cutting-path guides (BSSO uses both: right, left).
inline constexpr int kOsteotomyGuideActorKey = -530;
inline constexpr int kOsteotomyGuideLeftActorKey = -531;

// ORIENTACION reference bones while a registered scan waits for its composite (maxilla, mandible = key - 1).
inline constexpr int kOrientationBoneReferenceKey = -160;

// MODELOS composite contour wall (points method).
inline constexpr int kCompositeContourActorKey = -522;

// REPOSICIÓN analysis actors (key = base - label).
inline constexpr int kRepositionHighlightBaseKey = -3000;
inline constexpr int kRepositionPreOpBaseKey = -4000;

inline int objectActorKey(int label)
{
    return label > 0 ? 1000 + label : label;
}
