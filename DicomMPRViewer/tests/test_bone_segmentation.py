import importlib.util
import json
import os
import sys
import time
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import numpy as np
import SimpleITK as sitk


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "run_standalone_dental_segmentator.py"
spec = importlib.util.spec_from_file_location("dental_segmentator", SCRIPT)
segmentator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(segmentator)


def shell(shape=(21, 21, 21), source_label=2):
    labels = np.zeros(shape, dtype=np.uint8)
    labels[3:-3, 3:-3, 3:-3] = source_label
    labels[5:-5, 5:-5, 5:-5] = 0
    return labels


class BoneInteriorTests(unittest.TestCase):
    def complete(self, labels, source_label=2, intensities=None, spacing=(0.5, 0.5, 0.5)):
        if intensities is None:
            intensities = np.full(labels.shape, 50, dtype=np.float32)
        before = labels.copy()
        mask = labels == source_label
        result = segmentator.fill_bone_interiors(mask, labels, intensities, spacing, source_label)
        np.testing.assert_array_equal(labels, before)
        np.testing.assert_array_equal(mask, before == source_label)
        return result

    def test_both_bones_fill_without_exterior_growth(self):
        for source_label in (1, 2):
            with self.subTest(source_label=source_label):
                labels = shell(source_label=source_label)
                expected = np.zeros(labels.shape, dtype=bool)
                expected[3:-3, 3:-3, 3:-3] = True
                np.testing.assert_array_equal(self.complete(labels, source_label), expected)

    def test_protected_structures_are_not_replaced(self):
        labels = shell()
        labels[9:12, 9:12, 9:12] = 5
        labels[6, 6, 6] = 1
        labels[7, 7, 7] = 3
        labels[8, 8, 8] = 4
        result = self.complete(labels)
        self.assertTrue(result[12, 12, 12])
        self.assertFalse(np.any(result[(labels != 0) & (labels != 2)]))

    def test_tooth_can_close_the_boundary_without_changing_its_label(self):
        for bone, tooth in ((1, 3), (2, 4)):
            labels = shell(source_label=bone)
            labels[3:5, 10, 10] = tooth
            result = self.complete(labels, bone)
            self.assertTrue(result[10, 10, 10])
            self.assertFalse(np.any(result[labels == tooth]))

    def test_open_and_diagonally_open_cavities_stay_open(self):
        for diagonal in (False, True):
            labels = shell()
            if diagonal:
                labels[3, 3, 3] = labels[4, 4, 4] = 0
            else:
                labels[3:5, 10, 10] = 0
            np.testing.assert_array_equal(self.complete(labels), labels == 2)

    def test_truncated_cavity_stays_open_at_scan_border(self):
        labels = shell()[:11]
        np.testing.assert_array_equal(self.complete(labels), labels == 2)

    def test_canal_does_not_artificially_seal_an_open_boundary(self):
        labels = shell()
        labels[:5, 10, 10] = 5
        np.testing.assert_array_equal(self.complete(labels), labels == 2)

    def test_air_and_unknown_intensities_are_not_filled(self):
        labels = shell()
        for value in (-800, np.nan, np.inf):
            intensities = np.full(labels.shape, 50, dtype=np.float32)
            intensities[10, 10, 10] = value
            np.testing.assert_array_equal(self.complete(labels, intensities=intensities), labels == 2)

    def test_large_cranial_space_is_not_filled(self):
        labels = shell((41, 41, 41), source_label=1)
        np.testing.assert_array_equal(self.complete(labels, 1, spacing=(1, 1, 1)), labels == 1)

    def test_radius_guard_uses_millimeters(self):
        labels = shell()
        self.assertTrue(self.complete(labels, spacing=(2, 0.5, 1))[10, 10, 10])
        np.testing.assert_array_equal(self.complete(labels, spacing=(2, 2, 2)), labels == 2)

    def test_large_and_small_cavities_are_handled_independently(self):
        labels = np.zeros((50, 50, 85), dtype=np.uint8)
        labels[:41, :41, :41] = shell((41, 41, 41))
        labels[:21, :21, 60:81] = shell()
        result = self.complete(labels, spacing=(1, 1, 1))
        self.assertFalse(result[20, 20, 20])
        self.assertTrue(result[10, 10, 70])

    def test_non_bone_label_is_rejected(self):
        with self.assertRaises(ValueError):
            self.complete(shell(), source_label=5)


