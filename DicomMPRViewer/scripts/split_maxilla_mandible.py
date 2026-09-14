#!/usr/bin/env python3
"""
split_maxilla_mandible.py — Bone splitter (Maxilar / Mandíbula)

Reads a segmentation labelmap (NRRD) and splits the bone label (1) into:
  Label 5  →  Maxilar   (upper jaw / maxilla)
  Label 6  →  Mandíbula (lower jaw / mandible)

All other labels are preserved unchanged.

Algorithm
---------
Primary  : scipy connected-component labeling (6-connectivity) → classify
           each component by its superior/inferior centroid position.
Fallback : Z-profile minimum — finds the slice with fewest bone voxels
           between the two bone masses and splits there.

Requirements: numpy, scipy (recommended)

Usage:
    python split_maxilla_mandible.py <input.nrrd> <output.nrrd>
"""

import sys
import os
import re
import struct
import argparse
import traceback


# ──────────────────────────────────────────────────────────────────────────────
def log(msg):
    print(msg, flush=True)


# ──────────────────────────────────────────────────────────────────────────────
# Minimal NRRD reader / writer for the exact format written by NrrdVolumeExporter
# (raw, little-endian, 3-D, one scalar component).
# ──────────────────────────────────────────────────────────────────────────────
_DTYPE_MAP = {
    "uchar":        "u1", "unsigned char":  "u1",
    "char":         "i1", "signed char":    "i1",
    "ushort":       "u2", "unsigned short": "u2",
    "short":        "i2",
    "uint":         "u4", "unsigned int":   "u4",
    "int":          "i4",
    "float":        "f4",
    "double":       "f8",
}


def _parse_nrrd_header(filepath):
    """Return (header_dict, data_offset_bytes) for a raw NRRD file."""
    header = {}
    with open(filepath, "rb") as f:
        offset = 0
        prev_was_newline = False
        lines = []
        while True:
            line_bytes = b""
            while True:
                ch = f.read(1)
                if not ch:
                    raise ValueError("Unexpected EOF in NRRD header")
                if ch == b"\n":
                    break
                line_bytes += ch
            line = line_bytes.decode("utf-8", errors="replace").strip()
            offset += len(line_bytes) + 1   # +1 for the newline
            if line == "":
                data_offset = offset
                break
            lines.append(line)

    for line in lines:
        if line.startswith("#"):
            continue
        m = re.match(r"^([^:=]+)[=:](.*)$", line)
        if m:
            header[m.group(1).strip().lower()] = m.group(2).strip()

    return header, data_offset


def read_nrrd(filepath):
    """
    Returns (data, header).
    data.shape = (nz, ny, nx)  — i.e. data[z, y, x] = label value
    """
    import numpy as np

    header, data_offset = _parse_nrrd_header(filepath)

    if header.get("encoding", "raw").lower() != "raw":
        raise ValueError(f"Solo se admite encoding=raw (encontrado: {header.get('encoding')})")

    dtype_str = header.get("type", "short").lower()
    if dtype_str not in _DTYPE_MAP:
        raise ValueError(f"Tipo NRRD no reconocido: {dtype_str}")
    endian = "<" if header.get("endian", "little").lower() == "little" else ">"
    dtype = np.dtype(endian + _DTYPE_MAP[dtype_str])

    parts = header.get("sizes", "").split()
    if len(parts) < 3:
        raise ValueError(f"Header 'sizes' inválido: {header.get('sizes')}")
    nx, ny, nz = int(parts[0]), int(parts[1]), int(parts[2])

    with open(filepath, "rb") as f:
        f.seek(data_offset)
        raw = f.read()

    expected = nx * ny * nz * dtype.itemsize
    if len(raw) < expected:
        raise ValueError(f"Datos insuficientes: esperados {expected} bytes, encontrados {len(raw)}")

    data_flat = np.frombuffer(raw[:expected], dtype=dtype)
    # NrrdVolumeExporter writes: for z: for y: for x → C-order [z, y, x]
    data = data_flat.reshape(nz, ny, nx).copy()
    return data, header


