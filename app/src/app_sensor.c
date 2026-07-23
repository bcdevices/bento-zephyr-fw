// SPDX-License-Identifier: Apache-2.0

//
// Copyright (c) 2019-2026 Blue Clover Devices
//

/* app_sensor.c - BME280 environmental sensor interface (SPI) */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

#include "app_sensor.h"

#define BME280_NODE DT_NODELABEL(bme280)

static const struct device *const bme280 = DEVICE_DT_GET(BME280_NODE);

/*
 * Readiness is a boot-time property: device_is_ready() reflects whether the
 * driver's init hook succeeded, and that result cannot change afterwards. It
 * is checked once in app_sensor_environmental_setup() and cached here rather
 * than re-tested on every read.
 */
static bool bme280_ready;

int app_sensor_environmental_read(struct sensor_value *temp,
				  struct sensor_value *hum,
				  struct sensor_value *press)
{
	int rc;

	if (!bme280_ready) {
		return -ENODEV;
	}

	rc = sensor_sample_fetch(bme280);
	if (rc != 0) {
		return rc;
	}

	rc = sensor_channel_get(bme280, SENSOR_CHAN_AMBIENT_TEMP, temp);
	if (rc != 0) {
		return rc;
	}

	rc = sensor_channel_get(bme280, SENSOR_CHAN_HUMIDITY, hum);
	if (rc != 0) {
		return rc;
	}

	return sensor_channel_get(bme280, SENSOR_CHAN_PRESS, press);
}

int app_sensor_environmental_setup(void)
{
	struct sensor_value temp, hum, press;
	int rc;

	if (!device_is_ready(bme280)) {
		printk("BME280 device not ready\n");
		return -ENODEV;
	}

	bme280_ready = true;

	rc = app_sensor_environmental_read(&temp, &hum, &press);
	if (rc != 0) {
		printk("BME280: read failed: %d\n", rc);
		return rc;
	}

	printk("BME280: %.2f C ; %.2f %%RH ; %.2f kPa\n",
	       sensor_value_to_double(&temp),
	       sensor_value_to_double(&hum),
	       sensor_value_to_double(&press));

	return 0;
}
