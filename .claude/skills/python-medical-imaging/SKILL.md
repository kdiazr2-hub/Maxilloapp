---
name: python-medical-imaging
description: "Use when writing, reviewing or testing MaxilloApp's Python imaging code — scripts/run_standalone_dental_segmentator.py and any TotalSegmentator / DentalSegmentator glue: SimpleITK and NumPy axis order, spacing/origin/direction, NRRD/NIfTI/MHA I/O, label dtypes, Hounsfield units, millimetre-based morphology with scipy.ndimage, memory on big head CTs, and CPU-only unit tests in tests/test_bone_segmentation.py. NOT generic Python style (rsc `python`), NOT the anatomy rules (that is `medical-image-segmentation`), NOT GPU safety (that is `gpu-safe-execution`)."
tags: [python, simpleitk, numpy, scipy, nrrd, nifti, medical-imaging, maxilloapp]
recommends: [medical-image-segmentation, gpu-safe-execution]
origin: maxilloapp
---

# python-medical-imaging — geometría correcta, memoria contenida, pruebas en CPU

## 1. Dónde está el código

- `DicomMPRViewer/scripts/run_standalone_dental_segmentator.py`. Se copia junto al exe al compilar, y lo
  lanza `StandaloneDentalSegmentatorService` con el Python de `DENTALSEGMENTATOR_PYTHON` o del entorno conda
  `dentalgpu`.
- Pruebas: `DicomMPRViewer/tests/test_bone_segmentation.py` (CTest `BoneSegmentationPythonTests`).
- Dependencias del entorno: numpy, scipy, SimpleITK, torch, nnunetv2. **No instalar, actualizar ni quitar
  paquetes** del entorno `dentalgpu` sin pedirlo; tampoco tocar CUDA.

## 2. Geometría: los errores que más cuestan

- `sitk.GetArrayFromImage(img)` devuelve el array en **(z, y, x)**, pero `img.GetSpacing()` está en
  **(x, y, z)**. El script pasa `image.GetSpacing()[::-1]` como `spacing_zyx`. Toda función que reciba
  espaciado lo nombra con su orden (`spacing_zyx`).
- Al volver a imagen: `out = sitk.GetImageFromArray(arr); out.CopyInformation(ref)`. Sin eso se pierden el
  origen y la dirección, y la máscara aparece desplazada o girada en la app.
- Antes de combinar dos máscaras, por ejemplo TotalSegmentator con DentalSegmentator, comprobar que coinciden
  tamaño, espaciado, origen y dirección, con tolerancia de 1e-4 mm. Si no coinciden, `sitk.Resample` con
  `sitkNearestNeighbor` para etiquetas y la imagen de referencia de la app como destino.
- Las etiquetas son `uint8`/`uint16`, nunca `float`. Las intensidades están en HU, ya reescaladas por el lector.

## 3. Morfología en milímetros

- Radios y distancias en mm, convertidos con el espaciado real:
  `ndi.distance_transform_edt(mask, sampling=spacing_zyx)`. Un radio en vóxeles cambia de significado
  de un escáner a otro.
- Elementos estructurantes con conectividad explícita
  (`np.ones((3,3,3), bool)` = 26-vecindad, como `fill_bone_interiors`).
- Trabajar en el **recorte de la región** (bounding box + margen), como hacen `fill_bone_interiors` y
  `thicken_thin_bone`. Nunca operar sobre el volumen entero de 400³+ si basta el hueso.

## 4. Memoria y rendimiento (portátil)

- Un 512×512×400 en `float64` ocupa unos 800 MB. Usar `float32` para campos e `int16` para HU, y evitar
  copias (`np.asarray`, operaciones in situ, `out=`).
- No importar `torch` en código de pre o posprocesado ni en las pruebas: inicializa CUDA y reserva memoria.
- Nada de inferencia en paralelo: un solo proceso de predicción, con `-npp 1 -nps 1` (ver `gpu-safe-execution`).

## 5. Pruebas (CPU, sin modelos)

- Fantomas sintéticos pequeños (≤ 64³) con espaciado anisótropo realista, por ejemplo (1,0, 0,4, 0,4) mm en zyx.
- Cada función nueva lleva su prueba roja primero. Casos mínimos para hueso:
  - medular rellenada;
  - seno de aire intacto;
  - poro en la cortical;
  - borde del volumen;
  - etiquetas protegidas (dientes, canal) sin cambiar.
- Ejecutar sin GPU:
  `CUDA_VISIBLE_DEVICES="" "$DENTALSEGMENTATOR_PYTHON" -m unittest tests/test_bone_segmentation.py`.
  En Windows, PowerShell: `$env:CUDA_VISIBLE_DEVICES=""` solo en esa sesión.
- Las pruebas nunca descargan pesos, nunca llaman a `nnUNetv2_predict` ni a TotalSegmentator.

## 6. Criterios de validación

- [ ] Toda imagen de salida conserva origen, dirección y espaciado de la de entrada (se comprueba en la prueba).
- [ ] Los parámetros geométricos están en mm y las variables de espaciado dicen su orden.
- [ ] Las etiquetas salen con dtype entero; no hay etiquetas fuera del mapa esperado.
- [ ] La memoria máxima del paso nuevo está estimada y es razonable para el portátil.
- [ ] Las pruebas pasan en CPU con `CUDA_VISIBLE_DEVICES=""`.
