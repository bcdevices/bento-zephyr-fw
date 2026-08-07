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

### 2.5 Register semantics, second instance (BUFF_STATUS)

Same defect as §2.1 in the completion path. `BUFF_STATUS` bits are
write-1-to-clear and `INTS.BUFF_STATUS` is a read-only *level* derived from
them ("Clear by clearing all bits in BUFF_STATUS"). Cleared via the CLR alias,
so no completion was ever acknowledged: the register accumulated every bit ever
set, the interrupt stayed asserted permanently, and every later USB interrupt
re-dispatched all historical completions from a stale word — re-arming an OUT
endpoint whose buffer had already been handed upward, so the next host packet
was absorbed with nothing queued to receive it.

`EP_ABORT_DONE` is also W1C and was never acknowledged, so after the first
abort of an endpoint the handshake wait exited immediately on a stale bit,
allowing `buf_ctrl` to be modified while the SIE still owned the buffer.

### 2.6 Bus reset could not reach DEFAULT state

`event_handler_bus_reset()` returned early when `usbd_config_set(0)` failed,
skipping the state reset, the speed update, the `rwup` clear and the pending
control-IN dequeue — while `USBD_MSG_RESET` was still published, telling the
application a reset had completed. Every later reset failed identically, so the
divergence was permanent. Also clears the stale `ep_halt` bitmap (hardware
clears halts on reset; this was only cleared at init) and restarts the control
pipe.

### 2.7 SET_CONFIGURATION skipped its state assignment

The "already in the configuration" early return compared only the config value,
so the device could ACK the request while `usbd_state_is_configured()` stayed
false — leaving the host and device disagreeing, with everything gated on that
state silently failing.

### 2.8 Suspend/resume broadcast was gated on CONFIGURED

Suspend is orthogonal to Default/Address/Configured (USB 2.0 §9.1.1.6) and
hosts routinely suspend between enumeration steps. A suspend delivered while
configured whose resume arrived after a bus reset was dropped, leaving class
drivers suspended forever.

### 2.9 Data toggle not rolled back on cancel

`rpi_pico_ep_cancel()` never rolled back `next_pid`, which advances at *queue*
time. Cancelling burns a step the host never observed, and recovery is
impossible: a sequence error sets `DATA_SEQ_ERROR` but does not touch
`BUFF_STATUS`, so no completion is raised and the re-arm path never runs.

This is deterministic — we armed the PID, we are aborting it, the host never
saw it. **Contrast with an earlier failed attempt** to roll back after a
TRANSACTION error, which carries no information about whether the host accepted
the data; that guess measurably raised the error rate and was reverted.

### 2.10 Stale abort released another transfer's claim

The CDC-ACM error path released a busy flag keyed on endpoint address alone.
Completions arrive through a message queue (`usbd_event_carrier()` is a
`k_msgq_put`), so a transfer cancelled during a bus reset is still queued while
the reset completes — `usbd_thread` is several frames deep in the teardown and
cannot drain its own queue. By the time the `-ECONNABORTED` is processed,
`disable()` has released the flag and `enable()` has armed a fresh transfer that
now owns it.

**This is the opposite failure to every other busy-flag bug here:** the flag
ends up wrongly *clear*, handing a second concurrent transfer to a
single-outstanding-transfer design. Fixed by recording which buffer holds each
claim.

### 2.11 Smaller fixes

- Abort-handshake timeout logged and fell through, modifying `buf_ctrl` while
  the SIE may still own the buffer and reporting the endpoint idle.
- `handle_buff_status_out()` left `stat.busy` set when no queued buffer
  existed. `UDC_EVT_ERROR` is cosmetic — `usbd_core` only logs and publishes —
  so a transient buffer shortage killed reception permanently.
- `k_sem_reset()` does not wake waiters, so a thread blocked in
  `cdc_acm_send_notification()` stayed blocked forever on a failed transfer.
- `enable()` gated the RX re-arm on `IRQ_RX_ENABLED`, which the consumer sets
  once; a configuration torn down before the shell initialised left the
  endpoint unarmed with nothing able to arm it.
- `post_status` survived an abandoned control transfer, so the deferred
  SET_ADDRESS action applied against the wrong request — the address was never
  written while `ch9_data.state` already claimed ADDRESS.

## 3. The "lost OUT transfer" — superseded

An earlier revision of this document reported, as the single remaining bug,
that the controller silently dropped an OUT transfer: `rx enq=8 done=7 err=0`,
eight submitted and seven completed with no error.

