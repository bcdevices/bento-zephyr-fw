// SPDX-License-Identifier: Apache-2.0

//
// Copyright (c) 2019-2026 Blue Clover Devices
//

/* main.c - Application main entry point */

#include <zephyr/kernel.h>

#include "app_buzzer.h"
#include "app_sensor.h"

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

	return 0;
}
