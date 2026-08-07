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

### 3. Escalating a genuinely lost transfer

Suppressing the line errors (above) removed the only mechanism that used to
un-stick a wedged class driver. The stock driver raised `UDC_EVT_ERROR` for
each line error, and the CDC-ACM class clears its TX-busy flag on an error
event — noisy, but self-healing.

When the buffer-status re-check finds a latched completion, suppression is
correct: the transfer really did finish. But when an error fires with
*nothing* latched in `buf_status`, a transfer was genuinely lost on the wire
and no completion is ever coming — and staying silent leaves the class
driver's single-outstanding-transfer flag set forever.

The driver now counts consecutive suppressed errors that had no completion
behind them and raises one `UDC_EVT_ERROR` after
`RPI_PICO_EMPTY_ERR_LIMIT` (8) of them. Hub-artifact storms have real
completions behind them and reset the counter, so they stay suppressed and
cannot flood the usbd core thread.

### 4. Endpoint-level errors (the shell wedge)

`SIE_STATUS.ENDPOINT_ERROR` (bit 23) reports that an endpoint's transmit or
receive path has failed; `EP_TX_ERROR` / `EP_RX_ERROR` hold a saturating
2-bit error count per endpoint. The stock driver **enables neither the
interrupt nor reads those registers**, so a failing data endpoint is never
noticed.

That is the USB shell freeze. Measured on a wedged Bento2:

    usb   INTE=0001fbf0 BUFF_STATUS=00000000 SIE_STATUS=40851005
    usb_ep tx_err=00000010 rx_err=00000001
    sie_err crc=100 bitstuff=0 dataseq=0 rxto=1 rxovf=0

`SIE_STATUS` bit 23 is latched. `EP_TX_ERROR` shows EP1 (the CDC bulk IN —
the shell's output path) saturated at 3, then errors moving to EP2. `INTE`
bit 21 (`ENDPOINT_ERROR`) is clear, confirming the interrupt was never
enabled. Meanwhile `BUFF_STATUS` is 0, the shell's TX ring buffer is empty,
and every thread is idle: the SIE abandoned the endpoint, so no completion
interrupt is ever raised, and the whole stack settles believing its work is
done. Output stops permanently with no error anywhere in software.

The patch enables `USB_INTE_ENDPOINT_ERROR_BITS`, and on that interrupt
reads and clears `EP_TX_ERROR`/`EP_RX_ERROR` and the `ENDPOINT_ERROR`
latch (all write-clear), re-checks buffer status, and submits a single
`UDC_EVT_ERROR` so the class driver can reset its state instead of waiting
forever on a transfer the hardware has already given up on.

### 5. Data-toggle rollback (the actual root cause)

`rpi_pico_prep_tx()`/`prep_rx()` advance the DATA0/DATA1 toggle when a
transfer is *queued*, not when it *completes*:

    buf_ctrl |= ep_data->next_pid ? ..DATA1_PID : ..DATA0_PID;
    ep_data->next_pid ^= 1U;      /* before the SIE has sent anything */

One corrupted packet then desynchronises device and host permanently: the
driver queues DATA0 and advances to DATA1; the packet is lost to a CRC
error so the host still expects DATA0; the retry sends DATA1, which the
host discards as a duplicate *and ACKs*; the device sees the ACK and
believes it succeeded. From then on every packet is silently dropped by the
host while the device thinks the link is fine. Only a device reset clears
it, because reset is the only path that zeroes `next_pid`.

This is why a single CRC error — something USB is designed to survive by
retrying — killed the shell permanently.

`rpi_pico_rollback_pid()` undoes the advance for each endpoint reported in
`EP_TX_ERROR`/`EP_RX_ERROR` before requeueing, so the retry carries the PID
the host is actually waiting for. `EP_RX_ERROR` has a dedicated SEQ
(sequence-error) bit per endpoint: the hardware reports this condition
explicitly and the stock driver ignores it.

The CRC counter climbing alongside the endpoint errors (0 → 100 in ~10 s,
with bit-stuff/RX-timeout/overflow at zero) reflects the *trigger* rate.
CRC errors are generated by the SIE and cannot be caused by firmware
timing, but they are normal on any USB link — the defect is that the driver
did not survive them.

The board's USB front end checks out against the datasheet, so the trigger
is not a missing part: the USB-C CC termination is present (R51/R53, 5.1K
to GND, schematic sheet 10), and the 27 Ω series resistors R5/R6 on D+/D-
(sheet 2) are *required* by the RP2350 — the datasheet specifies a 27 Ω
series termination on each pin, with only the bus pullups and pulldowns
provided internally. Remaining physical-layer suspects are therefore
layout, rail decoupling around the PHY, and cable/host, not the schematic.

This patch does not fix the trigger — it makes the resulting wedge
recoverable instead of terminal.

Status: patch 0002 took CDC-ACM on the M33 from unusable to
first-command-reliable. Part 4 addresses the endpoint wedge that remained.

## Reverted experiments

Earlier revisions of this tree carried patches 0003 (CDC-ACM TX watchdog,
ZLP latch, orphaned-FIFO resubmit) and 0004 (shell TXDONE pre-clear and
bounded wait). Both were removed: on-target measurement showed the states
they targeted do not occur during the wedge — `CDC_ACM_TX_FIFO_BUSY` is
clear, the shell TX ring buffer is empty (`used=0`, `tx_busy=0`), and no
thread is blocked. They were fixing hypothesised faults, not the real one.

Two genuine upstream bugs were identified along the way and are recorded
here in case they matter later, though neither causes this failure:

  - `usbd_cdc_acm.c`: `data->zlp_needed` is assigned in exactly one place
    and never cleared, so once armed it stays armed.
  - `usbd_cdc_acm.c` force-`#undef`s its own `CONFIG_USBD_CDC_ACM_LOG_LEVEL`
    to `LOG_LEVEL_NONE` when the shell is on a CDC-ACM node *and*
    `CONFIG_SHELL_LOG_BACKEND=y`, silently discarding all logging from that
    module. Keep `CONFIG_SHELL_LOG_BACKEND=n` when debugging USB.
