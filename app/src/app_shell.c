// SPDX-License-Identifier: Apache-2.0

//
// Copyright (c) 2026 Blue Clover Devices
//

/*
 * app_shell.c - Board-specific shell commands for exercising Bento
 * peripherals over the USB CDC-ACM shell.
 *
 * Generic buses have their own built-in shell modules (gpio, i2c, spi,
 * sensor, adc). The commands here cover the board-specific peripherals
 * that lack a stock module: the PWM buzzer and the WS2812 RGB LED. They
 * are grouped under a "bento" root command.
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/drivers/sensor.h>

#include "app_buzzer.h"
#include "app_ledstrip.h"
#include "app_sensor.h"

/* ---- buzzer ------------------------------------------------------------ */

static int cmd_buzzer_beep(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t freq_hz = 880U;
	uint32_t duration_ms = 200U;
	int rc;

	if (argc >= 2) {
		freq_hz = (uint32_t)strtoul(argv[1], NULL, 0);
	}
	if (argc >= 3) {
		duration_ms = (uint32_t)strtoul(argv[2], NULL, 0);
	}

	if (freq_hz == 0U) {
		shell_error(sh, "frequency must be > 0");
		return -EINVAL;
	}

	rc = app_buzzer_beep(freq_hz, duration_ms);
	if (rc) {
		shell_error(sh, "beep failed: %d", rc);
		return rc;
	}

	shell_print(sh, "beeped %u Hz for %u ms", freq_hz, duration_ms);
	return 0;
}

static int cmd_buzzer_sweep(const struct shell *sh, size_t argc, char **argv)
{
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	rc = app_buzzer_startup_sweep();
	if (rc) {
		shell_error(sh, "sweep failed: %d", rc);
		return rc;
	}

	shell_print(sh, "sweep complete");
	return 0;
}

/*
 * Tunes built into the firmware, played by name ("bento buzzer play ode").
 * The first one is played when no argument is given.
 */
static const struct {
	const char *name;
	const char *rtttl;
} builtin_tunes[] = {
	/* "Twinkle, Twinkle, Little Star", first line (traditional). 14 notes, 8 s. */
	{ "twinkle", "twinkle:d=4,o=5,b=120:c,c,g,g,a,a,2g,f,f,e,e,d,d,2c" },
	/* Beethoven, "Ode to Joy", opening (public domain). 15 notes, 7 s. */
	{ "ode", "ode:d=4,o=5,b=140:e,e,f,g,g,f,e,d,c,c,d,e,e.,8d,2d" },
};

/* Resolve the play argument: RTTTL if it has a ':', else a built-in name. */
static const char *find_tune(const char *arg)
{
	if (arg == NULL) {
		return builtin_tunes[0].rtttl;
	}
	if (strchr(arg, ':') != NULL) {
		return arg;
	}
	for (size_t i = 0; i < ARRAY_SIZE(builtin_tunes); i++) {
		if (strcmp(arg, builtin_tunes[i].name) == 0) {
			return builtin_tunes[i].rtttl;
		}
	}
	return NULL;
}

/*
 * Parsed notes for "bento buzzer play". Static rather than on the shell
 * thread's stack; the shell runs one command at a time. A full command
 * line (CONFIG_SHELL_CMD_BUFF_SIZE) of the shortest notes ("c,") fits.
 */
#define TUNE_MAX_NOTES 256
static struct rtttl_note tune_notes[TUNE_MAX_NOTES];

