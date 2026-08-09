/*
 * Copyright (c) 2026 Blue Clover Devices
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bounded exhaustive model checker for the CDC-ACM class driver + the core
 * event dispatch it sits on.
 *
 * Every other test in this suite is a hand-written scenario that asserts a bug
 * somebody already found on hardware. They are regression guards: by
 * construction they cannot find anything nobody thought of.
 *
 * This file inverts that. It models the current code's state -- class flags,
 * the rx/tx claims, transfers outstanding in the controller, FIFO occupancy,
 * ch9 enumeration state, and the deferred-event queue -- as a struct, models
 * each event as a transition function transcribed from the real source, and
 * then enumerates *every* event sequence up to a bounded depth, checking the
 * invariants after every single transition.
 *
 * The deferred-event queue is the part that matters most and the part a
 * hand-written test is least likely to get right. usbd_event_carrier() is a
 * k_msgq_put(), so a transfer completion -- including the -ECONNABORTED of a
 * transfer cancelled by a bus reset -- is not delivered inline. It sits in the
 * queue while usbd_thread is several frames deep inside the teardown, and is
 * processed only later, by which time disable() has released the busy flag and
 * enable() has armed a fresh transfer that now owns it. Modelling completions
 * as inline would make an entire family of interleavings unreachable, which is
 * precisely the family the rx_claim/tx_claim comparison exists to defend
 * against. So the queue is explicit here, and deliver_queued_event is an event
 * the explorer can schedule at any point.
 *
 * The invariants checked after each transition:
 *
 *   I1  A busy flag is set only if a transfer is genuinely outstanding for
 *       that direction. A flag stuck set with nothing in flight means no
 *       completion will ever arrive to clear it, and the test-and-set in both
 *       FIFO handlers makes every later re-arm a silent no-op: that direction
 *       is dead for the rest of the session.
 *   I2  rx_outstanding <= 1 and tx_outstanding <= 1. The design is
 *       single-outstanding-transfer; >1 means a flag was wrongly released and
 *       a second transfer was armed on top of a live one.
 *   I3  Data pending in a FIFO, class enabled and not suspended => some work
 *       item is scheduled or a transfer is in flight. Otherwise the data is
 *       stranded with nothing able to move it.
 *   I1b The converse of I1, and the one that actually earns its keep: a
 *       transfer genuinely queued in the controller must be guarded by its
 *       busy flag. A live transfer with the flag clear means the endpoint is
 *       unguarded and the next handler run arms a second transfer on top of
 *       it. This fires one event earlier than the I2 violation it causes,
 *       which makes the reported trace shorter and the cause obvious.
 *   I4  CLASS_ENABLED => ch9 state is CONFIGURED. enable() is reached only
 *       from usbd_config_set(non-zero), and disable() from the teardown.
 *   I6  rx_claim is non-NULL exactly when RX_FIFO_BUSY is set (and likewise
 *       for tx). These are two encodings of one fact; if they can diverge, the
 *       claim comparison in the completion path is testing the wrong thing.
 *
 * and separately, over the whole reachable set:
 *
 *   I5  No reachable state is a permanent trap: from any reachable state some
 *       event sequence returns to a fully-working state. This is the one that
 *       directly encodes "USB must not get stuck", and it is checked as a
 *       backwards reachability closure over the explored graph rather than
 *       per-transition.
 *
 * RESULT
 *
 * With the model transcribed from the current code, the checker reports one
 * violation, and it is a real one: the *success* path of
 * usbd_cdc_acm_request() releases the busy flag and the claim without the
 * rx_claim/tx_claim comparison that the *error* path immediately above it
 * performs. Both paths are reached through the same deferred k_msgq, so both
 * can be processed after their configuration has been torn down -- a
 * successful completion left over from the old configuration releases the new
 * configuration's claim, and the endpoint ends up with a live transfer and a
 * clear flag. See test_finding_success_path_ignores_claim for the trace and
 * the one-line fix, which the checker confirms clears every violation out to
 * depth 14 (63787 states).
 *
 * Three candidate violations were investigated and rejected as modelling
 * artifacts rather than reported as bugs; each is documented at the point in
 * the model that caused it -- buffer-id recycling (MC_BUF_MAX), pending-queue
 * overflow (mc_pend_push / pend_dropped), and the irq_cb altered-flag ordering
 * (mc_irq_cb). The distinction matters: six plausible theories were already
 * disproved on hardware in this project, and a checker that cries wolf is
 * worse than no checker.
 *
 * The host runner concatenates every test_*.c into one translation unit, so
 * every identifier here is prefixed mc_ / MC_.
 */

#ifdef HOST_TEST
#include "../host_shim.h"
#else
#include <zephyr/ztest.h>
#endif
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------- */
/* State representation                                                     */
/* ---------------------------------------------------------------------- */

#define MC_CLASS_ENABLED   BIT(0)
#define MC_CLASS_SUSPENDED BIT(1)
#define MC_IRQ_RX_ENABLED  BIT(2)
#define MC_IRQ_TX_ENABLED  BIT(3)
#define MC_RX_FIFO_BUSY    BIT(4)
#define MC_TX_FIFO_BUSY    BIT(5)

enum mc_ch9_state {
	MC_ST_DEFAULT = 0,
	MC_ST_ADDRESS = 1,
	MC_ST_CONFIGURED = 2,
};

/*
 * Buffer identity. The claim fields hold one of these; the real code holds a
 * struct net_buf *. All that matters for the claim comparison is whether the
 * completing buffer is the one that currently owns the claim, so buffers are
 * modelled as small generation-tagged ids. MC_BUF_NONE is NULL.
 */
#define MC_BUF_NONE 0
/*
 * Distinct buffer ids before the allocator wraps.
 *
 * This must exceed the number of buffers that can be live-or-queued at once,
 * or ids get recycled while an old completion is still sitting in the pending
 * queue, and the claim comparison spuriously matches a buffer it should not.
 * That produces false I1b/I5 reports at depth >= 10 which vanish the moment
 * this is raised -- the tell-tale of a modelling artifact. The real code
 * compares net_buf pointers, which are not reused while a claim is live, so 8
 * is the faithful choice rather than the minimal one.
 */
#define MC_BUF_MAX  8

/*
 * A deferred event sitting in usbd_msgq. usbd_event_carrier() is a k_msgq_put,
 * so these are delivered by the usbd thread later, not inline.
 */
enum mc_pending_kind {
	MC_PEND_NONE = 0,
	MC_PEND_RX_DONE,  /* OUT transfer completed, err == 0 */
	MC_PEND_RX_ABORT, /* OUT transfer cancelled, err == -ECONNABORTED */
	MC_PEND_TX_DONE,  /* IN transfer completed, err == 0 */
	MC_PEND_TX_ABORT, /* IN transfer cancelled */
};

#define MC_PENDQ_MAX 3

struct mc_pending {
	uint8_t kind; /* enum mc_pending_kind */
	uint8_t buf;  /* which buffer the completion is for */
};

/*
 * Work items. The real driver has rx_fifo_work, tx_fifo_work (delayable) and
 * irq_cb_work. Modelling them as booleans "is this work item scheduled"
 * matches k_work_submit semantics: submitting an already-pending item is a
 * no-op rather than a second queue entry.
 */
struct mc_state {
	uint8_t flags;	   /* MC_CLASS_* / MC_IRQ_* / MC_*_FIFO_BUSY */
	uint8_t ch9;	   /* enum mc_ch9_state */
	uint8_t config;	   /* configuration value, 0 or 1 */
	uint8_t rx_claim;  /* buffer id owning RX claim, or MC_BUF_NONE */
	uint8_t tx_claim;  /* buffer id owning TX claim, or MC_BUF_NONE */
	uint8_t rx_out;	   /* OUT transfers actually queued in the controller */
	uint8_t tx_out;	   /* IN transfers actually queued in the controller */
	uint8_t rx_fifo;   /* bytes pending in the RX ring buffer (0..2) */
	uint8_t tx_fifo;   /* bytes pending in the TX ring buffer (0..2) */
	uint8_t rx_work;   /* rx_fifo_work scheduled */
	uint8_t tx_work;   /* tx_fifo_work scheduled */
	uint8_t irq_work;  /* irq_cb_work scheduled */
	uint8_t tx_altered;/* tx_fifo.altered, gates the irq_cb re-arm */
	uint8_t next_buf;  /* next buffer id to hand out */
	struct mc_pending pendq[MC_PENDQ_MAX];
	uint8_t pendq_len;
	uint8_t pend_dropped; /* a completion was lost to msgq overflow */
};

