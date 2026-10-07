# OpenVDB vs today's guide contour (2026-10-07)

Question (user): "¿Se puede usar una librería mejor que VTK 9.5 para que me genere mejores guías y placas?"

Tool: `DicomMPRViewer/tools/GuideResolutionProbe.cpp` (Linux core build, optional target when libopenvdb-dev is
installed). Same Le Fort guide (synthetic anatomy of LeFortGuideTests, 8 sleeves, case label, hull outline) built:

- A — today: `GuideDesignCore::Build`, dense grid 0.25 mm, Flying Edges, 70 windowed-sinc passes, repair.
- B — OpenVDB 10: the same implicit solid (`GuideDesignCore::SolidNode`) sampled at 0.10 mm only in a narrow band
  (coarse 0.5 mm pass picks the cells the surface crosses), signed flood fill, `tools::volumeToMesh`, 15 sinc passes,
  same repair.

| | A today | B OpenVDB 0.10 mm |
|---|---|---|
| time | 3.0 s | 15.8 s (4 threads) |
| voxels | 1.1 M (dense) | 8.9 M touched; a dense 0.10 grid would be 46.7 M |
| peak memory | — | 511 MB (whole probe) |
| triangles | 123 k | 785 k |
| bore, nominal 1.6 mm | 1.53 mm (worst −0.08), roundness sd 0.058 | 1.60 mm (worst 0.00), sd 0.040 |
| closed after repair | yes | yes |

Renders (A left, B right): `1_camisa_frente.png`, `2_camisa_lado.png`, `3_texto.png`, `4_guia_derecha.png`.
Sleeves come out as true cylinders with crisp rims, the engraved number becomes legible, the slit's edges are
straight. Most of the softness of A is the 70 sinc passes over a 0.25 mm grid melting every analytic edge.

Caveats: synthetic box anatomy, not the patient; the base's inputs (envelope, brushed columns) are still baked at
0.4 mm, so the support surface itself is no sharper — only what is analytic (sleeves, slots, holes, text) gains.
785 k triangles is 6× today's STL; decimation of flat areas would be needed before export.
