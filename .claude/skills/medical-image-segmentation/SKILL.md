---
name: medical-image-segmentation
description: "Use when working on MaxilloApp's maxillofacial CT segmentation — DentalSegmentator (teeth, soft tissue, canal) and TotalSegmentator (maxilla, mandible), the label map, separating teeth / soft tissue / bone masks, filling the medullary bone between the cortices, thin or perforated walls, or deciding whether a segmentation can be accepted. Diagnosis is static first: code, logs, saved masks, unit tests; no segmenter is run without explicit authorisation. NOT surface meshing or smoothing (that is `mesh-reconstruction-3d`), NOT GPU limits (that is `gpu-safe-execution`), NOT Python/SimpleITK conventions (that is `python-medical-imaging`)."
tags: [segmentation, ct, maxillofacial, dentalsegmentator, totalsegmentator, nnunet, masks, maxilloapp]
recommends: [gpu-safe-execution, python-medical-imaging, mesh-reconstruction-3d]
origin: maxilloapp
---

# medical-image-segmentation — máscaras separadas, hueso macizo, validado antes de aceptar

## 1. Qué hay hoy en el proyecto

- `DicomMPRViewer/scripts/run_standalone_dental_segmentator.py`: DentalSegmentator con nnU-Net, lanzado por
  `StandaloneDentalSegmentatorService`. Etiquetas de DentalSegmentator → app (`APP_LABEL_MAP`):
  1 maxilar/cráneo → 5, 2 mandíbula → 6, 3 dientes superiores → 5, 4 dientes inferiores → 6, 5 canal → 7.
  Los dientes superiores salen además en el sidecar `<salida>_dientes_superiores.nrrd`.
- Posprocesado del hueso, en CPU:
  - `fill_bone_interiors`: rellena **solo** cavidades cerradas en 3D, de radio ≤ 6 mm y sin ningún vóxel < −300 HU;
  - `thicken_thin_bone` y `bone_thickening_params`: el maxilar sella hasta 3,5 mm / −700 HU, la mandíbula 2,5 mm / −200 HU;
  - `cap_canal_openings`.
- `fill_medullary_bone` (2026-10-19): sella los poros solo para la prueba, y rellena lo que la cortical encierra.
  Desactivable con `DENTALSEGMENTATOR_MEDULLA_FILL=0`. Después corre `fill_bone_interiors`, más estricto.
- `refine_coarse_bone_mask`: relee del TC el borde de una máscara gruesa, como la de TotalSegmentator a 1,5 mm.
  Se activa con `DENTALSEGMENTATOR_REFINE_COARSE_BONE=1`.
- Pruebas: `DicomMPRViewer/tests/test_bone_segmentation.py` (`BoneInteriorTests`, `ThinBoneTests`,
  `MedullaryFillTests`, `CoarseMaskRefinementTests`, foramen mentoniano).
- TotalSegmentator para maxilar y mandíbula: lo integró el usuario en su equipo y **no está en el repositorio**
  (a 2026-10-19). Antes de opinar sobre él, pedir el código o leerlo en su rama.

## 2. Reglas

1. **Diagnóstico estático primero.** Leer el código, los logs (`NN% ...`), los labelmaps NRRD y `.mha` ya
   guardados y las pruebas. No ejecutar TotalSegmentator, DentalSegmentator ni nnU-Net sin autorización
   explícita. Si hace falta inferencia, seguir `gpu-safe-execution`.
2. **Tres familias de máscaras separadas, sin solaparse:**
   - **hueso**: maxilar, mandíbula y cráneo, con su medular;
   - **dientes**: superiores e inferiores, como etiquetas propias aunque cierren el contorno del hueso;
   - **tejidos blandos**.

   Cada vóxel pertenece a una sola familia. Orden de precedencia cuando dos modelos discrepan:
   dientes > canal > hueso > tejido blando. Se escribe en el código y se prueba.
3. **De qué modelo sale cada cosa.** Hueso de TotalSegmentator; dientes, canal y tejidos blandos de
   DentalSegmentator. Antes de fusionar, las dos predicciones deben compartir rejilla, origen, dirección y
   espaciado (`python-medical-imaging` §2). Si no, se remuestrea con vecino más cercano, nunca lineal.
