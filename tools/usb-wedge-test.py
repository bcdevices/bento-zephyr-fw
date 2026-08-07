#!/usr/bin/env python3
"""
Drive the Bento USB CDC-ACM shell until it stops responding.

This is the harness that made the USB investigation tractable: it turns a
vague "the shell sometimes freezes" into a number (transactions survived)
that can be compared between builds. Driving the port from a script rather
than by hand is what exposed the key clue -- slower pacing fails SOONER,
which inverts every load-based explanation and pointed at bus suspend.

Usage:
    python3 tools/usb-wedge-test.py [--pacing SEC] [--count N] [--cmd STR]

Typical:
    make flash-blinky && sleep 6 && python3 tools/usb-wedge-test.py

Pacing matters. 1.0 s is the harshest setting found so far (most idle
windows, so most chances for a spurious suspend); 0.0 s is the mildest.
Always re-flash between runs: once wedged, the device stays wedged, so a
second run against a dead port measures nothing.
"""
import argparse
import glob
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial required: pip3 install pyserial")


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    return ports[0] if ports else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pacing", type=float, default=1.0,
                    help="seconds between transactions (default 1.0, harshest)")
    ap.add_argument("--count", type=int, default=100,
                    help="transactions to attempt before declaring success")
    ap.add_argument("--cmd", default="\r\n",
                    help="what to send each iteration (default: bare newline)")
    args = ap.parse_args()

    port = find_port()
    if port is None:
        print("FAIL: no /dev/cu.usbmodem* -- device not enumerated")
        return 2

    print(f"port={port} pacing={args.pacing}s count={args.count}")
    s = serial.Serial(port, 115200, timeout=0.5)
    time.sleep(0.4)
    s.reset_input_buffer()

    total = 0
    payload = args.cmd.encode()
    for i in range(args.count):
        s.write(payload)
        s.flush()
        time.sleep(args.pacing)
        data = s.read(100000)
        total += len(data)
        if not data:
            print(f"WEDGED at transaction {i + 1}, cumulative bytes={total}")
            s.close()
            return 1

    print(f"SURVIVED {args.count} transactions, cumulative bytes={total}")
    s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