class ThinBoneTests(unittest.TestCase):
    """The anterior maxilla came out thin and perforated (user's report, 2026-10-05): thin walls are recovered
    from the CT and pinholes sealed, but only where the CT says bone, never into air or another label."""

    def wall(self, hole_hu):
        # A wall one voxel thick at z = 10, with a 4 x 4 hole; partial-volume bone (250 HU) one voxel either side.
        shape = (21, 21, 21)
        labels = np.zeros(shape, dtype=np.uint8)
        labels[10, 2:-2, 2:-2] = 1
        labels[10, 8:12, 8:12] = 0
        intensities = np.full(shape, 30.0, dtype=np.float32)  # soft tissue
        intensities[9:12, 2:-2, 2:-2] = 250.0
        intensities[10, 8:12, 8:12] = hole_hu
        intensities[9, 8:12, 8:12] = hole_hu
        intensities[11, 8:12, 8:12] = hole_hu
        return labels, intensities

    def thicken(self, labels, intensities):
        before = labels.copy()
        result = segmentator.thicken_thin_bone(labels == 1, labels, intensities, (0.5, 0.5, 0.5))
        np.testing.assert_array_equal(labels, before)
        return result

    def test_a_thin_wall_is_thickened_where_the_ct_shows_bone(self):
        labels, intensities = self.wall(250.0)
        result = self.thicken(labels, intensities)
        self.assertTrue(result[9, 5, 5] and result[11, 5, 5], "the partial-volume wall was not recovered")
        self.assertFalse(result[8, 5, 5] or result[12, 5, 5], "the bone grew into soft tissue")

    def test_a_pinhole_in_a_wall_is_sealed(self):
        labels, intensities = self.wall(60.0)  # the hole reads as soft tissue: partial volume, not an opening
        result = self.thicken(labels, intensities)
        self.assertTrue(result[10, 9, 9] and result[10, 10, 10], "the perforation was left open")

    def test_a_three_millimetre_perforation_is_sealed(self):
        # User's case, 2026-10-05: still holes after 1.5 mm — the walls of the orbit and the maxilla have
        # perforations of a few millimetres that are not air.
        shape = (25, 25, 25)
        labels = np.zeros(shape, dtype=np.uint8)
        labels[12, 2:-2, 2:-2] = 1
        labels[12, 9:15, 9:15] = 0  # 6 voxels = 3 mm
        intensities = np.full(shape, 30.0, dtype=np.float32)
        intensities[12, 2:-2, 2:-2] = 250.0
        intensities[12, 9:15, 9:15] = 60.0
        result = self.thicken(labels, intensities)
        self.assertTrue(np.all(result[12, 9:15, 9:15]), "a 3 mm perforation was left open")

    def test_a_foramen_sized_perforation_is_sealed(self):
        # User's case, 2026-10-06 ("ya casi"): the last spots were openings of about 4-5 mm under the orbits,
        # filled with soft tissue, not air.
        shape = (29, 29, 29)
        labels = np.zeros(shape, dtype=np.uint8)
        labels[14, 2:-2, 2:-2] = 1
        labels[14, 10:19, 10:19] = 0  # 9 voxels = 4.5 mm
        intensities = np.full(shape, 30.0, dtype=np.float32)
        intensities[14, 2:-2, 2:-2] = 250.0
        intensities[14, 10:19, 10:19] = 40.0
        result = self.thicken(labels, intensities)
        self.assertTrue(np.all(result[14, 10:19, 10:19]), "a 4.5 mm opening was left open")

    def test_a_wall_at_100_hu_is_recovered(self):
        labels, intensities = self.wall(250.0)
        intensities[9, 2:-2, 2:-2] = 110.0  # thin cortex, partial volume
        result = self.thicken(labels, intensities)
        self.assertTrue(result[9, 5, 5], "a faint wall at 110 HU was not recovered")

    def maxilla_wall(self, hole_hu):
        # The anterior wall of the maxilla over the sinus: one voxel of bone, sinus air behind it, cheek in front;
        # a 6 mm hole whose voxels are a mix of the thin bone and the air (partial volume).
        shape = (31, 31, 31)
        labels = np.zeros(shape, dtype=np.uint8)
        labels[15, 2:-2, 2:-2] = 1
        labels[15, 9:21, 9:21] = 0  # 12 voxels = 6 mm
        intensities = np.full(shape, 40.0, dtype=np.float32)
        intensities[:15] = -1000.0          # the sinus
        intensities[15, 2:-2, 2:-2] = 250.0
        intensities[15, 9:21, 9:21] = hole_hu
        return labels, intensities

    def test_a_maxillary_wall_hole_over_the_sinus_is_sealed(self):
        # User's case, 2026-10-06: the last holes are in the anterior maxilla, where the wall is so thin that the
        # CT reads the gap as half air (about -450 HU), which the general rule took for an opening.
        labels, intensities = self.maxilla_wall(-450.0)
        result = segmentator.thicken_thin_bone(labels == 1, labels, intensities, (0.5, 0.5, 0.5),
                                               **segmentator.bone_thickening_params(1))
        self.assertTrue(np.all(result[15, 9:21, 9:21]), "the hole in the maxillary wall was left open")
        self.assertFalse(np.any(result[10]), "the bone grew into the sinus")

    def test_a_true_opening_of_the_maxilla_stays_open(self):
        labels, intensities = self.maxilla_wall(-950.0)
        result = segmentator.thicken_thin_bone(labels == 1, labels, intensities, (0.5, 0.5, 0.5),
                                               **segmentator.bone_thickening_params(1))
        self.assertFalse(np.any(result[15, 9:21, 9:21]), "an air opening of the maxilla was sealed")

    def test_an_orbit_sized_opening_stays_open(self):
        shape = (50, 50, 50)
        labels = np.zeros(shape, dtype=np.uint8)
        labels[25, 2:-2, 2:-2] = 1
        labels[25, 8:42, 8:42] = 0
        intensities = np.full(shape, 40.0, dtype=np.float32)
        intensities[25, 2:-2, 2:-2] = 250.0
        intensities[25, 8:42, 8:42] = 30.0
        result = segmentator.thicken_thin_bone(labels == 1, labels, intensities, (0.5, 0.5, 0.5),
                                               **segmentator.bone_thickening_params(1))
        self.assertFalse(bool(result[25, 25, 25]), "an orbit-sized opening was filled with bone")

    def test_the_mandible_keeps_its_own_rule(self):
        labels, intensities = self.maxilla_wall(-450.0)
        labels[labels == 1] = 2
        result = segmentator.thicken_thin_bone(labels == 2, labels, intensities, (0.5, 0.5, 0.5),
                                               **segmentator.bone_thickening_params(2))
        self.assertFalse(np.all(result[15, 9:21, 9:21]), "the mandible took the maxilla's stronger rule")

    def test_an_opening_into_air_stays_open(self):
        labels, intensities = self.wall(-900.0)
        result = self.thicken(labels, intensities)
        self.assertFalse(np.any(result[10, 8:12, 8:12]), "an opening to the air was sealed")

    def test_other_structures_are_never_taken(self):
        labels, intensities = self.wall(60.0)
        labels[9, 2:-2, 2:-2] = 3  # a tooth against the wall
        labels[10, 9, 9] = 5       # the canal in the hole
        result = self.thicken(labels, intensities)
        self.assertFalse(np.any(result[labels == 3]) or result[10, 9, 9], "another label was taken as bone")


