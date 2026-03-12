// SPDX-License-Identifier: Apache-2.0
//
// Copyright (c) 2026 Blue Clover Devices
//
// LED bitbang demo for Bento demo board.
// Drives the LTST-E683CEGBW addressable LED on GPIO36 (gpio0_hi pin 4)
// using direct SIO register writes with cycle-counter delays.
//
// Timing from LTST-E683CEGBW datasheet:
//   T0H = 300ns (+/-150ns), T0L = 900ns (+/-150ns)
//   T1H = 900ns (+/-150ns), T1L = 300ns (+/-150ns)
//   Reset > 250us
//   Color order: RGB, MSB first

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <hardware/structs/sio.h>
#include <cmsis_core.h>

LOG_MODULE_REGISTER(led_bitbang, LOG_LEVEL_INF);

#define GPIO_HI_NODE DT_NODELABEL(gpio0_hi)
#define LED_PIN 4  /* gpio0_hi offset 4 = GPIO36 (RP_S2 / RGB_D_IN) */
#define LED_MASK (1u << LED_PIN)

/*
 * At 150 MHz: 1 cycle = 6.67 ns
 *   300 ns = 45 cycles
 *   900 ns = 135 cycles
 *
 * Subtract overhead for the register write (~2-3 cycles).
 */
#define CYCLES_300NS 42
#define CYCLES_900NS 132

static inline void dwt_init(void)
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static inline void delay_cycles(uint32_t cycles)
{
	uint32_t start = DWT->CYCCNT;
	while ((DWT->CYCCNT - start) < cycles) {
	}
}

static inline void pin_high(void)
{
	sio_hw->gpio_hi_set = LED_MASK;
}

static inline void pin_low(void)
{
	sio_hw->gpio_hi_clr = LED_MASK;
}

static void led_send_byte(uint8_t byte)
{
	for (int i = 7; i >= 0; i--) {
		if ((byte >> i) & 1) {
			pin_high();
			delay_cycles(CYCLES_900NS);
			pin_low();
			delay_cycles(CYCLES_300NS);
		} else {
			pin_high();
			delay_cycles(CYCLES_300NS);
			pin_low();
			delay_cycles(CYCLES_900NS);
		}
	}
}

static void send_pixel(uint8_t r, uint8_t g, uint8_t b)
{
	unsigned int key = irq_lock();
	led_send_byte(r);
	led_send_byte(g);
	led_send_byte(b);
	irq_unlock(key);
}

static void led_reset(void)
{
	pin_low();
	k_usleep(300);
}

struct color {
	uint8_t r, g, b;
	const char *name;
};

// NOTE: this is still QUITE bright in my testing
// but timing could still be off here.
static const struct color colors[] = {
	{ 0x08, 0x00, 0x00, "RED" },
	{ 0x00, 0x08, 0x00, "GREEN" },
	{ 0x00, 0x00, 0x08, "BLUE" },
	{ 0x00, 0x00, 0x00, "OFF" },
};

int main(void)
{
	const struct device *gpio_dev;
	int rc;

	/* Use Zephyr GPIO driver just for pin configuration (direction, pad) */
	/* The GPIO APIs are too slow for our goal */
	gpio_dev = DEVICE_DT_GET(GPIO_HI_NODE);
	if (!device_is_ready(gpio_dev)) {
		LOG_ERR("gpio0_hi not ready");
		return -ENODEV;
	}

	rc = gpio_pin_configure(gpio_dev, LED_PIN, GPIO_OUTPUT_LOW);
	if (rc) {
		LOG_ERR("Failed to configure LED pin: %d", rc);
		return rc;
	}

	dwt_init();
	led_reset();

	size_t idx = 0;

	while (1) {
		const struct color *c = &colors[idx];
		LOG_INF("Color: %s (R=%02x G=%02x B=%02x)", c->name, c->r, c->g, c->b);

		send_pixel(c->r, c->g, c->b);
		led_reset();

		idx = (idx + 1) % ARRAY_SIZE(colors);
		k_msleep(2000);
	}

	return 0;
}
