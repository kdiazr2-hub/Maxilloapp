---
type: spec
title: Spec — Guía de corte Le Fort I generada a partir del movimiento
description: WHAT y WHY de la guía de corte Le Fort I que se deriva del movimiento planificado (impactación o descenso) y propone sus orificios en hueso con buen soporte.
tags: [sdd, spec, guias, lefort]
timestamp: 2026-10-04T00:00:00Z
topic: sdd
slug: guia-lefort-por-movimiento
status: clarified
---

# Spec — Guía de corte Le Fort I generada a partir del movimiento

> Slug: `guia-lefort-por-movimiento` · Status: clarified · Created: 2026-10-04 · Aprobada por el usuario: 2026-10-04 ("Sí, la apruebo")
> Inherits: [constitution](../constitution.md) v1.0.0

## Problem & why

Hoy la guía de corte del Le Fort I solo se genera a partir de placas ya diseñadas y
siempre lleva una sola ranura. Cuando la planificación pide una impactación, el cirujano
tiene que quitar una franja de hueso por encima del corte para que el maxilar pueda
subir, y la guía no le dice dónde ni cuánto. Esa franja casi nunca es pareja: puede ser
mayor de un lado que del otro, o mayor adelante que atrás. Los orificios de fijación se
colocan a mano, uno por uno, sin saber si el hueso bajo cada tornillo es lo bastante
grueso. El usuario reporta que esta parte "no está funcionando" para su práctica.

## Cost of not building it

En cada caso con impactación, la cantidad de hueso a quitar se estima a ojo en
quirófano. Si se quita poco, el maxilar no sube hasta la posición planificada y el plan
virtual no se reproduce. Si se quita de más, queda un hueco que compromete el contacto
óseo y la estabilidad. Sin orificios propuestos, cada guía exige colocar a mano unos 16
puntos y comprobar el hueso visualmente; un tornillo en hueso delgado de la pared del
seno pierde fijación. El usuario la señala como lo que hoy le impide usar la app en
cirugía con guía.

## The cheapest alternative

Mantener la guía actual de una ranura y que el cirujano mida la impactación con el
informe de movimiento (que ya indica ascenso o descenso) y la marque a mano en
quirófano. Resuelve el "cuánto" en promedio, pero no la variación punto a punto ni la
asimetría izquierda/derecha, que es justo lo que el usuario necesita reproducir. Y no
resuelve la colocación de orificios en hueso con soporte.

## Goals

- La guía refleja el movimiento planificado del Le Fort: en impactación indica la franja
  exacta de hueso a quitar por encima del corte, con su altura real en cada punto.
- La app propone sola los sitios de los orificios de fijación en hueso con buen soporte,
  y el profesional los puede mover.
- Los orificios de la guía quedan registrados como los orificios que después usarán las
  placas a medida.

## Non-goals / out of scope

- Diseñar o generar las placas a partir de estos orificios, y reproducir con ellas el
  avance o retroceso. Es la siguiente fase (decisión del usuario, 2026-10-04).
- Interferencias posteriores (tuberosidad, apófisis pterigoides, palatina descendente):
  la guía anterior no llega a esas zonas.
- Guías de BSSO y de mentoplastia.
- Medir la proximidad a raíces dentarias: los dientes no están segmentados por separado.
- Injerto óseo en los descensos.

## Users & context

El cirujano maxilofacial (o el planificador que trabaja con él) que ya hizo la osteotomía
Le Fort I y la REPOSICIÓN del segmento en la app. Ahora quiere una guía imprimible para
llevar ese plan al quirófano: que le diga dónde cortar, cuánto hueso quitar y dónde
taladrar los orificios que después ocuparán las placas.

## Behaviour

- **Qué movimiento cuenta:** la guía solo tiene en cuenta los cambios de plano del Le Fort:
  la subida o bajada vertical, la rotación horaria/antihoraria (sube o baja más adelante
  que atrás) y la inclinación de un lado respecto al otro. El avance o retroceso, el
  desplazamiento lateral y el giro en el plano horizontal no cambian la guía: los
  determinará la placa (decisión del usuario, 2026-10-04).
- **Main path — impactación:** al generar la guía, la app compara la posición del
  segmento Le Fort antes y después de ese movimiento a lo largo de toda la línea de corte
  que cubre la guía. Donde el segmento sube, la guía muestra la franja a quitar por encima
  del corte, con la altura que corresponde a ese punto, y lleva dos ranuras: una por el
  corte Le Fort y otra por el borde superior de la franja.
  La franja se ve en un color distinto en la vista 3D y su altura se puede consultar.
- **Main path — descenso:** donde el segmento baja, no hay franja: la guía lleva un solo
  corte, como hoy.
- **Main path — orificios:** la app propone, en cada lado, 2 orificios encima y 2 debajo
  del corte en el reborde piriforme y otros tantos en el pilar cigomático, cada uno en un
  sitio con buen soporte óseo. La guía lleva una camisa de broca en cada orificio.
- **Edición:** el profesional puede mover cualquier orificio propuesto. La guía se
  regenera con el orificio en su nuevo sitio, conservando la franja y el resto de
  orificios.
- **Edge — movimiento mixto:** si una parte del corte sube y otra baja (o un lado sube más
  que el otro), la franja existe solo donde sube y su altura varía con el movimiento real.
  Donde la subida es menor de 0.5 mm (el ancho de un corte de sierra) no hay franja.
- **Edge — sin sitio válido:** si en un pilar no hay hueso con soporte suficiente para
  todos los orificios propuestos, la app propone los que sí caben e informa de cuáles
  faltan y por qué.
- **Edge — reabrir el proyecto:** los orificios (propuestos o movidos) y la franja se
  conservan al guardar y reabrir; los proyectos anteriores siguen abriendo.