def mandible_body(shape=(120, 40, 36), spacing=(0.5, 0.5, 0.5)):
    """A closed 5 x 1.6 x 1.2 cm body of cortex (1 mm) round a medulla of cancellous bone, as the network leaves a
    jaw: pores in the cortex, partial-volume voxels under -300 HU in the marrow, and a medulla far wider than the
    6 mm `fill_bone_interiors` accepts. Soft tissue round it."""
    labels = np.zeros(shape, dtype=np.uint8)
    hu = np.full(shape, 40.0, dtype=np.float32)
    outer = (slice(10, 110), slice(4, 36), slice(6, 30))
    inner = (slice(12, 108), slice(6, 34), slice(8, 28))  # 4.8 x 1.4 x 1.0 cm of marrow
    labels[outer] = 2
    hu[outer] = 1200.0
    labels[inner] = 0
    hu[inner] = 300.0
    for z in (30, 60, 90):                                 # three 2 mm pores through the cortex
        labels[z:z + 4, 4:6, 16:20] = 0
        hu[z:z + 4, 4:6, 16:20] = 40.0
    hu[50, 20, 18] = -350.0                                # one noisy voxel in the marrow
    medulla = np.zeros(shape, dtype=bool)
    medulla[inner] = True
    medulla &= hu >= -300
    outside = np.ones(shape, dtype=bool)
    outside[outer] = False
    return labels, hu, medulla, outside, spacing


class MedullaryFillTests(unittest.TestCase):
    """The medulla between the cortices is bone (user's report, 2026-10-19: "no sale el relleno en la medular,
    al hacer las osteotomías no necesito que se vea eso vacío")."""

    def fill(self, labels, hu, spacing, source_label=2, **params):
        before = labels.copy()
        mask = labels == source_label
        result = segmentator.fill_medullary_bone(mask, labels, hu, spacing, source_label, **params)
        np.testing.assert_array_equal(labels, before)
        return result, mask

    def test_a_porous_wide_medulla_is_filled(self):
        labels, hu, medulla, outside, spacing = mandible_body()
        strict = segmentator.fill_bone_interiors(labels == 2, labels, hu, spacing, 2)
        self.assertEqual(int(np.count_nonzero(strict & medulla)), 0,
                         "the fixture should defeat the closed-cavity fill, as the real jaw did")
        result, _ = self.fill(labels, hu, spacing)
        self.assertGreaterEqual(np.count_nonzero(result & medulla) / np.count_nonzero(medulla), 0.98)
        self.assertEqual(int(np.count_nonzero(result & outside)), 0, "the fill grew outside the cortex")
        self.assertFalse(result[50, 20, 18], "an air voxel became bone")

    def test_an_outer_groove_stays_open(self):
        # Two walls 4 mm apart on a floor: soft tissue in the groove has bone on two sides, but it opens to
        # the outside, so it is not marrow.
        shape, spacing = (60, 40, 40), (0.5, 0.5, 0.5)
        labels = np.zeros(shape, dtype=np.uint8)
        hu = np.full(shape, 40.0, dtype=np.float32)
        labels[:, 5:30, 8:12] = 2
        labels[:, 5:30, 20:24] = 2
        labels[:, 5:9, 8:24] = 2
        hu[labels == 2] = 1200.0
        result, mask = self.fill(labels, hu, spacing)
        self.assertEqual(int(np.count_nonzero(result & ~mask)), 0, "a groove open to the outside was filled")

    def test_a_bone_the_scan_cuts_open_stays_open(self):
        labels, hu, medulla, outside, spacing = mandible_body()
        labels, hu, medulla = labels[20:], hu[20:], medulla[20:]   # the scan starts inside the bone
        result, mask = self.fill(labels, hu, spacing)
        self.assertEqual(int(np.count_nonzero(result & ~mask)), 0, "a cavity open at the scan's edge was filled")

    def test_protected_labels_are_not_replaced(self):
        labels, hu, medulla, outside, spacing = mandible_body()
        labels[40:80, 18:20, 16:18] = 5      # the canal runs through the medulla
        result, _ = self.fill(labels, hu, spacing)
        self.assertFalse(np.any(result[40:80, 18:20, 16:18]), "the canal was taken")

    def test_the_maxilla_needs_cancellous_density_and_marrow_size(self):
        labels, hu, medulla, outside, spacing = mandible_body()
        labels[labels == 2] = 1
        hu[medulla] = 40.0                   # mucosa-like, not cancellous bone
        result, mask = self.fill(labels, hu, spacing, source_label=1)
        self.assertEqual(int(np.count_nonzero(result & ~mask)), 0, "soft tissue in the maxilla was filled")
        hu[medulla] = 300.0
        result, _ = self.fill(labels, hu, spacing, source_label=1, max_radius_mm=4.0)
        self.assertEqual(int(np.count_nonzero(result & ~mask)), 0, "a cavity wider than marrow was filled")
        result, _ = self.fill(labels, hu, spacing, source_label=1)
        self.assertGreaterEqual(np.count_nonzero(result & medulla) / np.count_nonzero(medulla), 0.98)

    def test_millimetres_not_voxels(self):
        # The same body on a grid twice as fine fills the same way.
        labels, hu, medulla, outside, spacing = mandible_body()
        fine = lambda a: np.repeat(np.repeat(np.repeat(a, 2, 0), 2, 1), 2, 2)
        result, _ = self.fill(fine(labels), fine(hu), (0.25, 0.25, 0.25))
        self.assertGreaterEqual(np.count_nonzero(result & fine(medulla)) / np.count_nonzero(fine(medulla)), 0.98)
        self.assertEqual(int(np.count_nonzero(result & fine(outside))), 0)


