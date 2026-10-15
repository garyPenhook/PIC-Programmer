#!/bin/sh
# clang-tidy over the firmware sources, using the ARM compile database. Needs a configured
# build: cmake --preset Debug
set -e
cd "$(dirname "$0")/.."
db=build/Debug
[ -f "$db/compile_commands.json" ] || { echo "run: cmake --preset Debug" >&2; exit 1; }
sysroot=$(arm-none-eabi-gcc -print-sysroot 2>/dev/null || true)
clang-tidy -p "$db" --quiet \
    --extra-arg=--target=arm-none-eabi --extra-arg=-mcpu=cortex-m0plus \
    ${sysroot:+--extra-arg=--sysroot="$sysroot"} \
    src/*.c src/proto/*.c "$@"
