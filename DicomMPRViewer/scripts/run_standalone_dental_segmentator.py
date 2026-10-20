import argparse
import json
import os
import re
import shutil
import shlex
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import zipfile
from pathlib import Path

try:
    import SimpleITK as sitk
    import numpy as np
except ImportError:
    sitk = None
    np = None

os.environ.setdefault("KMP_DUPLICATE_LIB_OK", "TRUE")

# Official DentalSegmentator weights; installed locally, never downloaded by the app.
MODEL_ZIP_NAME = "Dataset111_453CT_v100.zip"
DATASET_ID = "111"
CONFIGURATION = "3d_fullres"
FOLDS = "0"

# DentalSegmentator model labels:
#   1 Maxilla & Upper Skull, 2 Mandible, 3 Upper Teeth, 4 Lower Teeth, 5 Mandibular canal
# Bone (1 and 2) comes from DentalSegmentator (user's decision, 2026-10-19: TotalSegmentator's skull looked wrong
# from below). DENTALSEGMENTATOR_BONE_SOURCE=totalsegmentator replaces it with TotalSegmentator, which then runs
# first. The canal (5) is not a result: those voxels become mandible.
# App labels:
#   5 Maxilar + dientes superiores, 6 Mandibula + dientes inferiores, 2 Tejido blando
APP_LABEL_MAP = {1: 5, 2: 6, 3: 5, 4: 6, 5: 6}

# craniofacial_structures class ids (TotalSegmentator map_to_binary):
#   1 mandible, 2 teeth_lower, 3 skull, 4 head, 5 sinus_maxillary, 6 sinus_frontal, 7 teeth_upper
TS_SKULL = 3
TS_MANDIBLE = 1
TS_SINUS = (5, 6)


def log(message):
    print(message, flush=True)


# The app reads this script's output as UTF-8; unmappable characters must not abort the run.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass


def default_model_dir():
    configured = os.environ.get("DENTALSEGMENTATOR_MODEL_DIR", "").strip()
    if configured:
        return Path(configured).expanduser().resolve()
    local = os.environ.get("LOCALAPPDATA", "")
    if local:
        return Path(local) / "DicomMPRViewer" / "DentalSegmentator" / "ML"
    return Path.home() / ".dicom_mpr_viewer" / "DentalSegmentator" / "ML"


def require_imports():
    missing = []
    try:
        import SimpleITK  # noqa: F401
    except Exception:
        missing.append("SimpleITK")
    try:
        import numpy  # noqa: F401
    except Exception:
        missing.append("numpy")
    try:
        import nnunetv2  # noqa: F401
    except Exception:
        missing.append("nnunetv2")
    if missing:
        raise RuntimeError(
            "Faltan dependencias DentalSegmentator standalone: "
            + ", ".join(missing)
            + ". Instale en el Python configurado: "
              "pip install SimpleITK numpy torch nnunetv2"
        )


def find_dataset_dir(model_dir):
    model_dir = Path(model_dir)
    for dataset_json in model_dir.rglob("dataset.json"):
        for parent in [dataset_json.parent, *dataset_json.parents]:
            if parent == model_dir.parent:
                break
            if parent.name.startswith("Dataset111"):
                return parent
    return None


def model_search_dirs(model_dir):
    """Local folders only: the app never downloads the model."""
    dirs = [Path(model_dir), Path(__file__).resolve().parent / "models" / "DentalSegmentator"]
    local = os.environ.get("LOCALAPPDATA", "")
    if local:
        slicer_root = Path(local) / "slicer.org"
        if slicer_root.is_dir():
            # Weights already installed by the 3D Slicer DentalSegmentator extension.
            dirs.extend(sorted(
                slicer_root.glob("*/slicer.org/Extensions-*/DentalSegmentator/lib/*/qt-scripted-modules/Resources/ML"),
                reverse=True,
            ))
    unique = []
    for folder in dirs:
        if folder not in unique:
            unique.append(folder)
    return unique


def ensure_weights(model_dir, search_dirs=None):
    model_dir = Path(model_dir)
    candidates = [Path(d) for d in (search_dirs if search_dirs is not None else model_search_dirs(model_dir))]
    for folder in candidates:
        if folder.is_dir():
            dataset_dir = find_dataset_dir(folder)
            if dataset_dir:
                log(f"Pesos DentalSegmentator locales: {dataset_dir}")
                return dataset_dir

    for folder in candidates:
        zip_path = folder / MODEL_ZIP_NAME
        if zip_path.is_file():
            log(f"25% Descomprimiendo pesos DentalSegmentator locales: {zip_path}")
            model_dir.mkdir(parents=True, exist_ok=True)
            with zipfile.ZipFile(zip_path, "r") as zf:
                zf.extractall(model_dir)
            dataset_dir = find_dataset_dir(model_dir)
            if dataset_dir:
                return dataset_dir

    raise RuntimeError(
        "No se encontraron los pesos de DentalSegmentator en este equipo y la app no los descarga.\n"
        f"Copie {MODEL_ZIP_NAME} (o la carpeta Dataset111_453CT descomprimida) en:\n  {model_dir}\n"
        "El archivo es el de la release oficial de SlicerDentalSegmentator o el de Zenodo "
        "(doi 10.5281/zenodo.10829674)."
    )


def nnunet_predict_executable():
    scripts_dir = Path(sys.executable).resolve().parent
    prefix_dir = Path(sys.prefix).resolve()
    candidates = [
        scripts_dir / "nnUNetv2_predict.exe",
        scripts_dir / "nnUNetv2_predict",
        scripts_dir / "Scripts" / "nnUNetv2_predict.exe",
        scripts_dir / "Scripts" / "nnUNetv2_predict",
        prefix_dir / "Scripts" / "nnUNetv2_predict.exe",
        prefix_dir / "Scripts" / "nnUNetv2_predict",
        prefix_dir / "bin" / "nnUNetv2_predict.exe",
        prefix_dir / "bin" / "nnUNetv2_predict",
        scripts_dir.parent / "Scripts" / "nnUNetv2_predict.exe",
        scripts_dir.parent / "Scripts" / "nnUNetv2_predict",
    ]
    for candidate in candidates:
        if candidate.exists():
            return str(candidate)

    exe = shutil.which("nnUNetv2_predict")
    if exe:
        return exe

    log("No se encontro nnUNetv2_predict en estas rutas:")
    for candidate in candidates:
        log(f"  - {candidate}")
    raise RuntimeError(
        "No se encontro nnUNetv2_predict. Verifique que nnunetv2 este instalado "
        "en el Python seleccionado."
    )


def child_environment(env=None):
    """Child Python tools (nnU-Net, tqdm) write UTF-8 so the pipe decodes the same way."""
    child = dict(os.environ if env is None else env)
    child["PYTHONIOENCODING"] = "utf-8"
    child["PYTHONUNBUFFERED"] = "1"
    return child


def open_logged_process(command, env=None):
    # Never decode with the Windows ANSI code page: tqdm bars contain bytes
    # such as 0x8f that cp1252 cannot map (UnicodeDecodeError).
    return subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=child_environment(env),
    )


# tqdm bar: " 12%|█▏  | 54/448 [00:21<02:35,  2.53it/s]"
TQDM_PROGRESS = re.compile(r"(\d+)/(\d+)\s*\[(?:[^<\]]*<([0-9:]+))?")


def progress(percent, stage):
    """Progress line parsed by the app (SegmentationProgressCore)."""
    log("MAXILLO_PROGRESS " + json.dumps({"percent": int(percent), "stage": stage}, ensure_ascii=False))


def attach_kill_on_close_job(process):
    """Windows: nnU-Net and its worker processes die with this script, even if it is killed."""
    if os.name != "nt":
        return None
    try:
        import ctypes
        from ctypes import wintypes

        class IoCounters(ctypes.Structure):
            _fields_ = [(name, ctypes.c_ulonglong) for name in (
                "ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
                "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]

        class BasicLimits(ctypes.Structure):
            _fields_ = [
                ("PerProcessUserTimeLimit", ctypes.c_int64),
                ("PerJobUserTimeLimit", ctypes.c_int64),
                ("LimitFlags", wintypes.DWORD),
                ("MinimumWorkingSetSize", ctypes.c_size_t),
                ("MaximumWorkingSetSize", ctypes.c_size_t),
                ("ActiveProcessLimit", wintypes.DWORD),
                ("Affinity", ctypes.c_size_t),
                ("PriorityClass", wintypes.DWORD),
                ("SchedulingClass", wintypes.DWORD),
            ]

        class ExtendedLimits(ctypes.Structure):
            _fields_ = [
                ("BasicLimitInformation", BasicLimits),
                ("IoInfo", IoCounters),
                ("ProcessMemoryLimit", ctypes.c_size_t),
                ("JobMemoryLimit", ctypes.c_size_t),
                ("PeakProcessMemoryUsed", ctypes.c_size_t),
                ("PeakJobMemoryUsed", ctypes.c_size_t),
            ]

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateJobObjectW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p]
        kernel32.CreateJobObjectW.restype = wintypes.HANDLE
        kernel32.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
        kernel32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]

        job = kernel32.CreateJobObjectW(None, None)
        if not job:
            return None
        limits = ExtendedLimits()
        limits.BasicLimitInformation.LimitFlags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not kernel32.SetInformationJobObject(job, 9, ctypes.byref(limits), ctypes.sizeof(limits)) or \
                not kernel32.AssignProcessToJobObject(job, wintypes.HANDLE(int(process._handle))):
            kernel32.CloseHandle(job)
            return None
        return (kernel32, job)
    except Exception:
        return None


def close_job(job):
    if job:
        kernel32, handle = job
        kernel32.CloseHandle(handle)


def kill_process_tree(process):
    if process.poll() is not None:
        return
    if os.name == "nt":
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(process.pid)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        process.kill()
    process.wait()


