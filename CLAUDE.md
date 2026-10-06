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

Second PC (laptop, `C:\MaxilloApp`): VS 2022 Build Tools, so configure with `-G "Visual Studio 17 2022"` and the same
paths. It has hybrid graphics (Intel UHD + RTX 3050): `RepositionWorkspaceTests` fails on the Intel GPU ("Foreground
opacity was not blended", on `main` too) and passes once Windows → Pantalla → Gráficos sets its exe to «Alto
rendimiento». Run the render tests on the discrete GPU.

Tests (CTest):
- `GeometryCoreTests`, `BoneCavityFillTests`, `MeshGeneratorTests` — plain C++ executables
- `MaskToObjectTests` checks exact label extraction, committed cavity filling and immutable input.
- `ModelWorkflowTests` checks guided MODELOS steps, paired point requirements, fine adjustment and mandatory acceptance.
- `SplintHeightmapTests`, `SplintDesignTests`, `SplintContourEditTests`, `SplintPreviewSchedulerTests`, `ProjectSerializerTests`, `CompositeBlockTests`, `MeshRepairTests`, `OsteotomyCoreTests`, `CollisionTests`, `ImplicitCoreTests`, `WrapCoreTests`, `GuideBaseTests`, `CutSlotTests`, `GuideDesignTests`, `GuideSculptTests`, `PlateTests`, `LeFortGuideTests`, `LeFortMotionTests`, `LeFortHoleTests`, `RootAnalysisTests`, `GuidePlanTests`, `SegmentationProgressTests` — core tests declared with `add_core_test()`; synthetic arches in `tests/SplintTestGeometry.h`
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

Linux core build (cloud sessions without the Windows toolchain): `DicomMPRViewer/tools/linux-core-tests/CMakeLists.txt`
builds the guide/plate/serializer core tests only (no app, no workspace tests) and does not replace the Windows
merge bar. It needs VTK 9.5.2 built from source (Ubuntu's 9.1 shifts the geometry by tenths of a millimetre and
fails PlateTests and LeFortGuideTests); Qt can be Ubuntu's 6.4. `QT_NO_EMIT` keeps Qt's `emit` out of oneTBB.

```bash
cmake -S DicomMPRViewer/tools/linux-core-tests -B build-linux-core -DVTK_DIR=<vtk-9.5.2>/lib/cmake/vtk-9.5
cmake --build build-linux-core --parallel && ctest --test-dir build-linux-core --output-on-failure
```

## Architecture

- `MainWindow.cpp` (~12.7k lines) holds every workspace and the guided workflow:
  MPR/segmentation → Modelos (STL arch registration, composites) → Orientación (Frankfurt plane)
  → Osteotomía (Le Fort I, BSSO, genioplasty) → Registro de mordida → Reposición → Férulas.
  Prefer putting new logic in separate core classes (like `TransformCore`, `SplintGenerator`) that tests can link without Qt widgets.
- DICOM load: `AsyncDicomLoader` (worker thread) → `DicomSeriesIndexer` → `VolumeCacheManager` (.mha cache in `%LOCALAPPDATA%`) → `DicomVolumeLoader`.
- `MPRView` (2D reslice views), `Mesh3DView` (3D scenes, gizmos, picking).
- `MeshGenerator::generateMesh`: bone labels with ≥ 25 smoothing iterations are Gaussian-blurred before
  marching cubes; every voxel of the label is floored at 0.55 so a one-voxel wall can never blur under the 0.5
  contour (it used to, and thin maxillary/orbital walls came out as holes — user's report, 2026-10-05).
- AI segmentation: `StandaloneDentalSegmentatorService` launches `scripts/run_standalone_dental_segmentator.py` (nnU-Net DentalSegmentator, weights in `%LOCALAPPDATA%/DicomMPRViewer/DentalSegmentator/ML`).
- `ProjectSerializer` saves/loads `.maxilloproject` files; new keys must stay optional so older projects load.
- Splints: `SplintHeightmapGenerator` (height-map splint, Prepare/Build), `SplintDesignCore` (named designs, JSON),
  `SplintContourEditCore`, `SplintPreviewScheduler` (background preview); UI in `SplintDesignPanel` and
  `MainWindowSplint.cpp` (MainWindow methods kept out of `MainWindow.cpp`).
  Entering FERULA fills in the guide points of every design that has none, off the teeth themselves
  (`SplintDesignCore::AutoGuidePoints` via `MainWindow::autoFillSplintGuidePoints`): the occlusal band is the
  6 mm nearest the bite, its widest direction across the bite is the arch, and the highest cusp of each of five
  bins along it is a point. The composite's dental part is used when there is one, since bone has no cusps. The
  step opens with a preview instead of an empty arch waiting to be clicked (user's request, 2026-09-21) and the
  points stay draggable; anything the surgeon already marked is left alone.
  Only the height-map method is offered: the
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
- `WrapCore::Wrap` is 3-matic's Wrap on those steps. It needs a gap closing large enough to seal the anatomy:
  segmented maxilla is a thin perforated shell, and a closing of a few tenths leaves a shredded film of fragments
  rather than an envelope — never use one as a distance field for "is this inside bone". See `PlateKeepOut` below. closing in real millimetres (`gapClosingMm`,
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
  brazo» between arms. How a plate is built (user's reports, 2026-09-19: dived into the gap, went through real bone,
  turned on edge, rings like hooks — the last three only reproduced on the user's own anatomy, see PlateProbe):
  `walkOnBone` walks the surface from each hole towards the next (1 mm tangent steps dropped back onto the planned
  wrap, normals averaged over ~1 mm by `smoothNormal`, since segmented bone's raw gradient swings tens of degrees per
  voxel) and stops where the plate could not seat: a turn > 40°/step or > 60° from the hole, a hollow, no progress,
  or `PlateCore::MakeBoneQuery` saying no bone — the nearest planned bone, but nothing within 1.5 mm of the
  osteotomy or on its wrong side (points on the segment taken back through the motion first), so no arm goes round
  the edge and down the cut face. `strutPath` joins the two walks; what is left is a band pressed onto the bone
  wherever `boneAt` says there is bone within a millimetre, spanning only the rest — the osteotomy and its
  margins. `tautBand` does both: it pushes points out of `boneAndGap` in the arm's plane, presses them onto the
  planning wrap a quarter of a millimetre at a time (free to move sideways: it is following a surface, not
  spanning one), and smooths. Tautness fights the pressing, so the band wraps a convex wall and still bridges a
  notch rather than diving into it. Samples that come to rest on bone are marked seated and take the conformal
  layer; only what crosses the cut stays flat. Without the pressing the band was only ever pushed OUT of bone,
  so a chord already clear of it stayed straight: where a walk stopped early on a curved buttress the whole
  remaining arm was a flat blade in the air, and only the pillar whose walk happened to succeed came out right
  (user's report, 2026-09-21). What the bridge used to be, for the parts that still hold:
  straight, and where it would cut through bone (the corner of an advanced segment) its deepest point is lifted
  onto the surface and it bends there, recursively. The bar's normal comes from the plate's width axis (the bone
  normal × the arm), never from the bone normal squared to the bar (that degenerates when a large advancement runs
  the bar along an anterolaterally facing normal: the bar went on edge). The plate is then two parts: seated on
  bone (walked stretches and the hole rings), a layer 0..thickness of the planned wrap's distance within capsule /
  ring footprints, rims rounded with `SmoothIntersect` — bent to the bone like a real PSI plate; and the bridge, a
  flat sweep (`BakeFunction`, nearest mitred piece, rounded rectangle) intersected with outside the bone. The planned
  wrap uses a 3 mm closing: segmented maxilla has perforations and thin walls that a conformal plate copied as ragged
  patches. Rings, bores and countersinks use the bone normal at the hole (never the smoothed ribbon normal: the
  bridge's slope tilted the rings and the bone carved them into hooks); normals are smoothed only along seated
  stretches. Slivers under 3 % of the plate are dropped; a real break is still reported. The seated outer face lies
  on a Gaussian-smoothed copy of the bone field (`smoothedField`, σ 1.2 mm, local to the plate) with a 70 % minimum
  thickness over bumps, and the seated centreline is smoothed too: the plate is uniform like a machined one while
  its inner face follows the bone (user's report 2026-09-19: "tiene como irregularidades"). Across the osteotomy the bridge
  is a band pulled taut over `PlateKeepOut::boneAndGap` (`tautBand`: the straight chord relaxed by alternating a push
  out of that field, in the arm's plane only, with a Laplacian smoothing pass). That field is a 0.5 mm-closing wrap of
  the two planned bones AND the Le Fort segment in its pre-operative position, so it also fills the space the movement
  vacated: the band ramps across the step instead of dropping into the cut and standing in the way of the maxilla
  (user's report, 2026-09-20: "las placas se meten a la osteotomía e interfieren con el lefort"). This replaced a
  hand-drawn right-angled dogleg, which showed on the patient as a zig-zag no surgeon would bend, and it removes the
  need to tell an advancement from a flat span — with nothing in the way the taut band is simply the straight line.
  `PlateKeepOut::bone` clips the whole plate. It is `ImplicitCore::BakeMeshField` of the planned meshes THEMSELVES,
  not a wrap: exact on the surface, and "inside" means inside the wall rather than inside the sinus. The wrap the
  plate is laid on is quantised to voxel centres and stands up to a voxel inside the bone, which put the plate half
  a millimetre into the maxilla; clipping against the bone's own field brings that to a tenth (user's report,
  2026-09-21). `PlateKeepOut::boneAndGap`, the one an arm is routed over, has to be the opposite — a CLOSED
  envelope, with the same gap closing as the planning wrap. This is the whole point: segmented maxilla is a thin perforated shell
  round an open sinus, so a field taken from the meshes themselves lets an arm walk straight through the sinus, and
  a wrap closed by only 0.5 mm comes out SHREDDED — on the surgeon's own CT such a wrap left 63 % of the cranial
  bone's vertices more than 1 mm outside it, so the keep-out reported no bone anywhere and every earlier
  "0 vertices inside the bone" was a measurement against nothing (user's report, 2026-09-20: "las placas siguen
  entrando a la osteotomía"). `PlateTests` "the keep-out is closed bone" builds a 1 mm perforated wall round a
  12 mm cavity and fails at a 0.5 mm closing. The gap wrap is made alongside the planning wrap in
  `prepareGuidePlannedBone`. The bridge's pieces are mitred (overlapping pieces showed
  rings) and its width axis comes from the arm's plane (the bone normal at the piriform rim faces outward too and sent
  it on a lateral detour). `steppedBridges` now counts the arms whose band actually had to climb. The bridge is
  filleted onto the seated part with a smooth union (the negated `SmoothIntersect`, radius half the thickness), not
  merely unioned with it: the seated part takes the bone's shape and the bridge is flat, so where the arm leaves the
  bone their faces diverge at once and the joint read as one plate laid over another ("cuando se hace el doblez se ve
  montado"). `cutEdgeMarginMm` is 2.0 mm, so an arm seats to within 3.5 mm of the cut and the plate reaches the
  superior Le Fort border as the published implants do; at 3.5 mm it lifted off 5 mm short of it. Step 9 always shows the Le Fort in its planned position (it used to depend on a guide mesh
  being in memory, so after reopening a project the advancement "disappeared"), and `guideMotionSummary` states the
  movement (advance/retreat, ascent/descent, lateral, rotation) at the top of the plate report, or warns when the Le
  Fort has not been moved. `bridgedMm` reports the
  bar. PlateTests: 8 mm gap, 8 mm advancement on a wall facing 30° outward, curved rough wall.
  `tools/PlateProbe.cpp` (target `PlateProbe`, not a CTest) builds paranasal plates and an L plate on a real
  project's pre-reposition bones with a given advancement/descent, reports pieces, bridge, fit gap and penetration,
  then lays out and builds the cutting guide those plates imply, and renders frontal/oblique/lateral PNGs of both:
  `PlateProbe.exe --project x.maxilloproject --out dir --advance 6 --down 3 [--template paranasal|splintless]`.
  The splintless template is the four-pillar plate the surgeon actually builds, and it is the one that exposes the
  guide's connectivity.
  Defaults (user's choice): 1.0 mm plate, 2.0 mm screws, guide fixation 1.5 mm, one-piece guide across the midline;
  sleeve bore 1.6 mm / outer 4.2 mm / height 4 mm. `PlateCore::CheckHoleSeat` BLOCKS a hole where a screw
  could not hold: it needs bone all round its ring (16 samples at `ringDiameterMm`/2 + `minEdgeDistanceMm`, 1.0 mm by
  the user's choice, each within 1.5 mm of bone) and `minCutDistanceMm` to the osteotomy. Both are judged on the bone BEFORE the
  movement — that is where the drill goes, through the guide — because on the planned anatomy the osteotomy
  itself reads as a free margin and no screw could be placed near the cut at all. The ring's plane comes from
  the gradient of the bone's own distance a millimetre off the surface, not from the envelope's normal, which
  swings tens of degrees where the wrap is coarse and tilted the ring off a flat wall. A screw on a bony margin
  — the piriform rim, the lower border of the fragment, the edge of the segmentation — has nothing to hold it
  and the guide's sleeve would stand on air (user's rule, 2026-09-20: "no dejes que los orificios se coloquen en
  la orilla"). `MainWindow::guidePlateHoleSeat` calls it on every click in `kModePlateHoles` and refuses the
  point with the reason in the status bar and the plate report. `Check` still warns (never blocks) on < 2 screws
  per bone, overlapping rings and holes on the wrong side of the cut; `Build` reports the gap under each hole to the real bone (passive fit). The plan keeps
  `plates`, `plate` and `sleeve` as optional keys. UI in `MainWindowGuides.cpp` («PLACAS A MEDIDA» section, mode
  `kModePlateHoles`, `m_guidePlannedView` swaps the scene to the planned bone with the plates, predictive holes are
  purple markers on the pre-operative view, `exportGuidePlateFiles` writes `placa_N_lado.stl` plus
  `informe_placas.txt`). Not done yet: bone thickness under each screw from the CT, root proximity (teeth are not
  segmented separately), posterior bony interference, postoperative accuracy report.
- `LeFortGuideCore::Layout` lays out the Le Fort cutting and drilling guide of that workflow from the plan (user's
  report, 2026-09-19: the plates were new but the guide was still the hand-painted one). One piece across the midline
  on the anterior wall of the bone before the cut: a band of brush dabs (5 mm) along the osteotomy — per 2.5 mm bin
  across the plates' lateral extent (+5 mm), the most anterior forward-facing envelope vertex with |PathField| < 0.75;
  where the cut crosses the piriform aperture and there is none, the band dips to the alveolar wall just below it;
  a pad round every predictive hole, with a stem to the band if needed; the slit as `GuideSlot` pieces with ends,
  broken by 3 mm bridges at the midline and every 15 mm, so the halves stay rigid. The slit spans the whole band and
  comes from the plan, not from the envelope: `GuideDesignCore` cuts it with a slab of the planned osteotomy clipped
  to the guide's own material, so ending each piece at the last wrap vertex that happened to land exactly on the cut
  only produced a row of stubs (user's report, 2026-09-20). Where the band dips below the aperture it is a 0.75-width
  strap (a full band there was a slab over the nose) and there is no material on the cut for the slit to open.
  Positioning screws of 1.5 mm hold the guide while the holes are drilled and the cut is made, as both published
  protocols do (Gander 2015 "fixed with two 1.5-mm screws"; Ho 2025 "two or four monocortical positioning screws"):
  one at each lateral end, above the cut only when there are plates (the cranial side does not move), four above and
  below without them. The band is 16 mm wide and SOLID: the openwork cells (`latticeCellMm`, subtracted `GuideFigure`s) of
  2026-09-20 are off by default since the surgeon rejected them on the real case (2026-10-06: "esos orificios que hay
  de más, no me gustan"); with two guides `hullOutline` (the UI sets it) shapes each guide as the rounded convex hull of its sleeves
  (pads), positioning screws, labels and the cut ± `slitMarginMm` (5), seen as lateral × height above the cut, filled
  with dabs no larger than their depth in the hull — the printed guides the surgeon showed (2026-10-06); a tall
  nasomaxillary column no longer lifts the whole side. Labels are placed inside the hull first (full size, then
  0.8 and 2/3), never over the nose, and only then on their own strip. `keepOut` (the infraorbital foramina the
  surgeon marks in the guided steps 5–6 or with «3b · Marcar agujeros infraorbitarios», `GuidePlan::foramina`, optional key) is erased last with
  `keepOutRadiusMm` (5) and positioning screws avoid it (LeFortGuideTests "each guide is the hull of what it
  carries", "the guide keeps clear of the infraorbital foramen"). The screw is the surgeon's choice
  (`SleeveParams::screwDiameterMm`, «Tornillo» in step 4); the guide's hole is `PlateCore::PilotDrillFor` it
  (2.0 → 1.6 mm); plans saved without it get 2.0 / 1.6. A guide the assistant laid out (`GuidePlan::assistant`, optional key) is rebuilt only from step 4: the hand-drawn flow's «Reconstruir guía», its «Anterior» and EDITAR are hidden (user's report, 2026-10-06). `connectPaint` then guarantees it: the dabs are grouped into components
  by real overlap (allowing for the millimetre the region is opened by), and every patch but the largest is
  joined to it by a strap of dabs laid on the surface, nearest points first. A pad round a sleeve that touches
  nothing else is a hole drilled with no material under it, which is what the surgeon got ("que no queden
  espacios donde se perforó sin material"). `LeFortGuideTests` "every sleeve has guide under it" puts a plate
  hole far out on the lateral wall and requires the paint to come out as one patch covering it. It only fills
  the plan (paint, slotPlan, holes); `GuideDesignCore::Build` still carves it, and the brush and EDITAR still work.
  The guide is clipped out of the bone the same way the plates are: `GuideDesignParams::bone` is
  `ImplicitCore::BakeMeshField` of the guide's own source meshes, baked beside the envelope in `computeGuideWrap`,
  and `Build` intersects the finished solid with the outside of it, offset by half the contour detail plus 0.1 mm.
  Without it the guide stood half a millimetre inside the maxilla, because the wrap it is laid on does; the offset
  is there because 70 sinc passes pull a convex surface inwards after the field is contoured. `LeFortGuideTests`
  "the guide is one piece with an open slit" now also requires no vertex inside that field.
  The frame no longer averages the holes' drill axes: a hole whose envelope normal came out skewed tilted the whole
  band ("mal orientada"). The direction is the osteotomy's own sweep axis, checked against the bone; the holes only
  give the centre. And a hole's axis — the screw, the drill's vector and the guide's sleeve — is now
  `PlateCore::BoneNormalAt`, the gradient of the bone's own field averaged over about a millimetre, instead of the
  envelope's normal, which swings tens of degrees on a flat wall and left sleeves visibly tilted.
  HOW TO MEASURE any of this: never against a wrap. A tight one is a shredded film, a loose one stands proud of
  every concavity, and a mesh's own signed distance calls the whole cranial cavity "inside" by tens of millimetres.
  `ImplicitCore::BakeMeshField` of the meshes is the instrument, and `PlateProbe` validates it on the spot by
  requiring every bone vertex to read within a millimetre of zero before it reports anything.
  UI: «Generar guía de corte» in «PLACAS A MEDIDA» (`MainWindow::generateLeFortGuide`). With plates, the hand-drawn
  steps (zona, ranuras, agujeros, crear) stay hidden until the guide exists and then serve to retouch it («Reconstruir
  guía»); without plates a plain Le Fort guide is still drawn by hand. `LeFortGuideTests` uses a synthetic aperture.
  The Le Fort path: the osteotomy plan only kept the wizard's current type, so after Le Fort → BSSO → genioplasty and
  a reload the cut was gone (user's report: «La trayectoria de corte necesita al menos 2 puntos»). Executed cuts are
  now saved as `executedCuts` in the osteotomy plan and restored by `restoreGuidePlan`; older projects rebuild the Le
  Fort path from `m_segmentReferences[kLeFortSegLabel].landmarks` (`recoveredLeFortPath`, points stored pilar R,
  piriform R, piriform L, pilar L). `rememberOsteotomyCut` merges cuts with the same points (slot pieces of one cut).
- Movement-driven Le Fort guide (spec `02-DOCS/wiki/sdd/specs/guia-lefort-por-movimiento.md`, user's decisions
  2026-10-04; UI 2026-10-05). Only changes of plane shape the guide —
  rise/drop, clockwise/counter-clockwise rotation, cant; advancement, lateral shift and yaw are the plate's.
  `LeFortMotionCore::Band(cut, motion)` gives the band an impaction takes out: at each cut point the rise is
  `Z·(M·p − p)` (no term in a horizontal translation or a yaw, so the advancement drops out by construction;
  the same "Z +x impactación" REPOSICIÓN shows), linear along each piece of the cut. `spans` are the stretches
  where it is ≥ 0.5 mm, as arc length from pilar D with exact threshold crossings (`PointAlongCut`,
  `CutLength`); `upperCut` is the cut raised point by point; kind Impaction/Descent/Mixed/NoPlaneChange, pitch
  (counter-clockwise = the front rises more) and cant; `noMotion` when REPOSICIÓN was not done. The band is
  never saved: it is recomputed from the movement. `LeFortHoleCore::Support(site, axis, ctx)` judges a hole
  on the bone BEFORE the cut: no bone within 1 mm → refused; a cranial hole inside the band or < 4 mm above
  it → refused (that bone is taken out; judged before the ring so the reason is the band, not the margin it
  creates); then `PlateCore::CheckHoleSeat` (site carried to the planned position for the segment); then the
  bone along the drill on `BakeMeshField` of the meshes (entry/exit zero crossings) — < 2.0 mm is a WARNING,
  not a refusal (the surgeon decides). `LeFortHoleCore::Propose(ctx)` puts 2 above + 2 below the cut at each
  pillar (the cut's four points): a 1 mm grid, ±8 mm lateral, 4–12 mm from the cut or the band's upper edge,
  each node found by walking along −`ctx.anterior` into the bone field (the cut's sweep axis has no sign),
  kept only if `Support` is Ok, thickest first (to 0.25 mm), 6.5 mm apart; a pillar without sound bone goes
  to `missing` with a Spanish reason, never a weaker site. `LeFortGuideCore::Layout(..., params, &band)` adds
  the second slit along `upperCut` inside each Le Fort slit piece (same bridges), only where the rise is
  ≥ 0.5 mm; paints a second row of dabs on the band's upper edge when the band is taller than the first row
  covers; raises the cranial positioning screws by the rise; keeps lattice cells `rowOffset` from both slits.
  Without a band the layout is unchanged. The band spans the cut's ends ∪ the holes (it used to span only
  the holes and shrank to a block round the one sound site of a real maxilla). No guide in the nose nor on the
  anterior nasal spine (user's rule, 2026-10-05): the piriform rims are the last bins on the cut either side of
  the wall-less ones, dabs beside them shrink to stop at the rim, and the sides are joined by ONE bridge on the
  alveolar wall whose upper edge is `spineClearanceMm` (3) below the spine — the run of slices from the
  aperture's floor down standing > 1.5 mm in front of the wall's median depth — ramping up beside the rims
  (never above the floor over the opening) so it overlaps the band. `GuidePlan::lefortHoles` saves the guide's holes (center, axis,
  pillar, side, origin auto/manual) under the optional `lefortHoles` key; support is not saved.
  UI (`MainWindowGuides.cpp`): once the envelope exists, «Proponer orificios» computes the band (drawn red on
  the bone) and proposes the sites, keeping the surgeon's manual ones (`Propose(ctx, manual)`); they are only
  markers until «Aceptar orificios y crear guía» (`acceptLeFortHoles`, user's request 2026-10-05) lays the guide
  out with `Layout(..., &band)` and builds it with `DrillSites(lefortHoles)` as sleeves. The guide comes before
  the plates: plates designed earlier are only reported (`UnmatchedPlateHoles`); a legacy project with plates
  and no sites keeps drilling the plates' holes. The upper slit's pieces follow `band.upperCut`, so
  `guideChosenSlots` adds them only while the movement still gives that band. The band is drawn red on the
  bone itself between the two cut surfaces (`BandOnBone`, `kGuideBandActorKey`, `CranioPalette::resection`)
  and recomputed with the envelope. «Mover orificio» (`kModeMoveHoles`) shows the
  sites as draggable markers by verdict; a drop goes through `MoveHole`: Rejected stays put with the reason in
  the status bar, Warning/Ok moves it (manual) and, once the guide exists, rebuilds it with everything else unchanged.
- Le Fort guide assistant (spec `02-DOCS/wiki/sdd/specs/asistente-guia-lefort.md`, user's real case of
  2026-10-05): the GUIAS panel for Le Fort is four steps, each automatic and editable. 1 · `RootAnalysisCore`
  measures the upper roots off the «Dientes superiores» object (label 3): apices = height maxima of the teeth
  seen FROM ABOVE (a 2D grid over the horizontal plane, triangles sampled every half cell, flat tops merged to
  their middle, ≥ 4 mm apart, the higher kept) — a grid along the cut alone stacked the molars behind one
  another and the "first molar" took an unerupted third molar's apex above the cut (user's case, 2026-10-05);
  distance = `−PathField` to the cut; canine = longest root within 12 mm (horizontal) of the piriform point,
  first molar = root nearest the pillar point (≤ 14 mm) among those farther from the midline than that side's
  canine; the report, the overlay lines and their labels (`Mesh3DView::setOverlayLabels`, billboard text) give
  only apex → osteotomy for those four («el corte cruza la raíz» when negative). The teeth come from a SIDECAR the segmentation script
  writes (`<output>_dientes_superiores.nrrd`, DentalSegmentator label 3) and `importUpperTeethSidecar` loads
  hidden — the labelmap still maps 3 → 5, so the maxilla and the Le Fort segment keep their teeth.
  Real case (2026-10-05): `BandOnBone` only draws within 6 mm (across the vertical) of the pillar–piriform pieces
  of the cut, never medial to a rim — the two cut surfaces cross the whole skull (palate, orbits). `Propose` only
  searches outward of a piriform rim and drops sites whose drill axis is > 60° off the anterior (lateral zygoma),
  proposes thin bone (Warning) after sound bone instead of nothing, and an empty pillar's reason is the refusal
  most of its sites got; «Aceptar» also shows after an empty proposal (guide with positioning screws only);
  holes are 2 above + 2 below ON each pillar (user's rule, 2026-10-05): ±5 mm, ranked tier (Ok, thin, near a
  root) → nearest the pillar line → thickest, and the two are chosen as the best PAIR ≥ 6.5 mm apart (greedy
  left one per pillar where the usable bone was short, 2026-10-06); a drill path within 1 mm of `LeFortHoleContext::teeth`
  (BakeMeshField of label 3) to 6 mm deep is a WARNING naming the root (`nearRoot`), not a refusal.
  The aperture is only the wall-less gaps whose middle lies inside the piriform points (a perforation in a
  lateral wall cut the left guide back short of the pillar). The Le Fort guide is built WITHOUT the bone clip
  (`design.bone` reset for the build only; user's request 2026-10-06: "sobre el envolvente, no sobre el hueso"),
  and the maxillomalar holes are anchored over the first molar (`LeFortHoleContext::anchors`, the molar's apex on
  the cut; «Proponer» measures the roots first if needed) — the pillar point sits ~19 mm behind it on the real
  case. The holes are now MARKED BY THE SURGEON, guided (user's request 2026-10-06: «Proponer» hidden, no zone
  limits, on the envelope): «3 · Marcar orificios (guiado)» shows only the envelope, with the band drawn on it (`kGuideBandWrapActorKey`, 0.25 mm proud), and walks nasomaxilar D →
  maxilomalar D → nasomaxilar I → maxilomalar I («Siguiente pilar», «Quitar último», `m_guideMarkStep`,
  `showGuideMarkPrompt`); each click is `LeFortHoleCore::MarkHole` — exactly at the click, envelope normal as
  axis, never refused (a Support refusal becomes a warning). `AddHole` (nearest pillar, onto the bone) stays in the
  core. «Borrar orificios» clears them; after building, the guides are shown on the envelope, not the bone; the layout then ends each side `lateralMarginMm` past its outermost hole (`extentFromHoles`, set by the
  UI) — user's request 2026-10-06. The envelope's gap closing defaults to 6.0 mm (was 4.0, 2.5) and
  Le Fort projects saved with less are raised on reopening; the segmentation script thickens bone
  (`thicken_thin_bone`: grow ≤ 1.2 mm into ≥ 100 HU; seal: a non-air voxel with bone within 2.5 mm on BOTH
  sides along ≥ 2 of the 13 grid axes, up to 4 passes — a ball closing never seals a hole in a one-voxel wall;
  `DENTALSEGMENTATOR_BONE_GROW_MM/_GROW_HU/_SEAL_MM`; ~6 s per pass on a 400³ head). `cap_canal_openings` turns
  `bone_thickening_params(label)` gives the maxilla its own seal (3.5 mm, down to −700 HU: its anterior wall over the
  sinus is so thin that a gap reads as half air, ~−400 HU; user's report 2026-10-06 "enfócate en el maxilar"); the
  mandible keeps 2.5 mm / −200 HU. `cap_canal_openings` turns
  canal voxels (app label 7) within 1 mm of the outside into mandible: the mental foramina showed the canal
  through two holes either side of the chin (user's report, 2026-10-06). The band spinboxes are synced whenever the band is recomputed.
  2 · `LeFortMotionCore::BandFromHeights` builds the band from the surgeon's four heights (`GuidePlan::bandHeights`,
  optional key; `guideLeFortBand` prefers them; «Restablecer» clears them). 3 · holes as before. 4 · two guides:
  `LeFortGuideParams::separateSides` (the UI always sets it) — slits only over `slitLateralFraction` (0.65) of
  each guide from the piriform rim, the lateral end left whole so the guide cannot split along them (user's
  report, 2026-10-06) — no bridge, `connectPaint` per side, a second
  positioning screw at each guide's inner end, `SplitBySide` → `guia_der.stl` / `guia_izq.stl`
  (`exportLeFortGuideFiles`); `GuideEngraveCore` engraves `caseLabel` (optional key) above the cranial screws and
  DER / IZQ below the caudal ones: VTK's font rasterised, strokes thickened to print, stacked into a closed solid
  sitting on the guide's OUTER face (`labelWallMm` = clearance + thickness; on the bone it was buried in the
  wall), with its own strip of material and the cells under it removed.
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

## Knowledge map

| What | Where |
|------|-------|
| Project constitution (SDD non-negotiables) | `02-DOCS/wiki/sdd/constitution.md` |
| Full knowledge map (specs, decisions) | `02-DOCS/wiki/index.md` |
