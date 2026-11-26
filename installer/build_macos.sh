#!/usr/bin/env bash
# Build the macOS .app of the Russian-menu updater with PyInstaller.
#
# Runs the same way locally and on a GitHub macOS runner: it makes a venv,
# installs the pinned deps, runs the installer test suite, then produces a
# windowed .app and zips it into outputs/macos/. No firmware is bundled —
# the app downloads BINs from the project's GitHub releases or opens a local
# file the user picks.
set -euo pipefail
cd "$(dirname "$0")/.."           # repo root
ROOT="$(pwd)"
# Never reuse the CLT Python/Tk 8.5 environment: it can build successfully
# while producing an empty window on current macOS.
CHECK="$ROOT/installer/check_macos_runtime.py"
VENV="$ROOT/work/installer-macos-venv"
PY="$VENV/bin/python"
if [ -n "${PYTHON:-}" ]; then
    "$PYTHON" "$CHECK"
    BUILD_PYTHON="$PYTHON"
else
    BUILD_PYTHON=""
    for candidate in python3 /opt/homebrew/bin/python3.11 /opt/homebrew/bin/python3 /usr/local/bin/python3; do
        if command -v "$candidate" >/dev/null 2>&1 && "$candidate" "$CHECK" >/dev/null 2>&1; then
            BUILD_PYTHON="$candidate"
            break
        fi
    done
    if [ -z "$BUILD_PYTHON" ]; then
        echo 'A Python with Tcl/Tk >= 8.6 is required. Apple CLT Python is unsupported.' >&2
        echo 'Install Python from python.org or brew install python-tk@3.11.' >&2
        echo 'Then run: PYTHON=/path/to/python3 bash installer/build_macos.sh' >&2
        exit 1
    fi
fi
if [ ! -x "$PY" ] || ! "$PY" "$CHECK" >/dev/null 2>&1 || \
   [ "$("$PY" -c 'import sys; print(sys._base_executable)')" != "$("$BUILD_PYTHON" -c 'import sys; print(sys._base_executable)')" ]; then
    "$BUILD_PYTHON" -m venv --clear "$VENV"
fi
"$PY" "$CHECK"
"$PY" -m pip install --quiet --upgrade pip
"$PY" -m pip install --quiet -r installer/requirements.txt

echo "== Tests =="
# run_tests.py runs the full suite then exits hard, so a macOS Tk/hidapi
# teardown hang can never stall an otherwise-green build.
"$PY" installer/run_tests.py

VERSION="$("$PY" -c "import sys;sys.path.insert(0,'installer');from version import VERSION;print(VERSION)")"
NAME="XREAL-Russian-Updater-$VERSION"
DEST="$ROOT/outputs/macos"
rm -rf "$DEST" "$ROOT/work/installer-build" "$ROOT/work/$NAME.spec"
mkdir -p "$DEST"

echo "== PyInstaller ($NAME) =="
"$PY" -m PyInstaller --noconfirm --windowed --noupx \
    --name "$NAME" \
    --osx-bundle-identifier "com.davnozdu.xreal.russian.updater" \
    --paths work --paths installer \
    --hidden-import hid \
    --distpath "$DEST" \
    --workpath "$ROOT/work/installer-build" \
    --specpath "$ROOT/work" \
    installer/app.py

# Smoke-test the bundled app: the self-test maps the real GUI and parses a BIN
# committed in the repo, writes a JSON report, and performs zero firmware
# writes. This proves PyInstaller bundled every module (backend, hid, tkinter,
# updates) before we ship the .app. No device and no network are needed.
APP_BIN="$DEST/$NAME.app/Contents/MacOS/$NAME"
REPORT="$DEST/selftest.json"
SMOKE_BIN="$ROOT/outputs/xreal-pilot2-real3d-cpu/XREAL-Pilot2-RU-Hebe-Real3D-CPU.bin"
# hidapi's IOHIDManager enumeration can, rarely, hit a GIL race on macOS; retry
# a couple of times so that flake never fails an otherwise-good build.
for attempt in 1 2 3; do
    if "$APP_BIN" --self-test "$REPORT" --bin "$SMOKE_BIN"; then break; fi
    echo "self-test attempt $attempt failed; retrying"; sleep 2
    [ "$attempt" = 3 ] && { echo "self-test failed 3 times"; exit 1; }
done
"$PY" -c "import json,sys;d=json.load(open(sys.argv[1]));assert d['ok'] and d['firmware_writes']==0 and d['ui_visible'] and tuple(map(int,d['tk_version'].split('.')[:2])) >= (8,6),d;print('self-test ok:',d['version'])" "$REPORT"
rm -f "$REPORT"

cp installer/MACOS-README.md "$DEST/README.md"
( cd "$DEST" && /usr/bin/ditto -c -k --sequesterRsrc --keepParent "$NAME.app" "$NAME-macos.zip" )
echo "Built: $DEST/$NAME.app  ->  $DEST/$NAME-macos.zip"
