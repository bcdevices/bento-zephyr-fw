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

## 0002-udc-rpi-pico-fix-suspend-resume-ordering-race.patch

Fixes a suspend/resume ordering race in `udc_rpi_pico` that permanently
wedges a CDC-ACM port.

`rpi_pico_isr_handler()` takes one snapshot of the interrupt status and then
handles `DEV_RESUME_FROM_HOST` *before* `DEV_SUSPEND`. Both bits can be set
in a single pass when the host suspends and resumes faster than interrupt
latency, which happens routinely whenever the device sits briefly idle
between transfers. In that case:

  1. the resume handler runs, clearing the suspended flag;
  2. the stale suspend handler then runs and sets it again.

The driver is left marked suspended after a resume that already completed.
Both SIE latches have been cleared by that point, so no further suspend or
resume interrupt arrives to correct it — the state is permanent.

Downstream, `CDC_ACM_CLASS_SUSPENDED` makes `cdc_acm_tx_fifo_handler()`
return early on every call, so the port goes silent while the device stays
enumerated and otherwise healthy: no errors, no stuck buffers, every thread
idle. Only a device reset recovers it.

The patch handles suspend before resume, so that when both are present the
later, real event wins.

### Evidence

Measured on Bento2 (RP2350B / M33) against pristine Zephyr 4.3.0, driving
the USB shell from a host script and counting transactions until the port
stopped responding:

| Pacing between transactions | Stock 4.3.0 | With this patch |
|---|---|---|
| 0.3 s | died at 17 | 60 transactions, no failure |
| 1.0 s | died at 4 | 6 / 20 / 29 across runs |

The register state on a wedged stock board, read over J-Link:

    SIE_STATUS = 0x00000015   (VBUS_DETECTED | LINE_STATE | SUSPENDED)
    INTE       = 0x0001FBF0   (suspend and resume interrupts both enabled)
    EP_TX_ERROR = 0, EP_RX_ERROR = 0, BUFF_STATUS = 0

No endpoint errors and no stalled transfers — the device is simply
suspended and never resumes.

Slower pacing failing *sooner* is the signature that identified this: more
idle gaps means more suspend/resume pairs and more chances to hit the race.
Every load-based explanation predicts the opposite.

### Status

This is an improvement, **not a complete fix**. The port still wedges
eventually, and the transaction count before failure varies widely between
runs (1, 6, 29 at 1 s pacing). Something further remains, and the same
measurement method should be used to find it rather than reasoning from the
source alone.

See `docs/usb-shell-investigation.md` for the full investigation, including
a number of theories that were tested on hardware and disproved.
