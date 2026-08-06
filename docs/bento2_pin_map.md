# Bento2 GPIO Pin Map (SUB-00156 REV00)

Source: SUB-00156 REV00 Schematic 2026-04-23. Reflects the GPIO assignments
encoded in `boards/blueclover/bento2/bento2_rp2350b_m33.dts` and
`boards/blueclover/bento2/bento2-pinctrl.dtsi`.

| GPIO | Chip pin | Net                              | Function                                     | Where used                                |
|------|----------|----------------------------------|----------------------------------------------|-------------------------------------------|
| GP0  | 77       | C3_SPI.MISO                      | ESP32-C3 SPI MISO                            | (not in DTS — ESP block)                  |
| GP1  | 78       | RP_SPIM_SS0                      | **BME280 CS** (GPIO) — see note below        | `&spi1` cs-gpio                           |
| GP2  | 79       | C3_SPI.SCK                       | ESP32-C3 SPI SCK                             | (not in DTS — ESP block)                  |
| GP3  | 80       | C3_SPI.MOSI                      | ESP32-C3 SPI MOSI                            | (not in DTS — ESP block)                  |
| GP4  | 1        | SCI1_TXD / RP_TX1                | UART1 TX (console + shell)                   | `uart1_default`                           |
| GP5  | 2        | SCI1_RXD / RP_RX1                | UART1 RX (console + shell)                   | `uart1_default`                           |
| GP6  | 3        | SCI1_RTS / RP_RTS1 / CS_RTS1     | click CS net (GPIO; UART1 CTS not muxed)     | `&spi1` cs-gpio                           |
| GP7  | 4        | SCI1_CTS / RP_CTS1 / INT_CTS1    | click INT net (UART1 RTS not muxed)          | —                                         |
| GP8  | 6        | SENS_SPI.MISO                    | SPI1 RX (MISO)                               | `spi1_default`                            |
| GP9  | 7        | SPIS_SS / RP_SPIS_SS             | **ESP32-C3 CS** (C3_SS0) — see note below    | (not in DTS — ESP block)                  |
| GP10 | 8        | SENS_SPI.SCK                     | SPI1 SCK                                     | `spi1_default`                            |
| GP11 | 9        | SENS_SPI.MOSI                    | SPI1 TX (MOSI)                               | `spi1_default`                            |
| GP12 | 11       | SCI3_TXD / RP_TX0                | UART0 TX (general purpose)                   | `uart0_default`                           |
| GP13 | 12       | SCI3_RXD / RP_RX0                | UART0 RX (general purpose)                   | `uart0_default`                           |
| GP14 | 13       | RP_S2 / RGB_D_IN                 | WS2812 data via PIO1                         | `pio1_ws2812_default`, `ws2812` node      |
| GP15 | 14       | NC                               | —                                            | —                                         |
| GP16 | 16       | SCI2_TXD / RP_TX2                | NC (would-be UART2 TX)                       | —                                         |
| GP17 | 17       | SCI2_RXD / RP_RX2                | NC                                           | —                                         |
| GP18 | 18       | SCI2_CTS / RP_CTS2               | NC                                           | —                                         |
| GP19 | 19       | SCI2_RTS / RP_RTS2               | NC                                           | —                                         |
| GP20 | 20       | SENS_I2C.SDA                     | I2C0 SDA                                     | `i2c0_default`                            |
| GP21 | 21       | SENS_I2C.SCL                     | I2C0 SCL                                     | `i2c0_default`                            |
| GP22 | 22       | NC                               | —                                            | —                                         |
| GP23 | 23       | NC                               | —                                            | —                                         |
| GP24 | 25       | NC                               | —                                            | —                                         |
| GP25 | 26       | NC                               | —                                            | —                                         |
| GP26 | 27       | NC                               | —                                            | —                                         |
| GP27 | 28       | NC                               | —                                            | —                                         |
| GP28 | 29       | NC                               | —                                            | —                                         |
| GP29 | 36       | NC                               | —                                            | —                                         |
| GP30 | 38       | RP_PWM0 / AUDIO_IN               | Buzzer PWM (channel 7A)                      | `pwm_ch7a_default`                        |
| GP31 | 39       | RP_PWM1                          | mikroBUS PWM (channel 7B)                    | `pwm_ch7b_default`                        |
| GP32 | 40       | RP_PWM2                          | NC (channel 8A reserved in pinctrl comment)  | —                                         |
| GP33 | 42       | NC                               | —                                            | —                                         |
| GP34 | 43       | RP_S0                            | mikroBUS RST                                 | (no DTS node — app-driven)                |
| GP35 | 44       | RP_S1 / RELAY_S_IN               | 5V relay enable                              | `relay_5v` regulator (`gpio0_hi 3`)       |
| GP36 | 45       | NC                               | —                                            | —                                         |
| GP37 | 46       | RP_S3                            | spare                                        | —                                         |
| GP38 | 47       | RP_S4                            | spare                                        | —                                         |
| GP39 | 48       | RP_S5                            | spare                                        | —                                         |
| GP40 | 49       | ADC0 / AN1                       | ADC ch0 (mikroBUS AN)                        | `&adc channel@0`                          |
| GP41 | 52       | ADC1                             | ADC ch1                                      | `&adc channel@1`                          |
| GP42 | 53       | ADC2                             | ADC ch2                                      | `&adc channel@2`                          |
| GP43 | 54       | ADC3                             | ADC ch3                                      | `&adc channel@3`                          |
| GP44 | 55       | ADC4                             | NC                                           | —                                         |
| GP45 | 56       | ADC5                             | NC                                           | —                                         |
| GP46 | 57       | ADC6                             | NC                                           | —                                         |
| GP47 | 58       | ADC7                             | NC                                           | —                                         |

