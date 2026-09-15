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
