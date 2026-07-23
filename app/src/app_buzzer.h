/* SPDX-License-Identifier: Apache-2.0 */

//
// Copyright (c) 2021-2026 Blue Clover Devices
//

#include <stdint.h>

/*
 * @brief Verify the buzzer PWM device is ready.
 * @return 0 on success, non-zero on failure.
 */
int app_buzzer_setup(void);

/*
 * @brief Play the startup note sweep on the buzzer.
 * @return 0 on success, non-zero on failure.
 */
int app_buzzer_startup_sweep(void);

/*
 * @brief Sound the buzzer at a given frequency for a given duration.
 *
 * @param freq_hz   Tone frequency in Hz. Must be > 0.
 * @param duration_ms  How long to sound the tone, in milliseconds.
 * @return 0 on success, non-zero on failure.
 */
int app_buzzer_beep(uint32_t freq_hz, uint32_t duration_ms);
