#!/usr/bin/env python3
"""Tiny host-side client for the DUT Driver Flipper app.

    pip install pyserial
    python3 host/dut.py /dev/cu.usbmodemflip_XXXXX3 set 2 1
    python3 host/dut.py /dev/cu.usbmodemflip_XXXXX3 get 2
    python3 host/dut.py /dev/cu.usbmodemflip_XXXXX3 servo 90
    python3 host/dut.py /dev/cu.usbmodemflip_XXXXX3          # interactive
"""

import sys

import serial


class Dut:
    def __init__(self, port, timeout=1.0):
        self.ser = serial.Serial(port, 115200, timeout=timeout)
        self.ser.reset_input_buffer()

    def command(self, line):
        """Send one command and return the final OK/ERR line (plus any preceding text)."""
        self.ser.write((line.strip() + "\n").encode())
        out = []
        while True:
            resp = self.ser.readline().decode(errors="replace").rstrip("\r\n")
            if not resp:
                raise TimeoutError(f"no response to {line!r}")
            out.append(resp)
            if resp.startswith("OK") or resp.startswith("ERR"):
                return "\n".join(out)

    def set_pin(self, pin, level):
        return self._check(self.command(f"set {pin} {int(bool(level))}"))

    def get_pin(self, pin):
        resp = self._check(self.command(f"get {pin}"))
        return resp.split("=", 1)[1].split()[0] == "1"

    def servo(self, angle):
        return self._check(self.command(f"servo {angle}"))

    @staticmethod
    def _check(resp):
        if resp.startswith("ERR"):
            raise RuntimeError(resp)
        return resp


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    dut = Dut(sys.argv[1])
    if len(sys.argv) > 2:
        print(dut.command(" ".join(sys.argv[2:])))
        return

    try:
        while True:
            print(dut.command(input("> ")))
    except (EOFError, KeyboardInterrupt):
        print()


if __name__ == "__main__":
    main()
