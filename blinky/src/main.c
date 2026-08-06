// SPDX-License-Identifier: Apache-2.0
//
// Copyright (c) 2026 Blue Clover Devices
//
// RGB LED demo for the Bento demo boards.
//
// Drives the LTST-E683CEGBW WS2812-compatible RGB LED through Zephyr's
// WS2812 PIO led_strip driver (worldsemi,ws2812-rpi_pico-pio) via the
// "led-strip" devicetree alias. The data pin differs per board:
//   - Bento1: GPIO36 (gpio0_hi pin 4, high GPIO bank)
//   - Bento2: GPIO14 (gpio0_lo pin 14, low GPIO bank)
// The high-bank pin needs the RP2350 PIO GPIOBASE relocated to 16; that is
// handled inside the driver (see patches/zephyr/0001-...), so this example is
// board-agnostic and just cycles the LED through a few colors.

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/logging/log.h>

// Generated from ./VERSION by the build; see the Versioning section of the
// top-level README. The version is logged at startup here and is also
// available interactively via the `version` shell command (blinky_shell.c).
#include <zephyr/app_version.h>

// APP_BUILD_VERSION is generated as a bare token (the `git describe` output,
// unquoted), so it has to be stringified before it can be printed.
#define BLINKY_STR(s)  #s
#define BLINKY_XSTR(s) BLINKY_STR(s)

LOG_MODULE_REGISTER(blinky, LOG_LEVEL_INF);

#define STRIP_NODE DT_ALIAS(led_strip)
#define NUM_LEDS   DT_PROP(STRIP_NODE, chain_length)

static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);

static struct led_rgb pixels[NUM_LEDS];

struct color {
	uint8_t r, g, b;
	const char *name;
};

// Kept dim on purpose: the LTST-E683CEGBW is bright even at low values.
static const struct color colors[] = {
	{ 0x10, 0x00, 0x00, "RED" },
	{ 0x00, 0x10, 0x00, "GREEN" },
	{ 0x00, 0x00, 0x10, "BLUE" },
	{ 0x00, 0x00, 0x00, "OFF" },
};

int main(void)
{
	int rc;

	LOG_INF("Bento blinky %s (build %s)", APP_VERSION_STRING,
		BLINKY_XSTR(APP_BUILD_VERSION));
	LOG_INF("WS2812 led_strip blinky | %d LED(s)", NUM_LEDS);

	if (!device_is_ready(strip)) {
		LOG_ERR("LED strip device %s not ready", strip->name);
		return -ENODEV;
	}

	size_t idx = 0;

	while (1) {
		const struct color *c = &colors[idx];

		// Drive every LED in the chain to the same color.
		for (size_t i = 0; i < NUM_LEDS; i++) {
			pixels[i] = (struct led_rgb){ .r = c->r, .g = c->g, .b = c->b };
		}

		rc = led_strip_update_rgb(strip, pixels, NUM_LEDS);
		if (rc) {
			LOG_ERR("led_strip_update_rgb failed: %d", rc);
		}

		idx = (idx + 1) % ARRAY_SIZE(colors);
		k_msleep(1000);
	}

	return 0;
}
