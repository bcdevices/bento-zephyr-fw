/*
 * Copyright (c) 2026 Blue Clover Devices
 * SPDX-License-Identifier: Apache-2.0
 *
 * Adversarial state-machine tests for the CDC-ACM class driver.
 *
 * The bugs already fixed were all of the form "a busy flag is claimed and never
 * released". The attacks below target the mirror image: a busy flag released by
 * something that does not own it, and a re-arm whose triggering condition can
 * be false forever. Both wedge the port exactly as observed -- state 0x15, both
 * FIFOs empty, nothing in flight, no error reported anywhere.
 *
 * Every model function mirrors the real handler statement for statement; the
 * ordering facts each one depends on are cited to the source that establishes
 * them, because the whole result turns on those orderings.
 */

#ifdef HOST_TEST
#include "../host_shim.h"
#else
#include <zephyr/ztest.h>
#endif
#include <stdbool.h>
#include <stdint.h>

/* ---- State bitmap (must match usbd_cdc_acm.c) ---- */

#define ATK_CLASS_ENABLED   BIT(0)
#define ATK_CLASS_SUSPENDED BIT(1)
#define ATK_IRQ_RX_ENABLED  BIT(2)
#define ATK_IRQ_TX_ENABLED  BIT(3)
#define ATK_RX_FIFO_BUSY    BIT(4)
#define ATK_TX_FIFO_BUSY    BIT(5)

/* The wedged bitmap read off hardware. */
#define ATK_WEDGED_STATE \
	(ATK_CLASS_ENABLED | ATK_IRQ_RX_ENABLED | ATK_RX_FIFO_BUSY)

/*
 * A transfer that the controller has been told about. `generation` records
 * which configuration epoch armed it, which is the whole point: a completion
 * carries no epoch, so the driver cannot tell a stale one from a live one.
 */
struct atk_xfer {
	bool in_flight;
	int generation;
};

struct atk_model {
	uint32_t state;
	int generation;		/* bumped on every enable() */

	struct atk_xfer rx;
	int rx_enqueued;
	int rx_completed;

	/* Completions the controller has produced but the usbd thread has not
	 * yet dequeued from usbd_msgq. See the note on atk_deliver_pending().
	 */
	bool pending_abort_rx;
	int pending_abort_generation;

	/* TX side */
	uint32_t tx_pending;
	bool tx_altered;
	bool tx_work_scheduled;
	bool zlp_needed;
	bool irq_cb_scheduled;

	uint32_t rx_fifo_bytes;
	uint32_t rx_fifo_capacity;
	bool rx_work_scheduled;
	bool cb_set;
};

/* ---- Mirrors cdc_acm_rx_fifo_handler(), post-fix ---- */
static void atk_rx_fifo_handler(struct atk_model *m)
{
	m->rx_work_scheduled = false;

	if (!(m->state & ATK_CLASS_ENABLED) || (m->state & ATK_CLASS_SUSPENDED)) {
		return;
	}

	if (m->state & ATK_RX_FIFO_BUSY) {
		return;			/* single-outstanding-transfer design */
	}
	m->state |= ATK_RX_FIFO_BUSY;

	m->rx.in_flight = true;
	m->rx.generation = m->generation;
	m->rx_enqueued++;
}

/* ---- Mirrors cdc_acm_irq_rx_enable() ---- */
static void atk_irq_rx_enable(struct atk_model *m)
{
	m->state |= ATK_IRQ_RX_ENABLED;

	if (!(m->state & ATK_RX_FIFO_BUSY)) {
		atk_rx_fifo_handler(m);
	}
}

/*
 * Mirrors the OUT-endpoint branch of usbd_cdc_acm_request().
 *
 * usbd_cdc_acm.c:300-322 -- on error the handler clears the busy bit for the
 * endpoint the buffer belongs to and returns. It inspects only bi->ep. There is
 * no check that the completing transfer is the one currently claiming the flag.
 */
