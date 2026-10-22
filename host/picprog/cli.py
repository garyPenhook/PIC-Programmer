"""picprog [--port PORT] <command> ...: drive the PIC programmer over its serial port."""
import argparse
import sys

from . import configbits, devices, dia, ihex, programmer
from .link import Link, LinkError, LinkTimeout

DEFAULT_PORT = "/dev/ttyACM0"
DEFAULT_BAUD = 921600  # UART_BAUD in include/uart.h
FALLBACK_DIVISOR = 2  # UART_BAUD_FALLBACK = UART_BAUD / 2: the rate on the 16 MHz fallback clock


def close(link):
    stream = getattr(link, "stream", None)
    if hasattr(stream, "close"):
        stream.close()


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


def config_warnings(device, image):
    """What the hex file's Configuration Words would do that cannot be undone or that
    locks the part (code protection, ICSPDIS, LVP off)."""
    cells, _ = devices.image_to_cells(device, image)
    words = [cells["config"].get(i) for i in range(device.size("config"))]
    return configbits.decode(device, words)[1]


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


def cmd_dia(link, args):
    """The factory-set unique ID and calibration data (read only)."""
    device = programmer.connect(link)
    size = device.size("dia")
    if not size:
        raise LinkError(f"{device.name} has no Device Information Area")
    words = link.read("dia", 0, size)
    print(device.name)
    if args.raw:
        for i in range(0, len(words), 8):
            print(f"{i:06X}: " + " ".join(f"{w:04X}" for w in words[i:i + 8]))
    else:
        print("\n".join(dia.decode(device, words)))


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
    warn(config_warnings(device, image))  # before anything is written
    warnings = programmer.program(link, device, image, erase=not args.no_erase,
                                  verify=not args.no_verify,
                                  progress=None if args.quiet else progress)
    warn(warnings)
    print(f"{device.name}: programmed{'' if args.no_verify else ' and verified'}")


def cmd_config(link, args):
    """Decode the Configuration Words of a hex file (no programmer needed with --device) or
    of the connected part."""
    if args.file:
        device = devices.find(args.device) if args.device else programmer.connect(link)
        with open(args.file) as f:
            cells, _ = devices.image_to_cells(device, ihex.parse(f.read()))
        words = [cells["config"].get(i) for i in range(device.size("config"))]
    else:
        device = programmer.connect(link)
        words = link.read("config", 0, device.size("config"))
    lines, warnings = configbits.decode(device, words)
    if not lines:
        raise LinkError(f"{device.name} has no Configuration Word table yet")
    print(f"{device.name}")
    print("\n".join(lines))
    warn(warnings)


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
    shown = f"{vdd} mV" if vdd is not None else "unavailable (ADC failed, see picprog status)"
    print(f"target power {'on' if on else 'off'}, VDD {shown}")


def cmd_status(link, args):
    try:
        status = link.status()
    except LinkTimeout:
        # A programmer whose PLL failed runs its serial port at UART_BAUD_FALLBACK (include/uart.h)
        # because 921600 is 2% off at 16 MHz: try that before giving up.
        close(link)
        slow = argparse.Namespace(**{**vars(args), "baud": args.baud // FALLBACK_DIVISOR})
        retry = open_link(slow)
        try:
            status = retry.status()
        except LinkTimeout:
            raise LinkTimeout(f"no answer at {args.baud} or {slow.baud} baud: check the port "
                              "and the programmer's power") from None
        finally:
            close(retry)
        print(f"the programmer answered at {slow.baud} baud, not the normal {args.baud}: "
              "it is running on its fallback clock", file=sys.stderr)
    fell_back = status["clock"] != "pll"
    print(f"clock: {'64 MHz PLL' if not fell_back else '16 MHz fallback (PLL setup failed)'}")
    print(f"adc:   {status['adc']}")
    hung = status.get("reset") == "watchdog"
    if hung:
        print("reset: the last reset was the watchdog's")
    if fell_back:
        raise LinkError("the programmer is running on its 16 MHz fallback clock: the serial "
                        "baud rate and all timing are off")
    if hung:
        raise LinkError("the programmer hung and was reset by its watchdog (a WRITE or ERASE "
                        "in progress was lost); power-cycle it to clear this")
    if status["adc"] != "ok":
        raise LinkError("the ADC failed at start-up: VDD cannot be measured")


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
    p.add_argument("region", choices=devices.READ_REGIONS)
    p.add_argument("offset", type=lambda s: int(s, 0))
    p.add_argument("count", type=lambda s: int(s, 0))
    p.set_defaults(run=cmd_read)

    p = sub.add_parser("dia", help="show the Device Information Area (unique ID, calibration)")
    p.add_argument("--raw", action="store_true", help="print the words undecoded")
    p.set_defaults(run=cmd_dia)

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

    p = sub.add_parser("config", help="decode the Configuration Words of the part or a hex file")
    p.add_argument("file", nargs="?", help="hex file to decode instead of reading the part")
    p.add_argument("--device", help="part name, to decode a hex file without a programmer")
    p.set_defaults(run=cmd_config)

    p = sub.add_parser("crc", help="compare the target with an Intel HEX file by CRC-32")
    p.add_argument("file")
    p.set_defaults(run=cmd_crc)

    p = sub.add_parser("blank", help="check that regions are erased")
    p.add_argument("region", nargs="?", default="all", choices=("all",) + devices.REGIONS)
    p.set_defaults(run=cmd_blank)

    p = sub.add_parser("power", help="report or switch the target supply")
    p.add_argument("state", nargs="?", choices=("on", "off"))
    p.set_defaults(run=cmd_power)

    sub.add_parser("status", help="report start-up hardware faults (clock, ADC)"
                   ).set_defaults(run=cmd_status)
    sub.add_parser("vdd", help="measure the target VDD").set_defaults(run=cmd_vdd)

    p = sub.add_parser("bridge", help="terminal to the target's serial port (Ctrl-] ends)")
    p.add_argument("baud", type=int)
    p.set_defaults(run=cmd_bridge)

    sub.add_parser("selftest", help="check the ICSP wiring for shorts and stuck lines"
                   ).set_defaults(run=cmd_selftest)

    args = parser.parse_args(argv)
    link = None
    try:
        if not (args.command == "config" and args.file and args.device):
            link = open_link(args)  # decoding a file for a named part needs no programmer
        args.run(link, args)
    except LinkTimeout as e:
        print(f"picprog: {e}", file=sys.stderr)
        if args.command != "status":
            print("picprog: if the programmer's clock failed it talks at half the baud rate; "
                  "run `picprog status` to check", file=sys.stderr)
        return 1
    except (LinkError, ihex.HexError, KeyError, OSError) as e:
        print(f"picprog: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:  # Link has already ended any transfer in progress
        print("picprog: interrupted", file=sys.stderr)
        return 130
    finally:
        close(link)
    return 0
