---
type: progress
title: Progress — guia-lefort-por-movimiento
description: Append-only implement ledger (task, red/green evidence, blockers, decisions).
tags: [sdd, progress]
timestamp: 2026-10-04T00:00:00Z
topic: sdd
slug: guia-lefort-por-movimiento
---

# Progress — guia-lefort-por-movimiento

## T001 — 2026-10-04
- status: in progress
- done: `DicomMPRViewer/tools/linux-core-tests/CMakeLists.txt` builds the six guide/plate core tests on
  Ubuntu 24.04 (Qt 6.4.2, VTK 9.1.0 from apt). `DicomMPRViewer/CMakeLists.txt` untouched. Needed
  `QT_NO_EMIT` (Qt's `emit` macro vs oneTBB headers pulled by `<execution>`); no core source uses `emit`.
- result with VTK 9.1: ImplicitCoreTests, OsteotomyCoreTests, GuideDesignTests, GuidePlanTests PASS;
  PlateTests FAIL ("the plate steps across a large advancement: the arm did not come back onto the
  cranium: 0.000000"); LeFortGuideTests FAIL ("the guide is laid out from the plan: a slit point is off
  the cut or inside the aperture" — 1 of 11 slit points at x = −4.89 against the test's |x| > 5).
- hypothesis: VTK 9.1 vs the project's 9.5.2 (the envelope differs by ~0.1 mm at the aperture corner).
  Not yet known whether both tests pass on the user's Windows build.
- next: build VTK 9.5.2 from source (minimal modules) and re-run; ask the user for their Windows ctest.
- skill resolution: used [implement]; missing [config.yaml — sdd-init pending, test commands depend on
  this task; stack testing skill for C++/CTest not in the catalog]; fallback [commands from the plan's
  Tasks header].

## T001 — 2026-10-04 (cierre)
- status: complete
- evidence: VTK 9.5.2 built from source (tag v9.5.2, 17 non-rendering modules) in the session scratchpad;
  `ctest --test-dir build-linux-core` → 100% tests passed, 0 failed out of 6 (ImplicitCoreTests,
  OsteotomyCoreTests, GuideDesignTests, PlateTests, LeFortGuideTests, GuidePlanTests).
  `git diff DicomMPRViewer/CMakeLists.txt` empty.
- finding: the two failures above were VTK 9.1 (Ubuntu), not the code — both pass on 9.5.2.
  `tools/linux-core-tests` now requires `VTK 9.5`.
- decision: logged (Linux core verification needs the project's exact VTK).
- blocker: none for T002–T011. Merge still needs the Windows build (constitution 4); the user is
  installing the toolchain on a new PC (Intel x64, VS 2022 Build Tools, generator "Visual Studio 17 2022").

## T002 — 2026-10-04
- status: complete
- red: `build-linux-core/LeFortMotionTests` builds and all 12 cases FAIL on their assertions (stub
  `LeFortMotionCore` returns an empty profile): uniform 3 mm, 4 D / 1 I, anterior 4 mm, pure advance 5 mm,
  advance 5 + 3 mm, lateral shift + yaw, descent 3 mm, mixed, 0.4 mm threshold, no motion, invalid cut,
  points along the cut.
- files: src/LeFortMotionCore.h (contract of T003), src/LeFortMotionCore.cpp (stub), tests/LeFortMotionTests.cpp,
  DicomMPRViewer/CMakeLists.txt (`add_core_test(LeFortMotionTests …)`), tools/linux-core-tests/CMakeLists.txt.
- decision: spans are arc length along the cut from pilar D (plan said "lateral"); `noMotion` flag added
  to the profile so the UI can tell "no REPOSICIÓN" from a broken cut. Logged.
- blocker: none.
