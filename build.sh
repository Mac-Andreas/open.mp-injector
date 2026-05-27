#!/bin/bash
# Build injector.exe (Win32, x86) on macOS using MSVC under CrossOver/Wine.
#
# Prereqs (same as the SA-MP/open.mp build toolchain):
#   - CrossOver with a win64 bottle (default: "Rockstar Games Launcher")
#   - MSVC + Windows SDK fetched via msvc-wine into $MSVC_DIR (default /tmp/msvc)
# Override via env: CX_BOTTLE, MSVC_DIR, MSVCVER, SDKVER.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/src/injector.c"
OUT="$HERE/build/injector.exe"
mkdir -p "$HERE/build"

CX="/Applications/CrossOver.app/Contents/SharedSupport/CrossOver"
WINE="$CX/CrossOver-Hosted Application/wine"
export CX_BOTTLE="${CX_BOTTLE:-Rockstar Games Launcher}"
export WINEDEBUG=-all
MSVC_DIR="${MSVC_DIR:-/tmp/msvc}"
MSVCVER="${MSVCVER:-14.51.36231}"
SDKVER="${SDKVER:-10.0.26100.0}"

win() { printf 'Z:%s' "$(echo "$1" | sed 's#/#\\#g')"; }

RSP="$(mktemp -u).rsp"
{
  echo '/nologo /MT /O2 /W3'
  printf '"%s"\n' "$(win "$SRC")"
  printf '/Fe:"%s"\n' "$(win "$OUT")"
  echo '/link kernel32.lib user32.lib psapi.lib'
} > "$RSP"

BAT="$(mktemp -u).bat"
cat > "$BAT" <<EOF
@echo off
set MSVCROOT=$(win "$MSVC_DIR")\\VC\\Tools\\MSVC\\$MSVCVER
set KITS=$(win "$MSVC_DIR")\\Windows Kits\\10
set INCLUDE=%MSVCROOT%\\include;%KITS%\\Include\\$SDKVER\\ucrt;%KITS%\\Include\\$SDKVER\\shared;%KITS%\\Include\\$SDKVER\\um
set LIB=%MSVCROOT%\\lib\\x86;%KITS%\\Lib\\$SDKVER\\ucrt\\x86;%KITS%\\Lib\\$SDKVER\\um\\x86
set PATH=%MSVCROOT%\\bin\\Hostx64\\x86;%PATH%
cl @$(win "$RSP")
echo CL=%ERRORLEVEL%
EOF

"$WINE" cmd /c "$(win "$BAT")" 2>&1 | grep -vE 'msync|Wow64|prefix|bootstrapped|up and running'
rm -f "$RSP" "$BAT"
file "$OUT" 2>/dev/null && echo "built: $OUT"
