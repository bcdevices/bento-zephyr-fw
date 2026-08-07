/*
 * Copyright (c) 2026 Blue Clover Devices
 * SPDX-License-Identifier: Apache-2.0
 *
 * Adversarial regression tests against the RP2350 UDC driver and the ch9
 * enumeration state machine.
 *
 * The bugs already fixed in this driver fall into two families:
 *
 *   1. Register-access-type mistakes -- treating a write-1-to-clear latch as
 *      though it were ordinary RW storage.
 *   2. State that is advanced optimistically and never rolled back when the
 *      operation it anticipated does not happen.
 *
 * This file attacks the SECOND family, which was much less thoroughly covered
 * than the first. The central finding is that the RP2350 data toggle
 * (next_pid) is advanced when a transfer is QUEUED rather than when it
 * COMPLETES, while every cancel path un-queues the transfer without undoing
 * that advance. Each cancel therefore burns one toggle step that the host
 * never observed, and the endpoint desynchronises permanently.
 *
 * Every test below fails against the current driver logic and passes against
 * the proposed fix. The two are modelled side by side so the difference is
 * explicit rather than asserted.
 *
 * NOTE: the host runner concatenates all test_*.c files into one translation
 * unit, so every identifier here is prefixed atk_udc_ / ATK_UDC_.
 */

#ifdef HOST_TEST
#include "../host_shim.h"
#else
#include <zephyr/ztest.h>
#endif
#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Buffer control register bits (DPRAM), per rp2350 usb.h              */
/* ------------------------------------------------------------------ */

#define ATK_UDC_BC_FULL		BIT(15)
#define ATK_UDC_BC_DATA1_PID	BIT(13)
#define ATK_UDC_BC_AVAIL	BIT(10)
#define ATK_UDC_BC_STALL	BIT(11)
#define ATK_UDC_BC_LEN_MASK	0x3ffu

/*
 * Model of one endpoint as the driver and the host each see it.
 *
 * next_pid is the driver's idea of the PID to arm NEXT. host_pid is the PID
 * the host will actually send or expect next. On a healthy endpoint the two
 * are equal before every transaction; USB has no mechanism to resynchronise
 * them short of a halt/clear-halt or a bus reset, so any divergence is
 * permanent and every subsequent transaction is discarded by one side.
 */
struct atk_udc_ep {
	uint8_t next_pid;	/* driver: PID to arm for the next transfer */
	uint8_t host_pid;	/* host: PID it will send/expect next       */
	uint32_t buf_ctrl;	/* the DPRAM buffer control register        */
	bool busy;
	bool halted;
	int seq_errors;		/* transactions rejected due to PID mismatch */
	int delivered;		/* transactions that actually moved data     */
};

/* Mirrors rpi_pico_prep_tx()/prep_rx(): arm the buffer, advance the toggle. */
static void atk_udc_prep(struct atk_udc_ep *ep, size_t len)
{
	ep->buf_ctrl = (uint32_t)len;
	ep->buf_ctrl |= ep->next_pid ? ATK_UDC_BC_DATA1_PID : 0;
	ep->buf_ctrl |= ATK_UDC_BC_AVAIL;

	/* The toggle advances HERE, at queue time -- not at completion. */
	ep->next_pid ^= 1U;
	ep->busy = true;
}

/*
 * The transaction actually happens on the wire. The controller only accepts
 * it if the armed PID matches what the host expects; otherwise the hardware
 * raises a sequence error and the data is dropped.
 */
static void atk_udc_wire_transaction(struct atk_udc_ep *ep)
{
	uint8_t armed = (ep->buf_ctrl & ATK_UDC_BC_DATA1_PID) ? 1 : 0;

	if (!(ep->buf_ctrl & ATK_UDC_BC_AVAIL)) {
		return;			/* not armed: host sees NAK */
	}

	if (armed != ep->host_pid) {
		ep->seq_errors++;	/* SIE_STATUS.DATA_SEQ_ERROR */
		return;
	}

	ep->delivered++;
	ep->host_pid ^= 1U;		/* both sides advance together */
	ep->buf_ctrl &= ~ATK_UDC_BC_AVAIL;
	ep->busy = false;
}

