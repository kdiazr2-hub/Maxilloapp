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
