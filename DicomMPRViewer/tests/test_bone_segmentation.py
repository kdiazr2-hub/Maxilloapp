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

    def remap(self, labels, target, shifted_prediction=False):
        image = sitk.GetImageFromArray(np.full(labels.shape, 50, dtype=np.int16))
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
        segmentator.remap_prediction(input_path, prediction_path, output_path, target)
        output = sitk.ReadImage(str(output_path))
        np.testing.assert_allclose(output.GetSpacing(), image.GetSpacing(), atol=1e-6)
        np.testing.assert_allclose(output.GetOrigin(), image.GetOrigin(), atol=1e-6)
        np.testing.assert_allclose(output.GetDirection(), image.GetDirection(), atol=1e-6)
        return sitk.GetArrayFromImage(output)

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
                self.assertEqual(result[12, 12, 35], 7)
                self.assertEqual(result[8, 8, 8], 5)
                self.assertEqual(result[8, 8, 31], 6)
                self.assertTrue(np.any(result == 2))
                self.assertEqual(np.count_nonzero(result == 7), 1)

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
        self.assertTrue(np.all(result[16, 16, 10:20] == 7), "the canal inside the mandible was lost")

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