def write_nrrd(filepath, data, header):
    """Write data (shape [nz, ny, nx]) as NRRD with the same metadata."""
    import numpy as np

    dtype = data.dtype
    inv_map = {v: k for k, v in _DTYPE_MAP.items()}
    kind = inv_map.get(dtype.str[1:], "short")  # strip endian char

    nz, ny, nx = data.shape
    sp_dirs = header.get("space directions", "(1,0,0) (0,1,0) (0,0,1)")
    sp_orig = header.get("space origin", "(0,0,0)")

    hdr = (
        "NRRD0005\n"
        "# Split by split_maxilla_mandible.py\n"
        f"type: {kind}\n"
        "dimension: 3\n"
        "space: left-posterior-superior\n"
        f"sizes: {nx} {ny} {nz}\n"
        f"space directions: {sp_dirs}\n"
        f"space origin: {sp_orig}\n"
        "kinds: domain domain domain\n"
        "encoding: raw\n"
        "endian: little\n"
        "\n"
    )

    os.makedirs(os.path.dirname(os.path.abspath(filepath)), exist_ok=True)
    with open(filepath, "wb") as f:
        f.write(hdr.encode("utf-8"))
        f.write(data.astype(np.dtype("<" + _DTYPE_MAP[kind]), copy=False).tobytes())


# ──────────────────────────────────────────────────────────────────────────────
def _sz_positive(header):
    """Return True if the z-axis space direction points superiorly (+Z in LPS)."""
    dirs = header.get("space directions", "")
    # Expect something like "(sx,0,0) (0,sy,0) (0,0,sz)"
    numbers = re.findall(r"[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?", dirs)
    if len(numbers) >= 9:
        sz = float(numbers[8])
        return sz >= 0
    return True   # safe default: assume standard axial orientation


# ──────────────────────────────────────────────────────────────────────────────
def split_with_scipy(bone_mask, sz_pos, header):
    """
    Primary algorithm: connected-component classification.
    Returns (maxilla_mask, mandible_mask) or None if classification fails.
    """
    import numpy as np
    try:
        from scipy import ndimage
    except ImportError:
        return None

    log("15% Etiquetando componentes conectadas (6-conectividad)...")
    struct = ndimage.generate_binary_structure(3, 1)    # 6-connectivity
    labeled, n = ndimage.label(bone_mask, structure=struct)
    log(f"25% {n} componentes encontradas")

    if n < 2:
        return None     # only one blob — fall through to z-profile split

    # Sort by descending size
    sizes = sorted(
        [(i + 1, int((labeled == i + 1).sum())) for i in range(n)],
        key=lambda t: -t[1]
    )

    # Require the second component to be at least 3% of the first
    if sizes[1][1] < sizes[0][1] * 0.03:
        log(f"25% Segunda componente demasiado pequeña ({sizes[1][1]} vox) — usando perfil Z")
        return None

    comp_a, comp_b = sizes[0][0], sizes[1][0]
    ca = ndimage.center_of_mass(bone_mask, labeled, comp_a)   # (z, y, x)
    cb = ndimage.center_of_mass(bone_mask, labeled, comp_b)
    za, zb = ca[0], cb[0]

    # higher z = superior = maxilla when sz > 0
    log("35% Clasificando maxilar / mandíbula por posición en Z...")
    if sz_pos:
        maxilla_id = comp_a if za > zb else comp_b
        mandible_id = comp_b if za > zb else comp_a
        split_z = (za + zb) / 2
    else:
        maxilla_id = comp_a if za < zb else comp_b
        mandible_id = comp_b if za < zb else comp_a
        split_z = (za + zb) / 2

    # Build masks; assign small extra components to nearest cluster
    maxilla_mask = labeled == maxilla_id
    mandible_mask = labeled == mandible_id

    for comp_id, _ in sizes[2:]:
        c = ndimage.center_of_mass(bone_mask, labeled, comp_id)
        region = labeled == comp_id
        if sz_pos:
            if c[0] >= split_z:
                maxilla_mask |= region
            else:
                mandible_mask |= region
        else:
            if c[0] <= split_z:
                maxilla_mask |= region
            else:
                mandible_mask |= region

    log("75% Componentes clasificadas")
    return maxilla_mask, mandible_mask


