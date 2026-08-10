# Zephyr patches

Out-of-tree patches applied to the Zephyr source before building. They are
applied by `patches/apply-patches.sh` (invoked from the `Makefile` build
targets) and are idempotent — re-running is safe.

Current patches are generated against Zephyr 4.3.0.

## 0001-ws2812-pio-set-rp2350-gpio-base-for-high-bank-pins.patch

The stock Zephyr 4.3.0 `ws2812_rpi_pico_pio` driver drives the WS2812 data
pin using only the controller-relative GPIO number and never moves the
RP2350 PIO GPIOBASE. On the RP2350 the PIO can only address a 32-pin window,
selected by GPIOBASE (0 → GPIO 0-31, or 16 → GPIO 16-47). As a result a
data pin in the high GPIO bank (>= 32) is both numbered wrong and outside
the PIO window, so the LED never lights.

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

## 0002-udc-rpi-pico-fix-suspend-resume-ordering-race.patch

Fixes a set of defects in `udc_rpi_pico` that permanently wedge a CDC-ACM
port. All were found while chasing a USB shell that stopped responding while
the device stayed enumerated and otherwise healthy.

### Write-1-to-clear register handling

The event and error bits in `SIE_STATUS`, and all of `BUFF_STATUS` and
`EP_ABORT_DONE`, are write-1-to-clear. Upstream clears them through
`REG_ALIAS_CLR_BITS`, which writes 0 to the selected bits — a no-op on a W1C
bit, so the latch is never actually cleared. Consequences:

  - A single `BUS_RESET` latches permanently and is replayed on every
    subsequent interrupt, knocking `usbd_ch9` back to `USBD_STATE_DEFAULT`
    repeatedly so enumeration can never complete.
  - `BUFF_STATUS` accumulated every bit ever set, so each USB interrupt
    re-dispatched all historical completions from a stale word — appending
    duplicate payload, or re-arming an endpoint whose `net_buf` had already
    been handed up.
  - `EP_ABORT_DONE` bits accumulated, so the abort handshake exited
    immediately on a stale done bit and `buf_ctrl` could be modified while
    the SIE still owned the buffer.

The patch writes the bits directly so the 1s actually reach the register, and
acknowledges the `BUFF_STATUS` snapshot up front.

### Suspend/resume ordering race

`rpi_pico_isr_handler()` took one snapshot of the interrupt status and handled
`DEV_RESUME_FROM_HOST` *before* `DEV_SUSPEND`. Both bits can be set in a single
pass when the host suspends and resumes faster than interrupt latency, which
happens routinely whenever the device sits briefly idle between transfers. The
resume handler cleared the suspended flag and the stale suspend handler then
set it again, leaving the driver marked suspended after a resume that had
already completed. Both SIE latches are clear by that point, so no further
interrupt arrives to correct it.

Downstream, `CDC_ACM_CLASS_SUSPENDED` makes `cdc_acm_tx_fifo_handler()` return
early on every call, so the port goes silent while the device stays enumerated.

The patch handles suspend before resume, so the later, real event wins.

### Spurious suspend rejection

The RP2350 reports suspend on a bus that is still running. Frame arrivals are
timestamped and a reported suspend is believed only after
`RPI_PICO_SUSPEND_IDLE_MS` of genuine idle. Acting on a false suspend is
destructive: it tears down in-flight transfers, and the completions that would
have cleared the class driver's busy flags never arrive.

Conversely, if frames are arriving while the driver still thinks it is
suspended, the resume interrupt was dropped — the driver synthesises the
resume so the device does not stay suspended forever on a live bus.

### Bounded abort handshake

`rpi_pico_ep_cancel()` runs in interrupt context and spun forever waiting for
`EP_ABORT_DONE`. The wait is now bounded; on timeout the endpoint is left
marked busy rather than modifying `buf_ctrl` underneath the SIE.

### Benign line errors are not escalated

Stock Zephyr 4.3.0 treats every line error as a reportable fault, from five
paths inside `rpi_pico_isr_handler()`: CRC, bit stuff, RX overflow, RX timeout
and data sequence. Each logs, and each raises `UDC_EVT_ERROR`. These are
per-packet paths, and one board sustains roughly 10 CRC errors/second on a
link that is passing data normally, so both fire at line rate. This is a lot of
logging and event generation happening in the ISR context; logs have been removed here.

