@echo off
cd /d "%~dp0"
set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM21"
chcp 65001 >nul
pio device monitor -p %PORT% -b 115200
pause
