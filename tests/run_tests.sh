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
core="src/pic.c src/proto/icsp8.c src/proto/p16_8bit.c src/proto/p18_8bit.c src/proto/p18_q10.c $out/pic_devices.c"

gcc $flags $core tests/sim_p16_8bit.c tests/test_p16_8bit.c -o "$out/test_p16_8bit"
"$out/test_p16_8bit"

gcc $flags $core tests/sim_p18_8bit.c tests/test_p18_8bit.c -o "$out/test_p18_8bit"

gcc $flags $core tests/sim_p18_q10.c tests/test_p18_q10.c -o "$out/test_p18_q10"
"$out/test_p18_q10"
"$out/test_p18_8bit"

# Unity-based tests (vendored in tests/vendor/unity). pic_find_device needs no ICSP, but
# the protocol code in $core calls the icsp_* functions, so the p16 simulator stands in
# for icsp.c.
gcc $flags -Itests/vendor/unity $core tests/sim_p16_8bit.c tests/test_pic_find.c \
    tests/vendor/unity/unity.c -o "$out/test_pic_find"
"$out/test_pic_find"

# Serial command layer, against the p16 simulator with a stub UART (and no display).
gcc $flags -Itests/vendor/unity $core src/cmd.c src/ui.c src/oled.c src/selftest.c src/bridge.c \
    tests/sim_p16_8bit.c tests/stub_icsp_lines.c tests/stub_board.c tests/stub_uart.c tests/stub_i2c.c tests/test_cmd.c tests/vendor/unity/unity.c \
    -o "$out/test_cmd"
"$out/test_cmd"

# OLED driver and status screen, against a stub I2C bus.
gcc $flags -Itests/vendor/unity $core src/oled.c src/ui.c tests/sim_p16_8bit.c \
    tests/stub_i2c.c tests/test_oled.c tests/vendor/unity/unity.c -o "$out/test_oled"
"$out/test_oled"

# The same, built as for a module that never acknowledges (I2C_IGNORE_ACK).
gcc $flags -DI2C_IGNORE_ACK=1 -Itests/vendor/unity $core src/oled.c src/ui.c \
    tests/sim_p16_8bit.c tests/stub_i2c.c tests/test_oled.c tests/vendor/unity/unity.c \
    -o "$out/test_oled_ignore_ack"
"$out/test_oled_ignore_ack"

# ICSP wiring self-test, against a model of the three wires.
gcc $flags -Itests/vendor/unity src/selftest.c tests/stub_icsp_lines.c tests/test_selftest.c \
    tests/vendor/unity/unity.c -o "$out/test_selftest"
"$out/test_selftest"

# Serial bridge, against stub PC and target ports.
gcc $flags -Itests/vendor/unity src/bridge.c tests/stub_board.c tests/stub_uart.c \
    tests/test_bridge.c tests/vendor/unity/unity.c -o "$out/test_bridge"
"$out/test_bridge"

# Host tool (host/picprog) against the PC build of the firmware's command layer.
if command -v uv >/dev/null 2>&1; then
    uv run -q --project host pytest -q host/tests
elif python3 -c "import pytest" 2>/dev/null; then
    python3 -m pytest -q host/tests
else
    echo "neither uv nor pytest found: host tool tests skipped"
fi
