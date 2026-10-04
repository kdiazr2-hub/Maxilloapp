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
