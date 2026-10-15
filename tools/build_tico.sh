#!/usr/bin/env bash
set -euo pipefail

# Builds tico-nethersx2.nro and tico-nethersx2-module.zip, the module tico
# installs into sdmc:/tico/modules/. Runs inside the ghcr.io/autorunhq/switch-dev
# image (devkitPro with the Horizon Mesa SDK in portlibs).
#
# The NetherSX2 cores and their data files come from the upstream
# NetherSX2_nx release NRO (UPSTREAM_NRO, downloaded when unset), never
# from this repository.

APP="$(cd "$(dirname "$0")/.." && pwd)"
JOBS=${JOBS:-$(nproc)}
DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
UPSTREAM_RELEASE=${UPSTREAM_RELEASE:-1.3.0}
# libretro-database's mirror of the Redump PS2 DAT, pinned
DAT_COMMIT=${DAT_COMMIT:-12c21cf79f1e20eff6ac68b80875816c717d0a7a}
MODULE_SRC="$APP/tico/module"
OUT="$APP/out"

export DEVKITPRO
export PATH="$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PATH"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tico-nethersx2.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

echo "==== upstream cores and data ===="
if [[ -z "${UPSTREAM_NRO:-}" ]]; then
  UPSTREAM_NRO="$WORK/NetherSX2.nro"
  curl -fsSL -o "$UPSTREAM_NRO" \
    "https://github.com/NaGaa95/NetherSX2_nx/releases/download/$UPSTREAM_RELEASE/NetherSX2.nro"
fi
python3 "$APP/tools/extract_nro_romfs.py" "$UPSTREAM_NRO" "$WORK/romfs" --only cores/ res/
for build in 4248 3668; do
  [[ -f "$WORK/romfs/cores/emucore_$build.so" && -f "$WORK/romfs/res/$build/GameIndex.yaml" ]] || {
    echo "The upstream NRO has no core $build" >&2
    exit 1
  }
  rm -rf "$WORK/romfs/res/$build/shaders/dx11"
done
# the settings definition, which the host applies to its prefs
mkdir -p "$WORK/romfs/module"
cp -f "$MODULE_SRC/settings.json" "$WORK/romfs/module/"

echo "==== storage dependencies ===="
DEPS_BUILD="$APP/launcher/dependencies/build"
cmake -S "$APP/launcher/dependencies" -B "$DEPS_BUILD" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$DEPS_BUILD" --parallel "$JOBS"

echo "==== tico-nethersx2 ===="
make -C "$APP" -j"$JOBS" TICO=1 ELF_LIB= \
  MESA_SDK_ROOT="$DEVKITPRO/portlibs/switch" \
  TICO_ROMFS="$WORK/romfs"

echo "==== module bundle ===="
MODULE_ID=$(sed -n 's/.*"id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$MODULE_SRC/module.json" | head -1)
MODULE_OUT="$WORK/module/$MODULE_ID"
mkdir -p "$MODULE_OUT/gamelists" "$OUT"
cp -r "$MODULE_SRC/." "$MODULE_OUT/"
cp -f "$APP/tico-nethersx2.nro" "$MODULE_OUT/"
curl -fsSL "https://raw.githubusercontent.com/libretro/libretro-database/$DAT_COMMIT/metadat/redump/Sony%20-%20PlayStation%202.dat" \
  | gzip -9 > "$MODULE_OUT/gamelists/ps2.dat.gz"
rm -f "$OUT/tico-$MODULE_ID-module.zip"
( cd "$WORK/module" && zip -qr "$OUT/tico-$MODULE_ID-module.zip" "$MODULE_ID" )
cp -f "$APP/tico-nethersx2.nro" "$APP/tico-nethersx2.elf" "$OUT/"
cp -f "$APP/build_tico/tico-nethersx2.map" "$OUT/" 2>/dev/null || true
# the SD card layout, for testing without the Store
rm -rf "$OUT/sd"
mkdir -p "$OUT/sd/tico/modules"
cp -r "$MODULE_OUT" "$OUT/sd/tico/modules/"

echo
ls -la "$OUT"
echo "The module extracts to sdmc:/tico/modules/$MODULE_ID/:"
find "$MODULE_OUT" -type f | sed "s|$WORK/module/|    |"
