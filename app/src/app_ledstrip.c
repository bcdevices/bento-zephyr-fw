// SPDX-License-Identifier: Apache-2.0

//
// Copyright (c) 2019-2026 Blue Clover Devices
//

/*
 * app_ledstrip.c - WS2812 RGB LED control on the Bento boards.
 *
 * Drives the LTST-E683CEGBW WS2812-compatible RGB LED through Zephyr's
 * WS2812 PIO led_strip driver via the "led-strip" devicetree alias. On
 * Bento2 the LED 5V rail is gated by the relay_5v boot-on regulator (so no
 * GPIO power-enable is needed here); the data pin is GPIO14 (low GPIO bank).
 * On Bento1 the data pin is GPIO36 (high bank); the RP2350 PIO GPIOBASE is
 * relocated inside the driver (see patches/zephyr/0001-...).
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>

#include "app_ledstrip.h"

#define STRIP_NODE DT_ALIAS(led_strip)
#define NUM_LEDS   DT_PROP(STRIP_NODE, chain_length)

static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);
static size_t ledstrip_counter;

static const struct led_rgb colors[] = {
	{ .r = 0x1f, .g = 0x00, .b = 0x00 },   /* red */
	{ .r = 0x00, .g = 0x1f, .b = 0x00 },   /* green */
	{ .r = 0x00, .g = 0x00, .b = 0x1f },   /* blue */
};

static struct led_rgb pixels[NUM_LEDS];

int app_ledstrip_setup(void)
{
	if (!device_is_ready(strip)) {
		printk("LED strip device %s not ready\n", strip->name);
		return -ENODEV;
	}

	ledstrip_counter = 0;

	return 0;
}

int app_ledstrip_run(void)
{
	const struct led_rgb *c = &colors[ledstrip_counter % ARRAY_SIZE(colors)];

	ledstrip_counter++;

	return app_ledstrip_set_rgb(c->r, c->g, c->b);
}

int app_ledstrip_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
	if (!device_is_ready(strip)) {
		return -ENODEV;
	}

	for (size_t i = 0; i < NUM_LEDS; i++) {
		pixels[i].r = r;
		pixels[i].g = g;
		pixels[i].b = b;
	}

	return led_strip_update_rgb(strip, pixels, NUM_LEDS);
}

int app_ledstrip_off(void)
{
	return app_ledstrip_set_rgb(0, 0, 0);
}
