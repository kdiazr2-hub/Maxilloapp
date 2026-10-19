---
type: evaluation
title: Flujo de ortognática y el escaneo abierto
description: Qué hace Atrás en cada paso, dónde falta un cierre, y cuándo cerrar el borde del escaneo. 2026-10-18.
tags: [plataforma, flujo]
timestamp: 2026-10-18T00:00:00Z
topic: plataforma
---

# Flujo de ortognática y el escaneo abierto

Fecha: 2026-10-18. Lectura de código. No se cambió `src/`.

## Cuándo cerrar el escaneo

Después del match, y también después del compuesto. No durante el registro.

El STL intraoral es una cáscara abierta hacia las raíces: el superior queda abierto hacia arriba y el inferior hacia abajo. El match (`alignUpperArchToMaxilla`, `alignLowerArchToMandible`) solo mueve esa cáscara. No la cierra, y no debe cerrarla: el cirujano todavía está viendo si el escaneo sentó sobre el hueso.

El compuesto es el momento. `CreateContourComposite` se queda con el escaneo desde la línea de puntos hacia las cúspides y añade un faldón de 2 mm (`kContourSkirtMm`) solo en el borde que cae sobre esa línea. El comentario en `contourSkirt` dice que los demás bordes abiertos del escaneo conservan su forma. Ese borde que se conserva es el que mira hacia las raíces: arriba en el maxilar (`axisZ` +Z) y abajo en la mandíbula (`axisZ` −Z).

Cerrar ese borde va antes de extraer el ICP a un core. El registro no es la causa. El cambio es de `CompositeBlockCore`, con un test de que el compuesto superior no deja un agujero hacia +Z ni el inferior hacia −Z, y de que el hueso fuera de la línea de puntos sigue en su sitio.

## Los nueve pasos

El riel ‹ › solo cambia de módulo. No deshace nada. El ✓ sale de `updateOrthognathicSteps`.

| Paso | Cómo se avanza | Cómo se marca hecho | Atrás dentro del paso |
|------|----------------|---------------------|------------------------|
| 1 Segmentación | Seguir al tener maxilar y mandíbula | Hay labelmap, o existen los objetos 5 y 6 | No hay. ‹ está apagado. |
| 2 Modelos | Importar, puntos, match, crear compuesto, por arcada | Los dos compuestos existen y no hay un bloque a medias | Sí: «Atrás: rehacer el paso». Es el único que deshace trabajo. |
| 3 Orientación | Porion, porion, orbitale, orbitale, alinear, línea media opcional, «Guardar orientación y seguir» | Existe la copia de la mandíbula orientada | No hay botón de deshacer. «Guardar» salta a Osteotomía. |
| 4 Osteotomías | Tipo, hueso, puntos, aplicar corte, «Finalizar osteotomías» | Hay segmento de Le Fort, BSSO o mentón | «Atrás» del asistente solo entre tipo y el corte. En el paso final no hace nada: el corte ya está aplicado. |
| 5 Mordida | Puntos y registro del Le Fort y de la mandíbula | Basta con que uno de los dos quede registrado | No hay deshacer del registro. Cargar otro STL de mordida borra la copia de mandíbula orientada. |
| 6 Reposición | Gizmo, traslación y giro. «Restablecer» vuelve el segmento. | Nunca. El ✓ está fijo en falso. | No hay «Terminé». ‹ › salen del módulo con el movimiento ya aplicado. |
| 7 Férulas | Diseñar y crear | Existe el objeto de algún diseño | No hay deshacer del paso. |
| 8 Guías | Envolvente, pintura, orificios, ranuras, crear | Hay malla de guía | «Anterior» baja un índice del asistente y no borra pintura ni orificios. En la guía Le Fort del asistente ese botón está oculto. |
| 9 Placas | Orificios y construir, en el mismo panel | Hay placas en el plan | El mismo «Anterior» de guías. |

## Dónde Atrás se siente trabado o deshace de más

Todo esto está en `goBackModelWorkflow`, salvo el corte.

- Si está capturando puntos, el primer Atrás solo cancela la captura y se queda. Hay que pulsarlo otra vez para deshacer el paso. La barra dice «Puede escoger el paso anterior».
- Si el gizmo del escaneo está activo, el primer Atrás lo acepta («Ajuste manual aceptado antes de volver») y no lo tira. Deshacer el registro pide un segundo Atrás.
- Mientras el compuesto se calcula, Atrás está apagado.
- Descartar el compuesto no borra `m_orientedInitialMandibleMeshForSplint`. El ✓ de Orientación puede seguir puesto con el compuesto ya tirado.
- En osteotomía, al pulsar «Aplicar corte» el hueso ya quedó partido. Atrás en «Finalizar» no entra: la condición exige estar antes de ese paso.
- ‹ desde Orientación o más adelante no gira los modelos de vuelta ni bloquea el paso. Se puede volver a Modelos y descartar un compuesto que ya se orientó.

El orden de los botones de Modelos y de Orientación sí sigue la secuencia. Lo que falta es un cierre explícito en Reposición, y un Atrás que deshaga en todos los pasos lo que el de Modelos deshace en el suyo.