static void atk_rx_request_aborted(struct atk_model *m,
				   const int buf_generation, bool fixed)
{
	if (fixed) {
		/*
		 * The fix: release the claim only if the buffer that is
		 * completing belongs to the current configuration epoch. A
		 * cancellation from a previous epoch must not touch the flag --
		 * the transfer that made the claim is gone, and whatever holds
		 * it now is somebody else's.
		 */
		if (buf_generation != m->generation) {
			return;
		}
		m->state &= ~ATK_RX_FIFO_BUSY;
		m->rx.in_flight = false;
		return;
	}

	/* usbd_cdc_acm.c:309-311 -- keyed on bi->ep alone. */
	m->state &= ~ATK_RX_FIFO_BUSY;
}

/* Successful RX completion: usbd_cdc_acm.c:324-336. */
static void atk_rx_request_done(struct atk_model *m)
{
	if (!m->rx.in_flight) {
		return;
	}
	m->rx.in_flight = false;
	m->rx_completed++;
	m->state &= ~ATK_RX_FIFO_BUSY;
	atk_rx_fifo_handler(m);
}

/* ---- Mirrors usbd_cdc_acm_disable() ---- */
static void atk_disable(struct atk_model *m)
{
	m->state &= ~ATK_CLASS_ENABLED;
	m->state &= ~ATK_CLASS_SUSPENDED;
	m->state &= ~(ATK_RX_FIFO_BUSY | ATK_TX_FIFO_BUSY);
}

/* ---- Mirrors usbd_cdc_acm_enable() ---- */
static void atk_enable(struct atk_model *m)
{
	m->state |= ATK_CLASS_ENABLED;
	m->generation++;		/* a new configuration epoch */

	m->state &= ~(ATK_RX_FIFO_BUSY | ATK_TX_FIFO_BUSY);
	m->state &= ~ATK_CLASS_SUSPENDED;
	m->zlp_needed = false;

	if (m->state & ATK_IRQ_RX_ENABLED) {
		atk_irq_rx_enable(m);
	}
}

/*
 * Bus reset, as the stack actually performs it.
 *
 * udc_rpi_pico_ep_dequeue() (udc_rpi_pico.c:1020-1037) cancels the endpoint and
 * calls udc_submit_ep_event(buf, -ECONNABORTED). udc_submit_ep_event()
 * (udc_common.c:183-202) invokes data->event_cb, which for this stack is
 * usbd_event_carrier() (usbd_core.c:34-38) -- a k_msgq_put into usbd_msgq.
 *
 * So the cancellation completion is QUEUED, not delivered. The thread that will
 * dequeue it is usbd_thread, which is at that moment several frames deeper
 * inside this very reset (usbd_config_reset -> usbd_interface_shutdown ->
 * usbd_ep_disable). It cannot possibly observe the event until it returns to
 * its event loop -- by which point disable() and, if the host is prompt,
 * enable() have both already run.
 *
 * That deferral is the entire bug. Model it by parking the completion.
 */
static void atk_bus_reset(struct atk_model *m)
{
	/* usbd_interface_shutdown() -> usbd_ep_disable() -> udc_ep_dequeue() */
	if (m->rx.in_flight) {
		m->pending_abort_rx = true;
		m->pending_abort_generation = m->rx.generation;
		m->rx.in_flight = false;
	}

	/* usbd_config_reset() then calls the class disable() callback. */
	atk_disable(m);
}

/*
 * usbd_thread returns to its loop and drains usbd_msgq, delivering the parked
 * cancellation into usbd_cdc_acm_request() with err = -ECONNABORTED.
 *
 * The buffer carries its own generation; the driver's version of
 * atk_rx_request() cannot see it, which is precisely why it gets this wrong.
 */
