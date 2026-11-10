// SPDX-License-Identifier: Apache-2.0

//
// Copyright (c) 2026 Blue Clover Devices
//

/*
 * blinky_shell.c - Version reporting over the shell.
 *
 * The shell lives on a USB CDC-ACM port (zephyr,shell-uart = &cdc_acm_uart0 in
 * ../usb_shell.overlay). Zephyr's built-in `kernel version` command reports the Zephyr
 * kernel version, not the application's, so the app version from ./VERSION is
 * exposed here as a separate `version` command. Its output is shared with the
 * UART console transport (blinky_uart_cmd.c) via BLINKY_VERSION_LINES.
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include "blinky.h"

static int cmd_version(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

#define PRINT_LINE(...) shell_print(sh, __VA_ARGS__)
	BLINKY_VERSION_LINES(PRINT_LINE);
#undef PRINT_LINE

	return 0;
}

SHELL_CMD_REGISTER(version, NULL, "Show application, Zephyr, and board versions",
		   cmd_version);
