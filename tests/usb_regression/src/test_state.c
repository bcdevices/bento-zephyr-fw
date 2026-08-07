/*
 * Copyright (c) 2026 Blue Clover Devices
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB state-machine regression tests.
 *
 * The second family of bugs found was state handling: busy flags claimed and
 * never released, events dropped by a gate, an early return that skipped a
 * state assignment, and a fix of mine that armed two transfers where the
 * design allows one.
 *
 * These are all reachable off-target because they are ordinary logic over a
 * handful of flags. Each test below encodes one bug as an event sequence and
 * asserts the invariant that was violated on hardware.
 */

#ifdef HOST_TEST
#include "../host_shim.h"
#else
#include <zephyr/ztest.h>
#endif
#include <stdbool.h>
#include <stdint.h>

/* ---- Model of the CDC-ACM class driver's state bitmap ---- */

#define CLASS_ENABLED   BIT(0)
#define CLASS_SUSPENDED BIT(1)
#define IRQ_RX_ENABLED  BIT(2)
#define IRQ_TX_ENABLED  BIT(3)
#define RX_FIFO_BUSY    BIT(4)
#define TX_FIFO_BUSY    BIT(5)

struct cdc_model {
	uint32_t state;
	int rx_outstanding;	/* transfers actually queued in the UDC */
	int rx_enqueued;	/* cumulative, mirrors the on-target counter */
	int rx_completed;	/* cumulative */
	int tx_pending;		/* bytes left in the TX FIFO */
	bool tx_work_scheduled;
};

/* Mirrors cdc_acm_rx_fifo_handler(). */
static void rx_fifo_handler(struct cdc_model *m, bool alloc_ok, bool enqueue_ok)
{
	if (!(m->state & CLASS_ENABLED) || (m->state & CLASS_SUSPENDED)) {
		return;
	}

	if (m->state & RX_FIFO_BUSY) {
		return;			/* single-outstanding-transfer design */
	}
	m->state |= RX_FIFO_BUSY;

	if (!alloc_ok) {
		m->state &= ~RX_FIFO_BUSY;	/* fixed: release the claim */
		return;
	}

	if (!enqueue_ok) {
		m->state &= ~RX_FIFO_BUSY;	/* fixed: release the claim */
		return;
	}

	m->rx_outstanding++;
	m->rx_enqueued++;
}

/* Mirrors the OUT branch of usbd_cdc_acm_request(). */
static void rx_completion(struct cdc_model *m)
{
	if (m->rx_outstanding == 0) {
		return;			/* nothing in flight to complete */
	}
	m->rx_outstanding--;
	m->rx_completed++;
	m->state &= ~RX_FIFO_BUSY;
	rx_fifo_handler(m, true, true);
}

ZTEST(usb_state, test_rx_busy_released_on_alloc_failure)
{
	struct cdc_model m = { .state = CLASS_ENABLED | IRQ_RX_ENABLED };

	rx_fifo_handler(&m, false, true);

	zassert_equal(m.state & RX_FIFO_BUSY, 0,
		      "RX_FIFO_BUSY must be released when no buffer could be "
		      "allocated; leaving it set makes every later call return "
		      "early and the port stops accepting input permanently");

	/* Reception must still be recoverable. */
	rx_fifo_handler(&m, true, true);
	zassert_equal(m.rx_enqueued, 1, "RX must re-arm after a failed attempt");
}

ZTEST(usb_state, test_rx_busy_released_on_enqueue_failure)
{
	struct cdc_model m = { .state = CLASS_ENABLED | IRQ_RX_ENABLED };

	rx_fifo_handler(&m, true, false);

	zassert_equal(m.state & RX_FIFO_BUSY, 0,
		      "RX_FIFO_BUSY must be released when the enqueue fails");

	rx_fifo_handler(&m, true, true);
	zassert_equal(m.rx_enqueued, 1, "RX must re-arm after a failed enqueue");
}

/*
 * Suspend does not cancel transfers: usbd_core only updates status and
 * broadcasts, it never dequeues or disables an endpoint. Clearing the busy
 * flags on resume therefore arms a second transfer while the first is still
 * in flight, and the outstanding count ratchets up by one per resume.
 *
 * That produces an enqueued-minus-completed skew of exactly one per resume
 * with zero errors -- which is indistinguishable from a lost completion, and
 * was in fact misdiagnosed as one during this investigation.
 */
static void resume_buggy(struct cdc_model *m)
{
	m->state &= ~CLASS_SUSPENDED;
	m->state &= ~RX_FIFO_BUSY;	/* the bug */
	rx_fifo_handler(m, true, true);
}