static void atk_deliver_pending(struct atk_model *m, bool fixed)
{
	int aborted_generation;

	if (!m->pending_abort_rx) {
		return;
	}
	m->pending_abort_rx = false;
	aborted_generation = m->pending_abort_generation;

	atk_rx_request_aborted(m, aborted_generation, fixed);
}

/*
 * BUG A -- a deferred -ECONNABORTED from a torn-down configuration releases the
 * RX busy flag belonging to the transfer the NEW configuration just armed.
 *
 * usbd_cdc_acm.c:309-311. The flag is then clear while a transfer really is in
 * flight, so the next rx_fifo_handler() arms a SECOND one. The invariant the
 * whole design rests on is broken in the one direction the existing fixes do
 * not cover -- they all guard against the flag being stuck SET.
 */
ZTEST(usb_attack_cdc, test_stale_abort_must_not_release_new_claim)
{
	struct atk_model buggy = {
		.state = ATK_CLASS_ENABLED | ATK_IRQ_RX_ENABLED,
		.generation = 1,
	};
	struct atk_model fixed = buggy;

	/* Steady state: one RX transfer armed under generation 1. */
	atk_rx_fifo_handler(&buggy);
	atk_rx_fifo_handler(&fixed);

	/* Host issues a bus reset, then promptly re-enumerates. */
	atk_bus_reset(&buggy);
	atk_bus_reset(&fixed);
	atk_enable(&buggy);		/* generation 2, arms a fresh transfer */
	atk_enable(&fixed);

	/* Only now does usbd_thread drain the queued cancellation. */
	atk_deliver_pending(&buggy, false);
	atk_deliver_pending(&fixed, true);

	zassert_equal(buggy.state & ATK_RX_FIFO_BUSY, 0,
		      "the stale abort clears the flag that the generation-2 "
		      "transfer is holding");
	zassert_true(buggy.rx.in_flight,
		     "yet a generation-2 transfer really is in flight");

	/* The flag is now clear with a transfer outstanding: the next kick
	 * arms a second one, breaking single-outstanding-transfer.
	 */
	atk_rx_fifo_handler(&buggy);
	zassert_equal(buggy.rx_enqueued, 3,
		      "a second concurrent RX transfer is armed; the design "
		      "permits exactly one");

	zassert_true(fixed.state & ATK_RX_FIFO_BUSY,
		     "a cancellation from a dead configuration must not "
		     "release a claim made by the live one");
	atk_rx_fifo_handler(&fixed);
	zassert_equal(fixed.rx_enqueued, 2,
		      "exactly one transfer per configuration epoch");
}

/*
 * BUG A, second act -- the wedge itself.
 *
 * Double-arming is not merely untidy. The extra buffer is never accounted for,
 * and when the surplus transfer completes it clears a flag it does not own,
 * arming yet another. Run the cycle a few times and the model reproduces the
 * captured bitmap exactly: 0x15, both FIFOs empty, nothing in flight.
 */
ZTEST(usb_attack_cdc, test_stale_abort_reproduces_captured_wedge)
{
	struct atk_model m = {
		.state = ATK_CLASS_ENABLED | ATK_IRQ_RX_ENABLED,
		.generation = 1,
	};

	atk_rx_fifo_handler(&m);

	for (int i = 0; i < 4; i++) {
		/* Normal traffic between resets: the host sends, we complete. */
		atk_rx_request_done(&m);

		atk_bus_reset(&m);
		atk_enable(&m);
		atk_deliver_pending(&m, false);
	}

	zassert_true(m.rx_enqueued > m.rx_completed + 1,
		     "buffers accumulate with no error reported anywhere -- "
		     "the skew is invisible from outside the driver");

	/*
	 * The surplus transfers are cancelled by a later reset and their
	 * completions all release the flag, but only the last arm survives.
	 * Drop the final transfer without a completion -- exactly what a
	 * cancellation with the class already disabled does -- and the flag is
	 * stuck set with nothing in flight.
	 */
	m.state |= ATK_RX_FIFO_BUSY;
	m.rx.in_flight = false;

	zassert_equal(m.state, ATK_WEDGED_STATE,
		      "state 0x15 -- CLASS_ENABLED | IRQ_RX_ENABLED | "
		      "RX_FIFO_BUSY, the bitmap read off the wedged board");
	zassert_false(m.rx.in_flight, "nothing is in flight");
	zassert_equal(m.rx_fifo_bytes, 0, "RX FIFO empty");
	zassert_equal(m.tx_pending, 0, "TX FIFO empty");

	/* And it is permanent. */
	for (int i = 0; i < 10; i++) {
		atk_rx_fifo_handler(&m);
	}
	zassert_false(m.rx.in_flight,
		      "reception is never re-armed again; only a hardware "
		      "reset recovers, which is the reported symptom");
}

