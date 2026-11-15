@echo off
setlocal EnableExtensions DisableDelayedExpansion
set "SRC=%~1"
if "%SRC%"=="" set "SRC=F:\build\tmp\VinylM5_Dial_R1"
if not exist "%SRC%\src\main.cpp" (
  echo ERROR: Source project not found: %SRC%
  exit /b 1
)
findstr /C:"R1F27" "%SRC%\src\main.cpp" >nul
if errorlevel 1 (
  echo STOP: Source version is not R1F27. No GitHub changes.
  exit /b 1
)
where git >nul 2>nul
if errorlevel 1 (
  echo ERROR: Git command not found.
  exit /b 1
)
set "DEST=%TEMP%\vinylm5dial_public_source"
if exist "%DEST%\.git" (
  for /f "delims=" %%U in ('git -C "%DEST%" remote get-url origin') do set "REMOTE=%%U"
  if /I not "%REMOTE%"=="https://github.com/Layer812/vinylM5Dial.git" (
    echo STOP: Existing folder has unexpected Git remote.
    exit /b 1
  )
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
if errorlevel 1 exit /b 1
copy /Y "%SRC%\tools\build_assets.py" "%DEST%\tools\build_assets.py" >nul
if errorlevel 1 exit /b 1
for %%F in (UPLOAD_CMD.cmd BUILD_AND_FLASH_CMD.cmd MONITOR_CMD.cmd MAKE_LABELS_CMD.cmd) do (
  copy /Y "%SRC%\%%F" "%DEST%\%%F" >nul
  if errorlevel 1 exit /b 1
)
if not exist "%DEST%\assets" mkdir "%DEST%\assets"
copy /Y "%SRC%\assets\labels.csv" "%DEST%\assets\labels.csv" >nul
if errorlevel 1 exit /b 1
git -C "%DEST%" add src tools *.cmd assets/labels.csv
git -C "%DEST%" diff --cached --quiet
if not errorlevel 1 (
  echo GitHub source already matches the local project.
  exit /b 0
)
git -C "%DEST%" commit -m "source: publish VinylM5 Dial R1F27 experimental code"
if errorlevel 1 exit /b 1
git -C "%DEST%" push origin main
if errorlevel 1 exit /b 1
echo SUCCESS: https://github.com/Layer812/vinylM5Dial
