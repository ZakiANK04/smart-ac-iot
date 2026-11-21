@echo off
title Node-RED - Smart Air Conditioner
cd /d "%~dp0"

where node >nul 2>nul
if errorlevel 1 (
  echo [!] Node.js is not installed. Installing Node.js LTS with winget...
  winget install -e --id OpenJS.NodeJS.LTS --accept-source-agreements --accept-package-agreements
  echo.
  echo Node.js installed. Close this window and double-click this script again.
  pause
  exit /b
)

if not exist "%USERPROFILE%\.node-red" mkdir "%USERPROFILE%\.node-red"

where node-red >nul 2>nul
if errorlevel 1 (
  echo [1/3] Installing Node-RED ...
  call npm install -g --unsafe-perm node-red
)

if not exist "%USERPROFILE%\.node-red\node_modules\@flowfuse\node-red-dashboard" (
  echo [2/3] Installing Dashboard 2.0 ...
  pushd "%USERPROFILE%\.node-red"
  call npm install --no-fund --no-audit @flowfuse/node-red-dashboard
  popd
)

if not exist "%USERPROFILE%\.node-red\flows.json" (
  echo [3/3] Importing the Smart AC flow ...
  copy /y "%~dp0flows.json" "%USERPROFILE%\.node-red\flows.json" >nul
)

echo.
echo ============================================================
echo  Editor    : http://localhost:1880
echo  Dashboard : http://localhost:1880/dashboard
echo  (keep this window open while you use the dashboard)
echo ============================================================
call node-red
pause
