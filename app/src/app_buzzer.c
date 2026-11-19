// SPDX-License-Identifier: Apache-2.0

//
// Copyright (c) 2019-2026 Blue Clover Devices
//

/* app_buzzer.c - Buzzer interface via PWM on RP2350B */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/sys/util.h>

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

/* Tune playback: silent gap at the end of each note, 1/10 of it, capped. */
#define NOTE_GAP_DIVISOR 10U
#define NOTE_GAP_MAX_MS  30U

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

/* Start a 50% duty tone. Leaves the PWM running until tone_off(). */
static int tone_on(uint32_t freq_hz)
{
	uint32_t period_us = 1000000U / freq_hz;
	int rc;

	rc = pwm_set(buzzer_pwm, BUZZER_PWM_CHANNEL,
		     PWM_USEC(period_us), PWM_USEC(period_us / 2U), 0);
	if (rc) {
		printk("Buzzer PWM set failed: %d\n", rc);
	}
	return rc;
}

static int tone_off(uint32_t freq_hz)
{
	return pwm_set(buzzer_pwm, BUZZER_PWM_CHANNEL,
		       PWM_USEC(1000000U / freq_hz), 0, 0);
}

int app_buzzer_beep(uint32_t freq_hz, uint32_t duration_ms)
{
	int rc;

	if (!device_is_ready(buzzer_pwm)) {
		return -ENODEV;
	}

	if (freq_hz == 0U) {
		return -EINVAL;
	}

	rc = tone_on(freq_hz);
	if (rc) {
		return rc;
	}

	k_sleep(K_MSEC(duration_ms));

	return tone_off(freq_hz);
}

int app_buzzer_play(const struct rtttl_note *notes, size_t count)
{
	int rc;

	if (!device_is_ready(buzzer_pwm)) {
		return -ENODEV;
	}

	for (size_t i = 0; i < count; i++) {
		uint32_t ms = notes[i].duration_ms;
		uint32_t freq = notes[i].freq_hz;
		uint32_t gap;

		if (freq == 0U) {
			k_sleep(K_MSEC(ms));
			continue;
		}

		/*
		 * Cut each tone short by a small gap so repeated notes
		 * (e.g. "c,c") are heard as two notes, not one long one.
		 */
		gap = MIN(ms / NOTE_GAP_DIVISOR, NOTE_GAP_MAX_MS);

		rc = tone_on(freq);
		if (rc) {
			return rc;
		}
		k_sleep(K_MSEC(ms - gap));
		rc = tone_off(freq);
		if (rc) {
			return rc;
		}
		k_sleep(K_MSEC(gap));
	}

	return 0;
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
