# 2026-10-19 — Medular hueca y superficie escalonada (FTD)

**Intent.** The bone came out hollow between the cortices (visible when cutting osteotomies) and rough, with
terraces. The user also integrated TotalSegmentator (maxilla, mandible) + DentalSegmentator (teeth, soft tissue)
with Cursor; that code is on the user's PC, not in any branch.

**Scope.** Medullary fill and mesh smoothing in the repo's pipeline; a refinement for coarse masks ready for the
TotalSegmentator glue. No inference, no GPU, no change to the label map.

**Checklist (evidence).**
- [x] Why the fill failed: `fill_bone_interiors` needs a closed cavity (one pore opens it), rejects the whole
      cavity for one voxel < −300 HU, and stops at 6 mm radius (the mandibular body is wider).
- [x] `fill_medullary_bone` — Python `MedullaryFillTests` (6): porous wide medulla filled ≥ 98 % where the strict
      fill gave 0; groove open to the outside, scan-cut bone, canal, air and soft tissue left alone; mm not voxels.
      A first design (bone on both sides along ≥ 10 of 13 axes) was dropped: a flattened tube — the mandible's
      body — scores like an open groove.
- [x] Terraces: thick slices — `MeshGeneratorTests` sphere wobble 0.104 → 0.055 mm (2.5 mm slices), 0.035 →
      0.025 mm (1.25 mm). Mask in 1.5 mm blocks (TotalSegmentator's grid): 0.30 → 0.26 mm with smoothing alone —
      not fixable at the mesh without losing anatomy.
- [x] `refine_coarse_bone_mask` — Python `CoarseMaskRefinementTests` (4): blocks follow the bone again
      (< 10 % of the mismatch left), only near the boundary, marrow behind the cortex kept, teeth untouched.
- [x] Suites: Python 46 OK (1 skipped), app 30/30, Linux core 10/10.

**Next step.** The user pushes the TotalSegmentator integration; it should call `refine_coarse_bone_mask` on the
TotalSegmentator bone before `thicken_thin_bone` (or set `DENTALSEGMENTATOR_REFINE_COARSE_BONE=1`), and the
result is validated on the real CT with the acceptance list of `medical-image-segmentation` §4.

**Merge with the user's Cursor work (2026-10-19).** `cursor-totalsegmentator` (TotalSegmentator bone,
`fillBetweenCortices` in C++, GPU temperature pause at 80 °C, tooth sidecars) merged. `MeshGenerator.cpp`
conflict: their marrow fill runs first (`contourSource`), then `smoothBoneField`. With TotalSegmentator bone
`conservative_mask` returns the mask as is (their choice); `refine_coarse_bone_mask` is applied there only with
`DENTALSEGMENTATOR_REFINE_COARSE_BONE=1`, because carving < 100 HU could open the thinnest maxillary walls.
Python 56 OK, Linux core 11/11, app 30/32: `RepositionWorkspaceTests` ("Hiding the objects left their masks
hidden in 3D") and `SplintWorkspaceTests` ("teeth and gingiva are not the default composite method") fail on
their branch alone too — behaviour changes from the Cursor work, pending the user's answer.

**Freeze (user's report, 2026-10-19: «se congeló la pantalla», during the segmentation).** The app reads the
script's output asynchronously, so the run itself cannot block the window; but when it ends the bone meshes are
built on the UI thread, one label after another. Measured on a 512×512×400 skull shell (0.4 × 0.4 × 0.5 mm, 4
cores, Linux): the user's code before the merge took 20 s per bone and 1.7 GB; after the merge 38 s and 2.5 GB —
`smoothBoneField` ran its two distance transforms and `vtkImageGaussianSmooth` on one core (16 s) and kept six
float grids alive. Now: distance transform, own separable Gaussian, ridge and refinement split over the hardware
threads (`parallelFor`, as in `ImplicitCore`), the field written straight into the image that is blurred in
place, labels read through their own type instead of `GetTuple1`: 18.5 s and 1.3 GB, my part 4.5 s. The rest is
`fillBetweenCortices` (4.4 s), marching cubes, clean, sinc and normals (~10 s), as before.
`MeshGeneratorTests` unchanged (wobble 0.054 mm at 2.5 mm slices). The run's output is now also written as it
arrives to `segmentacion.log` in `%LOCALAPPDATA%\…\DicomCache\AISegmentation\<fecha_hora>\`, with the time of
each chunk: a frozen or killed run left nothing to read. Still open: which step froze (the log says it next
time), and whether the whole PC froze (GPU/RAM under TotalSegmentator) or only the window.
App 30/32 (same two Cursor failures).
