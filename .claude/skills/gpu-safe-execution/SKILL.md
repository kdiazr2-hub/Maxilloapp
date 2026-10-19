---
name: gpu-safe-execution
description: "Use before anything in MaxilloApp could put load on the GPU or CUDA — DentalSegmentator / nnU-Net or TotalSegmentator inference, torch on CUDA, training, benchmarks, or a test that imports torch — and whenever the user asks about GPU temperature, power or a hung inference. Sets the safety policy: explicit authorisation first, one inference at a time, a low-power mode, a configurable and conservative temperature/power limit with a verified safe stop, never touching the system. NOT the segmentation method itself (that is `medical-image-segmentation`), NOT Python code conventions (that is `python-medical-imaging`)."
tags: [gpu, cuda, safety, thermal, nnunet, totalsegmentator, maxilloapp]
recommends: [medical-image-segmentation, python-medical-imaging]
origin: maxilloapp
---

# gpu-safe-execution — la GPU se usa solo con permiso y con límites

La segmentación de MaxilloApp corre en el portátil del cirujano (Windows, RTX 3050 de portátil con gráficos
híbridos, entorno conda `dentalgpu`). Una inferencia de nnU-Net o TotalSegmentator calienta la GPU durante
minutos. Esta skill fija **cuándo** se puede cargar la GPU y **cómo** se para con seguridad.
Las sesiones en la nube no tienen GPU: ahí esta skill solo aplica a lo que se escribe o se propone.

## 1. Reglas que no se negocian

1. **Sin autorización explícita no hay carga de GPU.** No ejecutar inferencia (DentalSegmentator, nnU-Net,
   TotalSegmentator), entrenamiento, benchmarks ni pruebas de carga sin que el usuario lo pida en ese turno,
   nombrando la tarea. Un "sí" anterior no cubre una tarea nueva.
2. **El diagnóstico es estático.** Primero se leen el código, los logs, las máscaras ya guardadas y las
   pruebas unitarias (ver §6). Nunca se lanza un segmentador "para ver qué pasa".
3. **Una sola inferencia a la vez.** Nunca dos procesos de inferencia en paralelo, ni uno de la app y otro
   de la terminal. Antes de lanzar, comprobar que no hay otro (ver §4).
4. **Nunca tocar el sistema.** Prohibido: `nvidia-smi -pl`, `-lgc`, `-pm`, `-r`; cambiar controladores,
   CUDA, el entorno conda, planes de energía, variables de sistema persistentes, el registro o la BIOS;
   apagar, reiniciar o suspender el equipo. Solo **lecturas** de `nvidia-smi`. Las variables de entorno
   se ponen solo para el proceso hijo que se lanza.
5. **No afirmar lo que no se ha comprobado.** "Cancelado", "parado" o "sin carga" se dicen solo después
   de verificarlo (ver §5). Si no se pudo verificar, se dice así.
6. **No cargar volúmenes completos en GPU si no hace falta.** Recortar a la región de interés (ROI),
   preferir CPU para pre y posprocesado (scipy/SimpleITK son CPU), y usar la GPU solo para la red.

## 2. Límites configurables (conservadores por defecto)

No todas las RTX 3050 son iguales: la versión de portátil va de unos 35 W a 95 W según el fabricante, y su
temperatura objetivo depende del equipo. **No asumir un máximo**: el límite lo pone el usuario y, si no lo
pone, se usan valores conservadores muy por debajo de cualquier límite de fábrica.

| Variable (solo para el proceso que se lanza) | Por defecto | Significado |
|---|---|---|
| `MAXILLO_GPU_TEMP_LIMIT_C` | `75` | Temperatura a la que se para |
| `MAXILLO_GPU_TEMP_RESUME_C` | límite − 10 | Temperatura bajo la que se puede **proponer** reanudar |
| `MAXILLO_GPU_POWER_LIMIT_W` | 80 % del `power.limit` que reporta `nvidia-smi` | Potencia a la que se para |
| `MAXILLO_GPU_SAMPLE_S` | `5` | Intervalo de muestreo |
| `MAXILLO_GPU_TRIP_SAMPLES` | `3` | Muestras seguidas sobre el límite para disparar la parada |
| `MAXILLO_LOW_POWER` | `1` | Modo de bajo consumo activado |

Si `nvidia-smi` no responde o no da temperatura, **no se lanza** la tarea de GPU: sin lectura no hay
vigilancia. Se informa y se ofrece CPU (`DENTALSEGMENTATOR_DEVICE=cpu`).

## 3. Modo de bajo consumo (por defecto)

Para DentalSegmentator, solo con las variables que el script ya lee
(`scripts/run_standalone_dental_segmentator.py`, `run_dentalsegmentator_nnunet`):

- `DENTALSEGMENTATOR_NPP=1`, `DENTALSEGMENTATOR_NPS=1`: un proceso de preprocesado y otro de exportación.
- `DENTALSEGMENTATOR_DISABLE_TTA=1`: sin test-time augmentation; menos pasadas de la red.
- `DENTALSEGMENTATOR_STEP_SIZE=0.7`: menos ventanas solapadas. Es algo menos preciso, y hay que avisarlo.
- `DENTALSEGMENTATOR_DEVICE=cpu`: alternativa sin GPU, mucho más lenta pero sin carga térmica de la GPU.