def gpu_temperature():
    """Degrees Celsius, or None when the reading is not available."""
    try:
        output = subprocess.check_output(
            ["nvidia-smi", "--query-gpu=temperature.gpu", "--format=csv,noheader,nounits"],
            text=True, timeout=5, stderr=subprocess.DEVNULL,
        )
        return int(output.strip().splitlines()[0])
    except Exception:
        return None


def descendant_pids(root):
    """Process ids of `root` and everything it started. Windows only."""
    if os.name != "nt":
        return [root]
    try:
        import ctypes
        from ctypes import wintypes

        class ProcessEntry(ctypes.Structure):
            _fields_ = [
                ("dwSize", wintypes.DWORD),
                ("cntUsage", wintypes.DWORD),
                ("th32ProcessID", wintypes.DWORD),
                ("th32DefaultHeapID", ctypes.c_size_t),
                ("th32ModuleID", wintypes.DWORD),
                ("cntThreads", wintypes.DWORD),
                ("th32ParentProcessID", wintypes.DWORD),
                ("pcPriClassBase", ctypes.c_long),
                ("dwFlags", wintypes.DWORD),
                ("szExeFile", wintypes.WCHAR * 260),
            ]

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
        kernel32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
        kernel32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
        snap = kernel32.CreateToolhelp32Snapshot(0x00000002, 0)
        if snap == wintypes.HANDLE(-1).value:
            return [root]
        children = {}
        entry = ProcessEntry()
        entry.dwSize = ctypes.sizeof(ProcessEntry)
        ok = kernel32.Process32FirstW(snap, ctypes.byref(entry))
        while ok:
            children.setdefault(entry.th32ParentProcessID, []).append(entry.th32ProcessID)
            ok = kernel32.Process32NextW(snap, ctypes.byref(entry))
        kernel32.CloseHandle(snap)
        found = []
        pending = [root]
        while pending:
            pid = pending.pop()
            found.append(pid)
            pending.extend(children.get(pid, []))
        return found
    except Exception:
        return [root]


def set_process_tree_running(root, running):
    """Pause or continue a process tree. Pausing stops the GPU work so the chip can cool."""
    if os.name != "nt":
        return False
    try:
        import ctypes
        from ctypes import wintypes

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        ntdll = ctypes.WinDLL("ntdll")
        kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel32.OpenProcess.restype = wintypes.HANDLE
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        action = ntdll.NtResumeProcess if running else ntdll.NtSuspendProcess
        action.argtypes = [wintypes.HANDLE]
        action.restype = ctypes.c_long
        ok = False
        for pid in descendant_pids(root):
            handle = kernel32.OpenProcess(0x0800, False, pid)
            if not handle:
                continue
            if action(handle) == 0:
                ok = True
            kernel32.CloseHandle(handle)
        return ok
    except Exception:
        return False


def run_logged(command, env=None, progress_range=None, stage="Procesando", guard_gpu=False):
    log(" ".join([str(p) for p in command]))
    process = open_logged_process(command, env)
    job = attach_kill_on_close_job(process)
    last_percent = None
    paused = {"on": False}

    def gpu_guard():
        # The laptop powers off at 90 C. Pause the segmentation at 80 and let the fans catch up.
        announced = False
        while process.poll() is None:
            temp = gpu_temperature()
            if temp is None:
                if not announced:
                    announced = True
                    log("No se pudo leer la temperatura de la GPU.")
                time.sleep(5)
                continue
            if not paused["on"] and temp >= 80:
                if set_process_tree_running(process.pid, False):
                    paused["on"] = True
                    log(f"GPU a {temp}°. Pausa para que no llegue a 90°.")
            elif paused["on"] and temp <= 68:
                if set_process_tree_running(process.pid, True):
                    paused["on"] = False
                    log(f"GPU a {temp}°. Sigue la segmentacion.")
            time.sleep(2)

    if guard_gpu:
        threading.Thread(target=gpu_guard, daemon=True).start()

    def handle(text):
        nonlocal last_percent
        bar = TQDM_PROGRESS.search(text) if progress_range else None
        if bar and int(bar.group(2)) > 0:
            done, total = min(int(bar.group(1)), int(bar.group(2))), int(bar.group(2))
            start, end = progress_range
            percent = int(start + (end - start) * done / total)
            if percent != last_percent or done == total:
                last_percent = percent
                remaining = f", faltan {bar.group(3)}" if bar.group(3) and done < total else ""
                progress(percent, f"{stage}: {done}/{total}{remaining}")
            return  # bar refreshes would flood the log
        log(text)

    def read_output():
        with process.stdout:
            for line in process.stdout:
                handle(line.rstrip())

    reader = threading.Thread(target=read_output, daemon=True)
    reader.start()
    try:
        code = process.wait()
        # Worker processes inherit the pipe and keep it open after a crash: drain briefly, then stop them.
        reader.join(timeout=5.0)
    finally:
        if paused["on"]:
            set_process_tree_running(process.pid, True)
            paused["on"] = False
        # A failure or exception here must not leave nnU-Net running on the GPU.
        kill_process_tree(process)
        close_job(job)
    reader.join(timeout=5.0)
    if code != 0:
        raise RuntimeError(f"Comando fallo con codigo {code}: {command[0]}")


def fill_bone_interiors(mask, prediction, intensities, spacing_zyx, source_label):
    """Complete bounded bone interiors, not a tissue classification or global fill."""
    import scipy.ndimage as ndi

    if source_label not in (1, 2):
        raise ValueError("El relleno automatico solo admite maxilar y mandibula.")
    if mask.shape != prediction.shape or mask.shape != intensities.shape:
        raise ValueError("El hueso y la imagen deben compartir la misma cuadricula.")

    name = "Maxilar" if source_label == 1 else "Mandibula"
    # Teeth complete the jaw boundary but remain separate prediction labels.
    enclosure = mask | (prediction == (3 if source_label == 1 else 4))
    if not np.any(enclosure):
        return mask
    bounds = []
    for axis in range(3):
        occupied = np.flatnonzero(enclosure.any(axis=tuple(i for i in range(3) if i != axis)))
        bounds.append(slice(max(0, occupied[0] - 1), min(mask.shape[axis], occupied[-1] + 2)))
    roi = tuple(bounds)
    enclosure = enclosure[roi]

    # Diagonal openings remain connected to the exterior. Slice-wise filling
    # would incorrectly seal open anatomy and large cranial spaces.
    connectivity = np.ones((3, 3, 3), dtype=bool)
    cavities = ndi.binary_fill_holes(enclosure, structure=connectivity) & ~enclosure
    labels, count = ndi.label(cavities, structure=connectivity)
    if count == 0:
        log(f"    [Hueso - {name}] Sin cavidades internas cerradas para completar.")
        return mask

    # Conservative geometric guard, in physical units, not a clinical threshold.
    max_interior_radius_mm = 6.0
    accepted = np.zeros(count + 1, dtype=bool)
    rejected_air = rejected_size = 0
    for cavity_id, region in enumerate(ndi.find_objects(labels), start=1):
        component = labels[region] == cavity_id
        values = intensities[roi][region][component]
        if np.any(~np.isfinite(values)) or np.any(values < -300):
            rejected_air += 1
            continue
        distance = ndi.distance_transform_edt(np.pad(component, 1), sampling=spacing_zyx)
        if distance.max() > max_interior_radius_mm:
            rejected_size += 1
            continue
        accepted[cavity_id] = True

    # Never replace teeth, the mandibular canal, or another predicted structure.
    additions = accepted[labels] & (prediction[roi] == 0)
    added_count = int(np.count_nonzero(additions))
    log(f"    [Hueso - {name}] Relleno interno: {added_count} voxeles; "
        f"cavidades omitidas por aire/datos invalidos: {rejected_air}, "
        f"por tamano: {rejected_size}.")
    if not added_count:
        return mask
    result = mask.copy()
    result[roi] |= additions
    return result


def bone_thickening_params(source_label):
    """`thicken_thin_bone` settings per bone, each overridable by environment variable.

    The maxilla gets a stronger seal (user's report, 2026-10-06: "enfócate en el maxilar, la mandíbula está bien"):
    its anterior wall over the sinus is so thin that the CT reads a gap in it as half bone, half sinus air —
    around -400 HU — which the general rule took for an opening. Its pinholes are filled down to -700 HU and
    about 3.5 mm across. A wider seal fills the nose and the orbits, which have soft tissue and bone on both
    sides a few millimetres apart. A real opening into air (-1000) still stays open. The mandible keeps the
    general rule.
    """
    def env_float(name, default_value, minimum=None):
        try:
            value = float(os.environ.get(name, str(default_value)).strip())
        except Exception:
            value = default_value
        return value if minimum is None else max(minimum, value)

    if source_label == 1:
        return {
            "grow_mm": env_float("DENTALSEGMENTATOR_MAXILLA_GROW_MM", env_float("DENTALSEGMENTATOR_BONE_GROW_MM", 1.2, 0.0), 0.0),
            "grow_hu": env_float("DENTALSEGMENTATOR_MAXILLA_GROW_HU", env_float("DENTALSEGMENTATOR_BONE_GROW_HU", 100.0)),
            "seal_mm": env_float("DENTALSEGMENTATOR_MAXILLA_SEAL_MM", 3.5, 0.0),
            "seal_min_hu": env_float("DENTALSEGMENTATOR_MAXILLA_SEAL_MIN_HU", -700.0),
        }
    return {
        "grow_mm": env_float("DENTALSEGMENTATOR_BONE_GROW_MM", 1.2, 0.0),
        "grow_hu": env_float("DENTALSEGMENTATOR_BONE_GROW_HU", 100.0),
        "seal_mm": env_float("DENTALSEGMENTATOR_BONE_SEAL_MM", 2.5, 0.0),
        "seal_min_hu": env_float("DENTALSEGMENTATOR_BONE_SEAL_MIN_HU", -200.0),
    }


