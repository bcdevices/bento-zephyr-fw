#!/usr/bin/env python3
"""
Read the USB diagnostic counters from the Bento UART console.

The console is deliberately kept on UART1 (see blinky/app.overlay) so it
survives a USB wedge. blinky's wedge monitor prints every counter every 5 s;
this collects a window of that output and shows only the USB lines.

Usage:
    python3 tools/usb-read-counters.py [--seconds 11]

Requires the TUMPA FTDI adapter on the debug UART.
"""
import argparse
import sys
import time

try:
    from pyftdi.ftdi import Ftdi
    import pyftdi.serialext
except ImportError:
    sys.exit("pyftdi required: pip3 install pyftdi")

KEYS = ("susp=", "cdc state", "core susp", "bus resets", "ch9 ", "rx enq",
        "wedge monitor")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=11.0)
    ap.add_argument("--url", default="ftdi://ftdi:tumpa:TIM01416/2")
    ap.add_argument("--all", action="store_true", help="print every line")
    args = ap.parse_args()

    Ftdi.add_custom_product(0x0403, 0x8a98, "tumpa")
    port = pyftdi.serialext.serial_for_url(args.url, baudrate=115200, timeout=1)

    buf = b""
    t0 = time.time()
    while time.time() - t0 < args.seconds:
        chunk = port.read(4096)
        if chunk:
            buf += chunk
    port.close()

    for line in buf.decode("utf-8", "replace").splitlines():
        if args.all or any(k in line for k in KEYS):
            print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
