# Zephyr patches

Out-of-tree patches applied to the Zephyr source before building. They are
applied by `patches/apply-patches.sh` (invoked from the `Makefile` build
targets) and are idempotent — re-running is safe. Patches are generated
against pristine Zephyr 4.3.0.

## 0001-ws2812-pio-set-rp2350-gpio-base-for-high-bank-pins.patch

The stock Zephyr 4.3.0 `ws2812_rpi_pico_pio` driver drives the WS2812 data
pin using only the controller-relative GPIO number and never moves the
RP2350 PIO GPIOBASE. On the RP2350 the PIO can only address a 32-pin window,
selected by GPIOBASE (0 → GPIO 0-31, or 16 → GPIO 16-47). As a result a
data pin in the high GPIO bank (>= 32) is both numbered wrong and outside
the PIO window, so the LED never lights.

On the Bento boards the WS2812 (LTST-E683CEGBW) RGB LED data pin is:

  - Bento1: GPIO36 (gpio0_hi pin 4) — high bank, needs GPIOBASE = 16
  - Bento2: GPIO14 (gpio0_lo pin 14) — low bank, GPIOBASE = 0
  - PER-401 "Flashr": GPIO43 — high bank, needs GPIOBASE = 16

The patch:

  1. Computes the absolute GPIO number (`gpio_port_offset` = port_reg * 32,
     added to the relative pin) and uses it everywhere the driver programs
     the pin.
  2. Selects GPIOBASE per-instance at build time from the data pin
     (`WS2812_PIO_GPIO_BASE`: 16 if the absolute pin is >= 32, else 0) and
     sets it before the PIO program is loaded — the SDK rejects
     `pio_set_gpio_base()` once instruction memory is non-empty.
  3. Adds the matching `pio_gpio_init()`.

All RP2350-specific code is guarded by `CONFIG_SOC_SERIES_RP2350`, so other
SoCs are unaffected. Because GPIOBASE is chosen dynamically, the same driver
works for both the high-bank (Bento1) and low-bank (Bento2) data pins.

This is the Bento-2 adaptation of the PER-401 "Flashr" fix
(`0002-ws2812-pio-fix-gpio-pin-for-split-gpio-banks.patch` in plt-flash-os),
which hardcoded GPIOBASE = 16 because that board only has a high-bank data
pin; here the base is data-driven so a single build of the driver serves
both boards.

## 0002-udc-rpi-pico-rp2350-dpram-align-and-benign-bus-errors.patch

Two RP2350-specific fixes to the `udc_rpi_pico` USB device-controller driver.

### 1. Aligned USB DPRAM copies (Cortex-M33 UsageFault)

The stock driver copies packet payloads to/from the USB dual-port RAM with
plain `memcpy()`. Some libc `memcpy()` implementations issue unaligned word
accesses; the RP2350 DPRAM only tolerates naturally-aligned accesses, so on
the Cortex-M33 core these raise a UsageFault (the Hazard3 core tolerates
them, which is why it only bites the M33 build). The result is an
intermittent crash under USB traffic — notably with CDC-ACM.

The patch replaces the two DPRAM copy sites (`rpi_pico_prep_tx` and
`rpi_pico_handle_buff_status_out`) with helpers (`rpi_pico_dpram_write` /
`rpi_pico_dpram_read`) that access the DPRAM side with 32-bit words.

See upstream zephyr issue #96993.

### 2. Benign line-level bus errors

Behind a USB hub (including the internal hub on many host ports) the RP2350
SIE misreports the hub's split-transaction PRE/preamble tokens as CRC /
bit-stuff / data-sequence / RX-timeout errors, and its single shared
handshake-status latch can surface a genuine completion as one of these.
The device communicates normally, but the stock ISR submits a
`UDC_EVT_ERROR` for every one; under sustained bulk traffic this storm
floods the usbd core thread and starves the data path.

The patch treats those four line errors as non-fatal: it clears the SIE
status latch and re-checks the buffer-status register (a real completion may
be latched there with its BUFF_STATUS interrupt lost to the shared-latch
quirk) without raising an error event. `RX_OVERFLOW` — a genuine buffer
overrun — is still surfaced as an error.

Status: this substantially improves CDC-ACM-over-USB reliability on the M33
(from unusable to first-command-reliable) but does not fully eliminate a
wedge seen under rapid, sustained CDC output on a macOS host; that remains
under investigation and appears to be at least partly host-stack dependent
(cf. the plt-flash-os USB reliability notes).
