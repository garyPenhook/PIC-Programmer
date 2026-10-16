#!/usr/bin/env python3
"""Sweep the I2C pull-up simulation (docs/sim/i2c_pullup.cir) with ngspice.

Usage: sim_i2c_pullup.py [output.md]

For each external pull-up value and bus capacitance, runs ngspice in batch mode and
records the low level, the rise times and the fall time, then writes a Markdown table
(to stdout, or to the given file). The firmware also enables the STM32's internal pull-up on SCL and SDA,
so every case includes it in parallel (typical 40 kOhm, DS12232 Table 51).
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
NETLIST = os.path.join(HERE, "..", "docs", "sim", "i2c_pullup.cir")

R_INTERNAL = 40e3              # STM32 weak pull-up, typical (25 to 55 kOhm)
R_EXTERNAL = [None, 10e3, 4.7e3, 2.2e3]
C_BUS = [20e-12, 50e-12, 100e-12, 200e-12]
TR_MAX = 300e-9                # SSD1309 Table 13-6, for both rise and fall
VOL_MAX = 0.2 * 3.3            # SSD1309 VIL = 0.2 VDD (Table 12-1)


def parallel(a, b):
    return a * b / (a + b)


def run(r, c):
    with open(NETLIST) as f:
        text = f.read()
    text = re.sub(r"^\.param .*$", f".param RPU={r:.6g} CBUS={c:.6g}", text, count=1, flags=re.M)
    with tempfile.NamedTemporaryFile("w", suffix=".cir", delete=False) as f:
        f.write(text)
        path = f.name
    try:
        out = subprocess.run(["ngspice", "-b", path], capture_output=True, text=True,
                             check=True).stdout
    finally:
        os.unlink(path)
    values = {}
    for name in ("vol", "tr_ssd", "tr_stm", "tf_ssd"):
        m = re.search(rf"^{name}\s*=\s*([-+0-9.eE]+)", out, flags=re.M)
        if m is None:
            sys.exit(f"ngspice output has no {name}:\n{out}")
        values[name] = float(m.group(1))
    return values


def ns(seconds):
    return f"{seconds * 1e9:.0f} ns"


def main():
    rows = ["| External pull-up | Effective | C bus | VOL | Rise 20-80 % | Rise 30-70 % "
            "| Fall 80-20 % | Within 300 ns |",
            "|---|---|---|---|---|---|---|---|"]
    for rext in R_EXTERNAL:
        r = R_INTERNAL if rext is None else parallel(rext, R_INTERNAL)
        for c in C_BUS:
            v = run(r, c)
            ok_ssd = v["tr_ssd"] <= TR_MAX
            ok_stm = v["tr_stm"] <= TR_MAX
            verdict = "both" if ok_ssd else ("30-70 % only" if ok_stm else "no")
            if v["tf_ssd"] > TR_MAX:
                verdict += ", fall too slow"
            if v["vol"] > VOL_MAX:
                verdict += ", VOL too high"
            label = "none (internal only)" if rext is None else f"{rext / 1e3:g} kOhm"
            rows.append(f"| {label} | {r / 1e3:.2f} kOhm | {c * 1e12:.0f} pF | {v['vol']:.3f} V "
                        f"| {ns(v['tr_ssd'])} | {ns(v['tr_stm'])} | {ns(v['tf_ssd'])} | {verdict} |")
    table = "\n".join(rows) + "\n"
    if len(sys.argv) > 1:
        with open(sys.argv[1], "w") as f:
            f.write(table)
    else:
        sys.stdout.write(table)


if __name__ == "__main__":
    main()
