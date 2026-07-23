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
 * Buzzer AUDIO_IN is on GP30, which the board DTS muxes as PWM slice 7,
 * side A (pwm_ch7a_default). The RP2350 PWM cells are addressed as:
 * channel_A = slice*2, channel_B = slice*2+1, so slice 7 side A = 14.
 *
 * (The previous value 13 = slice 6B / GP29 was never pinned out on this
 * board, so pwm_set() succeeded but drove no connected pin -- silent.)
 */
#define BUZZER_PWM_NODE DT_ALIAS(pwm_buzzer)
#define BUZZER_PWM_CHANNEL 14

/* Startup note-sweep timing: per-note tone length and inter-note gap. */
#define SWEEP_NOTE_MS  120U
#define PAUSE_DURATION K_MSEC(40)

static const struct device *const buzzer_pwm = DEVICE_DT_GET(BUZZER_PWM_NODE);

static const uint32_t note_freqs_hz[] = {
	988,         /* B */
	880,         /* A */
	784,         /* G */
	699,         /* F */
	659,         /* E */
	587,         /* D */
	523,         /* C */
};

int app_buzzer_setup(void)
{
	if (!device_is_ready(buzzer_pwm)) {
		printk("Buzzer PWM device not ready\n");
		return -ENODEV;
	}

	return 0;
}

int app_buzzer_beep(uint32_t freq_hz, uint32_t duration_ms)
{
	uint32_t period_us;
	int rc;

	if (!device_is_ready(buzzer_pwm)) {
		return -ENODEV;
	}

	if (freq_hz == 0U) {
		return -EINVAL;
	}

	period_us = 1000000U / freq_hz;

	/* 50% duty cycle tone. */
	rc = pwm_set(buzzer_pwm, BUZZER_PWM_CHANNEL,
		     PWM_USEC(period_us), PWM_USEC(period_us / 2U), 0);
	if (rc) {
		printk("Buzzer PWM set failed: %d\n", rc);
		return rc;
	}

	k_sleep(K_MSEC(duration_ms));

	/* Silence. */
	return pwm_set(buzzer_pwm, BUZZER_PWM_CHANNEL, PWM_USEC(period_us), 0, 0);
}

int app_buzzer_startup_sweep(void)
{
	int rc;

	if (!device_is_ready(buzzer_pwm)) {
		printk("Buzzer PWM device not ready\n");
		return -ENODEV;
	}

	for (int i = 0; i < ARRAY_SIZE(note_freqs_hz); i++) {
		rc = app_buzzer_beep(note_freqs_hz[i], SWEEP_NOTE_MS);
		if (rc) {
			return rc;
		}

		k_sleep(PAUSE_DURATION);
	}

	return 0;
}
