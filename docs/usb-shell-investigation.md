# Bento2 USB CDC-ACM Shell Wedge — Investigation Report

**Board:** Bento2 (SUB-00156 REV00), RP2350B, Cortex-M33 core
**Firmware:** Zephyr 4.3.0 + out-of-tree patches
**Symptom:** Interactive shell over USB CDC-ACM freezes mid-output, typically
during `help`, and never recovers. Position of the freeze varies between runs.
**Status:** A suspend/resume ordering race in `udc_rpi_pico` was found and
fixed (patch 0002). It is a real bug and measurably improves the failure rate,
but the port still wedges eventually — the investigation is **not closed**.

> **Note on earlier revisions of this document.** Previous versions asserted,
> in turn, that the root cause was a lost TX completion, a ZLP latch, shell
> backpressure, a TXDONE wakeup race, a data-toggle desynchronisation, and the
> USB physical layer. All were tested on hardware and disproved. They are kept
> in §5 so they are not re-attempted.
>
> The method that finally worked: drive the board directly from a host script,
> count transactions until failure, vary one parameter at a time, and read the
> registers over J-Link at the moment of failure. Reasoning from the Zephyr
> source alone produced six wrong answers in a row.

## 0. The finding that mattered

**Slower pacing between transactions made the failure happen sooner.**

| Pacing | Stock 4.3.0 died at |
|---|---|
| none | transaction 7 |
| 0.15 s | transaction 22 |
| 0.3 s | transaction 17 |
| 1.0 s | **transaction 4** |

Every load-based theory predicts the opposite. That single inversion pointed
at idle-time behaviour — i.e. USB suspend — and led directly to the race in
§2a. It was measurable in about ten minutes once the board was driven from a
script instead of by hand.

## 1. The failure, precisely

The shell stops mid-transmission — sometimes a few hundred bytes in, sometimes
after a complete `help`, occasionally mid-token (`kernel` with no colon). After
the freeze the device stays enumerated but never transmits again, and stops
accepting input as well.

Input dies with output because the shell's state machine is pumped from its
*TX* path: `shell_uart.c:uart_tx_handle()` signals
`SHELL_TRANSPORT_EVT_TX_RDY` at the end of each transmit. With TX stalled the
shell thread never returns to reading input, so the console appears completely
dead rather than merely silent. This is a consequence, not the cause.

## 2. Earlier reading of the register state (superseded)

An earlier wedged-board capture showed latched `SIE_STATUS.ENDPOINT_ERROR`
with `EP_TX_ERROR` non-zero and heavy CRC counts, and this document previously
presented that as the root cause. It is **not**. Those dumps came from builds
carrying several of my own speculative patches, some of which generated the
errors they appeared to diagnose. On pristine 4.3.0 the wedged state shows
`EP_TX_ERROR = 0`, `EP_RX_ERROR = 0`, `BUFF_STATUS = 0` and simply
`SIE_STATUS.SUSPENDED` set — see §2a.

The genuinely useful observation from that period is that CRC errors alone
never explain a *permanent* failure: USB retries through them. A failure that
survives retry is a state-machine bug, which is what §2a turned out to be.

## 2a. Confirmed bug: suspend/resume ordering race

`rpi_pico_isr_handler()` takes one snapshot of the interrupt status and handles
`DEV_RESUME_FROM_HOST` **before** `DEV_SUSPEND`. Both bits can be set in a
single ISR pass when the host suspends and resumes faster than interrupt
latency — routine whenever the device idles briefly between transfers.

  1. Resume runs first and clears the suspended flag.
  2. The stale suspend then runs and sets it again.

The driver ends up marked suspended after a resume that already completed.
Both SIE latches are clear by then, so no further interrupt corrects it.

`CDC_ACM_CLASS_SUSPENDED` then makes `cdc_acm_tx_fifo_handler()` return early
on every call: the port goes silent while the device stays enumerated and
otherwise healthy. Register state on a wedged stock board, via J-Link:

    SIE_STATUS  = 0x00000015   (VBUS_DETECTED | LINE_STATE | SUSPENDED)
    INTE        = 0x0001FBF0   (suspend and resume both enabled)
    EP_TX_ERROR = 0, EP_RX_ERROR = 0, BUFF_STATUS = 0

No endpoint errors, no stalled transfers. Simply suspended, never resumed.

