---
type: plan
title: Plan — Asistente de guía de corte Le Fort I en cuatro pasos
description: Arquitectura, contratos y tareas del asistente (raíces, franja editable, orificios, dos guías grabadas).
tags: [sdd, plan, guias, lefort]
timestamp: 2026-10-05T00:00:00Z
topic: sdd
slug: asistente-guia-lefort
status: draft
---

# Plan — Asistente de guía de corte Le Fort I

> Spec: [asistente-guia-lefort](../specs/asistente-guia-lefort.md) (approved 2026-10-05) · Constitution v1.0.0

## Global constraints
- Logic in cores without Qt Widgets, test first (constitución 5); measured on the bone's own field or meshes,
  never on a wrap (constitución 8); new project keys optional (9); CLAUDE.md in the same change (18).
- Each task: red test → green → full Linux core + app suite (Xvfb) green; Windows build + real case by the user.

## Architecture

1. **Upper teeth, without touching the maxilla.** The labelmap stays as it is (teeth inside «Maxilar», so the
   Le Fort segment still carries them). `run_standalone_dental_segmentator.py` also writes the upper-teeth
   mask (DentalSegmentator label 3) as a sidecar NRRD next to the output; the app turns it into a hidden object
   «Dientes superiores» (`kUpperTeethLabel`), saved with the project like any object.
2. **`RootAnalysisCore`** (new): `Analyze(teethMesh, cut, params)` → apices (local height maxima of the teeth
   per 1 mm bin along the cut, ≥ 4 mm apart), each with its distance to the cut (`−PathField`) and root length
   (apex to the lowest teeth point within 2.5 mm of its column); canine = apex nearest the piriform point's
   lateral position, first molar = nearest the pillar's, per side; `warnings` for every apex < 5 mm.
3. **Editable band.** `LeFortMotionCore::BandFromHeights(cut, heights)` builds the same profile from four
   heights (spans, upperCut, kind, report). `GuidePlan::bandHeights` (optional, 4 numbers) overrides the
   movement's; absent → the movement's band.
4. **Two guides.** `LeFortGuideParams::separateSides` (default true): no bridge; `connectPaint` joins patches
   within each side only; each side gets two positioning screws (lateral end and piriform end, cranial).
   `Build` gives two pieces; `GuideDesignCore`/UI split by side for export (`guia_der.stl`, `guia_izq.stl`).
5. **Engraving.** `GuideEngraveCore::TextFigure(text, frame, heightMm, reliefMm)` → an Add `GuideFigure`
   (mesh shape) from `vtkVectorText` extruded; `PlaceLabel(layout, side)` finds a spot on the cranial part
   above the upper slit, clear of sleeves, screws and cells. `GuidePlan::caseLabel` (optional).
6. **UI** (`MainWindowGuides.cpp`): the Le Fort panel becomes four numbered steps — 1 Raíces (cotas + avisos),
   2 Franja (four spin boxes, «Restablecer»), 3 Orificios (propose / move / accept), 4 Guías (número de caso,
   crear, exportar dos STL).

## Tasks

| # | Task | Done-check | Deps |
|---|------|-----------|------|
| A1 | Sidecar upper-teeth NRRD in the segmentation script | python test: output dir has the sidecar with exactly label-3 voxels; labelmap unchanged | — |
| A2 | App loads it as hidden «Dientes superiores», persisted | workspace test: object present after segmentation result + reload | A1 |
| A3 | `RootAnalysisCore` | core tests: synthetic teeth → 4 named apices, lengths and distances ±0.3 mm; apex at 4 mm → warning | — |
| A4 | UI step 1 Raíces: overlays + report | workspace test: report lists 4 lengths/distances; missing teeth → message | A2, A3 |
| B1 | `BandFromHeights` + `GuidePlan::bandHeights` | core tests: heights 3/2/2/1 → band 3 mm at pilar D; JSON round trip; absent key → movement | — |
| B2 | UI step 2 Franja: spin boxes, Restablecer | workspace test: change pilar D → band/slit 3.0 mm there; reload keeps it | B1 |
| D1 | Two guides in `Layout` | core tests: 2 pieces, each one piece, nothing in the aperture, 2 screws each | — |
| D2 | `GuideEngraveCore` | core tests: text figure inside the side's region, clear of holes/slits; build stays one piece per side | D1 |
| D3 | UI step 4: case number, two guides, two STLs | workspace test: two STL written after showing; text present | D1, D2 |
| E1 | CLAUDE.md, decisions, verify | docs updated; Linux 100 %; user's Windows build + real case | all |

Order: A3, B1, D1, D2 (cores, cloud-verifiable) → A1, A2, B2, A4, D3 (UI) → E1.

## Risks
- Canine/molar identification without per-tooth segmentation (spec assumption): mitigated by showing the
  apices on the model and reporting all apices < 5 mm regardless of naming.
- Engraving legibility at print scale: text 4 mm high, 0.6 mm relief, configurable.
