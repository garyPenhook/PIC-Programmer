"""Test fixtures: the firmware's command layer built for the PC, behind a Link.

tests/host_sim_main.c runs src/cmd.c against the PIC16F171XX simulator with stdin/stdout as
the UART, so these tests exercise the real command parser and protocol code end to end.
"""
import os
import select
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "host"))

from picprog import devices  # noqa: E402
from picprog.link import Link  # noqa: E402

FLAGS = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wshadow", "-Wconversion"]
SOURCES = ["src/pic.c", "src/proto/icsp8.c", "src/proto/p16_8bit.c", "src/proto/p18_8bit.c",
           "src/cmd.c", "src/ui.c", "src/oled.c", "src/selftest.c", "tests/sim_p16_8bit.c",
           "tests/stub_icsp_lines.c", "tests/stub_i2c.c",
           "tests/host_sim_main.c"]


@pytest.fixture(scope="session")
def host_sim(tmp_path_factory):
    """Path of the built PC firmware."""
    out = tmp_path_factory.mktemp("host_sim")
    table = out / "pic_devices.c"
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "gen_devices.py"),
                    devices.CSV_PATH, str(table)], check=True)
    binary = out / "host_sim"
    subprocess.run(["gcc", *FLAGS, "-Iinclude", "-Itests",
                    *[os.path.join(ROOT, s) for s in SOURCES], str(table), "-o", str(binary)],
                   check=True, cwd=ROOT)
    return str(binary)


class PipeStream:
    """read/write over a subprocess's pipes, with a timeout so a stalled protocol fails the
    test instead of hanging it."""

    def __init__(self, proc, timeout=5.0):
        self.proc = proc
        self.timeout = timeout

    def read(self, n):
        fd = self.proc.stdout.fileno()
        ready, _, _ = select.select([fd], [], [], self.timeout)
        if not ready:
            return b""
        return os.read(fd, n)

    def write(self, data):
        self.proc.stdin.write(data)

    def flush(self):
        self.proc.stdin.flush()


@pytest.fixture
def target(host_sim, request):
    """A Link to a fresh simulated part (PIC16F17146 unless a test asks for another ID)."""
    device_id = getattr(request, "param", "30E1")
    proc = subprocess.Popen([host_sim, device_id], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, bufsize=0)
    yield Link(PipeStream(proc))
    proc.stdin.close()
    proc.wait(timeout=5)
