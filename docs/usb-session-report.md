# USB CDC-ACM wedge — session report

**Board:** Bento2 (SUB-00156 REV00), RP2350B
**Branch:** `pj/app`, commits `d6c6e3a`..`40035e4`
**Companion docs:** `usb-wedge-taxonomy.md` (variant signatures, disproved
hypotheses), `usb-shell-investigation.md` (prior history)

---

## 1. Headline

Two things changed this session:

1. **Wedge Variant C is fixed** (`37f8140`, confirmed on hardware `40035e4`).
   A four-line change to `usbd_cdc_acm_enable()`.
2. **The port now recovers itself** (`4aefa93`). Wedges that were permanent
   until a manual reflash now clear automatically in ~1.5 s.

Neither addresses the underlying cause. The evidence now points firmly at a
**hardware-level signal or power integrity problem**, and the leading candidate
is specific and cheap to test (§5).

---

## 2. What was fixed

### 2.1 Variant C — the TX pump was never started

`cdc_acm_tx_fifo_handler()` is the only thing that moves bytes from the CDC
`tx_fifo` onto the USB IN endpoint. Every path that schedules it is
**edge-triggered on an event that has already happened**:

| Site | Fires when | Fails when |
|---|---|---|
| `usbd_cdc_acm.c:428` TX completion | a transfer completed AND fifo non-empty | nothing in flight, so no completion comes |
| `:512` `enable()` | `IRQ_TX_ENABLED` AND fifo **full** | fifo has space, so it takes the "ask shell for more" branch |
| `:598` `resumed()` | always | only on a resume event |
| `:1175` `irq_cb` | `tx_fifo.altered` | only a fresh `fifo_fill` sets it |
| `:1178` `irq_cb` | `zlp_needed` | cleared at `enable()` |
| `:1251` `poll_out` | a new byte is written | shell has nothing to print |

After a re-enumeration with an idle shell, **all six are false while the FIFO
holds the banner**. Nothing pumps. That is Variant C: `tx_pending=25`,
`tx enq=0`, wedged at transaction 1 with 0 bytes.

The fix adds an unconditional drain, the symmetric counterpart of the RX
priming already ten lines above:

```c
if (!ring_buf_is_empty(data->tx_fifo.rb)) {
        cdc_acm_work_schedule(&data->tx_fifo_work, K_NO_WAIT);
}
```

It is level-triggered **on the enumeration event**, not on a timer — explicitly
not the idleness class of recovery that made things worse previously
(`usb-shell-investigation.md` §6).

**Verified:** 3/3 fresh flashes survived with the full banner (Variant C was
previously 2 of 6 fresh-flash runs). Confirmed independently by interactive
shell use.

### 2.2 Link-death recovery

When SOFs stop for >1500 ms, the driver drops the D+ pullup for 20 ms, forcing
the host to re-enumerate. Measured 13 self-recoveries in one session with the
shell functional afterwards.

Keyed on **SOF loss**, not RX_TIMEOUT. An earlier version keyed on RX_TIMEOUT
and never fired, because wedges occur with `rxto=0`. A host emits a SOF every
1 ms on any live bus, so silence is a positive signal, not an idleness
heuristic. Genuine host suspend is excluded via the driver's suspended state.

---

## 3. Root cause: where the evidence points

### 3.1 A sustained CRC error rate

**~9–10 errors/second on board 1, ~3/second on board 2.** A healthy USB link
should see approximately zero.

Attributed via `EP_TX_ERROR` to **EP1 IN, the CDC notification endpoint**
(`bInterval=10 ms`, ~100 polls/sec), so roughly **10% of interrupt-IN
transactions fail**. SOF packets are token-only with no CRC16 and cannot
produce these.

### 3.2 The bus drops out for hundreds of milliseconds

SOF-gap samples over 30 s: **121, 21, 539, 39, 181, 830 ms** — with zero
reattaches in that window, i.e. all self-healing. A healthy link does not go
silent for 830 ms.

### 3.3 Eliminated by measurement