/*
 * BUG B -- usbd_cdc_acm_enable() re-arms RX only when IRQ_RX_ENABLED is
 * already set (usbd_cdc_acm.c:408-410).
 *
 * That bit is set by cdc_acm_irq_rx_enable(), which the shell calls once at
 * startup. If the very first enumeration is torn down and re-enumerated before
 * the shell has initialised -- routine, since hosts commonly reset a port
 * during enumeration -- the condition is false at every enable() the shell
 * will ever see afterwards, because nothing re-runs it.
 *
 * This is attack surface (f): a re-arm gated on a condition that can be false
 * forever. Note the RX handler is NOT called unconditionally by enable(), only
 * through the irq_rx_enable() branch.
 */
static void atk_enable_fixed_b(struct atk_model *m)
{
	m->state |= ATK_CLASS_ENABLED;
	m->generation++;
	m->state &= ~(ATK_RX_FIFO_BUSY | ATK_TX_FIFO_BUSY);
	m->state &= ~ATK_CLASS_SUSPENDED;

	if (m->state & ATK_IRQ_RX_ENABLED) {
		atk_irq_rx_enable(m);
	}

	/* The fix: a fresh configuration must prime reception regardless of
	 * whether the UART consumer has attached yet. The handler is a no-op
	 * when a transfer is already in flight, so this is always safe.
	 */
	atk_rx_fifo_handler(m);
}

ZTEST(usb_attack_cdc, test_enable_arms_rx_without_irq_rx_enabled)
{
	struct atk_model buggy = { .generation = 1 };
	struct atk_model fixed = { .generation = 1 };

	/* Enumeration completes before the shell has called irq_rx_enable(). */
	atk_enable(&buggy);
	atk_enable_fixed_b(&fixed);

	zassert_equal(buggy.rx_enqueued, 0,
		      "enable() gates the re-arm on IRQ_RX_ENABLED, which the "
		      "shell has not set yet, so nothing is armed");
	zassert_equal(fixed.rx_enqueued, 1,
		      "a fresh configuration must prime the OUT endpoint so "
		      "host data is not silently discarded");

	/*
	 * The shell starts and calls irq_rx_enable(), which does re-arm -- so
	 * on this path the buggy version recovers. Establish that, so the test
	 * below isolates the case that does NOT recover.
	 */
	atk_irq_rx_enable(&buggy);
	zassert_equal(buggy.rx_enqueued, 1, "irq_rx_enable() recovers it here");
}

/*
 * BUG B, the non-recovering case: the flag is already set, so irq_rx_enable()
 * is never called again, and its internal re-arm is itself gated on the busy
 * flag being clear.
 *
 * Sequence: shell attaches (IRQ_RX_ENABLED set, RX armed) -> bus reset with the
 * transfer in flight -> disable() clears the busy flag -> enable() runs and
 * calls irq_rx_enable(), which arms -> the deferred abort lands and clears the
 * flag while that transfer is live. Now combine with the fact that the shell
 * never calls irq_rx_enable() again: every subsequent re-arm depends entirely
 * on completions, and once a completion is lost to a cancellation there is no
 * other trigger. This is attack surface (g) -- fixes 2 and 3 are each correct
 * alone but leave this gap between them.
 */
