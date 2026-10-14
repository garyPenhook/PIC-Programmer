#!/bin/sh
# Build and run the host-side tests (PC compiler, not the ARM toolchain).
# Each driver is tested against its own simulated device; the simulators both stand in
# for icsp.c, so they are built into separate programs.
set -e
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
flags="-std=c11 -Wall -Wextra -Werror -Wshadow -Wconversion -Iinclude -Itests"

gcc $flags src/pic16f171.c tests/sim_pic16f171.c tests/test_pic16f171.c -o "$out/test_pic16f171"
"$out/test_pic16f171"

gcc $flags src/pic18q.c tests/sim_pic18q.c tests/test_pic18q.c -o "$out/test_pic18q"
"$out/test_pic18q"
