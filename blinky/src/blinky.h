// SPDX-License-Identifier: Apache-2.0
//
// Copyright (c) 2026 Blue Clover Devices
//
// Shared declarations for the blinky command transports.

#ifndef BLINKY_H_
#define BLINKY_H_

#include <zephyr/kernel.h>
#include <zephyr/version.h>
#include <zephyr/app_version.h>

// APP_BUILD_VERSION is generated as a bare token (the `git describe` output,
// unquoted), so it has to be stringified before it can be printed.
#define BLINKY_STR(s)  #s
#define BLINKY_XSTR(s) BLINKY_STR(s)

// `version` command output. Shared by the USB shell (blinky_shell.c) and the
// UART console command (blinky_uart_cmd.c) so test plans can parse either.
// Each line is printed with a trailing newline.
#define BLINKY_VERSION_LINES(PRINT)                                            \
	do {                                                                   \
		PRINT("app:    blinky %s", APP_VERSION_STRING);                \
		PRINT("build:  %s", BLINKY_XSTR(APP_BUILD_VERSION));           \
		PRINT("zephyr: %s", KERNEL_VERSION_STRING);                    \
		PRINT("board:  %s", CONFIG_BOARD_TARGET);                      \
	} while (0)

#define BLINKY_PROMPT "blinky:~$ "

#ifdef CONFIG_SHELL

// The Zephyr shell runs in its own thread; main only has to sleep.
static inline int blinky_cmd_init(void)
{
	return 0;
}

static inline void blinky_cmd_wait(k_timeout_t timeout)
{
	k_sleep(timeout);
}

#else

// Start receiving commands on the console UART.
int blinky_cmd_init(void);

// Sleep for `timeout`, handling any commands that arrive in the meantime.
void blinky_cmd_wait(k_timeout_t timeout);

#endif

#endif // BLINKY_H_
