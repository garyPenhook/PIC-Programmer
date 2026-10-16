"""picprog [--port PORT] <command> ...: drive the PIC programmer over its serial port."""
import argparse
import sys

from . import devices, ihex, programmer
from .link import Link, LinkError

DEFAULT_PORT = "/dev/ttyACM0"
DEFAULT_BAUD = 921600  # UART_BAUD in include/uart.h


def open_link(args):
    import serial  # pyserial; imported here so the rest works without it

    port = serial.Serial(args.port, args.baud, timeout=args.timeout)
    port.reset_input_buffer()
    return Link(port)


def progress(region, done, total):
    sys.stderr.write(f"\r{region}: {done}/{total}")
    if done >= total:
        sys.stderr.write("\n")
    sys.stderr.flush()


def warn(messages):
    for message in messages:
        print(f"warning: {message}", file=sys.stderr)


def cmd_probe(link, args):
    name, device_id, revision = link.probe()
    print(f"{name}  device ID 0x{device_id:04X}  revision ID 0x{revision:04X}")


def cmd_id(link, args):
    device = programmer.connect(link)
    device_id, revision = link.ids()
    print(f"{device.name}  device ID 0x{device_id:04X}  revision ID 0x{revision:04X}")


def cmd_dci(link, args):
    programmer.connect(link)
    for key, value in link.dci().items():
        print(f"{key}: {value}")


def cmd_read(link, args):
    programmer.connect(link)
    cells = link.read(args.region, args.offset, args.count)
    for i in range(0, len(cells), 8):
        row = " ".join(f"{c:04X}" for c in cells[i:i + 8])
        print(f"{args.offset + i:06X}: {row}")


def cmd_dump(link, args):
    device = programmer.connect(link)
    image, warnings = programmer.dump(link, device, progress=None if args.quiet else progress)
    with open(args.file, "w") as f:
        f.write(ihex.to_text(image))
    warn(warnings)
    print(f"{device.name}: wrote {len(image)} bytes to {args.file}")


def cmd_erase(link, args):
    programmer.connect(link)
    if args.row is not None:
        link.erase_row(args.region, args.row)
    else:
        link.erase("ALL" if args.region == "all" else args.region)
    print("erased")


def cmd_write(link, args):
    device = programmer.connect(link)
    with open(args.file) as f:
        image = ihex.parse(f.read())
    warnings = programmer.program(link, device, image, erase=not args.no_erase,
                                  verify=not args.no_verify,
                                  progress=None if args.quiet else progress)
    warn(warnings)
    print(f"{device.name}: programmed{'' if args.no_verify else ' and verified'}")


def cmd_crc(link, args):
    """Compare each region in a hex file by CRC-32, without reading the cells back."""
    device = programmer.connect(link)
    with open(args.file) as f:
        cells, _ = devices.image_to_cells(device, ihex.parse(f.read()))
    same = True
    for region in devices.REGIONS:
        if not cells[region]:
            continue
        want = devices.region_crc(device, region, cells[region])
        got = link.crc(region)
        same = same and want == got
        print(f"{region:6s} part {got:08X}  file {want:08X}  {'same' if want == got else 'DIFFERENT'}")
    if not same:
        raise LinkError("the part differs from the file")


def cmd_blank(link, args):
    device = programmer.connect(link)
    regions = devices.REGIONS if args.region == "all" else (args.region,)
    for region in regions:
        if device.size(region):
            print(f"{region}: blank ({link.blank(region)} cells)")


def cmd_selftest(link, args):
    states, shorts, ok = link.selftest()
    for name, state in states.items():
        print(f"{name:4s} {state}")
    for a, b in shorts:
        print(f"short between {a} and {b}")
    if not ok:
        raise LinkError("wiring fault (see include/selftest.h for what each result means)")
    print("wiring ok (an open wire or swapped CLK/DAT cannot be detected this way)")


def cmd_power(link, args):
    on, vdd = link.power(None if args.state is None else args.state == "on")
    print(f"target power {'on' if on else 'off'}, VDD {vdd} mV")


def cmd_vdd(link, args):
    target, own = link.vdd()
    print(f"target VDD {target} mV, programmer VDDA {own} mV")


