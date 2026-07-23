/* SPDX-License-Identifier: Apache-2.0 */

//
// Copyright (c) 2021-2026 Blue Clover Devices
//

#include <stdint.h>

int app_ledstrip_setup(void);

/*
 * @brief Advance the built-in color cycle by one step (red/green/blue).
 * @return 0 on success, non-zero on failure.
 */
int app_ledstrip_run(void);

/*
 * @brief Set every pixel to a fixed RGB color.
 * @return 0 on success, non-zero on failure.
 */
int app_ledstrip_set_rgb(uint8_t r, uint8_t g, uint8_t b);

/*
 * @brief Turn the LED strip off (all pixels black).
 * @return 0 on success, non-zero on failure.
 */
int app_ledstrip_off(void);
