# DicomMPRViewer — Phase 1

DICOM MPR viewer for maxillofacial CT.  
Stack: **C++17 · Qt 6 · VTK 9 · CMake 3.20 · MSVC 2022 · Windows 10/11**

---

## Building

```powershell
# Configure
cmake -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH="C:/Qt/6.x.x/msvc2022_64;C:/vcpkg/installed/x64-windows" `
  -DVTK_DIR="C:/VTK/build" `
  -DUSE_GDCM=ON `
  -DGDCM_DIR="C:/vcpkg/installed/x64-windows/share/gdcm" `
  -DCMAKE_TOOLCHAIN_FILE="C:/vcpkg/scripts/buildsystems/vcpkg.cmake"

# Build
cmake --build build --config Release

# Deploy Qt DLLs (run once after build)
cd build\Release
windeployqt DicomMPRViewer.exe
```

---

## DICOM Load Flow

Opening a folder triggers a fully asynchronous pipeline — **the UI never freezes**:

```
MainWindow::loadVolume(folderPath)
  │
  ├─ (worker thread) AsyncDicomLoader::startLoad()
  │    │
  │    ├─ [1] DicomSeriesIndexer::indexFolder()
  │    │       Scans DICOM headers (no pixel decode).
  │    │       Groups files by SeriesInstanceUID.
  │    │       Sorts each group by ImagePositionPatient or InstanceNumber.
  │    │       Signal: statusChanged("Indexing DICOM...") -> progress 0%
  │    │
  │    ├─ [2] VolumeCacheManager::hasCachedVolume()
  │    │       Checks for a matching .mha + .json cache file.
  │    │       Signal: statusChanged("Checking cache...") -> progress 8%
  │    │
  │    ├─ [2a] Cache HIT -> vtkMetaImageReader reads .mha (seconds, not minutes)
  │    │        Signal: volumeReady -> progress 100%
  │    │
  │    └─ [2b] Cache MISS:
  │         ├─ [3] Build preview (every 4th slice, series >= 40 slices only)
  │         │       Fast coarse volume.  Signal: previewReady -> progress 10-28%
  │         │       MainWindow shows it so the user can start orienting.
  │         │
  │         ├─ [4] Load full volume (slice by slice, with progress callback)
  │         │       Signal: progressChanged(28->92%) live on each slice.
  │         │
  │         ├─ [5] VolumeCacheManager::saveToCache()
  │         │       Writes .mha (uncompressed) + .json metadata.
  │         │
  │         └─ Signal: volumeReady -> progress 100%
  │
  └─ (main thread) onVolumeReady() / onPreviewReady()
       Calls distributeVolume() -> MPRView::setVolume() on all three views.
       The same vtkImageData pointer is shared across axial / coronal / sagittal.
```

### Multi-series folders

If the folder contains more than one DICOM series (scout + main CT + derived), a
`SeriesSelectionDialog` appears.  The list is sorted by file count so the primary
CT series is pre-selected.

---

## Cache

### Formato elegido: MetaImage (.mha)

MetaImage was chosen because:
- Natively supported by VTK (`vtkMetaImageWriter / vtkMetaImageReader` from
  `VTK::IOImage`) — no extra dependency beyond what the project already needs.
- Sequential binary layout — sequential read saturates disk bandwidth.
- Stores all VTK scalar types (INT16, UINT16, FLOAT, …) without conversion.
- Single-file format (header + pixel data in the same `.mha` file).

### Ubicacion del cache

```
Windows: %LOCALAPPDATA%\MaxilloApp\DicomMPRViewer\DicomCache\
  e.g.   C:\Users\<user>\AppData\Local\MaxilloApp\DicomMPRViewer\DicomCache\
```

Two files per cached series:

| File | Contenido |
|------|-----------|
| `{md5(seriesUID)}.mha`  | Volume pixel data (uncompressed INT16) |
| `{md5(seriesUID)}.json` | Validation metadata |

### Validacion del cache

A cached volume is considered valid when ALL of the following match:

