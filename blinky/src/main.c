// SPDX-License-Identifier: Apache-2.0
//
// Copyright (c) 2026 Blue Clover Devices
//
// RGB LED demo for the Bento demo boards.
//
// Drives the LTST-E683CEGBW WS2812-compatible RGB LED through Zephyr's
// WS2812 PIO led_strip driver (worldsemi,ws2812-rpi_pico-pio) via the
// "led-strip" devicetree alias. The data pin differs per board:
//   - Bento1: GPIO36 (gpio0_hi pin 4, high GPIO bank)
//   - Bento2: GPIO14 (gpio0_lo pin 14, low GPIO bank)
// The high-bank pin needs the RP2350 PIO GPIOBASE relocated to 16; that is
// handled inside the driver (see patches/zephyr/0001-...), so this example is
// board-agnostic and just cycles the LED through a few colors.

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/shell/shell_uart.h>
#include <zephyr/usb/usbd.h>

/*
 * USB controller register offsets, relative to the usbd node's base address in
 * the devicetree (0x50110000 on RP2350). Taken from the RP2350 datasheet
 * section 12.7.6 rather than hardcoded, so a board with a different mapping
 * still resolves correctly.
 */
#define USB_BASE_ADDR       DT_REG_ADDR(DT_NODELABEL(usbd))
#define USB_REG_SIE_CTRL    (USB_BASE_ADDR + 0x4CU)
#define USB_REG_SIE_STATUS  (USB_BASE_ADDR + 0x50U)
#define USB_REG_BUFF_STATUS (USB_BASE_ADDR + 0x58U)
#define USB_REG_INTE        (USB_BASE_ADDR + 0x90U)
#define USB_REG_INTS        (USB_BASE_ADDR + 0x98U)
/*
 * SIE_STATUS.ENDPOINT_ERROR (bit 23) says only that *an* endpoint failed; the
 * datasheet directs you to these two registers to find which one and why. The
 * stock driver never reads them and never clears the latch.
 */
#define USB_REG_EP_ABORT      (USB_BASE_ADDR + 0x60U)
#define USB_REG_EP_ABORT_DONE (USB_BASE_ADDR + 0x64U)
#define USB_REG_EP_STALL_NAK  (USB_BASE_ADDR + 0x70U)
#define USB_REG_EP_TX_ERROR   (USB_BASE_ADDR + 0x10CU)
#define USB_REG_EP_RX_ERROR   (USB_BASE_ADDR + 0x110U)

/*
 * Buffer control for the CDC bulk IN endpoint (EP2 IN), in USB DPRAM -- a
 * different aperture from the register block above. Layout: 8-byte setup
 * packet, then 15 ep_ctrl pairs (0x78), then ep_buf_ctrl pairs from 0x80, two
 * words per endpoint (IN first). EP2 IN is therefore 0x80 + 2*8 = 0x90.
 *
 * AVAILABLE (bit 10) set here means the controller still owns the buffer and
 * is waiting to send it; clear means it has been handed back.
 */
#define USB_DPRAM_BASE             0x50100000U
#define USB_DPRAM_EP2_IN_BUF_CTRL  (USB_DPRAM_BASE + 0x90U)

/*
 * SIE error/lifecycle counters maintained by the patched udc_rpi_pico driver
 * (patches/zephyr/0002-...). Declared here rather than in a header because the
 * counters are a local diagnostic addition, not upstream API.
 */
struct udc_rpi_pico_err_stats {
	uint32_t crc;
	uint32_t bit_stuff;
	uint32_t data_seq;
	uint32_t rx_timeout;
	uint32_t rx_overflow;
	uint32_t enable_calls;
	uint32_t bus_resets;
	uint32_t suspends;
	uint32_t vbus_removed;
	uint32_t ep_tx_errors;
	uint32_t ep_rx_errors;
	uint32_t last_ep_tx_error;
	uint32_t last_ep_rx_error;
	uint32_t reenumerations;
	uint32_t ep_halts;
	uint32_t ep_halt_clears;
	uint8_t last_halted_ep;
	uint8_t last_cleared_ep;
};

extern struct udc_rpi_pico_err_stats udc_rpi_pico_err_stats;