**Fixed** by handling suspend before resume so the later real event wins.
Measured effect at 0.3 s pacing: died at 17 → 60 transactions with no failure.

**Still failing.** At 1 s pacing it dies at 6 / 20 / 29 across runs. Something
further remains; the same measurement method should be used to find it.

## 3. Secondary observation: ENDPOINT_ERROR is never handled (not the cause)

`drivers/usb/udc/udc_rpi_pico.c` in Zephyr 4.3.0 — and still in `main` as of
this writing:

- Never enables `USB_INTE_ENDPOINT_ERROR_BITS` (bit 21). Confirmed on target:
  stock `INTE = 0x0001fbf0`, bit 21 clear.
- Never reads `EP_TX_ERROR` / `EP_RX_ERROR`.
- Never clears `SIE_STATUS.ENDPOINT_ERROR`, which is write-clear and therefore
  stays latched forever once set.
- Has no recovery path for a failed endpoint.

The datasheet is explicit: *"An endpoint has encountered an error. Read the
ep_rx_error and ep_tx_error registers to find out which endpoint had an
error."* The driver does none of this.

This is a genuine gap and arguably worth reporting upstream, but it is **not**
the cause of this wedge: on pristine 4.3.0 the failure occurs with all
endpoint-error registers reading zero. Attempts to act on `ENDPOINT_ERROR`
(re-arm, toggle rollback, STALL-based resynchronisation) each made behaviour
measurably worse and were reverted.

## 4. Hardware hypotheses eliminated

| Hypothesis | Verdict | Evidence |
|---|---|---|
| Probe/target backpowering (per PLT report) | **Eliminated** | Wedges with J-Link fully unplugged — no second board present |
| Missing USB-C CC termination | **Eliminated** | R51/R53, 5.1K to GND, present and correct (schematic sheet 10) |
| 27 Ω series resistors R5/R6 wrong | **Eliminated** | RP2350 datasheet: *"A 27Ω series termination resistor is required on each pin"* — the board is correct |
| Schottky diodes needed | **Not indicated** | VBUS already has D901 (SK34A) + FB901; cannot place diodes on a 90 Ω differential pair; failure is not correlated with a load transient |
| Firmware starving the controller | **Eliminated** | Would produce RX_TIMEOUT/RX_OVERFLOW; both ≈ 0 |

## 5. Firmware theories tested and disproved

Each was implemented, flashed, and killed by on-target measurement. Recorded so
they are not re-attempted.

| Theory | Disproved by |
|---|---|
| Lost TX completion latching `CDC_ACM_TX_FIFO_BUSY` | Bit is clear when wedged; watchdog never fired |
| `zlp_needed` never cleared (a real upstream bug) | Fixing it changed nothing |
| Shell TX ring buffer backpressure (512 B) | Raised to 8192; `used=0` at wedge — path never entered |
| Lost `SHELL_SIGNAL_TXDONE` wakeup race | `k_event` is level-triggered, not edge; and `used=0` means the pend is never reached |
| Orphaned CDC-ACM TX FIFO (`altered` flag gate) | Fixed; wedge unchanged |

Patches 0003 and 0004 were written for these and have been **reverted and
deleted**. Two genuine upstream bugs were found along the way and are recorded
in `patches/zephyr/README.md`, though neither causes this failure.

## 6. Recovery attempts — and a warning

Two automatic recovery mechanisms were built and **both are currently
disabled**:

- **Driver-level re-enumeration** (drop/re-assert D+ pullup after N
  consecutive endpoint errors). At a threshold of 6 this fired constantly —
  the device disconnected itself repeatedly and the host could never finish
  enumerating. **Result: port churned continuously, unusable.**
- **App-level link watchdog** (`usbd_disable()`/`usbd_enable()` after 15 s of
  no error-counter movement). The detection heuristic is wrong: "errors seen,
  then quiet" also describes a link that has simply gone idle, so it fired
  during healthy periods.

**Lesson for anyone re-attempting this:** automatic re-enumeration needs a
*positive* liveness signal — an actual outstanding failed transfer — not an
absence of activity. A timeout heuristic produces a device that never settles,
which is worse than a wedge. A **user-triggered** recovery (a UART shell
command calling `usbd_disable()`/`usbd_enable()`) is deterministic and safe,
and is the recommended approach if recovery is wanted.

