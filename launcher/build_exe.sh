#!/bin/sh
# Build the one-file launcher exe and put it in the repository folder:
#   Mat Hoffman's Pro BMX 2 Launcher.exe
# Needs Python 3 with Tk and PyInstaller (python -m pip install pyinstaller).
set -e
cd "$(dirname "$0")"
${PYTHON:-python} -m PyInstaller --onefile --windowed --noconfirm --clean --name "Mat Hoffman's Pro BMX 2 Launcher" \
  --distpath dist --workpath ../work/launcher_pyi --specpath ../work/launcher_pyi mh2launcher.py
cp "dist/Mat Hoffman's Pro BMX 2 Launcher.exe" ..
echo "built $(cd .. && pwd)/Mat Hoffman's Pro BMX 2 Launcher.exe"