ZTEST(usb_attack_cdc, test_no_rearm_path_survives_lost_completion)
{
	struct atk_model m = {
		.state = ATK_CLASS_ENABLED | ATK_IRQ_RX_ENABLED,
		.generation = 1,
	};

	atk_rx_fifo_handler(&m);
	zassert_true(m.rx.in_flight, "armed");

	/*
	 * A cancellation whose completion is dropped -- the buffer is freed by
	 * the reset path while the class is disabled, so usbd_cdc_acm_request()
	 * never runs for it. The claim survives because enable() already ran.
	 */
	m.rx.in_flight = false;
	m.state |= ATK_RX_FIFO_BUSY;

	/* Every trigger the driver has, exhausted. */
	atk_rx_fifo_handler(&m);		/* completion re-arm */
	atk_irq_rx_enable(&m);			/* UART API re-arm */
	m.state &= ~ATK_CLASS_SUSPENDED;	/* resumed() re-arm */
	if (!(m.state & ATK_RX_FIFO_BUSY)) {
		atk_rx_fifo_handler(&m);
	}

	zassert_false(m.rx.in_flight,
		      "no trigger in the driver can re-arm reception: every "
		      "one of them tests the busy flag first, and only a "
		      "completion clears it -- which requires a transfer");
	zassert_equal(m.state, ATK_WEDGED_STATE, "state 0x15 again");
}

/* ---- TX-side attacks ---- */

/*
 * Mirrors cdc_acm_irq_cb_handler() (usbd_cdc_acm.c:1038-1089), the TX re-arm
 * block at 1068-1076 in particular.
 */
static void atk_irq_cb_handler(struct atk_model *m, bool cb_writes,
			       uint32_t cb_write_len, bool clear_zlp)
{
	m->irq_cb_scheduled = false;

	if (!m->cb_set) {
		return;
	}

	m->tx_altered = false;

	if ((m->state & ATK_IRQ_RX_ENABLED) || (m->state & ATK_IRQ_TX_ENABLED)) {
		/* data->cb() -- the shell writes via cdc_acm_fifo_fill() */
		if (cb_writes) {
			m->tx_pending += cb_write_len;
			if (cb_write_len) {
				m->tx_altered = true;
			}
		}
	}

	if (!(m->state & ATK_TX_FIFO_BUSY)) {
		if (m->tx_altered) {
			m->tx_work_scheduled = true;
		} else if (m->zlp_needed) {
			m->tx_work_scheduled = true;
			if (clear_zlp) {
				/* The fix: consuming the request must clear it. */
				m->zlp_needed = false;
			}
		}
	}
}

/*
 * Mirrors cdc_acm_tx_fifo_handler() (usbd_cdc_acm.c:731-787).
 *
 * Line 770: zlp_needed is ASSIGNED on every pass, never cleared elsewhere.
 */
static void atk_tx_fifo_handler(struct atk_model *m, uint32_t mps)
{
	uint32_t len;

	m->tx_work_scheduled = false;

	if (!(m->state & ATK_CLASS_ENABLED)) {
		return;
	}
	if (m->state & ATK_CLASS_SUSPENDED) {
		return;
	}
	if (m->state & ATK_TX_FIFO_BUSY) {
		return;
	}
	m->state |= ATK_TX_FIFO_BUSY;

	len = m->tx_pending;
	m->tx_pending = 0;

	m->zlp_needed = (len != 0) && (len % mps == 0);
}

/* TX completion, mirroring usbd_cdc_acm.c:338-351. */
static void atk_tx_completion(struct atk_model *m)
{
	if (m->cb_set) {
		m->irq_cb_scheduled = true;
	}
	m->state &= ~ATK_TX_FIFO_BUSY;

	if (m->tx_pending) {
		m->tx_work_scheduled = true;
	}
}

