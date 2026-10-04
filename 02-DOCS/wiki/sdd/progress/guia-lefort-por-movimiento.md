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

## T003 — 2026-10-04
- status: complete
- green: `build-linux-core/LeFortMotionTests` 12/12 PASS (red in T002). Full Linux suite
  `ctest --test-dir build-linux-core` → 100% passed, 7/7 (VTK 9.5.2).
- triangulation: added "a posterior impaction is clockwise and the left can be higher" (13/13 PASS).
  It passed first run because the code existed, so it was mutation-checked: with the Clockwise and
  LeftHigher branches disabled it FAILS ("a posterior impaction is not clockwise"); restored → 13/13.
- files: src/LeFortMotionCore.cpp, tests/LeFortMotionTests.cpp.
- decision: kind = Mixed if some point rises ≥ 0.5 mm and some drops ≥ 0.5 mm; Impaction if only rises;
  Descent if only drops; otherwise NoPlaneChange. No-motion = cut-centre shift < 0.2 mm and angle < 0.3°
  (same bars as `guideMotionSummary`). Pitch only for the four Le Fort points (piriform pair vs pillars).
- review: per-task fresh-reviewer subagent not dispatched (subagents only on the user's request in this
  session); self-review against the done-check and §0 Global Constraints. The end-of-branch `review`
  still runs.
- blocker: none.

## T004 — 2026-10-04
- status: complete
- red: `build-linux-core/LeFortHoleTests` builds; 7/7 FAIL on assertions against a stub `Support` that
  accepts everything with 0 mm: 3 mm wall → Ok 3 ± 0.3; 1 mm wall → Warning "grosor … 2.0"; lateral
  margin → Rejected "orilla"; 3 mm from an imaginary mid-wall cut → Rejected "osteotom"; cranial holes
  inside a 4 mm band and 2 mm above it → Rejected "franja", 6 mm above → Ok; segment hole under an
  impaction → Ok with ≥ 9.5 mm; site in the air → Rejected.
- files: src/LeFortHoleCore.h (contract of T005), src/LeFortHoleCore.cpp (stub), tests/LeFortHoleTests.cpp
  (own synthetic wall: 3 mm right, 1 mm left, sinus behind), both CMakeLists.
- decision: for a cranial hole the band rule is judged before the ring/cut rules, so a hole inside the
  band is refused for the band (the more useful reason) and not for the margin the band's edge creates.
- blocker: none.

## T005 — 2026-10-04
- status: complete
- green: `build-linux-core/LeFortHoleTests` 7/7 PASS (red in T004). Full Linux suite → 100% passed, 8/8.
- files: src/LeFortHoleCore.cpp, src/LeFortHoleCore.h (`bandThresholdMm = 0.5` added to the params).
- how: preop bone query decides the side (no bone within 1 mm → Rejected); for a cranial site with a band,
  the band height under it comes from the nearest piece of the cut seen along the band's vertical, linear
  along the piece; inside or < 4 mm above → Rejected "franja"; then `PlateCore::CheckHoleSeat` with the
  site carried to the planned position for the segment; then thickness along −axis on the baked bone
  field (entry and exit zero crossings interpolated, step ≤ ¼ of the field spacing, max 15 mm);
  < 2.0 mm → Warning "grosor mínimo de 2.0 mm".
- review: self-review (no subagent, as in T003).
- blocker: none.

## T006 — 2026-10-04
- status: complete
- red: `build-linux-core/LeFortHoleTests` — the 7 Support cases still PASS; the 4 new Propose cases FAIL on
  assertions against a stub that proposes nothing: 16 holes (2+2 per pillar) on sound bone, Ok, ≥ 2 mm,
  ≥ 6.5 mm apart, within 8 mm of their pillar, cranial 4–12 mm above the band's upper edge, segment 4–12 mm
  below the cut, each re-accepted by `Support`; without a band the cranial holes start 4 mm above the cut;
  a 1 mm left pillar gets no cranial hole and is listed in `missing` (2, with a reason); deterministic.
- files: src/LeFortHoleCore.h (Propose contract, pillar/side/origin types, window params, `anterior`),
  src/LeFortHoleCore.cpp (stub), tests/LeFortHoleTests.cpp (maxilla with aperture, thick/thin variants).
- decisions: `Propose(context)` seeks sites with the bone field instead of the envelope's vertices; the
  context carries `anterior`. Logged.
- blocker: none.

## T007 — 2026-10-04
- status: complete
- green: `build-linux-core/LeFortHoleTests` 11/11 PASS (the 4 Propose cases were red in T006), ~3 s.
  Full Linux suite → 100% passed, 8/8.