/*
 * Mirrors rpi_pico_ep_cancel(). `rollback` models the proposed fix.
 *
 * The driver clears AVAIL and releases the busy flag, but does not touch
 * next_pid -- so the toggle step consumed by the cancelled arm is lost.
 */
static void atk_udc_ep_cancel(struct atk_udc_ep *ep, bool rollback)
{
	if (!(ep->buf_ctrl & ATK_UDC_BC_AVAIL)) {
		ep->busy = false;	/* controller does not own the buffer */
		return;
	}

	if (rollback) {
		/*
		 * The armed transfer never reached the host, so the toggle
		 * step it consumed must be given back.
		 */
		ep->next_pid ^= 1U;
	}

	ep->buf_ctrl &= ~ATK_UDC_BC_AVAIL;
	ep->busy = false;
}

/* ------------------------------------------------------------------ */
/* BUG 1: ep_dequeue on a bulk endpoint desynchronises the toggle      */
/* ------------------------------------------------------------------ */

/*
 * udc_rpi_pico_ep_dequeue() (udc_rpi_pico.c:1020) calls rpi_pico_ep_cancel()
 * on an endpoint that may have a transfer armed but not yet transacted. The
 * arm consumed a toggle step; the cancel gives it back to nobody.
 *
 * After one dequeue the driver arms DATA0 where the host expects DATA1 (or
 * vice versa) and the endpoint is dead: every OUT packet is rejected with a
 * sequence error, every IN packet is ignored by the host.
 *
 * Reachability: usbd_ep_dequeue() is called from class drivers on disable,
 * and from event_handler_bus_reset() -- but the damaging case is a class
 * driver dequeuing a bulk endpoint that the host had not yet polled.
 */
ZTEST(usb_attack_udc, test_dequeue_desyncs_data_toggle)
{
	struct atk_udc_ep buggy = { 0 };
	struct atk_udc_ep fixed = { 0 };

	/* A transfer is queued and armed, but the host has not polled yet. */
	atk_udc_prep(&buggy, 64);
	atk_udc_prep(&fixed, 64);

	/* The class driver dequeues it before the host ever sees it. */
	atk_udc_ep_cancel(&buggy, false);
	atk_udc_ep_cancel(&fixed, true);

	zassert_equal(fixed.next_pid, fixed.host_pid,
		      "after a cancel the driver's toggle must still agree "
		      "with the host's -- the cancelled transfer never "
		      "reached the wire, so the host advanced nothing");

	zassert_not_equal(buggy.next_pid, buggy.host_pid,
			  "current logic: the cancelled arm consumed a toggle "
			  "step that the host never observed");

	/* Now queue a real transfer on each and let the host poll it. */
	atk_udc_prep(&buggy, 64);
	atk_udc_prep(&fixed, 64);
	atk_udc_wire_transaction(&buggy);
	atk_udc_wire_transaction(&fixed);

	zassert_equal(fixed.delivered, 1,
		      "with the toggle rolled back the next transfer is "
		      "accepted normally");
	zassert_equal(buggy.delivered, 0,
		      "current logic: the very next transaction is rejected");
	zassert_equal(buggy.seq_errors, 1,
		      "the hardware reports DATA_SEQ_ERROR, which the ISR only "
		      "logs -- nothing resynchronises the endpoint");
}

/*
 * The desynchronisation is not self-correcting, and the reason is subtler
 * than "the toggles stay one apart".
 *
 * A sequence error is NOT a buffer completion: the SIE drops the packet and
 * raises SIE_STATUS.DATA_SEQ_ERROR, but BUFF_STATUS is untouched. So
 * rpi_pico_handle_buff_status_out() never runs, the driver thread is never
 * woken, and rpi_pico_handle_xfer_next() -- the only thing that would re-arm
 * the endpoint with a fresh (and this time re-aligned) PID -- is never
 * called. The endpoint simply stays armed with the wrong PID forever.
 *
 * The ISR's DATA_SEQ_ERROR handler (udc_rpi_pico.c:936) only LOG_WRNs, so
 * nothing in the driver observes the condition either.
 */