def cmd_bridge(link, args):
    """Interactive terminal to the target's serial port. Ctrl-] ends it."""
    import os
    import select
    import termios
    import tty

    link.bridge_open(args.baud)
    print(f"bridge to the target at {args.baud} baud; Ctrl-] to end", file=sys.stderr)
    port = link.stream
    stdin = sys.stdin.fileno()
    saved = termios.tcgetattr(stdin) if os.isatty(stdin) else None
    try:
        if saved is not None:
            tty.setraw(stdin)
        while True:
            ready, _, _ = select.select([stdin, port.fileno()], [], [])
            if port.fileno() in ready:
                data = port.read(port.in_waiting or 1)
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
            if stdin in ready:
                data = os.read(stdin, 256)
                if not data or b"\x1d" in data:  # Ctrl-]
                    port.write(data.split(b"\x1d")[0])
                    break
                port.write(data)
    finally:
        if saved is not None:
            termios.tcsetattr(stdin, termios.TCSADRAIN, saved)
    rest, dropped = link.bridge_close()
    sys.stdout.buffer.write(rest)
    print(f"\nbridge closed ({dropped} bytes dropped)", file=sys.stderr)


def cmd_verify(link, args):
    device = programmer.connect(link)
    with open(args.file) as f:
        image = ihex.parse(f.read())
    cells, _ = devices.image_to_cells(device, image)
    programmer.verify_cells(link, cells, progress=None if args.quiet else progress)
    print(f"{device.name}: matches {args.file}")


def main(argv=None):
    parser = argparse.ArgumentParser(prog="picprog", description=__doc__)
    parser.add_argument("--port", default=DEFAULT_PORT, help=f"serial port ({DEFAULT_PORT})")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument("--timeout", type=float, default=5.0, help="seconds per read")
    parser.add_argument("-q", "--quiet", action="store_true", help="no progress output")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("probe", help="identify the target").set_defaults(run=cmd_probe)
    sub.add_parser("id", help="read the device and revision IDs").set_defaults(run=cmd_id)
    sub.add_parser("dci", help="show the Device Configuration Information").set_defaults(run=cmd_dci)

    p = sub.add_parser("read", help="print cells of one region")
    p.add_argument("region", choices=devices.REGIONS)
    p.add_argument("offset", type=lambda s: int(s, 0))
    p.add_argument("count", type=lambda s: int(s, 0))
    p.set_defaults(run=cmd_read)

    p = sub.add_parser("dump", help="read every region into an Intel HEX file")
    p.add_argument("file")
    p.set_defaults(run=cmd_dump)

    p = sub.add_parser("erase", help="bulk erase everything, one region, or one row")
    p.add_argument("region", choices=("all",) + devices.REGIONS)
    p.add_argument("--row", type=lambda s: int(s, 0), help="erase only the row holding this offset")
    p.set_defaults(run=cmd_erase)

    p = sub.add_parser("write", help="program an Intel HEX file (bulk erases first)")
    p.add_argument("file")
    p.add_argument("--no-erase", action="store_true", help="the target is already erased")
    p.add_argument("--no-verify", action="store_true")
    p.set_defaults(run=cmd_write)

    p = sub.add_parser("verify", help="compare the target with an Intel HEX file")
    p.add_argument("file")
    p.set_defaults(run=cmd_verify)

    p = sub.add_parser("crc", help="compare the target with an Intel HEX file by CRC-32")
    p.add_argument("file")
    p.set_defaults(run=cmd_crc)

    p = sub.add_parser("blank", help="check that regions are erased")
    p.add_argument("region", nargs="?", default="all", choices=("all",) + devices.REGIONS)
    p.set_defaults(run=cmd_blank)

    p = sub.add_parser("power", help="report or switch the target supply")
    p.add_argument("state", nargs="?", choices=("on", "off"))
    p.set_defaults(run=cmd_power)

    sub.add_parser("vdd", help="measure the target VDD").set_defaults(run=cmd_vdd)

    p = sub.add_parser("bridge", help="terminal to the target's serial port (Ctrl-] ends)")
    p.add_argument("baud", type=int)
    p.set_defaults(run=cmd_bridge)

    sub.add_parser("selftest", help="check the ICSP wiring for shorts and stuck lines"
                   ).set_defaults(run=cmd_selftest)

    args = parser.parse_args(argv)
    try:
        link = open_link(args)
        args.run(link, args)
    except (LinkError, ihex.HexError, KeyError, OSError) as e:
        print(f"picprog: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:  # Link has already ended any transfer in progress
        print("picprog: interrupted", file=sys.stderr)
        return 130
    return 0
