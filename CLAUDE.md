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
- `SplintHeightmapTests`, `SplintDesignTests`, `SplintContourEditTests`, `SplintPreviewSchedulerTests`, `ProjectSerializerTests`, `CompositeBlockTests`, `MeshRepairTests`, `OsteotomyCoreTests`, `CollisionTests`, `ImplicitCoreTests`, `WrapCoreTests`, `GuideBaseTests`, `CutSlotTests`, `GuideDesignTests`, `GuideSculptTests`, `PlateTests`, `GuidePlanTests`, `SegmentationProgressTests` — core tests declared with `add_core_test()`; synthetic arches in `tests/SplintTestGeometry.h`
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
  `MainWindowSplint.cpp` (MainWindow methods kept out of `MainWindow.cpp`). Only the height-map method is offered: the
  classic `SplintGenerator` and its panel stay in the code but the "Método" selector is hidden (delete only if the user
  confirms). The automatic contour keeps only teeth within the arch span of the guide points (+6 mm). Built-in designs:
  Intermedia = repositioned Le Fort on the unmoved mandible, Final = on the repositioned distal / post-genioplasty mandible.
  Extras (bevel, wire holes, bracket margins) are applied in the Build voxel domain so the splint stays closed. The
  panel keeps the main design/point/create flow visible and folds parameters, thickness, contour, extras and secondary
  exports. After Flying Edges/decimation, a conservative windowed-sinc pass regularizes voxel striations before normals
  and thickness are computed.
  `MeshRepairCore` validates/repairs STL (created splints are repaired automatically, export validates again).
- Osteotomies: `OsteotomyCore` (line-segment cutting paths for Le Fort I / genioplasty, bilateral BSSO from 6 landmarks,
  kerf split by a signed field that keeps cell data, guide slabs from the same field). ProPlan-style wizard in
  `OsteotomyWizardPanel` + `MainWindowOsteotomy.cpp` (tipo → hueso → puntos → trayectoria → finalizar); results land in the
  existing segment members/labels (Le Fort 205/206, BSSO 208–211, genioplasty 212/213). The old ribbon osteotomy actions in `MainWindow.cpp` are hidden but kept.
- Ribbon: only ARCHIVO, MEDIDAS and ORTOGNÁTICA are visible. The planning modules (SEGMENTACION → GUIAS) keep
  hidden "MT" tabs (tests and code still click them by text) and are steps of the ORTOGNÁTICA step bar at the top of
  the ribbon, above the module's actions (`MainWindowOrthognathic.cpp`: step buttons with ✓ when done, ‹ ›). Every module switch shows its
  3D views in the frontal standard view (`showModuleViewsFrontal`); `Mesh3DView` frames its first mesh in the
  current standard view (frontal by default, oblique only after `resetCamera`).
- Solids: `ImplicitCore` is a signed-distance kernel (negative inside) — primitives, min/max booleans, offset,
  hollow, layer, transforms and meshes baked to a grid (flood fill + exact EDT, read back trilinearly) — evaluated
  once into a grid and polygonised once (FlyingEdges → reverse sense → sinc → `MeshRepairCore`). Wrap, base plates,
  cut slots and tubes are meant to be thin wrappers over it. Never chain mesh booleans
  (`vtkBooleanOperationPolyDataFilter` fails on anatomy); carve everything into one field like the splint does. It
  resamples, so input triangulation is lost and edges round at voxel scale. Its grid steps are public
  (`RasterizeShells` → `DilateMask` → `FillInteriorFromOutside` → `SignedDistanceField` → `ToImage`) so callers can
  insert their own morphology. Phase 0 of the GUIAS (surgical guides) module; the app does not call it yet.
- `WrapCore::Wrap` is 3-matic's Wrap on those steps: closing in real millimetres (`gapClosingMm`,
  `smallestDetailMm`), dilate → fill → contour at iso −gap, then smooth and repair. The dilation and the distance to
  its boundary are quantised to voxel centres, which left the wrap up to one voxel INSIDE the surface (0.3 mm at the
  default detail: guides and plates sank into the bone). When the meshes enclose a volume, the wrap is now the union
  of the closing and the solid's own field, contoured half a voxel out: it never sinks into what it wraps and stands
  at most a voxel off it (`WrapCoreTests` "the wrap lies on the surface"). Open surfaces keep the old behaviour. Same four steps as
  `CompositeBlockCore::VoxelUnion`, which stays as it is because `MeshRepairCore`'s remesh calls it and a wrap that
  repairs its own output would loop back into the repair.