static int cmd_buzzer_play(const struct shell *sh, size_t argc, char **argv)
{
	const char *tune = find_tune((argc >= 2) ? argv[1] : NULL);
	struct rtttl_error err = { 0 };
	int count;
	int rc;

	if (tune == NULL) {
		shell_error(sh, "unknown tune '%s'; built-in tunes:", argv[1]);
		for (size_t i = 0; i < ARRAY_SIZE(builtin_tunes); i++) {
			shell_error(sh, "  %s", builtin_tunes[i].name);
		}
		return -ENOENT;
	}

	/* Parse the whole tune first, so a bad string plays nothing. */
	count = rtttl_parse(tune, tune_notes, ARRAY_SIZE(tune_notes), &err);
	if (count < 0) {
		shell_error(sh, "bad rtttl: %s at char %u", err.msg,
			    (unsigned int)err.pos);
		return count;
	}

	rc = app_buzzer_play(tune_notes, (size_t)count);
	if (rc) {
		shell_error(sh, "play failed: %d", rc);
		return rc;
	}

	/* count includes pauses: every RTTTL entry is a note. */
	shell_print(sh, "played %d notes", count);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_buzzer,
	SHELL_CMD_ARG(beep, NULL,
		"Sound the buzzer.\n"
		"Usage: bento buzzer beep [freq_hz] [duration_ms]",
		cmd_buzzer_beep, 1, 2),
	SHELL_CMD(sweep, NULL, "Play the startup note sweep.", cmd_buzzer_sweep),
	SHELL_CMD_ARG(play, NULL,
		"Play a tune; blocks until it finishes.\n"
		"Usage: bento buzzer play [twinkle | ode | \"<rtttl>\"]\n"
		"  <rtttl>  e.g. \"name:d=4,o=5,b=120:c,d,e\" (quote it)\n"
		"With no argument, plays twinkle.\n"
		"Prints \"played <n> notes\"; n counts pauses (p) too.",
		cmd_buzzer_play, 1, 1),
	SHELL_SUBCMD_SET_END
);

/* ---- ledstrip ---------------------------------------------------------- */

static int cmd_ledstrip_set(const struct shell *sh, size_t argc, char **argv)
{
	unsigned long r, g, b;
	int rc;

	r = strtoul(argv[1], NULL, 0);
	g = strtoul(argv[2], NULL, 0);
	b = strtoul(argv[3], NULL, 0);

	if (r > 255 || g > 255 || b > 255) {
		shell_error(sh, "each channel must be 0..255");
		return -EINVAL;
	}

	rc = app_ledstrip_set_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b);
	if (rc) {
		shell_error(sh, "set failed: %d", rc);
		return rc;
	}

	shell_print(sh, "ledstrip set to %lu,%lu,%lu", r, g, b);
	return 0;
}

static int cmd_ledstrip_off(const struct shell *sh, size_t argc, char **argv)
{
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	rc = app_ledstrip_off();
	if (rc) {
		shell_error(sh, "off failed: %d", rc);
		return rc;
	}

	shell_print(sh, "ledstrip off");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_ledstrip,
	SHELL_CMD_ARG(set, NULL,
		"Set every pixel to an RGB color.\n"
		"Usage: bento ledstrip set <r> <g> <b>   (0..255 each)",
		cmd_ledstrip_set, 4, 0),
	SHELL_CMD(off, NULL, "Turn the LED strip off.", cmd_ledstrip_off),
	SHELL_SUBCMD_SET_END
);

/* ---- sensor ------------------------------------------------------------ */

static int cmd_sensor_read(const struct shell *sh, size_t argc, char **argv)
{
	struct sensor_value temp, hum, press;
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	rc = app_sensor_environmental_read(&temp, &hum, &press);
	if (rc) {
		shell_error(sh, "read failed: %d", rc);
		return rc;
	}

	shell_print(sh, "temp: %.2f C", sensor_value_to_double(&temp));
	shell_print(sh, "hum:  %.2f %%RH", sensor_value_to_double(&hum));
	shell_print(sh, "pres: %.2f kPa", sensor_value_to_double(&press));
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_sensor,
	SHELL_CMD(read, NULL, "Read the BME280 (temp/humidity/pressure).",
		  cmd_sensor_read),
	SHELL_SUBCMD_SET_END
);

/* ---- root -------------------------------------------------------------- */

SHELL_STATIC_SUBCMD_SET_CREATE(sub_bento,
	SHELL_CMD(buzzer, &sub_buzzer, "Buzzer (PWM) commands.", NULL),
	SHELL_CMD(ledstrip, &sub_ledstrip, "WS2812 RGB LED commands.", NULL),
	SHELL_CMD(sensor, &sub_sensor, "BME280 environmental sensor commands.",
		  NULL),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(bento, &sub_bento, "Bento board peripheral commands", NULL);
