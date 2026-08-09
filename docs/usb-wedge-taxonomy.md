# USB CDC-ACM wedge taxonomy

Observed failure signatures for the Bento2 shell wedge, from the wedge monitor's
UART output. Recorded so variants are not conflated -- most of the wrong turns in
this investigation came from treating one variant's counters as evidence about
another.

**How to read `cdc state`** (bitmap from `usbd_cdc_acm_diag()`):

| Bit | Value | Meaning |
|---|---|---|
| 0 | 0x01 | `CLASS_ENABLED` |
| 1 | 0x02 | `CLASS_SUSPENDED` |
| 2 | 0x04 | `IRQ_RX_ENABLED` |
| 3 | 0x08 | `IRQ_TX_ENABLED` |
| 4 | 0x10 | `RX_FIFO_BUSY` |
| 5 | 0x20 | `TX_FIFO_BUSY` |

So `state=15` = enabled + irq-rx + rx-busy. `state=35` = the same **plus
tx-busy**. The difference between 15 and 35 is the single most discriminating
field in the dump.

---

## Variant A -- leaked TX claim

```
cdc state=35   tx_pending=0..3   rx_pending=0
tx enq=91 done=90   claimmiss=0
rx enq=24 done=23
IN armed=112 bs=112 finpost=111 din=111
```

`TX_FIFO_BUSY` set with the TX FIFO drained. One TX transfer enqueued, never
completed; the claim is held forever and every TX path tests that flag first, so
the port goes mute in the outbound direction.

Distinguishing marks:
- `cdc state=35`
- `tx enq` exceeds `tx done` by exactly 1
- `claimmiss=0` -- no mismatched-buffer completion arrived either

## Variant B -- RX stall with TX healthy

```
cdc state=15   tx_pending=0
tx enq=28 done=28      <-- perfectly balanced
rx enq=8 done=7
IN armed=49 bs=50 finpost=49 din=49
```

TX is provably fine. An OUT transfer is armed while the host is actively
sending, and no completion arrives.

Distinguishing marks:
- `cdc state=15` with `tx_pending=0`
- `tx enq == tx done`
- `recon IN sw=0 hw=0 q=0` -- the IN endpoint is genuinely idle

**Correction.** An earlier revision of this document called `bs > armed` (50 vs
49) "a hardware-level accounting inversion". That was wrong and it steered the
investigation toward the silicon. It is an instrumentation artifact: `in_armed`
increments only in `rpi_pico_handle_xfer_next()`, while `in_bs` increments on
every BUFF_STATUS dispatch -- including the ZLP/continuation packet armed from
*inside* `rpi_pico_handle_buff_status_in()`, which never touches `in_armed`.
Both counters also cover all IN endpoints, not just bulk. Do not read `bs`
against `armed` as a hardware signal.

## Variant C -- startup failure, banner never sent  [FIXED]

**Status: fixed.** `usbd_cdc_acm_enable()` now drains a non-empty `tx_fifo`
unconditionally (commit `37f8140`). Three fresh flashes at `--pacing 0.2
--count 5` all survived with the full banner delivered, where Variant C
previously accounted for 2 of 6 fresh-flash runs and wedged at transaction 1
with 0 bytes. Confirmed separately by interactive shell use.

The root cause was that every path scheduling the TX pump
(`cdc_acm_tx_fifo_handler`) is edge-triggered on an event that has already
happened, and after a re-enumeration with an idle shell all six were false
while the FIFO held the banner:

| Site | Fires when | Fails when |
|---|---|---|
| `:428` TX completion | a transfer completed AND fifo non-empty | nothing in flight, so no completion ever comes |
| `:512` `enable()` | `IRQ_TX_ENABLED` AND fifo **full** | fifo has free space, so it takes the "ask the shell for more" branch |
| `:598` `resumed()` | always | only on a resume event |
| `:1175` `irq_cb` | `tx_fifo.altered` | only a fresh `fifo_fill` sets it |
| `:1178` `irq_cb` | `zlp_needed` | cleared at `enable()` |
| `:1251` `poll_out` | a new byte is written | shell has nothing to print |

The fix is the symmetric counterpart of the RX priming ten lines above, and is
level-triggered on the enumeration event rather than on a timer -- not the
idleness class of recovery disproved in `usb-shell-investigation.md` section 6.

### Original signature (retained for reference)

```
cdc state=15   tx_pending=25   rx_pending=0
tx enq=0 done=0        <-- TX handler never ran at all
rx enq=1 done=0
OUT arms=1 bs=0   pids=00000000   seqerr=0
isr=93 bsirq=23 bscalls=23 bsempty=0 bsword=00000001
```