/*
 * CDC-ACM TX-path diagnostic (patches/zephyr/0003-...). Reports bytes still
 * queued in the class driver's TX FIFO and whether it believes a transfer is
 * in flight -- the pair that distinguishes a stalled controller from a class
 * driver that has stopped submitting work.
 */
extern int usbd_cdc_acm_tx_diag(const struct device *dev, uint32_t *pending,
				bool *busy);

/*
 * The USB device context created by CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT
 * (subsys/usb/device_next/app/cdc_acm_serial.c) is defined with
 * STRUCT_SECTION_ITERABLE, so it is reachable through the usbd_context linker
 * section rather than by symbol name. Needed to restart the stack from the
 * link watchdog below.
 */
static struct usbd_context *usb_ctx_get(void)
{
	STRUCT_SECTION_FOREACH(usbd_context, ctx) {
		return ctx;
	}

	return NULL;
}

/*
 * Consecutive 5 s monitor ticks with no USB error activity, after errors have
 * occurred, before the link is presumed dead. Three ticks (~15 s) is well past
 * any normal gap in shell traffic.
 */
#define USB_WATCHDOG_QUIET_TICKS 3

// Generated from ./VERSION by the build; see the Versioning section of the
// top-level README. The version is logged at startup here and is also
// available interactively via the `version` shell command (blinky_shell.c).
#include <zephyr/app_version.h>

// APP_BUILD_VERSION is generated as a bare token (the `git describe` output,
// unquoted), so it has to be stringified before it can be printed.
#define BLINKY_STR(s)  #s
#define BLINKY_XSTR(s) BLINKY_STR(s)

LOG_MODULE_REGISTER(blinky, LOG_LEVEL_INF);

#define STRIP_NODE DT_ALIAS(led_strip)
#define NUM_LEDS   DT_PROP(STRIP_NODE, chain_length)

static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);

static struct led_rgb pixels[NUM_LEDS];

struct color {
	uint8_t r, g, b;
	const char *name;
};

// Kept dim on purpose: the LTST-E683CEGBW is bright even at low values.
static const struct color colors[] = {
	{ 0x10, 0x00, 0x00, "RED" },
	{ 0x00, 0x10, 0x00, "GREEN" },
	{ 0x00, 0x00, 0x10, "BLUE" },
	{ 0x00, 0x00, 0x00, "OFF" },
};

/*
 * Wedge monitor.
 *
 * The USB shell freezes mid-output with no error and no watchdog trip, so the
 * question is simply: which thread is stuck, and on what? The shell itself is
 * on USB, so it cannot answer that once it wedges -- this thread reports over
 * the UART console instead.
 *
 * printk() is used deliberately rather than LOG_*: it writes straight to the
 * console driver, bypassing both the log subsystem's processing thread and the
 * shell, either of which may itself be the thing that is stuck.
 *
 * Every thread is dumped with its state and, when blocked, the wait object it
 * is parked on. Cross-referencing that address against the kernel objects in
 * the map file identifies the exact semaphore/event/mutex holding things up,
 * which discriminates between every theory on the table at once.
 */
static void wedge_report_thread(const struct k_thread *thread, void *user_data)
{
	ARG_UNUSED(user_data);

	const char *name = k_thread_name_get((k_tid_t)thread);
	char state[16];

	printk("  %-20s state=%-12s prio=%d wait_on=%p\n",
	       (name && name[0]) ? name : "(unnamed)",
	       k_thread_state_str((k_tid_t)thread, state, sizeof(state)),
	       thread->base.prio,
	       (void *)thread->base.pended_on);
}

/*
 * USB link watchdog.
 *
 * The driver-level escalation (patch 0002) forces a re-enumeration while the
 * host is still polling a failing endpoint. It cannot help once the host gives
 * up: the bus goes silent, no more endpoint errors are raised, no interrupt
 * ever fires again, and the device sits enumerated-but-dead forever. That is
 * the state every wedge dump shows -- error counters frozen, BUFF_STATUS empty,
 * every thread idle.
 *
 * Detect it from outside the interrupt path: if the CDC-ACM shell has produced
 * no successful transfer for long enough, tear the whole USB stack down and
 * bring it back up. usbd_disable()/usbd_enable() rebuilds controller and class
 * state on this side and forces the host to re-enumerate, which is the only
 * thing that revives an abandoned link.
 */
