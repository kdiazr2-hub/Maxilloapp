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
