# Bento2 USB CDC-ACM Shell Wedge — Findings and Next Steps

**Board:** Bento2 (SUB-00156 REV00), RP2350B, Cortex-M33 core
**Firmware:** Zephyr 4.3.0 + out-of-tree patches in `patches/zephyr/`
**Symptom:** The interactive shell over USB CDC-ACM stops responding after a
handful of transactions. The device stays enumerated; only a device reset
recovers it.

**Status:** Four real bugs found and fixed, verified on hardware. The port still
wedges eventually. The remaining failure is isolated and characterised (§3) but
not fixed.

---

## 1. Start here (for whoever picks this up)

### The method that worked

Everything useful came from **driving the board from a script and counting
transactions until failure**, not from reading Zephyr source. Source reading
produced six confident wrong answers in a row (§6). Two measurements broke the
problem open:

1. **Slower pacing fails sooner.** Stock Zephyr died at transaction 4 with 1 s
   between transactions, but survived to 22 at 0.15 s. That inverts every
   load-based explanation and pointed straight at bus-idle behaviour.
2. **Register reads at the moment of failure over J-Link**, plus counters
   printed to the UART console.

If you take one thing from this document: **measure first, on hardware.**

### Tools

| File | What it does |
|---|---|
| `tools/usb-wedge-test.py` | Drives the USB shell until it stops responding, prints the transaction count. The primary metric. |
| `tools/usb-read-counters.py` | Reads the diagnostic counters from the UART console (survives a USB wedge). |

Typical loop:

```sh
make flash-blinky && sleep 6
python3 tools/usb-wedge-test.py --pacing 1.0 --count 100
python3 tools/usb-read-counters.py
```

**Always re-flash between runs.** Once wedged the device stays wedged, so a
second run against a dead port measures nothing. `--pacing 1.0` is the harshest
setting found; `--pacing 0.0` is the mildest.

### Files that matter

| File | Why |
|---|---|
| `zephyrproject/zephyr/drivers/usb/udc/udc_rpi_pico.c` | The UDC driver. Most fixes live here. Where the remaining bug is. |
| `zephyrproject/zephyr/subsys/usb/device_next/class/usbd_cdc_acm.c` | CDC-ACM class driver. Busy-flag handling, TX/RX FIFO state. |
| `zephyrproject/zephyr/subsys/usb/device_next/usbd_core.c` | Event dispatch to class drivers; the `usbd_state_is_configured()` gate. |
| `zephyrproject/zephyr/subsys/usb/device_next/usbd_ch9.c` | Enumeration state machine (`ch9_data.state`). |
| `blinky/src/main.c` | Wedge monitor: dumps thread states, USB registers and all counters to UART every 5 s. |
| `blinky/app.overlay` | Shell on CDC-ACM, **console deliberately on UART1** so it survives a USB failure. Do not change this. |
| `patches/zephyr/README.md` | Per-patch rationale and evidence. |

### Register reference

USB registers at `0x50110000`; DPRAM at `0x50100000`.

| Address | Register | Use |
|---|---|---|
| `0x50110048` | `SOF_RD` | Frame counter. **Authoritative liveness signal** — advancing means the bus is running. |
| `0x50110050` | `SIE_STATUS` | Bus state. Bit 4 SUSPENDED (RO), bit 19 BUS_RESET (WC), bit 16 CONNECTED. |
| `0x50110058` | `BUFF_STATUS` | Per-endpoint completion latches. |
| `0x50110090` | `INTE` | Interrupt enables. |
| `0x5011010C` / `0x50110110` | `EP_TX_ERROR` / `EP_RX_ERROR` | Per-endpoint error counts, 2 bits each (TRANSACTION at 2n, SEQ at 2n+1). |
| `0x50100080`+ | `ep_buf_ctrl` | 2 words per endpoint, IN first. FULL = `0x8000`, AVAILABLE = `0x0400`. |

Read them on a wedged board with:

```sh
JLinkExe -device RP2350_M33_0 -if SWD -speed 5000 -autoconnect 1 \
    -CommanderScript <script>
```

---

## 2. Bugs found and fixed

### 2.1 SIE_STATUS write-1-to-clear (commit `bf7f38f`) — the big one

The event and error bits in `SIE_STATUS` are **write-1-to-clear** ("WC" in the
datasheet listing): `BUS_RESET`, `SETUP_REC`, `CRC_ERROR`, `BIT_STUFF_ERROR`,
`RX_TIMEOUT`, `RX_OVERFLOW`, `DATA_SEQ_ERROR`.

