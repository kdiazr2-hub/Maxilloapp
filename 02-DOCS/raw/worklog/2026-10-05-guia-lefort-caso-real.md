---
type: worklog
title: Guía Le Fort — correcciones del caso real
timestamp: 2026-10-05T00:00:00Z
tags: [guias, lefort, worklog]
---

# Guía Le Fort — correcciones del caso real (2026-10-05)

Five screenshots of the surgeon's real case after the four-step assistant showed:

1. The red band covered the whole skull (palate, posterior maxilla, orbits, aperture): the two cut surfaces
   extend everywhere. `BandOnBone` now keeps only bone within 6 mm (across the vertical) of the pillar–piriform
   pieces of the cut, and nothing medial to a piriform rim.
2. The four band spinboxes read 0.0 until «Proponer orificios»: they are synced every time the band is recomputed.
3. Holes were proposed under the aperture by the incisor roots and on the lateral zygoma: the piriform pillars
   search outward only, and a proposed site needs its drill axis within 60° of the anterior.
4. No root rule: `Support` refuses a drill path that passes within 1 mm of the upper teeth (6 mm deep).

Evidence: LeFortGuideTests, LeFortHoleTests (+3), SplintWorkspaceTests (12 sites, none over a root);
Linux core and full app suites. Pending: Windows rebuild and the same real case.

## Second round (same day)

5. Root measurements were not apex → osteotomy of the canine and first molar: apices were found along the cut
   only, so behind the first molar the 2nd/3rd molars stood in for it. Now a 2D height map seen from above;
   report and overlay show only apex → osteotomy for the four named teeth.
6. «Proponer orificios» gave nothing: thin anterior wall was never proposed. Now proposed with a warning; the
   empty pillars say why; the guide can be created without plate holes.

## Third round (same day)

7. Roots: one first molar was not found (pillar point > 8 mm away) and the lines carried no value. Reach 12/14
   mm, the molar taken behind the canine, and each measurement labelled «Canino D: 3.2 mm» on the view.
8. Holes: 2 above + 2 below ON each pillar — ±5 mm, nearest the pillar line before thickest; near a root is a
   warning, so the pillars below the cut keep their holes.
9. Left guide short of the zygomatic pillar: a perforation on the cut was read as part of the nasal aperture.
   Only gaps inside the piriform points are the aperture. Envelope closing 2.5 mm; segmentation thickens thin
   walls from the CT and seals pinholes denser than air.

## Fourth round (same day)

10. "Salen más huecos" after re-segmenting. Adding voxels cannot open holes; the holes come from
    `MeshGenerator`'s Gaussian pre-smoothing, which averaged one-voxel walls under the 0.5 contour. The label's
    voxels are now floored at 0.55 before contouring (MeshGeneratorTests: one-voxel wall at 25/40/70 iterations,
    red → green).

11. "Mejoró pero aún sale con orificios": grow 1.0 mm into ≥ 100 HU; the ball closing could never seal a hole in
    a one-voxel wall (the ball reaches past the wall), so the seal is now "bone on both sides within 2 mm along
    ≥ 2 grid axes", repeated up to 4 passes (Python: a 3 mm perforation sealed, a 110 HU wall recovered).

12. "Mejor, ya casi" (2026-10-06): grow 1.2 mm, seal 2.5 mm — openings of ~5 mm filled with soft tissue (the last spots under the orbits) close; air openings still stay open.

13. "Intenta cerrar esos dos orificios" (2026-10-06): the two holes by the chin were the mental foramina — the canal label reached the surface and sealing never touches other labels. `cap_canal_openings` gives the outer millimetre of the canal to the mandible (Python: canal exit 7 voxels wide closed, canal inside kept).

14. "No, me refiero a los orificios del maxilar" (2026-10-06): the anterior maxillary wall over the sinus reads around -400 HU in its gaps (partial volume with the sinus air), below the -200 HU "not air" limit. The maxilla now seals down to -700 HU and up to ~6 mm (3.5 mm reach); -1000 HU openings stay open; the mandible keeps its rule (Python 36 OK; ~18 s on a 400^3 head).

15. (2026-10-06) Slits ran the whole guide and nearly split it: they now cover 65% of each guide from the piriform rim, the lateral end whole (LeFortGuideTests "the slits leave the lateral end solid", red: slit to x 25 → green). Holes came one per pillar: greedy took the middle of short bone; now the best pair (LeFortHoleTests "two holes fit where the bone is short", red → green).

16. (2026-10-06) "La guía debe crearse sobre el envolvente": the Le Fort build drops the bone clip; envelope gap closing 4 mm. Holes off the pillars: the maxillomalar anchor is now the first molar on the cut (LeFortHoleTests "the pillar holes follow the first molar").
