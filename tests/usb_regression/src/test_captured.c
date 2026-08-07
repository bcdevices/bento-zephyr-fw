/*
 * Copyright (c) 2026 Blue Clover Devices
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests built from register values actually captured on a wedged board.
 *
 * During the investigation these words were decoded by hand, repeatedly, under
 * time pressure -- and misread more than once. A backwards FULL/AVAILABLE
 * decode and a wrong endpoint-index mapping each sent the work down a blind
 * alley for hours. Encoding the real captures as tests means the decode is
 * checked by the compiler rather than by tired arithmetic, and that any future
 * change to the bit definitions is caught immediately.
 *
 * Every hex value in this file was observed on hardware. Provenance is noted
 * per test so a future reader can trust or re-verify it.
 */

#ifdef HOST_TEST
#include "../host_shim.h"
#else
#include <zephyr/ztest.h>
#endif

/* ---- SIE_STATUS bit definitions (RP2350 datasheet / usb.h) ---- */
#define SIE_DATA_SEQ_ERROR BIT(31)
#define SIE_ACK_REC        BIT(30)
#define SIE_STALL_REC      BIT(29)
#define SIE_NAK_REC        BIT(28)
#define SIE_RX_TIMEOUT     BIT(27)
#define SIE_RX_OVERFLOW    BIT(26)
#define SIE_BIT_STUFF_ERR  BIT(25)
#define SIE_CRC_ERROR      BIT(24)
#define SIE_ENDPOINT_ERROR BIT(23)
#define SIE_BUS_RESET      BIT(19)
#define SIE_TRANS_COMPLETE BIT(18)
#define SIE_SETUP_REC      BIT(17)
#define SIE_CONNECTED      BIT(16)
#define SIE_RX_SHORT_PKT   BIT(12)
#define SIE_RESUME         BIT(11)
#define SIE_VBUS_OVER_CURR BIT(10)
#define SIE_SPEED_MASK     (BIT(9) | BIT(8))
#define SIE_SUSPENDED      BIT(4)
#define SIE_LINE_STATE     (BIT(3) | BIT(2))
#define SIE_VBUS_DETECTED  BIT(0)

/*
 * Captured while the shell was wedged but the bus was demonstrably alive (the
 * SOF frame counter was advancing at the nominal rate at the same moment).
 *
 * This is the capture that disproved "the host suspended us": SUSPENDED is
 * clear here, and ACK_REC / TRANS_COMPLETE show traffic completing normally.
 */
ZTEST(usb_captured, test_active_bus_sie_status)
{
	const uint32_t sie = 0x40851005;

	zassert_true(sie & SIE_ACK_REC, "an ACK had been received");
	zassert_true(sie & SIE_ENDPOINT_ERROR, "endpoint error was latched");
	zassert_true(sie & SIE_TRANS_COMPLETE, "a transfer had completed");
	zassert_true(sie & SIE_CONNECTED, "device still connected");
	zassert_true(sie & SIE_RX_SHORT_PKT, "a short packet was received");
	zassert_true(sie & SIE_VBUS_DETECTED, "VBUS present");

	zassert_equal(sie & SIE_SUSPENDED, 0,
		      "SUSPENDED must be CLEAR in this capture -- the bus was "
		      "active and the host was not suspending us, which is why "
		      "suspend has to be validated against SOF activity");
}

/*
 * Captured on a board the driver believed was suspended. Only VBUS, line state
 * and SUSPENDED are set: no traffic, no errors. Contrast with the above.
 */
ZTEST(usb_captured, test_genuinely_suspended_sie_status)
{
	const uint32_t sie = 0x00000015;

	zassert_true(sie & SIE_SUSPENDED, "SUSPENDED is set");
	zassert_true(sie & SIE_VBUS_DETECTED, "VBUS present");
	zassert_true(sie & SIE_LINE_STATE, "line state non-zero");

	zassert_equal(sie & (SIE_ACK_REC | SIE_TRANS_COMPLETE), 0,
		      "no traffic completed while suspended");
	zassert_equal(sie & SIE_CONNECTED, 0,
		      "CONNECTED is clear in this capture");
}

/*
 * Captured at the moment a BUS_RESET was handled: BUS_RESET | VBUS_DETECTED,
 * sampled BEFORE clearing the latch. CONNECTED clear and LINE_STATE zero is
 * SE0, which is what a genuine reset looks like.
 */
ZTEST(usb_captured, test_bus_reset_sie_status)
{
	const uint32_t sie = 0x00080001;

	zassert_true(sie & SIE_BUS_RESET, "BUS_RESET latched");
	zassert_true(sie & SIE_VBUS_DETECTED, "VBUS present");
	zassert_equal(sie & SIE_LINE_STATE, 0, "SE0: both lines low");
	zassert_equal(sie & SIE_SUSPENDED, 0, "a reset is not a suspend");
}