4. **Las clases de TotalSegmentator se confirman en el código instalado**, por ejemplo el mapa de clases del
   paquete, no de memoria ni ejecutándolo.

## 3. Hueso hueco entre corticales: cómo diagnosticarlo

El síntoma: al cortar, el hueso se ve como dos cáscaras con un vacío en medio. La medular falta en la máscara;
no es un problema de la malla. Causas a descartar en este orden, con las máscaras guardadas:

1. **El modelo solo marcó cortical.** En un corte axial, el hueso es un anillo y su interior es 0 en la máscara,
   pero la intensidad de ese interior es de hueso esponjoso (≈ 150–600 HU, varía según el escáner).
2. **`fill_bone_interiors` no lo rellenó.** Mirar su línea de log: «cavidades omitidas por aire/datos
   inválidos: N, por tamaño: M».
   - Por **aire**: un solo vóxel < −300 HU descarta toda la cavidad. Un seno o una celda aérea conectada,
     o el ruido, la invalidan entera.
   - Por **tamaño**: radio > 6 mm. El cuerpo mandibular o la tuberosidad pueden superarlo.
   - **Abierta**: si la cortical tiene un poro, la cavidad no está cerrada y `binary_fill_holes` no la toca.
     En el maxilar delgado es lo normal.
3. **Se rellenó y luego se perdió.** Algún paso posterior sobrescribe etiquetas, o el remapeo de etiquetas
   lo deshace.

Estrategia de relleno que hay que proponer y probar, sin implementarla aún:
- **Por intensidad dentro de la envolvente del hueso**: cerrar la cortical, con un cierre en mm o la envolvente
  convexa por corte de la mandíbula; luego añadir lo de dentro que tenga HU de hueso esponjoso.
- **Nunca rellenar aire anatómico**: senos maxilares, fosas nasales, órbitas, vía aérea y canal mandibular
  quedan fuera, por HU (< −300) y por etiqueta.
- **Cavidad abierta**: no se rellena por cierre global; se acota por la envolvente local del hueso.

## 4. Validación anatómica y geométrica (obligatoria antes de aceptar)

Una segmentación no se acepta sin este informe, calculado sobre las máscaras guardadas:

**Anatómica**
- [ ] Senos maxilares, fosas nasales y vía aérea siguen en aire: < 1 % de su volumen marcado como hueso.
- [ ] El canal mandibular existe y es continuo de foramen a foramen. No hay hueso dentro del canal.
- [ ] Dientes separados del hueso: ningún vóxel con dos familias; la corona sigue fuera del hueso.
- [ ] Cuántos componentes conectados tiene cada hueso, y qué tamaño tiene el mayor. Se informan los
      fragmentos > 0,5 cm³ que aparezcan.
- [ ] HU de la medular rellenada: media y percentiles 5–95 dentro del rango de hueso esponjoso.

**Geométrica**
- [ ] **Relleno**: en cortes cada 5 mm del cuerpo mandibular y la tuberosidad, la fracción del interior
      cortical que es hueso debe ser ≥ 95 %.
- [ ] **Paredes finas**: el maxilar anterior y el seno no tienen agujeros nuevos. Se cuentan las aberturas
      > 2 mm frente a la máscara anterior.
- [ ] **Sin crecimiento**: la máscara nueva no sale más de 1 vóxel fuera de la anterior, salvo en paredes
      selladas.
- [ ] Volumen de cada hueso antes y después del cambio, en cm³, con la diferencia explicada.

Ejemplo de resumen esperado:

```
Mandíbula: 78,4 cm³ (antes 61,2) · relleno medular 96,8 % en 12 cortes · 1 componente
Maxilar: 41,0 cm³ (antes 37,5) · senos en aire 99,6 % · 0 aberturas nuevas > 2 mm
Dientes: 32 piezas · solape con hueso 0 vóxeles · canal continuo D e I
```

## 5. Cómo se prueba un cambio

- Primero una prueba roja en `tests/test_bone_segmentation.py`, con fantoma sintético: un anillo cortical
  con medular de 300 HU, con y sin poro, y un seno de aire contiguo que no se debe rellenar. Luego el cambio.
- Pruebas solo en CPU: `CUDA_VISIBLE_DEVICES="" python -m unittest tests/test_bone_segmentation.py`.
- Para el caso real, el usuario corre la segmentación con permiso y se valida con §4 sobre su salida guardada.
