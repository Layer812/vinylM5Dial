@echo off
setlocal EnableExtensions
cd /d "%~dp0"
where py >nul 2>nul
if errorlevel 1 (
 echo ERROR: Python launcher py.exe unavailable.
 echo Use Python 3 and Pillow to regenerate images.
 exit /b 1
)
py -3 tools\build_assets.py
if errorlevel 1 (
 echo ERROR: Requires Pillow. Install using: py -3 -m pip install Pillow
 exit /b 1
)
echo Label assets regenerated. Now run BUILD_CMD.cmd
exit /b 0
