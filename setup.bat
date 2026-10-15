@echo off
rem One-step setup on Windows: setup.bat C:\path\to\your\SSX-On-Tour-USA.iso (or .zip)
rem Needs only Python 3.10+ (python.org). Downloads the compiler and libraries into
rem third_party\win64 and builds dist\ssx-windows-x64 from your own UMD dump.
setlocal
cd /d "%~dp0"
if "%~1"=="" (
  echo usage: setup.bat ^<your ULUS-10042 dump: .iso or .zip^> [--no-build] [--force] [-j N]
  exit /b 1
)
if not exist .venv\Scripts\python.exe (
  py -3 -m venv .venv 2>nul || python -m venv .venv || (
    echo Python 3.10 or newer is required: https://www.python.org/downloads/
    exit /b 1
  )
  .venv\Scripts\python.exe -m pip install -q -r requirements.txt cmake ninja || exit /b 1
)
.venv\Scripts\python.exe tools\setup_from_iso.py %*