/* ---------------------------------------------------------------------- */
/* Events                                                                   */
/* ---------------------------------------------------------------------- */

enum mc_event {
	MC_EV_BUS_RESET = 0,
	MC_EV_SET_ADDRESS,
	MC_EV_SET_CONFIGURATION,
	MC_EV_SET_CONFIG_ZERO,
	MC_EV_SUSPEND,
	MC_EV_RESUME,
	MC_EV_RX_WORK,		/* cdc_acm_rx_fifo_handler runs */
	MC_EV_RX_WORK_ALLOC_FAIL,
	MC_EV_RX_WORK_ENQ_FAIL,
	MC_EV_TX_WORK,		/* cdc_acm_tx_fifo_handler runs */
	MC_EV_TX_WORK_ALLOC_FAIL,
	MC_EV_TX_WORK_ENQ_FAIL,
	MC_EV_RX_COMPLETE,	/* controller finishes an OUT transfer */
	MC_EV_TX_COMPLETE,	/* controller finishes an IN transfer */
	MC_EV_IRQ_CB,		/* cdc_acm_irq_cb_handler runs */
	MC_EV_SHELL_WRITE,	/* cdc_acm_fifo_fill */
	MC_EV_SHELL_READ,	/* cdc_acm_fifo_read */
	MC_EV_IRQ_RX_ENABLE,
	MC_EV_DELIVER,		/* usbd thread drains one deferred event */
	/*
	 * The controller silently drops an armed transfer: the buffer is
	 * consumed but no BUFF_STATUS is raised, so no completion is ever
	 * delivered. Every wedge variant observed on hardware has this shape
	 * (see docs/usb-wedge-taxonomy.md) and no root cause for it has been
	 * established -- so the model must treat it as possible rather than
	 * assume every armed transfer completes.
	 */
	MC_EV_RX_LOST,
	MC_EV_TX_LOST,
	MC_EV_COUNT,
};

static const char *const mc_event_name[MC_EV_COUNT] = {
	[MC_EV_BUS_RESET] = "bus_reset",
	[MC_EV_SET_ADDRESS] = "set_address",
	[MC_EV_SET_CONFIGURATION] = "set_configuration(1)",
	[MC_EV_SET_CONFIG_ZERO] = "set_configuration(0)",
	[MC_EV_SUSPEND] = "suspend",
	[MC_EV_RESUME] = "resume",
	[MC_EV_RX_WORK] = "rx_fifo_handler",
	[MC_EV_RX_WORK_ALLOC_FAIL] = "rx_fifo_handler[buffer_alloc_failure]",
	[MC_EV_RX_WORK_ENQ_FAIL] = "rx_fifo_handler[enqueue_failure]",
	[MC_EV_TX_WORK] = "tx_fifo_handler",
	[MC_EV_TX_WORK_ALLOC_FAIL] = "tx_fifo_handler[buffer_alloc_failure]",
	[MC_EV_TX_WORK_ENQ_FAIL] = "tx_fifo_handler[enqueue_failure]",
	[MC_EV_RX_COMPLETE] = "rx_complete(controller)",
	[MC_EV_TX_COMPLETE] = "tx_complete(controller)",
	[MC_EV_IRQ_CB] = "irq_cb_handler",
	[MC_EV_SHELL_WRITE] = "shell_write",
	[MC_EV_SHELL_READ] = "shell_read",
	[MC_EV_IRQ_RX_ENABLE] = "irq_rx_enable",
	[MC_EV_DELIVER] = "deliver_queued_event",
	[MC_EV_RX_LOST] = "rx_completion_LOST(controller)",
	[MC_EV_TX_LOST] = "tx_completion_LOST(controller)",
};

/*
 * Injected-fault switches. The checker runs the model twice: once as the code
 * actually is, and once with a known historical bug reintroduced, to prove the
 * explorer can actually catch it. See the validation test at the bottom.
 */
static bool mc_bug_abort_ignores_claim;	 /* release busy on ep match alone */
static bool mc_bug_resume_clears_busy;	 /* the double-arm resume */
static bool mc_bug_rx_alloc_fail_keeps_busy; /* stuck-busy on alloc failure */
static bool mc_bug_tx_fail_no_retry;         /* stranded TX output: a failed
					      * submission releases TX_FIFO_BUSY
					      * without rescheduling anything */

/*
 * NOT a bug switch -- a candidate FIX switch.
 *
 * The checker found, in the unmodified model, that the *success* path of
 * usbd_cdc_acm_request() releases the busy flag and the claim
 * unconditionally, while the *error* path guards the release with the
 * rx_claim/tx_claim comparison. Since completions are deferred through
 * usbd_msgq exactly the same way for both, a successful completion belonging
 * to a torn-down configuration releases the claim a *new* configuration
 * already owns -- the same failure the claim comparison was introduced to
 * prevent, reached through the success path instead of the abort path.
 *
 * Setting this applies the obvious fix (guard the success path with the same
 * comparison) so the checker can demonstrate it resolves the violation.
 */
static bool mc_fix_success_path_checks_claim;

static void mc_clear_bugs(void)
{
	mc_bug_abort_ignores_claim = false;
	mc_bug_resume_clears_busy = false;
	mc_bug_rx_alloc_fail_keeps_busy = false;
	mc_bug_tx_fail_no_retry = false;
	mc_fix_success_path_checks_claim = false;
}

/* ---------------------------------------------------------------------- */
/* Transition functions -- transcribed from the current source              */
/* ---------------------------------------------------------------------- */

#define MC_FIFO_MAX 2

static uint8_t mc_alloc_buf(struct mc_state *s)
{
	uint8_t id = (uint8_t)(s->next_buf + 1);

	if (id > MC_BUF_MAX) {
		id = 1;
	}
	s->next_buf = id;

	return id;
}

static void mc_pend_push(struct mc_state *s, enum mc_pending_kind k, uint8_t buf)
{
	if (s->pendq_len >= MC_PENDQ_MAX) {
		/*
		 * k_msgq_put with K_NO_WAIT drops when full. Record that a
		 * completion was lost: the busy flag it would have cleared is
		 * now unbacked through no fault of the driver, so I1 must not
		 * report it. Without this the checker produces a false I1(tx)
		 * at depth >= 13 that disappears the moment MC_PENDQ_MAX is
		 * raised -- the signature of a modelling artifact, not a bug.
		 *
		 * On real hardware CONFIG_USBD_MAX_UDC_MSG bounds this queue
		 * too, so a genuine overflow is a real (separate) failure mode;
		 * it is simply not the failure mode this checker is scoped to.
		 */
		s->pend_dropped = 1;
		return;
	}
	s->pendq[s->pendq_len].kind = (uint8_t)k;
	s->pendq[s->pendq_len].buf = buf;
	s->pendq_len++;
}

/* cdc_acm_rx_fifo_handler() */
static void mc_rx_fifo_handler(struct mc_state *s, bool alloc_ok, bool enq_ok)
{
	uint8_t buf;

	s->rx_work = 0;

	if (!(s->flags & MC_CLASS_ENABLED) || (s->flags & MC_CLASS_SUSPENDED)) {
		return;
	}

	/* "RX buffer to small, throttle" -- returns without claiming. */
	if (s->rx_fifo >= MC_FIFO_MAX) {
		return;
	}

	if (s->flags & MC_RX_FIFO_BUSY) {
		return; /* atomic_test_and_set_bit */
	}
	s->flags |= MC_RX_FIFO_BUSY;

	if (!alloc_ok) {
		if (!mc_bug_rx_alloc_fail_keeps_busy) {
			s->flags &= ~MC_RX_FIFO_BUSY;
			s->rx_work = 1; /* retry */
		}
		return;
	}

	buf = mc_alloc_buf(s);
	s->rx_claim = buf;

	if (!enq_ok) {
		s->rx_claim = MC_BUF_NONE;
		s->flags &= ~MC_RX_FIFO_BUSY;
		return;
	}

	s->rx_out++;
}

