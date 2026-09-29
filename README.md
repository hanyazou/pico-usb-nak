# pico-usb-nak

Makes an RP2040 board act as a USB device that NAKs every control transfer,
so the host fails to enumerate it. For testing host-side error handling.

Target board: [Waveshare RP2040-Zero](https://www.waveshare.com/wiki/RP2040-Zero)
(other RP2040 boards work with small changes, see [Other boards](#other-boards)).

---

## Purpose and background

USB host products (embedded hosts, OTG devices, and similar) are expected to
tell the user when an attached device does not respond, for example with a
"Device not responding" message. USB-IF compliance testing checks this
behaviour. In the official test it is exercised with the USB-IF **PET**
(Protocol and Electrical Tester), which can play the role of a device that
connects but never answers.

A PET is expensive and not always at hand. For day-to-day development and
pre-checks, all you need is *a device that signals attach but never completes
enumeration*. This project turns a cheap RP2040 board into exactly that.

It is **not** a substitute for the real compliance test: it only reproduces the
"attached device does not respond" situation so that you can develop and check
the host's handling of it.

### Why RP2040, and why no SDK/core modification

An earlier version of this tool was built on an AVR Pro Micro, but it needed a
modified Arduino core, which was hard to maintain and was eventually lost.

The RP2040 is Full-Speed only and its USB controller is simple enough to drive
directly from application code. This firmware:

- uses the **unmodified** Raspberry Pi Pico SDK,
- does not use TinyUSB in the "unresponsive" mode — it programs the USB
  controller registers itself,
- uses the normal TinyUSB CDC stack (via `pico_stdio_usb`) only in the
  "normal" mode, to show usage instructions.

So it should keep building with future SDK releases without patches.

---

## How it works

In **UNRESP** mode the firmware:

1. resets the USB controller and clears its DPRAM,
2. connects the controller to the on-chip PHY and forces VBUS detect on,
3. enables the controller in device mode and turns on the D+ pull-up.

The host sees a Full-Speed device attach and starts enumeration. The RP2040
hardware always ACKs SETUP packets, but because no EP0 buffer is ever marked
available, every following IN/OUT data or status stage is **NAKed forever**.
The host's control transfers time out, it retries, and finally gives up.

On Linux this typically looks like the following (exact lines depend on the
host controller and kernel version):

```
usb 1-1: new full-speed USB device number 5 using xhci_hcd
usb 1-1: device descriptor read/64, error -110
usb 1-1: device descriptor read/64, error -110
...
usb usb1-port1: unable to enumerate USB device
```

Each control transfer times out after about 5 seconds and the kernel retries
several times, so it can take **tens of seconds** before the final message.

---

## Modes and LED

The board has two modes. The on-board WS2812 RGB LED (GP16) shows the state.

| LED            | State                                                        |
|----------------|--------------------------------------------------------------|
| Blue, blinking | Startup window (default 6 s). USB is **detached**.           |
| Green          | **NORMAL**: enumerates as a USB CDC serial port.             |
| Red            | **UNRESP**: attached, but NAKs everything. Host times out.   |

---

## Usage

The RP2040-Zero has only two buttons, **BOOT** and **RESET**. BOOT is used as
the mode button.

> **Do not hold BOOT while plugging in.** That starts the RP2040 ROM
> bootloader (the UF2 drive) instead of this firmware.

### Start in NORMAL mode

1. Plug the board in. The LED blinks blue for 3 seconds.
2. Do nothing. The LED turns green and a USB serial port appears
   (e.g. `/dev/ttyACM0`).
3. Open the port with any terminal program. After about 1 second, usage
   instructions are printed.

```sh
picocom /dev/ttyACM0      # or: screen /dev/ttyACM0, minicom -D /dev/ttyACM0
```

### Start in UNRESP mode (the test)

1. Plug the board into the host under test. The LED blinks blue.
2. **Press BOOT while the LED is blinking blue.** The LED turns red.
3. The host now sees a device that never responds.

During the blue window the pull-up is off, so the host never sees a
successful enumeration before the unresponsive device appears.

### Switch mode without unplugging

Press BOOT at any time while the board is running (green or red). The board
detaches from USB, waits 0.5 s, and reboots into the other mode, skipping the
blue window. This lets you repeat the test without touching the cable.

### Optional: external mode switch

If you want a true "set before plugging in" operation, connect a switch
between a GPIO and GND and enable it in `CMakeLists.txt`:

```cmake
target_compile_definitions(pico-usb-nak PRIVATE MODE_SWITCH_PIN=29)
```

When that pin is low at power-up, the board goes straight into UNRESP mode.

### Debug log

A short log is always printed on UART0 (GP0 = TX, GP1 = RX, 115200 8N1).
This is the only log output in UNRESP mode, since USB is not usable there.

---

## Building

### Requirements

- [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) (2.x)
- CMake 3.13 or later
- ARM GNU toolchain (`arm-none-eabi-gcc`)
- Optional: [picotool](https://github.com/raspberrypi/picotool) for flashing

On Debian/Ubuntu:

```sh
sudo apt install cmake gcc-arm-none-eabi libnewlib-arm-none-eabi \
                 libstdc++-arm-none-eabi-newlib build-essential git

git clone --depth 1 https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
cd ~/pico-sdk && git submodule update --init --depth 1
export PICO_SDK_PATH=~/pico-sdk
```

### Build

```sh
export PICO_SDK_PATH=~/pico-sdk
mkdir build && cd build
cmake ..
make -j
```

The output is `build/pico-usb-nak.uf2`.

The board is set to `waveshare_rp2040_zero` in `CMakeLists.txt`. You can
override it on the command line, e.g. `cmake -DPICO_BOARD=pico ..`.

---

## Flashing

**From NORMAL mode (green LED)** — the TinyUSB reset interface is active, so
picotool can reboot the board into the bootloader for you:

```sh
picotool load -f build/pico-usb-nak.uf2
picotool reboot
```

**From any state** — enter the ROM bootloader manually:

1. Hold **BOOT**, press and release **RESET**, then release **BOOT**
   (or: hold BOOT while plugging in).
2. A drive named `RPI-RP2` appears.
3. Copy `pico-usb-nak.uf2` to it. The board reboots automatically.

---

## Configuration

Compile-time options (set with `target_compile_definitions` in
`CMakeLists.txt`):

| Macro               | Default | Meaning                                               |
|---------------------|---------|-------------------------------------------------------|
| `STARTUP_WINDOW_MS` | `6000`  | Length of the blue window after power-up (ms).        |
| `WS2812_PIN`        | `16`    | GPIO of the WS2812 LED.                               |
| `LED_ORDER_RGB`     | `0`     | Set to `1` if red and green appear swapped.           |
| `MODE_SWITCH_PIN`   | (unset) | GPIO of an optional external mode switch (low = UNRESP). |

---

## Other boards

- **Other RP2040 boards** (e.g. Raspberry Pi Pico): set `PICO_BOARD`
  accordingly. The Pico has no WS2812, so either drop the LED code or point
  `WS2812_PIN` at an external LED. The BOOTSEL-reading code works on any
  RP2040 board.
- **RP2350 boards** (e.g. Pico 2): the UNRESP USB code itself also works on
  RP2350, but the BOOT button reading (`bootsel_raw()`) is written for RP2040
  and must be adapted before use.

---

## Troubleshooting

- **The UF2 drive appears instead of the firmware**: BOOT was held while the
  board powered up. Release it and replug, then press BOOT only after the LED
  starts blinking blue.
- **No usage text in NORMAL mode**: the text is printed when a terminal opens
  the port (DTR is asserted). Close and reopen the terminal to see it again.
- **LED colours look wrong**: build with `LED_ORDER_RGB=1`.
- **The host does not report anything in UNRESP mode**: it can take tens of
  seconds for the host to give up. Check `dmesg -w` (Linux) while waiting.

---

## File layout

```
.
├── CMakeLists.txt   Build configuration (board, libraries, options)
├── main.c           Firmware: mode selection, UNRESP USB setup, CDC usage text
├── ws2812.pio       PIO program for the WS2812 LED (from pico-examples)
└── README.md
```
