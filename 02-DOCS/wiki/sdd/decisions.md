---
type: decisions
title: MaxilloApp — SDD decisions
description: Append-only log of significant decisions (date, options, why).
tags: [sdd, decisions]
topic: sdd
---

# Decisiones (solo se añade)

## 2026-10-04 — Constitución v1.0.0 ratificada

Ratificada tal cual por el usuario. Tres conflictos entre las reglas de rsc y la práctica previa del repo:

- **A. Formato de commits.** Opciones: gitmoji + frase descriptiva en inglés / frase sin emoji
  (desactivar `gitmoji-guard`) / gitmoji + Conventional Commits.
  **Elegido:** gitmoji + frase descriptiva (principio 10). Por qué: conserva el estilo del
  historial y es compatible con el guard instalado.
- **B. Flujo de ramas.** Opciones: rama + PR siempre / rama + PR solo para cambios grandes /
  directo a `main` (práctica previa).
  **Elegido:** rama + PR siempre (principio 11). Por qué: app clínica; cada cambio queda revisable.
- **C. Autoría.** Opciones: solo humano, sin coautor IA (regla fija de rsc) / humano + línea
  `Co-Authored-By` de la IA.
  **Elegido:** humano + coautor IA (principio 12). Por qué: trazabilidad de qué cambios escribió
  la IA. Sustituye la regla fija de rsc; la fase `ship` debe respetar esta enmienda.

Además, el principio 16 (colores con `CranioPalette`) se limitó al código nuevo o tocado porque
`MainWindow.cpp` ya tiene unos 56 `QColor` fijos.

## 2026-10-04 — Spec `guia-lefort-por-movimiento` (draft)

El pedido inicial mezclaba guía de corte y placas a medida. **Elegido:** acotar este ciclo a
la guía de corte Le Fort I generada a partir del movimiento; las placas y la reproducción del
avance/retroceso pasan a una fase siguiente (decisión del usuario, con imágenes de referencia).
Respuestas del usuario: la franja de impactación va **encima del corte** y su altura sale del
movimiento pre/post punto a punto (puede ser asimétrica); orificios por defecto **piriforme +
pilar, 2 + 2 por lado**; soporte óseo = **grosor medido en la TC**; un orificio movido a un
sitio sin soporte **se avisa y se respeta**.

## 2026-10-04 — Spec `guia-lefort-por-movimiento` aprobada

Aprobada explícitamente por el usuario tras incorporar sus respuestas (solo cambios de plano; 2.0 mm; 0.5 mm; dos ranuras). Flujo por fases, sin autopiloto.

## 2026-10-04 — Plan `guia-lefort-por-movimiento` (draft)

- **Altura de la franja = componente vertical del desplazamiento rígido completo en cada punto del
  corte** (`Z·(M·p − p)`). Opciones: esa / descomponer en Euler y filtrar el avance. Por qué: exacta,
  sin convención de ejes ni pivote, ya ignora avance, lateral y giro horizontal, y coincide con lo
  que REPOSICIÓN muestra.
- **Dos clases core nuevas** (`LeFortMotionCore`, `LeFortHoleCore`) y `LeFortGuideCore::Layout`
  extendido con un perfil de franja opcional, para no tocar el comportamiento actual (constitución 5).
- **Spike previo:** compilar los tests core en Linux para poder verificar en la nube.
- Abiertas: medir el grosor en la segmentación (recomendado) o en HU de la TC; conservar o
  recolocar los orificios manuales si cambia el movimiento.

## 2026-10-04 — Plan `guia-lefort-por-movimiento` aprobado

Cerradas las dos abiertas: grosor medido en la segmentación ósea; los orificios manuales se conservan y se re-evalúan si cambia el movimiento.

## 2026-10-04 — Arreglos tras `analyze` (decisiones del usuario)

- PR 1 (cores) se integra solo tras el build Release + `ctest` completo en Windows (constitución 4).
- `CLAUDE.md` documenta las cores ya en PR 1 (T019, constitución 18).
- Un orificio craneal dentro de la franja o a < 4 mm de ella se **rechaza** (spec ampliada).
- Se mantiene el aviso de placas anteriores que no coinciden con la guía (spec ampliada).