def thicken_thin_bone(mask, prediction, intensities, spacing_zyx, grow_mm=1.2, grow_hu=100.0,
                      seal_mm=2.5, seal_min_hu=-200.0, report=True):
    """Recover the thin walls DentalSegmentator under-segments and seal the pinholes they leave.

    The anterior maxilla and the sinus walls are often under a millimetre thick, so partial volume keeps them
    below the network's threshold: the segmented bone came out thin and perforated, and the guide's envelope
    fell into those holes (user's report, 2026-10-05). Two bounded steps, in millimetres:
    - grow: up to `grow_mm` outwards, only into unlabelled voxels at least `grow_hu` (bone, not soft tissue);
    - seal: a closing of `seal_mm` that only adds unlabelled voxels denser than air (`seal_min_hu`), so a real
      opening to the sinus or the nose stays open.
    """
    import scipy.ndimage as ndi

    if not np.any(mask):
        return mask
    if mask.shape != prediction.shape or mask.shape != intensities.shape:
        raise ValueError("El hueso y la imagen deben compartir la misma cuadricula.")
    free = prediction == 0
    result = mask.copy()
    if grow_mm > 0:
        steps = max(1, int(round(grow_mm / max(1e-6, min(spacing_zyx)))))
        dense = free & (intensities >= grow_hu)
        struct = ndi.generate_binary_structure(3, 1)
        for _ in range(steps):
            grown = ndi.binary_dilation(result, structure=struct) & dense & ~result
            if not np.any(grown):
                break
            result |= grown
    if seal_mm > 0:
        # A ball closing never seals a hole in a wall one voxel thick: the ball always reaches past the wall
        # above and below the hole. Instead a voxel is filled when bone lies within `seal_mm` on BOTH sides of it
        # along at least two of the 13 axes of the voxel grid: true inside a perforation of a wall, false in the
        # hollow of a concave corner, which keeps its shape.
        candidates = free & ~result & np.isfinite(intensities) & (intensities > seal_min_hu)
        if np.any(candidates):
            occupied = np.argwhere(result)
            pad = [int(np.ceil(seal_mm / max(1e-6, sp))) + 1 for sp in spacing_zyx]
            lo = [max(0, int(occupied[:, a].min()) - pad[a]) for a in range(3)]
            hi = [min(result.shape[a], int(occupied[:, a].max()) + pad[a] + 1) for a in range(3)]
            roi = tuple(slice(lo[a], hi[a]) for a in range(3))
            directions = [(dz, dy, dx) for dz in (-1, 0, 1) for dy in (-1, 0, 1) for dx in (-1, 0, 1)
                          if (dz, dy, dx) > (0, 0, 0)]

            def shifted(volume, offset):
                out = np.zeros_like(volume)
                src = tuple(slice(max(0, -o), volume.shape[a] - max(0, o)) for a, o in enumerate(offset))
                dst = tuple(slice(max(0, o), volume.shape[a] - max(0, -o)) for a, o in enumerate(offset))
                out[dst] = volume[src]
                return out

            # A few passes: the middle of a hole closes first, then its rim sees bone on both sides too.
            for _ in range(4):
                bone = result[roi]
                pairs = np.zeros(bone.shape, dtype=np.uint8)
                for d in directions:
                    length = float(np.sqrt(sum((d[a] * spacing_zyx[a]) ** 2 for a in range(3))))
                    steps = max(1, int(np.floor(seal_mm / length + 1e-6)))
                    ahead = np.zeros(bone.shape, dtype=bool)
                    behind = np.zeros(bone.shape, dtype=bool)
                    for k in range(1, steps + 1):
                        ahead |= shifted(bone, tuple(-k * c for c in d))
                        behind |= shifted(bone, tuple(k * c for c in d))
                    pairs += (ahead & behind).astype(np.uint8)
                sealed = (pairs >= 2) & candidates[roi] & ~bone
                if not np.any(sealed):
                    break
                result[roi] |= sealed
    if report:
        log(f"    [Hueso] Engrosado de paredes finas: {int(np.count_nonzero(result & ~mask))} voxeles.")
    return result


def medulla_fill_params(source_label):
    """`fill_medullary_bone` settings per bone, each overridable by environment variable.

    The mandible's marrow can be fatty (down to about -100 HU in older patients) and its body and angle are far
    wider than the 6 mm `fill_bone_interiors` allows, so it gets no size limit: its canal has its own label. The
    upper label is the whole skull: there only cancellous density counts (>= 100 HU) and a cavity may not be
    wider than marrow (`max_radius_mm`), so neither the sinus mucosa nor the brain is ever taken for bone."""
    def env(name, default_value):
        try:
            return float(os.environ.get(name, str(default_value)).strip())
        except Exception:
            return default_value

    if source_label == 1:
        return {"seal_mm": env("DENTALSEGMENTATOR_MAXILLA_MEDULLA_SEAL_MM", 3.5),
                "min_hu": env("DENTALSEGMENTATOR_MAXILLA_MEDULLA_MIN_HU", 100.0),
                "max_radius_mm": env("DENTALSEGMENTATOR_MAXILLA_MEDULLA_MAX_RADIUS_MM", 8.0)}
    return {"seal_mm": env("DENTALSEGMENTATOR_MEDULLA_SEAL_MM", 2.5),
            "min_hu": env("DENTALSEGMENTATOR_MEDULLA_MIN_HU", -100.0),
            "max_radius_mm": env("DENTALSEGMENTATOR_MEDULLA_MAX_RADIUS_MM", 0.0)}


def fill_medullary_bone(mask, prediction, intensities, spacing_zyx, source_label, seal_mm=None, min_hu=None,
                        max_radius_mm=None):
    """Make the bone solid between its cortices (user's report, 2026-10-19: the medulla came out empty and the
    osteotomies showed two shells with nothing between them).

    `fill_bone_interiors` stays strict and is still run after this, but on a real jaw it leaves the marrow empty:
    one pore in the cortex opens the cavity, one voxel under -300 HU rejects all of it, and the body and angle of
    the mandible are wider than its 6 mm. Here:
    - pores up to about 2 x `seal_mm` are sealed as `thicken_thin_bone` seals them (bone on both sides along two
      grid axes), for this test only;
    - what that sealed shell encloses is marrow, and so are the sealed pores next to it; anything open to the
      outside — a groove, a notch, the edge of the scan — is not;
    - only voxels the network left unlabelled, with finite intensity of at least `min_hu`, are added: air pockets
      stay air, teeth, the canal and other labels keep theirs;
    - a cavity wider than `max_radius_mm` (when > 0) is not marrow and is left as it is."""
    import scipy.ndimage as ndi

    if source_label not in (1, 2):
        raise ValueError("El relleno medular solo admite maxilar y mandibula.")
    if mask.shape != prediction.shape or mask.shape != intensities.shape:
        raise ValueError("El hueso y la imagen deben compartir la misma cuadricula.")
    params = medulla_fill_params(source_label)
    seal_mm = params["seal_mm"] if seal_mm is None else seal_mm
    min_hu = params["min_hu"] if min_hu is None else min_hu
    max_radius_mm = params["max_radius_mm"] if max_radius_mm is None else max_radius_mm
    name = "Maxilar" if source_label == 1 else "Mandibula"
    # Teeth close the jaw's outline (as in fill_bone_interiors) but keep their own label.
    enclosure = mask | (prediction == (3 if source_label == 1 else 4))
    if not np.any(enclosure):
        return mask
    occupied = np.argwhere(enclosure)
    pad = [int(np.ceil(seal_mm / max(1e-6, sp))) + 2 for sp in spacing_zyx]
    lo = [max(0, int(occupied[:, a].min()) - pad[a]) for a in range(3)]
    hi = [min(mask.shape[a], int(occupied[:, a].max()) + pad[a] + 1) for a in range(3)]
    roi = tuple(slice(lo[a], hi[a]) for a in range(3))
    bone = enclosure[roi]
    # Seal the pores for this test only, as thicken_thin_bone seals them (a ball closing cannot: it passes
    # through a hole in a wall one or two voxels thick). The scan's own edge counts as outside, so a bone the
    # scan cuts open stays open (fill_bone_interiors' rule).
    framed = np.pad(bone, 1)
    if seal_mm > 0:
        sealed = thicken_thin_bone(framed, np.zeros(framed.shape, dtype=np.uint8),
                                   np.zeros(framed.shape, dtype=np.float32), spacing_zyx,
                                   grow_mm=0.0, seal_mm=seal_mm, seal_min_hu=-1.0, report=False)
    else:
        sealed = framed
    connectivity = np.ones((3, 3, 3), dtype=bool)
    enclosed = ndi.binary_fill_holes(sealed, structure=connectivity)[1:-1, 1:-1, 1:-1]
    sealed = sealed[1:-1, 1:-1, 1:-1]
    holes = enclosed & ~sealed
    if not np.any(holes):
        log(f"    [Hueso - {name}] Relleno medular: sin interior cerrado.")
        return mask
    # The seal also took marrow next to the caps and corners, and the pores themselves: take every free voxel
    # inside the sealed shell that is connected to the marrow, never through bone. A groove the seal closed on
    # the outside is not connected to it, and stays out.
    free = enclosed & ~bone
    struct = ndi.generate_binary_structure(3, 1)
    components, _ = ndi.label(free, structure=struct)
    inner_ids = np.unique(components[holes & (components > 0)])
    reach = np.isin(components, inner_ids) & (components > 0)
    values = intensities[roi]
    marrow = reach & (prediction[roi] == 0) & np.isfinite(values) & (values >= min_hu)
    rejected = 0
    if max_radius_mm and max_radius_mm > 0 and np.any(marrow):
        labels, count = ndi.label(holes, structure=connectivity)
        for cavity_id, region in enumerate(ndi.find_objects(labels), start=1):
            if region is None:
                continue
            component = labels[region] == cavity_id
            depth = ndi.distance_transform_edt(np.pad(component, 1), sampling=spacing_zyx)
            if depth.max() > max_radius_mm:
                # Too wide for marrow (a sinus, the cranial cavity): neither it nor its rim is filled.
                marrow &= ~np.isin(components, np.unique(components[labels == cavity_id]))
                rejected += 1
    added = int(np.count_nonzero(marrow))
    log(f"    [Hueso - {name}] Relleno medular: {added} voxeles (sellado {seal_mm:.1f} mm, >= {min_hu:.0f} HU"
        + (f", {rejected} cavidad(es) demasiado anchas omitidas" if rejected else "") + ").")
    if not added:
        return mask
    result = mask.copy()
    result[roi] |= marrow
    return result