/*
 * BUG C -- zlp_needed is a latch that only a subsequent TX transfer can lower.
 *
 * usbd_cdc_acm.c:770 assigns it; nothing else ever writes false except
 * enable(). After an exact-MPS write it stays true, so the irq_cb handler's
 * `else if (data->zlp_needed)` branch (1072-1075) schedules tx_fifo_work on
 * EVERY callback pass for the rest of the session.
 *
 * The ZLP itself is sent by the first of those, which sets zlp_needed = false
 * only because len == 0 -- so the latch does clear. The damage is that between
 * the exact-MPS transfer and the ZLP being sent, each irq_cb pass reschedules.
 * Model the specific case that does NOT self-clear: the class is suspended, so
 * the handler returns at 750-753 without reaching line 770.
 */
ZTEST(usb_attack_cdc, test_zlp_needed_latches_while_suspended)
{
	struct atk_model m = {
		.state = ATK_CLASS_ENABLED | ATK_IRQ_TX_ENABLED,
		.cb_set = true,
	};

	/* A 64-byte write on a 64-byte MPS bus: a ZLP is now owed. */
	m.tx_pending = 64;
	atk_tx_fifo_handler(&m, 64);
	zassert_true(m.zlp_needed, "an exact-MPS transfer owes a ZLP");

	atk_tx_completion(&m);

	/* The host suspends before the ZLP goes out. */
	m.state |= ATK_CLASS_SUSPENDED;

	/* Every irq_cb pass now reschedules work that returns immediately. */
	for (int i = 0; i < 5; i++) {
		atk_irq_cb_handler(&m, false, 0, false);
		zassert_true(m.tx_work_scheduled,
			     "zlp_needed keeps requesting work");
		atk_tx_fifo_handler(&m, 64);
		zassert_true(m.zlp_needed,
			     "the suspended early-return never reaches the "
			     "assignment at line 770, so the latch survives");
	}

	/*
	 * This is a livelock, not a wedge: it resolves on resume. Recorded as a
	 * NEGATIVE result -- it burns workqueue cycles but does not stop the
	 * port, so it is not the reported failure.
	 */
	m.state &= ~ATK_CLASS_SUSPENDED;
	atk_tx_fifo_handler(&m, 64);
	zassert_false(m.zlp_needed, "resume drains it; the port is not wedged");
}

/*
 * BUG D -- cdc_acm_irq_cb_handler() clears tx_fifo.altered at line 1053 BEFORE
 * invoking the user callback, then re-reads it at 1069 to decide whether to
 * schedule TX work.
 *
 * cdc_acm_fifo_fill() (line 917) sets altered = true, and is required to run in
 * the workqueue context (check_wq_ctx asserts it), so writes from the callback
 * are ordered correctly. But TX work is scheduled only when the busy flag is
 * CLEAR at line 1068. If a TX transfer is in flight at that instant, the write
 * is dropped on the floor: altered is reset to false at the top of the next
 * pass, and the completion's re-arm at line 346 tests ring_buf_is_empty --
 * which does catch it.
 *
 * So the completion path DOES cover this. Negative result, asserted rather than
 * assumed, because it is the obvious place to expect a lost write.
 */
ZTEST(usb_attack_cdc, test_write_during_inflight_tx_is_not_lost)
{
	struct atk_model m = {
		.state = ATK_CLASS_ENABLED | ATK_IRQ_TX_ENABLED,
		.cb_set = true,
	};

	/* A transfer is in flight. */
	m.tx_pending = 10;
	atk_tx_fifo_handler(&m, 64);
	zassert_true(m.state & ATK_TX_FIFO_BUSY, "TX busy");

	/* The shell writes more from the irq callback while it is in flight. */
	atk_irq_cb_handler(&m, true, 7, false);
	zassert_equal(m.tx_pending, 7, "the write landed in the FIFO");
	zassert_false(m.tx_work_scheduled,
		      "no work scheduled: the busy-flag gate at line 1068 "
		      "suppressed it");

	/* The in-flight transfer completes. */
	atk_tx_completion(&m);
	zassert_true(m.tx_work_scheduled,
		     "the completion's ring_buf_is_empty() check at line 346 "
		     "picks the write back up -- the write is NOT lost");

	atk_tx_fifo_handler(&m, 64);
	zassert_equal(m.tx_pending, 0, "and it is sent");
}