static void usb_link_watchdog(void)
{
	static uint32_t last_tx_evts;
	static uint32_t last_crc;
	static unsigned int quiet_ticks;
	struct usbd_context *ctx;
	int err;

	uint32_t tx_evts = udc_rpi_pico_err_stats.ep_tx_errors;
	uint32_t crc = udc_rpi_pico_err_stats.crc;

	/*
	 * "Quiet" means no endpoint errors AND no CRC errors since the last
	 * check. A healthy idle link is also quiet, so this alone is not a
	 * fault -- it only matters once errors have been seen, which is what
	 * distinguishes an abandoned link from an unused one.
	 */
	if (tx_evts == last_tx_evts && crc == last_crc && tx_evts > 0) {
		quiet_ticks++;
	} else {
		quiet_ticks = 0;
	}

	last_tx_evts = tx_evts;
	last_crc = crc;

	if (quiet_ticks < USB_WATCHDOG_QUIET_TICKS) {
		return;
	}

	quiet_ticks = 0;
	printk("  usb_watchdog: link dead after %u endpoint errors; restarting stack\n",
	       tx_evts);

	ctx = usb_ctx_get();
	if (ctx == NULL) {
		printk("  usb_watchdog: no usbd context\n");
		return;
	}

	err = usbd_disable(ctx);
	if (err) {
		printk("  usb_watchdog: usbd_disable failed (%d)\n", err);
		return;
	}

	k_msleep(100);

	err = usbd_enable(ctx);
	if (err) {
		printk("  usb_watchdog: usbd_enable failed (%d)\n", err);
	}
}

