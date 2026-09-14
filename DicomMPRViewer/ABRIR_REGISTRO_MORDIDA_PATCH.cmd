@echo off
setlocal

set "APP_DIR=%~dp0"
set "RELEASE_DIR=%APP_DIR%build\Release"
set "PATCHED_EXE=%APP_DIR%build\CodexRelease\DicomMPRViewer.exe"

if not exist "%PATCHED_EXE%" (
    echo No se encontro la version parcheada:
    echo "%PATCHED_EXE%"
    pause
    exit /b 1
)

set "PATH=%RELEASE_DIR%;%PATH%"
start "" "%PATCHED_EXE%"