Also `UDC_EVT_ERROR` is removed from the CRC, bit-stuff and data-sequence paths.
These errors are absorbed by the protocol level. No software recovery is needed.
`usbd_core`'s handler only logs and publishes a message. RX_TIMEOUT and
RX_OVERFLOW keep the event: they are rare, and RX_TIMEOUT is the measured
precursor to a link death.

The interrupts themselves stay **enabled**. Each handler also clears a
write-1-to-clear latch.

Logging is retained on the bounded paths — the abort-handshake timeout, the
unhandled-IRQ case and link recovery — which fire once per failure rather than
once per packet.

### Link recovery

If SOFs stop arriving for `RPI_PICO_LINK_DEAD_MS` while the device is not
suspended, the host has stopped servicing the port.

Recovery is a two-rung ladder exposed to the application:

  - `udc_rpi_pico_try_wakeup()` signals remote wakeup, which preserves
    enumeration (no `/dev` node churn). Requires
    `CONFIG_CDC_ACM_SERIAL_REMOTE_WAKEUP` (patch 0004); a single attempt is
    made, because retrying was measured to never succeed.
  - `udc_rpi_pico_force_reattach()` drops the D+ pullup briefly to force
    re-enumeration. This is the last resort and always works, at the cost of
    tearing down the port.

## 0003-cdc-acm-fix-busy-flag-leaks-and-resume-restart.patch

Fixes busy-flag leaks in `usbd_cdc_acm` that leave the port silent in one or
both directions. The class driver allows a single outstanding transfer per
direction, guarded by `CDC_ACM_RX_FIFO_BUSY` / `CDC_ACM_TX_FIFO_BUSY`; every
re-arm path is gated on those flags, so a flag that is wrongly set is fatal
and a flag that is wrongly clear hands a second concurrent transfer to a
single-transfer design.

  - **Stale flags across teardown.** A transfer in flight when the
    configuration is torn down is cancelled without a completion, so the flag
    the completion path would have cleared stays set. `usbd_cdc_acm_enable()`
    and `usbd_cdc_acm_disable()` now clear both flags; a fresh configuration
    has nothing in flight by definition.
  - **Wrong-buffer release.** Completions are delivered through a message
    queue, so a cancelled transfer's `-ECONNABORTED` is still queued while the
    reset runs. By the time it is processed, `disable()` has released the flag
    and `enable()` has armed a fresh transfer that now owns it. Releasing on
    endpoint address alone therefore freed another transfer's claim. Both the
    error and success paths now track which buffer holds the claim
    (`rx_claim` / `tx_claim`) and release only for that buffer.
  - **Stale suspend.** `usbd_class_bcast_event()` drops SUSPEND and RESUME
    while the device is not configured, so a suspend whose matching resume
    arrived during the unconfigured window after a bus reset left
    `CDC_ACM_CLASS_SUSPENDED` set forever. Both FIFO handlers return early
    while it is set. `enable()` now clears it — reaching `enable()` means the
    host has just issued SET_CONFIGURATION.
  - **Lost notification waiter.** `k_sem_reset()` zeroes the count without
    waking waiters, so a thread blocked in `cdc_acm_send_notification()`
    stayed blocked forever when the notification transfer failed. The
    semaphore is now given so the waiter observes the failure.
  - **Unprimed data path after enumeration.** RX priming was gated on
    `CDC_ACM_IRQ_RX_ENABLED`, which the consumer sets once and never re-sets,
    so a configuration torn down before the consumer initialises left the
    endpoint unarmed with nothing to arm it later. TX had the symmetric
    problem: every path that schedules `cdc_acm_tx_fifo_handler()` is
    edge-triggered on an event that has already happened, so after a
    re-enumeration with an idle shell, `tx_fifo` held the shell banner and
    nothing would ever schedule the handler again. Both are now primed
    unconditionally on enumeration. The handlers re-test their guards, so
    this is a no-op when a transfer is already in flight.
    This is level-triggered on the enumeration event — evaluated once per
    SET_CONFIGURATION on the positive fact that `tx_fifo` is non-empty. It is
    not an idleness or timeout heuristic; nothing here runs while the link is
    up and working.

## 0004-cdc-acm-serial-remote-wakeup-attribute.patch

Adds `CONFIG_CDC_ACM_SERIAL_REMOTE_WAKEUP`, which sets the Remote Wakeup
attribute (bmAttributes bit 5) in the configuration descriptor. Without it the
host never issues SET_FEATURE(DEVICE_REMOTE_WAKEUP) and `usbd_wakeup_request()`
is rejected with `-EACCES`, so the device has no way to ask a suspended host to
resume it.

Required by the remote-wakeup recovery rung in patch 0002.