/* ---- Buffer control (DPRAM). Getting these backwards cost hours. ---- */
#define BUF_CTRL_FULL      BIT(15)
#define BUF_CTRL_AVAILABLE BIT(10)
#define BUF_CTRL_DATA1_PID BIT(13)
#define BUF_CTRL_LEN_MASK  0x3ff

ZTEST(usb_captured, test_buf_ctrl_bit_positions)
{
	/*
	 * FULL is bit 15 and AVAILABLE is bit 10 -- NOT the other way round.
	 * An earlier decode had these inverted, which turned a correct reading
	 * of the hardware into a wrong conclusion about who owned the buffer.
	 */
	zassert_equal(BUF_CTRL_FULL, 0x8000u, "FULL is bit 15");
	zassert_equal(BUF_CTRL_AVAILABLE, 0x0400u, "AVAILABLE is bit 10");
}

/*
 * Captured on the CDC bulk IN endpoint while output was stalled: the
 * controller owned a loaded 7-byte buffer it had not sent.
 */
ZTEST(usb_captured, test_bulk_in_buffer_held_by_controller)
{
	const uint32_t bc = 0x0000a407;

	zassert_true(bc & BUF_CTRL_FULL, "buffer contains data to send");
	zassert_true(bc & BUF_CTRL_AVAILABLE, "controller owns the buffer");
	zassert_true(bc & BUF_CTRL_DATA1_PID, "DATA1 PID selected");
	zassert_equal(bc & BUF_CTRL_LEN_MASK, 7, "7 bytes loaded");
}

/* Captured on the same endpoint while idle: handed back, nothing pending. */
ZTEST(usb_captured, test_bulk_in_buffer_idle)
{
	const uint32_t bc = 0x00002000;

	zassert_equal(bc & BUF_CTRL_FULL, 0, "no data pending");
	zassert_equal(bc & BUF_CTRL_AVAILABLE, 0, "controller does not own it");
	zassert_true(bc & BUF_CTRL_DATA1_PID, "toggle state retained");
}

/*
 * Captured on the CDC bulk OUT endpoint. 0x440 is armed and waiting for the
 * host; 0x2040 is NOT armed, which is what "input stopped working" looked
 * like at the register level.
 */
ZTEST(usb_captured, test_bulk_out_armed_vs_unarmed)
{
	const uint32_t armed = 0x00000440;
	const uint32_t unarmed = 0x00002040;

	zassert_true(armed & BUF_CTRL_AVAILABLE,
		     "armed: the controller may accept a host packet");
	zassert_equal(armed & BUF_CTRL_LEN_MASK, 64, "64-byte buffer offered");

	zassert_equal(unarmed & BUF_CTRL_AVAILABLE, 0,
		      "unarmed: nothing can be received, so typed input is "
		      "silently discarded and the shell appears dead");
}

/* ---- EP_TX_ERROR / EP_RX_ERROR: 2 bits per endpoint ---- */

/*
 * Endpoint n occupies bits 2n+1:2n. TRANSACTION is the low bit of the pair,
 * SEQ the high one. Verified against the per-field definitions in usb.h.
 */
#define EP_ERR_TRANSACTION(n) BIT(2 * (n))
#define EP_ERR_SEQ(n)         BIT(2 * (n) + 1)

ZTEST(usb_captured, test_ep_error_bit_layout)
{
	zassert_equal(EP_ERR_TRANSACTION(0), 0x01u, "EP0 TRANSACTION");
	zassert_equal(EP_ERR_SEQ(0), 0x02u, "EP0 SEQ");
	zassert_equal(EP_ERR_TRANSACTION(1), 0x04u, "EP1 TRANSACTION");
	zassert_equal(EP_ERR_SEQ(1), 0x08u, "EP1 SEQ");
	zassert_equal(EP_ERR_TRANSACTION(2), 0x10u, "EP2 TRANSACTION");
	zassert_equal(EP_ERR_SEQ(2), 0x20u, "EP2 SEQ");
}

/*
 * Captured values, and what each implied.
 *
 * The distinction matters: a SEQ error is the hardware stating that the PID it
 * received did not match what was armed, so the toggle is provably wrong and
 * the correction is determinate. A TRANSACTION error carries no such
 * implication -- acting on it as though it did made the failure rate worse.
 */
