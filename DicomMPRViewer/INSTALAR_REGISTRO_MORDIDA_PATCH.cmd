@echo off
setlocal

set "APP_DIR=%~dp0"
set "PATCHED_EXE=%APP_DIR%build\CodexRelease\DicomMPRViewer.exe"
set "TARGET_EXE=%APP_DIR%build\Release\DicomMPRViewer.exe"

tasklist /FI "IMAGENAME eq DicomMPRViewer.exe" | find /I "DicomMPRViewer.exe" >nul
if not errorlevel 1 (
    echo Cierra DicomMPRViewer antes de instalar el parche.
    echo No se copiaron archivos.
    pause
    exit /b 1
)

if not exist "%PATCHED_EXE%" (
    echo No se encontro la version parcheada:
    echo "%PATCHED_EXE%"
    pause
    exit /b 1
)

copy /Y "%PATCHED_EXE%" "%TARGET_EXE%"
if errorlevel 1 (
    echo No se pudo copiar el parche.
    pause
    exit /b 1
)

echo Parche de Registro de Mordida instalado correctamente.
pause
