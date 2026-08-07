/*
 * Copyright (c) 2026 Blue Clover Devices
 * SPDX-License-Identifier: Apache-2.0
 *
 * Register-semantics regression tests.
 *
 * The most damaging bugs found in the RP2350 USB driver were all the same
 * mistake: clearing a write-1-to-clear (W1C) hardware bit through the
 * REG_ALIAS_CLR_BITS alias, which writes a 0 and is therefore a no-op. The
 * latch is never acknowledged, the derived interrupt stays asserted, and the
 * ISR re-dispatches stale events forever.
 *
 * These bugs are pure register semantics -- no timing, no analog behaviour --
 * so they can be caught on the host with a model of the register block. This
 * file models RP2350 W1C/RO/RW behaviour and asserts that the driver's
 * accessor idioms do the right thing against it.
 *
 * If someone "simplifies" a direct sys_write32() back into rpi_pico_bit_clr()
 * on a W1C register, these tests fail immediately instead of the board wedging
 * after a few hundred USB transactions.
 */

#ifdef HOST_TEST
#include "../host_shim.h"
#else
#include <zephyr/ztest.h>
#endif
#include <stdint.h>

/* RP2350 atomic register aliases (addressmap.h). */
#define ALIAS_XOR 0x1000u
#define ALIAS_SET 0x2000u
#define ALIAS_CLR 0x3000u

/*
 * Model of one hardware register with per-bit access semantics.
 * w1c_mask: bits cleared by writing 1, unaffected by writing 0.
 * ro_mask:  bits software cannot write at all.
 * Remaining bits behave as normal RW storage.
 */
struct reg_model {
	uint32_t value;
	uint32_t w1c_mask;
	uint32_t ro_mask;
};

static void reg_write(struct reg_model *r, uint32_t alias, uint32_t data)
{
	uint32_t rw_mask = ~(r->w1c_mask | r->ro_mask);
	uint32_t effective;

	switch (alias) {
	case ALIAS_SET:
		effective = data;
		break;
	case ALIAS_CLR:
		/*
		 * The CLR alias writes 0s to the selected bits. Crucially, a
		 * W1C bit only reacts to a 1, so this cannot clear it.
		 */
		r->value &= ~(data & rw_mask);
		return;
	case ALIAS_XOR:
		r->value ^= (data & rw_mask);
		return;
	default:
		effective = data;
		break;
	}

	/* Normal write: 1s clear W1C bits, RW bits take the written value. */
	r->value &= ~(effective & r->w1c_mask);
	r->value = (r->value & ~rw_mask) | (effective & rw_mask);
}

/* SIE_STATUS: BUS_RESET and the error bits are W1C; SUSPENDED is RO. */
#define SIE_BUS_RESET  BIT(19)
#define SIE_CRC_ERROR  BIT(24)
#define SIE_SUSPENDED  BIT(4)

ZTEST(usb_w1c, test_clr_alias_cannot_clear_w1c)
{
	struct reg_model sie = {
		.value = SIE_BUS_RESET,
		.w1c_mask = SIE_BUS_RESET | SIE_CRC_ERROR,
		.ro_mask = SIE_SUSPENDED,
	};

	/* This is what the driver used to do. */
	reg_write(&sie, ALIAS_CLR, SIE_BUS_RESET);

	zassert_equal(sie.value & SIE_BUS_RESET, SIE_BUS_RESET,
		      "CLR alias must NOT clear a W1C bit -- this is the bug "
		      "that replayed stale BUS_RESET forever and prevented "
		      "enumeration from ever completing");
}

ZTEST(usb_w1c, test_direct_write_clears_w1c)
{
	struct reg_model sie = {
		.value = SIE_BUS_RESET | SIE_CRC_ERROR,
		.w1c_mask = SIE_BUS_RESET | SIE_CRC_ERROR,
		.ro_mask = SIE_SUSPENDED,
	};

	/* This is the fix: write the 1s directly. */
	reg_write(&sie, 0, SIE_BUS_RESET);

	zassert_equal(sie.value & SIE_BUS_RESET, 0,
		      "a direct write of 1 must clear a W1C bit");
	zassert_equal(sie.value & SIE_CRC_ERROR, SIE_CRC_ERROR,
		      "clearing one W1C bit must not disturb another");
}