Wedges on transaction 1 with 0 bytes. The shell's 25-byte banner sits in the TX
FIFO and `cdc_acm_tx_fifo_handler()` is never invoked. The OUT endpoint is armed
once and never completes.

Distinguishing marks:
- `tx_pending` stuck at ~25 (the banner) with `tx enq=0`
- `rx enq=1 done=0` -- the very first RX never completes
- `bsword=00000001` -- last BUFF_STATUS word was EP0 IN, not a bulk endpoint

## Variant D -- survival

```
cdc state=15   tx_pending=0
tx enq=120 done=120    rx enq=31 done=30
```

For reference. Note `rx enq=31 done=30`: a skew of one on RX is the **normal
steady state** (one transfer armed, waiting for the host). It is not evidence of
loss. Misreading this skew cost several wrong diagnoses.

---

## Frequency

Six consecutive fresh-flash runs, identical build, `--pacing 1.0 --count 30`:

| Run | Result | Variant |
|---|---|---|
| 1 | wedged @1 | C |
| 2 | wedged @9 | B |
| 3 | wedged @6 | A |
| 4 | wedged @1 | C |
| 5 | survived 30 | D |
| 6 | survived 30 | D |

Any single run is uninformative about a build. Compare distributions across at
least 5-6 fresh flashes.

---

## Hypotheses disproved by measurement

Each was instrumented on hardware and the counter came back zero.

| Hypothesis | Killed by |
|---|---|
| `prep_tx()`/`prep_rx()` failing on multi-packet continuation | `contfail=0/0`, and `CONFIG_ASSERT=y` produced **no** assert across repeated wedges |
| Data-toggle desynchronisation on bulk OUT | `seqerr=0`, and `pids=00555555` -- a perfect alternating arm sequence |
| `XFER_NEW` dropped because the endpoint was busy | `newbusy=0 finbusy=0 qafterfin=0` in the committed build |
| BUFF_STATUS interrupts missed or spuriously serviced | `bsirq == bscalls` (23/23), `bsempty=0` |
| Enqueue onto a halted endpoint | `enq_halted=0` |
| Abort-handshake timeout stranding a transfer | `abort_to=0` |
| The enqueued/completed skew of one indicates a lost transfer | Variant D shows the same skew while fully healthy |

## The decisive measurement: software and hardware agree

A reconciliation snapshot compares the cached `stat.busy` bit against the
controller's `buf_ctrl.AVAILABLE` for both bulk endpoints, plus whether a buffer
is queued. Measured across six fresh flashes:

| Outcome | `recon OUT` | `recon IN` |
|---|---|---|
| survived | `sw=1 hw=1 q=1` | `sw=0 hw=0 q=0` |
| A (state=35) | `sw=1 hw=1 q=1` | `sw=1 hw=1 q=1` |
| B (state=15) | `sw=1 hw=1 q=1` | `sw=0 hw=0 q=0` |

**`sw` never disagrees with `hw`, in any variant or in the healthy case.** The
firmware's bookkeeping is correct. This kills the "lost completion" framing that
drove most of this investigation:

- In **variant A** the IN endpoint is *still armed in hardware* (`hw=1`) with a
  buffer queued. The transfer was never completed because the **host stopped
  IN-polling the endpoint**. Nothing was dropped device-side.
- In **variant B** the IN endpoint is idle and TX is balanced; the device has
  nothing to send and the host is not sending either.
- `recon OUT sw=1 hw=1 q=1` appears in the healthy case too -- an armed OUT
  endpoint waiting for the user to type is the normal resting state.

A consequence worth stating plainly: a device-side watchdog that reconciles
`stat.busy` against `AVAILABLE` cannot fix these wedges, because the two already
agree. The problem is on the wire or at the host, not in the driver's memory.

## Also disproved by direct measurement

| Hypothesis | Killed by |
|---|---|
| `usbd_msgq` overflow silently dropping completions (`k_msgq_put` is `K_NO_WAIT`, depth 10, return discarded everywhere) | `msgq drops=0`, `hiwater=1` -- the queue never held more than one event |
| RX throttle (`rx_fifo` too full to arm) leaving RX unarmed with no re-arm path | `throttle=0` -- the path never executes |
| `stat.busy` diverging from hardware `AVAILABLE` | `sw == hw` in every sample, wedged and healthy |

## The unifying observation

No variant recovers. That is the common thread and probably the more important
finding than any individual variant: **every re-arm path in this stack is
edge-triggered from a completion, and every busy flag is cleared only by the
completion that the flag itself gates.** Whatever causes a completion to go
missing -- and the causes may differ per variant -- the result is always
permanent, because the recovery path depends on the thing that failed.

Concretely, all of these are cleared only from a completion:

