// SPDX-License-Identifier: Apache-2.0
//
// Copyright (c) 2026 Blue Clover Devices
//
// Steady solid-color RGB LED demo for the Bento demo boards.
//
// Drives the LTST-E683CEGBW WS2812-compatible RGB LED through Zephyr's
// WS2812 PIO led_strip driver (worldsemi,ws2812-rpi_pico-pio) via the
// "led-strip" devicetree alias, and holds it at a single steady color
// (set at build time via SOLID_COLOR_R/G/B).

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(blinky_solid, LOG_LEVEL_INF);

#define STRIP_NODE DT_ALIAS(led_strip)
#define NUM_LEDS   DT_PROP(STRIP_NODE, chain_length)

#ifndef SOLID_COLOR_R
#define SOLID_COLOR_R 0x00
#endif
#ifndef SOLID_COLOR_G
#define SOLID_COLOR_G 0x00
#endif
#ifndef SOLID_COLOR_B
#define SOLID_COLOR_B 0x00
#endif

static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);

static struct led_rgb pixels[NUM_LEDS];

int main(void)
{
	int rc;

	LOG_INF("WS2812 solid-color blinky | %d LED(s) | R=%02x G=%02x B=%02x",
		NUM_LEDS, SOLID_COLOR_R, SOLID_COLOR_G, SOLID_COLOR_B);

	if (!device_is_ready(strip)) {
		LOG_ERR("LED strip device %s not ready", strip->name);
		return -ENODEV;
	}

	for (size_t i = 0; i < NUM_LEDS; i++) {
		pixels[i] = (struct led_rgb){
			.r = SOLID_COLOR_R,
			.g = SOLID_COLOR_G,
			.b = SOLID_COLOR_B,
		};
	}

	while (1) {
		rc = led_strip_update_rgb(strip, pixels, NUM_LEDS);
		if (rc) {
			LOG_ERR("led_strip_update_rgb failed: %d", rc);
		}

		k_msleep(1000);
	}

	return 0;
}
