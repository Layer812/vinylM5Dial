@echo off
setlocal EnableExtensions
cd /d "%~dp0"
set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM21"
echo [1/2] Build R1F27. Flash is blocked unless the build succeeds.
call "%~dp0BUILD_CMD.cmd"
if errorlevel 1 (
  echo STOP: Build failed. No flash attempted.
  exit /b 1
)
echo [2/2] APP-ONLY upload to %PORT% after read-only partition verification.
call "%~dp0UPLOAD_CMD.cmd" %PORT%
if errorlevel 1 (
  echo STOP: Upload failed or safe-check refused.
  exit /b 1
)
echo R1F27 APP-ONLY uploaded successfully.
exit /b 0