`sie_status_clr()` cleared them through `REG_ALIAS_CLR_BITS`, the atomic
bitwise-clear alias, which writes **0** to the selected bits. Writing 0 to a
write-1-to-clear bit does nothing, so **no latch was ever cleared** and the ISR
re-observed the same event on every subsequent interrupt.

For `BUS_RESET` this is fatal: one reset latches permanently and replays
forever, knocking `usbd_ch9` back to `USBD_STATE_DEFAULT` repeatedly so
enumeration can never complete.

Evidence — control-request log as `bRequest@ch9_state`:

```
before:  0005@1  8006@1 x7                      (SET_ADDRESS + descriptors, all stuck in DEFAULT)
after:   8006@1 x7  0009@2  2122@2  2120@2 x6   (SET_CONFIGURATION reaches ADDRESS, then CDC traffic)
```

Bus resets over the same window dropped from 3 to 1.

**This also explains the inflated CRC and bit-stuff counts** that made the
failure look like a physical-layer problem for most of the investigation.

### 2.2 Suspend/resume ISR ordering race (commit `add5252`)

`rpi_pico_isr_handler()` takes one status snapshot and handled
`DEV_RESUME_FROM_HOST` *before* `DEV_SUSPEND`. Both bits can be set in a single
pass when the host suspends and resumes faster than interrupt latency. Resume
cleared the suspended flag; the stale suspend then set it again, leaving the
driver suspended after a resume that had already completed. Both latches were
clear by then, so nothing corrected it.

Fixed by handling suspend first, so the later real event wins.

### 2.3 Spurious SUSPENDED, validated against SOF

The SIE asserts `SIE_STATUS.SUSPENDED` on a fully active bus. Measured: the SOF
counter advancing at the nominal 1000 frames/s while `SUSPENDED` flickered set
and clear between reads. **The host was never suspending the device.**

A device receiving SOFs is by definition not suspended, so `SOF_RD` is
authoritative and `SUSPENDED` is not. The driver now timestamps frame arrivals
and only believes a suspend after `RPI_PICO_SUSPEND_IDLE_MS` (5 ms) without
frames. It also synthesises a resume when frames return while the driver still
thinks it is suspended, because the resume interrupt is dropped routinely.

Effect at 1 s pacing: **4 → 49 transactions.** Largest single improvement.

### 2.4 CDC-ACM busy-flag leaks

`cdc_acm_rx_fifo_handler()` claims `CDC_ACM_RX_FIFO_BUSY`, then returns early on
buffer-alloc failure or enqueue failure **without releasing it**. No completion
ever arrives to clear it, so reception is never re-armed and the port silently
stops accepting input. The TX path already handled this correctly; the RX path
simply omitted it.

Also: the busy flags are now cleared in `usbd_cdc_acm_enable()`/`disable()`,
since a configuration change invalidates anything in flight.

---

## 3. The remaining bug — start here

**The controller silently drops an OUT transfer.**

Measured at the point of failure:

```
rx enq=8 done=7 err=0
cdc state=15 tx_pending=0 rx_pending=0
```

Eight RX transfers submitted to the UDC layer, **seven completed, one never
returned, with zero errors**. The class driver is behaving correctly — it
submitted a transfer and is entitled to a completion that never comes. With
`CDC_ACM_RX_FIFO_BUSY` still held, `cdc_acm_rx_fifo_handler()` returns early at
its test-and-set forever, so input dies. For a shell, no input means no commands
and therefore no output either, which is why the symptom looks like "output
stopped".

`cdc state=0x15` decodes as: bit 0 enabled, bit 2 irq-rx-enabled, **bit 4
RX_FIFO_BUSY** — with both FIFOs empty.

### Two ways forward

**(a) Fix it in the UDC driver.** Find why the transfer is lost. This is the
correct fix but needs evidence about controller behaviour that device-side
registers have not yet yielded. A USB protocol analyser, or `usbmon` on a Linux
host, would show whether the host issued the token at all. macOS has no
`usbmon`; a Raspberry Pi with `modprobe usbmon` + `tshark -i usbmon1` is the
cheapest route.

**(b) Bounded RX re-arm in the class driver.** If `RX_FIFO_BUSY` is set with an
empty RX FIFO and no completion for ~250 ms, clear it and resubmit. A guard, but
unlike the earlier failed attempts (§6) it keys on a *specific measured
condition* — one lost transfer, no error — rather than a timeout on idle
activity. It would satisfy "USB must not get stuck" without explaining the loss.

Recommendation: do (b) to make the port self-healing, and pursue (a) with a
protocol analyser when one is available. Report the UDC-level loss upstream —
`udc_rpi_pico.c` affects every RP2350 USB user.

---

## 4. Diagnostics currently in the tree

