---
name: zephyr-patches
description: |
  Edit, build, and flash out-of-tree Zephyr patches in bento-zephyr-fw. Use whenever you
  modify anything under zephyrproject/zephyr/ — drivers, subsys, or include — or when
  `make flash-blinky` / `make flash` reports "patch does not apply", "FAIL: 0003-...",
  or a patch that was working suddenly stops applying. Covers the edit -> regenerate ->
  build loop, the working-tree-is-the-source-of-truth model, and the shell pitfalls
  (persistent cd, no timeout on macOS) that waste build cycles.
author: Embedded Artistry
version: 1.0.0
---

# Managing Zephyr patches in bento-zephyr-fw

## The model

`zephyrproject/zephyr/` is a **git checkout of pristine Zephyr 4.3.0 with uncommitted
working-tree edits**. Those edits are the source of truth. The files in
`patches/zephyr/*.patch` are *generated artifacts* — snapshots of `git diff` — that exist
so the changes can be replayed onto a clean tree.

```
zephyrproject/zephyr/  (edit here)  --git diff-->  patches/zephyr/*.patch  (commit these)
```

`patches/apply-patches.sh` runs from the Makefile on **every** build target. For each
patch it does a reverse-apply check: if the patch is already present in the tree it prints
`SKIP` and moves on; otherwise it applies it.

## The failure this skill exists to prevent

Edit a file under `zephyrproject/zephyr/`, then run `make flash-blinky`:

```
FAIL: 0003-cdc-acm-fix-busy-flag-leaks-and-resume-restart.patch
error: patch failed: subsys/usb/device_next/class/usbd_cdc_acm.c:35
error: subsys/usb/device_next/class/usbd_cdc_acm.c: patch does not apply
Patches: 0 applied, 4 skipped, 1 failed
make: *** [apply-patches] Error 1
```

Nothing is broken. Your edit changed the file so the **old** patch no longer
reverse-applies cleanly, so the script can no longer recognise it as "already applied".
The fix is to regenerate the patch from the tree you just edited.

**Do not** revert your edit, `git checkout` the file, or hand-edit the `.patch` file.
Regenerate it.

## The loop

After any edit under `zephyrproject/zephyr/`, regenerate the matching patch **before**
building:

```sh
cd /Users/phillip/src/bcd/bento-zephyr-fw/zephyrproject/zephyr
git diff --src-prefix=i/ --dst-prefix=w/ -- <path/to/file.c> > ../../patches/zephyr/<NNNN-name>.patch
```

The `i/` and `w/` prefixes match the existing patch headers; plain `git diff` writes
`a/`/`b/` and still applies, but keep the repo consistent.

Then build from the **repo root**:

```sh
cd /Users/phillip/src/bcd/bento-zephyr-fw && make flash-blinky
```

A successful run prints `SKIP` for every patch — including the one you just regenerated,
because it now matches the tree.

## File-to-patch map

| Working-tree file | Patch |
|---|---|
| `drivers/led_strip/ws2812_rpi_pico_pio.c` | `0001-ws2812-pio-set-rp2350-gpio-base-for-high-bank-pins.patch` |
| `drivers/usb/udc/udc_rpi_pico.c` | `0002-udc-rpi-pico-fix-suspend-resume-ordering-race.patch` |
| `subsys/usb/device_next/class/usbd_cdc_acm.c` | `0003-cdc-acm-fix-busy-flag-leaks-and-resume-restart.patch` |
| `subsys/usb/device_next/usbd_core.c` | `0004-usbd-core-diagnostic-counters.patch` |
| `subsys/usb/device_next/usbd_ch9.c` | `0005-usbd-ch9-diagnostic-counters.patch` |

`include/zephyr/drivers/usb/udc_rpi_pico_trace.h` is **untracked** in the Zephyr checkout
and is not carried by any patch. `git diff` will not see it. If it is ever lost, it must
be restored by hand — check `git status` in the Zephyr tree for `??` entries before
assuming a clean state.

Verify the mapping rather than trusting this table if patches have been added:

```sh
grep -l "path/to/file.c" patches/zephyr/*.patch
```

## Checking state

```sh
git -C zephyrproject/zephyr status --short     # M = edited, ?? = untracked and unpatched
```

To see whether the patches and the tree agree, regenerate into a temp file and diff:

```sh
cd zephyrproject/zephyr
git diff --src-prefix=i/ --dst-prefix=w/ -- subsys/usb/device_next/class/usbd_cdc_acm.c \
  | diff - ../../patches/zephyr/0003-cdc-acm-fix-busy-flag-leaks-and-resume-restart.patch
```

Empty output means they are in sync.

## Shell pitfalls in this repo

These have each cost a build cycle:

- **`cd` persists between tool calls.** After `cd zephyrproject/zephyr`, a later
  `make flash-blinky` fails with `No rule to make target`. Always prefix build commands
  with `cd /Users/phillip/src/bcd/bento-zephyr-fw &&`, or use absolute paths and never
  bare `cd`.
- **`timeout` does not exist on macOS.** `timeout 180 python3 ...` fails with
  `command not found`. Use the tool's own `--count`/`--seconds` limits, or the Bash
  tool's `timeout` parameter.
- **Quote paths with spaces, never backslash-escape them** (see the global CLAUDE.md).
- **Build output is long.** Filter it — `2>&1 | grep -E "error:|Error|SKIP|OK:|FAIL"` —
  rather than dumping the tail, or a single real `error:` line scrolls out of reach behind
  GCC's macro-expansion notes.

## Flashing and the J-Link warning

`make flash-blinky` ends with:

```
Device specific reset executed.
****** Error: Failed to halt CPU.
```

This is **expected**. The Makefile flashes with a normal halt, then does a second J-Link
invocation that pin-resets the target to start it running; the "failed to halt" comes from
that second step and does not indicate a failed flash. Confirm success by checking the
device came back instead:

```sh
sleep 6 && ls /dev/cu.usbmodem*
```

## J-Link resets the device — do not use it on a wedged board

Connecting with `JLinkExe ... -autoconnect 1` **resets the target**. On a wedged board
this destroys the evidence: the device re-enumerates, gets a fresh USB address, and every
register you then read describes a healthy freshly-booted device rather than the failure.

Tells that a reset happened: `ADDR_ENDP` (`0x50110000`) changes between connects, `SOF_RD`
(`0x50110048`) reads `0` on the first access of each connect, and the UART console prints
a fresh `*** Booting Zephyr OS ***`.

To inspect a wedged board, read the UART console instead — it is on UART1 specifically so
it survives a USB failure:

```sh
python3 tools/usb-read-counters.py --seconds 12 --all
```

If that exits 0 with no output, the FTDI adapter is contended (another terminal holds it)
or the board has stopped printing. Distinguish the two by re-running after closing other
serial terminals.

## Before committing

Regenerate every patch whose file you touched, then confirm a clean apply from scratch:

```sh
cd /Users/phillip/src/bcd/bento-zephyr-fw && ./patches/apply-patches.sh
```

All `SKIP`, zero `FAIL`. Commit the `.patch` files — the working-tree edits under
`zephyrproject/` are not tracked by the outer repo, so **an unregenerated patch means the
change is lost** to everyone else and to CI.
