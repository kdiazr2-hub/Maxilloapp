---
type: analysis
title: Analyze — Guía de corte Le Fort I generada a partir del movimiento
description: Cruce constitución ↔ spec ↔ plan ↔ tareas antes de implementar.
tags: [sdd, analysis, guias, lefort]
timestamp: 2026-10-04T00:00:00Z
topic: sdd
slug: guia-lefort-por-movimiento
---

# Analyze — guia-lefort-por-movimiento

> Constitución v1.0.0 · [Spec](../specs/guia-lefort-por-movimiento.md) (clarified, aprobada) ·
> [Plan + tareas](../plans/guia-lefort-por-movimiento.md) (approved, T001–T018)

**GATE: PASS** (re-run 2026-10-04) — 0 CRITICAL · 0 HIGH · 0 MEDIUM · 1 LOW aceptado.

Primera pasada: BLOCKED (2 CRITICAL · 4 MEDIUM · 2 LOW). El usuario decidió los arreglos y se
aplicaron en sus fases: `tasks` (#1, #2, #4, #5 → T001, T012, T016, T019 y la previsión de entrega)
y `specify` (#3, #6, #7 → Behaviour y Revisions de la spec). Queda #8 (LOW), aceptado: las tareas de
test no llevan bloque Interfaces porque se ejecutan con el contexto completo, no en subagentes
aislados. La tabla de abajo es la de la primera pasada, conservada como historial.

## Coverage map

| REQ | Requisito de la spec (corto) | Plan | Tareas | Estado |
| --- | --- | --- | --- | --- |
| R1 | Impactación uniforme 3 mm → franja 3 ± 0.5 encima del corte | §2, §3 Band | T002, T003, T008, T009, T012 | covered |
| R2 | Asimetría 4 D / 1 I | §3 Band | T002, T003 | covered |
| R3 | Impactación anterior: más alta adelante | §3 Band (pitch) | T002, T003 | covered |
| R4 | Avance puro → sin franja | §2 decisión, §3 invariantes | T002, T003 | covered |
| R5 | Avance + impactación = misma franja | §3 invariantes | T002, T003 | covered |
| R6 | Descenso → un corte | §3 Band | T002, T003, T012 | covered |
| R7 | 16 orificios con ≥ 2.0 mm, camisa en cada uno | §3 Propose, SleeveFigures | T006, T007, T009, T012 | covered |
| R8 | Mover a sitio válido → regenera, resto igual | §4 paso 7 | T008, T009, T014 | covered |
| R9 | Mover a sitio delgado → avisa y respeta | §3 Support | T004, T005, T014 | covered |
| R10 | Guardar/reabrir conserva orificios y franja | §3 GuidePlanCore, §4 | T010, T011, T014 | covered |
| R11 | Sin REPOSICIÓN → informa, sin franja | §3 NoMotion, §4 paso 3 | T002, T003, T012 | covered |
| R12 | Una pieza, material bajo cada camisa | §3 Layout | T008, T009 | covered |
| R13 | Revisión visible antes de exportar | §5 | T015 | covered |
| R14 | Franja visible en otro color, altura consultable | §4 paso 6 | T012 (informe), T013 | covered |
| R15 | Sin sitio válido → propone los que caben e informa cuáles faltan | §3 Propose `missing[]` | T006, T007, T012 | covered (tras #4) |
| R16 | Borde óseo libre sigue rechazándose | §0, §3 Support | T004, T005 | covered |
| R17 | Orificios registrados para la fase de placas | §3 GuidePlanCore | T010, T011 | covered |
| R18 | Rechazo de orificio craneal dentro de la franja o a < 4 mm de ella | §3 Support | T004, T005 | covered (spec tras #3) |
| R19 | Aviso si hay placas cuyos orificios no coinciden con la guía | §4 migración | T012 | covered (spec tras #6) |
| R20 | Constitución 18: cores documentadas en el mismo PR | — | T019 | covered (tras #2) |

## Findings (primera pasada)

| # | Sev. | Tipo | Artefacto A (loc.) | Artefacto B (loc.) | Conflicto | Resolver en |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | CRITICAL | Constitución | Constitución, principio 4 ("build Release + `ctest -C Release` completo antes de integrar") | Tareas, Review Forecast ("PR 1 … verificable en la nube") | PR 1 se integraría con tests en Linux (Qt 6.4 / VTK 9.1), no con el build Release de Windows que exige el principio 4. | `tasks`: PR 1 solo se integra tras build Release + ctest completo en el PC del usuario |
| 2 | CRITICAL | Constitución | Constitución, principio 18 (`CLAUDE.md` se actualiza "en el mismo cambio" que añade una clase core) | Tareas T016 (al final, en PR 2) | PR 1 añade `LeFortMotionCore` y `LeFortHoleCore` sin tocar `CLAUDE.md`. | `tasks`: mover la parte de `CLAUDE.md` de las cores a PR 1 (p. ej. en T011 o una tarea nueva) |
| 3 | MEDIUM | Drift | Spec §Behaviour / §Points (solo el borde libre se rechaza; el resto avisa) | Plan §3 Support ("NUEVO, un orificio craneal dentro de la franja o a < 4 mm … Rechazo") | Regla de bloqueo nueva que la spec no recoge. Es razonable (ese hueso se quita), pero cambia lo que el cirujano puede hacer. | `specify`: que el usuario la ratifique y se añada a la spec |
| 4 | MEDIUM | Ambiguo | Spec §Behaviour "sin sitio válido … informa de cuáles faltan y por qué" | Tarea T012 (done-check) | El done-check de la UI no comprueba que el informe muestre los orificios faltantes. | `tasks`: añadir ese chequeo a T012 |
| 5 | MEDIUM | Ambiguo | Tarea T001 done-check ("Windows configure unchanged") | Entorno (no hay Windows en la nube) | No se puede ejecutar aquí tal cual. Verificable si el build Linux vive en un archivo aparte y `DicomMPRViewer/CMakeLists.txt` no cambia (diff vacío). | `tasks`: precisar el done-check |
| 6 | MEDIUM | Drift | Spec (no menciona placas existentes) | Plan §4 "el informe avisa si hay placas cuyos orificios no coinciden" · T012 | Comportamiento útil pero no pedido. | `specify`: aceptarlo en la spec o quitarlo de T012 |
| 7 | LOW | Desfase | Spec §Points "área no formulable" (orificios manuales si cambia el movimiento) | Plan §7 decisión cerrada 2 (se conservan y re-evalúan) | La spec sigue marcándolo como no formulable; el plan ya lo decidió con el usuario. | `specify`: pasarlo a Revisions |
| 8 | LOW | Portador | Tareas de test T002, T004, T006, T008, T010 | Interfaces de T003, T005, T007, T009, T011 | Las tareas de test no tienen bloque Interfaces propio; un implementador aislado no vería el contrato. Hoy no aplica: las ejecuto yo con todo el contexto. | `tasks` (solo si se reparten a subagentes) |

## Recommended routing

- **`tasks`** (#1, #2, #4, #5, #8): ajustes de una línea en la lista; ninguno cambia el diseño.
- **`specify`** (#3, #6, #7): tres decisiones del usuario sobre la spec — ratificar la regla de la
  franja, aceptar o quitar el aviso de placas, y cerrar el área no formulable.
- **Ninguno afecta a T001** (compilar los tests core en Linux): T001 no integra nada ni toca la spec.