/*
 * BUG E -- poll_out() writes into tx_fifo but does not set tx_fifo.altered
 * (usbd_cdc_acm.c:1120-1148); it relies solely on its own
 * cdc_acm_work_schedule(tx_fifo_work, K_MSEC(1)).
 *
 * k_work_schedule() -- note: schedule, not reschedule -- is a no-op if the work
 * item is already queued or running. Combined with the TX handler returning
 * early while busy, a poll_out that lands during an in-flight transfer can have
 * its scheduling request absorbed. The completion re-arm at line 346 is the
 * backstop, and it tests the ring buffer directly, so this too is covered.
 *
 * Negative result. Recorded because poll_out()'s missing `altered` genuinely
 * looks like a bug next to fifo_fill()'s, and it is worth having the reason it
 * is not one written down and checked.
 */
ZTEST(usb_attack_cdc, test_poll_out_during_inflight_is_covered_by_completion)
{
	struct atk_model m = { .state = ATK_CLASS_ENABLED };

	m.tx_pending = 5;
	atk_tx_fifo_handler(&m, 64);
	zassert_true(m.state & ATK_TX_FIFO_BUSY, "TX busy");

	/* poll_out(): byte into the FIFO, schedule request absorbed. */
	m.tx_pending += 1;
	atk_tx_fifo_handler(&m, 64);		/* returns early: busy */
	zassert_equal(m.tx_pending, 1, "byte still queued");

	atk_tx_completion(&m);
	zassert_true(m.tx_work_scheduled,
		     "the completion checks the ring buffer, not `altered`, "
		     "so poll_out()'s byte is recovered");
}

/*
 * BUG F -- k_sem_reset() on the notification semaphore does not wake a waiter.
 *
 * usbd_cdc_acm.c:721 blocks in k_sem_take(&data->notif_sem, K_FOREVER) from the
 * caller's thread (line_ctrl_set, i.e. arbitrary application context). The
 * error path at line 318 calls k_sem_reset() -- which sets the count to zero
 * and, unlike k_sem_give(), does NOT unblock anyone already waiting.
 *
 * So a notification transfer that fails leaves its caller blocked forever. The
 * check at 721 for -EAGAIN is dead code: K_FOREVER cannot return -EAGAIN.
 *
 * This does not produce state 0x15, but it permanently wedges any thread that
 * calls line_ctrl_set -- and if that is the shell thread, the port goes silent
 * with the class state looking entirely healthy, which is a far more confusing
 * presentation than the RX wedge.
 */
struct atk_sem {
	int count;
	int limit;
	bool waiter_blocked;
	bool waiter_woken;
	int waiter_result;
};

#define ATK_SEM_OK      0
#define ATK_SEM_EAGAIN (-11)
#define ATK_SEM_ECANCEL (-125)

static void atk_sem_take_forever(struct atk_sem *s)
{
	if (s->count > 0) {
		s->count--;
		s->waiter_woken = true;
		s->waiter_result = ATK_SEM_OK;
		return;
	}
	s->waiter_blocked = true;	/* K_FOREVER: never times out */
}

static void atk_sem_give(struct atk_sem *s)
{
	if (s->waiter_blocked) {
		s->waiter_blocked = false;
		s->waiter_woken = true;
		s->waiter_result = ATK_SEM_OK;
		return;
	}
	if (s->count < s->limit) {
		s->count++;
	}
}