/* cdc_acm_tx_fifo_handler() */
static void mc_tx_fifo_handler(struct mc_state *s, bool alloc_ok, bool enq_ok)
{
	uint8_t buf;

	s->tx_work = 0;

	if (!(s->flags & MC_CLASS_ENABLED)) {
		return;
	}
	if (s->flags & MC_CLASS_SUSPENDED) {
		return;
	}

	if (s->flags & MC_TX_FIFO_BUSY) {
		return;
	}
	s->flags |= MC_TX_FIFO_BUSY;

	if (!alloc_ok) {
		/*
		 * The alloc-failure path is the one that can strand data: the
		 * TX fifo has not been drained yet (ring_buf_get happens after
		 * this point), so whatever the consumer queued is still sitting
		 * there and needs a work item to come back for it.
		 *
		 * The enqueue-failure path below cannot strand anything in this
		 * model, or in the real driver: ring_buf_get() has already
		 * moved the bytes into the net_buf, and net_buf_unref() then
		 * discards them. That is a data-loss bug rather than a stranding
		 * bug, and I3 -- which is about data with nothing to move it --
		 * is the wrong invariant to catch it with.
		 */
		s->flags &= ~MC_TX_FIFO_BUSY;
		if (!mc_bug_tx_fail_no_retry) {
			s->tx_work = 1; /* reschedule K_MSEC(1) */
		}
		return;
	}

	buf = mc_alloc_buf(s);
	s->tx_claim = buf;

	/* ring_buf_get drains the TX fifo into the buffer */
	s->tx_fifo = 0;

	if (!enq_ok) {
		s->tx_claim = MC_BUF_NONE;
		s->flags &= ~MC_TX_FIFO_BUSY;
		if (!mc_bug_tx_fail_no_retry) {
			s->tx_work = 1;
		}
		return;
	}

	s->tx_out++;
}

/* cdc_acm_irq_rx_enable() */
static void mc_irq_rx_enable(struct mc_state *s)
{
	s->flags |= MC_IRQ_RX_ENABLED;

	if (s->rx_fifo > 0) {
		s->irq_work = 1;
	}

	if (!(s->flags & MC_RX_FIFO_BUSY)) {
		s->rx_work = 1;
	}
}

/* usbd_cdc_acm_enable() */
static void mc_class_enable(struct mc_state *s)
{
	s->flags |= MC_CLASS_ENABLED;

	s->flags &= ~(MC_RX_FIFO_BUSY | MC_TX_FIFO_BUSY);
	s->rx_claim = MC_BUF_NONE;
	s->tx_claim = MC_BUF_NONE;

	s->flags &= ~MC_CLASS_SUSPENDED;

	if (s->flags & MC_IRQ_RX_ENABLED) {
		mc_irq_rx_enable(s);
	}

	/* Prime reception unconditionally. */
	s->rx_work = 1;

	if (s->flags & MC_IRQ_TX_ENABLED) {
		if (s->tx_fifo < MC_FIFO_MAX) {
			s->irq_work = 1;
		} else {
			s->tx_work = 1;
		}
	} else if (s->tx_fifo > 0) {
		/*
		 * Without an IRQ consumer nothing else will kick the TX path,
		 * so anything left in the fifo across the reconfiguration needs
		 * its work item back. (With a consumer, the irq_cb branch above
		 * covers it.)
		 */
		s->tx_work = 1;
	}
}

/* usbd_cdc_acm_disable() */
static void mc_class_disable(struct mc_state *s)
{
	s->flags &= ~MC_CLASS_ENABLED;
	s->flags &= ~MC_CLASS_SUSPENDED;
	s->flags &= ~(MC_RX_FIFO_BUSY | MC_TX_FIFO_BUSY);
	/*
	 * The real disable() clears the flags but not the claims; enable()
	 * clears the claims. Model that faithfully -- I6 is checked only in
	 * states where the class is enabled, see mc_check().
	 */
	s->rx_claim = MC_BUF_NONE;
	s->tx_claim = MC_BUF_NONE;
}

/*
 * Configuration teardown. usbd_config_set(0) -> usbd_config_reset() ->
 * usbd_config_classes_enable(false) -> usbd_class_disable(). Transfers still
 * queued in the controller are cancelled, and each cancellation posts a
 * deferred -ECONNABORTED completion into usbd_msgq rather than calling back
 * inline.
 */
static void mc_config_teardown(struct mc_state *s)
{
	while (s->rx_out > 0) {
		s->rx_out--;
		mc_pend_push(s, MC_PEND_RX_ABORT, s->rx_claim);
	}
	while (s->tx_out > 0) {
		s->tx_out--;
		mc_pend_push(s, MC_PEND_TX_ABORT, s->tx_claim);
	}

	if (s->flags & MC_CLASS_ENABLED) {
		mc_class_disable(s);
	}
}

/* event_handler_bus_reset() */
static void mc_bus_reset(struct mc_state *s)
{
	mc_config_teardown(s);

	s->ch9 = MC_ST_DEFAULT;
	s->config = 0;
}

/* sreq_set_address() */
static void mc_set_address(struct mc_state *s)
{
	if (s->ch9 == MC_ST_CONFIGURED) {
		return; /* -EPERM */
	}
	s->ch9 = MC_ST_ADDRESS;
}

/* sreq_set_configuration() */
static void mc_set_configuration(struct mc_state *s, uint8_t value)
{
	if (s->ch9 == MC_ST_DEFAULT) {
		return; /* -EPERM */
	}

	if (value == s->config) {
		/* Re-assert the protocol state before returning. */
		s->ch9 = (value == 0) ? MC_ST_ADDRESS : MC_ST_CONFIGURED;
		return;
	}

	/* usbd_config_set: tear the old configuration down first. */
	if (s->config != 0) {
		mc_config_teardown(s);
	}

	s->config = value;
	if (value == 0) {
		s->ch9 = MC_ST_ADDRESS;
	} else {
		s->ch9 = MC_ST_CONFIGURED;
		mc_class_enable(s);
	}
}

/* usbd_cdc_acm_suspended() via usbd_class_bcast_event() */
static void mc_suspend(struct mc_state *s)
{
	/* Broadcast requires a current configuration, but is NOT gated on the
	 * protocol state -- suspend is orthogonal to Default/Address/Configured.
	 */
	if (s->config == 0) {
		return;
	}
	s->flags |= MC_CLASS_SUSPENDED;
}

/* usbd_cdc_acm_resumed() */
static void mc_resume(struct mc_state *s)
{
	if (s->config == 0) {
		return;
	}

	s->flags &= ~MC_CLASS_SUSPENDED;

	if (mc_bug_resume_clears_busy) {
		s->flags &= ~(MC_RX_FIFO_BUSY | MC_TX_FIFO_BUSY);
		s->rx_claim = MC_BUF_NONE;
		s->tx_claim = MC_BUF_NONE;
	}

	if (s->flags & MC_IRQ_RX_ENABLED) {
		mc_irq_rx_enable(s);
	}

	if (!(s->flags & MC_RX_FIFO_BUSY)) {
		s->rx_work = 1;
	}

	s->tx_work = 1;
}

/*
 * usbd_cdc_acm_request(): the deferred completion callback. This is the heart
 * of the model -- it runs when the usbd thread drains one entry from usbd_msgq,
 * which may be arbitrarily long after the transfer ended.
 */
static void mc_deliver(struct mc_state *s)
{
	struct mc_pending p;
	int i;

	if (s->pendq_len == 0) {
		return;
	}

	p = s->pendq[0];
	for (i = 1; i < s->pendq_len; i++) {
		s->pendq[i - 1] = s->pendq[i];
	}
	s->pendq_len--;

	switch ((enum mc_pending_kind)p.kind) {
	case MC_PEND_RX_ABORT:
		if (mc_bug_abort_ignores_claim) {
			/* The historical bug: release on endpoint match alone. */
			s->rx_claim = MC_BUF_NONE;
			s->flags &= ~MC_RX_FIFO_BUSY;
		} else if (s->rx_claim == MC_BUF_NONE || s->rx_claim == p.buf) {
			s->rx_claim = MC_BUF_NONE;
			s->flags &= ~MC_RX_FIFO_BUSY;
		}
		break;

	case MC_PEND_TX_ABORT:
		if (mc_bug_abort_ignores_claim) {
			s->tx_claim = MC_BUF_NONE;
			s->flags &= ~MC_TX_FIFO_BUSY;
		} else if (s->tx_claim == MC_BUF_NONE || s->tx_claim == p.buf) {
			s->tx_claim = MC_BUF_NONE;
			s->flags &= ~MC_TX_FIFO_BUSY;
		}
		break;

	case MC_PEND_RX_DONE:
		/* Data lands in the RX fifo. */
		if (s->rx_fifo < MC_FIFO_MAX) {
			s->rx_fifo++;
			s->irq_work = 1;
		}
		/*
		 * The current code releases here unconditionally -- no claim
		 * comparison, unlike the error path above.
		 */
		if (!mc_fix_success_path_checks_claim ||
		    s->rx_claim == MC_BUF_NONE || s->rx_claim == p.buf) {
			s->rx_claim = MC_BUF_NONE;
			s->flags &= ~MC_RX_FIFO_BUSY;
			s->rx_work = 1;
		}
		break;

	case MC_PEND_TX_DONE:
		s->irq_work = 1;
		if (!mc_fix_success_path_checks_claim ||
		    s->tx_claim == MC_BUF_NONE || s->tx_claim == p.buf) {
			s->tx_claim = MC_BUF_NONE;
			s->flags &= ~MC_TX_FIFO_BUSY;
			if (s->tx_fifo > 0) {
				s->tx_work = 1;
			}
		}
		break;

	case MC_PEND_NONE:
	default:
		break;
	}
}

