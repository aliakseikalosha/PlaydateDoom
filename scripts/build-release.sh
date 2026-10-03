#!/bin/bash
# Usage: scripts/build-release.sh [both|device|sim]     (default: both)
#
# Builds a release .pdx at release/doom.pdx that bundles only the shareware /
# freely redistributable WADs listed in scripts/release-wads.txt. The normal
# build bundles every WAD in Source/wad/ and the music rendered from all of
# them, so this stages its own Source folder instead: the WADs from the list,
# plus music rendered from just those WADs.
#
# "both" (default) gives one .pdx that runs on the device and in the simulator.
set -e
cd "$(dirname "$0")/.."

: "${PLAYDATE_SDK_PATH:?PLAYDATE_SDK_PATH is not set}"

TARGETS="${1:-both}"
case "$TARGETS" in
  both|device|sim) ;;
  *) echo "usage: $0 [both|device|sim]"; exit 1 ;;
esac

LIST=scripts/release-wads.txt
STAGE=build-release/Source
OUT=release/doom.pdx

# Release builds of the game itself (each also bundles a full, all-WADs .pdx
# next to the repo; that one is only a by-product and is not used here).
if [ "$TARGETS" != sim ]; then ./build.sh device Release; fi
if [ "$TARGETS" != device ]; then ./build.sh sim Release; fi

rm -rf build-release "$OUT"
mkdir -p "$STAGE/wad" "$STAGE/music" release

# Everything in Source/ except the WADs, the music and the game binaries.
for f in Source/*; do
  case "$f" in
    Source/wad|Source/music|Source/pdex.*) ;;
    *) cp -R "$f" "$STAGE/" ;;
  esac
done
if [ "$TARGETS" != sim ]; then cp Source/pdex.elf "$STAGE/"; fi
if [ "$TARGETS" != device ]; then cp Source/pdex.dylib "$STAGE/"; fi

# The WADs allowed in a release.
count=0
while IFS= read -r line; do
  name=$(echo "${line%%#*}" | xargs)
  [ -n "$name" ] || continue
  if [ -f "Source/wad/$name" ]; then
    cp "Source/wad/$name" "$STAGE/wad/"
    echo "Including $name"
    count=$((count + 1))
  else
    echo "Skipping $name (not in Source/wad/)"
  fi
done < "$LIST"

if [ "$count" -eq 0 ]; then
  echo "None of the WADs in $LIST were found in Source/wad/."
  exit 1
fi

# Music for the included WADs only.
cc -O2 -o build-release/musrender tools/musrender.c -lm
build-release/musrender "$STAGE/wad" "$STAGE/music"

"$PLAYDATE_SDK_PATH/bin/pdc" -sdkpath "$PLAYDATE_SDK_PATH" "$STAGE" "$OUT"

echo
echo "Release build: $OUT ($(du -sh "$OUT" | cut -f1))"
echo "WADs:  $(ls "$OUT/wad" | tr '\n' ' ')"
echo "Music: $(find "$OUT/music" -type f | wc -l | xargs) tracks"
