// SPDX-License-Identifier: Apache-2.0

//
// Copyright (c) 2019-2026 Blue Clover Devices
//

/* app_buzzer.c - Buzzer interface via PWM on RP2350B */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>

#include "app_buzzer.h"

/*
 * PWM channel 6B on GPIO29 (AUDIO_IN).
 * The RP2350 PWM cells are addressed as: channel_A = slice*2, channel_B = slice*2+1.
 * Slice 6, side B = channel 13.
 */
#define BUZZER_PWM_NODE DT_ALIAS(pwm_buzzer)
#define BUZZER_PWM_CHANNEL 13

#define BEEP_DURATION  K_MSEC(10)
#define PAUSE_DURATION K_MSEC(1)

static const uint32_t note_periods_us[] = {
	1000000U / 988,         /* B */
	1000000U / 880,         /* A */
	1000000U / 784,         /* G */
	1000000U / 699,         /* F */
	1000000U / 659,         /* E */
	1000000U / 587,         /* D */
	1000000U / 523,         /* C */
};

int app_buzzer_setup(void)
{
	const struct device *pwm_dev = DEVICE_DT_GET(BUZZER_PWM_NODE);
	int rc;

	if (!device_is_ready(pwm_dev)) {
		printk("Buzzer PWM device not ready\n");
		return -ENODEV;
	}

	for (int i = 0; i < ARRAY_SIZE(note_periods_us); i++) {
		uint32_t period_us = note_periods_us[i];

		rc = pwm_set(pwm_dev, BUZZER_PWM_CHANNEL,
			     PWM_USEC(period_us), PWM_USEC(period_us / 2U),
			     0);
		if (rc) {
			printk("Buzzer PWM set failed: %d\n", rc);
			return rc;
		}
		k_sleep(BEEP_DURATION);

		/* Silence between notes */
		pwm_set(pwm_dev, BUZZER_PWM_CHANNEL,
			PWM_USEC(period_us), 0, 0);
		k_sleep(PAUSE_DURATION);
	}

	return 0;
}
