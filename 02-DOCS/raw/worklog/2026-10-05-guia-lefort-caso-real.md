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