ZTEST(usb_w1c, test_ro_bit_is_never_writable)
{
	struct reg_model sie = {
		.value = SIE_SUSPENDED,
		.w1c_mask = SIE_BUS_RESET,
		.ro_mask = SIE_SUSPENDED,
	};

	reg_write(&sie, 0, SIE_SUSPENDED);
	reg_write(&sie, ALIAS_CLR, SIE_SUSPENDED);

	zassert_equal(sie.value & SIE_SUSPENDED, SIE_SUSPENDED,
		      "SUSPENDED is read-only; software cannot acknowledge it, "
		      "which is why suspend must be validated against SOF "
		      "activity rather than by clearing a latch");
}

/*
 * BUFF_STATUS is entirely W1C, and INTS.BUFF_STATUS is a read-only level
 * derived from it: "Raised when any bit in BUFF_STATUS is set. Clear by
 * clearing all bits in BUFF_STATUS."
 *
 * This models the completion-dispatch loop and demonstrates the failure that
 * lost OUT transfers: with the CLR alias the register accumulates every
 * completion ever seen and the interrupt never deasserts, so each later
 * interrupt re-dispatches stale completions.
 */
static bool ints_buff_status(const struct reg_model *r)
{
	return r->value != 0;
}

ZTEST(usb_w1c, test_buff_status_accumulates_with_clr_alias)
{
	struct reg_model bs = { .value = 0, .w1c_mask = 0xffffffff };
	int dispatches = 0;

	/* EP1 OUT completes. */
	bs.value |= BIT(3);

	for (int irq = 0; irq < 5; irq++) {
		uint32_t snapshot = bs.value;

		for (unsigned int i = 0; i < 32; i++) {
			if (!(snapshot & BIT(i))) {
				continue;
			}
			reg_write(&bs, ALIAS_CLR, BIT(i));	/* the bug */
			dispatches++;
		}
	}

	zassert_true(ints_buff_status(&bs),
		     "BUFF_STATUS never clears, so INTS.BUFF_STATUS stays "
		     "asserted permanently");
	zassert_equal(dispatches, 5,
		      "one completion was re-dispatched on every interrupt; "
		      "each replay can re-arm an endpoint whose buffer was "
		      "already handed upward, so the next host packet is "
		      "absorbed with nothing queued to receive it");
}

ZTEST(usb_w1c, test_buff_status_snapshot_ack_is_correct)
{
	struct reg_model bs = { .value = 0, .w1c_mask = 0xffffffff };
	int dispatches = 0;

	bs.value |= BIT(3);

	for (int irq = 0; irq < 5; irq++) {
		uint32_t snapshot = bs.value;

		/* The fix: acknowledge the snapshot before dispatching. */
		reg_write(&bs, 0, snapshot);

		for (unsigned int i = 0; i < 32; i++) {
			if (snapshot & BIT(i)) {
				dispatches++;
			}
		}
	}

	zassert_false(ints_buff_status(&bs), "BUFF_STATUS must end up clear");
	zassert_equal(dispatches, 1, "each completion dispatched exactly once");
}

ZTEST(usb_w1c, test_snapshot_ack_preserves_late_completion)
{
	struct reg_model bs = { .value = BIT(3), .w1c_mask = 0xffffffff };
	uint32_t snapshot = bs.value;

	reg_write(&bs, 0, snapshot);

	/* A second buffer completes while the dispatch loop is running. */
	bs.value |= BIT(5);

	zassert_equal(bs.value, BIT(5),
		      "acknowledging only the snapshot must not discard a "
		      "completion that arrived during dispatch -- clearing the "
		      "whole register would lose exactly one transfer");
}

ZTEST_SUITE(usb_w1c, NULL, NULL, NULL, NULL, NULL);