- evidence of layout (synthetic maxilla, 4 mm impaction): each pillar gets a vertical pair above the band
  (z = 17, 24; band edge at 13) and a pair on the segment (z = 5, −2), on its own line (x = ±10, ±20).
- test change (separate step, before the implementation): in the thin-pillar geometry the 3 mm / 1 mm
  boundary moved from x = 12 to x = 11, so it no longer sits exactly on the left pillar's window edge
  (20 − 8); the window stays entirely thin, the assertion is unchanged.
- how: per pillar and side, 1 mm grid in u ∈ ±8 mm and v ∈ [4, 12] mm from the cut / band edge; each node
  found on the wall by walking along −anterior into the bone field; window re-checked at the landed site;
  side checked with the pre-op bone query; axis `PlateCore::BoneNormalAt`; kept only if `Support` = Ok;
  ranked by thickness (to 0.25 mm), then nearest the pillar's line, then nearest the cut; greedy with
  6.5 mm spacing across all holes; shortfalls go to `missing` with a Spanish reason.
- review: self-review (no subagent).
- blocker: none.

## T008 — 2026-10-04
- status: complete
- red: `build-linux-core/LeFortGuideTests` — the 6 existing cases PASS; 4 new cases FAIL on assertions
  against a `Layout` that ignores the band: upper slit only where the segment rises (rolled band, right
  side to x = −5) + report mentions "franja"; upper pieces split by the same bridges (each inside a Le
  Fort piece, none across the midline); 3 mm band → one piece with the upper slit open at z = 12;
  7 mm band → paint reaches 2 mm above the band at x = ±15, positioning screws ≥ 4 mm above it, no
  lattice cell within slit clearance of either slit.
- guards (green before the implementation, on purpose): an empty band leaves the layout identical; a
  moved hole takes its pad with it (spec §Acceptance 8, core part).
- files: src/LeFortGuideCore.h/.cpp (optional `const LeFortBandProfile* band` after params, ignored for
  now), tests/LeFortGuideTests.cpp, CMakeLists (LeFortMotionCore in LeFortGuideTests, the app, PlateProbe;
  LeFortHoleCore in the app), tools/linux-core-tests.
- blocker: none.

## T009 — 2026-10-04
- status: complete
- green: `build-linux-core/LeFortGuideTests` 12/12 PASS (the 4 band cases were red in T008; the 6 old cases
  and the 2 guards stay green). Full Linux suite → 100% passed, 8/8.
- guard added after green: the 7 mm band guide built with `GuideDesignCore::Build` is one piece (it was,
  checked first with a temporary print: ok=1 pieces=1, 2 upper pieces) — now asserted in "a tall band is
  covered and kept clear".
- how (`LeFortGuideCore::Layout`, param renamed `impaction` — the function already has a local `band`):
  the band's rise is sampled along the cut every 0.25 mm and looked up by the guide's lateral coordinate;
  a second row of 8 mm dabs on the band's upper edge where it runs on the wall and the band is taller
  than the first row covers; cranial positioning screws wanted at offset + rise; lattice cells skipped
  within `rowOffset` of the upper slit; upper slit pieces inside each Le Fort piece (same bridges) where
  the rise ≥ 0.5 mm, ≥ 3 mm long, path = `upperCut`; report appends the band report and the piece count.
  New params `bandThresholdMm = 0.5`, `bandMarginAboveMm = 2.0`; `LeFortGuideLayout::upperSlitPieces`.
- review: self-review (no subagent).
- blocker: none.

## T010 — 2026-10-04
- status: complete
- red: `build-linux-core/GuidePlanTests` — the 2 existing cases PASS; "Le Fort holes travel with the plan"
  FAILS ("the guide's holes are not saved") and "an unreadable hole is skipped" FAILS. Guard green on
  purpose: "older plans load without holes" (no key → no holes; a plan without holes writes no key).
- files: src/GuidePlanCore.h (`std::vector<LeFortProposedHole> lefortHoles`, support not saved),
  tests/GuidePlanTests.cpp.
- blocker: none.

## T011 — 2026-10-04
- status: complete
- green: `build-linux-core/GuidePlanTests` 5/5 PASS (2 were red in T010). `ProjectSerializerTests` added to
  the Linux build (with Qt6::Gui) and PASSES 4/4, including "guides plan round trip" and "legacy project
  without designs". Full Linux suite → 100% passed, 9/9.
- files: src/GuidePlanCore.cpp (optional `lefortHoles`: center, axis, pillar key, side, origin; written only
  when there are holes; on load a hole with an unknown pillar/side or no centre is skipped),
  tools/linux-core-tests/CMakeLists.txt.
- review: self-review (no subagent).
- blocker: none.