The endpoint **re-arm** on error (cancel stale transfer, requeue) is retained —
it is bounded and does not disturb the connection.

## 7. What is in the tree now

`patches/zephyr/0002-udc-rpi-pico-*.patch`:

1. **DPRAM alignment fix** — replaces `memcpy()` to/from USB dual-port RAM with
   32-bit accesses. Fixes a genuine Cortex-M33 UsageFault (upstream issue
   #96993). Unrelated to this wedge but a real bug.
2. **Benign line-error suppression** — hub split-transaction artifacts no
   longer flood the usbd core, with per-type counters retained.
3. **`ENDPOINT_ERROR` handling** — enables the interrupt, reads and clears
   `EP_TX_ERROR`/`EP_RX_ERROR` and the SIE latch, re-arms affected endpoints,
   surfaces a `UDC_EVT_ERROR`.
4. **Diagnostic counters** — CRC, bit-stuff, data-seq, RX-timeout, RX-overflow,
   endpoint errors, bus resets, suspends, re-enumerations.

`blinky/src/main.c` carries a wedge monitor that dumps thread states, live USB
registers, and all counters to the UART console every 5 s. This is the
instrument that made the diagnosis possible and should be kept.

## 8. What must be addressed

**Root cause is fixed in firmware** (§2a). The remaining hardware question is
about *error rate*, not about whether the failure occurs: with the toggle bug
fixed, CRC errors should cost a retry rather than the endpoint. A high error
rate is still worth reducing — it costs throughput and latency — but it is no
longer a functional blocker.

**Still worth measuring** (none performed yet — all are minutes of bench time):

1. **Healthy baseline.** Boot, type nothing, read `crc` at 30 s. Then run a
   short command (`version`). Then `help`. If `crc` is 0 until bulk traffic
   starts, the errors are load-induced → signal integrity. If it climbs at
   idle, the link is bad independent of traffic. **Every sample taken so far
   has been of a wedged board; there is no baseline for normal.**
2. **Second board.** Highest-value single test. Clean board B → one defective
   unit. Both fail identically → systematic to the design.
3. **Swap matrix.** Cable (most common culprit), direct port vs hub, different
   host machine. Compare CRC/5 s across each.
4. **Scope on D+/D−.** An eye diagram settles in minutes what register counters
   can only imply. Required before committing to any respin change.

**Recommended, independent of the above:**

- File the `ENDPOINT_ERROR` gap upstream against `udc_rpi_pico.c`. Register
  evidence is in hand.
- If an interactive shell is needed *now*: move it back to UART1 (GP4/GP5) by
  removing the `chosen` override in `app.overlay`. Five-minute change, reliable
  today, leaves USB CDC as a documented known issue rather than a blocker.

## 9. Honest assessment of confidence

- **High:** the wedge mechanism (SIE abandons the CDC bulk IN endpoint; driver
  never notices) — measured, not inferred.
- **High:** the `ENDPOINT_ERROR` driver gap is real and present in upstream
  `main`.
- **High (code-level):** the toggle-advance-on-queue behaviour is plainly in
  the source (`udc_rpi_pico.c:451`), and reset being the only path that clears
  `next_pid` explains why only a power cycle recovers.
- **Medium (not yet verified on hardware):** that the toggle desync is *the*
  mechanism producing this wedge. It fits every observation, but the fix has
  not yet been confirmed to keep a `help` alive across CRC errors on target.
  **This is the next thing to verify.**
- **Low / unknown:** whether the elevated CRC rate affects all Bento2 units or
  one board. Now a performance question rather than a functional one.

No oscilloscope measurement has been taken; all physical-layer statements are
inference from register counters and schematic review.

### Method note

Several firmware theories in §5 were pursued and discarded before the toggle
bug was found. The pattern worth repeating: **on-target register dumps killed
each wrong theory quickly, while source reading alone did not.** The wedge
monitor that dumps thread states, USB registers, and SIE counters over UART was
the single most valuable artifact of this investigation and should be kept in
the tree.

The error that cost the most time was accepting "the physical layer is bad" as
a root cause. CRC errors are expected on any USB link; the protocol retries
through them. The question that should have been asked several hours earlier is
not *why are there CRC errors* but **why does one CRC error kill the endpoint
permanently** — a failure that survives retry is always a state-machine bug.