static void wedge_monitor(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		k_msleep(5000);

		printk("\n--- wedge monitor @ %lld ms ---\n", k_uptime_get());

		/*
		 * The thread dump alone cannot distinguish "everything finished
		 * normally" from "bytes are stranded in a buffer with nothing
		 * scheduled to send them" -- both leave every thread idle. These
		 * two counts settle it directly.
		 *
		 * shell_tx: bytes the shell has handed to its backend ring buffer
		 * but that have not been pulled out yet. Non-zero while output is
		 * frozen means the shell wrote data that nothing is draining.
		 */
		{
			const struct shell *sh = shell_backend_uart_get_ptr();
			struct shell_uart_int_driven *su =
				(struct shell_uart_int_driven *)sh->iface->ctx;

			printk("  shell_tx used=%u free=%u tx_busy=%ld state=%d\n",
			       ring_buf_size_get(&su->tx_ringbuf),
			       ring_buf_space_get(&su->tx_ringbuf),
			       atomic_get(&su->tx_busy),
			       (int)sh->ctx->state);
		}

		/*
		 * Live USB controller registers. INTE going to zero while
		 * BUFF_STATUS still holds completions is the signature of the
		 * wedge: the SIE finished transfers and latched them, but no
		 * interrupt can fire to collect them.
		 */
		printk("  usb INTE=%08x INTS=%08x BUFF_STATUS=%08x SIE_CTRL=%08x SIE_STATUS=%08x\n",
		       sys_read32(USB_REG_INTE), sys_read32(USB_REG_INTS),
		       sys_read32(USB_REG_BUFF_STATUS),
		       sys_read32(USB_REG_SIE_CTRL), sys_read32(USB_REG_SIE_STATUS));

		/*
		 * SIE error counters. CRC and bit-stuff are generated by the USB
		 * hardware and cannot be produced by firmware timing, so if these
		 * climb before a wedge the cause is electrical, not code.
		 */
		printk("  usb_ep tx_err=%08x rx_err=%08x stall_nak=%08x abort=%08x abort_done=%08x\n",
		       sys_read32(USB_REG_EP_TX_ERROR), sys_read32(USB_REG_EP_RX_ERROR),
		       sys_read32(USB_REG_EP_STALL_NAK), sys_read32(USB_REG_EP_ABORT),
		       sys_read32(USB_REG_EP_ABORT_DONE));

		printk("  sie_err crc=%u bitstuff=%u dataseq=%u rxto=%u rxovf=%u\n",
		       udc_rpi_pico_err_stats.crc,
		       udc_rpi_pico_err_stats.bit_stuff,
		       udc_rpi_pico_err_stats.data_seq,
		       udc_rpi_pico_err_stats.rx_timeout,
		       udc_rpi_pico_err_stats.rx_overflow);

		printk("  ep_err tx_evts=%u rx_evts=%u last_tx=%08x last_rx=%08x reenum=%u\n",
		       udc_rpi_pico_err_stats.ep_tx_errors,
		       udc_rpi_pico_err_stats.ep_rx_errors,
		       udc_rpi_pico_err_stats.last_ep_tx_error,
		       udc_rpi_pico_err_stats.last_ep_rx_error,
		       udc_rpi_pico_err_stats.reenumerations);

		{
			const struct device *cdc =
				DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));
			uint32_t pending = 0;
			bool busy = false;

			if (usbd_cdc_acm_tx_diag(cdc, &pending, &busy) == 0) {
				printk("  cdc_tx pending=%u class_busy=%d "
				       "ep82_bufctrl=%08x\n",
				       pending, (int)busy,
				       sys_read32(USB_DPRAM_EP2_IN_BUF_CTRL));
			}
		}

		printk("  ep_halt halts=%u (ep 0x%02x) host_clears=%u (ep 0x%02x)\n",
		       udc_rpi_pico_err_stats.ep_halts,
		       udc_rpi_pico_err_stats.last_halted_ep,
		       udc_rpi_pico_err_stats.ep_halt_clears,
		       udc_rpi_pico_err_stats.last_cleared_ep);

		printk("  usb_life enable=%u resets=%u suspend=%u vbus_rm=%u\n",
		       udc_rpi_pico_err_stats.enable_calls,
		       udc_rpi_pico_err_stats.bus_resets,
		       udc_rpi_pico_err_stats.suspends,
		       udc_rpi_pico_err_stats.vbus_removed);

		/*
		 * DISABLED. Restarting the USB stack on a quiet link caused the
		 * device to re-enumerate continuously: the host never completed
		 * a session before the next restart, so the port kept dropping
		 * and reappearing and could not be opened at all. A wedge that
		 * can be power-cycled is better than a port that never settles.
		 *
		 * The detection heuristic is the flaw -- "errors seen, then no
		 * change for 15 s" also matches a link that has simply gone
		 * idle, so the watchdog fires on healthy quiet periods too.
		 * Re-enable only with a positive liveness check (an actual
		 * failed transfer outstanding), not an absence of activity.
		 */
		if (IS_ENABLED(CONFIG_BENTO_USB_LINK_WATCHDOG)) {
			usb_link_watchdog();
		}

		/*
		 * k_thread_foreach_unlocked() walks with interrupts enabled
		 * between threads, so it will not itself stall the system while
		 * printing over a slow UART.
		 */
		k_thread_foreach_unlocked(wedge_report_thread, NULL);
	}
}

K_THREAD_DEFINE(wedge_monitor_tid, 1024, wedge_monitor, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

int main(void)
{
	int rc;

	LOG_INF("Bento blinky %s (build %s)", APP_VERSION_STRING,
		BLINKY_XSTR(APP_BUILD_VERSION));
	LOG_INF("WS2812 led_strip blinky | %d LED(s)", NUM_LEDS);

	if (!device_is_ready(strip)) {
		LOG_ERR("LED strip device %s not ready", strip->name);
		return -ENODEV;
	}

	size_t idx = 0;

	while (1) {
		const struct color *c = &colors[idx];

		// Drive every LED in the chain to the same color.
		for (size_t i = 0; i < NUM_LEDS; i++) {
			pixels[i] = (struct led_rgb){ .r = c->r, .g = c->g, .b = c->b };
		}

		rc = led_strip_update_rgb(strip, pixels, NUM_LEDS);
		if (rc) {
			LOG_ERR("led_strip_update_rgb failed: %d", rc);
		}

		idx = (idx + 1) % ARRAY_SIZE(colors);
		k_msleep(1000);
	}

	return 0;
}