| Candidate | How it was eliminated |
|---|---|
| Firmware as source | Rate identical idle (10.8/s) vs loaded (9.1/s) — traffic-independent |
| Cable | Swapped; 9.1/s unchanged |
| Host port | Swapped; unchanged |
| D+/D− length skew | 0.12 mm over ~31 mm |
| Trace width / necking | 0.175 mm uniform |
| Series termination | 27 Ω ±1% (R4/R5), per RP2350 datasheet |
| ESD array / common-mode choke | **None exist** — only R5/R6 on the pair |
| CC termination | 5.1 kΩ present and correct |
| Crystal | 12 MHz ±10 ppm, 24 mm from the pair, XOSC-sourced |
| Crystal load caps | 12 pF C0G on 10 pF-load part; pulls tens of ppm |
| RP2350-E12 | clk_sys 150 MHz vs clk_usb 48 MHz = 3.1× margin vs 1.1× required |
| ROSC drift | clk_usb ← pll_usb ← xosc |
| Data-toggle desync | `seqerr=0`, `pids=00555555` (perfect alternation) |
| Lost completions in software | `sw == hw` in every sample, wedged and healthy |
| `usbd_msgq` overflow | `drops=0`, `hiwater=1` |
| RX throttle path | `throttle=0`, never executes |
| Missed BUFF_STATUS interrupts | `bsirq == bscalls`, `bsempty=0` |

---

## 4. Concerns

### 4.1 The test suite has now produced two tautological tests

Twice this session a test used a `fixed` boolean parameter — asserting that the
model's own `fixed==true` branch behaves correctly, with **no tie to the
driver**. Such a test passes whether or not the fix exists in the source.

- `test_enobufs_leaves_out_endpoint_dead` kept passing throughout the period
  when `handle_buff_status_out()` was missing its busy-flag release, and
  `usb-shell-investigation.md` §2.11 described that bug as *fixed* on the
  test's strength.
- The TX-pump tests had the same shape. **Demonstrated, not assumed**: deleting
  the driver fix entirely left the suite at 82 run / 1 failed — byte-identical.

Both are now tied to the source via hand-maintained constants
(`ATK_UDC_RELEASES_BUSY_{IN,OUT}`, `CDC_ENABLE_DRAINS_PENDING_TX`) and
mutation-tested in both directions.

**This remains a live hazard.** The host harness compiles standalone C against
a shim and cannot link the driver, so models are unavoidable — but any new test
using a `fixed` flag without a sync constant is worthless. Worth auditing the
remaining tests for this shape.

### 4.2 Statistical discipline

Six fresh-flash runs of one identical build produced **1, 9, 6, 1, 30, 30**.
Single runs cannot distinguish builds. Compare distributions across ≥5–6 fresh
flashes, or target a specific variant signature.

### 4.3 Re-enumeration is disruptive

The `/dev/cu.*` node disappears and reappears; anything holding the device file
gets an I/O error and must reconnect. It is a last resort, not a repair.

### 4.4 Instrumentation caveats

- `sof_seen` increments on any ISR entry observing a changed `SOF_RD`, so during
  a CRC storm it tracks ISR frequency, **not** the true 1000 frames/s.
- `in_bs` counts every IN BUFF_STATUS dispatch including continuation packets
  armed inside the completion handler, which `in_armed` never sees. **Do not
  read `bs > armed` as a hardware signal** — an earlier revision of the
  taxonomy did, and it wrongly steered the investigation toward the silicon.
- `CONFIG_ASSERT=y` is currently enabled in `blinky/prj.conf` as a deliberate
  diagnostic. Cheap, and keeps a whole bug class from failing silently.

### 4.5 Masking error interrupts makes things worse

Masking CRC/bit-stuff/overflow/data-seq interrupts drove reattaches from 2 to
27 with the port churning. Those handlers are load-bearing for **W1C latch
hygiene** even though they perform no recovery — masking leaves the latches
permanently set (`sie=c3851005` observed). Any future attempt to reduce
interrupt load must clear the latches by other means (e.g. polling), not by
masking.

---

## 5. Open items, by priority

### 5.1 Decoupling on `USB_OTP_VDD` — the leading hardware candidate