def solid_bone_mask(mask, prediction, intensities, spacing_zyx, source_label, air_hu=-300.0, max_radius_mm=8.0,
                    reach_mm=15.0, max_escapes=3, air_margin_mm=2.0):
    """The bone as a solid: cortex and marrow, whatever the marrow's density (user's request, 2026-10-19:
    "rellena todo dentro de las corticales ... no rellenar las orbitas ni la nariz").

    - Closed cavities: what the bone encloses in 3D is filled, if no wider than `max_radius_mm`.
    - Enclosed marrow: a free voxel is marrow when, of the 26 grid directions, at most `max_escapes` reach
      `reach_mm` without meeting this bone (teeth and the canal count as its walls). The cortex has to surround
      it on every side; the fill stops where a foramen or an opening lets more directions out, and the orbits,
      the cranial cavity and the pharynx are far too open. A tube of marrow along the mandibular body loses only
      the two or three directions along its axis.
    - Never air (< `air_hu`), never within `air_margin_mm` of air: the mucosa of the nasal cavity and the sinuses
      lines air, and the nose's meatuses are tubes the rule above would otherwise call enclosed.
    No lower HU limit otherwise: fatty marrow reads below 0 HU, in CBCT lower still. Teeth, the canal and every
    other label keep their voxels. A pit narrower than `reach_mm` in the surface counts as enclosed, as a
    closing would."""
    import scipy.ndimage as ndi

    if source_label not in (1, 2):
        raise ValueError("El hueso solido solo admite maxilar y mandibula.")
    if mask.shape != prediction.shape or mask.shape != intensities.shape:
        raise ValueError("El hueso y la imagen deben compartir la misma cuadricula.")
    name = "Maxilar" if source_label == 1 else "Mandibula"
    if not np.any(mask):
        return mask
    walls = mask | (prediction == (3 if source_label == 1 else 4))
    if source_label == 2:
        walls |= prediction == 5
    pad = [int(np.ceil(reach_mm / max(1e-6, sp))) + 2 for sp in spacing_zyx]
    occupied = np.argwhere(walls)
    lo = [max(0, int(occupied[:, a].min()) - pad[a]) for a in range(3)]
    hi = [min(mask.shape[a], int(occupied[:, a].max()) + pad[a] + 1) for a in range(3)]
    roi = tuple(slice(lo[a], hi[a]) for a in range(3))
    bone = mask[roi]
    wall = walls[roi]
    values = intensities[roi]
    finite = np.isfinite(values)
    air = ~finite | (values < air_hu)
    free = (prediction[roi] == 0) & ~bone & ~air

    def narrow(holes, sampling):
        labels, count = ndi.label(holes)
        if not count:
            return holes
        depth = ndi.distance_transform_edt(holes, sampling=sampling)
        widest = ndi.maximum(depth, labels, index=np.arange(1, count + 1))
        keep = np.zeros(count + 1, dtype=bool)
        keep[1:] = np.asarray(widest) <= max_radius_mm
        return keep[labels]

    # Closed cavities (the scan's edge is outside: a bone the scan cuts open stays open there).
    enclosure = np.pad(wall, 1)
    holes = (ndi.binary_fill_holes(enclosure) & ~enclosure)[1:-1, 1:-1, 1:-1]
    cavities = narrow(holes, spacing_zyx) & free
    wall = wall | cavities

    # Enclosed marrow: count, for every voxel, the directions that meet the wall within reach.
    candidates = free & ~cavities
    if air_margin_mm > 0 and np.any(air):
        candidates &= ndi.distance_transform_edt(~air, sampling=spacing_zyx) > air_margin_mm
    candidates &= ndi.distance_transform_edt(~wall, sampling=spacing_zyx) <= reach_mm
    marrow = np.zeros(bone.shape, dtype=bool)
    if np.any(candidates):
        box = np.argwhere(candidates)
        c_lo = box.min(axis=0)
        c_hi = box.max(axis=0) + 1
        hits = np.zeros(tuple(c_hi - c_lo), dtype=np.uint8)
        for dz in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    if dz == dy == dx == 0:
                        continue
                    step = np.sqrt((dz * spacing_zyx[0]) ** 2 + (dy * spacing_zyx[1]) ** 2
                                   + (dx * spacing_zyx[2]) ** 2)
                    seen = np.zeros(hits.shape, dtype=bool)
                    for k in range(1, int(reach_mm / step) + 1):
                        src_lo = c_lo + k * np.array((dz, dy, dx))
                        src_hi = c_hi + k * np.array((dz, dy, dx))
                        # The part of the shifted box inside the ROI; beyond it there is no wall.
                        a = np.maximum(src_lo, 0)
                        z = np.minimum(src_hi, wall.shape)
                        if np.any(z <= a):
                            break
                        dst = tuple(slice(int(a[n] - src_lo[n]), int(z[n] - src_lo[n])) for n in range(3))
                        seen[dst] |= wall[a[0]:z[0], a[1]:z[1], a[2]:z[2]]
                    hits += seen
        inner = hits >= 26 - max_escapes
        marrow[c_lo[0]:c_hi[0], c_lo[1]:c_hi[1], c_lo[2]:c_hi[2]] = inner
        marrow &= candidates

    log(f"    [Hueso - {name}] Solido: cavidades cerradas +{int(cavities.sum())}, medular encerrada "
        f"+{int(marrow.sum())} voxeles (encierro a {reach_mm:.0f} mm con <= {max_escapes} de 26 direcciones "
        f"libres; nunca aire ni a < {air_margin_mm:.0f} mm de aire; sin umbral de HU).")
    result = mask.copy()
    result[roi] = bone | cavities | marrow
    return result


def drop_bone_islands(mask, spacing_zyx, source_label, min_mm3=50.0):
    """Fragments smaller than `min_mm3` are debris, not bone: on the user's CT (2026-10-19) the skull came out in
    90 pieces and the mandible in 8, each a loose speck in the 3D view."""
    import scipy.ndimage as ndi

    labels, count = ndi.label(mask, structure=np.ones((3, 3, 3), dtype=bool))
    if count <= 1:
        return mask
    voxel_mm3 = float(np.prod(spacing_zyx))
    sizes = np.bincount(labels.ravel()) * voxel_mm3
    small = sizes < min_mm3
    small[0] = False
    dropped = int(small.sum())
    if not dropped:
        return mask
    name = "Maxilar" if source_label == 1 else "Mandibula"
    log(f"    [Hueso - {name}] {dropped} fragmento(s) < {min_mm3:.0f} mm3 quitados; quedan {count - dropped}.")
    return mask & ~small[labels]


def split_cortical_medullary(solid, intensities, cortical_hu=500.0, air_hu=-300.0, min_island_voxels=27):
    """Cortex = solid at cortical density, without islands; marrow = the rest of the solid that is not air.
    Returns (cortical, medullary, air): disjoint masks whose union is `solid`."""
    import scipy.ndimage as ndi

    finite = np.isfinite(intensities)
    cortical = solid & finite & (intensities >= cortical_hu)
    labels, count = ndi.label(cortical)
    if count:
        sizes = np.bincount(labels.ravel())
        small = sizes < min_island_voxels
        small[0] = False
        cortical &= ~small[labels]
    air = solid & ~cortical & (~finite | (intensities < air_hu))
    medullary = solid & ~cortical & ~air
    return cortical, medullary, air


def bone_surface_mesh(mask, spacing_zyx, sigma_voxels=0.75):
    """A closed surface of a mask, for checking it: float mask, Gaussian of `sigma_voxels`, marching cubes at 0.5
    in millimetres, padded so a bone at the scan's edge still closes. Returns a trimesh.Trimesh, or None."""
    import scipy.ndimage as ndi
    import trimesh
    from skimage.measure import marching_cubes

    if not np.any(mask):
        return None
    occupied = np.argwhere(mask)
    pad = int(np.ceil(3 * sigma_voxels)) + 2
    lo = np.maximum(occupied.min(axis=0) - pad, 0)
    hi = occupied.max(axis=0) + pad + 1
    sub = np.pad(mask[lo[0]:hi[0], lo[1]:hi[1], lo[2]:hi[2]].astype(np.float32), pad)
    field = ndi.gaussian_filter(sub, sigma_voxels)
    vertices, faces, _, _ = marching_cubes(field, level=0.5, spacing=tuple(float(s) for s in spacing_zyx))
    return trimesh.Trimesh(vertices=vertices, faces=faces, process=True)


def log_volume_spacing(spacing_xyz):
    """Spacing in millimetres; a ratio over 1.5 between the coarsest and finest axes steps the surfaces."""
    ratio = max(spacing_xyz) / max(1e-6, min(spacing_xyz))
    log("    [Diagnostico] Spacing del volumen (x, y, z): "
        + ", ".join(f"{s:.3f}" for s in spacing_xyz) + f" mm; anisotropia {ratio:.2f}.")
    if ratio > 1.5:
        log(f"    [Diagnostico] AVISO: max/min spacing = {ratio:.2f} > 1.5; la malla se contorna sobre un campo "
            "refinado a voxel casi isotropico para no mostrar escalones.")
    return ratio


