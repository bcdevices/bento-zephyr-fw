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
