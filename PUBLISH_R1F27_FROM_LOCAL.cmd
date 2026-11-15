@echo off
setlocal EnableExtensions DisableDelayedExpansion
set "SRC=%~1"
if "%SRC%"=="" set "SRC=F:\build\tmp\VinylM5_Dial_R1"
if not exist "%SRC%\src\main.cpp" (
  echo ERROR: Source project not found: %SRC%
  exit /b 1
)
where git >nul 2>nul
if errorlevel 1 (
  echo ERROR: Git is required to upload the source.
  exit /b 1
)
set "DEST=%TEMP%\vinylm5dial_public_source"
if exist "%DEST%\.git" (
  git -C "%DEST%" pull --ff-only
  if errorlevel 1 exit /b 1
) else (
  git clone https://github.com/Layer812/vinylM5Dial.git "%DEST%"
  if errorlevel 1 exit /b 1
)
if not exist "%DEST%\src" mkdir "%DEST%\src"
copy /Y "%SRC%\src\main.cpp" "%DEST%\src\main.cpp" >nul
if errorlevel 1 exit /b 1
copy /Y "%SRC%\src\label_assets.h" "%DEST%\src\label_assets.h" >nul
if errorlevel 1 exit /b 1
if not exist "%DEST%\tools" mkdir "%DEST%\tools"
copy /Y "%SRC%\tools\app_only_flash.py" "%DEST%\tools\app_only_flash.py" >nul
copy /Y "%SRC%\tools\build_assets.py" "%DEST%\tools\build_assets.py" >nul
for %%F in (BUILD_CMD.cmd UPLOAD_CMD.cmd BUILD_AND_FLASH_CMD.cmd MONITOR_CMD.cmd MAKE_LABELS_CMD.cmd) do copy /Y "%SRC%\%%F" "%DEST%\%%F" >nul
if not exist "%DEST%\assets" mkdir "%DEST%\assets"
copy /Y "%SRC%\assets\labels.csv" "%DEST%\assets\labels.csv" >nul
rem Artwork is not automatically published: see COPYRIGHT_ARTWORK.md.
git -C "%DEST%" add src tools *.cmd assets\labels.csv
git -C "%DEST%" diff --cached --quiet
if not errorlevel 1 (
  echo Source files are already up to date.
  exit /b 0
)
git -C "%DEST%" commit -m "source: add VinylM5 Dial R1F27 development build"
if errorlevel 1 exit /b 1
git -C "%DEST%" push origin main
if errorlevel 1 exit /b 1
echo SUCCESS: Source uploaded to https://github.com/Layer812/vinylM5Dial
