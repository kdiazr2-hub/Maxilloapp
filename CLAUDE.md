# MaxilloApp — Planificación Maxilofacial

Desktop app for orthognathic surgery planning from CT. The code lives in `DicomMPRViewer/`
(C++17 · Qt 6.11 · VTK 9.5.2 · GDCM via vcpkg · Windows). UI text is Spanish; sources compile with `/utf-8`.
Research software, not a validated medical device: the splash, the status bar and the exported reports say so
(`src/ClinicalNotice.h`).

`vtk/` at the repo root is an unused VTK 9.3 source clone (git-ignored). The build uses `C:/VTK`.
`SlicerDentalSegmentator-main/` is the upstream DentalSegmentator Slicer module (Apache-2.0), kept for reference;
the build does not use it.

Detailed design record of every module (why it is the way it is, the user's reports, the tests that pin it):
`02-DOCS/wiki/plataforma/arquitectura.md`. Read the module's section there before changing it.

## Build & test

CI: `.github/workflows/windows-ci.yml` builds Release on `windows-2022` (VS 2022, Qt 6.11.0, GDCM from vcpkg,
VTK 9.5.2 from source, cached) and runs every CTest except the GPU workspace tests, plus the segmentation Python
tests on CPU. It runs on every pull request to `main`.

Locally `cl` is not on PATH; go through CMake. The `DicomMPRViewer/build` tree must be configured on the machine
that builds it (`USE_GDCM=ON`, `BUILD_WORKSPACE_TESTS=ON`):

```powershell
cmake --build DicomMPRViewer/build --config Release --parallel
ctest --test-dir DicomMPRViewer/build -C Release --output-on-failure
```

Fresh configure (generator "Visual Studio 18 2026" on the VS 18 Insiders PC, "Visual Studio 17 2022" on the laptop):

```powershell
cmake -S DicomMPRViewer -B DicomMPRViewer/build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH="C:/Qt/6.11.0/msvc2022_64;C:/vcpkg/installed/x64-windows" `
  -DVTK_DIR="C:/VTK/build" -DUSE_GDCM=ON `
  -DGDCM_DIR="C:/vcpkg/installed/x64-windows/share/gdcm" `
  -DCMAKE_TOOLCHAIN_FILE="C:/vcpkg/scripts/buildsystems/vcpkg.cmake" `
  -DBUILD_WORKSPACE_TESTS=ON
```

Qt, VTK and GDCM DLLs are deployed in `build/Release`. Scripts in `scripts/` are copied next to the exe post-build.

Machines: a PC with VS 18 Insiders; the laptop (`C:\MaxilloApp`, VS 2022 Build Tools, hybrid Intel UHD + RTX 3050:
`RepositionWorkspaceTests` fails on the Intel GPU with "Foreground opacity was not blended" and passes once
Windows → Pantalla → Gráficos sets its exe to «Alto rendimiento»; run render tests on the discrete GPU); and
`C:\Users\Kevin\Documents\MaxilloApp`, which has no toolchain and can only run already-built binaries. Its
`dentalgpu` env (Miniforge, Python 3.11) has torch 2.11.0+cu128 for its RTX 5050 (Blackwell, sm_120); installing
nnunetv2 pulls a newer torchvision that swaps torch for a CPU-only build, so reinstall torch and torchvision
together from the cu128 index afterwards.

Tests (CTest):
- `GeometryCoreTests`, `BoneCavityFillTests`, `MeshGeneratorTests` — plain C++ executables.
- Core tests declared with `add_core_test()` (no Qt Widgets; synthetic geometry, e.g. `tests/SplintTestGeometry.h`):
  splints (`SplintHeightmapTests`, `SplintDesignTests`, `SplintContourEditTests`, `SplintPreviewSchedulerTests`),
  `ProjectSerializerTests`, `CompositeBlockTests`, `ScanEraseTests`, `MeshRepairTests`, `CollisionTests`,
  `MaskToObjectTests`, `ModelWorkflowTests`, `SegmentationProgressTests`, `OrientationCoreTests`,
  `RegistrationCoreTests`, osteotomy (`OsteotomyCoreTests`), solids and guides (`ImplicitCoreTests`,
  `WrapCoreTests`, `GuideBaseTests`, `CutSlotTests`, `GuideDesignTests`, `GuideSculptTests`, `GuidePlanTests`),
  plates and the Le Fort guide (`PlateTests`, `LeFortGuideTests`, `LeFortMotionTests`, `LeFortHoleTests`,
  `RootAnalysisTests`).
- `RepositionWorkspaceTests`, `SplintWorkspaceTests` instantiate `MainWindow` (declared `friend`), render offscreen
  and write PNGs to `build/workspace-test-artifacts`; `SplintWorkspaceTests` also covers the composite block flow
  and the osteotomy wizard. `Mesh3DViewInteractionTests` drives `Mesh3DView` with synthetic mouse events. All three
  need a GPU and are not in CI.
- `BoneSegmentationPythonTests` — `tests/test_bone_segmentation.py`, runs with `DENTALSEGMENTATOR_PYTHON`
  (conda env `dentalgpu`); needs only numpy, scipy and SimpleITK.

Tools (not CTests): `SplintBenchmark` (splint timing; `-DBUILD_PERF_TESTS=ON` registers a 300k-triangle smoke
test; points JSON from "Exportar puntos"), `PlateProbe` (plates and Le Fort guide on a real project:
`PlateProbe.exe --project x.maxilloproject --out dir --advance 6 --down 3 [--template paranasal|splintless]`).
`tools/GuideResolutionProbe.cpp` is an OpenVDB experiment with no CMake target.

Linux core build (cloud sessions): `DicomMPRViewer/tools/linux-core-tests/CMakeLists.txt` builds the core tests
only. It needs VTK 9.5.2 from source (Ubuntu's 9.1 shifts geometry by tenths of a millimetre and fails PlateTests
and LeFortGuideTests); Qt can be Ubuntu's 6.4. `QT_NO_EMIT` keeps Qt's `emit` out of oneTBB.

```bash
cmake -S DicomMPRViewer/tools/linux-core-tests -B build-linux-core -DVTK_DIR=<vtk-9.5.2>/lib/cmake/vtk-9.5
cmake --build build-linux-core --parallel && ctest --test-dir build-linux-core --output-on-failure
```

## Architecture (summary — detail in `02-DOCS/wiki/plataforma/arquitectura.md`)

- `MainWindow.cpp` (~14.5k lines) plus `MainWindow<Module>.cpp` files hold the UI of every workspace and the guided
  workflow: Segmentación → Modelos → Orientación → Osteotomía → Registro de mordida → Reposición → Férulas → Guías.
  New logic goes in core classes without Qt Widgets that tests can link (`TransformCore`, `OrientationCore`,
  `RegistrationCore`, …); `MainWindow` only orchestrates.
- Ribbon: only ARCHIVO and ORTOGNÁTICA are visible (IMAGE and MEDIDAS are compiled out with `if constexpr (false)`;
  measurements open from ARCHIVO). Planning modules keep hidden "MT" tabs that tests click by text; they are steps
  of the ORTOGNÁTICA step bar (`MainWindowOrthognathic.cpp`). Module switches show the frontal standard view.
- DICOM: `AsyncDicomLoader` (worker thread) → `DicomSeriesIndexer` → `VolumeCacheManager` (.mha cache in
  `%LOCALAPPDATA%`) → `DicomVolumeLoader`. Views: `MPRView` (2D reslice), `Mesh3DView` (3D, gizmos, picking).
- AI segmentation: `StandaloneDentalSegmentatorService` runs `scripts/run_standalone_dental_segmentator.py`
  (log in `AISegmentation/<fecha>/segmentacion.log`): bone, teeth and canal from nnU-Net DentalSegmentator
  (user's decision, 2026-10-19; `DENTALSEGMENTATOR_BONE_SOURCE=totalsegmentator` takes skull and mandible from
  TotalSegmentator instead, its blocks then re-read from the CT). Bone post-processing per jaw: closing, thicken,
  then `solid_bone_mask` (closed cavities ≤ 8 mm, then marrow = free voxels the bone encloses: at most 3 of 26
  directions run 15 mm without meeting it; never air nor within 2 mm of air, so the nose, the sinuses' mucosa,
  the orbits and the cranial cavity stay open; no HU floor otherwise), `drop_bone_islands` (< 50 mm3) and a diagnostic report
  in the log (spacing and anisotropy, cortical/medullary/air voxels, trimesh watertightness;
  `DENTALSEGMENTATOR_MESH_CHECK=0` skips the mesh check). `fill_medullary_bone` is no longer called.
- Meshes: `MeshGenerator::generateMesh` contours a Gaussian-smoothed signed distance of each bone label; thin walls
  are protected by forcing only their medial voxels inside. `MaskToObjectCore` extracts a label without smoothing.
  Labels the script delivered solid (`MainWindow::m_solidBoneLabels`, AI labels 5/6, optional project key
  `solidBoneLabels`) skip `fillBetweenCortices`, whose 20 mm axis chords put rectangular slabs in the nose and palate.
- Modelos: `ModelWorkflowCore` gates the eight guided steps (`MainWindowModels.cpp`); `RegistrationCore` registers
  each intraoral scan to its bone (paired points, then ICP in a jaw-specific ROI) and also the bite scans;
  `CompositeBlockCore` merges bone and scan into one mesh whose cells carry `CompositePart` (0 bone, 1 dental).
- Orientación: `OrientationCore::AlignFrankfurt` levels the four marked points onto Z+ and yaws the porions onto X.
- Osteotomies: `OsteotomyCore` (Le Fort I, BSSO, genioplasty paths and kerf split) with the wizard in
  `OsteotomyWizardPanel` + `MainWindowOsteotomy.cpp`; segment labels 205/206, 208–211, 212/213 (`ObjectLabels.h`).
- Reposición (`MainWindowReposition.cpp`): `CollisionCore`, restrictions, pre-op ghost, impaction per cut point.
- Splints: `SplintHeightmapGenerator` (Prepare/Build), `SplintDesignCore`, `SplintContourEditCore`,
  `SplintPreviewScheduler`; UI in `SplintDesignPanel` + `MainWindowSplint.cpp`. Guide points fill themselves in
  from the cusps on entering FERULA. Only the height-map method is offered; the classic `SplintGenerator` stays in
  the code but hidden (delete only if the user confirms).
- Solids: `ImplicitCore` (signed distance, negative inside, baked to a grid, polygonised once), `WrapCore` (3-matic
  Wrap), `MeshRepairCore`. Guides: `GuideBaseCore`, `CutSlotCore`, `GuideDesignCore` (Prepare/Build into ONE
  field), `GuideSculptCore` (EDITAR), `GuidePlanCore` (the plan, optional `guidesPlan` key), UI in
  `MainWindowGuides.cpp`.
- Le Fort plates and guide: `PlateCore` (custom plates designed on the planned bone, predictive holes taken back
  through the segment motion, `CheckHoleSeat` blocks screws on a bony margin), `LeFortMotionCore` (the band an
  impaction removes), `LeFortHoleCore` (hole support), `LeFortGuideCore::Layout`, `RootAnalysisCore` (root apices),
  `GuideEngraveCore` (labels).
- `ProjectSerializer` saves `.maxilloproject` files; composites also store a `.vtp` because STL drops cell data.

## Rules that are easy to break

- Never chain mesh booleans (`vtkBooleanOperationPolyDataFilter` fails on anatomy): carve everything into one
  `ImplicitCore` field and contour it once.
- Measure against the bone with `ImplicitCore::BakeMeshField` of the meshes, never against a wrap: a tight wrap is
  a shredded film, a loose one stands proud of every concavity. A keep-out that must mean "inside bone" needs a
  closed envelope (≥ 3 mm gap closing), not a 0.5 mm wrap.
- New `ProjectSerializer` keys are optional with defaults, so older projects still open.
- Composites and their segments keep cell data: avoid filters that drop it (e.g. appending with untagged meshes).
- `slots` is a Qt keyword macro; `near`/`far` are Windows macros. Never use them as names.
- Hidden legacy code (ribbon osteotomy actions, classic splint, IMAGE/MEDIDAS tabs) stays unless the user says to
  delete it.
- Patient data (DICOM, real-case STL/projects, segmentations) and model weights never enter git; `.gitignore`
  excludes them.
- Before anything that loads the GPU (segmentation, torch), follow `.claude/skills/gpu-safe-execution`.

## Knowledge map

| What | Where |
|------|-------|
| Project constitution (SDD non-negotiables) | `02-DOCS/wiki/sdd/constitution.md` |
| Full knowledge map (specs, decisions) | `02-DOCS/wiki/index.md` |
| Detailed architecture record | `02-DOCS/wiki/plataforma/arquitectura.md` |
| Project skills for CT segmentation (GPU policy, masks, meshes, Python imaging) | `.claude/skills/{gpu-safe-execution,medical-image-segmentation,mesh-reconstruction-3d,python-medical-imaging}/SKILL.md` |
