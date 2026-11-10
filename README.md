# sbu3-zephyr-fw

Zephyr-based firmware for the Blue Clover Devices Bento board (plt demo board v3)

- Build platform: macOS, Linux
- Host platform: PLT Demo (v2) (nRF52)
- Target platform: PLT Demo (v2) (nRF52)

## Status

- [x] Blinky application for RP2350B
- [x] Programming RP2350B
- [ ] Test updated application with peripherals (pending fixes to pin assignment in hardware)

## Building the Project

### Local build

#### Prerequisites

- Linux hosts or Apple Mac computer running macOS
- Linux:
    - Zephyr SDK
    - device-tree-compiler
    - cmake
    - ninja
    - dfu-util
- macOS:
    - Install GNU Arm Embedded Toolchain, Version 10-2020-q4-update :
      Install the Mac OS X 64-bit Package (Signed and notarized),
      `gcc-arm-none-eabi-10-2020-q4-update-mac.pkg`, from
      https://developer.arm.com/tools-and-software/open-source-software/developer-tools/gnu-toolchain/gnu-rm/downloads
      This will install the toolchain in `/Applications/ARM`
    - `brew install cmake ninja gperf python3 ccache qemu dtc`

#### Local build instructions

- Linux:
    - `export ZEPHYR_TOOLCHAIN_VARIANT="zephyr"`
    - `export ZEPHYR_SDK_INSTALL_DIR="/opt/zephyr-sdk"`
- macOS:
    - `export ZEPHYR_TOOLCHAIN_VARIANT="gnuarmemb"`
    - `export GNUARMEMB_TOOLCHAIN_PATH="/Applications/ARM"`
- `make prereq` to install build pre-requisites
- `make dist` to end up with build artifacts in `dist/`

### Docker build

Prerequisites:

- Linux hosts or Apple Mac computer running macOS
- Docker

From terminal, execute

```sh
make docker
```

to end up with build artifacts in `dist/`

### Flashing

You'll need to source the zephyr env first:

```sh
source zephyrproject/zephyr/zephyr-env.sh
```

Flashing with J-Link:

```sh
west flash --build-dir sub3-zephyr-fw/build.bento/rp2350b/m33/blinky --runner jlink --reset
```

### Blinky on other demo boards

`blinky` also builds for other Zephyr boards, so the same demo (LED blink and
a `version` command) can be used with demo projects:

```sh
make blinky BOARD=nucleo_f439zi        # -> build.nucleo_f439zi/blinky/zephyr/zephyr.hex
make flash-blinky BOARD=nucleo_f439zi  # west flash --runner openocd (on-board ST-LINK)
make blinky BOARD=nucleo_l011k4
```

| Board | LED | Default `BLINKY_SHELL` | Console / logs |
|---|---|---|---|
| `bento` | WS2812 RGB (`led-strip`) | `usb` (USB CDC-ACM) | UART1 (GP4/GP5) |
| `nucleo_f439zi` | LD1 (`led0`) | `usb` (CN13) | ST-LINK VCP (USART3) |
| `nucleo_l011k4` | PB3 (`led0`) | `uart` only | ST-LINK VCP (USART2) |

- The LED backend comes from the devicetree: a `led-strip` alias drives the
  WS2812 colour cycle; otherwise the `led0` GPIO LED toggles at 1 Hz.
- The command transport is a build option, `BLINKY_SHELL`:
  - `usb`: the Zephyr shell over USB CDC-ACM (`usb_shell.conf`,
    `usb_shell.overlay`, `usb_shell/<board>.conf`). Needs a USB device
    controller labelled `zephyr_udc0`. Logging is enabled and goes to the
    console UART.
  - `uart`: a minimal `version` command on the console UART
    (`uart_console.conf`, `src/blinky_uart_cmd.c`). On a Nucleo that is the
    ST-LINK virtual COM port, so one USB cable does programming and the
    shell. No logging.

  Both print identical `version` output, so a test plan can parse either.
  Boards in `BLINKY_UART_ONLY_BOARDS` (`blinky/CMakeLists.txt`) always use
  `uart`.

  ```sh
  make blinky BOARD=nucleo_f439zi BLINKY_SHELL=uart  # version on the ST-LINK VCP
  ```

- The console UART also prints `blinky <version> (build <git>) on <board>`
  at boot.
- Per-board Kconfig lives in `blinky/boards/<board>.conf`, and USB-shell-only
  settings (e.g. `CONFIG_CDC_ACM_SERIAL_PRODUCT_STRING`) in
  `blinky/usb_shell/<board>.conf`. Set `BOARD_TARGET` on the `make` command
  line if the Zephyr board target has SoC/CPU qualifiers.

`app` remains Bento-only.

### Versioning

`app` and `blinky` are versioned independently, each from a `VERSION` file in
its own directory (`app/VERSION`, `blinky/VERSION`). Zephyr generates
`<zephyr/app_version.h>` from it at build time.

For `app`, Zephyr registers the stock `app` shell command, so the version can
be queried interactively:

```sh
app version            # 0.1.0
app version-extended   # 0.1.0+0
app build-version      # git describe, e.g. v0.1.0-3-g2c85d9224fca
```

`blinky` has no shell, so it logs its version once at startup instead:

```
Bento blinky 0.1.0 (build ca83aae68547)
```

To bump either version, edit the fields in that application's `VERSION` file
and rebuild.

The build version comes from `git describe --abbrev=12 --always`, so it falls
back to a bare commit hash until the repo has a `v[0-9]*` tag.

## Links

- PLT Demo Board Product page:

  [PLT Demo Board – Blue Clover Devices](https://bcdevices.com/products/plt-demo-board)

- PLT Demo Board Reference documentation:

  [PLT Demo Board (v2) — PLT Documentation](https://docs.pltcloud.com/acc/pltdemov2/)

- Design files for the PLT Demo board:

  [plt-docs/PLT-DEMOv2 · bcdevices/plt-docs](https://github.com/bcdevices/plt-docs/tree/main/PLT-DEMOv2)

- Zephyr Project documentation:

  [Blue Clover PLT Demo V2 nRF52832 — Zephyr Project Documentation](https://docs.zephyrproject.org/latest/boards/arm/blueclover_plt_demo_v2_nrf52832/doc/index.html)

- Cloud-based simulator for the PLT Demo (v2) board:

  [Renodepedia - blueclover\_plt\_demo\_v2\_nrf52832](https://zephyr-dashboard.renode.io/renodepedia/boards/blueclover_plt_demo_v2_nrf52832/)
