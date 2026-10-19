---
type: inventory
title: Geometría que todavía calcula MainWindow
description: Inventario de la matemática clínica que vive en MainWindow. Frankfurt está en OrientationCore; el registro no. 2026-10-18.
tags: [plataforma, arquitectura]
timestamp: 2026-10-18T00:00:00Z
topic: plataforma
---

# Geometría que todavía calcula MainWindow

Fecha: 2026-10-18. Cero cambios en `src/`. Las líneas son las de ese día y se mueven; el nombre de la función es la referencia.

`TransformCore` ya sabe componer matrices, aplicarlas a una malla y describir un movimiento rígido. No registra por puntos ni por ICP: eso sigue dentro de `MainWindow.cpp`. El plano de Frankfurt ya no: lo calcula `OrientationCore::AlignFrankfurt`.

## Extraer, en este orden

Son el camino guiado que el cirujano usa. Ninguna tiene core todavía.

| Orden | Función | Líneas | Qué calcula | Quién la llama | Estado |
|------|---------|--------|-------------|----------------|--------|
| 1 | `alignFrankfurtPlane` | llama a `OrientationCore::AlignFrankfurt` | Plano de Newell. La ventana conserva el pivote (centro de los compuestos), el disco y los marcadores. | Acción «Alinear al plano de Frankfort» | extraída |
| 2 | `transformMesh` | 9492–9571 | `vtkLandmarkTransform` rígido de al menos 3 pares. Devuelve la malla, la matriz y el RMS en mm. | Registro de arcadas y de mordida | sin extraer |
| 3 | `refineArchWithIcp` | 9636–9815 | Recorta un ROI alrededor del STL (margen XY 12–28 mm; Z distinto arriba y abajo) y corre ICP rígido, 80 iteraciones. Informa RMSE. | `alignUpperArchToMaxilla`, `alignLowerArchToMandible` | sin extraer |
| 4 | `refineBiteRegistrationWithIcp` | 3551–3814 | El mismo recorte y el mismo ICP, más un filtro que se queda con las celdas cercanas a los puntos de la mordida. | `alignLeFortToBiteScan`, `alignMandibleDistalToBiteScan` | sin extraer |

La matemática de Frankfurt ocupa hasta la línea 10720. De ahí al 10835 solo actualiza la vista: disco, tubos y marcadores. Eso se queda en la ventana.

`refineArchWithIcp` y `refineBiteRegistrationWithIcp` copian `cleanTri` y `cropByBox`. Un solo core de registro, con la política de ROI como parámetro, evita que las dos copias diverjan. Frankfurt ya está aparte, en `OrientationCore`.

## Extracción 1 — Frankfurt

**ORI-1.** Hecha. `OrientationCore::AlignFrankfurt` calcula el plano. `alignFrankfurtPlane` lo aplica y sigue dibujando el disco. El registro ICP no entra.

**Objetivo.** La ventana deja de calcular el plano. `alignFrankfurtPlane` llama a `OrientationCore::AlignFrankfurt` y se queda con el disco, los tubos y los marcadores.

**Contexto.** Entran cuatro puntos (Porion D, Orbital D, Orbital I, Porion I) y el centro de los dos compuestos; salen la matriz de nivelación y la de guiñada. El registro por puntos y el ICP de arcada y de mordida no entran. El ribbon viejo y `SplintGenerator` no se borran. Cefalometría sigue fuera.

**Archivos permitidos.** `DicomMPRViewer/src/OrientationCore.h`, `DicomMPRViewer/src/OrientationCore.cpp`, `DicomMPRViewer/tests/OrientationCoreTests.cpp`, `DicomMPRViewer/CMakeLists.txt`, `DicomMPRViewer/tools/linux-core-tests/CMakeLists.txt`, y solo el cuerpo matemático de `alignFrankfurtPlane` (hasta la línea 10720 en este inventario). El disco y los marcadores se quedan en la ventana.

**Dependencias.** Ninguna sobre el registro. Reposición no se toca.

**Criterios de aceptación.**

- Un plano que ya está horizontal no mueve las mallas.
- Una inclinación conocida vuelve a Z+.
- Cuatro puntos iguales no giran las mallas.
- El disco y los marcadores siguen en la ventana.
- Los proyectos antiguos abren, porque no hay clave nueva.

**Prueba.** `OrientationCoreTests`. Este inventario no sustituye `ctest`.

**Riesgo.** Que el eje de Newell cambie de orden y la cara quede boca abajo.

## Se queda en la ventana

Construyen una matriz pequeña y luego llaman a algo que ya existe. Extraerlas antes del registro no reduce el riesgo.

| Función | Líneas | Qué hace | Ya delega en |
|---------|--------|----------|----------------|
| `translateRepositionTarget` | 5401–5422 | Traslación del segmento, respetando la restricción de solo rotación | `applyRepositionTransform` |
| `rotateRepositionTarget` | 5424–5468 | Giro alrededor del pivote o del centro de la caja | `applyRepositionTransform` |
| `recordMandibleMovement` | 5196–5206 | Acumula el delta y describe mm y grados | `TransformCore::DescribeRigidMovement` |
| `updateRepositionMeasurements` | `MainWindowReposition.cpp` 147–177 | Impactación por punto del corte | `OsteotomyCore::MeasureLandmarkMovement` |
| `applyOsteotomyCut` | `MainWindowOsteotomy.cpp` | Parte Le Fort, BSSO o mentón | `OsteotomyCore::SplitByPath` / `SplitBsso` |
| `calculateBlockComposite` | `MainWindowComposite.cpp` 501 | Arma el compuesto | `CompositeBlockCore` |
| `autoFillSplintGuidePoints` | `MainWindowSplint.cpp` 809–834 | Cinco puntos de cúspide | `SplintDesignCore::AutoGuidePoints` |
| Envolvente y hueso de guías | `MainWindowGuides.cpp` ~1257, ~1270, ~3152 | Wrap y campo de distancia | `WrapCore`, `ImplicitCore::BakeMeshField` |

`identityMatrix` (195) y `matrixToText` (202) repiten `TransformCore::IdentityMatrix` y `MatrixToString`. Son de informe, no de clínica.

## No extraer

| Símbolo | Dónde | Por qué |
|---------|-------|---------|
| `booleanComposite` | 9839, declarado en `MainWindow.h` 576 | No tiene ninguna llamada. El compuesto real es `CompositeBlockCore`. |
| `fitPlaneFromPolyData` | 259; usos en 12801, 12859, 12968, 13201 | Sirve a los planos del ribbon viejo de osteotomía, que sigue oculto a propósito. |
| `createSplint` | 4764 | Camino del `SplintGenerator` clásico. El método ofrecido es el mapa de alturas. |
| `#include <vtkBooleanOperationPolyDataFilter.h>` | línea 106 | El filtro no se usa en este archivo. El include sobra. |

Borrar esas piezas es la misma decisión que dejó el ribbon y `SplintGenerator` en su sitio: no se hace en este inventario.

## Siguiente extracción, cuando se pida

El registro (puntos + ICP de arcada y de mordida) va después de ORI-1, en un solo core, porque las dos funciones de ICP están duplicadas. No está pedido y no está hecho. Reposición no entra.