def log_bone_report(solid, intensities, spacing_zyx, source_label, check_mesh=True):
    """Voxel counts of the cortex and the marrow, and the closure of the surface, for one bone."""
    name = "Maxilar" if source_label == 1 else "Mandibula"
    voxel_cm3 = float(np.prod(spacing_zyx)) / 1000.0
    cortical, medullary, air = split_cortical_medullary(solid, intensities)
    total = int(solid.sum())
    disjoint = not np.any(cortical & medullary)
    covers = np.array_equal(cortical | medullary | air, solid)
    log(f"    [Diagnostico - {name}] solido {total} voxeles ({total * voxel_cm3:.1f} cm3): cortical "
        f"{int(cortical.sum())}, medular {int(medullary.sum())}, aire {int(air.sum())}; "
        f"disjuntos {'si' if disjoint else 'NO'}, union = solido {'si' if covers else 'NO'}.")
    if total and medullary.sum() < 0.01 * total:
        log(f"    [Diagnostico - {name}] AVISO: la medular es ~0: el hueco esta en la mascara, no en la malla.")
    if not check_mesh or not total:
        return
    try:
        mesh = bone_surface_mesh(solid, spacing_zyx)
        import trimesh
    except ImportError as exc:
        log(f"    [Diagnostico - {name}] Malla sin revisar (falta {exc.name}: pip install trimesh scikit-image).")
        return
    components = len(trimesh.graph.connected_components(mesh.face_adjacency, nodes=np.arange(len(mesh.faces))))
    volume = f"{abs(mesh.volume) / 1000.0:.1f} cm3" if mesh.is_watertight else "no definido (abierta)"
    log(f"    [Diagnostico - {name}] Malla: watertight {'si' if mesh.is_watertight else 'NO'}, "
        f"{components} componente(s), volumen {volume}, {len(mesh.faces)} triangulos.")


def refine_coarse_bone_mask(mask, prediction, intensities, spacing_zyx, band_mm=2.0, add_hu=250.0, remove_hu=100.0):
    """Re-read a coarse bone mask's boundary from the CT at full resolution.

    A network that predicts on a 1.5 mm grid (TotalSegmentator) hands its mask back on the CT's grid as 1.5 mm
    blocks, and the mesh shows every block as a terrace (user's report, 2026-10-19). Within `band_mm` of the
    mask's boundary only:
    - unlabelled voxels of at least `add_hu`, connected to the mask, are added (the bone the blocks cut off);
    - mask voxels under `remove_hu` reached from the outside through such voxels are removed (the soft tissue the
      blocks took in). Carving only from the outside keeps a low-density marrow behind its cortex.
    Other labels are never touched. Meant for coarse predictions: a native-resolution mask (DentalSegmentator)
    does not need it, and on the thin maxillary walls, whose partial volume reads below `remove_hu`, it would
    thin them."""
    import scipy.ndimage as ndi

    if mask.shape != prediction.shape or mask.shape != intensities.shape:
        raise ValueError("El hueso y la imagen deben compartir la misma cuadricula.")
    if not np.any(mask) or band_mm <= 0:
        return mask
    occupied = np.argwhere(mask)
    pad = [int(np.ceil(band_mm / max(1e-6, sp))) + 2 for sp in spacing_zyx]
    lo = [max(0, int(occupied[:, a].min()) - pad[a]) for a in range(3)]
    hi = [min(mask.shape[a], int(occupied[:, a].max()) + pad[a] + 1) for a in range(3)]
    roi = tuple(slice(lo[a], hi[a]) for a in range(3))
    bone = mask[roi]
    values = intensities[roi]
    finite = np.isfinite(values)
    free = (prediction[roi] == 0) & ~bone
    struct = ndi.generate_binary_structure(3, 1)
    outside_distance = ndi.distance_transform_edt(~bone, sampling=spacing_zyx)
    inside_distance = ndi.distance_transform_edt(bone, sampling=spacing_zyx)

    # Carve: soft tissue inside the outer band, reached from outside the mask.
    soft = bone & (inside_distance <= band_mm) & finite & (values < remove_hu)
    exterior = ~bone & ~(free & finite & (values >= add_hu))
    labels, count = ndi.label(soft | exterior, structure=struct)
    removed = np.zeros_like(bone)
    if count:
        outside_ids = np.unique(labels[exterior & (labels > 0)])
        removed = soft & np.isin(labels, outside_ids)

    # Add: dense free voxels in the outer band, connected to what remains of the mask.
    kept = bone & ~removed
    dense = free & finite & (values >= add_hu) & (outside_distance <= band_mm)
    labels, count = ndi.label(dense | kept, structure=struct)
    added = np.zeros_like(bone)
    if count:
        kept_ids = np.unique(labels[kept & (labels > 0)])
        added = dense & np.isin(labels, kept_ids)

    log(f"    [Hueso] Borde refinado con el TC: +{int(np.count_nonzero(added))} / "
        f"-{int(np.count_nonzero(removed))} voxeles (banda {band_mm:.1f} mm).")
    result = mask.copy()
    result[roi] = kept | added
    return result


def cap_canal_openings(labelmap, spacing_zyx, bone_label=6, canal_label=7, cap_mm=1.0):
    """The mandibular canal reaches the bone's surface at the mental and mandibular foramina, and there the
    mandible's mesh had a hole the canal showed through (user's report, 2026-10-06: two holes either side of the
    chin). Canal voxels within `cap_mm` of the outside (at least one voxel) become bone: the canal keeps its
    course inside the mandible, the bone's surface stays closed. Changes `labelmap` in place."""
    import scipy.ndimage as ndi

    canal = labelmap == canal_label
    if not np.any(canal):
        return 0
    outside = labelmap == 0
    if not np.any(outside):
        return 0
    reach = max(cap_mm, 1.01 * max(spacing_zyx))
    depth = ndi.distance_transform_edt(~outside, sampling=spacing_zyx)
    cap = canal & (depth <= reach)
    labelmap[cap] = bone_label
    count = int(np.count_nonzero(cap))
    log(f"    [Canal mandibular] Salidas cerradas en la superficie del hueso: {count} voxeles.")
    return count


def fill_tooth_interiors(mask):
    """Fill holes inside a tooth. The crown often comes out as a shell or with pulp-sized gaps; closing those
    does not join a separate tooth, and it does not grow into the bone around it."""
    import scipy.ndimage as ndi

    if mask is None or not np.any(mask):
        return mask
    return ndi.binary_fill_holes(mask)


def teeth_sidecar_path(output_path, suffix):
    """Where one arch of teeth goes, next to the labelmap: <name>_<suffix><ext>."""
    path = Path(output_path)
    for ext in (".nii.gz", ".nrrd", ".mha", ".nii"):
        if path.name.endswith(ext):
            return path.with_name(path.name[: -len(ext)] + "_" + suffix + ext)
    return path.with_name(path.name + "_" + suffix + ".nrrd")


def upper_teeth_sidecar_path(output_path):
    """Where the upper teeth go, next to the labelmap: <name>_dientes_superiores<ext>."""
    return teeth_sidecar_path(output_path, "dientes_superiores")


def lower_teeth_sidecar_path(output_path):
    """Where the lower teeth go, next to the labelmap: <name>_dientes_inferiores<ext>."""
    return teeth_sidecar_path(output_path, "dientes_inferiores")


def write_teeth_sidecar(teeth_mask, reference, output_path, suffix):
    """One arch (DentalSegmentator 3 or 4) on its own.

    The labelmap keeps those voxels inside the jaw (APP_LABEL_MAP 3 -> 5, 4 -> 6) so the osteotomy
    still carries the teeth. This file is the only place they are apart. Returns the path, or None.
    """
    import numpy as np
    import SimpleITK as sitk

    if teeth_mask is None or not np.any(teeth_mask):
        return None
    image = sitk.GetImageFromArray(teeth_mask.astype(np.uint8))
    image.CopyInformation(reference)
    path = teeth_sidecar_path(output_path, suffix)
    sitk.WriteImage(image, str(path), True)
    return path


def write_upper_teeth_sidecar(teeth_mask, reference, output_path):
    """The upper teeth (DentalSegmentator label 3) on their own, for the guide's root analysis."""
    return write_teeth_sidecar(teeth_mask, reference, output_path, "dientes_superiores")


def write_lower_teeth_sidecar(teeth_mask, reference, output_path):
    """The lower teeth (DentalSegmentator label 4) on their own. The mandible labelmap is unchanged."""
    return write_teeth_sidecar(teeth_mask, reference, output_path, "dientes_inferiores")


