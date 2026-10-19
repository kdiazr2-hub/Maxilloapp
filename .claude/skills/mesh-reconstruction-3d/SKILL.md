---
name: mesh-reconstruction-3d
description: "Use when turning MaxilloApp's segmentation masks into 3D bone, teeth or soft-tissue meshes — rough or terraced (staircase) surfaces, holes in thin walls, smoothing that melts detail, closed/solid meshes for osteotomies, mesh validation against the mask. Covers MeshGenerator (Gaussian pre-smoothing with the 0.55 floor, marching cubes, windowed sinc) and the metrics that must pass before a mesh is accepted. NOT the segmentation or medullary filling (that is `medical-image-segmentation`), NOT surgical guides and plates (ImplicitCore / GuideDesignCore, see CLAUDE.md)."
tags: [mesh, marching-cubes, smoothing, vtk, surface, stl, maxilloapp]
recommends: [medical-image-segmentation, python-medical-imaging]
origin: maxilloapp
---

# mesh-reconstruction-3d — superficies lisas, fieles a la máscara y cerradas

## 1. Qué hay hoy

`DicomMPRViewer/src/MeshGenerator.cpp` (`MeshGenerator::generateMesh`):
- Hueso con ≥ 25 iteraciones: umbral de la etiqueta → `vtkImageGaussianSmooth` (σ 0,40–0,70 mm en xy y
  0,50–0,90 mm en z, convertido a vóxeles y **acotado a 1,45 vóxeles en xy y 1,70 en z**). Después, el
  **suelo de 0,55**: un vóxel de la etiqueta nunca baja de la isosuperficie, para que las paredes de un vóxel no
  desaparezcan. Después, `vtkMarchingCubes` a 0,5.
- Dientes, canal y la etiqueta 8: `vtkDiscreteMarchingCubes`, sin desenfoque y con pocas iteraciones.
- Luego `vtkWindowedSincPolyDataFilter`, con banda de paso 0,02–0,10 según las iteraciones.
- Pruebas: `MeshGeneratorTests` (una pared de un vóxel sobrevive a 25, 40 y 70 iteraciones).

## 2. Diagnóstico de una superficie rugosa (estático)

Antes de cambiar nada, leer de la imagen guardada (`.mha` de la caché o labelmap NRRD) el **espaciado**.
Revisar también el log de la segmentación:

- **Escalones horizontales regulares** en la bóveda y en las paredes casi horizontales: el vóxel es
  anisótropo, con z mucho mayor que xy. El σ en z está acotado a 1,70 vóxeles, así que con cortes de 1 mm o
  más el escalón sobrevive. Arreglo propuesto: **remuestrear la etiqueta a vóxel isótropo** antes del contorno,
  con un campo continuo, no vecino más cercano sobre la máscara.
- **Rugosidad fina sin patrón**: la máscara misma es ruidosa, porque la red duda en el borde. Se arregla en la
  máscara, con apertura y cierre en mm (`medical-image-segmentation`), no con más suavizado de malla.
- **Paredes interiores visibles al cortar**: la máscara es hueca; no es un fallo de la malla
  (`medical-image-segmentation` §3).
- **Detalles derretidos**: demasiadas pasadas de sinc. Lo mismo vimos en las guías: 70 pasadas sobre
  0,25 mm convirtieron cilindros en donas (`02-DOCS/raw/experiments/openvdb-2026-10-07/`).

## 3. Método recomendado para hueso

1. **Campo de distancia con signo** de la máscara binaria (EDT dentro − EDT fuera, en mm, con el espaciado
   real), en lugar de desenfocar el binario 0/1. Es suave por construcción y no adelgaza paredes.
2. **Remuestreo isótropo del campo** (por ejemplo 0,3–0,5 mm, configurable) con interpolación lineal o
   B-spline del campo. Se interpola el campo; nunca la máscara.
3. Suavizado **del campo** con σ en mm igual en los tres ejes; se conserva la protección de paredes finas
   (el equivalente del suelo de 0,55: el campo no cruza a fuera donde la máscara es hueso).
4. Contorno en 0, con Flying Edges o marching cubes.
5. **Pocas pasadas de sinc** (10–20) solo para quitar el patrón del contorno.
6. Malla cerrada y orientada (`MeshRepairCore::Analyze` / `Repair`).

Todo con parámetros en mm, no en vóxeles, para que el resultado no dependa del escáner.

## 4. Validación geométrica (obligatoria antes de aceptar una malla)

Calculada contra la máscara de la que sale:

- [ ] **Fidelidad**: distancia de Hausdorff al 95 % entre malla y borde de la máscara ≤ 0,5 × el vóxel más
      grande; media ≤ 0,25 × el vóxel.
- [ ] **Volumen**: el volumen encerrado difiere ≤ 2 % del volumen de la máscara.
- [ ] **Paredes finas**: ninguna pared de 1–2 vóxeles desaparece; ampliar `MeshGeneratorTests`.
- [ ] **Cerrada**: 0 aristas de borde, 0 no-variedad, orientación coherente
      (`MeshRepairCore::Analyze(mesh).Valid()`).
- [ ] **Escalones**: en la bóveda, la desviación de la normal entre caras vecinas no muestra el patrón
      periódico del espaciado en z. Se mide sobre un parche y se informa.
- [ ] **Tamaño**: número de triángulos y tiempo, frente a la versión anterior.

## 5. Cómo se prueba un cambio

- Fantomas sintéticos en `MeshGeneratorTests`: una esfera vista con vóxel anisótropo (0,4 × 0,4 × 1,25 mm)
  debe salir sin escalones medibles, y una pared de un vóxel debe seguir sin agujeros.
- Compilar y probar según CLAUDE.md (Windows: `cmake --build DicomMPRViewer/build --config Release`,
  `ctest -C Release`).
- Nada de esto usa GPU. Si un cambio la usara, aplicar `gpu-safe-execution`.