def coarse_sphere(block_mm=1.5, spacing=(0.4, 0.4, 0.4), shape=(60, 60, 60), radius=8.0):
    z, y, x = np.indices(shape).astype(np.float32)
    centre = [0.5 * (n - 1) for n in shape]
    true = ((z - centre[0]) * spacing[0]) ** 2 + ((y - centre[1]) * spacing[1]) ** 2 + \
           ((x - centre[2]) * spacing[2]) ** 2 <= radius ** 2

    def snap(i, axis):
        v = (i - centre[axis]) * spacing[axis]
        return (np.floor(v / block_mm) + 0.5) * block_mm

    coarse = snap(z, 0) ** 2 + snap(y, 1) ** 2 + snap(x, 2) ** 2 <= radius ** 2
    hu = np.where(true, 1000.0, 40.0).astype(np.float32)
    return true, coarse, hu, spacing


def cortical_box(source_label=2, interior_hu=300.0, foramina=True, shape=(28, 28, 40)):
    """A cortical box 2 voxels thick at 0.5 mm, marrow inside; optionally an 8-voxel (4 mm) foramen in two
    opposite faces, wider than the 1.5 mm closing seals, so the marrow reaches the outside in 3D."""
    labels = np.zeros(shape, dtype=np.uint8)
    labels[4:-4, 4:-4, 4:-4] = source_label
    labels[6:-6, 6:-6, 6:-6] = 0
    intensities = np.full(shape, -50.0, dtype=np.float32)
    intensities[4:-4, 4:-4, 4:-4] = 1200.0
    intensities[6:-6, 6:-6, 6:-6] = interior_hu
    if foramina:
        mid = shape[0] // 2
        labels[mid - 4:mid + 4, mid - 4:mid + 4, 4:6] = 0
        labels[mid - 4:mid + 4, mid - 4:mid + 4, -6:-4] = 0
        intensities[mid - 4:mid + 4, mid - 4:mid + 4, 4:6] = -50.0
        intensities[mid - 4:mid + 4, mid - 4:mid + 4, -6:-4] = -50.0
    return labels, intensities


class SolidBoneTests(unittest.TestCase):
    """User's request, 2026-10-19: the jaws came out hollow on osteotomy. The bone is the network's mask closed,
    filled in 3D and in 2 of 3 slice planes, with no lower HU limit but air."""

    spacing = (0.5, 0.5, 0.5)

    def solid(self, labels, intensities, source_label=2, spacing=None, **kwargs):
        before = labels.copy()
        result = segmentator.solid_bone_mask(labels == source_label, labels, intensities, spacing or self.spacing,
                                             source_label, **kwargs)
        np.testing.assert_array_equal(labels, before)
        return result

    def test_marrow_open_through_foramina_is_filled(self):
        labels, intensities = cortical_box()
        result = self.solid(labels, intensities)
        marrow = np.zeros(labels.shape, dtype=bool)
        marrow[6:-6, 6:-6, 6:-6] = True
        marrow[10:18, 10:18, :] = False  # in line with both foramina: a canal, open in two of the three planes
        self.assertTrue(np.all(result[marrow]), "the marrow was left hollow")
        self.assertFalse(result[1, 1, 20], "the bone grew outside the cortex")

    def test_fatty_marrow_below_zero_hu_is_bone(self):
        for source_label in (1, 2):
            with self.subTest(source_label=source_label):
                labels, intensities = cortical_box(source_label, interior_hu=-150.0)
                result = self.solid(labels, intensities, source_label)
                self.assertTrue(result[8, 8, 20], "fatty marrow under 0 HU was left out")

    def test_air_stays_air(self):
        labels, intensities = cortical_box(1, interior_hu=-900.0)
        self.assertFalse(np.any(self.solid(labels, intensities, 1)[7:-7, 7:-7, 8:-8]))

    def test_a_cavity_wider_than_marrow_stays_open(self):
        # The cranial cavity: brain at 30 HU closed in by the skull, 20 mm from its centre to the wall at 1 mm
        # voxels (a real one is about 60 mm): wider than marrow, and most directions travel further than 15 mm.
        labels, intensities = cortical_box(1, interior_hu=30.0, foramina=False, shape=(52, 52, 52))
        result = self.solid(labels, intensities, 1, spacing=(1.0, 1.0, 1.0))
        self.assertFalse(np.any(result[12:-12, 12:-12, 12:-12]), "the cranial cavity became bone")

    def test_the_nose_is_not_filled(self):
        # A nasal meatus: an air tube along y between bony walls, lined by 1 mm of mucosa at 40 HU. The meatus is
        # closed in on most sides, but its mucosa lines air and the air is never bone.
        labels = np.zeros((30, 40, 30), dtype=np.uint8)
        labels[4:26, :, 4:26] = 1
        labels[8:22, :, 8:22] = 0
        intensities = np.full(labels.shape, 1200.0, dtype=np.float32)
        intensities[8:22, :, 8:22] = 40.0         # mucosa
        intensities[10:20, :, 10:20] = -1000.0    # air
        result = self.solid(labels, intensities, 1)
        self.assertFalse(np.any(result[8:22, 5:35, 8:22]), "the nasal cavity or its mucosa became bone")

    def test_an_outer_groove_stays_open(self):
        labels = np.zeros((30, 30, 30), dtype=np.uint8)
        labels[4:26, 4:26, 4:26] = 2
        labels[10:20, 10:20, 18:26] = 0  # a 5 mm notch cut in from one face
        intensities = np.full(labels.shape, 300.0, dtype=np.float32)
        result = self.solid(labels, intensities)
        self.assertFalse(np.any(result[12:18, 12:18, 20:26]))

    def test_other_labels_keep_their_voxels(self):
        labels, intensities = cortical_box()
        labels[12:16, 12:16, 6:-6] = 5   # the canal
        labels[8, 8, 10] = 4             # a tooth
        result = self.solid(labels, intensities)
        self.assertFalse(np.any(result[labels == 5]))
        self.assertFalse(result[8, 8, 10])
        self.assertTrue(result[20, 20, 20])