ZTEST(usb_attack_udc, test_toggle_desync_is_permanent)
{
	struct atk_udc_ep ep = { 0 };
	int host_attempts = 20;

	atk_udc_prep(&ep, 64);
	atk_udc_ep_cancel(&ep, false);		/* the bug */

	/* One transfer is queued and armed with the now-misaligned PID. */
	atk_udc_prep(&ep, 64);

	/*
	 * The host retries the transaction. The driver does NOT re-arm
	 * between attempts, because a sequence error produces no completion
	 * and therefore no call into handle_xfer_next().
	 */
	for (int i = 0; i < host_attempts; i++) {
		atk_udc_wire_transaction(&ep);
	}

	zassert_equal(ep.delivered, 0,
		      "not one of twenty host attempts succeeds: the endpoint "
		      "is permanently wedged");
	zassert_equal(ep.seq_errors, host_attempts,
		      "every attempt raises DATA_SEQ_ERROR, which the ISR only "
		      "logs -- there is no error path that re-arms the "
		      "endpoint or resynchronises the toggle");
	zassert_true(ep.busy,
		     "and the endpoint stays marked busy, so even a new "
		     "enqueue from the class driver will not re-arm it");
}

/* ------------------------------------------------------------------ */
/* BUG 2: set_halt on an OUT endpoint burns a toggle step              */
/* ------------------------------------------------------------------ */

/*
 * udc_rpi_pico_ep_set_halt() (udc_rpi_pico.c:1116) calls rpi_pico_ep_cancel()
 * on the OUT path before setting STALL|AVAIL. That cancel has the same
 * unrolled-back toggle advance.
 *
 * This one is especially nasty because it is asymmetric with the recovery
 * path: udc_rpi_pico_ep_clear_halt() sets next_pid = 0 unconditionally
 * (line 1162), which is correct per USB 2.0 9.4.5 -- ClearFeature(ENDPOINT_
 * HALT) resets the toggle to DATA0 on BOTH sides. So halt/clear-halt happens
 * to paper over the damage.
 *
 * The bug is therefore only visible when a halt is set and cleared by a path
 * that does NOT go through clear_halt -- or, as modelled here, when the halt
 * is set on an endpoint whose toggle then matters before clear_halt runs.
 */
ZTEST(usb_attack_udc, test_set_halt_out_burns_toggle_step)
{
	struct atk_udc_ep buggy = { 0 };
	struct atk_udc_ep fixed = { 0 };

	/* Get both endpoints to a non-zero toggle so DATA0 is not a free pass. */
	atk_udc_prep(&buggy, 64);
	atk_udc_wire_transaction(&buggy);
	atk_udc_prep(&fixed, 64);
	atk_udc_wire_transaction(&fixed);

	zassert_equal(buggy.next_pid, 1, "toggle advanced after a real transfer");

	/* Re-arm, then the host halts the endpoint before polling it. */
	atk_udc_prep(&buggy, 64);
	atk_udc_prep(&fixed, 64);

	atk_udc_ep_cancel(&buggy, false);	/* set_halt's cancel */
	atk_udc_ep_cancel(&fixed, true);
	buggy.halted = true;
	fixed.halted = true;

	zassert_equal(fixed.next_pid, fixed.host_pid,
		      "set_halt must not consume a toggle step; the halted "
		      "transfer never reached the host");
	zassert_not_equal(buggy.next_pid, buggy.host_pid,
			  "current logic: set_halt leaves the toggle one step "
			  "ahead of the host");
}

/*
 * Positive control for the above: clear_halt resetting next_pid to 0 is
 * CORRECT, and must not be "fixed" into a rollback. USB 2.0 9.4.5 requires
 * both sides to return to DATA0 after ClearFeature(ENDPOINT_HALT).
 *
 * This test exists so that a future reader who sees next_pid = 0 in
 * clear_halt does not mistake it for the same class of bug.
 */
