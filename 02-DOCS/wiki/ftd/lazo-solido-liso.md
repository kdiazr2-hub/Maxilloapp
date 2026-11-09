---
author: Kevin
branch: registro-arcada-preciso
status: in-progress
---

# Lazo sólido sin escalones

## Intent

Al cortar una pieza cerrada con el lazo en Laboratorio, las caras que el trazo no toca tienen que
quedar tan lisas como estaban. Salían con relieve en terrazas, un escalón por vóxel
(usuario, 2026-11-09, captura de la Base tras dos cortes).

## Scope

- `LabCore::LassoCutSolid` (`DicomMPRViewer/src/LabCore.cpp`): la distancia de la pieza se suaviza
  1,2 vóxeles antes de contornear. El trazo no se suaviza, para que el corte siga nítido.
- Fuera: el borrador de triángulos para escáneres abiertos y piezas grandes
  (`ScanEraseCore::ErasePolygon`), y la creación de la Base.

## Checklist

- [x] Una prueba detecta las terrazas: cuenta pliegues de más de 12° lejos del corte, antes y después.
- [x] El corte deja las caras intactas lisas.
- [x] La pieza cortada sigue cerrada, en una pieza, sólida y sin entrar en el objeto.
- [ ] La app está recompilada con el cambio.
- [ ] Comprobado en la app sobre el caso real.

## Evidence

- Rojo, antes del cambio: `LabCoreTests: the lasso left the untouched faces terraced: 52804 creases,
  1729 before the cut`.
- Verde, después: `LabCoreTests: all passed`; el límite de la prueba es 2 × antes + 400 pliegues.
- `ctest -R "LabCoreTests|ScanEraseTests|GuideBaseTests"`: 3 de 3 pasan (2026-11-09).
- Las comprobaciones de cerrada, una sola cáscara, sólida junto al corte y ningún vértice dentro
  del objeto son las de `testBaseSitsOnTheObjectAndCutsSolid` y siguen pasando.
- Enlazado de `DicomMPRViewer.exe`: falló con LNK1104 porque la app estaba abierta.

## Next

Otra sesión desconectó el corte sólido de la vista mientras se hacía este cambio
(`MainWindowLab.cpp`: `m_labView->setLassoSolidCut({})`), por la misma queja sobre el tubo guía:
la reconstrucción dejaba la pared estriada. El lazo vuelve a borrar caras y tapar el hueco, así que
este arreglo está probado en el núcleo pero la app no lo usa. Decide el usuario si se reconecta
ahora que la reconstrucción sale lisa.

Después: cerrar la app, recompilar el objetivo `DicomMPRViewer` y repetir el corte sobre la Base del caso
real. Si la Base recién creada ya trae relieve antes del primer corte, el origen es otro y hay que
mirarlo en `GuideBaseCore::CreateBase`.