class BoneDiagnosticsTests(unittest.TestCase):
    def test_the_marrow_is_counted_and_the_parts_add_up(self):
        labels, intensities = cortical_box()
        solid = segmentator.solid_bone_mask(labels == 2, labels, intensities, (0.5, 0.5, 0.5), 2)
        cortical, medullary, air = segmentator.split_cortical_medullary(solid, intensities)
        self.assertGreater(int(medullary.sum()), 0, "no marrow in the bone")
        self.assertFalse(np.any(cortical & medullary))
        np.testing.assert_array_equal(cortical | medullary | air, solid)

    def test_a_hollow_shell_reports_almost_no_marrow(self):
        labels, intensities = cortical_box(interior_hu=-900.0, foramina=False)
        _, medullary, _ = segmentator.split_cortical_medullary(labels == 2, intensities)
        self.assertEqual(int(medullary.sum()), 0)

    def test_the_bone_mesh_is_watertight_and_a_plane_cut_is_capped(self):
        import trimesh
        labels, intensities = cortical_box()
        solid = segmentator.solid_bone_mask(labels == 2, labels, intensities, (0.5, 0.5, 0.5), 2)
        mesh = segmentator.bone_surface_mesh(solid, (0.5, 0.5, 0.5))
        self.assertTrue(mesh.is_watertight, "the bone surface is open")
        self.assertEqual(len(trimesh.graph.connected_components(mesh.face_adjacency,
                                                                nodes=np.arange(len(mesh.faces)))), 1)
        centre = mesh.bounds.mean(axis=0)
        half = mesh.slice_plane(centre, [1.0, 0.0, 0.0], cap=True)
        self.assertTrue(half.is_watertight, "the cut left the section open")
        self.assertGreater(abs(half.volume), 0.3 * abs(mesh.volume), "the cut half is hollow")

    def test_loose_fragments_are_dropped_and_the_bone_kept(self):
        mask = np.zeros((40, 40, 40), dtype=bool)
        mask[5:35, 5:35, 5:20] = True     # the bone, 3375 mm3 at 0.5 mm
        mask[30:33, 30:33, 30:33] = True  # a speck of 3.4 mm3
        mask[2:12, 25:38, 28:38] = True   # a real separate piece of 162 mm3
        result = segmentator.drop_bone_islands(mask, (0.5, 0.5, 0.5), 2)
        self.assertFalse(np.any(result[30:33, 30:33, 30:33]))
        self.assertTrue(np.all(result[5:35, 5:35, 5:20]))
        self.assertTrue(np.all(result[2:12, 25:38, 28:38]))

    def test_anisotropic_spacing_is_flagged(self):
        with patch.object(segmentator, "log") as log:
            ratio = segmentator.log_volume_spacing((0.377, 0.377, 0.7))
        self.assertGreater(ratio, 1.5)
        self.assertTrue(any("AVISO" in str(call) for call in log.call_args_list))


class CoarseMaskRefinementTests(unittest.TestCase):
    """A bone mask predicted on a 1.5 mm grid (TotalSegmentator) and brought back on the CT's grid is made of
    blocks, and the mesh shows them as terraces (user's report, 2026-10-19: "sale muy rugosa"). Near its
    boundary the bone is re-read from the CT at full resolution."""

    def test_the_blocks_follow_the_bone_again(self):
        true, coarse, hu, spacing = coarse_sphere()
        labels = np.zeros(true.shape, dtype=np.uint8)
        refined = segmentator.refine_coarse_bone_mask(coarse, labels, hu, spacing)
        before = np.count_nonzero(coarse ^ true)
        after = np.count_nonzero(refined ^ true)
        self.assertLess(after, 0.1 * before, f"{after} voxels still off the bone (were {before})")

    def test_only_near_the_boundary(self):
        true, coarse, hu, spacing = coarse_sphere()
        hu[:, :, :6] = 1000.0                # other dense bone 4 mm off the sphere, not part of it
        labels = np.zeros(true.shape, dtype=np.uint8)
        refined = segmentator.refine_coarse_bone_mask(coarse, labels, hu, spacing)
        self.assertFalse(np.any(refined[:, :, :6]), "the refinement reached bone far from the mask")

    def test_the_medulla_behind_the_cortex_is_kept(self):
        true, coarse, hu, spacing = coarse_sphere(radius=9.0)
        z, y, x = np.indices(true.shape)
        c = 29.5
        inner = ((z - c) ** 2 + (y - c) ** 2 + (x - c) ** 2) * spacing[0] ** 2 <= 7.0 ** 2
        hu[inner] = 20.0                     # fatty marrow, low density, behind a 2 mm cortex
        labels = np.zeros(true.shape, dtype=np.uint8)
        refined = segmentator.refine_coarse_bone_mask(coarse, labels, hu, spacing)
        self.assertTrue(np.all(refined[inner & coarse]), "the refinement carved the medulla")

    def test_other_labels_are_untouched(self):
        true, coarse, hu, spacing = coarse_sphere()
        labels = np.zeros(true.shape, dtype=np.uint8)
        labels[true & ~coarse] = 3           # teeth where the blocks fell short
        refined = segmentator.refine_coarse_bone_mask(coarse, labels, hu, spacing)
        self.assertFalse(np.any(refined & (labels == 3)), "a tooth became bone")


class BoneRemapTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.env = patch.dict(os.environ, {
            "DENTALSEGMENTATOR_BONE_OPEN_RADIUS": "0",
            "DENTALSEGMENTATOR_BONE_CLOSE_RADIUS": "0",
            "DENTALSEGMENTATOR_TEETH_OPEN_RADIUS": "0",
            "DENTALSEGMENTATOR_TEETH_CLOSE_RADIUS": "0",
        })
        self.env.start()
        self.addCleanup(self.env.stop)

    def remap(self, labels, target, shifted_prediction=False, bone_from_totalsegmentator=False, intensities=None):
        if intensities is None:
            intensities = np.full(labels.shape, 50, dtype=np.int16)
        image = sitk.GetImageFromArray(intensities.astype(np.int16))
        image.SetSpacing((0.4, 0.7, 1.1))
        image.SetOrigin((12.5, -37, 80))
        image.SetDirection((0, -1, 0, 1, 0, 0, 0, 0, 1))
        prediction = sitk.GetImageFromArray(labels)
        prediction.CopyInformation(image)
        if shifted_prediction:
            prediction.SetOrigin((13.5, -37, 80))
        input_path = self.root / "input.nrrd"
        prediction_path = self.root / "prediction.nii.gz"
        output_path = self.root / "output.nrrd"
        sitk.WriteImage(image, str(input_path))
        sitk.WriteImage(prediction, str(prediction_path))
        segmentator.remap_prediction(input_path, prediction_path, output_path, target,
                                     bone_from_totalsegmentator=bone_from_totalsegmentator)
        output = sitk.ReadImage(str(output_path))
        np.testing.assert_allclose(output.GetSpacing(), image.GetSpacing(), atol=1e-6)
        np.testing.assert_allclose(output.GetOrigin(), image.GetOrigin(), atol=1e-6)
        np.testing.assert_allclose(output.GetDirection(), image.GetDirection(), atol=1e-6)
        return sitk.GetArrayFromImage(output)

    def test_a_hole_inside_a_tooth_is_filled(self):
        labels = np.zeros((16, 16, 16), dtype=np.uint8)
        labels[4:12, 4:12, 4:12] = 3
        labels[7:9, 7:9, 7:9] = 0
        result = self.remap(labels, "dientes")
        self.assertTrue(np.all(result[4:12, 4:12, 4:12] == 5), "the hole inside the tooth was left empty")
        self.assertEqual(result[2, 2, 2], 0, "filling the tooth grew into the space around it")

    def test_individual_bones_are_completed_even_without_smoothing(self):
        for source, target, app_label in ((1, "maxilar", 5), (2, "mandibula", 6)):
            result = self.remap(shell(source_label=source), target)
            self.assertEqual(result[10, 10, 10], app_label)
            self.assertEqual(result[0, 0, 0], 0)

    def test_full_automatic_fill_belongs_to_bone_not_soft_tissue(self):
        labels = np.zeros((26, 26, 48), dtype=np.uint8)
        labels[2:23, 2:23, 2:23] = shell(source_label=1)
        labels[2:23, 2:23, 25:46] = shell(source_label=2)
        labels[12, 12, 35] = 5
        labels[8, 8, 8] = 3
        labels[8, 8, 31] = 4
        for closing in ("0", "1"):
            with self.subTest(closing=closing), patch.dict(os.environ, {
                    "DENTALSEGMENTATOR_BONE_CLOSE_RADIUS": closing}):
                result = self.remap(labels, "completo")
                self.assertEqual(result[12, 12, 12], 5)
                self.assertEqual(result[13, 13, 35], 6)
                self.assertEqual(result[12, 12, 35], 6)
                self.assertEqual(result[8, 8, 8], 5)
                self.assertEqual(result[8, 8, 31], 6)
                self.assertTrue(np.any(result == 2))
                self.assertEqual(np.count_nonzero(result == 7), 0)

    def test_teeth_only_does_not_fill_bone(self):
        labels = shell()
        labels[8, 8, 8] = 4
        result = self.remap(labels, "dientes")
        self.assertEqual(result[8, 8, 8], 6)
        self.assertEqual(np.count_nonzero(result), 1)

    def test_bone_target_includes_both_completed_jaws(self):
        labels = np.zeros((21, 21, 44), dtype=np.uint8)
        labels[:, :, :21] = shell(source_label=1)
        labels[:, :, 23:] = shell(source_label=2)
        result = self.remap(labels, "hueso")
        self.assertEqual(result[10, 10, 10], 5)
        self.assertEqual(result[10, 10, 33], 6)
        self.assertFalse(np.any(result == 2))

    def test_the_mental_foramen_does_not_open_a_hole_in_the_mandible(self):
        # User's case, 2026-10-06: two holes either side of the chin, where the mandibular canal leaves the
        # bone. The canal stays inside the mandible; its exit no longer opens the bone's surface.
        labels = np.zeros((32, 32, 32), dtype=np.uint8)
        labels[5:27, 5:27, 5:27] = 2
        labels[15:18, 15:18, 8:24] = 5   # the canal, along x inside the bone
        labels[18:27, 13:20, 13:20] = 5  # its exit through the cortex, 7 voxels wide (the orange spots)
        result = self.remap(labels, "hueso")
        self.assertTrue(np.all(result[26, 13:20, 13:20] == 6), "the foramen still opens the bone's surface")
        self.assertTrue(np.all(result[16, 16, 10:20] == 6), "the canal was left as its own structure")
        self.assertEqual(np.count_nonzero(result == 7), 0)

    def totalsegmentator_shell(self, source_label, interior_hu):
        labels = shell(source_label=source_label)
        intensities = np.full(labels.shape, -50, dtype=np.int16)
        intensities[3:-3, 3:-3, 3:-3] = 1200        # cortex
        intensities[5:-5, 5:-5, 5:-5] = interior_hu  # what the cortex encloses
        return labels, intensities

    def test_totalsegmentator_medulla_is_filled(self):
        # User's report, 2026-10-19: once TotalSegmentator gave the bone, every bone step was skipped and the
        # medulla came out hollow, so an osteotomy showed an empty shell. Cancellous bone inside the cortex is bone.
        for source, app_label in ((1, 5), (2, 6)):
            with self.subTest(source=source):
                labels, intensities = self.totalsegmentator_shell(source, 300)
                result = self.remap(labels, "hueso", bone_from_totalsegmentator=True, intensities=intensities)
                self.assertTrue(np.all(result[5:-5, 5:-5, 5:-5] == app_label), "the medulla was left hollow")
                self.assertEqual(result[3, 10, 10], app_label)
                self.assertEqual(result[0, 0, 0], 0)

    def test_totalsegmentator_air_inside_the_bone_stays_open(self):
        # A sinus is air the bone encloses: filling it would turn the maxilla into a solid block.
        labels, intensities = self.totalsegmentator_shell(1, -900)
        result = self.remap(labels, "hueso", bone_from_totalsegmentator=True, intensities=intensities)
        self.assertEqual(result[10, 10, 10], 0)
        self.assertEqual(result[3, 10, 10], 5)

    def test_mismatched_physical_grids_fail_without_writing_output(self):
        with self.assertRaisesRegex(ValueError, "cuadricula fisica"):
            self.remap(shell(), "completo", shifted_prediction=True)
        self.assertFalse((self.root / "output.nrrd").exists())


class OfflineWeightsTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    @staticmethod
    def dataset(base):
        folder = base / "Dataset111_453CT" / "nnUNetTrainer__nnUNetPlans__3d_fullres"
        folder.mkdir(parents=True)
        (folder / "dataset.json").write_text("{}")
        return base / "Dataset111_453CT"

    def test_script_has_no_download_code(self):
        source = SCRIPT.read_text(encoding="utf-8")
        for token in ("urllib", "urlretrieve", "requests", "http.client", "socket"):
            self.assertNotIn(token, source)

    def test_uses_weights_installed_by_slicer(self):
        slicer = self.root / "slicer" / "Resources" / "ML"
        expected = self.dataset(slicer)
        app = self.root / "app"
        with patch("socket.socket", side_effect=AssertionError("network used")):
            found = segmentator.ensure_weights(app, [app, slicer])
        self.assertEqual(found, expected)
        self.assertFalse(app.exists())

    def test_extracts_a_local_zip(self):
        app = self.root / "app"
        app.mkdir()
        with zipfile.ZipFile(app / segmentator.MODEL_ZIP_NAME, "w") as archive:
            archive.writestr("Dataset111_453CT/nnUNetTrainer__nnUNetPlans__3d_fullres/dataset.json", "{}")
        with patch("socket.socket", side_effect=AssertionError("network used")):
            found = segmentator.ensure_weights(app, [app])
        self.assertEqual(found, app / "Dataset111_453CT")

    def test_missing_weights_fail_without_network(self):
        app = self.root / "app"
        with patch("socket.socket", side_effect=AssertionError("network used")):
            with self.assertRaisesRegex(RuntimeError, "no los descarga"):
                segmentator.ensure_weights(app, [app])


def process_alive(pid):
    import ctypes
    kernel32 = ctypes.WinDLL("kernel32")
    handle = kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        return False
    code = ctypes.c_ulong()
    try:
        return kernel32.GetExitCodeProcess(handle, ctypes.byref(code)) and code.value == 259  # STILL_ACTIVE
    finally:
        kernel32.CloseHandle(handle)


class ProgressAndCleanupTests(unittest.TestCase):
    def test_tqdm_bar_becomes_app_progress(self):
        child = (
            "import sys; "
            "sys.stdout.write('  1%|| 4/448 [00:01<02:55,  2.53it/s]\\r 50%|| 224/448 [01:30<01:30,  2.5it/s]\\r"
            "100%|| 448/448 [03:00<00:00,  2.5it/s]\\ndone with case\\n'); sys.stdout.flush()"
        )
        lines = []
        with patch.object(segmentator, "log", side_effect=lines.append):
            segmentator.run_logged([sys.executable, "-c", child], progress_range=(45, 88), stage="Prediciendo")
        updates = [json.loads(line[len("MAXILLO_PROGRESS "):]) for line in lines if line.startswith("MAXILLO_PROGRESS ")]
        self.assertEqual([u["percent"] for u in updates], [45, 66, 88])
        self.assertEqual(updates[1]["stage"], "Prediciendo: 224/448, faltan 01:30")
        self.assertIn("done with case", lines)
        self.assertFalse(any("it/s" in line for line in lines[1:]))  # lines[0] echoes the command

    @unittest.skipUnless(os.name == "nt", "Windows job object")
    def test_failed_run_leaves_no_worker_processes(self):
        child = (
            "import subprocess, sys, time; "
            "worker = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)']); "
            "print(worker.pid, flush=True); time.sleep(0.5); sys.exit(3)"
        )
        lines = []
        started = time.time()
        with patch.object(segmentator, "log", side_effect=lines.append):
            with self.assertRaisesRegex(RuntimeError, "codigo 3"):
                segmentator.run_logged([sys.executable, "-c", child])
        # The worker keeps the output pipe open: the failed run must not wait for it to finish.
        self.assertLess(time.time() - started, 20.0)
        worker = int(next(line for line in lines if line.strip().isdigit()))
        deadline = time.time() + 5.0
        while process_alive(worker) and time.time() < deadline:
            time.sleep(0.1)
        self.assertFalse(process_alive(worker), "the nnU-Net worker outlived a failed run")


class ProcessOutputTests(unittest.TestCase):
    def test_tqdm_bytes_do_not_abort_logging(self):
        # nnU-Net progress bars write bytes such as 0x8f that cp1252 cannot decode.
        child = (
            "import sys; "
            "sys.stdout.buffer.write(b'  1%|\\xe2\\x96\\x8f | 4/448\\n  2%|\\x8f|\\n'); "
            "sys.stdout.flush()"
        )
        lines = []
        with patch.object(segmentator, "log", side_effect=lines.append):
            segmentator.run_logged([sys.executable, "-c", child])
        self.assertTrue(any("4/448" in line for line in lines))
        self.assertTrue(any("2%" in line for line in lines))