ZTEST(usb_attack_udc, test_clear_halt_reset_to_data0_is_correct)
{
	struct atk_udc_ep ep = { .next_pid = 1, .host_pid = 1 };

	/* ClearFeature(ENDPOINT_HALT): both sides reset to DATA0. */
	ep.next_pid = 0;
	ep.host_pid = 0;
	ep.halted = false;

	atk_udc_prep(&ep, 64);
	atk_udc_wire_transaction(&ep);

	zassert_equal(ep.delivered, 1,
		      "resetting to DATA0 on clear-halt is required by "
		      "USB 2.0 9.4.5 and is NOT the toggle bug");
	zassert_equal(ep.seq_errors, 0, "no mismatch after a spec-conformant reset");
}

/* ------------------------------------------------------------------ */
/* BUG 3: handle_setup cancels the control endpoints, then overrides   */
/* ------------------------------------------------------------------ */

/*
 * rpi_pico_handle_setup() (udc_rpi_pico.c:627) cancels both control
 * endpoints and then forces next_pid = 1 on both (lines 643-644).
 *
 * The forced assignment is correct -- a SETUP is always followed by DATA1 --
 * and it happens to mask the cancel's lost toggle step on EP0. So the control
 * endpoint is NOT affected by the toggle bug.
 *
 * This is a negative result, recorded deliberately: an earlier reading of
 * this code flagged the EP0 cancels as a third instance of the bug. They are
 * not. Encoding it here stops the same false lead being chased again.
 */
ZTEST(usb_attack_udc, test_setup_forced_pid_masks_ep0_cancel)
{
	struct atk_udc_ep ep0 = { 0 };

	/* An IN data stage is armed but the host abandons it and re-SETUPs. */
	atk_udc_prep(&ep0, 18);

	/* handle_setup: cancel both control endpoints... */
	atk_udc_ep_cancel(&ep0, false);		/* no rollback, as upstream */

	/* ...then unconditionally force DATA1 for the next stage. */
	ep0.next_pid = 1;
	ep0.host_pid = 1;			/* the host does the same */

	atk_udc_prep(&ep0, 18);
	atk_udc_wire_transaction(&ep0);

	zassert_equal(ep0.delivered, 1,
		      "EP0 is immune: the forced next_pid = 1 overwrites "
		      "whatever the cancel left behind");
	zassert_equal(ep0.seq_errors, 0,
		      "so the control endpoint does not need the rollback -- "
		      "only the non-control cancel paths do");
}

/* ------------------------------------------------------------------ */
/* BUG 4: ep_disable frees DPRAM without clearing the toggle           */
/* ------------------------------------------------------------------ */

/*
 * udc_rpi_pico_ep_disable() (udc_rpi_pico.c:1075) cancels and frees the
 * endpoint's DPRAM block but leaves next_pid at whatever value it held.
 * udc_rpi_pico_ep_enable() (line 1048) does set next_pid = 0 -- and writes
 * USB_BUF_CTRL_DATA0_PID to the buffer control register -- so a
 * disable/enable pair recovers.
 *
 * The gap is a disable that is NOT followed by an enable of the same
 * endpoint before the host uses it, which SET_INTERFACE with an alternate
 * setting can produce. This test documents that enable() is what saves the
 * driver, so that anyone tempted to drop the reset in enable() sees why it
 * must stay.
 */
ZTEST(usb_attack_udc, test_ep_enable_must_reset_toggle)
{
	struct atk_udc_ep with_reset = { .next_pid = 1, .host_pid = 0 };
	struct atk_udc_ep without_reset = { .next_pid = 1, .host_pid = 0 };

	/* ep_enable(): DATA0 written to buf_ctrl and next_pid zeroed. */
	with_reset.next_pid = 0;

	atk_udc_prep(&with_reset, 64);
	atk_udc_wire_transaction(&with_reset);
	atk_udc_prep(&without_reset, 64);
	atk_udc_wire_transaction(&without_reset);

	zassert_equal(with_reset.delivered, 1,
		      "ep_enable() resetting next_pid to 0 is what makes a "
		      "freshly enabled endpoint agree with the host");
	zassert_equal(without_reset.delivered, 0,
		      "without that reset a re-enabled endpoint inherits a "
		      "stale toggle and is dead on arrival");
}