## 2026-10-04 — Linux core verification uses the project's exact VTK  (feature: guia-lefort-por-movimiento, task: T001)
Context  — Ubuntu ships VTK 9.1; with it PlateTests and LeFortGuideTests fail by tenths of a millimetre.
Options  — accept 9.1 and skip those tests / relax tolerances / build VTK 9.5.2 from source.
Decision — build VTK 9.5.2 (no rendering modules) and require `VTK 9.5` in tools/linux-core-tests.
Why      — the tests are right on 9.5.2; relaxing them would hide real geometry regressions.

## 2026-10-04 — Band spans as arc length along the cut  (feature: guia-lefort-por-movimiento, task: T002)
Context  — the plan's interface said spans are "lateral" positions, but the Le Fort cut runs obliquely back
           to the pillars, so a lateral coordinate is ambiguous there and depends on the guide's own frame.
Options  — lateral coordinate in the guide frame / arc length along the cut / 3D end points.
Decision — arc length from pilar D, with `PointAlongCut` and `CutLength` to turn it into points.
Why      — it is intrinsic to the cut, exact where the height is linear, and Layout can map it to its frame.
           Also a `noMotion` flag, so "no REPOSICIÓN" is not confused with an invalid cut.

## 2026-10-04 — Hole proposal samples the bone field, and the caller gives the anterior  (feature: guia-lefort-por-movimiento, task: T006)
Context  — the plan had `Propose(surface, ctx)` picking among the envelope's vertices. Envelope vertices are
           irregular (a synthetic box has 8), and the cut's sweep axis has no sign of its own.