static void resume_fixed(struct cdc_model *m)
{
	m->state &= ~CLASS_SUSPENDED;
	if (!(m->state & RX_FIFO_BUSY)) {
		rx_fifo_handler(m, true, true);
	}
}

ZTEST(usb_state, test_resume_must_not_double_arm)
{
	struct cdc_model buggy = { .state = CLASS_ENABLED | IRQ_RX_ENABLED };
	struct cdc_model fixed = { .state = CLASS_ENABLED | IRQ_RX_ENABLED };

	rx_fifo_handler(&buggy, true, true);
	rx_fifo_handler(&fixed, true, true);

	for (int i = 0; i < 3; i++) {
		buggy.state |= CLASS_SUSPENDED;
		fixed.state |= CLASS_SUSPENDED;
		resume_buggy(&buggy);
		resume_fixed(&fixed);
	}

	zassert_equal(buggy.rx_outstanding, 4,
		      "the buggy resume leaks one outstanding buffer per "
		      "resume; the shared UDC pool eventually runs dry");
	zassert_equal(fixed.rx_outstanding, 1,
		      "the single-outstanding-transfer invariant must hold "
		      "across any number of suspend/resume cycles");
}

ZTEST(usb_state, test_skew_looks_like_a_lost_completion)
{
	struct cdc_model m = { .state = CLASS_ENABLED | IRQ_RX_ENABLED };

	rx_fifo_handler(&m, true, true);

	for (int i = 0; i < 7; i++) {
		rx_completion(&m);
		m.state |= CLASS_SUSPENDED;
		resume_buggy(&m);
	}

	zassert_true(m.rx_enqueued - m.rx_completed > 1,
		     "double-arming produces an enqueued/completed skew with "
		     "no errors -- do not read such a skew as proof that the "
		     "controller lost a transfer");
}

/*
 * usbd_class_bcast_event() dropped SUSPEND and RESUME while unconfigured, and
 * only disable() ever cleared CLASS_SUSPENDED. A suspend delivered while
 * configured whose resume was dropped during the post-reset window left the
 * class suspended forever, with both FIFO handlers returning early and no busy
 * flag to explain it.
 */
ZTEST(usb_state, test_enable_clears_stale_suspend)
{
	struct cdc_model m = { .state = CLASS_ENABLED | IRQ_RX_ENABLED };

	m.state |= CLASS_SUSPENDED;		/* suspend delivered */
	/* resume dropped by the configured-state gate: nothing clears it */

	/* Bus reset then re-enumeration reaches enable(). */
	m.state |= CLASS_ENABLED;
	m.state &= ~CLASS_SUSPENDED;		/* the fix */
	m.state &= ~(RX_FIFO_BUSY | TX_FIFO_BUSY);

	zassert_equal(m.state & CLASS_SUSPENDED, 0,
		      "reaching enable() means SET_CONFIGURATION just arrived, "
		      "so the class cannot be suspended");

	rx_fifo_handler(&m, true, true);
	zassert_equal(m.rx_enqueued, 1, "RX must work after re-enumeration");
}

/*
 * cdc_acm_tx_fifo_handler() released TX_FIFO_BUSY on enqueue failure but
 * scheduled no retry, unlike the alloc-failure path directly above it. Queued
 * output was then stranded: the completion re-arm cannot fire because nothing
 * is in flight, and the irq_cb re-arm requires tx_fifo.altered, which only a
 * fresh write sets.
 */
static void tx_fifo_handler(struct cdc_model *m, bool enqueue_ok, bool fixed)
{
	m->tx_work_scheduled = false;

	if (m->state & TX_FIFO_BUSY) {
		return;
	}
	m->state |= TX_FIFO_BUSY;

	if (!enqueue_ok) {
		m->state &= ~TX_FIFO_BUSY;
		if (fixed) {
			m->tx_work_scheduled = true;
		}
		return;
	}

	m->tx_pending = 0;
}

ZTEST(usb_state, test_tx_enqueue_failure_reschedules)
{
	struct cdc_model buggy = { .state = CLASS_ENABLED, .tx_pending = 25 };
	struct cdc_model fixed = { .state = CLASS_ENABLED, .tx_pending = 25 };

	tx_fifo_handler(&buggy, false, false);
	tx_fifo_handler(&fixed, false, true);

	zassert_true(buggy.tx_pending > 0 && !buggy.tx_work_scheduled,
		     "without a retry the queued output is stranded with no "
		     "work item able to send it");
	zassert_true(fixed.tx_work_scheduled,
		     "a failed enqueue must reschedule, as the alloc-failure "
		     "path does");
}

