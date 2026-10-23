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
SOURCES = ["src/pic.c", "src/proto/icsp8.c", "src/proto/p16_8bit.c", "src/proto/p18_8bit.c", "src/proto/p18_q10.c", "src/proto/p18_k42.c",
           "src/cmd.c", "src/ui.c", "src/oled.c", "src/selftest.c", "src/bridge.c",
           "tests/stub_board.c",
           "tests/stub_icsp_lines.c", "tests/stub_i2c.c",
           "tests/host_sim_main.c"]


@pytest.fixture(scope="session")
def host_sim_builds(tmp_path_factory):
    """Build of the PC firmware per protocol, made on first use: protocol -> binary path."""
    out = tmp_path_factory.mktemp("host_sim")
    table = out / "pic_devices.c"
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "gen_devices.py"),
                    devices.CSV_PATH, str(table)], check=True)
    builds = {}

    def build(protocol):
        if protocol not in builds:
            sim = {"p16_8bit": "p16_8bit", "p18_8bit": "p18_8bit", "p18_q10": "p18_q10",
                   "p18_k42": "p18_k42"}[protocol]
            binary = out / f"host_sim_{protocol}"
            subprocess.run(["gcc", *FLAGS, "-Iinclude", "-Itests", f'-DSIM_HEADER="sim_{sim}.h"',
                            *[os.path.join(ROOT, s) for s in SOURCES],
                            os.path.join(ROOT, "tests", f"sim_{sim}.c"), str(table),
                            "-o", str(binary)], check=True, cwd=ROOT)
            builds[protocol] = str(binary)
        return builds[protocol]
    return build


@pytest.fixture(scope="session")
def host_sim(host_sim_builds):
    """Path of the built PC firmware (the PIC16F171XX model)."""
    return host_sim_builds("p16_8bit")


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


def spawn_target(binary, device_id):
    proc = subprocess.Popen([binary, device_id], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, bufsize=0)
    return proc, Link(PipeStream(proc))


@pytest.fixture
def target(host_sim, request):
    """A Link to a fresh simulated part (PIC16F17146 unless a test asks for another ID)."""
    device_id = getattr(request, "param", "30E1")
    proc = subprocess.Popen([host_sim, device_id], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, bufsize=0)
    yield Link(PipeStream(proc))
    proc.stdin.close()
    proc.wait(timeout=5)


PDBG_SOURCES = ["target/pdbg/pdbg.c", "tests/pdbg_fake_target.c", "tests/pdbg_sim_main.c"]


@pytest.fixture(scope="session")
def pdbg_sim(tmp_path_factory):
    """Path of the PC build of the debug agent (target/pdbg) on a fake part, stdin/stdout as
    the UART."""
    binary = tmp_path_factory.mktemp("pdbg_sim") / "pdbg_sim"
    subprocess.run(["gcc", *FLAGS, "-Itarget/pdbg", "-Itests",
                    *[os.path.join(ROOT, s) for s in PDBG_SOURCES], "-o", str(binary)],
                   check=True, cwd=ROOT)
    return str(binary)


@pytest.fixture
def spawn_agent(pdbg_sim):
    """spawn_agent(*sim_args, timeout=5.0) -> (Pdbg, process); the process is closed at the end."""
    from picprog.debug.pdbg import Pdbg
    procs = []

    def spawn(*args, timeout=5.0):
        proc = subprocess.Popen([pdbg_sim, *args], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, bufsize=0)
        procs.append(proc)
        return Pdbg(PipeStream(proc, timeout)), proc
    yield spawn
    for proc in procs:
        proc.stdin.close()
        proc.wait(timeout=5)


@pytest.fixture
def agent(spawn_agent):
    """A Pdbg talking to a fresh simulated agent."""
    return spawn_agent()[0]
