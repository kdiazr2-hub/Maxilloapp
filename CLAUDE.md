# MaxilloApp — Planificación Maxilofacial

Desktop app for orthognathic surgery planning from CT. The code lives in `DicomMPRViewer/`
(C++17 · Qt 6.11 · VTK 9.5.2 · GDCM via vcpkg · Windows). UI text is Spanish; sources compile with `/utf-8`.

`vtk/` at the repo root is an unused VTK 9.3 source clone (git-ignored). The build uses `C:/VTK`.

## Build & test

The `DicomMPRViewer/build` tree is already configured (generator "Visual Studio 18 2026",
VS 18 Insiders, `USE_GDCM=ON`, `BUILD_WORKSPACE_TESTS=ON`). `cl` is not on PATH; go through CMake.

```powershell
cmake --build DicomMPRViewer/build --config Release --parallel
ctest --test-dir DicomMPRViewer/build -C Release --output-on-failure
```

Fresh configure (only if the cache is lost):

```powershell
cmake -S DicomMPRViewer -B DicomMPRViewer/build -G "Visual Studio 18 2026" -A x64 `
  -DCMAKE_PREFIX_PATH="C:/Qt/6.11.0/msvc2022_64;C:/vcpkg/installed/x64-windows" `
  -DVTK_DIR="C:/VTK/build" -DUSE_GDCM=ON `
  -DGDCM_DIR="C:/vcpkg/installed/x64-windows/share/gdcm" `
  -DCMAKE_TOOLCHAIN_FILE="C:/vcpkg/scripts/buildsystems/vcpkg.cmake" `
  -DBUILD_WORKSPACE_TESTS=ON
