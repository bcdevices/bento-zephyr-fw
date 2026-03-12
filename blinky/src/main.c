// SPDX-License-Identifier: Apache-2.0
//
// Copyright (c) 2026 Blue Clover Devices
//
// Blinky for the Bento demo board.
// Asserts RELAY_S_IN (GPIO45) to enable the RGB_5V rail, then cycles
// the WS2812-compatible RGB LED (LTST-E683CEGBW) through
// red -> green -> blue -> off, repeating.

#include <zephyr/kernel.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#define STRIP_NODE DT_ALIAS(led_strip)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_NODE, chain_length)

#define RELAY_5V_NODE DT_NODELABEL(relay_5v)

#define DELAY_MS 500

static const struct device *strip = DEVICE_DT_GET(STRIP_NODE);
static const struct gpio_dt_spec relay_5v = GPIO_DT_SPEC_GET(RELAY_5V_NODE, gpios);

static struct led_rgb colors[] = {
	{ .r = 0x1f, .g = 0x00, .b = 0x00, },   /* red */
	{ .r = 0x00, .g = 0x1f, .b = 0x00, },   /* green */
	{ .r = 0x00, .g = 0x00, .b = 0x1f, },   /* blue */
	{ .r = 0x00, .g = 0x00, .b = 0x00 }, /* off   */
};

int main(void)
{
	/*
	 * Enable RGB_5V rail via relay (RELAY_S_IN = GPIO45 / RP_S1).
	 * Try active high first (2s), then active low (2s), then settle on
	 * whichever made the LED work. Once polarity is confirmed, remove
	 * this toggling and fix GPIO_ACTIVE_HIGH/LOW in the DTS.
	 */
	if (!gpio_is_ready_dt(&relay_5v)) {
		return -ENODEV;
	}
	gpio_pin_configure_dt(&relay_5v, GPIO_OUTPUT_ACTIVE);
	gpio_pin_set_dt(&relay_5v, 1);
	k_msleep(50);

	if (!device_is_ready(strip)) {
		return -ENODEV;
	}

	size_t color_idx = 0;
	struct led_rgb pixel;

	while (1) {
		pixel = colors[color_idx];
		led_strip_update_rgb(strip, &pixel, STRIP_NUM_PIXELS);

		color_idx = (color_idx + 1) % ARRAY_SIZE(colors);
		k_msleep(DELAY_MS);
	}

	return 0;
}