Compared against Raspberry Pi's own `RP2350B Minimal` reference design
(`RP-010329-CA-1`), parsed from both `.kicad_pcb` files. Both boards are
RP2350B in the same QFN-80 package, so pin numbers are directly comparable.

Pin functions below are from datasheet Table 1432 ("Power supply pins"), not
inferred from the netlist.

**`USB_OTP_VDD` is pin 68**, and it is the supply for the USB full-speed PHY
(datasheet Table 1431: both `USB_DP` and `USB_DM` sit in the `USB_OTP_VDD`
power domain).

What the datasheet asks for (§6.1.4):

> "USB_OTP_VDD should be decoupled with a 100nF capacitor close to the chip's
> USB_OTP_VDD pin."

| | RP reference | Bento 2 |
|---|---|---|
| Nearest cap to pin 68 | C6, 4.7 µF @ **1.88 mm** | C16, 0.1 µF @ **3.68 mm** |
| Second nearest | C12, 100 nF @ 4.81 mm | C15, 0.1 µF @ 8.12 mm |
| Caps within 6 mm of pin 68 | 3 | 1 |
| Nearest bulk (4.7 µF) | 1.88 mm | 13.76 mm |
| Caps on the 3V3 rail | 13 | 6 |

**Bento does have a 100 nF on pin 68's net** — C16, at 3.68 mm. The deviation
is one of degree, not a missing part:

1. Nearest cap is ~2× further (3.68 mm vs 1.88 mm), so more loop inductance
   to the closest charge reservoir.
2. Local bulk is absent: RP's 4.7 µF is 1.88 mm from pin 68, Bento's is
   13.76 mm. Bento has one cap of any kind within 6 mm; RP has three.
3. C16 is shared with pins 59, 60, 62, 64 and 69 — including `QSPI_IOVDD`.

**This is consistent with both unexplained observations**, though it does not
prove them. Traffic-independence: `QSPI_IOVDD` (pin 69) shares both the rail
*and* C16, and XIP flash fetches run constantly whether or not USB moves a
byte. Unit-to-unit 3× variation: ceramic tolerance, DC-bias derating and
solder/via variation on a design with thin local margin.

Confidence: a real but **modest** deviation. 3.68 mm is not so far as to make
errors inevitable, and nothing measured here demonstrates causation — this
remains the leading candidate, not a diagnosis.

**Test (5 minutes):** tack a 100 nF 0402 from pin 68 to its nearest ground via,
shortest possible leads, and re-measure the CRC rate. Adding local bulk
(≥1 µF) at the same spot tests item 2 and is the closer analogue of RP's C6.

*Correction to an earlier version of this document:* it claimed "RP's hardware
design guide calls for 100 nF per supply pin at roughly 1 mm". **That number was
inferred, not sourced** — the datasheet says only "close to", with no dimension
anywhere in §6.1, and prescribes caps on *named* pins rather than on every
supply pin. The reference layout above replaces that inference with a
measurement. Likewise, the earlier `${ALTIUM_VALUE}` caveat does not apply to
the PCB file: footprint `Value` fields there resolve normally (C16 = 0.1 µF).

### 5.2 Buck regulator coupling — second candidate

`VREG_FB` (1.1 V core rail, carries buck ripple) runs **4.5 mm parallel to the
USB pair at 0.4 mm separation**. `VREG_LX` — the switch node, full 3.3 V swing
at ~1.5 MHz — is **1.83 mm** from `RP_USB_P`. R5/R6 sit 2.04 mm from L1.

**Test:** remove L1, feed clean 1.1 V to DVDD from a bench supply, re-measure.
Combined with 5.1, this separates the two candidates.

### 5.3 Impedance was never enforced

`dielectric_constraints: no` in the PCB file — KiCad was **not** checking
impedance. The `90R` netclass declares 0.2 mm track width; traces are routed at
**0.175 mm**. Someone hand-adjusted without updating the rule. Computed Zdiff
lands ~97–104 Ω against a 90 Ω target — tolerable for full-speed, but the
geometry was never verified. Worth fixing before any reuse, and mandatory if
this layout is ever adapted for high-speed.