```

Qt, VTK and GDCM DLLs are already deployed in `build/Release`. Scripts in `scripts/` are copied next to the exe post-build.

Tests (CTest):
- `GeometryCoreTests`, `BoneCavityFillTests`, `MeshGeneratorTests` — plain C++ executables
- `MaskToObjectTests` checks exact label extraction, committed cavity filling and immutable input.
- `ModelWorkflowTests` checks guided MODELOS steps, paired point requirements, fine adjustment and mandatory acceptance.
- `SplintHeightmapTests`, `SplintDesignTests`, `SplintContourEditTests`, `SplintPreviewSchedulerTests`, `ProjectSerializerTests`, `CompositeBlockTests`, `MeshRepairTests`, `OsteotomyCoreTests` — core tests declared with `add_core_test()`; synthetic arches in `tests/SplintTestGeometry.h`
- `SplintWorkspaceTests` also covers the composite block flow and the osteotomy wizard (Le Fort I → BSSO → genioplasty)
- `RepositionWorkspaceTests`, `SplintWorkspaceTests` — instantiate `MainWindow` (declared `friend`), render offscreen, write PNGs to `build/workspace-test-artifacts`
- `Mesh3DViewInteractionTests` — drives `Mesh3DView` offscreen with synthetic mouse events

Splint generator timing (not a CTest; `-DBUILD_PERF_TESTS=ON` registers a 300k-triangle smoke test):

```powershell
DicomMPRViewer/build/Release/SplintBenchmark.exe --upper sup.stl --lower inf.stl --points puntos.json --repeat 3 --csv tiempos.csv
DicomMPRViewer/build/Release/SplintBenchmark.exe --synthetic-triangles 300000 --grid 0.4 --no-thickness
```

The points JSON comes from "Exportar puntos" in the splint panel.
- `BoneSegmentationPythonTests` — `tests/test_bone_segmentation.py`, runs with `DENTALSEGMENTATOR_PYTHON` (conda env `dentalgpu`: numpy, scipy, SimpleITK, torch, nnunetv2)

## Architecture

- `MainWindow.cpp` (~12.7k lines) holds every workspace and the guided workflow:
  MPR/segmentation → Modelos (STL arch registration, composites) → Orientación (Frankfurt plane)
  → Osteotomía (Le Fort I, BSSO, genioplasty) → Registro de mordida → Reposición → Férulas.
  Prefer putting new logic in separate core classes (like `TransformCore`, `SplintGenerator`) that tests can link without Qt widgets.
- DICOM load: `AsyncDicomLoader` (worker thread) → `DicomSeriesIndexer` → `VolumeCacheManager` (.mha cache in `%LOCALAPPDATA%`) → `DicomVolumeLoader`.
- `MPRView` (2D reslice views), `Mesh3DView` (3D scenes, gizmos, picking).
- AI segmentation: `StandaloneDentalSegmentatorService` launches `scripts/run_standalone_dental_segmentator.py` (nnU-Net DentalSegmentator, weights in `%LOCALAPPDATA%/DicomMPRViewer/DentalSegmentator/ML`).
- `ProjectSerializer` saves/loads `.maxilloproject` files; new keys must stay optional so older projects load.
- Splints: `SplintHeightmapGenerator` (height-map splint, Prepare/Build), `SplintDesignCore` (named designs, JSON),
  `SplintContourEditCore`, `SplintPreviewScheduler` (background preview); UI in `SplintDesignPanel` and
  `MainWindowSplint.cpp` (MainWindow methods kept out of `MainWindow.cpp`). The classic `SplintGenerator` stays behind the "Método" selector.
  Extras (bevel, wire holes, bracket margins) are applied in the Build voxel domain so the splint stays closed.
  `MeshRepairCore` validates/repairs STL (created splints are repaired automatically, export validates again).
- Osteotomies: `OsteotomyCore` (line-segment cutting paths for Le Fort I / genioplasty, bilateral BSSO from 6 landmarks,
  kerf split by a signed field that keeps cell data, guide slabs from the same field). ProPlan-style wizard in
  `OsteotomyWizardPanel` + `MainWindowOsteotomy.cpp` (tipo → hueso → puntos → trayectoria → finalizar); results land in the
  existing segment members/labels (Le Fort 205/206, BSSO 208–211, genioplasty 212/213). The old ribbon osteotomy actions in `MainWindow.cpp` are hidden but kept.
- Object label constants and `objectActorKey` live in `ObjectLabels.h`.
- Mask conversion uses `MaskToObjectCore` and `MainWindowSegmentation.cpp`: extract the current
  label without smoothing or new filling; update object actors only, leaving mask data and display intact.
- Composites: `CompositeBlockCore` cuts bone outside / intraoral scan inside an oriented block and merges them into one
  mesh whose cells carry `CompositePart` (0 bone, 1 dental). Clips, transforms, clean and normals keep that cell data,
  so the scan follows its bone through orientation, osteotomies and repositioning; `ExtractPart` recovers it (the splint
  uses it). STL drops cell data: `ProjectSerializer` stores a `.vtp` next to tagged meshes. Avoid filters that drop cell
  data (e.g. appending with untagged meshes) on composites and their segments. MODELOS flow in `MainWindowComposite.cpp`
  (Registrar → Ajuste fino with scan contours on the MPR slices → Bloque → Revisar, review never skipped).
  Default composite method is "Contorno por puntos": points on the registered scan form a polygon extruded over the
  block thickness (`CompositeBlockCore::CreateContourComposite`, long edges refined to 1 mm before clipping); the
  cutting block and the classic trim stay selectable. Arch registration has no accept dialog (metrics in status bar).
  ORIENTACION always shows the composites it orients; without composites and without a pending scan it builds them
  from the segmented bones (`createBoneOnlyComposites`, same as «Continuar sin match»).
  Le Fort I / genioplasty cuts skip bone below the path that is not connected to the segment (e.g. mastoids).
  The permanent eight-step guide and action gates use `ModelWorkflowCore` + `MainWindowModels.cpp`.
  Each jaw's registration continues to its own block and acceptance before the next jaw.

`README.md` documents the load pipeline and cache but its build section is outdated.
