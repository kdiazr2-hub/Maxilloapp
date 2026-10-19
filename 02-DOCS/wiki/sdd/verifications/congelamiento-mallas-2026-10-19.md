---
type: verification
title: Verification — congelamiento-mallas — 2026-10-19
description: Evidence for the bone-mesh speed-up and the segmentation log file (FTD lane, no spec).
tags: [sdd, verification, mesh, segmentation]
timestamp: 2026-10-19T20:05:00Z
topic: sdd
slug: congelamiento-mallas
source_state: a5e426e (dirty — the change under test, committed right after)
---

# Verification — congelamiento-mallas — 2026-10-19

FTD lane: no spec or plan. The criteria are the ones in the worklog
`02-DOCS/raw/worklog/2026-10-19-segmentacion-medular-y-escalones.md` («Freeze»).

## Stack gate
- No `scripts/verify.sh` (C++/CMake project). Gate = CTest on the Linux app build (Qt 6.4, VTK 9.5.2, Xvfb),
  fresh run after the last code edit: 30/32 passed. `RepositionWorkspaceTests` («Hiding the objects left their
  masks hidden in 3D») and `SplintWorkspaceTests` («teeth and gingiva are not the default composite method») fail
  the same way on the user's `cursor-totalsegmentator` branch before this change (recorded in the worklog's merge
  note), so not caused by it.

## Criteria
- [x] Bone mesh at least as fast as before the merge — `meshbench 400 0.5` (512×512×400, 0.4/0.4/0.5 mm, 4
      cores) on a byte-identical copy of `MeshGenerator.cpp` (`cmp`): 18.5 s / 19.3 s at 40 / 70 iterations, peak
      1.3 GB; pre-merge user code 19.9 s / 21.2 s, 1.7 GB; merged code 38.3 s / 36.5 s, 2.5 GB.
- [x] Same surfaces — `MeshGeneratorTests` passed (thin walls survive; wobble 0.0245 / 0.0541 mm at 1.25 / 2.5 mm
      slices, limit 0.07).
- [x] `segmentacion.log` written as output arrives — runtime check `logcheck` (service + fake
      `DENTALSEGMENTATOR_CMD`): every line time-stamped, a chunk without a newline closed, stderr included,
      «Proceso terminado, codigo 3.» at the end. Control: a command that does not exist gives no run and no file.

## Capas no ejecutadas
- **HERRAMIENTA-AUSENTE** — Windows/MSVC build and its CTest: not available in the cloud. The user builds it.
- **SUSTITUIDA** — real CT: a synthetic skull shell instead. It cannot show the real bounding box of
  TotalSegmentator's skull label nor the cost of `fillBetweenCortices` with Hounsfield units.
- **NO-APLICA** — the freeze's cause during segmentation is not reproduced: no GPU here; the new log is what will
  show it.

## Hallazgos descartados
- ninguno

## Verdict: PASS for the two changes. The freeze itself stays open until the user's next run and its log.
