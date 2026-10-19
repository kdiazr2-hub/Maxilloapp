# Compila MaxilloApp (Release) y la abre en modo de bajo consumo de GPU para segmentar un caso.
# Uso, desde la carpeta del repositorio (por ejemplo C:\MaxilloApp):
#   powershell -ExecutionPolicy Bypass -File DicomMPRViewer\tools\compilar_y_abrir.ps1
# -ExecutionPolicy Bypass vale solo para esta ejecucion: no cambia la configuracion de Windows.
# Las variables de bajo consumo se ponen solo para la app que se abre (skill gpu-safe-execution, seccion 3).
# Mensajes sin tildes: Windows PowerShell 5.1 lee los .ps1 sin BOM como ANSI.

$ErrorActionPreference = "Stop"
$repo = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$build = Join-Path $repo "DicomMPRViewer\build"
if (-not (Test-Path (Join-Path $build "CMakeCache.txt"))) {
    Write-Error "No existe DicomMPRViewer\build configurado. Configurelo primero (ver CLAUDE.md, 'Fresh configure')."
}

Write-Host "Compilando MaxilloApp (Release)..."
cmake --build $build --config Release --parallel
if ($LASTEXITCODE -ne 0) {
    Write-Error "La compilacion fallo (codigo $LASTEXITCODE). Copie el error y envielo."
}

$exe = Join-Path $build "Release\DicomMPRViewer.exe"
if (-not (Test-Path $exe)) {
    Write-Error "No se encontro $exe despues de compilar."
}

# Bajo consumo: un proceso de preparacion y otro de exportacion, sin pasadas extra (TTA) de la red.
$env:DENTALSEGMENTATOR_NPP = "1"
$env:DENTALSEGMENTATOR_NPS = "1"
$env:DENTALSEGMENTATOR_DISABLE_TTA = "1"

Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe)
Write-Host ""
Write-Host "MaxilloApp abierta en modo de bajo consumo. Segmente un solo caso a la vez."
Write-Host "Vigile la GPU en otra ventana (solo lectura, cada 5 s):"
Write-Host "  nvidia-smi --query-gpu=temperature.gpu,power.draw,power.limit,utilization.gpu --format=csv -l 5"
Write-Host "Si pasa de 75 C tres veces seguidas: cancele la segmentacion en la app."
