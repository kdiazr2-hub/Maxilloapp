---
type: progress
title: Progress — Asistente de guía de corte Le Fort I
description: Ledger of the four-step assistant tasks, autopilot run of 2026-10-05.
tags: [sdd, progress, guias, lefort]
timestamp: 2026-10-05T00:00:00Z
topic: sdd
slug: asistente-guia-lefort
status: in-progress
---

# Progress — asistente-guia-lefort (autopilot, user's approval 2026-10-05)

| Task | Status | Evidence |
|------|--------|----------|
| A3 RootAnalysisCore | complete | RootAnalysisTests 3/3 (red → green; flat root tips gave two peaks → keep the higher within 4 mm) |
| B1 BandFromHeights + bandHeights/caseLabel | complete | LeFortMotionTests +1, GuidePlanTests +1 |
| D1 two guides | complete | LeFortGuideTests "two guides, one each side" (2 patches, no material in the nose, ≥ 2 screws each, 2 pieces) |
| D2 engraving | complete | LeFortGuideTests: text is a closed solid of its width; labels on the guide, clear of screws, stand ≥ 0.35 mm proud (red until the text was put on the outer face, not the bone) |
| A1 teeth sidecar | complete | Python 25/25 (2 new) |
| A2 sidecar import | complete | SplintWorkspaceTests impaction flow: hidden «Dientes superiores» object |
| A4 roots step UI | complete | workspace: 4 teeth named, report shown, overlays |
| B2 band step UI | complete | workspace: pilar D 4.0 → 3.0 taken; saved and reloaded |
| D3 guides step UI | complete | workspace: 4 engraved labels, two STL written, no false «piezas» warning |
| E1 docs + verify | complete in the cloud | Linux core 10/10, full app ctest 30/30 under Xvfb; Windows build + real case pending (user) |
| F1 real case 2026-10-05: band only by the cut | complete | LeFortGuideTests "the band is the bone between the two cuts": bone far behind and a septum are not drawn (red → green: `BandOnBone` clips to 6 mm across the vertical from the pillar–piriform pieces, nothing medial to a rim) |
| F2 band heights shown with the envelope | complete | SplintWorkspaceTests: spins read 4.0 before «Proponer» (`refreshGuideBand` syncs them) |
| F3 holes on the anterior wall, outward of the rim | complete | LeFortHoleTests "a wall facing sideways is not proposed" (red → green, axis·anterior ≥ 0.5), "piriform holes stay lateral of the rim" (guard) |
| F4 roots | complete | LeFortHoleTests "a drill near a root is refused" (red → green: the drill path to 6 mm keeps 1 mm from the teeth field, reason «raíz»); workspace: 12 sites, none over the synthetic roots |
| F5 roots measured per tooth, apex → osteotomy | complete | RootAnalysisTests "the molars behind do not stand in for the first" (red: took the high third molar, as on the real case → green with the 2D height map), "the report states apex to osteotomy" |
| F6 no proposals on a real maxilla | complete | LeFortHoleTests "a thin pillar is proposed with a warning" and "an empty pillar says what blocked it" (red → green); «Aceptar» shown after an empty proposal |
| F7 roots: first molar found, values on the view | complete | RootAnalysisTests "a first molar ten millimetres from the pillar is found" (red: took the canine → green); workspace: 4 labels |
| F8 holes 2+2 on each pillar, roots warn | complete | LeFortHoleTests "two above and two below in a column on each pillar", "roots do not leave a pillar without holes below" (red → green) |
| F9 guide to the pillar past a perforation | complete | LeFortGuideTests "a perforation is not the aperture" (red: left guide x 22–25 only → green) |
| F10 thicker segmented bone | complete | Python ThinBoneTests 4/4 (pinhole sealed at 1.5 mm, air opening kept, other labels untouched) |
| F11 holes in the bone mesh | complete | MeshGeneratorTests thin wall (red: vanished → green with the 0.55 floor) |
| F12 solid guide, uniform rim, band while marking | complete | LeFortGuideTests "no openwork cells", "uniform rim" (red: 1.5 mm uneven → green); workspace: band on the envelope while marking |