def remap_prediction(input_path, prediction_path, output_path, target, seed=None, seed2=None,
                     bone_from_totalsegmentator=False):
    import numpy as np
    import SimpleITK as sitk

    if prediction_path:
        seg = sitk.ReadImage(str(prediction_path))
        arr = sitk.GetArrayFromImage(seg)
        out = np.zeros(arr.shape, dtype=np.uint8)
    else:
        seg = sitk.ReadImage(str(input_path))
        arr = None
        out = np.zeros(sitk.GetArrayFromImage(seg).shape, dtype=np.uint8)

    input_image = None
    input_array = None

    def source_image():
        nonlocal input_image, input_array
        if input_image is None:
            input_image = sitk.ReadImage(str(input_path))
            log_volume_spacing(input_image.GetSpacing())
            # Do not assign CT intensities to prediction voxels by array index
            # unless the complete physical grids agree (including NIfTI rounding).
            if (input_image.GetSize() != seg.GetSize()
                    or not np.allclose(input_image.GetSpacing(), seg.GetSpacing(), rtol=0, atol=1e-5)
                    or not np.allclose(input_image.GetOrigin(), seg.GetOrigin(), rtol=0, atol=1e-3)
                    or not np.allclose(input_image.GetDirection(), seg.GetDirection(), rtol=0, atol=1e-5)):
                raise ValueError("La prediccion y el TAC no comparten la cuadricula fisica; "
                                 "no se puede completar el hueso ni combinar las mascaras.")
            input_array = sitk.GetArrayFromImage(input_image)
        return input_image, input_array

    def radius_env(name, default_value):
        try:
            return max(0, int(os.environ.get(name, str(default_value)).strip()))
        except Exception:
            return default_value

    def conservative_mask(label_value):
        if arr is None:
            return None
        mask = arr == label_value
        if not np.any(mask):
            return mask
        if label_value == 5:
            return mask  # The mandibular canal is not a bone smoothing target.
        # TotalSegmentator already returns the skull and the mandible. The DentalSegmentator
        # closing would grow that surface and fill openings the craniofacial model left empty.
        if label_value in (1, 2) and bone_from_totalsegmentator:
            image, intensities = source_image()
            spacing_zyx = image.GetSpacing()[::-1]
            # Its 1.5 mm grid comes back as blocks, ripples on the mesh: the boundary is re-read from the CT.
            # Checked on the user's CT (2026-10-19): smooth jaws, but the maxilla's walls that read under 100 HU
            # open, so its pores are sealed without growing the bone (the TotalSegmentator wall is already full
            # thickness; growing it put 3 % of the sinus in the bone, sealing only 2.7 %).
            if os.environ.get("DENTALSEGMENTATOR_REFINE_COARSE_BONE", "1").strip() != "0":
                mask = refine_coarse_bone_mask(mask, arr, intensities, spacing_zyx)
            seal = dict(bone_thickening_params(label_value), grow_mm=0.0)
            mask = thicken_thin_bone(mask, arr, intensities, spacing_zyx, **seal)
            return complete_bone(mask, label_value, intensities, spacing_zyx)

        # DentalSegmentator may keep very thin bracket/metal spikes. A tiny
        # binary opening + closing keeps anatomy while softening serrated masks.
        if label_value in (3, 4):
            # Opening ate thin enamel and the contacts. Holes inside a tooth are filled afterwards.
            open_radius = radius_env("DENTALSEGMENTATOR_TEETH_OPEN_RADIUS", 0)
            close_radius = radius_env("DENTALSEGMENTATOR_TEETH_CLOSE_RADIUS", 1)
        else:
            open_radius = radius_env("DENTALSEGMENTATOR_BONE_OPEN_RADIUS", 0)
            close_radius = radius_env("DENTALSEGMENTATOR_BONE_CLOSE_RADIUS", 1)

        if open_radius > 0 or close_radius > 0:
            img = sitk.GetImageFromArray(mask.astype(np.uint8))
            try:
                if open_radius > 0:
                    img = sitk.BinaryMorphologicalOpening(img, [open_radius] * 3)
                if close_radius > 0:
                    img = sitk.BinaryMorphologicalClosing(img, [close_radius] * 3)
                mask = sitk.GetArrayFromImage(img) > 0
            except Exception as exc:
                log(f"Postproceso conservador omitido para label {label_value}: {exc}")

        if label_value in (3, 4):
            mask = fill_tooth_interiors(mask)
        mask &= (arr == 0) | (arr == label_value)
        if label_value in (1, 2):
            image, intensities = source_image()

            spacing_zyx = image.GetSpacing()[::-1]
            # A mask predicted on a coarser grid (TotalSegmentator, 1.5 mm) is re-read from the CT near its
            # boundary first; off by default because DentalSegmentator's masks are already at full resolution.
            if os.environ.get("DENTALSEGMENTATOR_REFINE_COARSE_BONE", "0").strip() == "1":
                mask = refine_coarse_bone_mask(mask, arr, intensities, spacing_zyx)
            mask = thicken_thin_bone(mask, arr, intensities, spacing_zyx, **bone_thickening_params(label_value))
            mask = complete_bone(mask, label_value, intensities, spacing_zyx)
        return mask

    def complete_bone(mask, label_value, intensities, spacing_zyx):
        """The bone as one solid, cortex and marrow (`solid_bone_mask`), whichever network gave it; then the
        strict small-cavity fill and the diagnostic report."""
        if os.environ.get("DENTALSEGMENTATOR_MEDULLA_FILL", "1").strip() != "0":
            mask = solid_bone_mask(mask, arr, intensities, spacing_zyx, label_value)
        mask = fill_bone_interiors(mask, arr, intensities, spacing_zyx, label_value)
        mask = drop_bone_islands(mask, spacing_zyx, label_value)
        log_bone_report(mask, intensities, spacing_zyx, label_value,
                        check_mesh=os.environ.get("DENTALSEGMENTATOR_MESH_CHECK", "1").strip() != "0")
        return mask

    target = target.lower()
    if target in ("teeth", "dientes"):
        if arr is not None:
            out[conservative_mask(3)] = 5
            out[conservative_mask(4)] = 6
    elif target in ("mandible", "mandibula", "mandíbula"):
        if arr is not None:
            out[conservative_mask(2)] = 6
    elif target in ("maxilla", "maxilar"):
        if arr is not None:
            out[conservative_mask(1)] = 5
    elif target in ("via aerea", "via_aerea", "airway"):
        pass
    else:
        if arr is not None:
            for source_label, app_label in APP_LABEL_MAP.items():
                out[conservative_mask(source_label)] = app_label

    if target in ("completo", "segmentacion automatica", "auto", "dental",
                  "via aerea", "via_aerea", "airway"):
        log("95% Generando mascaras avanzadas de tejido y via aerea...")
        import scipy.ndimage as ndi
        img, img_arr = source_image()
        depth, height, width = img_arr.shape
        struct3d = ndi.generate_binary_structure(3, 1)  # 6-conectividad 3D
        struct2d = ndi.generate_binary_structure(2, 1)  # 4-conectividad 2D

        # =========================================================
        # === 1. CUERPO / TEJIDOS BLANDOS (mascara solida) ========
        # =========================================================
        log("    [Tejidos Blandos] Aislando cuerpo del paciente...")
        # El paciente son todos los voxeles > -200 HU (hueso, musculo, grasa, etc.)
        body_raw = img_arr > -200
        # Erosion para eliminar artefactos de anillo del escaner
        body_eroded = ndi.binary_erosion(body_raw, structure=struct3d, iterations=2)
        # Quedarse solo con el componente mas grande (el paciente)
        labeled_body, n_body = ndi.label(body_eroded, structure=struct3d)
        if n_body > 0:
            counts = np.bincount(labeled_body.ravel()); counts[0] = 0
            body_core = labeled_body == counts.argmax()
        else:
            body_core = body_eroded
        # Restaurar y limpiar bordes
        clean_body = ndi.binary_dilation(body_core, structure=struct3d, iterations=2)

        if target in ("completo", "segmentacion automatica", "auto", "dental",
                      "via aerea", "via_aerea", "airway"):
            log("    [Tejidos Blandos] Generando mascara corporal sellada...")
            # Un pequeño closing en clean_body sella labios y nariz externamente
            # sin contraer ni aplastar los conductos de aire internos.
            closed_body = ndi.binary_closing(clean_body, structure=struct3d, iterations=3)
            solid_body = np.zeros_like(closed_body)
            for z in range(depth):
                solid_body[z] = ndi.binary_fill_holes(closed_body[z])
            
            if target in ("completo", "segmentacion automatica", "auto", "dental"):
                soft_tissue_mask = ndi.binary_closing(solid_body, structure=struct3d, iterations=2)
            else:
                soft_tissue_mask = None
        else:
            solid_body = None
            soft_tissue_mask = None

        # =========================================================
        # === 2. VIA AEREA - ALGORITMO TIPO MIMICS ================
        # =========================================================
        if target in ("via aerea", "via_aerea", "airway"):
            log("    [Via Aerea] Calculando mascara de aire interno (acotada al cuerpo)...")
            air_raw = img_arr < -300
            # Al acotar el aire al interior de solid_body, evitamos que crezca hacia el aire exterior (la sala),
            # resolviendo fugas sin necesidad de colapsar la via aerea interna.
            internal_air = solid_body & air_raw

            # Parse manual seeds if provided
            user_seed_point = None
            user_seed2_point = None
            p1_phys = None
            p2_phys = None

            if seed:
                try:
                    x_phys, y_phys, z_phys = map(float, seed.split(','))
                    p1_phys = np.array([x_phys, y_phys, z_phys], dtype=np.float32)
                    user_seed_point = img.TransformPhysicalPointToIndex((x_phys, y_phys, z_phys))
                    log(f"    [Via Aerea] Semilla 1 fisica (nasofaringe): {p1_phys} -> voxel: {user_seed_point}")
                except Exception as e:
                    log(f"    [Via Aerea] Error al parsear semilla 1 '{seed}': {e}")

            if seed2:
                try:
                    x_phys, y_phys, z_phys = map(float, seed2.split(','))
                    p2_phys = np.array([x_phys, y_phys, z_phys], dtype=np.float32)
                    user_seed2_point = img.TransformPhysicalPointToIndex((x_phys, y_phys, z_phys))
                    log(f"    [Via Aerea] Semilla 2 fisica (traquea): {p2_phys} -> voxel: {user_seed2_point}")
                except Exception as e:
                    log(f"    [Via Aerea] Error al parsear semilla 2 '{seed2}': {e}")

            # Define snap helper to find nearest air voxel if seed clicked on non-air boundary
            def find_nearest_air_voxel(idx_point):
                if idx_point is None:
                    return None
                x, y, z = idx_point
                if not (0 <= z < depth and 0 <= y < height and 0 <= x < width):
                    return None
                if internal_air[z, y, x]:
                    return (z, y, x)
                # Search in neighborhood (radius up to 3 voxels)
                for r in range(1, 4):
                    for dz in range(-r, r + 1):
                        for dy in range(-r, r + 1):
                            for dx in range(-r, r + 1):
                                nz, ny, nx = z + dz, y + dy, x + dx
                                if 0 <= nz < depth and 0 <= ny < height and 0 <= nx < width:
                                    if internal_air[nz, ny, nx]:
                                        return (nz, ny, nx)
                return (z, y, x)

            # If both seeds are provided, construct the cylinder bounding volume mask along the line segment
            if p1_phys is not None and p2_phys is not None:
                log("    [Via Aerea] Aplicando mascara de cilindro acotada entre Semilla 1 y Semilla 2...")
                spacing = img.GetSpacing()
                origin = img.GetOrigin()
                
                idx1 = np.array(user_seed_point)
                idx2 = np.array(user_seed2_point)
                
                padding_mm = float(os.environ.get("DENTALSEGMENTATOR_AIRWAY_PADDING", "50"))
                pad_voxels = np.ceil(padding_mm / np.array(spacing)).astype(int)
                
                z_min = max(0, min(idx1[2], idx2[2]) - pad_voxels[2])
                z_max = min(depth, max(idx1[2], idx2[2]) + pad_voxels[2] + 1)
                y_min = max(0, min(idx1[1], idx2[1]) - pad_voxels[1])
                y_max = min(height, max(idx1[1], idx2[1]) + pad_voxels[1] + 1)
                x_min = max(0, min(idx1[0], idx2[0]) - pad_voxels[0])
                x_max = min(width, max(idx1[0], idx2[0]) + pad_voxels[0] + 1)
                
                log(f"    [Via Aerea] Bounding Box: Z:[{z_min}, {z_max}], Y:[{y_min}, {y_max}], X:[{x_min}, {x_max}]")
                
                z_indices = np.arange(z_min, z_max)
                y_indices = np.arange(y_min, y_max)
                x_indices = np.arange(x_min, x_max)
                
                z_phys = origin[2] + z_indices * spacing[2]
                y_phys = origin[1] + y_indices * spacing[1]
                x_phys = origin[0] + x_indices * spacing[0]
                
                Z_grid = z_phys[:, np.newaxis, np.newaxis]
                Y_grid = y_phys[np.newaxis, :, np.newaxis]
                X_grid = x_phys[np.newaxis, np.newaxis, :]
                
                D = p2_phys - p1_phys
                L2 = np.sum(D**2)
                
                if L2 > 1e-6:
                    dx = X_grid - p1_phys[0]
                    dy = Y_grid - p1_phys[1]
                    dz = Z_grid - p1_phys[2]
                    
                    t = (dx * D[0] + dy * D[1] + dz * D[2]) / L2
                    t_clamped = np.clip(t, 0.0, 1.0)
                    
                    proj_x = p1_phys[0] + t_clamped * D[0]
                    proj_y = p1_phys[1] + t_clamped * D[1]
                    proj_z = p1_phys[2] + t_clamped * D[2]
                    
                    dist2 = (X_grid - proj_x)**2 + (Y_grid - proj_y)**2 + (Z_grid - proj_z)**2
                    radius = float(os.environ.get("DENTALSEGMENTATOR_AIRWAY_RADIUS", "35"))
                    inside_cylinder = dist2 <= (radius**2)
                    
                    # Cut off elements outside the seeds' segment range
                    t_min = float(os.environ.get("DENTALSEGMENTATOR_AIRWAY_T_MIN", "-0.05"))
                    t_max = float(os.environ.get("DENTALSEGMENTATOR_AIRWAY_T_MAX", "1.05"))
                    inside_t = (t >= t_min) & (t <= t_max)
                    
                    mask_bbox = inside_cylinder & inside_t
                else:
                    dx = X_grid - p1_phys[0]
                    dy = Y_grid - p1_phys[1]
                    dz = Z_grid - p1_phys[2]
                    dist2 = dx**2 + dy**2 + dz**2
                    radius = float(os.environ.get("DENTALSEGMENTATOR_AIRWAY_RADIUS", "35"))
                    mask_bbox = dist2 <= (radius**2)
                
                cylinder_mask = np.zeros_like(internal_air)
                cylinder_mask[z_min:z_max, y_min:y_max, x_min:x_max] = mask_bbox
                internal_air = internal_air & cylinder_mask

            def find_airway_seed():
                if user_seed2_point is not None:
                    ret = []
                    s2 = find_nearest_air_voxel(user_seed2_point)
                    if s2:
                        ret.append(s2)
                    s1 = find_nearest_air_voxel(user_seed_point)
                    if s1:
                        ret.append(s1)
                    return ret
                elif user_seed_point is not None:
                    s1 = find_nearest_air_voxel(user_seed_point)
                    if s1:
                        return [s1]
                    return []

                candidates = []
                for z in range(depth):
                    slice_air = internal_air[z, :, :]
                    if not np.any(slice_air):
                        continue
                    labeled_s, n_f = ndi.label(slice_air, structure=struct2d)
                    for i in range(1, n_f + 1):
                        comp = (labeled_s == i)
                        area = int(np.sum(comp))
                        if not (50 < area < 10000):
                            continue
                        cy, cx = ndi.center_of_mass(comp)
                        cx, cy = float(cx), float(cy)
                        if not (0.25 * width < cx < 0.75 * width):
                            continue
                        if not (0.15 * height < cy < 0.85 * height):
                            continue
                        priority = abs(area - 800)
                        candidates.append((priority, z, int(cy), int(cx)))

                candidates.sort(key=lambda c: c[0])
                return [(z, y, x) for _, z, y, x in candidates[:10]]

            seeds = find_airway_seed()
            airway_mask = np.zeros_like(air_raw)

            if seeds:
                log(f"    [Via Aerea] {len(seeds)} semillas encontradas, iniciando crecimiento...")
                internal_air_sitk = sitk.GetImageFromArray(internal_air.astype(np.uint8))
                
                for z, y, x in seeds:
                    seed_sitk = (x, y, z)
                    grown_img = sitk.ConnectedThreshold(
                        internal_air_sitk, seedList=[seed_sitk], lower=1, upper=1
                    )
                    grown = sitk.GetArrayFromImage(grown_img) > 0
                    volume = int(np.sum(grown))

                    border_hit = (
                        np.any(grown[:, :, 0])  or np.any(grown[:, :, -1]) or
                        np.any(grown[:, 0, :])  or np.any(grown[:, -1, :])
                    )
                    if border_hit:
                        log(f"      -> Semilla ({x},{y},{z}) descartada por fuga lateral (vol={volume})")
                        continue

                    min_volume = 150 if (user_seed_point is not None or user_seed2_point is not None) else 1000
                    if volume < min_volume:
                        log(f"      -> Semilla ({x},{y},{z}) muy pequena (vol={volume}), descartando")
                        continue

                    log(f"      -> Semilla ({x},{y},{z}) exitosa! (vol={volume})")
                    airway_mask = grown
                    break
            else:
                log("    [Via Aerea] ADVERTENCIA: No se encontro semilla valida de via aerea.")

            # Limpiar ruidos pequeños desconectados de la via aerea final
            labeled_aw, n_aw = ndi.label(airway_mask, structure=struct3d)
            if n_aw > 1:
                target_lbl = 0
                for pt in [user_seed2_point, user_seed_point]:
                    if pt is not None:
                        x_s, y_s, z_s = pt
                        if 0 <= z_s < depth and 0 <= y_s < height and 0 <= x_s < width:
                            target_lbl = labeled_aw[z_s, y_s, x_s]
                            if target_lbl > 0:
                                break
                if target_lbl > 0:
                    airway_mask = labeled_aw == target_lbl
                else:
                    aw_counts = np.bincount(labeled_aw.ravel())
                    aw_counts[0] = 0
                    airway_mask = labeled_aw == aw_counts.argmax()

            log(f"    [Via Aerea] Voxeles de via aerea: {int(np.sum(airway_mask))}")
        else:
            airway_mask = None

        # Aplicar mascaras: primero via aerea (label=4), luego tejidos blandos (label=2)
        empty_mask = (out == 0)
        if airway_mask is not None:
            out[empty_mask & airway_mask] = 4
        if soft_tissue_mask is not None:
            out[empty_mask & soft_tissue_mask] = 2

    out_img = sitk.GetImageFromArray(out)
    out_img.CopyInformation(seg)
    sitk.WriteImage(out_img, str(output_path), True)
    if arr is not None and target not in ("mandible", "mandibula", "mandíbula", "maxilla", "maxilar",
                                          "via aerea", "via_aerea", "airway"):
        sidecar = write_upper_teeth_sidecar(conservative_mask(3), seg, output_path)
        if sidecar:
            log(f"Dientes superiores aparte: {sidecar}")
        lower = write_lower_teeth_sidecar(conservative_mask(4), seg, output_path)
        if lower:
            log(f"Dientes inferiores aparte: {lower}")


