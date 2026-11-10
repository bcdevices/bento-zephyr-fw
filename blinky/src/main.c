// SPDX-License-Identifier: Apache-2.0
//
// Copyright (c) 2026 Blue Clover Devices
//
// Blinky for the Bento demo board and other demo boards (e.g. Nucleo).
//
// The LED backend is chosen from the devicetree:
//
// - "led-strip" alias (Bento): drives the LTST-E683CEGBW WS2812-compatible
//   RGB LED through Zephyr's led_strip API, cycling through a few colors.
// - "led0" alias (most Zephyr boards, e.g. nucleo_f439zi LD1): toggles a
//   GPIO LED.
//
// Either way the LED changes state once per second.

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

// Version macros (generated from ./VERSION; see the Versioning section of
// the top-level README) and the command transport. The version is logged at
// startup and is also available via the `version` command.
#include "blinky.h"

LOG_MODULE_REGISTER(blinky, LOG_LEVEL_INF);

#define BLINK_PERIOD_MS 1000

#if DT_HAS_ALIAS(led_strip)

#include <zephyr/drivers/led_strip.h>

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

static size_t color_idx;

static int led_init(void)
{
	LOG_INF("WS2812 led_strip blinky | %d LED(s)", NUM_LEDS);

	if (!device_is_ready(strip)) {
		LOG_ERR("LED strip device %s not ready", strip->name);
		return -ENODEV;
	}

	return 0;
}

static void led_step(void)
{
	const struct color *c = &colors[color_idx];
	int rc;

	// Drive every LED in the chain to the same color.
	for (size_t i = 0; i < NUM_LEDS; i++) {
		pixels[i] = (struct led_rgb){ .r = c->r, .g = c->g, .b = c->b };
	}

	rc = led_strip_update_rgb(strip, pixels, NUM_LEDS);
	if (rc) {
		LOG_ERR("led_strip_update_rgb failed: %d", rc);
	}

	color_idx = (color_idx + 1) % ARRAY_SIZE(colors);
}

#elif DT_HAS_ALIAS(led0)

#include <zephyr/drivers/gpio.h>

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static int led_init(void)
{
	int rc;

	LOG_INF("GPIO blinky | led0 = %s pin %d", led.port->name, led.pin);

	if (!gpio_is_ready_dt(&led)) {
		LOG_ERR("LED GPIO %s not ready", led.port->name);
		return -ENODEV;
	}

	rc = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	if (rc) {
		LOG_ERR("gpio_pin_configure_dt failed: %d", rc);
	}

	return rc;
}

static void led_step(void)
{
	int rc = gpio_pin_toggle_dt(&led);

	if (rc) {
		LOG_ERR("gpio_pin_toggle_dt failed: %d", rc);
	}
}

#else
#error "blinky needs a \"led-strip\" or \"led0\" devicetree alias"
#endif

int main(void)
{
	int rc;

	// printk rather than LOG_INF so the identity line also appears on small
	// boards built without the logging subsystem.
	printk("blinky %s (build %s) on %s\n", APP_VERSION_STRING,
	       BLINKY_XSTR(APP_BUILD_VERSION), CONFIG_BOARD_TARGET);

	rc = led_init();
	if (rc) {
		return rc;
	}

	rc = blinky_cmd_init();
	if (rc) {
		LOG_ERR("command transport init failed: %d", rc);
	}

	while (1) {
		led_step();
		blinky_cmd_wait(K_MSEC(BLINK_PERIOD_MS));
	}

	return 0;
}