- `CDC_ACM_RX_FIFO_BUSY`, `CDC_ACM_TX_FIFO_BUSY` (class)
- `udc_ep_config.stat.busy` (driver; gates both re-arm paths in
  `rpi_pico_thread_handler()`)
- `rx_claim` / `tx_claim` (class)

A level-triggered recovery path is therefore worth having independently of root
cause. Two cautions, both learned the hard way:

- It must key on a **positive** stuck signal (work outstanding with no progress),
  never on idleness. An RX transfer legitimately stays outstanding indefinitely
  while a user types nothing; a timeout there fires constantly, releases live
  claims and makes the port far worse. Measured: `rx enq=12 done=0` within 15 s
  of boot, wedging on transaction 1.
- Absence-based recovery has failed here twice before (automatic re-enumeration
  on a timeout heuristic; dropping shell output on TXDONE timeout). See
  `usb-shell-investigation.md` section 6.


---

## Root-cause status: a sustained CRC error rate

Measured with per-error-type counters on a link that was **working at the time**
(shell responding, SOFs advancing, 30/30 transactions surviving):

```
err crc=2277 -> 2332 over 5 s   (~10 CRC errors per second, sustained)
    bitstuff=1                  (essentially zero)
    rxover=0
```

The ISR fires roughly ten times a second with `INTS=0x200` (ERROR_CRC) and
nothing else set. `SIE_STATUS.CRC_ERROR` reads clear between interrupts, so the
write-1-to-clear path is working and each one is a genuinely new error, not a
latch replaying (which was the artifact behind the section 2.1 confusion).

A healthy USB link should see essentially zero CRC errors. Ten per second is a
physical-layer fault rate. The near-total absence of bit-stuff errors alongside
it is worth noting -- the two usually track together when signalling is
marginal.

**This is the most likely root cause of the wedges.** The device is
structurally correct at every wedge (endpoints armed, nothing halted, `sw==hw`),
and errors accumulate until the host stops servicing the port.

### Instrumentation caveat, recorded so it is not misread

`sof_seen` and the CRC counter appeared to advance at an identical ~9.6/s, which
looked like a physical correlation. It is not: `sof_seen` increments on every
ISR entry that observes a changed `SOF_RD`, so during a CRC-error storm both
counters are simply tracking ISR frequency. The event log confirms the ISRs
carry `0x200` alone with no SOF bit (`0x004`) set. Neither counter measures the
true 1000 frames/s bus rate.

### The CRC rate is independent of traffic

Measured directly, same build, same session:

| Condition | CRC/sec |
|---|---|
| Idle -- no shell traffic at all | 10.8 |
| Continuous load -- newline hammered as fast as the port accepts | 9.1 |

Statistically identical. **The error rate does not depend on what the firmware
is doing.** On an idle bus the only traffic is host SOFs and periodic polling,
so ~10 errors/second arrive whether or not this firmware transfers a single
byte.

This eliminates the firmware as the *source* of the errors. No amount of driver
or class-layer work will reduce a rate that is already at full value with the
data path quiescent. It also means the errors are not caused by anything about
packet size, endpoint usage, toggle handling, or transfer pacing.

The remaining candidates are all below the firmware: cable, connector, board
routing/termination, host port, or the RP2350 SIE itself. Distinguishing them
needs either a swap test (different cable, port, host) or an instrument
(oscilloscope on the differential pair, or a protocol analyser).

### Swap test: cable and port make no difference

Section 5 of `usb-shell-investigation.md` eliminated host/port/cable topology,
but did so while the CRC counts were believed to be a write-1-to-clear
artifact. Redone now that the errors are known to be real:

| Configuration | CRC/sec |
|---|---|
| Original cable, original port | 9.6 / 10.8 / 9.1 |
| **Different cable, different port** | **9.1** |

Unchanged. The rate is the same across two cables and two ports on the same
host, and the wedge still reproduces (with the forced-reattach recovery firing
and restoring the port).

One intermediate cable produced a harder failure worth recording: the device
did not enumerate at all, and `SIE_STATUS` read `0x0000000D` --
`VBUS_DETECTED=1`, `CONNECTED=0`, and **`LINE_STATE=3` (SE1, both D+ and D-
high)**, while `SIE_CTRL=0x20010000` showed the firmware had `PULLUP_EN` set
and the transceiver powered. SE1 is an illegal USB line state that never occurs
in normal signalling. That cable was faulty; replacing it restored enumeration.
It is noted here because it demonstrates the register signature of a genuinely
broken physical link, which is clearly distinguishable from the normal wedge --
and the normal wedge does NOT show it.

So the sustained ~9-10 CRC errors/second are not attributable to the cable or
the host port. Combined with the traffic-independence result above, that leaves
the board (routing, termination, connector, power integrity) or the RP2350
itself.