- `GuideBaseCore::CreateBase` is 3-matic's Create Base. `MakeRegion` turns the marked points into a `GuideRegion`:
  the projection axis is the normal of the best-fit plane of the points, turned outward by the field (averaging
  surface normals tilts it when a point sits on an edge); the polygon is rounded by a 2D opening + closing of
  `cornerRadiusMm` (exact EDT, `ImplicitCore::SquaredDistanceTransform` on a {nu, nv, 1} grid) and kept as a 2D signed
  distance (`PlanarPrism`); and a height map of the first surface seen along the axis (`PlanarHeight`) keeps the wall
  on the marked face — strictly in front where the surface is seen, and past the silhouette only down to the wall's
  own thickness, so it never runs behind a thin wall or down the far side. Base = prism ∧ layer of the wrap's real
  signed distance (`clearance <= d <= clearance + thickness`, uniform on oblique walls) ∧ that height limit.
  `RegionOutline` lays the rounded outline on the surface for display. Contour persists via `ContourToJson`.
  The UI marks the region with a brush instead (user's request, 2026-09-16: the point polygon depended on click
  order and self-intersected): `MakeBrushRegion` takes `GuideBrushStroke` dabs (centre, radius, erase) applied in
  order; the outline is the dabs seen along the axis, and each dab is also a column along the local surface normal
  through the whole wall, baked into `GuideRegion::paint` and intersected in `BaseNode`, so the base covers exactly
  the painted surface. Saved as `paint` in the plan.
  Rim finish (user's request, 2026-09-16, after a reference image of printed guide pads): the outer face is a
  variable offset baked with `BakeFunction` — the thickness eases (smoothstep) from `edgeThicknessFraction` at the
  outline to full at `edgeTaperMm` inside — and all limits meet in `ImplicitCore::SmoothIntersect` (polynomial smooth
  max, radius `edgeRoundMm`; it only removes material). Surface ridges came from raster steps: the 2D outline SDF is
  Gaussian-blurred (1.2 cells), brushed masks get a 2 mm closing + 1 mm opening (dab scallops), the brushed columns
  are blurred and 1.5 mm wider than the dabs so the smooth outline sets the rim, and the guide is contoured with 30
  sinc iterations, pass band 0.05. Tests that probe depths in the wall set taper and round to 0.
- `CutSlotCore::CutSlots` subtracts a slab of `OsteotomyCore`'s own path field from the base, so slot and planned
  osteotomy coincide by construction. `OsteotomyCore::PreparePathField` / `FieldAt` build the frame once for grid
  sweeps (the older `PathField` rebuilds it per call); the field is baked with `ImplicitCore::BakeFunction`. The
  field extends past the ends of the path, so a slot across the base separates it — `CutSlotResult::pieces` says
  how many parts came out. Careful: `slots` is a Qt keyword macro, never a variable name.
- `GuideDesignCore` assembles the guide: `Prepare` bakes the wrap once (slow), `Build` carves base
  (`GuideBaseCore::BaseNode`), saw slots (`CutSlotCore::SlotNode`) and fixation-hole cylinders into ONE field and
  contours it once — building the base, meshing it and re-baking to cut slots would resample twice. Split like
  `SplintHeightmapGenerator`'s Prepare/Build, ready for `SplintPreviewScheduler`. `GuideFixationHole` mirrors
  `SplintWireHole`; `SurfaceNormalAt` gives the drilling axis. Each `GuideSlot` names its osteotomy and, once the
  user places both ends, is limited to them; every slot is also clipped to the marked region shrunk by
  `edgeMarginMm`, so it never reaches the rim and the guide stays in one piece (user's decision, 2026-09-15). `GuidePlanCore`
  holds what the user decided (sources, region, slots with their ends, holes, parameters) and saves it under the
  optional `guidesPlan` project key; `OsteotomyCore::PathToJson` / `PathFromJson` persist the cut a slot follows.
  `GuideFigure`s are the Boolean tools (cylinder, box, sphere with exact measurements, imported STL, copied project
  object, or a three-point curved tube): a local frame (`FrameAt`, z along the surface normal) and Add/Subtract, carved into the same field
  (guide = (base ∪ added) − slots − holes − subtracted); `FigurePreview` draws them.
  `GuideDesignCore::KeepOutNode` returns slots + holes + subtracted figures as one node: `Build` and the edit
  session's protected field both come from that one list, so an edit can never fill in what the plan cut away.
- `PlateCore` is the custom Le Fort osteosynthesis module (user's decision, 2026-09-19: it replaces the plain Le Fort
  guide; the combo item is «Placas + guía Le Fort I»; the chin guide is unchanged). It follows the predictive-hole
  technique of the literature the user supplied (Cureus 2025 RCT, CMTR 2026 stability cohort, ACFS 2026, JCM 2025):
  plates are designed on the bone in its PLANNED position (`repositionMeshForLabel` of the cranial base and the Le Fort
  segment); every hole is assigned to its bone (`AssignBones`, nearest surface); cranial holes stay, segment holes go
  back to the pre-operative position through the inverse of the segment motion (`PredictHoles`); the guide, still on
  the pre-operative bone, gets a drill sleeve at each (`SleeveFigures`: an added cylinder body starting 0.5 mm off
  the bone plus a subtracted bore, merged into the guide's figures by `MainWindow::guideFiguresWithSleeves`, so the
  build and the EDITAR keep-out both carry them). The segment motion is recovered with `PlateCore::RigidMotion`
  (rigid least squares on corresponding vertices of `m_repositionOriginalMeshes[kLeFortSegLabel]` and the moved
  segment; fails if the mesh was re-cut), so REPOSICIÓN keeps no extra state. Templates: paranasal (one strut) or L
  (piriform arm, buttress arm, bar joining their lowest holes; `TemplateStruts`), holes clicked top to bottom, «Siguiente
  brazo» between arms. A plate is a continuous ribbon swept along its arms (user's report, 2026-09-19: a plate built
  as a layer on the wrap dived into the gap a large movement opens and broke up). `strutPath` samples each arm every
  millimetre, drops the samples onto the planned wrap (1 mm closing) and walks from each hole while the bone under it
  is the hole's own bone (`PlateBoneQuery`), the surface turns < 15°/mm and < 50° in total, and the walk progresses
  (samples near a rounded edge collapse onto the corner otherwise); what is left is crossed by a straight bar with
  one normal, lifted along that normal over any corner. Normals are smoothed along the arm; the ribbon is baked
  with `BakeFunction` as the distance to the nearest mitred piece (rounded rectangle width × thickness, pieces cut at
  the bisector planes so bends meet flush, only free ends rounded), ∩ outside the bone, minus bores and countersinks.
  `PlateBuildResult::bridgedMm` reports the bar. PlateTests has an 8 mm gap case.
  Defaults (user's choice): 1.0 mm plate, 2.0 mm screws, guide fixation 1.5 mm, one-piece guide across the midline;
  sleeve bore 1.6 mm / outer 4.2 mm / height 4 mm. `Check` warns (never blocks) on < 2 screws per bone, holes < 4 mm
  from the osteotomy (measured with `OsteotomyCore::PathField` before the cut), overlapping rings and holes on the
  wrong side of the cut; `Build` reports the gap under each hole to the real bone (passive fit). The plan keeps
  `plates`, `plate` and `sleeve` as optional keys. UI in `MainWindowGuides.cpp` («PLACAS A MEDIDA» section, mode
  `kModePlateHoles`, `m_guidePlannedView` swaps the scene to the planned bone with the plates, predictive holes are
  purple markers on the pre-operative view, `exportGuidePlateFiles` writes `placa_N_lado.stl` plus
  `informe_placas.txt`). Not done yet: bone thickness under each screw from the CT, root proximity (teeth are not
  segmented separately), posterior bony interference, postoperative accuracy report.
- `GuideSculptCore` is the EDITAR step: Freeform's clay, except the clay is the signed distance grid the guide was
  contoured from. `SculptSession::Reset` bakes the finished guide (`BakeMeshField`, detail spacing, ≥ 3 mm padding so
  material can be added outside it) and the brushes edit that grid: Suavizar `φ += w·λ·(G∗φ − φ)` with a 3×3×3
  separable kernel and a level that runs 50-fold from end to end (`LevelScale`), Cera caliente (derretir = wider
  kernel reaching 1.5 R, suavizar, añadir/quitar `∓ δ·w`), Añadir/Quitar material (`min`/`max` against an
  `ImplicitCore::Capsule` from the previous dab to this one, so a fast drag leaves no gaps), Aplanar on a plane
  fitted by PCA to the surface voxels under the brush and oriented with the field's gradient (aplanar / rascar =
  only `max` / rellenar = only `min`). `Trim` is Split Piece: a polygon clicked on the guide swept along
  `Mesh3DView::viewDirection()` as an `ImplicitCore::Prism`, then `KeepLargestPiece` (Select Lump) drops the islands.
  Two fields are re-applied after every stroke, in `EndStroke`: the anatomy (`φ = max(φ, clearance − wrapDist)`) and
  the baked keep-out (`φ = max(φ, −keepOut)`). Undo is per stroke in 32³ blocks saved before they are touched and
  swapped back, capped at 30 strokes or ~300 MB. `Contour(fast)` is the raw preview during a drag (no sinc, no
  repair, throttled to ~10 Hz by a `QTimer`); letting go contours properly and that mesh becomes `m_guideMesh`.
  Smoothing never accumulates because every stroke starts from the grid, not from the previous mesh.
  The GUIAS module is the eighth ORTOGNÁTICA step: `MainWindowGuides.cpp` holds the side panel (guide type → wrap →
  paint the support region (drag; Ctrl erases; Alt + vertical drag resizes; the envelope is its own teal layer with
  the painted patch in blue, and CAPAS toggles models / envelope / guide / figures; computing the envelope hides the
  bone and makes it opaque) → tick which osteotomies get a
  slot and place its ends →
  figures placed by click and moved with the gizmo → fixation holes → build → edit → thickness map, STL export) and
  workspace page 7. The panel shows only the six steps (tipo, zona de apoyo, ranuras, agujeros, crear, editar,
  exportar), each appearing when the step before it has produced something, with one short hint line per mode; the
  layers are a single row of toggles and everything with a sensible default lives in the folded «Avanzado» and
  «Figuras» sections. EDITAR is a palette of eight square `QToolButton`s whose icons are drawn with `QPainter` in
  `MainWindowGuides.cpp` (`sculptPixmap`; deliberately our own drawing, not anyone else's files) and a contextual
  bar under it that shows only what the active tool uses. The surface brush drives it: `kModeSculpt` dispatches
  `surfaceBrushed` to the active tool (Ctrl inverts añadir↔quitar, Alt + vertical drag resizes), `kModeTrim` picks
  the trim contour, and `handleGuideSculptKey` (called first from `MainWindow::eventFilter` and `keyPressEvent`)
  takes +/−, Ctrl+Z, Ctrl+Y, Intro and Esc. `GuideType` decides the envelope (user's rule, 2026-09-17): Le Fort I = Le Fort segment + cranial base
  captured at osteotomy time, before REPOSICION; chin = chin segment + post-genioplasty mandible in their planned
  position. Those optional pre-reposition meshes are persisted with the project. A saw slot is centred on the exact
  saved osteotomy path and clipped by the configured edge margin so guide material remains on both sides. The slot list only shows that
  type's cuts (`m_guideCuts`, filled by `rememberOsteotomyCut` when the wizard executes a Le Fort or genioplasty and by
  the plan on reload). Imported figures reload by file path; integrated splint copies reload by object label while
  keeping their own mesh/transform, and curved tubes persist their three control points. A splint copy plus tubes is
  unioned with the painted guide in the same Boolean build and never modifies the original bite splint.
- Object label constants and `objectActorKey` live in `ObjectLabels.h`. OBJETOS has a row-specific context menu for
  STL export and deletion; its visible delete button calls the same deletion path.
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
  Contour points lie on the buccal/palatal gingiva in any order: everything from their interpolated line to the cusps
  within the convex outline (+10 mm) comes from the scan; CT bone there is replaced only within 3 mm of the kept scan;
  a 2 mm skirt closes the gap to the bone. The block normal is set by jaw (`CompositeJaw`: upper +Z, lower −Z), never
  by the bone centroid, which the mandibular rami pull above the lower teeth.
  The eight-step guide and the MODELOS actions live in a left side panel (`MainWindowModels.cpp`, buttons mirror the
  QActions; the ribbon row is hidden). "Atrás" (`goBackModelWorkflow`) undoes the last step of the current jaw.
  Gates use `ModelWorkflowCore`.
  Each jaw's registration continues to its own block and acceptance before the next jaw.

- Osteotomies: `OsteotomyCore` (line-segment paths, BSSO planes, kerf split, segment references for movement measurements),
  wizard in `OsteotomyWizardPanel` + `MainWindowOsteotomy.cpp`; the plan is saved under the optional `osteotomyPlan` project key.
- REPOSICIÓN analysis in `MainWindowReposition.cpp`: `CollisionCore` intersection volume/highlight, translation/rotation
  restriction, pre-op ghost, impaction per cut-path point (oriented frame, Z+ superior).

`README.md` documents the load pipeline and cache but its build section is outdated.