def same_physical_grid(image, reference):
    return (image.GetSize() == reference.GetSize()
            and np.allclose(image.GetSpacing(), reference.GetSpacing(), rtol=0, atol=1e-5)
            and np.allclose(image.GetOrigin(), reference.GetOrigin(), rtol=0, atol=1e-3)
            and np.allclose(image.GetDirection(), reference.GetDirection(), rtol=0, atol=1e-5))


def resample_label_array(moving, reference):
    """Nearest-neighbour labels of `moving` on the voxel grid of `reference`."""
    if same_physical_grid(moving, reference):
        return sitk.GetArrayFromImage(moving)
    resampler = sitk.ResampleImageFilter()
    resampler.SetReferenceImage(reference)
    resampler.SetInterpolator(sitk.sitkNearestNeighbor)
    resampler.SetDefaultPixelValue(0)
    return sitk.GetArrayFromImage(resampler.Execute(moving))


def replace_bone_with_totalsegmentator(dental, total, skull_id=TS_SKULL, mandible_id=TS_MANDIBLE,
                                       sinus_ids=TS_SINUS):
    """Skull and mandible come from TotalSegmentator. Teeth stay DentalSegmentator.

    `dental` uses DentalSegmentator ids (1 skull/maxilla, 2 mandible, 3 upper teeth, 4 lower teeth,
    5 canal). `total` uses craniofacial_structures ids. Sinuses, the head envelope and TotalSegmentator
    teeth are not bone. A voxel that is both skull and mandible is kept as mandible. The canal is not
    kept: where the mandible does not already cover it, it becomes mandible.
    """
    dental = np.asarray(dental)
    total = np.asarray(total)
    if dental.shape != total.shape:
        raise ValueError("TotalSegmentator y DentalSegmentator no comparten la cuadricula.")
    sinus = np.zeros(total.shape, dtype=bool)
    for label in sinus_ids:
        sinus |= total == label
    skull = (total == skull_id) & ~sinus
    mandible = (total == mandible_id) & ~sinus
    protected = (dental == 3) | (dental == 4)
    out = np.array(dental, copy=True)
    out[(out == 1) | (out == 2) | (out == 5)] = 0
    out[skull & ~protected] = 1
    out[mandible & ~protected] = 2
    out[(dental == 5) & ~protected & (out == 0)] = 2
    return out


