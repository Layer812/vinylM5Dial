@echo off
setlocal EnableExtensions
cd /d "%~dp0"
where pio >nul 2>nul
if errorlevel 1 (echo ERROR: PlatformIO command not found & exit /b 1)
set "FRAME=%USERPROFILE%\.platformio\packages\framework-arduinoespressif32\tools\pioarduino-build.py"
if not exist "%FRAME%" (
  call pio pkg install -e m5dial
  if errorlevel 1 exit /b 1
)
call pio run -e m5dial
if errorlevel 1 exit /b 1
echo BUILD PASSED - NO FLASH
exit /b 0