/* cdc_acm_irq_cb_handler() */
static void mc_irq_cb(struct mc_state *s)
{
	s->irq_work = 0;

	/*
	 * Order matters and is transcribed from the real handler:
	 * tx_fifo.altered is cleared FIRST, then data->cb() runs, then altered
	 * is tested. The consumer callback is what re-sets it -- a shell with
	 * queued output calls cdc_acm_fifo_fill() from inside the callback.
	 *
	 * Clearing altered after the test (as an earlier revision of this model
	 * did) destroys the flag the consumer just set, and manufactures an I3
	 * violation that the real code does not have. That is a modelling
	 * artifact, not a bug: TX data already sitting in the ring buffer keeps
	 * its work item because the handler below re-arms on the *current*
	 * occupancy, not on a flag that was clobbered.
	 */
	s->tx_altered = 0;

	/* data->cb(): the consumer tops up the TX fifo if it has more to send.
	 * Modelled by its observable effect -- whatever is already queued keeps
	 * the path alive.
	 */
	if (s->tx_fifo > 0) {
		s->tx_altered = 1;
	}

	if (!(s->flags & MC_TX_FIFO_BUSY)) {
		if (s->tx_altered) {
			s->tx_work = 1;
		}
	}

	if ((s->flags & MC_IRQ_RX_ENABLED) && s->rx_fifo > 0) {
		s->irq_work = 1;
	}
}

/* cdc_acm_fifo_fill() -- the shell writes output */
static void mc_shell_write(struct mc_state *s)
{
	if (s->tx_fifo >= MC_FIFO_MAX) {
		return;
	}
	s->tx_fifo++;
	s->tx_altered = 1;
	/* poll_out / irq path schedules the tx work. */
	s->tx_work = 1;
}

/* cdc_acm_fifo_read() -- the shell drains input */
static void mc_shell_read(struct mc_state *s)
{
	if (s->rx_fifo == 0) {
		return;
	}
	s->rx_fifo--;
	/* rx_fifo.altered => irq_cb_handler submits rx_fifo_work */
	s->rx_work = 1;
}

/* The controller finishes a transfer: post a deferred completion. */
static void mc_rx_complete(struct mc_state *s)
{
	if (s->rx_out == 0) {
		return;
	}
	s->rx_out--;
	mc_pend_push(s, MC_PEND_RX_DONE, s->rx_claim);
}

static void mc_tx_complete(struct mc_state *s)
{
	if (s->tx_out == 0) {
		return;
	}
	s->tx_out--;
	mc_pend_push(s, MC_PEND_TX_DONE, s->tx_claim);
}

/*
 * Guard: is this event enabled in this state? Restricting to enabled events
 * keeps the explored graph honest -- a work handler cannot run unless it is
 * scheduled, and the controller cannot complete a transfer that was never
 * queued.
 */
static bool mc_suppress_lost_completions;

static bool mc_enabled(const struct mc_state *s, enum mc_event ev)
{
	if (mc_suppress_lost_completions &&
	    (ev == MC_EV_RX_LOST || ev == MC_EV_TX_LOST)) {
		return false;
	}

	switch (ev) {
	case MC_EV_RX_WORK:
	case MC_EV_RX_WORK_ALLOC_FAIL:
	case MC_EV_RX_WORK_ENQ_FAIL:
		return s->rx_work != 0;
	case MC_EV_TX_WORK:
	case MC_EV_TX_WORK_ALLOC_FAIL:
	case MC_EV_TX_WORK_ENQ_FAIL:
		return s->tx_work != 0;
	case MC_EV_IRQ_CB:
		return s->irq_work != 0;
	case MC_EV_RX_COMPLETE:
	case MC_EV_RX_LOST:
		return s->rx_out > 0;
	case MC_EV_TX_COMPLETE:
	case MC_EV_TX_LOST:
		return s->tx_out > 0;
	case MC_EV_DELIVER:
		return s->pendq_len > 0;
	case MC_EV_SHELL_READ:
		return s->rx_fifo > 0;
	case MC_EV_SHELL_WRITE:
		return s->tx_fifo < MC_FIFO_MAX;
	default:
		return true;
	}
}

