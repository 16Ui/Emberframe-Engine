@echo off
setlocal
cd /d "%~dp0"
if not exist "%~dp0emberframe_workbench.exe" (
    echo Missing emberframe_workbench.exe. Extract the entire package first.
    pause
    exit /b 1
)
if "%~1"=="" (
    "%~dp0emberframe_workbench.exe" --portable --showcase materials --fit-viewport
) else (
    "%~dp0emberframe_workbench.exe" --portable --fit-viewport %*
)
set "emberExit=%errorlevel%"
if not "%emberExit%"=="0" (
    echo EmberFrame exited with code %emberExit%. See START_HERE.txt and the user log.
    pause
)
exit /b %emberExit%
