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
/* EP1 OUT buffer control: 0x80 + 1*8 + 4 (OUT is the second word). */
#define USB_DPRAM_EP1_OUT_BUF_CTRL (USB_DPRAM_BASE + 0x8CU)
/* Endpoint control for EP1 OUT: 0x08 + (1-1)*8 + 4. */
#define USB_DPRAM_EP1_OUT_EP_CTRL  (USB_DPRAM_BASE + 0x0CU)




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
#include <zephyr/drivers/usb/udc_rpi_pico_trace.h>
extern int usbd_cdc_acm_diag(const struct device *dev, uint32_t *tx_pending,
			     uint32_t *rx_pending);
extern uint32_t usbd_cdc_acm_susp_calls;
extern uint32_t usbd_cdc_acm_res_calls;
extern uint32_t usbd_cdc_acm_rxbusy_sets;
extern uint32_t usbd_cdc_acm_rx_enq;
extern uint32_t usbd_cdc_acm_rx_done;
extern uint32_t usbd_cdc_acm_rx_err;
extern uint32_t usbd_cdc_acm_last_req_err;
extern uint32_t usbd_cdc_acm_tx_enq;
extern uint32_t usbd_cdc_acm_tx_done;
extern uint32_t usbd_cdc_acm_tx_claim_miss;
extern uint32_t usbd_core_evt_susp;
extern uint32_t usbd_core_evt_res;
extern uint32_t usbd_core_bcast_blocked;
extern uint32_t usbd_core_ch9_state;
extern uint32_t usbd_core_bcast_ok;
extern uint32_t usbd_core_blocked_state;
extern uint32_t usbd_core_blocked_type;
extern uint32_t usbd_ch9_setup_count;
extern uint32_t usbd_ch9_last_req;
extern uint32_t usbd_ch9_err_count;
extern uint32_t usbd_ch9_last_err;
extern uint16_t usbd_ch9_reqlog[16];
extern uint8_t  usbd_ch9_reqlog_idx;
extern uint8_t  usbd_ch9_state_log[16];

#define USB_BASE_ADDR      DT_REG_ADDR(DT_NODELABEL(usbd))
#define USB_REG_SIE_STATUS (USB_BASE_ADDR + 0x50U)

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

static void wedge_monitor(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		k_msleep(5000);

		printk("\n--- wedge monitor @ %lld ms ---\n", k_uptime_get());
		printk("  susp=%u res=%u synth=%u synthS=%u false=%u both=%u sw_susp=%u hw_sie=%08x "
		       "sie@susp=%08x sie@res=%08x\n",
		       udc_rpi_pico_susp_trace.suspends,
		       udc_rpi_pico_susp_trace.resumes,
		       udc_rpi_pico_susp_trace.synth_resumes,
		       udc_rpi_pico_susp_trace.synth_suspends,
		       udc_rpi_pico_susp_trace.false_suspends,
		       udc_rpi_pico_susp_trace.both_same_isr,
		       udc_rpi_pico_susp_trace.sw_suspended,
		       sys_read32(USB_REG_SIE_STATUS),
		       udc_rpi_pico_susp_trace.sie_at_suspend,
		       udc_rpi_pico_susp_trace.sie_at_resume);
		{
			const struct device *cdc =
				DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));
			uint32_t txp = 0, rxp = 0;
			int st = usbd_cdc_acm_diag(cdc, &txp, &rxp);

			printk("  cdc state=%02x tx_pending=%u rx_pending=%u "
			       "cls_susp=%u cls_res=%u rxarm=%u\n",
			       st, txp, rxp, usbd_cdc_acm_susp_calls,
			       usbd_cdc_acm_res_calls, usbd_cdc_acm_rxbusy_sets);
			printk("  rx enq=%u done=%u err=%u lasterr=%u\n",
			       usbd_cdc_acm_rx_enq, usbd_cdc_acm_rx_done,
			       usbd_cdc_acm_rx_err, usbd_cdc_acm_last_req_err);
			printk("  core susp_evt=%u res_evt=%u blocked=%u ok=%u "
			       "ch9=%u blk_state=%u blk_type=%u\n",
			       usbd_core_evt_susp, usbd_core_evt_res,
			       usbd_core_bcast_blocked, usbd_core_bcast_ok,
			       usbd_core_ch9_state, usbd_core_blocked_state,
			       usbd_core_blocked_type);
			printk("  bus resets=%u setups=%u sie@reset=%08x "
			       "addr@reset=%08x\n",
			       udc_rpi_pico_susp_trace.bus_resets,
			       udc_rpi_pico_susp_trace.setups,
			       udc_rpi_pico_susp_trace.sie_at_reset,
			       udc_rpi_pico_susp_trace.addr_at_reset);
			printk("  tx enq=%u done=%u claimmiss=%u\n",
			       usbd_cdc_acm_tx_enq, usbd_cdc_acm_tx_done,
			       usbd_cdc_acm_tx_claim_miss);
			printk("  IN armed=%u bs=%u finpost=%u din=%u xnfail=%u contfail=%u/%u nobuf=%u cont=%u\n",
			       udc_rpi_pico_susp_trace.in_armed,
			       udc_rpi_pico_susp_trace.in_bs,
			       udc_rpi_pico_susp_trace.in_fin_post,
			       udc_rpi_pico_susp_trace.in_evt_din,
			       udc_rpi_pico_susp_trace.xn_fail,
			       udc_rpi_pico_susp_trace.in_cont_fail,
			       udc_rpi_pico_susp_trace.out_cont_fail,
			       udc_rpi_pico_susp_trace.in_nobuf,
			       udc_rpi_pico_susp_trace.in_cont);
			printk("  udc newbusy=%u ep=%02x finbusy=%u qafterfin=%u\n",
			       udc_rpi_pico_susp_trace.new_but_busy,
			       udc_rpi_pico_susp_trace.last_busy_ep,
			       udc_rpi_pico_susp_trace.fin_but_busy,
			       udc_rpi_pico_susp_trace.queued_after_fin);
			printk("  ch9 setups=%u last_req=%08x errs=%u last_err=%u\n",
			       usbd_ch9_setup_count, usbd_ch9_last_req,
			       usbd_ch9_err_count, usbd_ch9_last_err);
			printk("  ch9 log(req@state):");
			for (int k = 0; k < 16; k++) {
				int idx = (usbd_ch9_reqlog_idx + k) & 0xf;

				if (usbd_ch9_reqlog[idx] == 0) {
					continue;
				}
				printk(" %04x@%u", usbd_ch9_reqlog[idx],
				       usbd_ch9_state_log[idx]);
			}
			printk("\n");
		}

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

		/*
		 * SIE error counters. CRC and bit-stuff are generated by the USB
		 * hardware and cannot be produced by firmware timing, so if these
		 * climb before a wedge the cause is electrical, not code.
		 */

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
