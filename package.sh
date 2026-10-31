#!/bin/bash
# Assemble a ready-to-copy package for the PS Vita:
#   <out>/rof.vpk
#   <out>/ux0/data/rof/{libAppMain.so, assets/, com.snkplaymore.android009/, save/}
# Usage: bash package.sh [APK] [SONGS_DIR] [SAVE_DIR] [OUT]
set -e
cd "$(dirname "$0")"
SRC=../rhythm-of-fighters
APK=${1:-$SRC/ROF.v.1.4.1.b.10.crk.Support.apk}
SONGS=${2:-$SRC/Songs/com.snkplaymore.android009}
SAVE=${3:-"$SRC/DATA ROOT Save/com.snkplaymore.android009"}
OUT=${4:-../vita_package}
PKG=com.snkplaymore.android009
DATA="$OUT/ux0/data/rof"

[ -f build/rof.vpk ] || { echo "build/rof.vpk not found - run build.sh first"; exit 1; }

mkdir -p "$DATA/assets" "$DATA/$PKG"
cp build/rof.vpk "$OUT/"

echo "* libAppMain.so"
unzip -o -q -j "$APK" lib/armeabi/libAppMain.so -d "$DATA"

echo "* assets"
TMP=$(mktemp -d)
unzip -o -q "$APK" 'assets/*' -d "$TMP"
cp -r "$TMP/assets/." "$DATA/assets/"
rm -rf "$TMP"

echo "* songs / downloaded data"
cp -r "$SONGS/." "$DATA/$PKG/"

# Players start a new game; set INCLUDE_SAVE=1 to ship a save as well.
if [ "${INCLUDE_SAVE:-0}" = 1 ]; then
  echo "* save data"
  mkdir -p "$DATA/save"
  for f in save.dat tunes.dat res.dat login.dat; do
    [ -f "$SAVE/$f" ] && cp "$SAVE/$f" "$DATA/save/"
  done
fi

cp LEIA-ME.txt README.txt "$OUT/" 2>/dev/null || true
du -sh "$OUT"
echo "Done: $OUT"
