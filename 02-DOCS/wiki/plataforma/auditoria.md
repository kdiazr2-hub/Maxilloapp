---
type: audit
title: Auditoría de plataforma — 2026-10-18
description: Estado real de MaxilloApp antes de tratarlo como plataforma multi-especialidad. Inspección de código. Sin reescritura.
tags: [plataforma, auditoria]
timestamp: 2026-10-18T00:00:00Z
topic: plataforma
---

# Auditoría de plataforma

Fecha: 2026-10-18. Rama inspeccionada: `claude/eager-rubin-zc74k4`.
Fuente: `DicomMPRViewer/` (CMake, `src/`, `tests/`) y `02-DOCS/wiki/`. `ctest` no se volvió a ejecutar en esta pasada. El inventario de tests sale de `CMakeLists.txt`.

Decisiones tomadas ese día: `sdd/decisions.md`, entrada «Plataforma modular».

## 1. Arquitectura actual

Aplicación de escritorio en `DicomMPRViewer`. C++17, Qt 6 Widgets (`Core`, `Gui`, `Widgets`, `Concurrent`), VTK y GDCM opcional (`USE_GDCM`). La constitución fija Qt 6.11 y VTK 9.5.2; el `find_package` principal no fija esas versiones. No hay QML, frontend web, API, base de datos ni autenticación.

| Capa | Qué hace | Dónde |
|------|----------|--------|
| Arranque | `QApplication`, splash, formato OpenGL | `src/main.cpp` |
| DICOM | Indexa, cachea `.mha`, carga con GDCM en un hilo | `AsyncDicomLoader`, `VolumeCacheManager` |
| MPR | Tres `vtkResliceImageViewer` y medidas 2D | `MPRView`, `Measurement*` |
| Visor 3D | `QVTKOpenGLNativeWidget`, una vista por paso | `Mesh3DView` |
| Segmentación | Proceso Python local, nnU-Net DentalSegmentator | `scripts/run_standalone_dental_segmentator.py` |
| Mallas | Marching cubes; suelo 0.55 si el suavizado es alto | `MeshGenerator` |
| Plan | JSON `.maxilloproject` versión 1 y carpeta `_assets` | `ProjectSerializer` |
| UI | Ribbon y rail de 9 pasos, texto en español | `MainWindowOrthognathic.cpp` |

El volumen está en LPS, en milímetros. Anterior es −Y. Superior es +Z. Frankfurt (`alignFrankfurtPlane`) rota el plano de Porion y Orbitale hasta la normal Z+. Las rígidas son `vtkMatrix4x4` (`TransformCore`: mm y grados). El estado del caso es el archivo de proyecto más los miembros de `MainWindow`. No hay un almacén de sesión aparte del widget.

El identificador de paciente es el tag DICOM mostrado en pantalla. La caché vive en `AppLocalData/DicomCache`. La exportación pide una ruta al usuario.

`MainWindow.cpp` ronda las 14.800 líneas. `MainWindowGuides.cpp`, unas 4.200. La lógica de Frankfurt y del registro de mordida está en esa ventana, no en un core.

## 2. Funcionalidades

### Ortognática — disponible

Los nueve pasos tienen clases, interfaz y tests. No son etiquetas vacías.

| Paso | Evidencia | Hueco |
|------|-----------|--------|
| Segmentación | Carga, IA local, `MeshGenerator`, `MaskToObjectCore` | Hay más de un camino de IA. El que usa la app es DentalSegmentator. |
| Modelos | `ModelWorkflowCore`, `CompositeBlockCore`, ICP de arcadas | El flujo guiado está en el panel, no en el ribbon. |
| Orientación | `alignFrankfurtPlane` | La matemática está en `MainWindow`. `LandmarkCore` solo lo usan los tests. |
| Osteotomía | `OsteotomyCore`: Le Fort I, BSSO, genioplastia | El ribbon viejo sigue compilado y oculto. |
| Registro de mordida | STL, puntos homólogos, ICP, gizmo | También vive en `MainWindow`. |
| Reposición | Gizmo, `CollisionCore`, fantasma preoperatorio | El paso nunca marca hecho. |
| Férulas | `SplintHeightmapGenerator`, Intermedia y Final | `SplintGenerator` sigue compilado. El selector de método está oculto. |
| Guías | Le Fort por movimiento, asistente de cuatro pasos, mentón pintado | El mentón no tiene el asistente ni las placas del maxilar. |
| Placas | `PlateCore`, agujeros predictivos, informe | Faltan grosor óseo leído del TC, interferencia posterior y exactitud postoperatoria. |

Medidas en el MPR: distancia, ángulo, área, perpendicular, ROI en Hounsfield, anotación, JSON. Cefalometría no está conectada a la ventana.

Las medidas se abren desde ARCHIVO. Los módulos IMAGE y MEDIDAS del ribbon están fuera de compilación (`if constexpr (false)` en `MainWindow.cpp`). `CLAUDE.md` todavía describe MEDIDAS como pestaña visible.

### Otras especialidades — ausentes

Oncología maxilofacial, trauma facial y anomalías craneofaciales no tienen clases, specs ni pruebas. Definirlas en un documento no las implementa.

## 3. Riesgos