### Ruled out on the board and in the clock tree

Checked against the KiCad sources in `example-projects/bento-board-2` and the
build configuration:

| Candidate | Finding |
|---|---|
| D+/D- length matching | 31.43 mm vs 31.31 mm -- **0.12 mm skew**, excellent |
| Trace width | 0.175 mm uniform, no necking |
| Vias on the pair | one per leg, adjacent and symmetric |
| Series termination | 27 ohm +/-1% (R4/R5), required by the RP2350 datasheet |
| Crystal | 12 MHz +/-10 ppm, XOSC-sourced -- inside USB's +/-2500 ppm budget |
| Crystal load caps | 12 pF C0G on a 10 pF-load crystal; CL ~8-11 pF, pulls tens of ppm |
| **RP2350-E12** (clk_sys must exceed clk_usb by >=10%) | **Not applicable**: clk_sys 150 MHz vs clk_usb 48 MHz, a 3.1x margin |
| clk_usb source | `pll_usb` <- `xosc`, not ROSC -- rules out oscillator-accuracy drift |

### Error attribution: not SOFs

The errors are attributable to real endpoint transactions, not to frame
markers. Sampled at the CRC interrupt:

```
rxerr=00000001  -> EP0 OUT, TRANSACTION error
txerr=0000000c  -> EP1 IN, TRANSACTION + SEQ errors
```

EP1 IN is the CDC notification endpoint, polled every 10 ms (`bInterval=10`),
so ~100 polls/second. SOF packets are token-only -- no data payload and no
CRC16 -- so they cannot generate CRC errors at all. The correct reading of the
rate is therefore **~10 errors per ~100 interrupt-IN polls, i.e. roughly 10% of
notification-endpoint transactions failing**, not "1% of SOFs". The bus is
never truly idle: the host polls that endpoint continuously, which is why the
rate is flat whether or not the shell is doing anything.

### Second board: the CRC rate is ~3x lower

Same firmware, same cable, same host port, different Bento2 board:

| Board | CRC/sec |
|---|---|
| Board 1 | 9.1 - 10.8 (four separate measurements) |
| **Board 2** | **3.0** |

This is the first variable that has moved the rate at all. Cable, host port and
shell traffic load all left it unchanged; swapping the board cut it by roughly
three times.

**Preliminary -- a single ~15 s measurement on a freshly flashed board.** It has
not been repeated, and the wedge behaviour on board 2 has not been
characterised. But 3/sec is still far above the ~0 a healthy USB link should
show, so the reading is not "board 1 is faulty and board 2 is fine": both boards
show the fault, at different severities.

That pattern -- present on both units, varying in degree -- points away from a
single defective board and toward something common to the design or the part,
with unit-to-unit variation. Board-level power integrity and the RP2350 itself
both remain candidates.

Next measurements, in order of value:
1. Repeat the board 2 rate several times to confirm 3/sec is stable and not an
   artifact of the fresh flash or a short window.
2. Run the wedge test on board 2. If the wedge rate scales with the CRC rate,
   that is strong evidence the errors drive the wedge rather than merely
   accompanying it.
3. Scope VBUS and the 3V3 rail on both boards during active transfer, looking
   for droop or noise coincident with error bursts. This is the one candidate
   class no software measurement can reach.

### The wedge scales with packet count, not with which command runs

`help` fails far more often than `app.version`. Measured, that is purely
exposure -- not a code path unique to long replies:

| Command | Bytes | 64-byte packets |
|---|---|---|
| `app.version` | 75 | 2 |
| `help` | 1335 | 21 |

`help` is a 10.5x larger surface for a per-transaction error.

**The discriminating test:** `app.version` was run in a loop. It wedged at
iteration 11, having sent roughly 20 packets -- essentially the same packet
count at which a single `help` (21 packets) fails. The short command reaches the
same failure, it just needs ~10 invocations to accumulate the same exposure.

So the wedge is a function of packets transferred, which is exactly what a
per-transaction error rate of order 10% predicts. There is no path that long
replies uniquely trip. This is consistent with the CRC rate being
traffic-independent: the fault is per-transaction, so anything that moves more
transactions meets it sooner.

Practical consequence for testing: measure wedges per packet, not per command
or per transaction of the harness. Two runs that differ in reply size are not
comparable.

### What has NOT been established

- Whether the CRC errors originate in the cable, connector, board, host port, or
  the RP2350 itself. No oscilloscope or protocol-analyser measurement has been
  taken, and the counters cannot distinguish these.
- Whether the rate differs across cables/ports/hosts. Section 5 of
  `usb-shell-investigation.md` eliminated topology, but did so when the CRC
  counts were believed to be a W1C artifact. That elimination should be redone
  now that the errors are known to be real.
