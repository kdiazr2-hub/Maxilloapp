---
type: spec
title: Orientación — plano de los cuatro puntos
description: Paso Orientación de ortognática, ya en el producto. Gira los dos compuestos con los cuatro puntos que marca el cirujano. No diagnostica.
tags: [plataforma, ortognatica, orientacion]
timestamp: 2026-10-18T00:00:00Z
topic: plataforma
status: disponible
---

# Orientación — plano de los cuatro puntos

Fecha: 2026-10-18. Paso ya existente de ortognática, el tercero de los nueve: después de Modelos y antes de Osteotomía. Estado del paso: **disponible**. No abre una especialidad.

El cirujano marca cuatro puntos. El software ajusta un plano a esos puntos, lo deja horizontal y gira los dos compuestos. Con eso quedan orientados los modelos para el resto del módulo. El resultado es una rotación rígida de las mallas. No es un diagnóstico, no es una medida cefalométrica y no afirma que el plan sirva para operar.

La etiqueta del botón es «Alinear al plano de Frankfort». Lo que calcula el programa es el plano de los cuatro clics, en milímetros, en el marco LPS (superior +Z, anterior −Y).

## Qué marca el cirujano

Los puntos se guardan en `m_frankfurtPoints`, en este orden, y el panel los pide uno a uno. Al entrar en Orientación solo está activo el primero; cada clic abre el siguiente (`MainWindow::onFrankfurtPointPicked`, `MainWindow::updateFrankfurtPointStatus`).

| Índice guardado | Punto | Vista que abre el botón |
|-----------------|--------|-------------------------|
| 0 | Porion derecho | Lateral derecha |
| 1 | Porion izquierdo | Lateral izquierda |
| 2 | Orbital derecho | Frontal |
| 3 | Orbital izquierdo | Frontal |

En pantalla los dos últimos botones se llaman «Orbitale derecho» y «Orbitale izquierdo». El texto del panel pide el punto más alto del conducto auditivo y el punto más bajo del reborde orbitario. Esa frase orienta el clic. El programa no comprueba que el punto caiga en esa anatomía: guarda la coordenada del clic.

«Alinear al plano de Frankfort» se habilita con los cuatro puntos. Si faltan, `MainWindow::alignFrankfurtPlane` avisa y no gira nada.

## Qué hace el ajuste

Con los cuatro puntos, `MainWindow::alignFrankfurtPlane` hace tres cosas.

1. **Plano.** Ajusta la normal por el método de Newell. El polígono no sigue el orden de los clics: recorre Porion derecho → Orbital derecho → Orbital izquierdo → Porion izquierdo. Así el cuadrilátero queda convexo y cubre el plano de esos puntos.
2. **Horizontal.** Rota esa normal hasta +Z (superior). Si la normal ya apunta hacia arriba, el giro de nivelación es nulo. Si apunta hacia abajo, el giro es de 180° alrededor de X.
3. **Eje porion–porion.** Sobre el plano ya horizontal, toma Porion izquierdo − Porion derecho, anula su componente Z y gira alrededor de Z hasta dejar ese eje sobre X. El ángulo de esa proyección se pliega a ±90° antes de aplicar el giro, para no dar media vuelta de más. Si los dos porion coinciden en el plano horizontal, ese segundo giro es cero.

El pivote de las dos rotaciones es el centro de la caja que engloba los dos compuestos (`m_upperCompositeMesh` y `m_lowerCompositeMesh`). Cada giro es trasladar al pivote, rotar y volver. Las dos rotaciones se aplican, en ese orden, a los dos compuestos y a los cuatro puntos.

El enlace hueso/diente del compuesto se conserva: `SplintWorkspaceTests` exige que `CompositeBlockCore::HasParts` siga en el compuesto superior después de alinear.

## Qué se ve y qué no se afirma

Después del giro, la ventana dibuja en la vista de orientación un disco translúcido (etiqueta −100, normal +Z, centro en el promedio de los cuatro puntos ya girados), una línea horizontal, los dos segmentos porion–orbital y los cuatro marcadores. Luego pasa a la vista lateral. Ese dibujo es una referencia en pantalla. No es una medida y no sale como objeto clínico.

A continuación, en el mismo panel, el cirujano puede usar el gizmo de línea media (opcional) y «Guardar orientación y seguir». El gizmo mueve los dos compuestos juntos. Guardar deja el compuesto inferior como referencia de la férula y pasa a Osteotomía. Ni el gizmo ni el guardado miden un ángulo cefalométrico.

Este paso orienta modelos. SNA y SNB no existen en el producto. Cefalometría no forma parte de Orientación ni del módulo. Nada en esta pantalla dice que la posición resultante sea la correcta para operar.

Los pasos posteriores de la misma ortognática trabajan sobre esos compuestos ya girados: `OsteotomyCore`, el registro de mordida, la reposición, `SplintHeightmapGenerator`, `LeFortGuideCore` y `PlateCore`. `LeFortMotionCore` toma +Z de este marco como vertical al describir un movimiento. Ninguno de ellos vuelve a localizar los cuatro puntos ni certifica el plano.

Los cuatro puntos no tienen clave propia en `ProjectSerializer`. Lo que queda guardado es la geometría de los compuestos después del giro.

## Estado

| | Disponible hoy | Requiere desarrollo | Requiere validación clínica |
|---|----------------|---------------------|-----------------------------|
| Plano de los cuatro puntos | `OrientationCore::AlignFrankfurt` calcula las dos matrices. `MainWindow::alignFrankfurtPlane` las aplica a los compuestos. El panel está en `MainWindowOrientation.cpp`. `OrientationCoreTests` cubre el plano horizontal, una inclinación, el giro de los porion y cuatro puntos iguales. | El registro por ICP sigue en `MainWindow`. `TransformCore` no calcula este plano. | Afirmar que ese plano es el Frankfort anatómico del paciente. El software solo usa los cuatro puntos que marcó el usuario. |
| Dibujo del disco, tubos y marcadores | Lo dibuja la misma función, en la vista de orientación. | Se queda en la ventana. No entra en el core. | El disco no es una comprobación anatómica. |
| Línea media opcional y guardar | Gizmo y «Guardar orientación y seguir», en el mismo panel, después de alinear. | Sin cambio de alcance en este documento. | El gizmo no define una línea media anatómica ni autoriza el plan. |

### Extracción hecha

`OrientationCore::AlignFrankfurt` es la función pura: entran los cuatro puntos en el orden guardado y el centro de la caja; salen la rotación que lleva la normal de Newell a +Z y la que deja el eje porion–porion sobre X, con el pliegue a ±90°. `alignFrankfurtPlane` la llama y sigue dibujando el disco, los tubos y los marcadores. `OrientationCoreTests` usa puntos sintéticos.

Ese test geométrico no cierra la columna clínica. «Orbital» frente a «Orbitale» en algún aviso es deuda de texto; este cambio no renombra botones.

### Límite clínico

El plano del programa coincide con los cuatro clics y con la construcción de arriba. Llamarlo el Frankfort anatómico verdadero de ese paciente es otra afirmación: exige que un revisor clínico juzgue si esos puntos, marcados a mano, son los puntos anatómicos. El software no hace esa comprobación y este documento no la da por hecha.

El revisor clínico lee este texto antes de cerrarlo. Un test de geometría no valida un criterio clínico.