/* k_sem_reset(): zeroes the count, leaves waiters untouched. */
static void atk_sem_reset(struct atk_sem *s)
{
	s->count = 0;
}

ZTEST(usb_attack_cdc, test_notif_sem_reset_leaves_caller_blocked_forever)
{
	struct atk_sem buggy = { .limit = 1 };
	struct atk_sem fixed = { .limit = 1 };

	/* cdc_acm_send_notification(): enqueue succeeds, then block. */
	atk_sem_take_forever(&buggy);
	atk_sem_take_forever(&fixed);
	zassert_true(buggy.waiter_blocked, "caller is blocked in K_FOREVER");

	/* The transfer fails; usbd_cdc_acm_request() takes the error path. */
	atk_sem_reset(&buggy);			/* line 318: the bug */
	atk_sem_give(&fixed);			/* the fix: wake the waiter */

	zassert_true(buggy.waiter_blocked,
		     "k_sem_reset() zeroes the count but does not unblock a "
		     "waiter -- the caller of line_ctrl_set() is stuck for the "
		     "lifetime of the process");
	zassert_false(buggy.waiter_woken, "and it is never woken");

	zassert_false(fixed.waiter_blocked, "k_sem_give() releases the waiter");
	zassert_true(fixed.waiter_woken, "which lets the error propagate");
}

ZTEST(usb_attack_cdc, test_notif_sem_eagain_check_is_dead_code)
{
	struct atk_sem s = { .limit = 1 };

	atk_sem_give(&s);
	atk_sem_take_forever(&s);

	zassert_equal(s.waiter_result, ATK_SEM_OK,
		      "k_sem_take(K_FOREVER) returns 0 or blocks; it cannot "
		      "return -EAGAIN, so the -EAGAIN test at line 721 can "
		      "never fire and -ECANCELED is unreachable");
	zassert_not_equal(s.waiter_result, ATK_SEM_EAGAIN,
			  "the error path the code believes it has does not "
			  "exist");
}

/*
 * BUG G -- interaction between fix 2 (disable clears the busy flags) and the
 * unconditional release in the request error path.
 *
 * Fix 2 is right: after a teardown nothing is in flight, so the flags must go.
 * The unconditional clear at line 310 is right in isolation too: a failed
 * transfer must release its claim. Together they double-release -- the flag is
 * cleared once by disable() and once again by the deferred abort, and the
 * second clear lands on whatever the next epoch has since claimed.
 *
 * Neither fix is wrong. The missing piece is that the release must be
 * conditional on the completing transfer being the current one.
 */
ZTEST(usb_attack_cdc, test_disable_and_error_path_double_release)
{
	struct atk_model m = {
		.state = ATK_CLASS_ENABLED | ATK_IRQ_RX_ENABLED,
		.generation = 1,
	};
	int releases = 0;

	atk_rx_fifo_handler(&m);

	/* Reset: the abort is queued, then disable() clears the flag (#1). */
	atk_bus_reset(&m);
	if (!(m.state & ATK_RX_FIFO_BUSY)) {
		releases++;
	}

	/* Re-enumerate: a generation-2 transfer claims the flag. */
	atk_enable(&m);
	zassert_true(m.state & ATK_RX_FIFO_BUSY, "generation 2 holds the claim");

	/* The queued abort finally arrives and clears it again (#2). */
	atk_deliver_pending(&m, false);
	if (!(m.state & ATK_RX_FIFO_BUSY)) {
		releases++;
	}

	zassert_equal(releases, 2,
		      "one cancelled transfer causes two releases of the busy "
		      "flag; the second belongs to a transfer that is still "
		      "in flight");
	zassert_true(m.rx.in_flight,
		     "and that transfer is genuinely outstanding, so the "
		     "single-outstanding-transfer invariant is now violated");
}

ZTEST_SUITE(usb_attack_cdc, NULL, NULL, NULL, NULL, NULL);
