#!/bin/sh
# Build and run the host-side tests (PC compiler, not the ARM toolchain).
# Each protocol is tested against its own simulated device; the simulators both stand in
# for icsp.c, so they are built into separate programs. Each program runs its protocol
# against every device table row that uses it.
set -e
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
flags="-std=c11 -Wall -Wextra -Werror -Wshadow -Wconversion -Iinclude -Itests"

python3 tools/gen_devices.py data/pic_devices.csv "$out/pic_devices.c"
core="src/pic.c src/proto/icsp8.c src/proto/p16_8bit.c src/proto/p18_8bit.c $out/pic_devices.c"

gcc $flags $core tests/sim_p16_8bit.c tests/test_p16_8bit.c -o "$out/test_p16_8bit"
"$out/test_p16_8bit"

gcc $flags $core tests/sim_p18_8bit.c tests/test_p18_8bit.c -o "$out/test_p18_8bit"
"$out/test_p18_8bit"

# Unity-based tests (vendored in tests/vendor/unity). pic_find_device needs no ICSP, but
# pic.c references the icsp_* pins elsewhere, so the p16 simulator stands in for icsp.c.
gcc $flags -Itests/vendor/unity $core tests/sim_p16_8bit.c tests/test_pic_find.c \
    tests/vendor/unity/unity.c -o "$out/test_pic_find"
"$out/test_pic_find"