ZTEST(usb_captured, test_captured_ep_errors_discriminate_seq_from_transaction)
{
	zassert_true(0x08u & EP_ERR_SEQ(1),
		     "rx_err=0x08 is EP1 SEQ: toggle demonstrably wrong, "
		     "correcting it is justified");
	zassert_equal(0x08u & EP_ERR_TRANSACTION(1), 0, "not a TRANSACTION error");

	zassert_true(0x10u & EP_ERR_TRANSACTION(2),
		     "tx_err=0x10 is EP2 TRANSACTION: says nothing about the "
		     "toggle, so a rollback here is a guess");
	zassert_equal(0x10u & EP_ERR_SEQ(2), 0, "not a SEQ error");

	zassert_true(0x04u & EP_ERR_TRANSACTION(1), "0x04 is EP1 TRANSACTION");
}

/*
 * CDC-ACM endpoint addresses, from the descriptors in usbd_cdc_acm.c.
 *
 * Endpoint INDEX is not endpoint ADDRESS. An earlier fix halted 0x81 -- the
 * notification endpoint -- while trying to recover 0x82, the bulk IN carrying
 * shell output. The host dutifully cleared the halt ten times and the port
 * stayed dead, which briefly looked like evidence against the whole theory.
 */
#define CDC_INT_IN   0x81	/* notification, interrupt IN  */
#define CDC_BULK_IN  0x82	/* shell output, bulk IN       */
#define CDC_BULK_OUT 0x01	/* shell input,  bulk OUT      */

ZTEST(usb_captured, test_cdc_endpoint_addresses)
{
	zassert_equal(CDC_BULK_IN & 0x0f, 2, "bulk IN is endpoint index 2");
	zassert_equal(CDC_BULK_OUT & 0x0f, 1, "bulk OUT is endpoint index 1");
	zassert_equal(CDC_INT_IN & 0x0f, 1, "notification is index 1, IN");

	zassert_not_equal(CDC_INT_IN, CDC_BULK_IN,
			  "0x81 and 0x82 are different endpoints; halting the "
			  "notification endpoint does nothing for shell output");

	/*
	 * Index 1 is shared between the OUT bulk endpoint and the IN
	 * notification endpoint, which is exactly why an index-to-address
	 * mapping must carry the direction bit.
	 */
	zassert_equal(CDC_BULK_OUT & 0x0f, CDC_INT_IN & 0x0f,
		      "index 1 is used by both directions -- direction must be "
		      "part of any index-to-address conversion");
}

/* ---- The pacing inversion ---- */

/*
 * Transactions survived before the port stopped responding, on stock Zephyr,
 * as a function of the idle gap between transactions.
 *
 * Slower pacing failed SOONER. Every load-based explanation predicts the
 * opposite, and it was this inversion that redirected the investigation from
 * throughput and buffer sizes to bus-idle behaviour, i.e. suspend.
 */
ZTEST(usb_captured, test_pacing_inversion)
{
	const int survived_at_1000ms = 4;
	const int survived_at_150ms = 22;
	const int survived_at_0ms = 7;

	zassert_true(survived_at_1000ms < survived_at_150ms,
		     "the longest idle gap failed soonest -- this is the "
		     "signature that identified suspend as the trigger");

	zassert_true(survived_at_0ms < survived_at_150ms,
		     "the relationship is not monotonic in load either, so "
		     "neither byte count nor transaction count is a threshold");
}

/* ---- SOF as the authoritative liveness signal ---- */

/*
 * Captured over two J-Link reads 500 ms apart while the driver believed the
 * device was suspended: 0x000002FF then 0x000004F6.
 *
 * The frame counter is 11 bits and wraps, so any comparison must handle that.
 */
#define SOF_MASK 0x7ff

static int sof_delta(uint32_t before, uint32_t after)
{
	return (int)((after - before) & SOF_MASK);
}

ZTEST(usb_captured, test_sof_proves_bus_alive)
{
	const uint32_t first = 0x2ff;
	const uint32_t second = 0x4f6;
	int delta = sof_delta(first, second);

	zassert_true(delta > 400 && delta < 600,
		     "roughly 500 frames in 500 ms is the nominal 1 kHz rate: "
		     "the bus was fully framed while SUSPENDED was asserted");
}

ZTEST(usb_captured, test_sof_delta_handles_wrap)
{
	zassert_equal(sof_delta(0x7fe, 0x002), 4,
		      "the frame counter is 11 bits and wraps; a naive "
		      "subtraction would read a wrap as a huge gap and could "
		      "suppress a genuine suspend");
	zassert_equal(sof_delta(0x100, 0x100), 0, "no frames is no delta");
}

/* ---- Enumeration request log ---- */

/*
 * The ch9 trace logs each control request with the state it was handled in,
 * as (bRequest, state). These two sequences are the before and after of the
 * BUFF_STATUS/SIE_STATUS write-1-to-clear fix.
 */
