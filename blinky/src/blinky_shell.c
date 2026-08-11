// SPDX-License-Identifier: Apache-2.0

//
// Copyright (c) 2026 Blue Clover Devices
//

/*
 * blinky_shell.c - Version reporting over the shell.
 *
 * The shell lives on a USB CDC-ACM port (zephyr,shell-uart = &cdc_acm_uart0 in
 * ./app.overlay). Zephyr's built-in `kernel version` command reports the Zephyr
 * kernel version, not the application's, so the app version from ./VERSION is
 * exposed here as a separate `version` command.
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
// KERNEL_VERSION_STRING
#include <zephyr/version.h>

// Generated from ./VERSION by the build; see the Versioning section of the
// top-level README.
#include <zephyr/app_version.h>

// APP_BUILD_VERSION is generated as a bare token (the `git describe` output,
// unquoted), so it has to be stringified before it can be printed.
#define BLINKY_STR(s)  #s
#define BLINKY_XSTR(s) BLINKY_STR(s)

static int cmd_version(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "app:    blinky %s", APP_VERSION_STRING);
	shell_print(sh, "build:  %s", BLINKY_XSTR(APP_BUILD_VERSION));
	shell_print(sh, "zephyr: %s", KERNEL_VERSION_STRING);
	shell_print(sh, "board:  %s", CONFIG_BOARD_TARGET);

	return 0;
}

SHELL_CMD_REGISTER(version, NULL, "Show application, Zephyr, and board versions",
		   cmd_version);