Options  — envelope vertices / a regular grid per pillar projected onto the segmented bone / both.
Decision — `Propose(ctx)`: per pillar and side, a 1 mm grid in the window, each node found on the anterior
           wall by marching along −anterior into the bone field; `LeFortHoleContext::anterior` is given by
           the caller (LeFortGuideCore already works it out for the guide's frame).
Why      — regular, deterministic coverage measured on the bone itself (constitution 8), and no guessing of
           which way is the face.

## 2026-10-05 — Where the generated guide enters GUÍAS  (feature: guia-lefort-por-movimiento, task: T012)
Context  — GUÍAS had become a hand-painting assistant (envelope → paint right/left/bridge → holes → slots →
           build); the automatic button was hidden. The guide "seguía igual" because nothing called the cores.
Options  — a button after the envelope / replace the assistant / generate on entry.
Decision — a button after the envelope, «Generar guía desde el movimiento»; the hand-drawn steps stay as the
           alternative and to retouch it (user's choice, 2026-10-05).

## 2026-10-05 — With plates, the guide drills the plates' holes  (feature: guia-lefort-por-movimiento, task: T012)
Context  — the spec assumed the generated guide replaces the plate-derived one and warns when earlier plates do
           not match. In the app today the plate step comes before the guide, and `SplintWorkspaceTests` requires
           the guide to carry exactly the plates' predictive holes — the principle the user started from
           ("las guías ... tengan los orificios iguales al de la placa").
Options  — always propose and warn on mismatch / plates win when they exist / ask every time.
Decision — plates win: with plates the guide drills their predictive holes; without, the app proposes the sites
           and the surgeon can move them. The band and the second slit apply either way.
Why      — a guide whose holes miss existing plates would not fit them in theatre. To confirm with the user;
           `UnmatchedPlateHoles` stays in the core for the other choice.

## 2026-10-05 — Guide first, one guide bridged under the aperture  (feature: guia-lefort-por-movimiento)
Context  — first real case: the band was not drawn on the bone at the cut, the guide ran over the nose and
           the anterior nasal spine, proposed holes went straight into the guide, and the plate step came first.
Decision — (user) one guide, each side pillar nasomaxilar → pillar maxilomalar, joined by a bridge below the
           piriform aperture clear of the ANS; band above the cut, two thin slits; holes proposed as markers and
           put into the guide with «Aceptar orificios»; Guías before Placa personalizada, the plates later use
           the guide's holes. Supersedes "With plates, the guide drills the plates' holes".

## 2026-10-05 — Upper teeth as a sidecar, not a label change  (feature: asistente-guia-lefort, task: A1)
Context  — the root analysis needs the upper teeth apart; the labelmap maps them into the maxilla (3 → 5).
Options  — new label in the labelmap / a second file next to it.
Decision — a second file (`<output>_dientes_superiores.nrrd`) loaded as a hidden object.
Why      — taking the teeth out of label 5 would take them out of the maxilla and the Le Fort segment, which
           must move with its teeth through REPOSICIÓN and the splints.

## 2026-10-05 — Labels bring their own material  (feature: asistente-guia-lefort, task: D2)
Context  — the band's rows next to the slits are full of positioning screws and sleeves; there was no room.
Decision — the case number goes above the cranial screws and DER/IZQ below the caudal ones, each on a strip of
           added material joined to the guide, as the user's printed guides are taller where the number is.

## 2026-10-05 — Screws keep 1 mm from the roots; holes only on the anterior wall  (feature: asistente-guia-lefort, real case)
Context  — on the surgeon's case holes were proposed by the incisor roots, under the aperture and on the lateral
           zygoma, and the band was drawn over the whole skull.
Decision — a drill path within 1 mm of the upper teeth (to 6 mm deep) is refused, not warned; proposals search
           outward of the piriform rims and need an axis within 60° of the anterior; the band is drawn only within
           6 mm of the pillar–piriform pieces of the cut.

## 2026-10-05 — Thin bone is proposed with a warning  (feature: asistente-guia-lefort, real case)
Context  — on the surgeon's maxilla «Proponer orificios» gave 0 holes: the anterior wall is under 2 mm almost
           everywhere, and only Ok sites were proposed (the earlier "odd places" were the few thick spots).
Decision — Warning sites are proposed after Ok ones; an empty pillar reports the dominant refusal; the guide can
           be created without plate holes. Supersedes "never a weaker site" of guia-lefort-por-movimiento.

## 2026-10-05 — Holes on the pillars; roots warn; thicker segmented bone  (feature: asistente-guia-lefort, real case)
Context  — the surgeon wants two holes above and two below the cut at each nasomaxillary and zygomaticomaxillary
           pillar; the root rule left the pillars below the cut empty; the left guide stopped short of the pillar.
Decision — holes ranked by nearness to the pillar line within ±5 mm; root proximity becomes a warning
           (supersedes "refused, not warned"); the aperture is only gaps inside the piriform points; envelope
           closing 2.5 mm; the segmentation grows thin walls into ≥ 150 HU and seals non-air pinholes (1.5 mm).

## 2026-10-06 — Solid guide with a uniform rim; band visible while marking  (feature: asistente-guia-lefort, real case)
Context  — the surgeon rejected the openwork cells ("orificios de más"), found the rim irregular, and lost sight of
           the band while marking holes on the envelope.
Decision — cells off by default (supersedes the 2026-09-20 openwork frame); with two guides each side's rim is
           uniform (rows of band dabs from the lowest to the highest painted reach); the band is also drawn on the
           envelope and shown whenever the bone models are hidden.

## 2026-10-06 — Guide = hull of what it carries; foramina marked; screw chosen  (feature: asistente-guia-lefort)
Context  — reference image of printed guides; the guide reached the infraorbital foramen; holes came out 2.0 mm.
Decision — each guide is the rounded convex hull of sleeves, screws, labels and the cut ±5 mm (supersedes the
           uniform rim); the surgeon marks each infraorbital foramen (optional guided step) and the guide keeps
           5 mm clear; the surgeon picks the screw and the hole is its pilot drill (2.0 → 1.6). Foramen detection
           is manual: on segmented bone a foramen and a perforation of the sinus wall look alike.

