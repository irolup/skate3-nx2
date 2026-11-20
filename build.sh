#!/bin/sh
# Builds the NRO inside the ounce-devkit Docker image (https://github.com/ouncebrew/ounce-devkit):
#   ./build.sh            -> build/skate3.nro
#   ./build.sh -j8        extra arguments go to ninja (keep -j low: the recompiled code needs
#                         about 2 GB of RAM per job)
ROOT="$(cd "$(dirname "$0")" && pwd)"
IMAGE="${OUNCE_DEVKIT_IMAGE:-ounce-devkit}"
exec docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT:$ROOT" -w "$ROOT" "$IMAGE" sh -c '
. /opt/devkitpro/switch2vars.sh
[ -f build/build.ninja ] || aarch64-none-elf-cmake -S . -B build -G Ninja >/dev/null || exit 1
cmake --build build -- "$@"' sh "$@"