/* ---- Model of the ch9 enumeration state machine ---- */

enum ch9_state { ST_DEFAULT = 0, ST_ADDRESS = 2, ST_CONFIGURED = 4 };

struct ch9_model {
	enum ch9_state state;
	uint8_t configuration;
	uint32_t ep_halt;
	int broadcasts_delivered;
	int broadcasts_dropped;
};

/* Mirrors sreq_set_configuration()'s "already in the configuration" path. */
static void set_configuration(struct ch9_model *m, uint8_t wValue, bool fixed)
{
	if (m->state == ST_DEFAULT) {
		return;			/* -EPERM */
	}

	if (wValue == m->configuration) {
		if (fixed) {
			m->state = (wValue == 0) ? ST_ADDRESS : ST_CONFIGURED;
		}
		return;			/* upstream returns with no state change */
	}

	m->configuration = wValue;
	m->state = (wValue == 0) ? ST_ADDRESS : ST_CONFIGURED;
}

ZTEST(usb_state, test_set_configuration_reasserts_state)
{
	struct ch9_model buggy = { .state = ST_ADDRESS, .configuration = 1 };
	struct ch9_model fixed = { .state = ST_ADDRESS, .configuration = 1 };

	/* Host re-sends SET_CONFIGURATION(1) for the value we already hold. */
	set_configuration(&buggy, 1, false);
	set_configuration(&fixed, 1, true);

	zassert_not_equal(buggy.state, ST_CONFIGURED,
			  "upstream ACKs the request while leaving the state "
			  "unconfigured, so the host and device disagree");
	zassert_equal(fixed.state, ST_CONFIGURED,
		      "the state must be re-asserted before returning");
}

/*
 * event_handler_bus_reset() returned early when usbd_config_set(0) failed,
 * skipping the state reset, the ep_halt clear and the rwup clear -- while
 * USBD_MSG_RESET was still published. Every later reset failed identically, so
 * the device could never return to DEFAULT and re-enumerate.
 */
static void bus_reset(struct ch9_model *m, bool config_set_fails, bool fixed)
{
	if (config_set_fails && !fixed) {
		return;			/* the early return */
	}

	m->state = ST_DEFAULT;
	m->configuration = 0;
	m->ep_halt = 0;
}

ZTEST(usb_state, test_bus_reset_always_reaches_default)
{
	struct ch9_model buggy = {
		.state = ST_CONFIGURED, .configuration = 1, .ep_halt = BIT(2),
	};
	struct ch9_model fixed = {
		.state = ST_CONFIGURED, .configuration = 1, .ep_halt = BIT(2),
	};

	bus_reset(&buggy, true, false);
	bus_reset(&fixed, true, true);

	zassert_not_equal(buggy.state, ST_DEFAULT,
			  "the early return leaves the stack unable to "
			  "re-enumerate, permanently");
	zassert_equal(fixed.state, ST_DEFAULT,
		      "a reset must always reach DEFAULT; that is the one "
		      "thing that lets the host re-enumerate us");
	zassert_equal(fixed.ep_halt, 0,
		      "hardware clears endpoint halts on reset (USB 2.0 "
		      "9.4.5); a stale bitmap misreports GET_STATUS");
}

/*
 * Suspend is orthogonal to Default/Address/Configured (USB 2.0 9.1.1.6).
 * Gating the class broadcast on CONFIGURED dropped a resume that arrived
 * during the post-reset window, leaving class drivers suspended forever.
 */
static void broadcast(struct ch9_model *m, bool gate_on_configured)
{
	if (gate_on_configured && m->state != ST_CONFIGURED) {
		m->broadcasts_dropped++;
		return;
	}
	m->broadcasts_delivered++;
}

ZTEST(usb_state, test_resume_after_reset_is_delivered)
{
	struct ch9_model buggy = { .state = ST_DEFAULT };
	struct ch9_model fixed = { .state = ST_DEFAULT };

	/* Suspend arrived while configured; reset dropped us to DEFAULT; the
	 * resume now arrives before SET_CONFIGURATION.
	 */
	broadcast(&buggy, true);
	broadcast(&fixed, false);

	zassert_equal(buggy.broadcasts_dropped, 1,
		      "the configured-state gate drops the resume, so class "
		      "drivers stay suspended with no further resume coming");
	zassert_equal(fixed.broadcasts_delivered, 1,
		      "suspend and resume are device-level events and must be "
		      "delivered regardless of configuration state");
}

ZTEST_SUITE(usb_state, NULL, NULL, NULL, NULL, NULL);
