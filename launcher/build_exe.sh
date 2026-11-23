#!/bin/sh
# Build the one-file launcher exe and put it next to the "Mat Hoffman 2" folder:
#   Mat Hoffman 2 Launcher.exe (in the repository root)
# Needs Python 3 with Tk and PyInstaller (python -m pip install pyinstaller).
set -e
cd "$(dirname "$0")"
${PYTHON:-python} -m PyInstaller --onefile --windowed --noconfirm --clean --name "Mat Hoffman 2 Launcher" \
  --distpath dist --workpath ../work/launcher_pyi --specpath ../work/launcher_pyi mh2launcher.py
cp "dist/Mat Hoffman 2 Launcher.exe" ..
echo "built $(cd .. && pwd)/Mat Hoffman 2 Launcher.exe"