Patches 0004 and 0005 are **diagnostic only** and should be dropped before any
upstream submission. Patch 0003 mixes real fixes with instrumentation; separate
them if upstreaming.

Counters printed by the wedge monitor every 5 s:

| Field | Meaning |
|---|---|
| `susp` / `res` | Suspends accepted / resume interrupts delivered |
| `synth` / `synthS` | Resumes / suspends synthesised from SOF state |
| `false` | Suspends rejected because SOFs were still arriving |
| `sw_susp` / `hw_sie` | Driver's view vs live `SIE_STATUS` |
| `cdc state` | CDC-ACM state bitmap (bit 0 enabled, 1 suspended, 2 irq-rx-en, 3 irq-tx-en, 4 rx-busy, 5 tx-busy) |
| `rx enq/done/err` | RX transfers submitted / completed / failed — **the counter that isolated §3** |
| `core ... blocked/ok` | Class-broadcast events dropped by the configured-state gate |
| `ch9 log(req@state)` | Ring of the last 16 control requests with the ch9 state each was handled in |

> **Warning.** The trace struct is shared via
> `zephyrproject/zephyr/include/zephyr/drivers/usb/udc_rpi_pico_trace.h`. An
> earlier hand-duplicated copy in `blinky/src/main.c` drifted from the driver's
> and every counter past the divergence point read out of bounds, printing
> convincing garbage (a phantom `reenum=8` for code that no longer existed).
> **Keep the shared header. Never re-duplicate the struct.**

---

## 5. Hardware hypotheses eliminated

| Hypothesis | Verdict | Evidence |
|---|---|---|
| Probe/target backpowering (per PLT report) | Eliminated | Wedges with J-Link unplugged |
| Missing USB-C CC termination | Eliminated | R51/R53, 5.1K to GND present (schematic sheet 10) |
| 27 Ω series resistors R5/R6 wrong | Eliminated | RP2350 datasheet **requires** 27 Ω on each pin; board is correct |
| Schottky diodes needed | Not indicated | VBUS already has D901 + FB901; cannot place diodes on a 90 Ω differential pair |
| Host/dock/port topology | Eliminated | Fails identically on MacBook direct and on a CalDigit dock |
| Physical layer / signal integrity | **Largely eliminated** | The CRC and bit-stuff counts that suggested this were an artifact of §2.1 — error latches replaying because they were never cleared |

---

## 6. Theories tested on hardware and disproved

Recorded so they are not re-attempted. Each was implemented, flashed, measured,
and reverted.

| Theory | Disproved by |
|---|---|
| Lost TX completion latching `CDC_ACM_TX_FIFO_BUSY` | Bit clear when wedged; watchdog never fired |
| `zlp_needed` never cleared (a real upstream bug) | Fixing it changed nothing |
| Shell TX ring buffer backpressure (512 B) | Raised to 8192; `used=0` at wedge, path never entered |
| Lost `SHELL_SIGNAL_TXDONE` wakeup race | `k_event` is level-triggered, not edge; `used=0` means the pend is never reached |
| Orphaned CDC-ACM TX FIFO (`altered` flag gate) | Fixed; wedge unchanged |
| Data-toggle desynchronisation, TX side | Toggle rollback raised the error rate ~5/s → ~8/s and introduced new SEQ errors |
| STALL-based resynchronisation | Host issued 10 CLEAR_FEATUREs against the halted endpoint; interface stayed dead |
| Endpoint re-enumeration on error | Device disconnected itself repeatedly; host could never finish enumerating |

### Recovery mechanisms that made things worse

- **Automatic re-enumeration on a timeout heuristic** — fired during healthy idle
  periods, port churned continuously, unusable.
- **Dropping shell output on TXDONE timeout** — silently discarded everything a
  command was about to print, making a healthy shell look dead.

**Lesson.** Automatic recovery needs a *positive* signal that something is
genuinely stuck (a submitted transfer with no completion), never an *absence* of
activity. A timeout heuristic on idle produces a device that never settles,
which is worse than a wedge.

---

## 7. Honest status

- **High confidence:** §2.1 (write-1-to-clear) is a genuine, upstream-affecting
  bug with clean before/after evidence. §2.4 (RX busy-flag leak) likewise.
- **High confidence:** §3 is real and correctly isolated — `enq=8 done=7 err=0`
  is unambiguous.
- **Measured but variable:** transaction-count-to-failure swings widely between
  identical runs (1, 6, 22, 29, 49 at 1 s pacing). Single runs do not distinguish
  builds; compare several, and always from a fresh flash.
- **Not established:** why the controller drops the transfer.

No oscilloscope or protocol-analyser measurement has been taken. Everything here
is from device-side registers, host-side transaction counts, and J-Link.