def run_totalsegmentator_bone(nifti_path, output_file, device):
    """Skull and mandible from task craniofacial_structures.

    That task has no fast model and rejects roi_subset. It crops to the skull itself, then
    segments mandible, skull, teeth, sinuses and head. Only mandible and skull are kept later.
    """
    import importlib.util
    if importlib.util.find_spec("totalsegmentator") is None:
        raise RuntimeError(
            "Falta TotalSegmentator en el Python de la segmentacion. "
            "Instale en ese entorno: pip install TotalSegmentator"
        )
    ts_device = "cpu" if device == "cpu" else "gpu"
    log(f"25% Craneo y mandibula con TotalSegmentator ({ts_device}), por separado.")
    progress(25, "Hueso con TotalSegmentator")
    # A child process, so the temperature guard can pause it without freezing itself.
    code = (
        "from totalsegmentator.python_api import totalsegmentator\n"
        "totalsegmentator(%r, %r, ml=True, task='craniofacial_structures', device=%r, "
        "quiet=False, nr_thr_resamp=1, nr_thr_saving=1)\n"
        % (str(nifti_path), str(output_file), ts_device)
    )
    try:
        run_logged([sys.executable, "-c", code], progress_range=(25, 55),
                   stage="Hueso con TotalSegmentator", guard_gpu=(ts_device == "gpu"))
    except RuntimeError:
        if ts_device != "gpu":
            raise
        log("TotalSegmentator no cupo en la GPU. El hueso sigue en el procesador.")
        code = code.replace("device='gpu'", "device='cpu'")
        run_logged([sys.executable, "-c", code], progress_range=(25, 55),
                   stage="Hueso con TotalSegmentator en CPU")
    if not Path(output_file).is_file():
        raise RuntimeError("TotalSegmentator termino, pero no escribio el hueso.")


def run_dentalsegmentator_nnunet(input_path, output_path, target, seed=None, seed2=None):

    require_imports()
    dataset_dir = ensure_weights(default_model_dir())
    results_root = dataset_dir.parent

    work_dir = Path(output_path).resolve().parent / "dentalsegmentator_work"
    if work_dir.exists():
        shutil.rmtree(work_dir)
    images_dir = work_dir / "imagesTs"
    pred_dir = work_dir / "predictions"
    raw_dir = work_dir / "nnUNet_raw"
    prep_dir = work_dir / "nnUNet_preprocessed"
    images_dir.mkdir(parents=True, exist_ok=True)
    pred_dir.mkdir(parents=True, exist_ok=True)
    raw_dir.mkdir(parents=True, exist_ok=True)
    prep_dir.mkdir(parents=True, exist_ok=True)

    log("20% Convirtiendo el TAC...")
    image = sitk.ReadImage(str(input_path))
    case_input = images_dir / "case_0000.nii.gz"
    sitk.WriteImage(image, str(case_input), True)

    env = os.environ.copy()
    env["nnUNet_results"] = str(results_root)
    env.setdefault("nnUNet_raw", str(raw_dir))
    env.setdefault("nnUNet_preprocessed", str(prep_dir))

    device = env.get("DENTALSEGMENTATOR_DEVICE", "auto").strip().lower() or "auto"
    if device == "auto":
        import torch
        device = "cuda" if torch.cuda.is_available() else "cpu"
    log(f"Python: {sys.executable}")

    bone_from_totalsegmentator = False
    bone_source = os.environ.get("DENTALSEGMENTATOR_BONE_SOURCE", "dentalsegmentator").strip().lower()
    if target.lower() not in ("teeth", "dientes") and bone_source == "totalsegmentator":
        ts_path = pred_dir / "totalsegmentator_craniofacial.nii.gz"
        run_totalsegmentator_bone(case_input, ts_path, device)
        bone_from_totalsegmentator = True

    log(f"58% Dientes y tejido blando con DentalSegmentator ({device}).")
    cmd = [
        nnunet_predict_executable(),
        "-i", str(images_dir),
        "-o", str(pred_dir),
        "-d", env.get("DENTALSEGMENTATOR_DATASET_ID", DATASET_ID),
        "-c", env.get("DENTALSEGMENTATOR_CONFIGURATION", CONFIGURATION),
        "-f", env.get("DENTALSEGMENTATOR_FOLDS", FOLDS),
        "-device", device,
        # One case: extra preprocessing/export workers only add process start-up (torch import) and RAM.
        "-npp", env.get("DENTALSEGMENTATOR_NPP", "1"),
        "-nps", env.get("DENTALSEGMENTATOR_NPS", "1"),
    ]
    step_size = env.get("DENTALSEGMENTATOR_STEP_SIZE", "").strip()
    if step_size:
        cmd += ["-step_size", step_size]
    # Mirroring runs the network several times and holds the GPU at full load. Off unless asked.
    if env.get("DENTALSEGMENTATOR_DISABLE_TTA", "1").strip() != "0":
        cmd.append("--disable_tta")
    run_logged(cmd, env=env, progress_range=(58, 88), stage="Dientes con DentalSegmentator",
               guard_gpu=(device != "cpu"))
    log("88% Exportando dientes...")

    prediction_path = pred_dir / "case.nii.gz"
    if not prediction_path.exists():
        alternatives = [path for path in pred_dir.glob("*.nii.gz")
                        if path.name != "totalsegmentator_craniofacial.nii.gz"]
        if alternatives:
            prediction_path = alternatives[0]
    if not prediction_path.exists():
        raise RuntimeError("nnU-Net termino, pero no genero prediccion NIfTI.")

    if bone_from_totalsegmentator:
        dental_img = sitk.ReadImage(str(prediction_path))
        merged = replace_bone_with_totalsegmentator(
            sitk.GetArrayFromImage(dental_img),
            resample_label_array(sitk.ReadImage(str(ts_path)), dental_img),
        )
        merged_img = sitk.GetImageFromArray(np.ascontiguousarray(merged.astype(np.uint8)))
        merged_img.CopyInformation(dental_img)
        combined_path = pred_dir / "bone_totalsegmentator.nii.gz"
        sitk.WriteImage(merged_img, str(combined_path), True)
        prediction_path = combined_path
        log("92% Craneo y mandibula de TotalSegmentator; dientes de DentalSegmentator. Sin canal.")

    log("93% Remapeando etiquetas a la app...")
    remap_prediction(input_path, prediction_path, output_path, target, seed, seed2,
                     bone_from_totalsegmentator=bone_from_totalsegmentator)
    if not Path(output_path).exists():
        raise RuntimeError("No se genero labelmap NRRD de salida.")
    log(f"100% Labelmap guardado: {output_path}")


def expand_command(template, input_path, output_path, target):
    parts = shlex.split(template)
    has_placeholders = any(
        "{input}" in p or "{output}" in p or "{target}" in p for p in parts
    )
    parts = [
        p.replace("{input}", input_path)
        .replace("{output}", output_path)
        .replace("{target}", target)
        for p in parts
    ]
    if not has_placeholders:
        parts.extend([input_path, output_path])
    return parts


def run_command(command):
    process = open_logged_process(command)
    with process.stdout:
        for line in process.stdout:
            log(line.rstrip())
    return process.wait()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("input")
    parser.add_argument("output")
    parser.add_argument("--target", default="hueso")
    parser.add_argument("--seed", default=None)
    parser.add_argument("--seed2", default=None)
    args = parser.parse_args()

    input_path = os.path.abspath(args.input)
    output_path = os.path.abspath(args.output)
    target = args.target
    if not os.path.exists(input_path):
        raise RuntimeError(f"No existe input: {input_path}")

    os.makedirs(os.path.dirname(output_path), exist_ok=True)

    # Bypass nnU-Net entirely for airway/via aerea segmentation
    if target.lower() in ("via aerea", "via_aerea", "airway"):
        log("20% Segmentando via aerea sin nnU-Net...")
        remap_prediction(input_path, None, output_path, target, args.seed, args.seed2)
        if not os.path.exists(output_path):
            raise RuntimeError("No se genero labelmap de via aerea.")
        log(f"100% Labelmap de via aerea guardado: {output_path}")
        return

    cli = os.environ.get("DENTALSEGMENTATOR_CLI", "").strip()
    if cli:
        log(f"20% Ejecutando DENTALSEGMENTATOR_CLI sin Slicer para {target}...")
        code = run_command(expand_command(cli, input_path, output_path, target))
        if code != 0:
            raise RuntimeError(f"DENTALSEGMENTATOR_CLI fallo con codigo {code}")
        if not os.path.exists(output_path):
            raise RuntimeError("El CLI termino, pero no genero el labelmap de salida.")
        log(f"100% Labelmap guardado: {output_path}")
        return

    module_name = os.environ.get("DENTALSEGMENTATOR_MODULE", "").strip()
    if module_name:
        log(f"20% Ejecutando modulo Python {module_name} sin Slicer para {target}...")
        code = run_command(
            [sys.executable, "-m", module_name, input_path, output_path, "--target", target]
        )
        if code != 0:
            raise RuntimeError(f"Modulo {module_name} fallo con codigo {code}")
        if not os.path.exists(output_path):
            raise RuntimeError("El modulo termino, pero no genero el labelmap de salida.")
        log(f"100% Labelmap guardado: {output_path}")
        return

    run_dentalsegmentator_nnunet(input_path, output_path, target, args.seed, args.seed2)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        log("ERROR: " + str(exc))
        traceback.print_exc()
        sys.exit(1)