class TotalSegmentatorBoneTests(unittest.TestCase):
    def test_skull_and_mandible_replace_the_dental_shell(self):
        dental = np.zeros((6, 6, 6), dtype=np.uint8)
        dental[1:5, 1:5, 1] = 1
        dental[1:5, 1:5, 4] = 2
        total = np.zeros_like(dental)
        total[1:5, 1:5, 1:3] = segmentator.TS_SKULL
        total[1:5, 1:5, 3:5] = segmentator.TS_MANDIBLE
        out = segmentator.replace_bone_with_totalsegmentator(dental, total)
        self.assertTrue(np.all(out[1:5, 1:5, 1:3] == 1))
        self.assertTrue(np.all(out[1:5, 1:5, 3:5] == 2))
        self.assertEqual(out[0, 0, 0], 0)

    def test_teeth_stay_and_the_canal_becomes_bone(self):
        dental = np.zeros((5, 5, 5), dtype=np.uint8)
        dental[2, 2, 2] = 3
        dental[2, 2, 3] = 4
        dental[2, 2, 1] = 5
        total = np.full((5, 5, 5), segmentator.TS_SKULL, dtype=np.uint8)
        total[2, 2, 3] = segmentator.TS_MANDIBLE
        out = segmentator.replace_bone_with_totalsegmentator(dental, total)
        self.assertEqual(out[2, 2, 2], 3)
        self.assertEqual(out[2, 2, 3], 4)
        self.assertEqual(out[2, 2, 1], 1)  # canal covered by skull becomes skull, not a canal
        self.assertEqual(out[0, 0, 0], 1)
        uncovered = np.zeros((3, 3, 3), dtype=np.uint8)
        uncovered[1, 1, 1] = 5
        filled = segmentator.replace_bone_with_totalsegmentator(uncovered, np.zeros_like(uncovered))
        self.assertEqual(filled[1, 1, 1], 2)

    def test_sinus_head_and_totalsegmentator_teeth_are_not_bone(self):
        dental = np.ones((4, 4, 4), dtype=np.uint8)
        total = np.zeros_like(dental)
        total[1, 1, 1] = 5  # maxillary sinus
        total[2, 2, 2] = 7  # teeth_upper
        total[3, 3, 3] = 4  # head
        out = segmentator.replace_bone_with_totalsegmentator(dental, total)
        self.assertEqual(out[1, 1, 1], 0)
        self.assertEqual(out[2, 2, 2], 0)
        self.assertEqual(out[3, 3, 3], 0)

    def test_mandible_wins_where_the_two_bones_overlap(self):
        dental = np.zeros((2, 2, 2), dtype=np.uint8)
        total = np.full((2, 2, 2), segmentator.TS_SKULL, dtype=np.uint8)
        total[0, 0, 0] = segmentator.TS_MANDIBLE
        out = segmentator.replace_bone_with_totalsegmentator(dental, total)
        self.assertEqual(out[0, 0, 0], 2)
        self.assertEqual(out[1, 1, 1], 1)

    def test_mismatched_shapes_fail(self):
        with self.assertRaisesRegex(ValueError, "cuadricula"):
            segmentator.replace_bone_with_totalsegmentator(
                np.zeros((2, 2, 2), np.uint8), np.zeros((3, 3, 3), np.uint8))

    def test_resample_keeps_labels_on_the_same_grid(self):
        reference = sitk.GetImageFromArray(np.zeros((4, 4, 4), np.uint8))
        reference.SetSpacing((1.0, 1.0, 1.0))
        moving = sitk.GetImageFromArray(np.full((4, 4, 4), segmentator.TS_SKULL, np.uint8))
        moving.CopyInformation(reference)
        np.testing.assert_array_equal(
            segmentator.resample_label_array(moving, reference),
            np.full((4, 4, 4), segmentator.TS_SKULL))


if __name__ == "__main__":
    unittest.main()


class UpperTeethSidecarTests(unittest.TestCase):
    """spec asistente-guia-lefort: the upper teeth come out on their own, the labelmap untouched."""

    def test_the_sidecar_holds_exactly_the_upper_teeth(self):
        labels = np.zeros((6, 7, 8), dtype=np.uint8)
        labels[1:3, 2:4, 2:5] = 1
        labels[3:5, 2:4, 2:5] = 1
        teeth = np.zeros_like(labels, dtype=bool)
        teeth[3:5, 2:4, 2:5] = True
        reference = sitk.GetImageFromArray(labels)
        reference.SetSpacing((0.4, 0.4, 0.7))
        reference.SetOrigin((-10.0, 5.0, 2.0))
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / "output_segmentation.nrrd"
            path = segmentator.write_upper_teeth_sidecar(teeth, reference, output)
            self.assertEqual(path, Path(folder) / "output_segmentation_dientes_superiores.nrrd")
            written = sitk.ReadImage(str(path))
            np.testing.assert_array_equal(sitk.GetArrayFromImage(written), teeth.astype(np.uint8))
            self.assertEqual(written.GetSpacing(), reference.GetSpacing())
            self.assertEqual(written.GetOrigin(), reference.GetOrigin())

    def test_no_teeth_writes_nothing(self):
        reference = sitk.GetImageFromArray(np.zeros((3, 3, 3), dtype=np.uint8))
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / "out.nrrd"
            self.assertIsNone(segmentator.write_upper_teeth_sidecar(np.zeros((3, 3, 3), bool), reference, output))
            self.assertFalse((Path(folder) / "out_dientes_superiores.nrrd").exists())

    def test_the_sidecar_holds_exactly_the_lower_teeth(self):
        labels = np.zeros((6, 7, 8), dtype=np.uint8)
        labels[1:3, 2:4, 2:5] = 2
        teeth = np.zeros_like(labels, dtype=bool)
        teeth[1:3, 2:4, 2:5] = True
        reference = sitk.GetImageFromArray(labels)
        reference.SetSpacing((0.4, 0.4, 0.7))
        reference.SetOrigin((-10.0, 5.0, 2.0))
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / "output_segmentation.nrrd"
            path = segmentator.write_lower_teeth_sidecar(teeth, reference, output)
            self.assertEqual(path, Path(folder) / "output_segmentation_dientes_inferiores.nrrd")
            written = sitk.ReadImage(str(path))
            np.testing.assert_array_equal(sitk.GetArrayFromImage(written), teeth.astype(np.uint8))
            self.assertEqual(written.GetSpacing(), reference.GetSpacing())
            self.assertFalse((Path(folder) / "output_segmentation_dientes_superiores.nrrd").exists())