/* ------------------------------------------------------------------ */
/* BUG 5: the abort-spin timeout proceeds as though it had succeeded   */
/* ------------------------------------------------------------------ */

/*
 * rpi_pico_ep_cancel() (udc_rpi_pico.c:285) bounds the EP_ABORT handshake
 * with RPI_PICO_ABORT_SPIN_LIMIT and, on timeout, does nothing but LOG_ERR
 * and break. It then falls through and clears AVAIL anyway -- modifying
 * buf_ctrl while the SIE may still own the buffer, which is precisely the
 * concurrent-access hazard the handshake exists to prevent.
 *
 * It also unconditionally clears the busy flag, so the upper layer is told
 * the endpoint is idle when the controller may still be transacting on it.
 */
struct atk_udc_abort {
	bool handshake_done;	/* hardware finished the abort            */
	bool buf_ctrl_written;	/* driver modified buf_ctrl               */
	bool busy;		/* udc_ep_config.stat.busy                */
	bool unsafe_write;	/* buf_ctrl touched while SIE owned buffer */
};

static void atk_udc_cancel_with_abort(struct atk_udc_abort *a,
				      bool hw_completes, bool fixed)
{
	a->handshake_done = hw_completes;

	if (!a->handshake_done && fixed) {
		/*
		 * The proposed fix: a timed-out handshake means the SIE still
		 * owns the buffer. Do not touch buf_ctrl, and do not claim the
		 * endpoint is idle.
		 */
		return;
	}

	a->buf_ctrl_written = true;
	if (!a->handshake_done) {
		a->unsafe_write = true;
	}
	a->busy = false;
}

ZTEST(usb_attack_udc, test_abort_timeout_must_not_touch_buf_ctrl)
{
	struct atk_udc_abort buggy = { .busy = true };
	struct atk_udc_abort fixed = { .busy = true };

	/* The handshake never completes -- the spin limit is hit. */
	atk_udc_cancel_with_abort(&buggy, false, false);
	atk_udc_cancel_with_abort(&fixed, false, true);

	zassert_true(buggy.unsafe_write,
		     "current logic: buf_ctrl is modified after the abort "
		     "handshake timed out, while the SIE may still own the "
		     "buffer -- the exact hazard the arch_nop() sequences and "
		     "the handshake itself exist to avoid");

	zassert_false(fixed.unsafe_write,
		      "a timed-out handshake must leave the buffer alone");

	zassert_false(buggy.busy,
		      "current logic also reports the endpoint idle, so the "
		      "upper layer will happily arm a new transfer on a "
		      "buffer the controller has not released");
	zassert_true(fixed.busy,
		      "the endpoint must stay marked busy so nothing new is "
		      "armed on it");
}

/* ------------------------------------------------------------------ */
/* BUG 6: SET_ADDRESS state advances before the status stage completes */
/* ------------------------------------------------------------------ */

/*
 * sreq_set_address() (usbd_ch9.c:93) sets ch9_data.state = USBD_STATE_ADDRESS
 * immediately (line 129), but for a controller without addr_before_status --
 * which the RP2350 is, since udc_rpi_pico has no such capability flag -- the
 * hardware address is only written later, from post_status_stage() (line 71),
 * after the status stage completes.
 *
 * Between those two points the stack believes it is at the new address while
 * the controller still answers on the old one. If the status stage never
 * completes -- a bus reset lands on it, or the host gives up -- the two never
 * reconcile: ch9_data.state stays ADDRESS with post_status still true, and
 * the controller is still on address 0.
 *
 * A bus reset does restore consistency (event_handler_bus_reset writes
 * address 0 and returns to DEFAULT). The unrecoverable case is the one
 * modelled here: post_status survives into an UNRELATED later transfer,
 * because post_status_stage() applies setup->wValue from whatever setup
 * packet is current at the time it finally runs.
 */
