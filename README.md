# DUT Driver (Flipper Zero)

A Flipper Zero app that takes text commands over USB serial to drive a device under test:
set GPIO pins high or low, read pin state, and move a hobby servo.

> 🤖 **AI Disclosure:** Most of this repository's code and documentation were generated or assisted by artificial intelligence. All outputs have been human-reviewed, tested, and verified for accuracy and safety.


## Build and install

```sh
python3 -m venv .venv && .venv/bin/pip install ufbt   # one-time
.venv/bin/ufbt                # build -> dist/dut_driver.fap
.venv/bin/ufbt launch         # build, upload and run on a connected Flipper
```

On the Flipper the app shows up under **Apps → GPIO → DUT Driver**.

## Connecting

When the app starts, it switches the Flipper's USB to dual-serial mode (the same mode the
built-in USB-UART bridge uses). Two serial ports appear:

| Port | Purpose |
|------|---------|
| first  (`/dev/cu.usbmodemflip_<name>1`) | normal Flipper CLI, so qFlipper keeps working |
| second (`/dev/cu.usbmodemflip_<name>3`) | **DUT Driver commands** |

Baud rate doesn't matter (it's USB CDC). Press **Back** on the Flipper to exit. Exiting puts
USB back the way it was, stops the servo, and sets every pin the app touched back to
high-impedance.

## Protocol

Commands are one per line, ending in `\n` or `\r\n`, and are case-insensitive. Every command
gets exactly one reply line that starts with `OK` or `ERR`.

| Command | Example reply | Notes |
|---|---|---|
| `set <pin> <0\|1>` | `OK PA7=1` | Sets the pin as a push-pull output. Also accepts `low/high` and `off/on`. |
| `get <pin>` | `OK PA7=1 out` | Reads the pin level. Outputs are read back as they are; any other pin becomes a floating input (`in`). |
| `servo <0-180>` | `OK servo=90` | Moves the servo to an absolute angle. |
| `servo +N` / `servo -N` | `OK servo=180` | Moves the servo relative to where it is now (e.g. `servo +90`). |
| `servo off` | `OK servo=off` | Stops the PWM signal. |
| `servo` | `OK servo=90` | Reports the current angle. |
| `ping` | `OK pong` | Checks that the app is responding. |
| `help` | (command list) `OK` | |

**Pins** can be given by header number or by name:

| Number | 2 | 3 | 4 | 5 | 6 | 7 | 15 | 16 |
|---|---|---|---|---|---|---|---|---|
| Name | PA7 | PA6 | PA4 | PB3 | PB2 | PC3 | PC1 | PC0 |

Pins 13/14 (TX/RX) work too. The SWD debug pins are refused.

## Servo wiring

| Servo wire | Flipper pin |
|---|---|
| Signal (orange/yellow) | 2 (PA7) |
| V+ (red) | 1 (5V) |
| GND (brown/black) | 8 or 11 (GND) |

The signal is a 50 Hz pulse with a 0.5–2.5 ms width (0°–180°), so 90° is 1.5 ms. If your
servo expects 1.0–2.0 ms, change `SERVO_MIN_US`/`SERVO_MAX_US` in `dut_driver.c`. While the
servo is running, pin 2 belongs to the servo, so `set`/`get` on that pin return an error.

The 5V pin is live whenever USB is plugged in. If the Flipper is running on battery, the app
turns on 5V output by itself and turns it off again on exit. A large servo can draw more
current than the Flipper can supply, so use an external 5V supply (with a shared ground) for
anything bigger than an SG90-class micro servo.

## Host client

```sh
pip install pyserial
python3 host/dut.py /dev/cu.usbmodemflip_<name>3 set 2 1
python3 host/dut.py /dev/cu.usbmodemflip_<name>3 get 2
python3 host/dut.py /dev/cu.usbmodemflip_<name>3 servo 90
python3 host/dut.py /dev/cu.usbmodemflip_<name>3          # interactive prompt
```

You can also use it from your own test code:

```python
from dut import Dut
dut = Dut("/dev/cu.usbmodemflip_<name>3")
dut.set_pin(3, 1)
assert dut.get_pin(3)
dut.servo(90)
```