**That reading was probably wrong.** The same skew is produced by the CDC-ACM
class driver double-arming its RX endpoint, which one of the fixes below
(§2.10) addresses. Suspend does not cancel transfers — `usbd_core` only updates
status and broadcasts, it never dequeues or disables an endpoint — so clearing
the busy flags on resume armed a second transfer while the first was still in
flight, and the outstanding count ratcheted up by one per resume. That yields
an enqueued-minus-completed skew of exactly one per resume, with zero errors,
which is indistinguishable from a lost completion.

The lesson is recorded as a test (`usb_state::test_skew_looks_like_a_lost_
completion`) so the same misreading is not repeated. Whether a genuine loss
also occurs is unknown and needs re-measuring on hardware now that the
double-arm is fixed.

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

## 7. Regression test suite

`tests/usb_regression/` encodes every bug found here as a test that fails
against the buggy logic and passes against the fix. 51 tests, about a second to
run, no hardware required.

    ./tests/usb_regression/run_host_tests.sh          # macOS / any host
    west twister -p native_sim -T tests/usb_regression # Linux / CI

`native_sim` requires Linux; the shell script builds the same sources against a
minimal ztest shim so they can also run on macOS, where this work was done.

| File | Covers |
|---|---|
| `test_w1c.c` | RP2350 register access semantics — the write-1-to-clear/CLR-alias bug class |
| `test_state.c` | Busy-flag lifecycles, event ordering, enumeration state |
| `test_captured.c` | Real register values captured on wedged boards, and three decode errors that were actually made |
| `test_attack_cdc.c` | Adversarial: stale-abort claim release, notification semaphore, RX priming |
| `test_attack_udc.c` | Adversarial: data-toggle rollback, abort timeout, `-ENOBUFS` recovery |

**Every test must be validated by reintroducing the bug it covers** and
confirming it fails with a message naming the defect. A test that passes both
with and without the bug is worse than no test. Both adversarial agents
mutation-tested their own fixes this way.

Three tests deliberately encode **negative** results — EP0's immunity to the
toggle bug, and two `next_pid` resets that are correct and must not be
"fixed" — so they are not re-chased.

## 8. Open questions

**The highest-value remaining experiment.** `SIE_STATUS.CONNECTED` (bit 16),
`SUSPENDED` (bit 4) and `VBUS_DETECTED` (bit 0) are all documented `ACCESS
"RO"`, yet the ISR calls `sie_status_clr()` on them. By the access-type rule
those writes are no-ops, which would leave `INTS.DEV_CONN_DIS`,
`INTS.VBUS_DETECT` and `INTS.DEV_SUSPEND` permanently asserted — the same wedge
as the write-1-to-clear bugs in §2. But the `INTS` field descriptions
explicitly say "Cleared by writing to SIE_STATUS.CONNECTED / .SUSPENDED", i.e.
the datasheet prescribes writing a bit it also marks read-only.

These cannot be distinguished from the headers. **Check on hardware:** force a
suspend, then confirm `INTS.DEV_SUSPEND` actually deasserts after the write. If
it does not, there is a third instance of the register-semantics bug family.

**Everything in §2 is untested on target.** Thirteen fixes, all verified only
by logic tests. Re-measure with:

    make flash-blinky && sleep 6
    python3 tools/usb-wedge-test.py --pacing 1.0 --count 100

Stock Zephyr died at transaction 4. The best measured result during this work
was 49. Note the variance is large — 1, 6, 22, 29 and 49 were all observed on
identical builds — so compare several runs, always from a fresh flash.

## 9. Honest status


- **High confidence:** the write-1-to-clear bugs are genuine and
  upstream-affecting, with clean before/after evidence on hardware (the
  enumeration request log in §2.1).
- **Medium confidence:** the ten fixes found by audit and adversarial review
  are each supported by a discriminating test, but only by a *model* of the
  hardware. They have not run on a board.
- **Superseded:** the "lost OUT transfer" of §3 was most likely the class
  driver double-arming, not the controller dropping a transfer.
- **Measured but variable:** transaction-count-to-failure swings widely between
  identical runs (1, 6, 22, 29, 49 at 1 s pacing). Single runs do not distinguish
  builds; compare several, and always from a fresh flash.
- **Not established:** why the controller drops the transfer.

No oscilloscope or protocol-analyser measurement has been taken. Everything here
is from device-side registers, host-side transaction counts, and J-Link.
