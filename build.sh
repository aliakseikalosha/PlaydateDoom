#!/bin/sh
# Usage: ./build.sh sim|device [Release|Debug]
set -e
cd "$(dirname "$0")"
TYPE="${2:-Release}"
case "$1" in
  device) mkdir -p build-device && cd build-device && cmake .. -DCMAKE_TOOLCHAIN_FILE="$PLAYDATE_SDK_PATH/C_API/buildsupport/arm.cmake" -DTOOLCHAIN=armgcc -DCMAKE_BUILD_TYPE="$TYPE" && make ;;
  sim|"") mkdir -p build-sim && cd build-sim && cmake .. -DCMAKE_BUILD_TYPE="$TYPE" && make ;;
  *) echo "usage: $0 sim|device [Release|Debug]"; exit 1 ;;
esac