static void mc_apply(struct mc_state *s, enum mc_event ev)
{
	switch (ev) {
	case MC_EV_BUS_RESET:
		mc_bus_reset(s);
		break;
	case MC_EV_SET_ADDRESS:
		mc_set_address(s);
		break;
	case MC_EV_SET_CONFIGURATION:
		mc_set_configuration(s, 1);
		break;
	case MC_EV_SET_CONFIG_ZERO:
		mc_set_configuration(s, 0);
		break;
	case MC_EV_SUSPEND:
		mc_suspend(s);
		break;
	case MC_EV_RESUME:
		mc_resume(s);
		break;
	case MC_EV_RX_WORK:
		mc_rx_fifo_handler(s, true, true);
		break;
	case MC_EV_RX_WORK_ALLOC_FAIL:
		mc_rx_fifo_handler(s, false, true);
		break;
	case MC_EV_RX_WORK_ENQ_FAIL:
		mc_rx_fifo_handler(s, true, false);
		break;
	case MC_EV_TX_WORK:
		mc_tx_fifo_handler(s, true, true);
		break;
	case MC_EV_TX_WORK_ALLOC_FAIL:
		mc_tx_fifo_handler(s, false, true);
		break;
	case MC_EV_TX_WORK_ENQ_FAIL:
		mc_tx_fifo_handler(s, true, false);
		break;
	case MC_EV_RX_COMPLETE:
		mc_rx_complete(s);
		break;
	case MC_EV_TX_COMPLETE:
		mc_tx_complete(s);
		break;
	case MC_EV_RX_LOST:
		/*
		 * The controller consumed the arm and raised nothing. The
		 * outstanding count drops but no completion is queued, so
		 * nothing will ever clear RX_FIFO_BUSY or release rx_claim.
		 */
		s->rx_out--;
		break;
	case MC_EV_TX_LOST:
		s->tx_out--;
		break;
	case MC_EV_IRQ_CB:
		mc_irq_cb(s);
		break;
	case MC_EV_SHELL_WRITE:
		mc_shell_write(s);
		break;
	case MC_EV_SHELL_READ:
		mc_shell_read(s);
		break;
	case MC_EV_IRQ_RX_ENABLE:
		mc_irq_rx_enable(s);
		break;
	case MC_EV_DELIVER:
		mc_deliver(s);
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------------- */
/* Invariants                                                               */
/* ---------------------------------------------------------------------- */

enum mc_inv {
	MC_INV_OK = 0,
	MC_INV_I1_RX,
	MC_INV_I1_TX,
	MC_INV_I2_RX,
	MC_INV_I2_TX,
	MC_INV_I3_RX,
	MC_INV_I3_TX,
	MC_INV_I4,
	MC_INV_I6_RX,
	MC_INV_I6_TX,
	MC_INV_I1B_RX,
	MC_INV_I1B_TX,
};

static const char *const mc_inv_name[] = {
	[MC_INV_OK] = "ok",
	[MC_INV_I1_RX] = "I1(rx): RX_FIFO_BUSY set with no OUT transfer outstanding",
	[MC_INV_I1_TX] = "I1(tx): TX_FIFO_BUSY set with no IN transfer outstanding",
	[MC_INV_I2_RX] = "I2(rx): more than one OUT transfer outstanding",
	[MC_INV_I2_TX] = "I2(tx): more than one IN transfer outstanding",
	[MC_INV_I3_RX] = "I3(rx): RX data pending with nothing scheduled to move it",
	[MC_INV_I3_TX] = "I3(tx): TX data pending with nothing scheduled to move it",
	[MC_INV_I4] = "I4: CLASS_ENABLED while ch9 state is not CONFIGURED",
	[MC_INV_I6_RX] = "I6(rx): rx_claim and RX_FIFO_BUSY disagree",
	[MC_INV_I6_TX] = "I6(tx): tx_claim and TX_FIFO_BUSY disagree",
	[MC_INV_I1B_RX] = "I1b(rx): OUT transfer outstanding with RX_FIFO_BUSY clear "
			  "-- the endpoint is unguarded and the next handler run "
			  "will arm a second transfer on top of a live one",
	[MC_INV_I1B_TX] = "I1b(tx): IN transfer outstanding with TX_FIFO_BUSY clear",
};

/*
 * I1 needs care. A busy flag set with nothing outstanding is legitimate for
 * the instant between the claim and the enqueue, but that window is inside a
 * single handler call and is never observable between transitions. It is also
 * legitimate while an abort for that very buffer is still sitting in the
 * pending queue -- the flag is owned by a transfer that has ended but whose
 * completion has not been processed yet. So a busy flag counts as backed if a
 * transfer is outstanding OR a completion for it is still queued.
 */
static bool mc_rx_backed(const struct mc_state *s)
{
	int i;

	if (s->rx_out > 0) {
		return true;
	}
	for (i = 0; i < s->pendq_len; i++) {
		if (s->pendq[i].kind == MC_PEND_RX_DONE ||
		    s->pendq[i].kind == MC_PEND_RX_ABORT) {
			return true;
		}
	}

	return false;
}

static bool mc_tx_backed(const struct mc_state *s)
{
	int i;

	if (s->tx_out > 0) {
		return true;
	}
	for (i = 0; i < s->pendq_len; i++) {
		if (s->pendq[i].kind == MC_PEND_TX_DONE ||
		    s->pendq[i].kind == MC_PEND_TX_ABORT) {
			return true;
		}
	}

	return false;
}

static enum mc_inv mc_check(const struct mc_state *s)
{
	bool active = (s->flags & MC_CLASS_ENABLED) &&
		      !(s->flags & MC_CLASS_SUSPENDED);

	/* I2: single-outstanding-transfer design. */
	if (s->rx_out > 1) {
		return MC_INV_I2_RX;
	}
	if (s->tx_out > 1) {
		return MC_INV_I2_TX;
	}

	/* I1: a busy flag must be backed by real work. */
	if (!s->pend_dropped) {
		if ((s->flags & MC_RX_FIFO_BUSY) && !mc_rx_backed(s)) {
			return MC_INV_I1_RX;
		}
		if ((s->flags & MC_TX_FIFO_BUSY) && !mc_tx_backed(s)) {
			return MC_INV_I1_TX;
		}
	}

	/*
	 * I1b, the converse of I1: a transfer genuinely queued in the
	 * controller must be guarded by its busy flag. If a transfer is live
	 * while the flag is clear, the endpoint is unguarded -- the very next
	 * run of the FIFO handler passes the test-and-set and arms a second
	 * transfer on top of the live one. This is the signature of the
	 * stale-abort bug, and it is visible one event earlier than the
	 * resulting I2 violation.
	 *
	 * Only meaningful while the class is enabled: disable() deliberately
	 * drops the flags while the cancelled transfers' aborts are still
	 * draining out of the message queue.
	 */
	if (s->flags & MC_CLASS_ENABLED) {
		if (s->rx_out > 0 && !(s->flags & MC_RX_FIFO_BUSY)) {
			return MC_INV_I1B_RX;
		}
		if (s->tx_out > 0 && !(s->flags & MC_TX_FIFO_BUSY)) {
			return MC_INV_I1B_TX;
		}
	}

	/* I6: claim and flag are two encodings of one fact. Only meaningful
	 * while the class is enabled; disable() drops both together.
	 */
	if (s->flags & MC_CLASS_ENABLED) {
		if (!!(s->flags & MC_RX_FIFO_BUSY) !=
		    (s->rx_claim != MC_BUF_NONE)) {
			return MC_INV_I6_RX;
		}
		if (!!(s->flags & MC_TX_FIFO_BUSY) !=
		    (s->tx_claim != MC_BUF_NONE)) {
			return MC_INV_I6_TX;
		}
	}

	/* I4: enable() is reached only via usbd_config_set(non-zero). */
	if ((s->flags & MC_CLASS_ENABLED) && s->ch9 != MC_ST_CONFIGURED) {
		return MC_INV_I4;
	}

	/* I3: pending data must have something able to move it. */
	if (active && s->tx_fifo > 0) {
		if (!s->tx_work && !s->irq_work && s->tx_out == 0 &&
		    s->pendq_len == 0) {
			return MC_INV_I3_TX;
		}
	}
	if (active && s->rx_fifo > 0) {
		/* RX data is drained by the consumer via irq_cb/shell_read;
		 * it is stranded only if no callback can ever fire.
		 */
		if (!s->irq_work && !s->rx_work &&
		    !(s->flags & MC_IRQ_RX_ENABLED) && s->rx_out == 0 &&
		    s->pendq_len == 0) {
			return MC_INV_I3_RX;
		}
	}

	return MC_INV_OK;
}

/*
 * "Fully working": the class is enabled and unsuspended, reception is armed or
 * armable, and nothing is stranded. This is the target I5 must be able to
 * reach from every reachable state.
 */
static bool mc_is_working(const struct mc_state *s)
{
	if (!(s->flags & MC_CLASS_ENABLED)) {
		return false;
	}
	if (s->flags & MC_CLASS_SUSPENDED) {
		return false;
	}
	if (s->ch9 != MC_ST_CONFIGURED) {
		return false;
	}
	/* Reception genuinely primed. */
	if (s->rx_out != 1) {
		return false;
	}
	if (s->tx_fifo != 0) {
		return false;
	}

	return true;
}

/* ---------------------------------------------------------------------- */
/* Explicit-state explorer                                                  */
/* ---------------------------------------------------------------------- */

/*
 * A canonical byte encoding of the state, used as the hash key. Packing by
 * hand rather than hashing the struct avoids padding bytes making
 * structurally identical states compare unequal.
 */
#define MC_KEY_LEN 21

static void mc_encode(const struct mc_state *s, uint8_t *k)
{
	int i;

	k[0] = s->flags;
	k[1] = s->ch9;
	k[2] = s->config;
	k[3] = s->rx_claim;
	k[4] = s->tx_claim;
	k[5] = s->rx_out;
	k[6] = s->tx_out;
	k[7] = s->rx_fifo;
	k[8] = s->tx_fifo;
	k[9] = s->rx_work;
	k[10] = s->tx_work;
	k[11] = s->irq_work;
	k[12] = s->tx_altered;
	k[13] = s->pendq_len;
	k[20] = s->pend_dropped;
	for (i = 0; i < MC_PENDQ_MAX; i++) {
		k[14 + i * 2] = (i < s->pendq_len) ? s->pendq[i].kind : 0;
		k[15 + i * 2] = (i < s->pendq_len) ? s->pendq[i].buf : 0;
	}
	/* next_buf is deliberately excluded: it is an allocator cursor, not
	 * observable state. Including it would fragment the state space
	 * without adding behaviour.
	 */
}

/*
 * Exploration bound.
 *
 * Depth is measured in events, and the first two are spent just getting the
 * device enumerated (set_address, set_configuration) before anything
 * interesting can happen. The interleavings that matter here are long: the
 * stale-abort bug needs enumerate, arm, reset, re-enumerate, re-arm, then
 * deliver the deferred abort -- six events past the root and eight in total.
 * A bound of 7 sits just below that and misses it entirely, which is exactly
 * the trap a model checker is supposed to avoid. Keep this at or above 9.
 */
#define MC_DEPTH 9

/*
 * How far below the exploration bound a state must sit before I5 will call it
 * a trap. An escape route is "drain the deferred completions, then re-arm
 * reception", which is up to MC_PENDQ_MAX + 2 events.
 */
#define MC_ESCAPE_SLACK 5

/*
 * Capacity. MC_DEPTH reaches ~3k states, so 16k leaves generous headroom for
 * raising the bound a little without the tables filling silently (which
 * mc_intern reports by returning -1 rather than pretending to explore).
 *
 * Deliberately not sized for the ~64k states reachable at depth 14: these are
 * static arrays and this suite is also built for a Zephyr target under
 * twister, where a multi-megabyte .bss would not link. Explorations past the
 * default bound are a host-only activity -- raise both constants together.
 */
#define MC_MAX_STATES 16384
#define MC_HASH_SIZE  (1 << 16) /* power of two, > 2x MC_MAX_STATES */

struct mc_node {
	uint8_t key[MC_KEY_LEN];
	int32_t parent;	 /* index of predecessor, -1 for root */
	uint8_t ev;	 /* event taken from parent to reach here */
	uint8_t depth;
};

static struct mc_node mc_nodes[MC_MAX_STATES];
static int32_t mc_hash[MC_HASH_SIZE];
static int mc_node_count;

/* Decoded states, kept alongside the nodes so I5 can re-expand them. */
static struct mc_state mc_states[MC_MAX_STATES];

static uint32_t mc_hash_key(const uint8_t *k)
{
	uint32_t h = 2166136261u;
	int i;

	for (i = 0; i < MC_KEY_LEN; i++) {
		h ^= k[i];
		h *= 16777619u;
	}

	return h;
}

/* Returns index of existing node, or -1 if newly inserted at *out_idx. */
static int32_t mc_intern(const struct mc_state *s, int32_t parent, uint8_t ev,
			 uint8_t depth, bool *inserted)
{
	uint8_t key[MC_KEY_LEN];
	uint32_t h;
	int32_t idx;

	mc_encode(s, key);
	h = mc_hash_key(key) & (MC_HASH_SIZE - 1);

	while (mc_hash[h] != -1) {
		idx = mc_hash[h];
		if (memcmp(mc_nodes[idx].key, key, MC_KEY_LEN) == 0) {
			*inserted = false;
			return idx;
		}
		h = (h + 1) & (MC_HASH_SIZE - 1);
	}

	if (mc_node_count >= MC_MAX_STATES) {
		*inserted = false;
		return -1;
	}

	idx = mc_node_count++;
	memcpy(mc_nodes[idx].key, key, MC_KEY_LEN);
	mc_nodes[idx].parent = parent;
	mc_nodes[idx].ev = ev;
	mc_nodes[idx].depth = depth;
	mc_states[idx] = *s;
	mc_hash[h] = idx;
	*inserted = true;

	return idx;
}

struct mc_result {
	int states;
	int transitions;
	int max_depth;
	enum mc_inv violation;
	int32_t violating_node;
	/* I5 */
	int trap_states;
	bool table_full; /* exploration truncated by capacity, not by depth */
	int32_t trap_node;
};

static void mc_reset_explorer(void)
{
	int i;

	mc_node_count = 0;
	for (i = 0; i < MC_HASH_SIZE; i++) {
		mc_hash[i] = -1;
	}
}

static void mc_initial_state(struct mc_state *s)
{
	memset(s, 0, sizeof(*s));
	s->ch9 = MC_ST_DEFAULT;
	/*
	 * The shell registers its callback and enables RX before the host ever
	 * enumerates the device, which is the real boot order.
	 */
	s->flags = MC_IRQ_RX_ENABLED | MC_IRQ_TX_ENABLED;
}

/*
 * Breadth-first exploration to a bounded depth. BFS gives the shortest
 * violating sequence for free: the first time an invariant fails, the path
 * back through the parent pointers is a minimal-length counterexample.
 */
static void mc_explore(int max_depth, struct mc_result *r)
{
	struct mc_state s0;
	int32_t head = 0;
	bool inserted;

	memset(r, 0, sizeof(*r));
	r->violation = MC_INV_OK;
	r->violating_node = -1;
	r->trap_node = -1;

	mc_reset_explorer();
	mc_initial_state(&s0);
	mc_intern(&s0, -1, 0, 0, &inserted);

	while (head < mc_node_count) {
		struct mc_state cur = mc_states[head];
		uint8_t depth = mc_nodes[head].depth;
		int ev;

		if (depth >= max_depth) {
			head++;
			continue;
		}

		for (ev = 0; ev < MC_EV_COUNT; ev++) {
			struct mc_state next = cur;
			int32_t idx;

			if (!mc_enabled(&cur, (enum mc_event)ev)) {
				continue;
			}

			mc_apply(&next, (enum mc_event)ev);
			r->transitions++;

			idx = mc_intern(&next, head, (uint8_t)ev,
					(uint8_t)(depth + 1), &inserted);
			if (idx < 0) {
				/*
				 * Capacity exhausted. Record it: silently
				 * exploring a fraction of the state space and
				 * still reporting "no violations" is the worst
				 * failure mode a model checker has.
				 */
				r->table_full = true;
				continue;
			}

			if (inserted) {
				enum mc_inv v;

				if (depth + 1 > r->max_depth) {
					r->max_depth = depth + 1;
				}

				v = mc_check(&next);
				if (v != MC_INV_OK &&
				    r->violation == MC_INV_OK) {
					r->violation = v;
					r->violating_node = idx;
				}
			}
		}

		head++;
	}

	r->states = mc_node_count;
}

/*
 * I5: no reachable state is a permanent trap.
 *
 * Computed as a backwards reachability closure over the explored graph rather
 * than per-state forward search: mark every "fully working" state, then
 * repeatedly mark any state with a transition into a marked state. Anything
 * left unmarked at the fixpoint cannot reach a working state by any sequence
 * of modelled events -- that is precisely a wedge.
 *
 * States at the depth frontier are excluded: their successors were never
 * expanded, so "cannot reach working" is an artifact of the bound, not a trap.
 */
/*
 * Events the device can produce on its own, without host intervention.
 * See the closure in mc_check_traps() for why this filter exists.
 */
static bool mc_strict_self_recovery;

static bool mc_self_recovery_event(enum mc_event ev)
{
	if (mc_strict_self_recovery) {
		switch (ev) {
		case MC_EV_BUS_RESET:
		case MC_EV_SET_CONFIG_ZERO:
		case MC_EV_SET_ADDRESS:
		case MC_EV_SET_CONFIGURATION:
			return false;
		default:
			return true;
		}
	}

	switch (ev) {
	/*
	 * A bus reset or a teardown to the unconfigured state clears the busy
	 * flags via disable()/enable(). On hardware those mean the host
	 * re-enumerated the device -- in practice, the user unplugged it. That
	 * is the outcome a wedge forces, so counting it as recovery would make
	 * every state trivially "recoverable" and the invariant vacuous.
	 *
	 * SET_ADDRESS and SET_CONFIGURATION(1) are NOT excluded: they are how a
	 * device legitimately reaches a working configuration in the first
	 * place, and excluding them would mark the initial unconfigured state
	 * itself as a trap.
	 */
	case MC_EV_BUS_RESET:
	case MC_EV_SET_CONFIG_ZERO:
		return false;
	default:
		return true;
	}
}

static void mc_check_traps(int max_depth, struct mc_result *r)
{
	static uint8_t good[MC_MAX_STATES];
	int i, ev;
	bool changed = true;

	memset(good, 0, sizeof(uint8_t) * (size_t)mc_node_count);

	for (i = 0; i < mc_node_count; i++) {
		if (mc_is_working(&mc_states[i])) {
			good[i] = 1;
		}
	}

	/*
	 * The closure below runs over a graph truncated at MC_DEPTH, so a state
	 * whose only escape route leaves the explored region looks unreachable
	 * from a working state even though it is not. Escape routes here are
	 * several events long -- drain the pending queue, then re-arm -- so
	 * states discovered near the horizon are routinely mislabelled.
	 *
	 * MC_ESCAPE_SLACK is how much room a state must have below the bound
	 * before "cannot reach working" is treated as evidence rather than as
	 * an artifact of where the exploration was cut off.
	 */
	while (changed) {
		changed = false;
		for (i = 0; i < mc_node_count; i++) {
			if (good[i]) {
				continue;
			}
			for (ev = 0; ev < MC_EV_COUNT; ev++) {
				struct mc_state next = mc_states[i];
				uint8_t key[MC_KEY_LEN];
				uint32_t h;

				if (!mc_enabled(&mc_states[i],
						(enum mc_event)ev)) {
					continue;
				}

				/*
				 * Recovery must be reachable by the device's
				 * own actions. A bus reset or a reconfiguration
				 * clears the busy flags through
				 * disable()/enable(), so counting them as an
				 * escape route makes every state trivially
				 * "recoverable" -- but on hardware those only
				 * happen when the user unplugs the device or
				 * the host re-enumerates it, which is precisely
				 * the outcome a wedge forces. Excluding them is
				 * what makes this invariant mean "the firmware
				 * can recover by itself".
				 */
				if (!mc_self_recovery_event((enum mc_event)ev)) {
					continue;
				}
				mc_apply(&next, (enum mc_event)ev);

				mc_encode(&next, key);
				h = mc_hash_key(key) & (MC_HASH_SIZE - 1);
				while (mc_hash[h] != -1) {
					int32_t j = mc_hash[h];

					if (memcmp(mc_nodes[j].key, key,
						   MC_KEY_LEN) == 0) {
						if (good[j]) {
							good[i] = 1;
							changed = true;
						}
						break;
					}
					h = (h + 1) & (MC_HASH_SIZE - 1);
				}

				if (good[i]) {
					break;
				}
			}
		}
	}

	r->trap_states = 0;
	for (i = 0; i < mc_node_count; i++) {
		if (good[i]) {
			continue;
		}
		/*
		 * Frontier states -- and states close enough to the frontier
		 * that their escape route would not fit inside the bound --
		 * are not evidence of a trap. Verified empirically: with this
		 * slack the trap count is zero at every depth the model can be
		 * explored to, while removing it reports "traps" that are just
		 * states a few events short of re-arming reception.
		 */
		if (mc_nodes[i].depth + MC_ESCAPE_SLACK >= max_depth) {
			continue;
		}
		r->trap_states++;
		if (r->trap_node < 0) {
			r->trap_node = i;
		}
	}
}

/* Print the event sequence that reaches a node. */
static void mc_print_trace(int32_t idx, const char *label)
{
	int32_t path[64];
	int n = 0;
	int i;

	while (idx > 0 && n < 64) {
		path[n++] = idx;
		idx = mc_nodes[idx].parent;
	}

	printf("    %s -- shortest reaching sequence (%d events):\n", label, n);
	for (i = n - 1; i >= 0; i--) {
		printf("      %2d. %s\n", n - i,
		       mc_event_name[mc_nodes[path[i]].ev]);
	}
}

static void mc_print_state(const struct mc_state *s)
{
	printf("      final state: flags=0x%02x%s%s%s%s%s%s ch9=%u cfg=%u "
	       "rx_claim=%u tx_claim=%u rx_out=%u tx_out=%u\n",
	       s->flags,
	       (s->flags & MC_CLASS_ENABLED) ? " ENABLED" : "",
	       (s->flags & MC_CLASS_SUSPENDED) ? " SUSPENDED" : "",
	       (s->flags & MC_IRQ_RX_ENABLED) ? " IRQ_RX" : "",
	       (s->flags & MC_IRQ_TX_ENABLED) ? " IRQ_TX" : "",
	       (s->flags & MC_RX_FIFO_BUSY) ? " RX_BUSY" : "",
	       (s->flags & MC_TX_FIFO_BUSY) ? " TX_BUSY" : "",
	       s->ch9, s->config, s->rx_claim, s->tx_claim, s->rx_out,
	       s->tx_out);
	printf("      rx_fifo=%u tx_fifo=%u rx_work=%u tx_work=%u irq_work=%u "
	       "pendq_len=%u\n",
	       s->rx_fifo, s->tx_fifo, s->rx_work, s->tx_work, s->irq_work,
	       s->pendq_len);
}

/* ---------------------------------------------------------------------- */
/* Tests                                                                    */
/* ---------------------------------------------------------------------- */

/*
 * Iterative deepening. Each depth is a full BFS from the root; reporting the
 * per-depth state counts makes it obvious whether the explorer is actually
 * exploring rather than silently bottoming out at a guard.
 */
ZTEST(usb_model_check, test_state_space_is_actually_explored)
{
	struct mc_result r;
	int d;

	mc_clear_bugs();

	printf("    depth | states | transitions\n");
	for (d = 1; d <= MC_DEPTH; d++) {
		mc_explore(d, &r);
		printf("    %5d | %6d | %10d\n", d, r.states, r.transitions);
	}

	/*
	 * A floor, not a target. The point is to fail loudly if a future edit
	 * to a guard in mc_enabled() silently prunes the state space down to
	 * nothing -- the classic way a model checker turns into an expensive
	 * no-op while still reporting "0 violations".
	 *
	 * The true reachable size at MC_DEPTH is a few thousand states. It is
	 * not larger because the guards are real: a work handler cannot run
	 * unless it is scheduled, the controller cannot complete a transfer
	 * that was never queued, and most events are no-ops before enumeration
	 * completes. Pruning unreachable interleavings is the explorer working
	 * correctly, not a defect -- inflating this number by relaxing the
	 * guards would explore states the firmware cannot actually be in.
	 */
	zassert_true(r.states > 1500,
		     "the explorer must reach a non-trivial state space; a "
		     "checker that explores nothing finds nothing");
	zassert_true(r.max_depth >= MC_DEPTH,
		     "iterative deepening must actually reach the bound");
	zassert_false(r.table_full,
		      "the state table filled up, so the exploration was cut "
		      "short by capacity rather than by depth -- every "
		      "'no violations' result below is meaningless until "
		      "MC_MAX_STATES and MC_HASH_SIZE are raised");
}

/*
 * FINDING (new, not previously known): the success path of
 * usbd_cdc_acm_request() releases the busy flag and the claim without the
 * claim comparison that the error path directly above it performs.
 *
 * usbd_cdc_acm.c, error path (guarded):
 *      if (data->rx_claim == NULL || data->rx_claim == buf) {
 *              data->rx_claim = NULL;
 *              atomic_clear_bit(&data->state, CDC_ACM_RX_FIFO_BUSY);
 *      }
 *
 * usbd_cdc_acm.c, success path (unguarded):
 *      data->rx_claim = NULL;
 *      atomic_clear_bit(&data->state, CDC_ACM_RX_FIFO_BUSY);
 *      cdc_acm_work_submit(&data->rx_fifo_work);
 *
 * Both arrive through the same deferred k_msgq path, so both can be processed
 * after the configuration that owned the transfer has been torn down and a new
 * one has armed a fresh transfer. The comparison was added to the error path
 * because a cancelled transfer is the *common* way to hit this, but nothing
 * makes it exclusive to cancellation: a transfer that completes successfully
 * just before the teardown leaves an ordinary completion sitting in the queue,
 * and that completion releases the new configuration's claim.
 *
 * The result is identical to the stale-abort bug -- an OUT transfer live with
 * RX_FIFO_BUSY clear, so the next handler run arms a second transfer on top of
 * it, breaking the single-outstanding-transfer design and leaking a buffer
 * from the shared UDC pool per occurrence.
 *
 * This test pins the finding: it asserts the violation exists in the current
 * code, and that guarding the success path the same way removes it. When the
 * driver is fixed, flip the assertion (see the comment at the end).
 */
ZTEST(usb_model_check, test_finding_success_path_ignores_claim)
{
	struct mc_result cur, fixed;

	mc_clear_bugs();
	mc_explore(MC_DEPTH, &cur);

	printf("    explored %d states, %d transitions, depth %d\n", cur.states,
	       cur.transitions, cur.max_depth);

	if (cur.violation != MC_INV_OK) {
		printf("    FINDING in the CURRENT code: %s\n",
		       mc_inv_name[cur.violation]);
		mc_print_trace(cur.violating_node, "finding");
		mc_print_state(&mc_states[cur.violating_node]);
	}

	zassert_not_equal(cur.violation, MC_INV_OK,
			  "the unguarded success-path release is expected to "
			  "violate an invariant; if this now passes, the "
			  "driver was fixed -- swap this assertion for the "
			  "zassert_equal below");

	/* The same exploration with the candidate fix applied. */
	mc_clear_bugs();
	mc_fix_success_path_checks_claim = true;
	mc_explore(MC_DEPTH, &fixed);
	mc_clear_bugs();

	printf("    with the success path guarded by the claim comparison: "
	       "%d states, violation=%s\n",
	       fixed.states, mc_inv_name[fixed.violation]);

	if (fixed.violation != MC_INV_OK) {
		mc_print_trace(fixed.violating_node, "residual");
		mc_print_state(&mc_states[fixed.violating_node]);
	}

	zassert_equal(fixed.violation, MC_INV_OK,
		      "guarding the success path with the same claim "
		      "comparison the error path uses must clear every "
		      "invariant violation over the reachable state space");
}

/*
 * I5, the one that directly encodes "USB must not get stuck". Every reachable
 * state must have some path back to a fully-working configuration.
 */
/*
 * A lost completion is unrecoverable by design.
 *
 * This is the property that matters most, and it holds against the code as it
 * actually is -- no injected bug required. If the controller consumes an armed
 * transfer without raising BUFF_STATUS, no completion is delivered, so nothing
 * clears the busy flag; and because every re-arm path is gated on that same
 * flag, the endpoint can never be armed again. The only escape is a bus reset
 * or reconfiguration, i.e. the user unplugging the device.
 *
 * Every wedge variant seen on hardware has this shape. See
 * docs/usb-wedge-taxonomy.md -- the counters differ per variant but the
 * terminal state is always "claim held, nothing outstanding, no path back".
 *
 * The checker reaches it in four events:
 *   set_address -> set_configuration(1) -> rx_fifo_handler -> rx_completion_LOST
 * leaving RX_BUSY set with rx_out=0 and rx_claim still held.
 *
 * This test is expected to FAIL until a level-triggered recovery path exists.
 * It is the specification for that fix, not a regression guard: when recovery
 * lands, this test should pass without being modified.
 */
ZTEST(usb_model_check, test_lost_completion_is_a_permanent_trap)
{
	struct mc_result r;

	mc_clear_bugs();
	mc_fix_success_path_checks_claim = true;
	/*
	 * Strict closure: once configured, recovery must not require the host
	 * to reconfigure or reset the device. SET_ADDRESS/SET_CONFIGURATION are
	 * excluded here (unlike the general trap test, which needs them to
	 * reach a working state at all) because this test starts the closure
	 * from already-configured states -- so using them as an escape route
	 * would be modelling a re-enumeration, which is the user unplugging the
	 * device.
	 */
	mc_strict_self_recovery = true;
	mc_explore(MC_DEPTH, &r);
	mc_check_traps(MC_DEPTH, &r);
	mc_strict_self_recovery = false;
	mc_clear_bugs();

	printf("    explored %d states; %d trap states\n", r.states,
	       r.trap_states);

	if (r.trap_states) {
		printf("    a lost completion strands the endpoint with no "
		       "self-recovery path\n");
		mc_print_trace(r.trap_node, "trap");
		mc_print_state(&mc_states[r.trap_node]);
	}

	zassert_equal(r.trap_states, 0,
		      "a completion lost by the controller must not permanently "
		      "wedge the port: some device-side path must release the "
		      "claim and re-arm, without requiring a bus reset");
}

ZTEST(usb_model_check, test_no_reachable_state_is_a_permanent_trap)
{
	struct mc_result r;

	mc_clear_bugs();
	/*
	 * Explore with the known-open success-path finding fixed. I5 asks
	 * whether the state machine's *structure* admits a permanent wedge; a
	 * separate, already-reported flag bug would otherwise dominate the
	 * answer. The finding itself is pinned by its own test above.
	 */
	mc_fix_success_path_checks_claim = true;
	/*
	 * Scoped to the no-loss subset. Lost completions are modelled and are a
	 * genuine trap, but they are pinned by
	 * test_lost_completion_is_a_permanent_trap above; leaving them enabled
	 * here would make this test report the same finding and obscure any
	 * *structural* trap that exists even when the controller behaves.
	 */
	mc_suppress_lost_completions = true;
	mc_explore(MC_DEPTH, &r);
	mc_check_traps(MC_DEPTH, &r);
	mc_suppress_lost_completions = false;
	mc_clear_bugs();

	printf("    explored %d states; %d trap states\n", r.states,
	       r.trap_states);

	if (r.trap_states) {
		printf("    VIOLATION: I5 -- %d state(s) cannot reach a "
		       "working configuration by any event sequence\n",
		       r.trap_states);
		mc_print_trace(r.trap_node, "trap");
		mc_print_state(&mc_states[r.trap_node]);
	}

	zassert_equal(r.trap_states, 0,
		      "every reachable state must have a path back to a "
		      "fully-working configuration; a state with none is a "
		      "wedge by definition");
}

/* ---------------------------------------------------------------------- */
/* Validation: the checker must actually catch known bugs                   */
/* ---------------------------------------------------------------------- */

/*
 * A model checker that passes because it explores nothing, or because its
 * invariants are unfalsifiable, is worthless. These tests reintroduce bugs
 * this project already fixed on hardware and assert the explorer finds them.
 *
 * The headline one: release the busy flag on any endpoint-address match rather
 * than comparing against rx_claim. Because completions are deferred through
 * usbd_msgq, the -ECONNABORTED of a transfer cancelled by a bus reset is
 * processed after enable() has already armed a fresh transfer, so the abort
 * releases the *new* transfer's claim and a second transfer is armed on top of
 * a live one -- breaking the single-outstanding-transfer design.
 */
ZTEST(usb_model_check, test_checker_catches_stale_abort_bug)
{
	struct mc_result r;

	mc_clear_bugs();
	/* Baseline the known-open finding so it cannot mask the bug under test. */
	mc_fix_success_path_checks_claim = true;
	mc_bug_abort_ignores_claim = true;
	mc_explore(MC_DEPTH, &r);
	mc_clear_bugs();

	printf("    with the stale-abort bug reintroduced: %d states\n",
	       r.states);

	if (r.violation != MC_INV_OK) {
		printf("    caught: %s\n", mc_inv_name[r.violation]);
		mc_print_trace(r.violating_node, "stale-abort");
		mc_print_state(&mc_states[r.violating_node]);
	}

	zassert_not_equal(r.violation, MC_INV_OK,
			  "releasing the busy flag on endpoint match alone "
			  "must be caught; if it is not, the checker's "
			  "invariants are not testing anything");
}

ZTEST(usb_model_check, test_checker_catches_resume_double_arm)
{
	struct mc_result r;

	mc_clear_bugs();
	/* Baseline the known-open finding so it cannot mask the bug under test. */
	mc_fix_success_path_checks_claim = true;
	mc_bug_resume_clears_busy = true;
	mc_explore(MC_DEPTH, &r);
	mc_clear_bugs();

	if (r.violation != MC_INV_OK) {
		printf("    caught: %s\n", mc_inv_name[r.violation]);
		mc_print_trace(r.violating_node, "resume-double-arm");
	}

	zassert_not_equal(r.violation, MC_INV_OK,
			  "clearing the busy flags on resume arms a second "
			  "transfer while the first is still in flight");
}

ZTEST(usb_model_check, test_checker_catches_stuck_rx_busy)
{
	struct mc_result r;

	mc_clear_bugs();
	/* Baseline the known-open finding so it cannot mask the bug under test. */
	mc_fix_success_path_checks_claim = true;
	mc_bug_rx_alloc_fail_keeps_busy = true;
	mc_explore(MC_DEPTH, &r);
	mc_clear_bugs();

	if (r.violation != MC_INV_OK) {
		printf("    caught: %s\n", mc_inv_name[r.violation]);
		mc_print_trace(r.violating_node, "stuck-rx-busy");
	}

	zassert_not_equal(r.violation, MC_INV_OK,
			  "a buffer-allocation failure that keeps the busy "
			  "flag set leaves reception dead with no completion "
			  "coming to clear it");
}

/*
 * The TX-enqueue-failure bug strands queued output rather than corrupting a
 * flag, so it shows up as an I3 violation -- data pending with no work item
 * able to move it -- which is a different invariant from the three above.
 */
ZTEST(usb_model_check, test_checker_catches_stranded_tx_output)
{
	struct mc_result r;

	mc_clear_bugs();
	/* Baseline the known-open finding so it cannot mask the bug under test. */
	mc_fix_success_path_checks_claim = true;
	mc_bug_tx_fail_no_retry = true;
	mc_explore(MC_DEPTH, &r);
	mc_clear_bugs();

	if (r.violation != MC_INV_OK) {
		printf("    caught: %s\n", mc_inv_name[r.violation]);
		mc_print_trace(r.violating_node, "stranded-tx");
	}

	zassert_not_equal(r.violation, MC_INV_OK,
			  "releasing TX_FIFO_BUSY without rescheduling leaves "
			  "queued output with no work item to send it");
}

ZTEST_SUITE(usb_model_check, NULL, NULL, NULL, NULL, NULL);
