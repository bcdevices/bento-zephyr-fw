// SPDX-License-Identifier: Apache-2.0

//
// Copyright (c) 2026 Blue Clover Devices
//

/*
 * blinky_uart_cmd.c - Minimal line-based commands on the console UART.
 *
 * For boards without a USB device controller, or without the RAM for the
 * Zephyr shell (e.g. STM32L011K4, 2 KB). Supports one command, `version`,
 * whose output matches the shell's `version` command (BLINKY_VERSION_LINES).
 *
 * The RX ISR echoes characters and assembles a line; on CR or LF it hands the
 * line to the main thread, which runs the command from blinky_cmd_wait()
 * between LED updates. Characters received while a command is running are
 * dropped. Output uses printk(), which goes straight to the console driver
 * (CONFIG_LOG_PRINTK=n).
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "blinky.h"

#define LINE_MAX 32

static const struct device *const uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

static char rx_line[LINE_MAX];
static size_t rx_len;
static char cmd_line[LINE_MAX];
static atomic_t cmd_busy;
static K_SEM_DEFINE(cmd_sem, 0, 1);

static void rx_char(char c)
{
	if (c == '\r' || c == '\n') {
		// Ignore the LF of a CRLF pair, and anything while a command runs.
		if ((c == '\n' && rx_len == 0) || atomic_get(&cmd_busy)) {
			rx_len = 0;
			return;
		}
		uart_poll_out(uart, '\r');
		uart_poll_out(uart, '\n');
		memcpy(cmd_line, rx_line, rx_len);
		cmd_line[rx_len] = '\0';
		rx_len = 0;
		atomic_set(&cmd_busy, 1);
		k_sem_give(&cmd_sem);
		return;
	}

	if ((c == '\b' || c == 0x7f) && rx_len > 0) {
		rx_len--;
		uart_poll_out(uart, '\b');
		uart_poll_out(uart, ' ');
		uart_poll_out(uart, '\b');
		return;
	}

	if (c >= ' ' && c <= '~' && rx_len < LINE_MAX - 1) {
		rx_line[rx_len++] = c;
		uart_poll_out(uart, c);
	}
}

static void uart_isr(const struct device *dev, void *user_data)
{
	uint8_t c;

	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
		while (uart_fifo_read(dev, &c, 1) == 1) {
			rx_char((char)c);
		}
	}
}

#define PRINT_LINE(fmt, ...) printk(fmt "\n", ##__VA_ARGS__)

static void run_command(const char *line)
{
	if (strcmp(line, "version") == 0) {
		BLINKY_VERSION_LINES(PRINT_LINE);
	} else if (line[0] != '\0') {
		printk("%s: command not found (try: version)\n", line);
	}
	printk(BLINKY_PROMPT);
}

int blinky_cmd_init(void)
{
	int rc;

	if (!device_is_ready(uart)) {
		return -ENODEV;
	}

	rc = uart_irq_callback_set(uart, uart_isr);
	if (rc) {
		return rc;
	}

	uart_irq_rx_enable(uart);
	printk(BLINKY_PROMPT);

	return 0;
}

void blinky_cmd_wait(k_timeout_t timeout)
{
	k_timepoint_t end = sys_timepoint_calc(timeout);

	while (k_sem_take(&cmd_sem, sys_timepoint_timeout(end)) == 0) {
		run_command(cmd_line);
		atomic_set(&cmd_busy, 0);
	}
}
