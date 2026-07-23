/* SPDX-License-Identifier: Apache-2.0 */

//
// Copyright (c) 2021-2026 Blue Clover Devices
//

#include <zephyr/drivers/sensor.h>

/*
 * @brief Verify the BME280 is ready and print an initial reading.
 * @return 0 on success, non-zero on failure.
 */
int app_sensor_environmental_setup(void);

/*
 * @brief Fetch a fresh BME280 sample.
 *
 * @param temp   Out: ambient temperature (deg C).
 * @param hum    Out: relative humidity (%).
 * @param press  Out: pressure (kPa).
 * @return 0 on success, non-zero on failure.
 */
int app_sensor_environmental_read(struct sensor_value *temp,
				  struct sensor_value *hum,
				  struct sensor_value *press);