Para TotalSegmentator, la integración está en el equipo del usuario y **no** en este repositorio
(a fecha 2026-10-19). Sus opciones de bajo consumo, por ejemplo modo rápido, ROI, número de hilos o
dispositivo, **se confirman leyendo el código instalado**, por ejemplo el `argparse` del paquete. No se
confirman ejecutándolo, ni siquiera con `--help`. Nunca se inventa un flag.

## 4. Antes de lanzar (lista de comprobación)

1. El usuario autorizó **esta** tarea en **este** turno.
2. No hay otra inferencia viva:
   `nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv,noheader` sin procesos de
   python / nnUNet / TotalSegmentator, y la app no tiene una segmentación en curso
   (`StandaloneDentalSegmentatorService` mantiene un solo `QProcess`).
3. La GPU está fría, por debajo del límite de reanudación:
   `nvidia-smi --query-gpu=temperature.gpu,power.draw,power.limit,utilization.gpu,clocks_throttle_reasons.active --format=csv,noheader,nounits`
4. Modo de bajo consumo puesto, salvo que el usuario pida lo contrario.
5. El volumen está recortado a la ROI; no se manda el cráneo entero si basta el maxilar.

## 5. Vigilancia y parada segura

Un vigilante de **solo lectura** muestrea `nvidia-smi` cada `MAXILLO_GPU_SAMPLE_S` segundos mientras dura la
tarea. Si `MAXILLO_GPU_TRIP_SAMPLES` muestras seguidas superan el límite de temperatura o de potencia:

1. **No lanzar nada pesado nuevo.** La cola se congela.
2. **Pedir la parada de la tarea en curso** por el camino propio: el botón cancelar de la app, o la
   terminación del árbol de procesos que ya hace el script (`kill_process_tree`, el job object de Windows
   `attach_kill_on_close_job`). Nunca se mata un proceso que no se lanzó en esta tarea.
3. **Verificar la parada** antes de decir nada:
   - el PID ya no existe (`tasklist /FI "PID eq <pid>"` en Windows, `ps -p <pid>` en Linux);
   - `--query-compute-apps` ya no lo lista;
   - `utilization.gpu` baja y la temperatura empieza a descender en las muestras siguientes.

   Solo entonces se dice «tarea detenida». Si algo de esto falla, se dice exactamente qué sigue vivo.
4. **Enfriar.** No se propone reanudar hasta que la temperatura esté por debajo de
   `MAXILLO_GPU_TEMP_RESUME_C`. Reanudar necesita **una nueva autorización** del usuario.
5. **Informar**: temperatura y potencia máximas vistas, cuándo saltó el límite, qué se paró, qué quedó a
   medias (por ejemplo `dentalsegmentator_work/` sin predicción), y que no se tocó la configuración del sistema.

Ejemplo de vigilante, PowerShell 7 (el operador `??` no existe en Windows PowerShell 5.1), solo lectura; una línea por muestra y sale al terminar el proceso. `$pid_tarea` es el PID del proceso lanzado en esta tarea:

```powershell
$limit = [int]($env:MAXILLO_GPU_TEMP_LIMIT_C ?? 75); $over = 0
while (Get-Process -Id $pid_tarea -ErrorAction SilentlyContinue) {
  $t,$p,$pl = (nvidia-smi --query-gpu=temperature.gpu,power.draw,power.limit --format=csv,noheader,nounits).Split(',') | % { [double]$_ }
  $wlimit = [double]($env:MAXILLO_GPU_POWER_LIMIT_W ?? (0.8 * $pl))
  if ($t -ge $limit -or $p -ge $wlimit) { $over++ } else { $over = 0 }
  "$(Get-Date -f HH:mm:ss) T=$t C P=$p W (limites $limit C / $wlimit W) sobre=$over"
  if ($over -ge [int]($env:MAXILLO_GPU_TRIP_SAMPLES ?? 3)) { "LIMITE: pedir parada"; break }
  Start-Sleep -Seconds ([int]($env:MAXILLO_GPU_SAMPLE_S ?? 5))
}
```

## 6. Diagnóstico sin GPU (siempre primero)

- Código: `scripts/run_standalone_dental_segmentator.py`, `src/StandaloneDentalSegmentatorService.cpp`,
  `src/MeshGenerator.cpp`.
- Logs: la salida que la app guarda de la segmentación (cada línea `NN% ...` del script).
- Máscaras ya guardadas: el labelmap NRRD de salida, el sidecar `<salida>_dientes_superiores.nrrd`, la
  caché `.mha` en `%LOCALAPPDATA%`.
- Pruebas: `tests/test_bone_segmentation.py`, que es CPU puro, con `CUDA_VISIBLE_DEVICES=""` para garantizar que
  no toca la GPU.

## 7. Criterios de validación de esta skill

- [ ] Ninguna orden de inferencia, entrenamiento o benchmark sin autorización explícita del turno.
- [ ] Ninguna orden que escriba configuración de GPU o del sistema (`-pl`, `-lgc`, `-pm`, `-r`, reinicio).
- [ ] Nunca dos inferencias a la vez; comprobado con `--query-compute-apps` antes de lanzar.
- [ ] Límites leídos de variables con valores por defecto conservadores; ningún máximo de fábrica asumido.
- [ ] Toda afirmación de parada viene con su verificación (PID, `compute-apps`, tendencia térmica).
