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
# App labels:
#   5 Maxilar + dientes superiores, 6 Mandibula + dientes inferiores, 7 Canal mandibular
APP_LABEL_MAP = {1: 5, 2: 6, 3: 5, 4: 6, 5: 7}


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


def run_logged(command, env=None, progress_range=None, stage="Procesando"):
    log(" ".join([str(p) for p in command]))
    process = open_logged_process(command, env)
    job = attach_kill_on_close_job(process)
    last_percent = None

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


def thicken_thin_bone(mask, prediction, intensities, spacing_zyx, grow_mm=1.0, grow_hu=100.0,
                      seal_mm=2.0, seal_min_hu=-200.0):
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
    added = int(np.count_nonzero(result & ~mask))
    log(f"    [Hueso] Engrosado de paredes finas: {added} voxeles.")
    return result


def upper_teeth_sidecar_path(output_path):
    """Where the upper teeth go, next to the labelmap: <name>_dientes_superiores<ext>."""
    path = Path(output_path)
    for ext in (".nii.gz", ".nrrd", ".mha", ".nii"):
        if path.name.endswith(ext):
            return path.with_name(path.name[: -len(ext)] + "_dientes_superiores" + ext)
    return path.with_name(path.name + "_dientes_superiores.nrrd")


def write_upper_teeth_sidecar(teeth_mask, reference, output_path):
    """The upper teeth (DentalSegmentator label 3) on their own, for the guide's root analysis.

    The labelmap keeps them inside the maxilla (APP_LABEL_MAP 3 -> 5) so the Le Fort segment carries its
    teeth; this second file is the only place they are apart. Returns the path, or None without teeth.
    """
    import numpy as np
    import SimpleITK as sitk

    if teeth_mask is None or not np.any(teeth_mask):
        return None
    image = sitk.GetImageFromArray(teeth_mask.astype(np.uint8))
    image.CopyInformation(reference)
    path = upper_teeth_sidecar_path(output_path)
    sitk.WriteImage(image, str(path), True)
    return path


def remap_prediction(input_path, prediction_path, output_path, target, seed=None, seed2=None):
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

        # DentalSegmentator may keep very thin bracket/metal spikes. A tiny
        # binary opening + closing keeps anatomy while softening serrated masks.
        if label_value in (3, 4):
            open_radius = radius_env("DENTALSEGMENTATOR_TEETH_OPEN_RADIUS", 1)
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

        mask &= (arr == 0) | (arr == label_value)
        if label_value in (1, 2):
            image, intensities = source_image()

            def mm_env(name, default_value):
                try:
                    return max(0.0, float(os.environ.get(name, str(default_value)).strip()))
                except Exception:
                    return default_value

            mask = thicken_thin_bone(mask, arr, intensities, image.GetSpacing()[::-1],
                                     grow_mm=mm_env("DENTALSEGMENTATOR_BONE_GROW_MM", 1.0),
                                     grow_hu=float(os.environ.get("DENTALSEGMENTATOR_BONE_GROW_HU", "100")),
                                     seal_mm=mm_env("DENTALSEGMENTATOR_BONE_SEAL_MM", 2.0))
            mask = fill_bone_interiors(mask, arr, intensities, image.GetSpacing()[::-1], label_value)
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

    if target in ("completo", "segmentacion automatica", "auto", "via aerea", "via_aerea", "airway"):
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

        if target in ("completo", "segmentacion automatica", "auto", "via aerea", "via_aerea", "airway"):
            log("    [Tejidos Blandos] Generando mascara corporal sellada...")
            # Un pequeño closing en clean_body sella labios y nariz externamente
            # sin contraer ni aplastar los conductos de aire internos.
            closed_body = ndi.binary_closing(clean_body, structure=struct3d, iterations=3)
            solid_body = np.zeros_like(closed_body)
            for z in range(depth):
                solid_body[z] = ndi.binary_fill_holes(closed_body[z])
            
            if target in ("completo", "segmentacion automatica", "auto"):
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

    log("30% Convirtiendo NRRD a NIfTI para nnU-Net...")
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
    log(f"Python DentalSegmentator: {sys.executable}")
    log(f"40% Usando dispositivo nnU-Net: {device}")
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
    # Optional faster, slightly less accurate inference (nnU-Net defaults otherwise).
    step_size = env.get("DENTALSEGMENTATOR_STEP_SIZE", "").strip()
    if step_size:
        cmd += ["-step_size", step_size]
    if env.get("DENTALSEGMENTATOR_DISABLE_TTA", "").strip() == "1":
        cmd.append("--disable_tta")

    log("45% Ejecutando DentalSegmentator nnU-Net...")
    run_logged(cmd, env=env, progress_range=(45, 88), stage="Prediciendo con nnU-Net")
    log("88% Exportando la prediccion de nnU-Net...")

    prediction_path = pred_dir / "case.nii.gz"
    if not prediction_path.exists():
        alternatives = list(pred_dir.glob("*.nii.gz"))
        if alternatives:
            prediction_path = alternatives[0]
    if not prediction_path.exists():
        raise RuntimeError("nnU-Net termino, pero no genero prediccion NIfTI.")

    log("90% Remapeando etiquetas DentalSegmentator a la app...")
    remap_prediction(input_path, prediction_path, output_path, target, seed, seed2)
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
