// SPDX-License-Identifier: Apache-2.0

//
// Copyright (c) 2019-2026 Blue Clover Devices
//

/* main.c - Application main entry point */

#include <zephyr/kernel.h>

#include "app_buzzer.h"
#include "app_sensor.h"
#include "app_ledstrip.h"

/*
 * Peripherals are exercised interactively through the USB CDC-ACM shell
 * (see app_shell.c and app.overlay). main() just brings the drivers up and
 * gives a brief visual power-on indication; the shell runs on its own backend
 * thread and waits for a host to open the USB serial port.
 */
int main(void)
{
	int err;

	err = app_sensor_environmental_setup();
	if (err) {
		printk("app_sensor_environmental_setup() failed, err=%d\n", err);
	}

	err = app_buzzer_setup();
	if (err) {
		printk("app_buzzer_setup() failed, err=%d\n", err);
	}

	err = app_ledstrip_setup();
	if (err) {
		printk("app_ledstrip_setup() failed, err=%d\n", err);
	}

	app_ledstrip_set_rgb(0x00, 0x1f, 0x00);

	printk("Bento peripheral shell ready. Connect over USB serial.\n");

	return 0;
}