- `MainWindow.cpp` concentra el producto. Un módulo nuevo ahí multiplica la regresión.
- Frankfurt y mordida no tienen core propio. La red es el workspace test.
- `CLAUDE.md` está desfasado en la pestaña MEDIDAS y en la frase de que la app no llama a `ImplicitCore`. El código de guías sí lo llama.
- La constitución prohíbe subir DICOM y proyectos reales. `.gitignore` no lista `*.dcm` ni `*.maxilloproject`.
- No hay `.github/workflows`. La barra de `ctest` depende de la máquina y, en los workspace tests, de la GPU discreta.
- `WrapCore::Wrap` y `ImplicitCore::BakeMeshField` de guías y placas se invocan en el hilo de la ventana.
- Osteotomía de ribbon y férula clásica siguen compiladas. Borrarlas quedó descartado el 2026-10-18.

## 4. Componentes reutilizables

`Mesh3DView`, `MPRView`, `TransformCore`, `MeshGenerator`, el pipeline DICOM, `ImplicitCore`, `WrapCore`, `OsteotomyCore`, `CollisionCore`, `MeshRepairCore`, `CompositeBlockCore`, la familia de guías (`GuideBaseCore`, `CutSlotCore`, `GuideDesignCore`, `GuideSculptCore`, `LeFortGuideCore`, `LeFortMotionCore`, `LeFortHoleCore`), `PlateCore`, `ProjectSerializer`, `CranioPalette` y las medidas del MPR.

`ObjectLabels.h` registra las piezas del caso ortognático (etiquetas 3 y 201–218). No es un catálogo de otras especialidades.

## 5. Arquitectura objetivo

Se queda el stack actual. Un cambio de librería 3D, de base de datos o un backend sería una enmienda mayor de la constitución.

- **Núcleo.** Caso local, visor LPS en mm, cores sin widgets, exportación tras el paso de revisión que ya existe.
- **Módulo clínico.** Declara etapas, herramientas, parámetros y una clave opcional del proyecto. No posee el visor y no depende de otro módulo clínico.
- **Ortognática** es el primer módulo a extraer de `MainWindow`, cuando se haga esa tarea. Hoy el contrato se llama `MainWindow`.
- Oncología, trauma y anomalías esperan. La candidata siguiente, cuando el usuario abra esa fase, es trauma facial, porque reutiliza transformaciones rígidas y colisión. Esa spec no está escrita.

El sistema visual sigue en `CranioPalette` y Segoe UI Variable. Espaciado en pasos de 4 px. El visor ocupa el área de trabajo. El modo claro que pide la constitución §16 no se rehace en esta etapa.

## 6. Plan de migración

1. Auditoría. Este documento.
2. Contratos y extracción mínima: Frankfurt y mordida a cores con test, antes de sumar especialidades.
3. Paleta y navegación: claro/oscuro con `CranioPalette`, sin cuatro productos.
4. Huecos ortognáticos: la marca de reposición y la analítica de placas que falta. El código legado oculto se queda.
5. Una especialidad, spec revisada por el revisor clínico, y solo entonces código.
6. Regresión, privacidad (`*.dcm`, `*.maxilloproject` fuera de git) y CI cuando se elija el runner.

No se desarrollan los cuatro módulos en paralelo.

## 7. Equipo

Nueve subagentes de Cursor en `.cursor/agents/`. Se invocan con `/nombre` en el chat. No quedan ejecutándose solos. Dos agentes no editan el mismo archivo en la misma tarea.

| Archivo | Rol |
|---------|-----|
| `product-lead.md` | Backlog, criterios, un escritor por archivo |
| `ux-ui.md` | Paleta, jerarquía, estados de interfaz |
| `arquitecto-3d.md` | Visor, mallas, LPS, cores |
| `frontend.md` | Qt y orquestación de `MainWindow` |
| `datos.md` | `.maxilloproject` y caché local |
| `flujos-clinicos.md` | Specs. No escribe C++ |
| `qa.md` | Tests. No implementa la función que verifica |
| `seguridad.md` | Revisión de PHI y exportación. Solo lectura |
| `revisor-clinico.md` | Terminología. No valida clínicamente. Solo lectura |

## 8. Backlog siguiente

Ninguna de estas tareas está hecha por el hecho de estar listada.

| Id | Trabajo | Responsable |
|----|---------|-------------|
| P1 | Ignorar `*.dcm` y `*.maxilloproject` sin ocultar fixtures sintéticos | seguridad propone; otro aplica |
| P2 | Inventario de métodos geométricos dentro de `MainWindow`, sin editarlos. Hecho: `plataforma/inventario-mainwindow.md` | arquitecto 3D y frontend |
| P2 | Extraer Frankfurt y mordida a cores con test | arquitecto 3D, luego frontend |
| P3 | Claro/oscuro con `CranioPalette` | UX y frontend |
| P4 | Condición real para marcar reposición hecha; nombrar la analítica de placas que falta | frontend, flujos, revisor clínico |

## 9. Sigue abierto

- **D7.** Cuándo hacer el modo claro. Recomendación: en la fase 3, no como rediseño del ribbon. La constitución ya lo exige.
- **D9.** Historial de ediciones del proyecto. Recomendación: seguir guardando solo la última versión.
