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
- **`bs > armed`** (50 vs 49): the controller dispatched a completion for a
  buffer the driver never armed. A hardware-level accounting inversion, not a
  software drop.

## Variant C -- startup failure, banner never sent

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
