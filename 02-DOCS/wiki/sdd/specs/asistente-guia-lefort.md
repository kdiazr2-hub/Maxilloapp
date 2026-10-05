---
type: spec
title: Spec — Asistente de guía de corte Le Fort I en cuatro pasos
description: WHAT y WHY del paso GUÍAS como asistente automático y editable — raíces y ápices, corte, franja (overlap), orificios y dos guías grabadas — a partir del caso real del usuario.
tags: [sdd, spec, guias, lefort]
timestamp: 2026-10-05T00:00:00Z
topic: sdd
slug: asistente-guia-lefort
status: approved
---

# Spec — Asistente de guía de corte Le Fort I en cuatro pasos

> Slug: `asistente-guia-lefort` · Status: approved · Created: 2026-10-05 · Aprobada por el usuario: 2026-10-05 ("Apruebo")
> Inherits: [constitution](../constitution.md) v1.0.0 · Builds on: [guia-lefort-por-movimiento](guia-lefort-por-movimiento.md)

## Problem & why

El usuario planifica hoy sus guías Le Fort fuera de la app, en cuatro pasos (imágenes de un caso real,
2026-10-05): mide la longitud de las raíces, mide la distancia de los ápices al corte propuesto, calcula la
franja de hueso que el movimiento obliga a quitar (overlap) y diseña dos guías (derecha e izquierda) con los
orificios y el número de caso grabado. La app ya calcula la franja y propone orificios, pero no mide las raíces,
no permite corregir la franja y hace una sola guía unida por un puente.

## Goals

- El paso GUÍAS guía al cirujano por los cuatro pasos, cada uno calculado automáticamente y editable.
- El corte nunca queda a menos de 5 mm de un ápice sin que la app lo diga.
- El cirujano puede corregir la altura de la franja y la posición de los tornillos, y la guía se rehace con
  esos ajustes.
- Salen dos guías, derecha e izquierda, listas para imprimir y rotuladas.

## Non-goals

- Diseñar las placas (fase siguiente; las placas usarán los orificios de esta guía).
- Segmentar cada diente por separado con un modelo nuevo.
- Mover la trayectoria del corte dentro de este paso (se hace en OSTEOTOMÍAS).

## Users & context

El cirujano maxilofacial que ya hizo la osteotomía Le Fort I y la REPOSICIÓN, en el paso 8 · Guías.

## Behaviour

1. **Raíces.** La app muestra, para el canino y el primer molar de cada lado, la longitud de la raíz (del
   ápice a la cúspide) y la distancia del ápice al corte propuesto, como cotas sobre el modelo.
   Revisa además todos los ápices bajo el corte: si alguno queda a menos de **5 mm**, lo marca y avisa.
2. **Franja (overlap).** La franja sale del movimiento (spec anterior) con su altura en pilar D,
   piriforme D, piriforme I y pilar I. El cirujano puede cambiar cualquiera de las cuatro alturas; la franja,
   la segunda ranura y el informe se rehacen con ellas. «Restablecer» vuelve a las del movimiento.
3. **Orificios.** La app los propone sobre el hueso; el cirujano los mueve (reglas de soporte de la spec
   anterior) y los acepta.
4. **Guías.** Dos guías independientes, una por lado, del pilar nasomaxilar al maxilomalar, sin entrar en la
   nariz: cada una con las dos ranuras (corte y borde superior de la franja), sus camisas y sus tornillos de
   posición, y grabado en relieve el número de caso (que escribe el cirujano) y DER o IZQ. Se exportan como dos
   STL.
- **Edge — dientes no separados:** si el proyecto no tiene los dientes superiores como objeto aparte (casos
  segmentados antes), el paso 1 lo dice y explica cómo volver a segmentar; los pasos 2–4 funcionan igual.
- **Edge — raíz no identificable:** si un canino o primer molar no se distingue, la app lo dice y sigue con
  la revisión de todos los ápices.
- **Edge — reabrir:** las alturas editadas de la franja, los orificios y el número de caso se guardan con el
  proyecto (claves opcionales).

## Acceptance criteria

- Given dientes superiores segmentados aparte, When se entra al paso 1, Then aparecen 4 longitudes de raíz y
  4 distancias ápice-corte (canino y primer molar por lado), en mm con un decimal.
- Given un ápice a 4 mm del corte, When se revisa el paso 1, Then ese ápice queda marcado y el informe avisa
  de que está a menos de 5 mm.
- Given una franja calculada, When el cirujano cambia la altura del pilar D de 2.0 a 3.0 mm, Then la franja y
  la segunda ranura miden 3.0 mm en el pilar D y el resto no cambia.
- Given alturas editadas, When se guarda y se reabre el proyecto, Then las alturas son las editadas.
- Given orificios aceptados, When se crean las guías, Then salen dos piezas independientes (DER e IZQ), cada
  una de una sola pieza, sin material en la apertura piriforme, con sus camisas y el texto grabado.
- Given las guías creadas, When se exportan, Then se escriben dos STL (`guia_der.stl`, `guia_izq.stl`) tras
  haberlas mostrado (constitución 15).

## Points to clarify

- **suposición tomada** — Sin segmentación por diente, el canino es el ápice más alto en la zona del reborde
  piriforme y el primer molar el ápice en la zona del pilar maxilomalar, y la longitud se mide hasta la
  cúspide más baja de esa misma columna. *Riesgo:* en dientes apiñados o inclinados la cota puede caer en el
  diente vecino; el cirujano la verá sobre el modelo.
- **suposición tomada** — El texto se graba en la cara externa, por encima de la ranura superior, con una
  fuente de trazo de VTK. *Riesgo:* solo estético.
- **decisión del usuario (2026-10-05)** — dos guías separadas (sustituye a «una guía con puente»); aviso a
  5 mm; se miden ápice-corte y longitud de raíz; grabado de número de caso y lado.
