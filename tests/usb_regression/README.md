# USB regression tests

Host-side regression tests for the USB bugs found while debugging the Bento2
CDC-ACM shell wedge. See `docs/usb-shell-investigation.md` for the full story.

Every bug these cover is pure logic -- register access semantics and state
machine handling -- with no timing or analog behaviour, so they run on
`native_sim` in about a second and need no hardware.

## Running

    west build -p -b native_sim -T bento.usb.regression tests/usb_regression
    ./build/zephyr/zephyr.exe

or with twister:

    west twister -p native_sim -T tests/usb_regression

## What is covered

`src/test_w1c.c` -- register semantics. Models RP2350 write-1-to-clear,
read-only and read-write bit behaviour, then asserts the driver's accessor
idioms are correct against it. Catches the class of bug that caused the worst
failures: clearing a W1C latch through the REG_ALIAS_CLR_BITS alias, which
writes a 0 and does nothing. Includes the BUFF_STATUS completion-dispatch loop,
where the same mistake caused stale completions to be re-dispatched on every
interrupt.

`src/test_captured.c` -- register values captured on a wedged board, encoded
so the decode is checked by the compiler rather than by hand. Covers SIE_STATUS
in three real conditions (active bus, genuinely suspended, bus reset), buffer
control for the bulk IN and OUT endpoints in armed/unarmed and held/idle
states, the EP_TX_ERROR/EP_RX_ERROR bit layout and the SEQ-vs-TRANSACTION
distinction that determines whether a toggle correction is justified, the CDC
endpoint addresses, the pacing inversion that identified suspend as the
trigger, SOF frame-counter arithmetic including wrap, the enumeration request
log before and after the write-1-to-clear fix, and why the diagnostic counter
struct must live in a shared header.

Several of these encode mistakes that were actually made during the
investigation: FULL and AVAILABLE were decoded backwards once, and an endpoint
index was mapped to the wrong address, causing a fix to halt the notification
endpoint (0x81) while trying to recover the bulk IN endpoint (0x82). The host
cleared that halt ten times and the port stayed dead, which briefly looked like
evidence against a correct theory.

`src/test_state.c` -- state machines. Encodes each state-handling bug as an
event sequence:

  - RX busy flag leaked on buffer-alloc and enqueue failure
  - resume double-arming the RX endpoint (and why the resulting
    enqueued/completed skew must not be read as a lost completion)
  - stale CLASS_SUSPENDED surviving re-enumeration
  - TX enqueue failure stranding queued output with no retry scheduled
  - SET_CONFIGURATION's early return skipping the state assignment
  - bus reset failing to reach DEFAULT state
  - suspend/resume broadcast dropped by the configured-state gate

## Scope and honesty

These tests model the logic; they do not exercise the real driver against real
hardware. They are a regression net -- if someone "simplifies" a direct
`sys_write32()` back into `rpi_pico_bit_clr()` on a W1C register, or restores an
early return, a test fails in seconds instead of a board wedging after a few
hundred USB transactions.

They cannot catch anything timing-dependent, anything in the analog domain, or
any behaviour of the real silicon that differs from the datasheet. Hardware
testing with `tools/usb-wedge-test.py` remains necessary.

## Running on macOS

`native_sim` requires Linux. For local runs on macOS:

    ./tests/usb_regression/run_host_tests.sh

This compiles the same test sources against a minimal ztest shim
(`host_shim.h`) and runs them as a plain host binary. The tests are
self-contained logic over integers and flags, so nothing is lost.

## Verifying the tests actually work

Each test was checked by reintroducing the bug it covers and confirming it
fails with a message that names the defect. A test that passes both with and
without the bug is worse than no test, so do this for anything added here.
