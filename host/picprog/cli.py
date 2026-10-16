"""Command line: python3 -m picprog [--port PORT] <command> ..."""
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

    args = parser.parse_args(argv)
    try:
        link = open_link(args)
        args.run(link, args)
    except (LinkError, ihex.HexError, KeyError, OSError) as e:
        print(f"picprog: {e}", file=sys.stderr)
        return 1
    return 0
