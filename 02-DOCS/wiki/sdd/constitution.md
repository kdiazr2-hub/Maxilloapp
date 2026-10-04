---
type: constitution
title: MaxilloApp — Constitución
description: Los principios no negociables que obedece cada fase rsc-sdd.
tags: [sdd, constitution]
timestamp: 2026-10-04T00:00:00Z
topic: sdd
version: v1.0.0
---

# MaxilloApp — Constitución

> Versión: v1.0.0 · Ratificada: 2026-10-04 · Última enmienda: 2026-10-04
> Principios no negociables que obedece cada fase rsc-sdd. La mecánica concreta
> (build, tests, arquitectura, geometría) vive en el `CLAUDE.md` de la raíz; este archivo
> ratifica el principio y apunta al detalle.

## 1. Stack canónico

1. La app es C++17 · Qt 6.11 · VTK 9.5.2 · GDCM (vcpkg) · Windows x64 con MSVC, configurada
   con CMake (`DicomMPRViewer/CMakeLists.txt`). Cambiar cualquiera de estas piezas o sus
   versiones mayores es una enmienda MAJOR. Detalle: `CLAUDE.md` § Build & test.
2. La segmentación por IA es Python (nnU-Net DentalSegmentator, entorno conda `dentalgpu`)
   lanzada como proceso externo desde `scripts/`; la app C++ no enlaza Python.
3. Todo fuente se compila con `/utf-8`; cada target nuevo de `CMakeLists.txt` lo incluye.

## 2. Barra de calidad

4. El build Release compila sin errores y `ctest -C Release` pasa completo antes de integrar.
5. La lógica nueva va en clases core sin widgets de Qt (patrón `TransformCore`,
   `SplintGenerator`) y entra con al menos un test registrado con `add_core_test()`.
   `MainWindow*.cpp` solo orquesta la UI.
6. Todo bug corregido trae un test que fallaba antes del arreglo (rojo → verde).

## 3. Convenciones

7. El texto visible para el usuario está en español.
8. Toda medida geométrica está en milímetros reales y se mide contra el hueso, nunca contra
   un wrap. Detalle: `CLAUDE.md` § Architecture (HOW TO MEASURE).
9. Los `.maxilloproject` antiguos siguen abriendo: toda clave nueva de `ProjectSerializer` es
   opcional y tiene valor por defecto.
10. Mensajes de commit: gitmoji + una frase descriptiva en inglés que diga qué cambia
    (`🐛 No screw on a bony margin`). Enforced by `gitmoji-guard`.

## 4. Ramas y entrega

11. Todo cambio se hace en una rama desde `main` y entra por PR; nunca se sube directo a `main`.
    Enforced by `branch-guard` / `trunk-policy`.
12. El autor de cada commit y PR es el humano dueño del repo. Se permite y se conserva la
    línea `Co-Authored-By` de la IA cuando la IA escribió el cambio, para trazabilidad.
    (Sustituye la regla fija de rsc "sin coautor IA"; decisión del usuario, 2026-10-04.)

## 5. Seguridad y privacidad

13. Nunca se suben secretos; se cargan desde `01-TOOLS/<proveedor>/.env` (gitignored).
14. Nunca se suben datos de pacientes (DICOM, STL de casos reales, `.maxilloproject` con
    datos identificables) ni los pesos del modelo de IA. Los tests usan geometría sintética
    (`tests/SplintTestGeometry.h`).

## 6. Seguridad clínica y UX

15. Ninguna salida que vaya a fabricarse (férula, guía, placa) se exporta sin que el usuario
    la haya revisado en un paso visible: los flujos guiados no saltan su paso de revisión.
16. La UI funciona en modo claro y oscuro. El código de UI nuevo o tocado toma sus colores de
    `CranioPalette`, sin `QColor` fijos nuevos (los existentes se migran cuando se tocan).

## 7. Conocimiento y decisiones

17. Toda decisión significativa se anota en `02-DOCS/wiki/sdd/decisions.md` (fecha, opciones,
    porqué). La constitución es el registro de decisiones de mayor rango.
18. Al añadir una clase core o cambiar un flujo, se actualiza `CLAUDE.md` § Architecture en el
    mismo cambio.

## Definition of Done (la barra que corre `verify`)

Un cambio se integra solo si se cumple TODO:

- [ ] Build Release limpio y `ctest` completo en verde (principio 4).
- [ ] Lógica nueva en core con test; bug con test de regresión (principios 5-6).
- [ ] UI en español, medidas en mm contra hueso, proyectos antiguos abren (principios 7-9).
- [ ] Commit gitmoji, en rama con PR, autor humano (principios 10-12).
- [ ] Sin secretos ni datos de pacientes (principios 13-14).
- [ ] Paso de revisión antes de exportar; claro/oscuro correctos (principios 15-16).
- [ ] Decisiones anotadas y `CLAUDE.md` al día (principios 17-18).

## Registro de enmiendas (solo se añade)

| Fecha | Versión | Cambio | Porqué |
|-------|---------|--------|--------|
| 2026-10-04 | v1.0.0 | Constitución inicial ratificada. | Instalación del harness rsc. |
