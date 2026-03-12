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

int app_sensor_environmental_setup(void)
{
	struct sensor_value temp, hum, press;
	const struct device *dev = DEVICE_DT_GET(BME280_NODE);
	int rc;

	if (!device_is_ready(dev)) {
		printk("BME280 device not ready\n");
		return -ENODEV;
	}

	rc = sensor_sample_fetch(dev);
	if (rc != 0) {
		printk("BME280: fetch failed: %d\n", rc);
		return rc;
	}

	rc = sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &temp);
	if (rc != 0) {
		printk("BME280: get temp failed: %d\n", rc);
		return rc;
	}

	rc = sensor_channel_get(dev, SENSOR_CHAN_HUMIDITY, &hum);
	if (rc != 0) {
		printk("BME280: get humidity failed: %d\n", rc);
		return rc;
	}

	rc = sensor_channel_get(dev, SENSOR_CHAN_PRESS, &press);
	if (rc != 0) {
		printk("BME280: get pressure failed: %d\n", rc);
		return rc;
	}

	printk("BME280: %.2f C ; %.2f %%RH ; %.2f kPa\n",
	       sensor_value_to_double(&temp),
	       sensor_value_to_double(&hum),
	       sensor_value_to_double(&press));

	return 0;
}