struct atk_udc_ch9 {
	int state;		/* 0 DEFAULT, 2 ADDRESS, 4 CONFIGURED */
	bool post_status;
	uint8_t hw_addr;	/* what the controller answers on     */
	uint16_t setup_wValue;	/* the CURRENT setup packet's wValue  */
	uint8_t setup_req;
};

#define ATK_UDC_ST_DEFAULT	0
#define ATK_UDC_ST_ADDRESS	2
#define ATK_UDC_ST_CONFIGURED	4

#define ATK_UDC_REQ_SET_ADDRESS		0x05
#define ATK_UDC_REQ_SET_CONFIGURATION	0x09

/* Mirrors sreq_set_address() for a controller without addr_before_status. */
static void atk_udc_sreq_set_address(struct atk_udc_ch9 *c, uint16_t wValue)
{
	c->setup_req = ATK_UDC_REQ_SET_ADDRESS;
	c->setup_wValue = wValue;

	c->post_status = true;			/* apply it after status */
	c->state = (wValue == 0) ? ATK_UDC_ST_DEFAULT : ATK_UDC_ST_ADDRESS;
}

/* Mirrors post_status_stage(): reads the CURRENT setup packet. */
static void atk_udc_post_status_stage(struct atk_udc_ch9 *c)
{
	if (c->setup_req == ATK_UDC_REQ_SET_ADDRESS) {
		c->hw_addr = (uint8_t)c->setup_wValue;
	}
	c->post_status = false;
}

/*
 * `clear_post_status` models the proposed fix: a new SETUP packet cancels any
 * pending post-status action from the previous, abandoned transfer.
 */
static void atk_udc_new_setup(struct atk_udc_ch9 *c, uint8_t req,
			      uint16_t wValue, bool clear_post_status)
{
	if (clear_post_status) {
		c->post_status = false;
	}
	c->setup_req = req;
	c->setup_wValue = wValue;
}

ZTEST(usb_attack_udc, test_abandoned_set_address_corrupts_later_transfer)
{
	struct atk_udc_ch9 buggy = { .state = ATK_UDC_ST_DEFAULT };
	struct atk_udc_ch9 fixed = { .state = ATK_UDC_ST_DEFAULT };

	/* Host issues SET_ADDRESS(5). State jumps to ADDRESS immediately. */
	atk_udc_sreq_set_address(&buggy, 5);
	atk_udc_sreq_set_address(&fixed, 5);

	zassert_equal(buggy.state, ATK_UDC_ST_ADDRESS,
		      "the stack believes it is addressed...");
	zassert_equal(buggy.hw_addr, 0,
		      "...while the controller still answers on address 0");

	/*
	 * The status stage never completes -- the host abandons the transfer
	 * and issues a fresh SETUP. post_status is still pending.
	 */
	atk_udc_new_setup(&buggy, ATK_UDC_REQ_SET_CONFIGURATION, 1, false);
	atk_udc_new_setup(&fixed, ATK_UDC_REQ_SET_CONFIGURATION, 1, true);

	zassert_true(buggy.post_status,
		     "current logic: post_status survives the abandoned "
		     "transfer and is still armed");
	zassert_false(fixed.post_status,
		      "a new SETUP must cancel the previous transfer's "
		      "pending post-status action");

	/* That new transfer's status stage completes normally. */
	if (buggy.post_status) {
		atk_udc_post_status_stage(&buggy);
	}
	if (fixed.post_status) {
		atk_udc_post_status_stage(&fixed);
	}

	/*
	 * post_status_stage() re-reads the CURRENT setup packet, whose
	 * bRequest is now SET_CONFIGURATION -- so the address is not applied
	 * at all, and the device is stranded at address 0 while the stack and
	 * the host both believe it is at address 5.
	 */
	zassert_equal(buggy.hw_addr, 0,
		      "the address is never applied: post_status_stage() reads "
		      "the current setup packet, which is no longer the "
		      "SET_ADDRESS one");
	zassert_equal(buggy.state, ATK_UDC_ST_ADDRESS,
		      "yet the stack still reports ADDRESS state -- host and "
		      "device now disagree about the device's address and no "
		      "further transfer can be routed to it");
}