## Notes

- **GP6/GP7 net double-duty.** These carry UART1 hardware flow control AND the mikroBUS click CS/INT nets via sheet-1 net aliasing
  (`RP_RTS1 ↔ CS_RTS1`, `RP_CTS1 ↔ INT_CTS1`). With `hw-flow-control` enabled on UART1 the click CS line is being driven by the UART hardware;
  the `cs-gpios` entry on `&spi1` for GP6 represents the click CS but has no SPI peripheral child node attached. If a click module needs CS/INT, UART1 must be reconfigured (drop hw flow control and drive GP6/GP7 as GPIOs).
- **GP1/GP9 chip selects are swapped relative to the net groupings.**
  - **GP1** (`RP_SPIM_SS0`) is the **BME280** chip select, despite the `SPIM`
    name and its position in the GP0–GP3 `C3_SPI` block.
  - **GP9** (`RP_SPIS_SS`) is the **ESP32-C3** chip select — the net lands on
    `C3_SS0` — despite the `SPIS` ("sensor SS") name and its position beside
    the `SENS_SPI` pins on GP8/10/11.

  Wiring `&spi1`'s `cs-gpios` to GP9 leaves the BME280 permanently
  deselected: every register read returns `0x00`, so the driver's chip-ID
  check fails and `device_is_ready()` reports `-ENODEV`. The SPI transfer
  itself still completes normally (no timeout), which makes this look like a
  dead sensor rather than a chip-select fault. It also asserts the ESP32-C3's
  select line on every sensor transaction.
- **GPIO bank offsets.** The `gpio0_hi` controller covers GP32–GP47 with
  offset = `GPIO − 32`. Examples:
  - GP35 (relay) = `gpio0_hi 3`
  - GP34 (mikroBUS RST) = `gpio0_hi 2`
  - GP14 (WS2812) = `gpio0 14` (low bank, no offset)
- **Chip pin numbers** are from the RP2350B QFN-80 package. Gaps (e.g. 24, 30/31, 41, 50/51, 60, 68/69, 76) are power, crystal, USB, reset, or SWD pins — not GPIOs.
- **SPI controller.** The SENS_SPI bus on GP8/10/11 maps to the **SPI1** hardware controller; GP8/10/11 are SPI1 native function pins. The BME280 chip select is GP1, driven as a GPIO (see the GP1/GP9 note above).