### 5.4 Reference-plane break near the connector

A `VBUS` pour on In1.Cu at **priority 28** outranks the GND pour at priority 2,
breaking the return path under the pair for ~0.8 mm near the connector.
Electrically negligible at full speed. **Would matter at high speed.**

*Caveat:* PCB zones are unfilled in the saved file, so this analysis used zone
outlines plus priority ordering, not actual copper. Real voiding could be worse.
Re-run a fill in KiCad to settle it.

### 5.5 Remote wakeup — a possible gentler recovery

`LINE_STATE = J (idle)` at every wedge, verified across many samples, **not
SE0**. Per USB 2.0, a *disabled* port drives SE0 and a *suspended* port idles at
J. This suggests the port is suspended, not disabled — the case where remote
wakeup legitimately applies and would avoid re-enumeration entirely.

Currently impossible for three independent reasons:
1. `app/cdc_acm_serial.c:35` sets `attributes` to `USB_SCD_SELF_POWERED` or `0`
   — `USB_SCD_REMOTE_WAKEUP` is unreachable and there is no Kconfig for it, so
   the host never issues `SET_FEATURE(DEVICE_REMOTE_WAKEUP)`.
2. `usbd_device.c:201` rejects the request with `-EACCES` unless
   `status.rwup && usbd_is_suspended()`.
3. `SIE_CTRL.RESUME` is `ACCESS "SC"` — an illegal write is consumed silently.

Ambiguity worth noting: `CONNECTED=1` and `SUSPENDED=0` simultaneously, and
this driver's own comments establish that the RP2350 `SUSPENDED` bit is
unreliable. So J alone is suggestive, not conclusive.

**If a disabled port is the real state, no device-side action can recover it**
short of a disconnect (USB 2.0 §11.5.1.4) — which validates the current
mechanism as the only option.

### 5.6 Remaining wedge variants

**Variant A** (`cdc state=35`, TX claim held): the IN endpoint is armed with
`AVAILABLE` set and a buffer queued (`recon IN sw=1 hw=1 q=1`), presenting data
the host does not collect. Skew stable at 3, not growing — nothing is leaking.

**Variant B** (`cdc state=15`, TX balanced): an OUT transfer armed with the host
sending nothing back.

Both are the same underlying condition: a host that stopped servicing a device
which is in a provably valid state. Neither is fixable from the device side
without addressing §5.1/§5.2.

### 5.7 Model checker specification test

`test_lost_completion_is_a_permanent_trap` **fails by design** — 3770 states,
17 traps. It is the specification for a level-triggered recovery path, not a
regression guard. When such a path lands it should pass unmodified. Do not
"fix" it. Suite baseline is therefore **82 run, 1 failed**.

### 5.8 Reducing notification-endpoint polling

`CDC_ACM_FS_INT_EP_INTERVAL` is 10 ms. Raising it toward 255 ms would cut
notification polls ~25×, and a shell does not use modem-status notifications.

**Do not do this before establishing that wedge frequency tracks CRC rate.**
Board 2 at ~3/s vs board 1 at ~9–10/s is the natural test. If the errors merely
accompany the wedge rather than driving it, this changes only the measurement.

---

## 6. Corrections to the prior record

- **`usb-shell-investigation.md` §5** listed the physical layer as "largely
  eliminated" because CRC counts were believed to be a W1C artifact. That
  premise is retired: the errors are distinct ISR entries at distinct
  timestamps, and `SIE_STATUS.CRC_ERROR` reads clear between them. The
  elimination no longer holds.
- **§2.11** described `handle_buff_status_out()`'s busy-flag release as fixed.
  It was applied to the IN handler only; the OUT handler lacked it.
- **§3's "lost OUT transfer"** superseding note is itself superseded. The
  enqueued/completed skew of one is the *normal steady state* — one transfer in
  flight — visible on a fully healthy device.
- **The taxonomy's `bs > armed`** was described as a hardware accounting
  inversion. It is an instrumentation artifact (§4.4).