| Campo | Comparado con |
|-------|---------------|
| `seriesInstanceUid` | Current series UID |
| `fileCount`         | Number of DICOM files in folder |
| `totalFileSize`     | Sum of `QFileInfo::size()` for every file in the series |

If any field mismatches, the cache is ignored and the full DICOM load runs,
after which a fresh cache entry is written.

### Como limpiar el cache

**PowerShell (manual):**
```powershell
Remove-Item "$env:LOCALAPPDATA\MaxilloApp\DicomMPRViewer\DicomCache\*" -Force
```

**Programatico (C++):**
```cpp
VolumeCacheManager cache(m_cacheDir);
cache.clearCache();
```

---

## DICOM comprimido (JPEG2000 / JPEG-LS)

| Flag de compilacion | DICOM comprimido |
|---------------------|-----------------|
| `-DUSE_GDCM=OFF` (default) | Solo Transfer Syntaxes sin compresion |
| `-DUSE_GDCM=ON` | JPEG2000, JPEG-LS, RLE Lossless, JPEG Baseline/Extended |

Con `USE_GDCM=ON`:
- `DicomSeriesIndexer` usa `gdcm::Scanner` — lee headers de archivos comprimidos
  **sin** descomprimir pixeles.
- `DicomVolumeLoader::loadFromFilesGDCM` usa `gdcm::ImageReader` por slice —
  descomprime bajo demanda.
- El cache guarda el volumen ya descomprimido y reescalado a HU (INT16), por lo
  que las aperturas siguientes son siempre rapidas independientemente del
  Transfer Syntax original.

**Limitacion:** la descompresion GDCM es single-threaded y secuencial (un
`gdcm::ImageReader` por slice).  Para series grandes y comprimidas (>500 slices)
esto puede tomar 30-60 s en la primera apertura.  El preview (cada 4to slice)
se muestra en ~8 s mientras continua la carga completa.

---

## Vistas MPR

- Las tres vistas (**Axial, Coronal, Sagittal**) comparten el **mismo puntero
  `vtkImageData`**.  No hay duplicacion de datos.
- La navegacion por slices solo actualiza `vtkResliceImageViewer::SetSlice()`
  — el pipeline VTK completo **no** se re-ejecuta en cada frame.
- Los cambios de Window/Level se aplican via `SetColorWindow / SetColorLevel`
  — tambien sin re-ejecutar el pipeline.
- La sincronizacion del crosshair usa un **`vtkResliceCursor` compartido**
  propiedad del axial, referenciado por coronal y sagittal.

---

## Atajos de teclado

| Tecla | Accion |
|-------|--------|
| `Ctrl+O` | Abrir carpeta DICOM |
| `1` | Ventana Hueso (W:2000 / L:500) |
| `2` | Tejido Blando (W:400 / L:40) |
| `3` | Pulmon (W:1500 / L:-600) |
| `4` | Cerebro (W:80 / L:40) |
| `R` | Reset camara |
| `C` | Cursor normal |
| `D` | Medicion de distancia |
| `A` | Medicion de angulo |
| `N` | Anotacion |
| `Del` | Eliminar medicion seleccionada |
| `Esc` | Salir de pantalla completa |
| Doble-click en vista | Pantalla completa / normal |

---

## Resumen de clases

| Clase | Responsabilidad |
|-------|----------------|
| `DicomSeriesIndexer` | Scan de headers; agrupa y ordena archivos DICOM por serie |
| `DicomVolumeLoader` | Decodificacion de pixeles + reescalado HU; soporta `loadFromFiles()` con callback de progreso |
| `AsyncDicomLoader` | Worker QObject en QThread; orquesta index -> cache -> preview -> carga completa |
| `VolumeCacheManager` | Lectura/escritura de cache MetaImage con validacion JSON |
| `SeriesSelectionDialog` | Dialogo de seleccion para carpetas con multiples series |
| `MPRView` | Panel MPR individual: `QVTKOpenGLNativeWidget` + `vtkResliceImageViewer` + crosshair |
| `MainWindow` | Layout, menus, preset broadcast, tabla de mediciones |
| `MeasurementManager` | Almacen en memoria para mediciones |
| `MeasurementSerialization` | Guardar/cargar mediciones como JSON |