- **Error — orificio movido a mal sitio:** si el profesional mueve un orificio a un sitio
  con soporte insuficiente, la app lo marca, explica el motivo y respeta su decisión.
  Los orificios sobre un borde óseo libre siguen rechazándose como hoy.
- **Error — sin movimiento planificado:** si el Le Fort no tiene REPOSICIÓN, la app no
  inventa movimiento: lo dice y no genera franja.

## Acceptance criteria

- Given un Le Fort impactado 3 mm de forma uniforme, When se genera la guía, Then la
  franja a quitar mide 3 mm (±0.5 mm) de alto en toda la longitud del corte que cubre la
  guía, y queda por encima del corte.
- Given una impactación de 4 mm a la derecha y 1 mm a la izquierda, When se genera la
  guía, Then la franja mide 4 mm (±0.5) en el lado derecho y 1 mm (±0.5) en el izquierdo.
- Given una impactación anterior de 4 mm sin movimiento vertical posterior, When se genera
  la guía, Then la franja es más alta en el extremo anterior que en el posterior de cada
  lado.
- Given un avance de 5 mm sin ningún cambio vertical ni de plano, When se genera la guía,
  Then la guía tiene un solo corte y ninguna franja.
- Given un avance de 5 mm combinado con una impactación uniforme de 3 mm, When se genera la
  guía, Then la franja mide 3 mm (±0.5) en toda la guía, igual que sin el avance.
- Given un descenso de 3 mm, When se genera la guía, Then la guía tiene un solo corte y
  ninguna franja.
- Given un movimiento planificado y hueso suficiente, When se genera la guía, Then cada
  lado tiene 4 orificios en el reborde piriforme y 4 en el pilar cigomático (2 encima y 2
  debajo del corte en cada uno), todos con al menos 2.0 mm de hueso bajo el tornillo, y una camisa de broca en
  cada uno.
- Given una guía generada, When el profesional mueve un orificio a un sitio válido, Then
  la guía se regenera con la camisa en el nuevo sitio y el resto de orificios y la franja
  sin cambios.
- Given una guía generada, When el profesional mueve un orificio a un sitio con soporte
  insuficiente, Then el orificio se acepta, se marca como aviso y el informe da el motivo.
- Given una guía con orificios movidos, When se guarda y se reabre el proyecto, Then los
  orificios aparecen en las mismas posiciones y la franja es la misma.
- Given un Le Fort sin REPOSICIÓN, When se pide generar la guía, Then la app informa de
  que falta el movimiento y no muestra franja.
- Given cualquier guía generada, When se construye, Then sale de una sola pieza y cada
  camisa de broca tiene material de guía debajo.
- Given una guía generada, When el profesional la exporta, Then antes ha pasado por un paso
  de revisión visible (constitución, principio 15).

## Points to clarify

- **suposición tomada** — "Rotación antihoraria" es la que sube la parte anterior respecto a
  la posterior (impactación anterior) y "horaria" la que la baja, vistas desde la derecha
  del paciente, como en la convención ortognática. *Base:* tu imagen 1 (impactación
  anterior). *Riesgo:* solo cambia las etiquetas del informe, no la franja.
- **suposición tomada** — La inclinación de un lado respecto al otro (cant) cuenta como
  cambio de plano. *Base:* pediste que la franja refleje la impactación asimétrica entre
  lados. *Riesgo:* si no debe contar, la franja sería igual en ambos lados.
- **suposición tomada** — La franja se mide en la dirección vertical del plano de
  orientación del paciente (Frankfurt). *Base:* el informe de movimiento ya separa
  ascenso/descenso con esa referencia. *Riesgo:* con una rotación grande del plano oclusal
  la altura "vertical" difiere de la perpendicular al corte; cambiaría la tolerancia.
- **suposición tomada** — La regla actual que rechaza orificios en el borde óseo libre se
  mantiene como bloqueo; el aviso sin bloqueo solo aplica al soporte (grosor). *Base:*
  regla del usuario del 2026-09-20 ("no dejes que los orificios se coloquen en la
  orilla"). *Riesgo:* si el usuario quiere que todo sea aviso, cambia un criterio.
- **suposición tomada** — Los orificios del lado del segmento se proponen sobre el hueso
  en su posición preoperatoria (donde se taladra con la guía). La fase de placas los
  trasladará a la posición planificada. *Base:* el principio de las placas a medida que ya
  usa la app. *Riesgo:* ninguno para esta fase.
- **suposición tomada** — Esta guía sustituye a la que hoy se genera desde las placas con
  «Generar guía de corte». *Base:* el usuario pide que la guía salga del movimiento y deja
  las placas para después. *Riesgo:* si quiere conservar ambas rutas, hay que decidir cuál
  manda cuando hay placas.
- **decisión diferida** — Forma y número de placas, y reproducción del avance/retroceso
  con ellas: fase siguiente.
- **área no formulable** — Cómo se comportan los orificios propuestos cuando el
  profesional cambia después el movimiento en REPOSICIÓN (¿se recolocan, se conservan los
  movidos a mano?). Sé que habrá una pregunta; aún no está clara.

## Revisions

- **2026-10-04 — respuestas del usuario a los puntos abiertos** (incorporadas arriba):
  - Grosor mínimo de hueso bajo un tornillo: **2.0 mm**.
  - Por debajo de **0.5 mm** de subida no hay franja.
  - La guía lleva **dos ranuras** en la impactación: el corte Le
    Fort y el borde superior de la franja.
  - Solo cuentan los cambios de plano (vertical, rotación
    horaria/antihoraria, inclinación lateral); el avance lo determina la placa.