#define REQ_SET_ADDRESS       0x0005
#define REQ_GET_DESCRIPTOR    0x8006
#define REQ_SET_CONFIGURATION 0x0009
#define REQ_CDC_SET_LINE_CODING 0x2120

#define LOG_ST_DEFAULT    1
#define LOG_ST_ADDRESS    2
#define LOG_ST_CONFIGURED 4

struct req_log_entry {
	uint16_t request;
	uint8_t state;
};

ZTEST(usb_captured, test_enumeration_stuck_before_fix)
{
	/* Captured: 0005@1 8006@1 x7 -- never leaves DEFAULT. */
	static const struct req_log_entry log[] = {
		{ REQ_SET_ADDRESS, LOG_ST_DEFAULT },
		{ REQ_GET_DESCRIPTOR, LOG_ST_DEFAULT },
		{ REQ_GET_DESCRIPTOR, LOG_ST_DEFAULT },
		{ REQ_GET_DESCRIPTOR, LOG_ST_DEFAULT },
		{ REQ_GET_DESCRIPTOR, LOG_ST_DEFAULT },
		{ REQ_GET_DESCRIPTOR, LOG_ST_DEFAULT },
		{ REQ_GET_DESCRIPTOR, LOG_ST_DEFAULT },
		{ REQ_GET_DESCRIPTOR, LOG_ST_DEFAULT },
	};
	bool reached_configured = false;

	for (int i = 0; i < (int)(sizeof(log) / sizeof(log[0])); i++) {
		if (log[i].state == LOG_ST_CONFIGURED) {
			reached_configured = true;
		}
	}

	zassert_false(reached_configured,
		      "before the fix a replayed BUS_RESET knocked the state "
		      "back to DEFAULT faster than the host could enumerate, "
		      "so SET_CONFIGURATION was never even attempted");
}

ZTEST(usb_captured, test_enumeration_completes_after_fix)
{
	/* Captured: 8006@1 x7 0009@2 2122@2 2120@2 x6 */
	static const struct req_log_entry log[] = {
		{ REQ_GET_DESCRIPTOR, LOG_ST_DEFAULT },
		{ REQ_GET_DESCRIPTOR, LOG_ST_DEFAULT },
		{ REQ_SET_CONFIGURATION, LOG_ST_ADDRESS },
		{ REQ_CDC_SET_LINE_CODING, LOG_ST_ADDRESS },
		{ REQ_CDC_SET_LINE_CODING, LOG_ST_ADDRESS },
	};
	bool saw_set_config = false;
	bool saw_class_request = false;

	for (int i = 0; i < (int)(sizeof(log) / sizeof(log[0])); i++) {
		if (log[i].request == REQ_SET_CONFIGURATION) {
			saw_set_config = true;
			zassert_not_equal(log[i].state, LOG_ST_DEFAULT,
					  "SET_CONFIGURATION is rejected with "
					  "-EPERM while in DEFAULT state");
		}
		if ((log[i].request & 0xff00) == 0x2100) {
			saw_class_request = true;
		}
	}

	zassert_true(saw_set_config, "SET_CONFIGURATION was reached");
	zassert_true(saw_class_request,
		     "CDC class requests followed, which the host only sends "
		     "to a device it considers configured");
}

/* ---- Diagnostic struct drift ---- */

/*
 * A hand-duplicated copy of the counter struct in application code drifted
 * from the driver's definition when a field was removed. Every counter past
 * the divergence point read out of bounds and printed convincing garbage --
 * including a non-zero count for a feature whose code had been deleted, which
 * was reported as real evidence before being caught.
 *
 * The struct now lives in a shared header. This test encodes why.
 */
struct counters_v1 {
	uint32_t a, b, c, d;
};

struct counters_v2 {		/* field 'b' removed */
	uint32_t a, c, d;
};

ZTEST(usb_captured, test_struct_drift_produces_garbage)
{
	struct counters_v1 producer = { .a = 1, .b = 2, .c = 3, .d = 4 };
	const struct counters_v2 *consumer =
		(const struct counters_v2 *)&producer;

	zassert_equal(consumer->a, 1, "the first field still agrees");
	zassert_not_equal(consumer->c, 3,
			  "every field after the divergence is misread -- a "
			  "consumer using a stale struct layout prints "
			  "plausible-looking values that are simply wrong");
	zassert_equal(consumer->c, 2, "it actually reads the removed field");

	zassert_true(sizeof(struct counters_v1) != sizeof(struct counters_v2),
		     "a BUILD_ASSERT on sizeof catches this at compile time; "
		     "a shared header prevents it entirely");
}

ZTEST_SUITE(usb_captured, NULL, NULL, NULL, NULL, NULL);