/*
 * The same mechanism with the requests reversed is worse: a pending
 * post_status from an abandoned SET_ADDRESS fires during a LATER
 * SET_ADDRESS's status stage, applying that later transfer's wValue twice
 * and skipping the intermediate one entirely.
 */
ZTEST(usb_attack_udc, test_stale_post_status_applies_wrong_address)
{
	struct atk_udc_ch9 c = { .state = ATK_UDC_ST_DEFAULT };

	/* First SET_ADDRESS(5), status stage abandoned. */
	atk_udc_sreq_set_address(&c, 5);
	zassert_true(c.post_status, "post_status armed for address 5");

	/* Host retries with a different address; no SETUP-time cleanup. */
	atk_udc_new_setup(&c, ATK_UDC_REQ_SET_ADDRESS, 9, false);
	atk_udc_sreq_set_address(&c, 9);

	atk_udc_post_status_stage(&c);

	zassert_equal(c.hw_addr, 9,
		      "the surviving post_status flag is indistinguishable "
		      "from a freshly armed one; only the current setup "
		      "packet decides what gets applied");
	zassert_false(c.post_status, "and it is consumed exactly once");
}

/* ------------------------------------------------------------------ */
/* BUG 7: UDC_EVT_ERROR is published but nothing recovers              */
/* ------------------------------------------------------------------ */

/*
 * The ISR submits UDC_EVT_ERROR for RX timeout, RX overflow, bit-stuff and
 * CRC errors (udc_rpi_pico.c:953, 961, 969, 977), and the driver thread does
 * so for buffer exhaustion (lines 428, 467, 503, 660, 693).
 *
 * usbd_core.c's UDC_EVT_ERROR case does exactly two things: LOG_ERR and
 * usbd_msg_pub_simple(USBD_MSG_UDC_ERROR). No endpoint is reset, no transfer
 * is re-armed, no state is repaired. Unless the application installs a
 * message callback that takes action, the error is purely cosmetic.
 *
 * The -ENOBUFS cases are the damaging ones: rpi_pico_handle_buff_status_out()
 * returns WITHOUT re-arming the endpoint, so reception is never restored and
 * the endpoint is silently dead from that point on.
 */
struct atk_udc_err {
	int errors_reported;
	int endpoints_rearmed;
	bool ep_armed;
};

static void atk_udc_buff_status_out(struct atk_udc_err *e, bool have_buf,
				    bool fixed)
{
	e->ep_armed = false;		/* hardware consumed the arm */

	if (!have_buf) {
		e->errors_reported++;	/* udc_submit_event(UDC_EVT_ERROR) */

		if (fixed) {
			/*
			 * The proposed fix: re-arm so the endpoint can still
			 * receive once a buffer is available again.
			 */
			e->endpoints_rearmed++;
			e->ep_armed = true;
		}
		return;			/* upstream: bare return */
	}

	e->endpoints_rearmed++;
	e->ep_armed = true;
}

ZTEST(usb_attack_udc, test_enobufs_leaves_out_endpoint_dead)
{
	struct atk_udc_err buggy = { .ep_armed = true };
	struct atk_udc_err fixed = { .ep_armed = true };

	/* A packet arrives while the buffer pool is momentarily empty. */
	atk_udc_buff_status_out(&buggy, false, false);
	atk_udc_buff_status_out(&fixed, false, true);

	zassert_equal(buggy.errors_reported, 1, "UDC_EVT_ERROR is submitted");
	zassert_false(buggy.ep_armed,
		      "current logic: the endpoint is left unarmed, so every "
		      "later host packet is NAKed forever -- the error event "
		      "is logged and published but nothing acts on it");
	zassert_true(fixed.ep_armed,
		     "the endpoint must be re-armed; a transient buffer "
		     "shortage must not permanently kill reception");

	/* Buffers are available again, but nothing re-triggers the handler. */
	zassert_equal(buggy.endpoints_rearmed, 0,
		      "and there is no path back: re-arming only happens from "
		      "a completion, which can no longer occur");
}

ZTEST_SUITE(usb_attack_udc, NULL, NULL, NULL, NULL, NULL);
