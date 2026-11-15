@echo off
setlocal
cd /d "%~dp0"
set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM21"
set "PY=%USERPROFILE%\.platformio\penv\Scripts\python.exe"
if not exist "%PY%" (
  echo ERROR: PlatformIO Python not found: %PY%
  pause
  exit /b 1
)
"%PY%" "%~dp0tools\app_only_flash.py" "%PORT%"
if errorlevel 1 (
  echo UPLOAD FAILED OR STOPPED SAFELY
  pause
  exit /b 1
)
echo APP-ONLY upload successful.
pause