# ──────────────────────────────────────────────────────────────────────────────
def split_with_zprofile(bone_mask, sz_pos):
    """
    Fallback: find the Z-slice with the fewest bone voxels and split there.
    Returns (maxilla_mask, mandible_mask).
    """
    import numpy as np

    log("35% Calculando perfil de densidad en Z...")
    z_profile = bone_mask.sum(axis=(1, 2))   # shape (nz,)
    nonzero = np.where(z_profile > 0)[0]
    if len(nonzero) < 4:
        raise ValueError("Hueso insuficiente para dividir en Z")

    z_min, z_max = int(nonzero[0]), int(nonzero[-1])
    margin = max(1, (z_max - z_min) // 8)
    search_s = z_min + margin
    search_e = z_max - margin
    seg = z_profile[search_s : search_e + 1]

    if seg.size == 0:
        split_z = (z_min + z_max) // 2
    else:
        split_z = search_s + int(np.argmin(seg))

    log(f"60% Plano de separación en z={split_z}")

    below = np.zeros_like(bone_mask)
    above = np.zeros_like(bone_mask)
    below[:split_z, :, :] = bone_mask[:split_z, :, :]
    above[split_z:, :, :]  = bone_mask[split_z:, :, :]

    # higher z = superior = maxilla when sz_pos
    if sz_pos:
        return above, below    # maxilla above, mandible below
    else:
        return below, above


# ──────────────────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(description="Divide hueso en maxilar y mandíbula")
    parser.add_argument("input",  help="Labelmap de entrada (NRRD)")
    parser.add_argument("output", help="Labelmap de salida (NRRD)")
    args = parser.parse_args()

    try:
        import numpy as np
    except ImportError:
        log("ERROR: numpy no está instalado. Ejecute: pip install numpy scipy")
        sys.exit(1)

    # ── 1. Load ───────────────────────────────────────────────────────────────
    log("5% Leyendo labelmap...")
    data, header = read_nrrd(args.input)
    log(f"10% Volumen cargado: {data.shape[2]}×{data.shape[1]}×{data.shape[0]} voxeles")

    bone_mask = (data == 1)
    total_bone = int(bone_mask.sum())
    if total_bone == 0:
        log("ERROR: No se encontraron voxeles de hueso (label=1) en el labelmap")
        sys.exit(1)
    log(f"12% Voxeles de hueso: {total_bone:,}")

    sz_pos = _sz_positive(header)

    # ── 2. Split ──────────────────────────────────────────────────────────────
    result = split_with_scipy(bone_mask, sz_pos, header)
    if result is None:
        log("30% Usando método de perfil Z como alternativa...")
        result = split_with_zprofile(bone_mask, sz_pos)

    maxilla_mask, mandible_mask = result

    maxilla_count = int(maxilla_mask.sum())
    mandible_count = int(mandible_mask.sum())
    log(f"80% Maxilar: {maxilla_count:,} vox   Mandíbula: {mandible_count:,} vox")

    if maxilla_count == 0 or mandible_count == 0:
        log("ERROR: Una de las estructuras quedó vacía. "
            "Verifique que el labelmap contenga hueso facial completo.")
        sys.exit(1)

    # ── 3. Build output ───────────────────────────────────────────────────────
    log("85% Construyendo labelmap de salida...")
    out = data.copy()
    out[bone_mask]    = 0              # clear original bone label
    out[maxilla_mask] = 5              # maxilar
    out[mandible_mask] = 6             # mandíbula

    # ── 4. Write ──────────────────────────────────────────────────────────────
    log("90% Guardando resultado...")
    write_nrrd(args.output, out, header)
    log(f"100% Listo: {args.output}")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        log("ERROR: " + str(exc))
        traceback.print_exc()
        sys.exit(1)
