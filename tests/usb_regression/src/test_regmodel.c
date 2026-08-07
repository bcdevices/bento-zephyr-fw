/*
 * Copyright (c) 2026 Blue Clover Devices
 * SPDX-License-Identifier: Apache-2.0
 *
 * Register-level hardware model of the RP2350 USB device controller, plus a
 * deterministic fuzzer over the driver's ISR and endpoint state machine.
 *
 * Motivation
 * ----------
 * The existing tests in this directory are hand-written scenarios: each one
 * encodes a bug somebody already found. That is useful as a regression net but
 * it cannot find bug number four. The single most damaging bug family in this
 * project -- clearing a write-1-to-clear (W1C) latch through
 * REG_ALIAS_CLR_BITS, which writes 0s and is therefore a no-op -- occurred
 * three separate times in three separate registers (SIE_STATUS, BUFF_STATUS,
 * EP_ABORT_DONE) and was found three separate times, by hand, hours apart.
 *
 * A model that knows which bits are W1C finds all three in one pass, and finds
 * the fourth before it ships. That is what RM_ACC_WC below buys: the access
 * type is transcribed from the *_ACCESS strings in
 *   modules/hal/rpi_pico/src/rp2350/hardware_regs/include/hardware/regs/usb.h
 * which is the authoritative, generated description of the silicon.
 *
 * Part 1 (rm_reg / rm_hw) models the register block with per-bit semantics:
 * WC, RO, RW, SC, and the three atomic aliases. It also models the *derived*
 * interrupt level INTS.BUFF_STATUS, which is read-only and asserted while any
 * BUFF_STATUS bit is set -- that dependency is what turned a cosmetic missing
 * acknowledge into a permanently stuck interrupt.
 *
 * Part 2 (rm_fuzz_*) drives random event sequences from a seeded xorshift PRNG
 * against a model of the driver's ISR + endpoint state, checking invariants
 * after every step.
 *
 * Everything here is prefixed rm_/RM_ because the host runner concatenates all
 * test_*.c into one translation unit.
 */

#ifdef HOST_TEST
#include "../host_shim.h"
#else
#include <zephyr/ztest.h>
#endif
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/*
 * A formatted failure that does not abort the sweep. The generic W1C test wants
 * to report EVERY offending bit in one run, not stop at the first -- the whole
 * point is that three instances of the same bug get found together.
 */
#define RM_REPORT_FAIL(fmt, ...) zassert_true(false, fmt, ##__VA_ARGS__)

/* ------------------------------------------------------------------------ */
/* Part 1: register model                                                    */
/* ------------------------------------------------------------------------ */

/*
 * RP2350 atomic register aliases (addressmap.h). The alias is encoded in
 * address bits 13:12 of the peripheral window.
 */
#define RM_ALIAS_NORM 0x0000u
#define RM_ALIAS_XOR  0x1000u	/* 0x1 << 12 */
#define RM_ALIAS_SET  0x2000u	/* 0x2 << 12 */
#define RM_ALIAS_CLR  0x3000u	/* 0x3 << 12 */

/* Per-bit access classes, transcribed from the *_ACCESS strings in usb.h. */
enum rm_access {
	RM_ACC_RW,	/* normal storage */
	RM_ACC_RO,	/* software can never write it */
	RM_ACC_WC,	/* write 1 to clear; writing 0 does nothing */
	RM_ACC_SC,	/* self-clearing: write 1 triggers, reads back 0 */
};

struct rm_field {
	const char *name;
	uint32_t mask;
	enum rm_access access;
};

struct rm_reg {
	const char *name;
	uint32_t value;
	uint32_t w1c_mask;
	uint32_t ro_mask;
	uint32_t sc_mask;
	/* Bits the hardware last saw a 1 written to on an SC field. */
	uint32_t sc_triggered;
	const struct rm_field *fields;
	unsigned int n_fields;
};

static uint32_t rm_mask_of(const struct rm_field *f, unsigned int n,
			   enum rm_access acc)
{
	uint32_t m = 0;

	for (unsigned int i = 0; i < n; i++) {
		if (f[i].access == acc) {
			m |= f[i].mask;
		}
	}

	return m;
}

static void rm_reg_init(struct rm_reg *r, const char *name,
			const struct rm_field *fields, unsigned int n,
			uint32_t reset_value)
{
	r->name = name;
	r->fields = fields;
	r->n_fields = n;
	r->value = reset_value;
	r->w1c_mask = rm_mask_of(fields, n, RM_ACC_WC);
	r->ro_mask = rm_mask_of(fields, n, RM_ACC_RO);
	r->sc_mask = rm_mask_of(fields, n, RM_ACC_SC);
	r->sc_triggered = 0;
}

/* Bits that behave as plain storage. */
static uint32_t rm_rw_mask(const struct rm_reg *r)
{
	return ~(r->w1c_mask | r->ro_mask | r->sc_mask);
}

/*
 * The core of the model. Every driver register access in the tests below goes
 * through here, so the semantics are stated exactly once.
 */
static void rm_write(struct rm_reg *r, uint32_t alias, uint32_t data)
{
	const uint32_t rw = rm_rw_mask(r);
	uint32_t effective;

	switch (alias) {
	case RM_ALIAS_SET:
		/* SET alias: the written 1s are OR'd in, 0s are ignored. */
		effective = r->value | data;
		break;
	case RM_ALIAS_CLR:
		/*
		 * CLR alias: the hardware writes 0s to the selected bits.
		 *
		 * This is the whole point of the model. A W1C bit reacts only
		 * to a 1, so a CLR-alias write CANNOT acknowledge it. An RO bit
		 * is likewise untouched. Only RW storage bits actually clear.
		 */
		r->value &= ~(data & rw);
		return;
	case RM_ALIAS_XOR:
		r->value ^= (data & rw);
		return;
	default:
		effective = data;
		break;
	}

	/* W1C: a written 1 clears; a written 0 leaves the latch alone. */
	r->value &= ~(data & r->w1c_mask);

	/* SC: a written 1 triggers the action, but never stores. */
	r->sc_triggered |= (data & r->sc_mask);
	r->value &= ~r->sc_mask;

	/* RW storage takes the written value; RO bits are dropped. */
	r->value = (r->value & ~rw) | (effective & rw);
}

static uint32_t rm_read(const struct rm_reg *r)
{
	return r->value;
}

/* Hardware-side event injection: sets a latch regardless of access type. */
static void rm_hw_assert(struct rm_reg *r, uint32_t bits)
{
	r->value |= bits;
}

static void rm_hw_deassert(struct rm_reg *r, uint32_t bits)
{
	r->value &= ~bits;
}

/* --- Field tables, transcribed from hardware_regs/usb.h ----------------- */

#define RM_BIT(n) (1UL << (n))

/* SIE_STATUS (offset 0x50). */
#define RM_SIE_DATA_SEQ_ERROR   RM_BIT(31)
#define RM_SIE_ACK_REC          RM_BIT(30)
#define RM_SIE_STALL_REC        RM_BIT(29)
#define RM_SIE_NAK_REC          RM_BIT(28)
#define RM_SIE_RX_TIMEOUT       RM_BIT(27)
#define RM_SIE_RX_OVERFLOW      RM_BIT(26)
#define RM_SIE_BIT_STUFF_ERROR  RM_BIT(25)
#define RM_SIE_CRC_ERROR        RM_BIT(24)
#define RM_SIE_ENDPOINT_ERROR   RM_BIT(23)
#define RM_SIE_BUS_RESET        RM_BIT(19)
#define RM_SIE_TRANS_COMPLETE   RM_BIT(18)
#define RM_SIE_SETUP_REC        RM_BIT(17)
#define RM_SIE_CONNECTED        RM_BIT(16)
#define RM_SIE_RX_SHORT_PACKET  RM_BIT(12)
#define RM_SIE_RESUME           RM_BIT(11)
#define RM_SIE_VBUS_OVER_CURR   RM_BIT(10)
#define RM_SIE_SPEED            (0x300UL)
#define RM_SIE_SUSPENDED        RM_BIT(4)
#define RM_SIE_LINE_STATE       (0x00cUL)
#define RM_SIE_VBUS_DETECTED    RM_BIT(0)

static const struct rm_field rm_sie_status_fields[] = {
	{ "DATA_SEQ_ERROR",  RM_SIE_DATA_SEQ_ERROR,  RM_ACC_WC },
	{ "ACK_REC",         RM_SIE_ACK_REC,         RM_ACC_WC },
	{ "STALL_REC",       RM_SIE_STALL_REC,       RM_ACC_WC },
	{ "NAK_REC",         RM_SIE_NAK_REC,         RM_ACC_WC },
	{ "RX_TIMEOUT",      RM_SIE_RX_TIMEOUT,      RM_ACC_WC },
	{ "RX_OVERFLOW",     RM_SIE_RX_OVERFLOW,     RM_ACC_WC },
	{ "BIT_STUFF_ERROR", RM_SIE_BIT_STUFF_ERROR, RM_ACC_WC },
	{ "CRC_ERROR",       RM_SIE_CRC_ERROR,       RM_ACC_WC },
	{ "ENDPOINT_ERROR",  RM_SIE_ENDPOINT_ERROR,  RM_ACC_WC },
	{ "BUS_RESET",       RM_SIE_BUS_RESET,       RM_ACC_WC },
	{ "TRANS_COMPLETE",  RM_SIE_TRANS_COMPLETE,  RM_ACC_WC },
	{ "SETUP_REC",       RM_SIE_SETUP_REC,       RM_ACC_WC },
	{ "CONNECTED",       RM_SIE_CONNECTED,       RM_ACC_RO },
	{ "RX_SHORT_PACKET", RM_SIE_RX_SHORT_PACKET, RM_ACC_WC },
	{ "RESUME",          RM_SIE_RESUME,          RM_ACC_WC },
	{ "VBUS_OVER_CURR",  RM_SIE_VBUS_OVER_CURR,  RM_ACC_RO },
	{ "SPEED",           RM_SIE_SPEED,           RM_ACC_RO },
	{ "SUSPENDED",       RM_SIE_SUSPENDED,       RM_ACC_RO },
	{ "LINE_STATE",      RM_SIE_LINE_STATE,      RM_ACC_RO },
	{ "VBUS_DETECTED",   RM_SIE_VBUS_DETECTED,   RM_ACC_RO },
};

/*
 * BUFF_STATUS (offset 0x58): 32 W1C bits, EPn_IN at bit 2n, EPn_OUT at 2n+1.
 * EP_ABORT_DONE (0x64) has the identical layout and is also fully W1C.
 */
static const struct rm_field rm_buff_status_fields[] = {
	{ "EP_BUFF", 0xffffffffUL, RM_ACC_WC },
};

static const struct rm_field rm_abort_done_fields[] = {
	{ "EP_ABORT_DONE", 0xffffffffUL, RM_ACC_WC },
};

/* EP_ABORT (0x60): plain RW, 32 bits. Deliberately NOT W1C. */
static const struct rm_field rm_abort_fields[] = {
	{ "EP_ABORT", 0xffffffffUL, RM_ACC_RW },
};

/* EP_STALL_ARM (0x68): only EP0_IN/EP0_OUT exist, both RW. */
#define RM_STALL_ARM_EP0_IN   RM_BIT(0)
#define RM_STALL_ARM_EP0_OUT  RM_BIT(1)

static const struct rm_field rm_stall_arm_fields[] = {
	{ "EP0_IN",  RM_STALL_ARM_EP0_IN,  RM_ACC_RW },
	{ "EP0_OUT", RM_STALL_ARM_EP0_OUT, RM_ACC_RW },
	{ "RESERVED", (uint32_t)~(RM_STALL_ARM_EP0_IN | RM_STALL_ARM_EP0_OUT), RM_ACC_RO },
};

/* EP_TX_ERROR / EP_RX_ERROR: two W1C bits per endpoint. */
static const struct rm_field rm_tx_error_fields[] = {
	{ "EP_TX_ERROR", 0xffffffffUL, RM_ACC_WC },
};

static const struct rm_field rm_rx_error_fields[] = {
	{ "EP_RX_ERROR", 0xffffffffUL, RM_ACC_WC },
};

/* INTE (0x90): every bit RW. INTS (0x98): every bit RO, derived. */
#define RM_INT_ENDPOINT_ERROR        RM_BIT(21)
#define RM_INT_ABORT_DONE            RM_BIT(18)
#define RM_INT_DEV_SOF               RM_BIT(17)
#define RM_INT_SETUP_REQ             RM_BIT(16)
#define RM_INT_DEV_RESUME_FROM_HOST  RM_BIT(15)
#define RM_INT_DEV_SUSPEND           RM_BIT(14)
#define RM_INT_DEV_CONN_DIS          RM_BIT(13)
#define RM_INT_BUS_RESET             RM_BIT(12)
#define RM_INT_VBUS_DETECT           RM_BIT(11)
#define RM_INT_ERROR_CRC             RM_BIT(9)
#define RM_INT_ERROR_BIT_STUFF       RM_BIT(8)
#define RM_INT_ERROR_RX_OVERFLOW     RM_BIT(7)
#define RM_INT_ERROR_RX_TIMEOUT      RM_BIT(6)
#define RM_INT_ERROR_DATA_SEQ        RM_BIT(5)
#define RM_INT_BUFF_STATUS           RM_BIT(4)
#define RM_INT_TRANS_COMPLETE        RM_BIT(3)

static const struct rm_field rm_inte_fields[] = {
	{ "INTE", 0xffffffffUL, RM_ACC_RW },
};

static const struct rm_field rm_ints_fields[] = {
	{ "INTS", 0xffffffffUL, RM_ACC_RO },
};

/* SOF_RD (0x28): COUNT is RO, bits 10:0. */
#define RM_SOF_RD_COUNT 0x7ffUL

static const struct rm_field rm_sof_rd_fields[] = {
	{ "COUNT", RM_SOF_RD_COUNT, RM_ACC_RO },
	{ "RESERVED", (uint32_t)~RM_SOF_RD_COUNT, RM_ACC_RO },
};

/* ADDR_ENDP (0x00): ADDRESS bits 6:0 RW, ENDPOINT bits 19:16 RW. */
#define RM_ADDR_ENDP_ADDRESS  0x0000007fUL
#define RM_ADDR_ENDP_ENDPOINT 0x000f0000UL

static const struct rm_field rm_addr_endp_fields[] = {
	{ "ADDRESS",  RM_ADDR_ENDP_ADDRESS,  RM_ACC_RW },
	{ "ENDPOINT", RM_ADDR_ENDP_ENDPOINT, RM_ACC_RW },
	{ "RESERVED", (uint32_t)~(RM_ADDR_ENDP_ADDRESS | RM_ADDR_ENDP_ENDPOINT), RM_ACC_RO },
};

/* SIE_CTRL (0x4c): mostly RW, but RESET_BUS/RESUME/STOP_TRANS/START_TRANS are SC. */
#define RM_SIE_CTRL_EP0_INT_1BUF  RM_BIT(29)
#define RM_SIE_CTRL_PULLUP_EN     RM_BIT(16)
#define RM_SIE_CTRL_RESET_BUS     RM_BIT(13)
#define RM_SIE_CTRL_RESUME        RM_BIT(12)
#define RM_SIE_CTRL_STOP_TRANS    RM_BIT(4)
#define RM_SIE_CTRL_START_TRANS   RM_BIT(0)

#define RM_SIE_CTRL_SC_BITS                                                    \
	(RM_SIE_CTRL_RESET_BUS | RM_SIE_CTRL_RESUME | RM_SIE_CTRL_STOP_TRANS | \
	 RM_SIE_CTRL_START_TRANS)

static const struct rm_field rm_sie_ctrl_fields[] = {
	{ "RESET_BUS",   RM_SIE_CTRL_RESET_BUS,   RM_ACC_SC },
	{ "RESUME",      RM_SIE_CTRL_RESUME,      RM_ACC_SC },
	{ "STOP_TRANS",  RM_SIE_CTRL_STOP_TRANS,  RM_ACC_SC },
	{ "START_TRANS", RM_SIE_CTRL_START_TRANS, RM_ACC_SC },
	{ "RW_REST",     (uint32_t)~RM_SIE_CTRL_SC_BITS, RM_ACC_RW },
};

/* --- DPRAM endpoint buffer control ------------------------------------- */

/*
 * ep_buf_ctrl words live in DPRAM, not in the register block, so they have no
 * atomic aliases of their own -- but the driver reaches them through
 * rpi_pico_bit_set()/rpi_pico_bit_clr() in ep_set_halt/ep_clear_halt, which
 * compute an aliased address. DPRAM does support the aliases (it is in the
 * same 0x5000_0000 window), so model them the same way. Every bit is plain RW
 * storage as far as software is concerned; the SIE mutates FULL/AVAILABLE and
 * the length field on its own.
 */
#define RM_BUF_CTRL_FULL      RM_BIT(15)
#define RM_BUF_CTRL_DATA1_PID RM_BIT(13)
#define RM_BUF_CTRL_DATA0_PID (0UL)
#define RM_BUF_CTRL_STALL     RM_BIT(11)
#define RM_BUF_CTRL_AVAIL     RM_BIT(10)
#define RM_BUF_CTRL_LEN_MASK  (0x3ffUL)

static const struct rm_field rm_buf_ctrl_fields[] = {
	{ "BUF_CTRL", 0xffffffffUL, RM_ACC_RW },
};

/* --- The modelled register block --------------------------------------- */

#define RM_NUM_EPS 16

struct rm_hw {
	struct rm_reg addr_endp;
	struct rm_reg sof_rd;
	struct rm_reg sie_ctrl;
	struct rm_reg sie_status;
	struct rm_reg buf_status;
	struct rm_reg abort;
	struct rm_reg abort_done;
	struct rm_reg stall_arm;
	struct rm_reg tx_error;
	struct rm_reg rx_error;
	struct rm_reg inte;
	/* ep_buf_ctrl[n].in / .out, indexed [ep][dir] with dir 0=IN, 1=OUT. */
	struct rm_reg buf_ctrl[RM_NUM_EPS][2];
};

#define RM_INIT(hw, member, tbl, reset)                                        \
	rm_reg_init(&(hw)->member, #member, tbl, ARRAY_SIZE_RM(tbl), reset)

#define ARRAY_SIZE_RM(a) (sizeof(a) / sizeof((a)[0]))

static void rm_hw_reset(struct rm_hw *hw)
{
	memset(hw, 0, sizeof(*hw));

	RM_INIT(hw, addr_endp,  rm_addr_endp_fields,  0);
	RM_INIT(hw, sof_rd,     rm_sof_rd_fields,     0);
	RM_INIT(hw, sie_ctrl,   rm_sie_ctrl_fields,   0);
	RM_INIT(hw, sie_status, rm_sie_status_fields, 0);
	RM_INIT(hw, buf_status, rm_buff_status_fields, 0);
	RM_INIT(hw, abort,      rm_abort_fields,      0);
	RM_INIT(hw, abort_done, rm_abort_done_fields, 0);
	RM_INIT(hw, stall_arm,  rm_stall_arm_fields,  0);
	RM_INIT(hw, tx_error,   rm_tx_error_fields,   0);
	RM_INIT(hw, rx_error,   rm_rx_error_fields,   0);
	RM_INIT(hw, inte,       rm_inte_fields,       0);

	for (int e = 0; e < RM_NUM_EPS; e++) {
		for (int d = 0; d < 2; d++) {
			rm_reg_init(&hw->buf_ctrl[e][d], "ep_buf_ctrl",
				    rm_buf_ctrl_fields,
				    ARRAY_SIZE_RM(rm_buf_ctrl_fields), 0);
		}
	}
}

/*
 * INTR: the raw interrupt state, derived from the latches. This is the model's
 * most important piece of behaviour beyond W1C: INTS.BUFF_STATUS is read-only
 * and is a *level* asserted while any BUFF_STATUS bit is set. Failing to
 * acknowledge BUFF_STATUS therefore does not merely lose a bit of bookkeeping;
 * it wedges the interrupt line high forever and every subsequent ISR entry
 * re-dispatches the same stale completions.
 *
 * The same is true, less catastrophically, for the SIE_STATUS-derived bits.
 */
static uint32_t rm_intr(const struct rm_hw *hw)
{
	uint32_t sie = hw->sie_status.value;
	uint32_t intr = 0;

	if (hw->buf_status.value != 0) {
		intr |= RM_INT_BUFF_STATUS;
	}
	if (hw->abort_done.value != 0) {
		intr |= RM_INT_ABORT_DONE;
	}
	if (hw->tx_error.value != 0 || hw->rx_error.value != 0) {
		intr |= RM_INT_ENDPOINT_ERROR;
	}

	if (sie & RM_SIE_SETUP_REC) {
		intr |= RM_INT_SETUP_REQ;
	}
	if (sie & RM_SIE_BUS_RESET) {
		intr |= RM_INT_BUS_RESET;
	}
	if (sie & RM_SIE_SUSPENDED) {
		intr |= RM_INT_DEV_SUSPEND;
	}
	if (sie & RM_SIE_RESUME) {
		intr |= RM_INT_DEV_RESUME_FROM_HOST;
	}
	if (sie & RM_SIE_CONNECTED) {
		intr |= RM_INT_DEV_CONN_DIS;
	}
	if (sie & RM_SIE_VBUS_DETECTED) {
		intr |= RM_INT_VBUS_DETECT;
	}
	if (sie & RM_SIE_CRC_ERROR) {
		intr |= RM_INT_ERROR_CRC;
	}
	if (sie & RM_SIE_BIT_STUFF_ERROR) {
		intr |= RM_INT_ERROR_BIT_STUFF;
	}
	if (sie & RM_SIE_RX_OVERFLOW) {
		intr |= RM_INT_ERROR_RX_OVERFLOW;
	}
	if (sie & RM_SIE_RX_TIMEOUT) {
		intr |= RM_INT_ERROR_RX_TIMEOUT;
	}
	if (sie & RM_SIE_DATA_SEQ_ERROR) {
		intr |= RM_INT_ERROR_DATA_SEQ;
	}
	if (sie & RM_SIE_TRANS_COMPLETE) {
		intr |= RM_INT_TRANS_COMPLETE;
	}

	return intr;
}

/* INTS = INTR & INTE, and every bit of it is RO. */
static uint32_t rm_ints(const struct rm_hw *hw)
{
	return rm_intr(hw) & hw->inte.value;
}

/* --- Driver accessor idioms, mirrored exactly --------------------------- */

/* rpi_pico_bit_set(): sys_write32(bit, REG_ALIAS_SET_BITS | reg) */
static void rm_drv_bit_set(struct rm_reg *r, uint32_t bit)
{
	rm_write(r, RM_ALIAS_SET, bit);
}

/* rpi_pico_bit_clr(): sys_write32(bit, REG_ALIAS_CLR_BITS | reg) */
static void rm_drv_bit_clr(struct rm_reg *r, uint32_t bit)
{
	rm_write(r, RM_ALIAS_CLR, bit);
}

/* sie_status_clr(): sys_write32(bit, &base->sie_status) -- a direct write. */
static void rm_drv_sie_status_clr(struct rm_hw *hw, uint32_t bit)
{
	rm_write(&hw->sie_status, RM_ALIAS_NORM, bit);
}

/* ======================================================================== */
/* Part 1 tests                                                             */
/* ======================================================================== */

/*
 * THE generic test. Rather than asserting "SIE_STATUS.BUS_RESET must not be
 * cleared with the CLR alias" three times for three registers found three
 * months apart, walk every W1C bit in every modelled register and assert the
 * property once. This is the test that would have caught all three historical
 * instances in a single run, and that catches the fourth.
 */
static struct rm_reg *rm_all_w1c_regs(struct rm_hw *hw, unsigned int i,
				      const char **name)
{
	switch (i) {
	case 0: *name = "SIE_STATUS";    return &hw->sie_status;
	case 1: *name = "BUFF_STATUS";   return &hw->buf_status;
	case 2: *name = "EP_ABORT_DONE"; return &hw->abort_done;
	case 3: *name = "EP_TX_ERROR";   return &hw->tx_error;
	case 4: *name = "EP_RX_ERROR";   return &hw->rx_error;
	default: *name = NULL;           return NULL;
	}
}

#define RM_N_W1C_REGS 5

ZTEST(usb_regmodel, test_clr_alias_cannot_clear_any_w1c_bit)
{
	struct rm_hw hw;
	unsigned int checked = 0;

	for (unsigned int ri = 0; ri < RM_N_W1C_REGS; ri++) {
		const char *rname;
		struct rm_reg *r;

		rm_hw_reset(&hw);
		r = rm_all_w1c_regs(&hw, ri, &rname);

		for (unsigned int b = 0; b < 32; b++) {
			uint32_t bit = RM_BIT(b);

			if (!(r->w1c_mask & bit)) {
				continue;
			}

			/* Hardware latches the event. */
			rm_hw_assert(r, bit);

			/* Driver tries to acknowledge with the CLR alias. */
			rm_drv_bit_clr(r, bit);

			if ((r->value & bit) == 0) {
				RM_REPORT_FAIL(
					"%s bit %u: the CLR alias appeared to "
					"clear a W1C latch. On real silicon the "
					"alias writes 0s, which a W1C bit "
					"ignores, so the latch would stay set "
					"and the interrupt would re-fire "
					"forever. Acknowledge it with a direct "
					"sys_write32() of the 1s instead.",
					rname, b);
			}

			/* And the correct idiom must work. */
			rm_write(r, RM_ALIAS_NORM, bit);
			if (r->value & bit) {
				RM_REPORT_FAIL("%s bit %u: a direct write of 1 "
					     "failed to clear a W1C latch",
					     rname, b);
			}

			checked++;
		}
	}

	zassert_true(checked >= 32 + 32 + 32 + 32 + 32 - 64,
		     "expected to sweep a meaningful number of W1C bits, "
		     "swept %u -- if this drops to zero the test is vacuous",
		     checked);
}

/*
 * The mirror-image property: the SET alias must never be used to acknowledge a
 * W1C bit either, because it writes a 1 and therefore silently *does* clear it
 * -- which happens to be right, but only by accident, and it also OR-writes
 * every other bit position, so on a mixed register it is not equivalent to a
 * direct write. The check that matters is that the driver's actual idiom
 * (direct write of the snapshot) is the only one that is both correct and
 * side-effect free.
 */
ZTEST(usb_regmodel, test_set_alias_on_mixed_register_has_side_effects)
{
	struct rm_hw hw;
	uint32_t before, after;

	rm_hw_reset(&hw);

	/* SIE_STATUS has both W1C latches and RO status bits. */
	rm_hw_assert(&hw.sie_status, RM_SIE_BUS_RESET | RM_SIE_CONNECTED |
					     RM_SIE_VBUS_DETECTED);
	before = rm_read(&hw.sie_status);

	rm_drv_sie_status_clr(&hw, RM_SIE_BUS_RESET);
	after = rm_read(&hw.sie_status);

	zassert_equal(after & RM_SIE_BUS_RESET, 0,
		      "direct write must acknowledge BUS_RESET");
	zassert_equal(after & (RM_SIE_CONNECTED | RM_SIE_VBUS_DETECTED),
		      before & (RM_SIE_CONNECTED | RM_SIE_VBUS_DETECTED),
		      "acknowledging one latch must not disturb the RO status "
		      "bits the ISR reads immediately afterwards");
}

/* Every RO bit in the model must be unwritable through every alias. */
ZTEST(usb_regmodel, test_ro_bits_unwritable_through_every_alias)
{
	struct rm_hw hw;
	static const uint32_t aliases[] = { RM_ALIAS_NORM, RM_ALIAS_SET,
					    RM_ALIAS_CLR, RM_ALIAS_XOR };

	for (unsigned int ai = 0; ai < ARRAY_SIZE_RM(aliases); ai++) {
		rm_hw_reset(&hw);

		rm_hw_assert(&hw.sie_status,
			     RM_SIE_SUSPENDED | RM_SIE_CONNECTED |
				     RM_SIE_VBUS_DETECTED | RM_SIE_LINE_STATE);

		rm_write(&hw.sie_status, aliases[ai], 0xffffffffUL);

		zassert_equal(hw.sie_status.value &
				      (RM_SIE_SUSPENDED | RM_SIE_CONNECTED |
				       RM_SIE_VBUS_DETECTED | RM_SIE_LINE_STATE),
			      RM_SIE_SUSPENDED | RM_SIE_CONNECTED |
				      RM_SIE_VBUS_DETECTED | RM_SIE_LINE_STATE,
			      "RO bits must survive alias 0x%x -- SUSPENDED in "
			      "particular cannot be acknowledged by software, "
			      "which is why suspend has to be cross-checked "
			      "against SOF_RD rather than by clearing a latch",
			      aliases[ai]);
	}
}

/* SOF_RD is entirely read-only: the ISR may sample it but never write it. */
ZTEST(usb_regmodel, test_sof_rd_is_read_only)
{
	struct rm_hw hw;

	rm_hw_reset(&hw);
	hw.sof_rd.value = 0x123;

	rm_write(&hw.sof_rd, RM_ALIAS_NORM, 0);
	rm_write(&hw.sof_rd, RM_ALIAS_CLR, 0xffffffffUL);

	zassert_equal(rm_read(&hw.sof_rd) & RM_SOF_RD_COUNT, 0x123,
		      "SOF_RD.COUNT is RO; the frame counter is the "
		      "authoritative evidence that the bus is live and must "
		      "not be perturbable by the driver");
}

/*
 * SIE_CTRL: RESUME, RESET_BUS, STOP_TRANS and START_TRANS are self-clearing.
 * The driver uses the SET alias for RESUME (host_wakeup) and PULLUP_EN, and the
 * CLR alias for PULLUP_EN. PULLUP_EN is RW, so the CLR alias is correct there;
 * RESUME is SC, so it must never be read back as a state flag.
 */
ZTEST(usb_regmodel, test_sie_ctrl_pullup_and_resume_idioms)
{
	struct rm_hw hw;

	rm_hw_reset(&hw);

	/* sie_dp_pullup(dev, true) */
	rm_drv_bit_set(&hw.sie_ctrl, RM_SIE_CTRL_PULLUP_EN);
	zassert_equal(rm_read(&hw.sie_ctrl) & RM_SIE_CTRL_PULLUP_EN,
		      RM_SIE_CTRL_PULLUP_EN,
		      "SET alias must enable the DP pullup");

	/* sie_dp_pullup(dev, false) -- PULLUP_EN is RW, so CLR is correct. */
	rm_drv_bit_clr(&hw.sie_ctrl, RM_SIE_CTRL_PULLUP_EN);
	zassert_equal(rm_read(&hw.sie_ctrl) & RM_SIE_CTRL_PULLUP_EN, 0,
		      "PULLUP_EN is RW, so the CLR alias is the right idiom "
		      "here -- the CLR alias is not wrong in general, only on "
		      "W1C bits");

	/* udc_rpi_pico_host_wakeup() */
	rm_drv_bit_set(&hw.sie_ctrl, RM_SIE_CTRL_RESUME);
	zassert_equal(rm_read(&hw.sie_ctrl) & RM_SIE_CTRL_RESUME, 0,
		      "RESUME is self-clearing: it triggers on the write and "
		      "always reads back 0, so it can never be used to tell "
		      "whether a remote wakeup is in flight (which is why the "
		      "driver tracks rwu_pending in software)");
	zassert_equal(hw.sie_ctrl.sc_triggered & RM_SIE_CTRL_RESUME,
		      RM_SIE_CTRL_RESUME,
		      "...but the write must still have triggered the action");
}

/*
 * ADDR_ENDP: the bus-reset path writes 0 directly, which must actually zero
 * the address. If someone converted this to the CLR alias it would still work
 * (RW bits), but if they converted it to the SET alias the address would stick.
 */
ZTEST(usb_regmodel, test_addr_endp_reset_to_zero)
{
	struct rm_hw hw;

	rm_hw_reset(&hw);

	rm_write(&hw.addr_endp, RM_ALIAS_NORM, 0x27);
	zassert_equal(rm_read(&hw.addr_endp) & RM_ADDR_ENDP_ADDRESS, 0x27,
		      "set_address must land in ADDR_ENDP.ADDRESS");

	/* The bus-reset handler: sys_write32(0, &base->dev_addr_ctrl) */
	rm_write(&hw.addr_endp, RM_ALIAS_NORM, 0);
	zassert_equal(rm_read(&hw.addr_endp) & RM_ADDR_ENDP_ADDRESS, 0,
		      "a bus reset must return the device to address 0; "
		      "leaving a stale address makes the device deaf to the "
		      "host's re-enumeration attempts");
}

/*
 * The derived-level dependency, stated as an executable property: with the
 * driver's real idiom the interrupt deasserts; with the CLR alias it never
 * does. This is the difference between "we forgot to clear a bit" and "the
 * device is bricked until power cycle".
 */
ZTEST(usb_regmodel, test_ints_buff_status_is_a_derived_level)
{
	struct rm_hw hw;

	rm_hw_reset(&hw);
	rm_write(&hw.inte, RM_ALIAS_NORM, RM_INT_BUFF_STATUS);

	zassert_equal(rm_ints(&hw) & RM_INT_BUFF_STATUS, 0,
		      "no buffers complete, no interrupt");

	rm_hw_assert(&hw.buf_status, RM_BIT(3));
	zassert_equal(rm_ints(&hw) & RM_INT_BUFF_STATUS, RM_INT_BUFF_STATUS,
		      "INTS.BUFF_STATUS is raised while any BUFF_STATUS bit "
		      "is set");

	/* Wrong idiom. */
	rm_drv_bit_clr(&hw.buf_status, RM_BIT(3));
	zassert_equal(rm_ints(&hw) & RM_INT_BUFF_STATUS, RM_INT_BUFF_STATUS,
		      "the CLR alias leaves the level asserted -- the ISR "
		      "returns, the NVIC re-enters it immediately, and the "
		      "same completion is dispatched forever");

	/* Right idiom. */
	rm_write(&hw.buf_status, RM_ALIAS_NORM, RM_BIT(3));
	zassert_equal(rm_ints(&hw) & RM_INT_BUFF_STATUS, 0,
		      "acknowledging every set bit is the only thing that "
		      "deasserts the derived level");
}

/*
 * INTS is masked by INTE, and INTS is entirely RO. A driver that tried to
 * "acknowledge" an interrupt by writing INTS would accomplish nothing.
 */
ZTEST(usb_regmodel, test_ints_is_read_only_and_masked_by_inte)
{
	struct rm_hw hw;

	rm_hw_reset(&hw);
	rm_hw_assert(&hw.sie_status, RM_SIE_BUS_RESET);

	/* INTE clear: the raw condition exists but no interrupt is signalled. */
	zassert_equal(rm_intr(&hw) & RM_INT_BUS_RESET, RM_INT_BUS_RESET,
		      "the raw condition is present");
	zassert_equal(rm_ints(&hw) & RM_INT_BUS_RESET, 0,
		      "INTS must be masked by INTE");

	rm_write(&hw.inte, RM_ALIAS_NORM, RM_INT_BUS_RESET);
	zassert_equal(rm_ints(&hw) & RM_INT_BUS_RESET, RM_INT_BUS_RESET,
		      "enabling the mask exposes it");

	/* The only way to clear it is through the SIE_STATUS latch. */
	rm_drv_sie_status_clr(&hw, RM_SIE_BUS_RESET);
	zassert_equal(rm_ints(&hw) & RM_INT_BUS_RESET, 0,
		      "clearing the source latch is what clears the interrupt; "
		      "INTS itself is RO and cannot be written");
}

/*
 * EP_ABORT_DONE handshake, modelled end to end. This reproduces the third
 * historical instance of the family: the driver never acknowledged
 * EP_ABORT_DONE, so its bits accumulated and the second abort of an endpoint
 * exited the wait loop on a stale bit.
 */
ZTEST(usb_regmodel, test_abort_done_handshake_requires_ack)
{
	struct rm_hw hw;
	const uint32_t ep_mask = RM_BIT(2);	/* EP1 IN */

	rm_hw_reset(&hw);

	/* First abort: hardware raises done, driver acknowledges. */
	rm_write(&hw.abort_done, RM_ALIAS_NORM, ep_mask);	/* pre-clear */
	rm_drv_bit_set(&hw.abort, ep_mask);
	rm_hw_assert(&hw.abort_done, ep_mask);			/* SIE responds */
	zassert_equal(rm_read(&hw.abort_done) & ep_mask, ep_mask,
		      "handshake completes");
	rm_write(&hw.abort_done, RM_ALIAS_NORM, ep_mask);	/* ack */
	rm_drv_bit_clr(&hw.abort, ep_mask);

	zassert_equal(rm_read(&hw.abort_done) & ep_mask, 0,
		      "EP_ABORT_DONE must be acknowledged after the handshake, "
		      "otherwise the next abort of this endpoint sees a stale "
		      "done bit and skips the wait entirely -- modifying "
		      "buf_ctrl while the SIE still owns the buffer");
	zassert_equal(rm_read(&hw.abort) & ep_mask, 0,
		      "EP_ABORT is plain RW, so the CLR alias correctly "
		      "releases the abort request");
}

ZTEST(usb_regmodel, test_abort_done_without_ack_defeats_handshake)
{
	struct rm_hw hw;
	const uint32_t ep_mask = RM_BIT(2);
	bool second_abort_waited;

	rm_hw_reset(&hw);

	/* First abort, but acknowledged with the CLR alias (the old bug). */
	rm_drv_bit_set(&hw.abort, ep_mask);
	rm_hw_assert(&hw.abort_done, ep_mask);
	rm_drv_bit_clr(&hw.abort_done, ep_mask);	/* no-op on W1C */
	rm_drv_bit_clr(&hw.abort, ep_mask);

	/*
	 * Second abort. The driver spins until EP_ABORT_DONE shows the bit --
	 * but it is already set from last time, so the loop never actually
	 * waits and the SIE may still own the buffer.
	 */
	rm_drv_bit_set(&hw.abort, ep_mask);
	second_abort_waited = (rm_read(&hw.abort_done) & ep_mask) == 0;

	zassert_false(second_abort_waited,
		      "demonstrates the failure: the stale done bit makes the "
		      "second handshake return instantly");
	zassert_equal(rm_read(&hw.abort_done) & ep_mask, ep_mask,
		      "and the latch is still stuck");
}

/*
 * ep_buf_ctrl semantics: the FULL/AVAILABLE/DATA1_PID/length field layout, and
 * the two-step arm sequence the driver uses (write control word, nops, then
 * set AVAILABLE) which exists to avoid the concurrent-access hazard in
 * datasheet 4.1.2.5.1.
 */
ZTEST(usb_regmodel, test_buf_ctrl_arm_sequence_field_layout)
{
	struct rm_hw hw;
	struct rm_reg *bc;
	uint32_t word;

	rm_hw_reset(&hw);
	bc = &hw.buf_ctrl[1][0];	/* EP1 IN */

	/* prep_tx(): len | pid | FULL, then a second write adding AVAIL. */
	word = 37U | RM_BUF_CTRL_DATA1_PID | RM_BUF_CTRL_FULL;
	rm_write(bc, RM_ALIAS_NORM, word);

	zassert_equal(rm_read(bc) & RM_BUF_CTRL_AVAIL, 0,
		      "AVAILABLE must NOT be set by the first write; setting "
		      "it in the same store as the data lets the SIE read a "
		      "half-written descriptor");

	rm_write(bc, RM_ALIAS_NORM, word | RM_BUF_CTRL_AVAIL);

	zassert_equal(rm_read(bc) & RM_BUF_CTRL_LEN_MASK, 37,
		      "length occupies bits 9:0");
	zassert_equal(rm_read(bc) & RM_BUF_CTRL_FULL, RM_BUF_CTRL_FULL,
		      "FULL is bit 15 and marks an IN buffer as holding data");
	zassert_equal(rm_read(bc) & RM_BUF_CTRL_DATA1_PID, RM_BUF_CTRL_DATA1_PID,
		      "DATA1_PID is bit 13");
	zassert_equal(rm_read(bc) & RM_BUF_CTRL_AVAIL, RM_BUF_CTRL_AVAIL,
		      "AVAILABLE is bit 10 and hands the buffer to the SIE");
}

/*
 * prep_rx() must NOT set FULL: an OUT buffer is handed to the SIE empty, and
 * the SIE sets FULL when it has written data in. Setting FULL on an OUT arm
 * tells the controller the buffer already holds a packet.
 */
ZTEST(usb_regmodel, test_buf_ctrl_out_arm_leaves_full_clear)
{
	struct rm_hw hw;
	struct rm_reg *bc;
	uint32_t word;

	rm_hw_reset(&hw);
	bc = &hw.buf_ctrl[1][1];	/* EP1 OUT */

	/* prep_rx(): buf_ctrl = mps | pid; no FULL. */
	word = 64U | RM_BUF_CTRL_DATA0_PID;
	rm_write(bc, RM_ALIAS_NORM, word);
	rm_write(bc, RM_ALIAS_NORM, word | RM_BUF_CTRL_AVAIL);

	zassert_equal(rm_read(bc) & RM_BUF_CTRL_FULL, 0,
		      "an armed OUT buffer must be empty -- FULL is the SIE's "
		      "to set once a packet lands");
	zassert_equal(rm_read(bc) & RM_BUF_CTRL_AVAIL, RM_BUF_CTRL_AVAIL,
		      "but AVAILABLE must be set or the SIE will NAK");
}

/*
 * ep_set_halt on an OUT endpoint uses rpi_pico_bit_set() to OR in
 * STALL|AVAIL. Because buf_ctrl bits are RW, the SET alias is the correct
 * idiom -- and crucially AVAIL must survive, since the controller only emits a
 * STALL handshake for an OUT DATAx if the buffer is available.
 */
ZTEST(usb_regmodel, test_ep_set_halt_out_requires_avail)
{
	struct rm_hw hw;
	struct rm_reg *bc;

	rm_hw_reset(&hw);
	bc = &hw.buf_ctrl[1][1];

	/* ep_cancel() cleared AVAIL first. */
	rm_write(bc, RM_ALIAS_NORM, 64U);

	/* rpi_pico_bit_set(buf_ctrl_reg, STALL | AVAIL) */
	rm_drv_bit_set(bc, RM_BUF_CTRL_STALL | RM_BUF_CTRL_AVAIL);

	zassert_equal(rm_read(bc) & (RM_BUF_CTRL_STALL | RM_BUF_CTRL_AVAIL),
		      RM_BUF_CTRL_STALL | RM_BUF_CTRL_AVAIL,
		      "a halted OUT endpoint needs both STALL and AVAILABLE, "
		      "otherwise the controller NAKs instead of stalling and "
		      "the host retries forever");

	/* ep_clear_halt on an IN endpoint uses the CLR alias -- RW, so fine. */
	bc = &hw.buf_ctrl[1][0];
	rm_drv_bit_set(bc, RM_BUF_CTRL_STALL);
	rm_drv_bit_clr(bc, RM_BUF_CTRL_STALL);
	zassert_equal(rm_read(bc) & RM_BUF_CTRL_STALL, 0,
		      "buf_ctrl.STALL is RW storage, so the CLR alias really "
		      "does clear it");
}

/*
 * Replay of the whole ISR acknowledge path with the driver's current idioms,
 * asserting that after one ISR pass nothing is left latched. This is the
 * register-level statement of fuzzer invariant F1.
 */
ZTEST(usb_regmodel, test_isr_ack_path_leaves_no_latch_set)
{
	struct rm_hw hw;
	uint32_t status;

	rm_hw_reset(&hw);
	rm_write(&hw.inte, RM_ALIAS_NORM, 0xffffffffUL);

	/* A pathological interrupt: everything at once. */
	rm_hw_assert(&hw.sie_status,
		     RM_SIE_BUS_RESET | RM_SIE_SETUP_REC | RM_SIE_CRC_ERROR |
			     RM_SIE_BIT_STUFF_ERROR | RM_SIE_DATA_SEQ_ERROR |
			     RM_SIE_RX_TIMEOUT | RM_SIE_RX_OVERFLOW |
			     RM_SIE_RESUME);
	rm_hw_assert(&hw.buf_status, RM_BIT(0) | RM_BIT(3) | RM_BIT(5));

	status = rm_ints(&hw);

	/* Mirror of rpi_pico_isr_handler(), in the same order. */
	if (status & RM_INT_DEV_RESUME_FROM_HOST) {
		rm_drv_sie_status_clr(&hw, RM_SIE_RESUME);
	}
	if (status & RM_INT_BUS_RESET) {
		rm_drv_sie_status_clr(&hw, RM_SIE_BUS_RESET);
		rm_write(&hw.addr_endp, RM_ALIAS_NORM, 0);
	}
	if (status & RM_INT_ERROR_DATA_SEQ) {
		rm_drv_sie_status_clr(&hw, RM_SIE_DATA_SEQ_ERROR);
	}
	if (status & RM_INT_ERROR_RX_TIMEOUT) {
		rm_drv_sie_status_clr(&hw, RM_SIE_RX_TIMEOUT);
	}
	if (status & RM_INT_ERROR_RX_OVERFLOW) {
		rm_drv_sie_status_clr(&hw, RM_SIE_RX_OVERFLOW);
	}
	if (status & RM_INT_ERROR_BIT_STUFF) {
		rm_drv_sie_status_clr(&hw, RM_SIE_BIT_STUFF_ERROR);
	}
	if (status & RM_INT_ERROR_CRC) {
		rm_drv_sie_status_clr(&hw, RM_SIE_CRC_ERROR);
	}
	if (status & RM_INT_BUFF_STATUS) {
		/* Snapshot-and-acknowledge, exactly as the driver does. */
		uint32_t snapshot = rm_read(&hw.buf_status);

		rm_write(&hw.buf_status, RM_ALIAS_NORM, snapshot);
	}
	if (status & RM_INT_SETUP_REQ) {
		rm_drv_sie_status_clr(&hw, RM_SIE_SETUP_REC);
	}

	zassert_equal(rm_ints(&hw), 0,
		      "after one ISR pass with no new hardware activity the "
		      "interrupt must be fully deasserted; anything left "
		      "asserted means the ISR re-enters immediately and the "
		      "system livelocks (leftover INTS = 0x%08x)",
		      (unsigned int)rm_ints(&hw));
}

/* ======================================================================== */
/* Part 2: deterministic fuzzer                                             */
/* ======================================================================== */

/*
 * Seeded xorshift32. No libc rand(): the sequences must be byte-identical on
 * every host and every libc so that a reported seed is a real reproducer.
 */
struct rm_rng {
	uint32_t s;
};

static uint32_t rm_rand(struct rm_rng *r)
{
	uint32_t x = r->s;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	r->s = x;

	return x;
}

static uint32_t rm_rand_below(struct rm_rng *r, uint32_t n)
{
	return rm_rand(r) % n;
}

/* --- Model of the driver's endpoint + ISR state ------------------------- */

#define RM_FUZZ_EPS 3	/* EP0, EP1, EP2 -- enough for control + a data pair */

struct rm_ep_state {
	bool enabled;
	bool halted;
	bool busy;		/* udc_ep_set_busy() */
	bool queued;		/* a net_buf is in the endpoint queue */
	uint8_t next_pid;	/* driver's software toggle */
	uint8_t host_pid;	/* what the host expects next */
	bool armed;		/* a real transfer is handed to the controller */
	/*
	 * AVAILABLE set purely to arm a STALL handshake response on a halted
	 * OUT endpoint. The buffer is controller-owned but there is no transfer
	 * behind it, so it must not be confused with s->armed -- and a cancel
	 * that sees this AVAIL must not roll back a toggle step that was never
	 * burned.
	 */
	bool stall_armed;
	/*
	 * An IN arm that the host will never accept because a STALL was armed
	 * over the top of it. The toggle is resynchronised by CLEAR_FEATURE.
	 */
	bool stall_pending;
	/*
	 * Set when ep_clear_halt() reset next_pid while a real IN arm was still
	 * live in the controller -- the known driver bug documented at the F2
	 * check and asserted by its own directed test.
	 */
	bool clear_halt_stomped_live_arm;
};

struct rm_model {
	struct rm_hw hw;
	/* [ep][dir], dir 0 = IN, 1 = OUT */
	struct rm_ep_state ep[RM_FUZZ_EPS][2];
	bool suspended;
	uint32_t isr_entries;
};

/* Event kinds the fuzzer can emit. */
enum rm_evt {
	RM_EVT_IRQ,		/* deliver an interrupt with a random bit set */
	RM_EVT_ENQUEUE,
	RM_EVT_DEQUEUE,
	RM_EVT_EP_ENABLE,
	RM_EVT_EP_DISABLE,
	RM_EVT_SET_HALT,
	RM_EVT_CLEAR_HALT,
	RM_EVT_MAX,
};

struct rm_step {
	uint8_t evt;
	uint8_t ep;
	uint8_t dir;
	uint32_t irq_bits;
};

static const char *rm_evt_name(uint8_t e)
{
	switch (e) {
	case RM_EVT_IRQ:        return "IRQ";
	case RM_EVT_ENQUEUE:    return "ep_enqueue";
	case RM_EVT_DEQUEUE:    return "ep_dequeue";
	case RM_EVT_EP_ENABLE:  return "ep_enable";
	case RM_EVT_EP_DISABLE: return "ep_disable";
	case RM_EVT_SET_HALT:   return "ep_set_halt";
	case RM_EVT_CLEAR_HALT: return "ep_clear_halt";
	default:                return "?";
	}
}

/* buf_status bit for (ep, dir): IN at 2n, OUT at 2n+1. */
static uint32_t rm_bs_bit(uint8_t ep, uint8_t dir)
{
	return RM_BIT(ep * 2U + (dir ? 1U : 0U));
}

/* EP_ABORT / EP_ABORT_DONE use the same encoding. */
static uint32_t rm_abort_bit(uint8_t ep, uint8_t dir)
{
	return rm_bs_bit(ep, dir);
}

static struct rm_reg *rm_bc(struct rm_model *m, uint8_t ep, uint8_t dir)
{
	return &m->hw.buf_ctrl[ep][dir];
}

/* --- Modelled driver operations ---------------------------------------- */

/*
 * rpi_pico_prep_rx()/prep_tx(): advance next_pid at QUEUE time and arm the
 * buffer. The toggle advancing here, before the host has seen anything, is the
 * root of invariant F2: every path that cancels the arm owes a rollback.
 */
static void rm_op_prep(struct rm_model *m, uint8_t ep, uint8_t dir)
{
	struct rm_ep_state *s = &m->ep[ep][dir];
	struct rm_reg *bc = rm_bc(m, ep, dir);
	uint32_t word;

	if (rm_read(bc) & RM_BUF_CTRL_AVAIL) {
		return;		/* -EBUSY, driver refuses to re-arm */
	}

	word = 64U;
	word |= s->next_pid ? RM_BUF_CTRL_DATA1_PID : RM_BUF_CTRL_DATA0_PID;
	if (dir == 0) {
		word |= RM_BUF_CTRL_FULL;	/* IN buffers carry data */
	}
	s->next_pid ^= 1U;

	rm_write(bc, RM_ALIAS_NORM, word);
	rm_write(bc, RM_ALIAS_NORM, word | RM_BUF_CTRL_AVAIL);
	s->armed = true;
	s->busy = true;
}

/*
 * rpi_pico_ep_cancel(). Set RM_FUZZ_BUG_NO_PID_ROLLBACK to reintroduce the
 * historical bug where the toggle burned by a cancelled arm was never rolled
 * back; the fuzzer's F2 invariant then fires with a reproducing seed.
 */
static void rm_op_cancel(struct rm_model *m, uint8_t ep, uint8_t dir)
{
	struct rm_ep_state *s = &m->ep[ep][dir];
	struct rm_reg *bc = rm_bc(m, ep, dir);
	const uint32_t mask = rm_abort_bit(ep, dir);
	uint32_t buf_ctrl;

	buf_ctrl = rm_read(bc);
	if (!(buf_ctrl & RM_BUF_CTRL_AVAIL)) {
		/* Buffer not owned by the controller; nothing to undo. */
		s->busy = false;
		return;
	}

	/*
	 * NOTE: the real driver keys its rollback purely off AVAIL, so when
	 * AVAIL was set by the ep_set_halt stall-arm path rather than by
	 * prep_rx()/prep_tx(), it rolls back a toggle step that was never
	 * burned. The model records that distinction explicitly so the
	 * invariant checker can tell the two apart and report the real
	 * asymmetry rather than drowning in it.
	 */
	if (s->stall_armed && !s->armed) {
		rm_write(&m->hw.abort_done, RM_ALIAS_NORM, mask);
		rm_drv_bit_set(&m->hw.abort, mask);
		rm_hw_assert(&m->hw.abort_done, mask);
		rm_write(&m->hw.abort_done, RM_ALIAS_NORM, mask);

		rm_write(bc, RM_ALIAS_NORM, buf_ctrl & ~RM_BUF_CTRL_AVAIL);
		rm_drv_bit_clr(&m->hw.abort, mask);

		s->stall_armed = false;
		s->busy = false;
		return;
	}

	/*
	 * The abort handshake. EP_ABORT_DONE must be pre-cleared (W1C, direct
	 * write) or a stale bit from a previous abort defeats the wait.
	 */
	rm_write(&m->hw.abort_done, RM_ALIAS_NORM, mask);
	rm_drv_bit_set(&m->hw.abort, mask);
	rm_hw_assert(&m->hw.abort_done, mask);	/* SIE completes the handshake */
	rm_write(&m->hw.abort_done, RM_ALIAS_NORM, mask);	/* ack */

	rm_write(bc, RM_ALIAS_NORM, buf_ctrl & ~RM_BUF_CTRL_AVAIL);
	rm_drv_bit_clr(&m->hw.abort, mask);

#ifndef RM_FUZZ_BUG_NO_PID_ROLLBACK
	/* Undo the toggle step burned by the arm the host never saw. */
	s->next_pid ^= 1U;
#endif


	s->armed = false;
	s->busy = false;
}

static void rm_op_enqueue(struct rm_model *m, uint8_t ep, uint8_t dir)
{
	struct rm_ep_state *s = &m->ep[ep][dir];

	if (!s->enabled) {
		return;
	}

	s->queued = true;

	/* udc_rpi_pico_ep_enqueue(): only kicks the thread if not halted. */
	if (!s->halted && !s->busy) {
		rm_op_prep(m, ep, dir);
	}
}

static void rm_op_dequeue(struct rm_model *m, uint8_t ep, uint8_t dir)
{
	struct rm_ep_state *s = &m->ep[ep][dir];

	if (!s->enabled) {
		return;
	}

	rm_op_cancel(m, ep, dir);
	s->queued = false;
}

static void rm_op_ep_enable(struct rm_model *m, uint8_t ep, uint8_t dir)
{
	struct rm_ep_state *s = &m->ep[ep][dir];
	struct rm_reg *bc = rm_bc(m, ep, dir);

	/* udc_rpi_pico_ep_enable(): buf_ctrl = DATA0_PID, next_pid = 0. */
	rm_write(bc, RM_ALIAS_NORM, RM_BUF_CTRL_DATA0_PID);
	s->next_pid = 0;
	s->host_pid = 0;	/* a fresh endpoint resets the host toggle too */
	s->enabled = true;
	s->halted = false;
	s->busy = false;
	s->armed = false;
	s->queued = false;
	s->stall_armed = false;
	s->stall_pending = false;
	s->clear_halt_stomped_live_arm = false;
}

static void rm_op_ep_disable(struct rm_model *m, uint8_t ep, uint8_t dir)
{
	struct rm_ep_state *s = &m->ep[ep][dir];

	if (!s->enabled) {
		return;
	}

	rm_op_cancel(m, ep, dir);
	s->enabled = false;
	s->queued = false;
	s->armed = false;
	s->busy = false;
	s->stall_armed = false;
	s->stall_pending = false;
	s->clear_halt_stomped_live_arm = false;
}

static void rm_op_set_halt(struct rm_model *m, uint8_t ep, uint8_t dir)
{
	struct rm_ep_state *s = &m->ep[ep][dir];
	struct rm_reg *bc = rm_bc(m, ep, dir);

	if (!s->enabled) {
		return;
	}

	if (ep == 0) {
		rm_drv_bit_set(&m->hw.stall_arm,
			       dir ? RM_STALL_ARM_EP0_OUT : RM_STALL_ARM_EP0_IN);
	}

	if (dir) {
		/*
		 * OUT: cancel, then set STALL|AVAIL so the SIE can answer an
		 * OUT DATAx with a STALL handshake. The cancel already rolled
		 * the toggle back and cleared busy/armed.
		 */
		rm_op_cancel(m, ep, dir);
		rm_drv_bit_set(bc, RM_BUF_CTRL_STALL | RM_BUF_CTRL_AVAIL);

		/*
		 * AVAIL is now set, but it arms a stall *response*, not a
		 * transfer. Record that separately: it is not a transfer the
		 * driver is tracking (so s->armed stays false), and a later
		 * cancel must not treat it as one.
		 */
		s->stall_armed = true;
	} else {
		/*
		 * IN: the driver only ORs in STALL. It does NOT cancel, so any
		 * in-flight arm stays live and busy/armed are untouched. The
		 * host will see the STALL instead of the data, so the PID we
		 * armed was never consumed.
		 */
		rm_drv_bit_set(bc, RM_BUF_CTRL_STALL);

		if (s->armed) {
			/*
			 * The armed packet is superseded by the stall, so the
			 * host never accepts that PID. A subsequent
			 * CLEAR_FEATURE(HALT) resets both sides, which is the
			 * legitimate resynchronisation point -- model the host
			 * as still expecting what it expected before the arm.
			 */
			s->stall_pending = true;
		}
	}

	if (ep != 0) {
		s->halted = true;
	}
}

static void rm_op_clear_halt(struct rm_model *m, uint8_t ep, uint8_t dir)
{
	struct rm_ep_state *s = &m->ep[ep][dir];
	struct rm_reg *bc = rm_bc(m, ep, dir);

	if (!s->enabled || ep == 0) {
		return;
	}

	if (dir) {
		rm_op_cancel(m, ep, dir);
		/* Cancel clears AVAIL; the stall response is retired. */
		rm_drv_bit_clr(bc, RM_BUF_CTRL_STALL);
	} else {
		/*
		 * The IN path only clears STALL. It does NOT cancel, so a
		 * transfer armed before the halt is still owned by the
		 * controller when next_pid is reset below. This is the defect
		 * pinned by test_clear_halt_in_stomps_toggle_of_live_arm.
		 */
		rm_drv_bit_clr(bc, RM_BUF_CTRL_STALL);

		if (s->armed) {
			s->clear_halt_stomped_live_arm = true;
		}
	}

	/*
	 * ep_clear_halt() resets the toggle unconditionally. This is the
	 * legitimate resynchronisation point: CLEAR_FEATURE(ENDPOINT_HALT)
	 * resets the host's toggle too, so both sides return to DATA0 and any
	 * stall-superseded arm is forgiven.
	 */
	s->next_pid = 0;
	s->host_pid = 0;
	s->halted = false;
	s->stall_armed = false;
	s->stall_pending = false;

	if (s->queued && !s->busy) {
		rm_op_prep(m, ep, dir);
	}
}

/*
 * Completion of an armed transfer: the host actually saw the PID we armed, so
 * its expectation advances too.
 */
static void rm_op_complete(struct rm_model *m, uint8_t ep, uint8_t dir)
{
	struct rm_ep_state *s = &m->ep[ep][dir];
	struct rm_reg *bc = rm_bc(m, ep, dir);

	if (!s->armed) {
		return;
	}

	/* SIE releases the buffer. */
	rm_write(bc, RM_ALIAS_NORM, rm_read(bc) & ~RM_BUF_CTRL_AVAIL);
	s->armed = false;
	s->busy = false;
	s->queued = false;
	s->stall_pending = false;
	s->host_pid ^= 1U;
}

/*
 * rpi_pico_isr_handler(), modelled. Note that this deliberately handles
 * multiple simultaneously-set status bits in the driver's real order:
 * error bits, then BUFF_STATUS, then SETUP_REQ. Ordering bugs of exactly this
 * shape have already been found here once.
 */
static void rm_op_isr(struct rm_model *m)
{
	uint32_t status = rm_ints(&m->hw);

	m->isr_entries++;

	if (status & RM_INT_DEV_RESUME_FROM_HOST) {
		rm_drv_sie_status_clr(&m->hw, RM_SIE_RESUME);
		m->suspended = false;
	}

	if (status & RM_INT_DEV_SUSPEND) {
		/*
		 * SUSPENDED is RO -- software cannot acknowledge it. The
		 * driver writes the bit anyway (harmless), and the model shows
		 * why suspend must be cross-checked against SOF_RD: the latch
		 * simply does not clear.
		 */
		rm_drv_sie_status_clr(&m->hw, RM_SIE_SUSPENDED);
		m->suspended = true;
		/* The SIE deasserts it when bus activity returns. */
		rm_hw_deassert(&m->hw.sie_status, RM_SIE_SUSPENDED);
	}

	if (status & RM_INT_BUS_RESET) {
		rm_drv_sie_status_clr(&m->hw, RM_SIE_BUS_RESET);
		rm_write(&m->hw.addr_endp, RM_ALIAS_NORM, 0);
	}

	if (status & RM_INT_ERROR_DATA_SEQ) {
		rm_drv_sie_status_clr(&m->hw, RM_SIE_DATA_SEQ_ERROR);
	}
	if (status & RM_INT_ERROR_RX_TIMEOUT) {
		rm_drv_sie_status_clr(&m->hw, RM_SIE_RX_TIMEOUT);
	}
	if (status & RM_INT_ERROR_RX_OVERFLOW) {
		rm_drv_sie_status_clr(&m->hw, RM_SIE_RX_OVERFLOW);
	}
	if (status & RM_INT_ERROR_BIT_STUFF) {
		rm_drv_sie_status_clr(&m->hw, RM_SIE_BIT_STUFF_ERROR);
	}
	if (status & RM_INT_ERROR_CRC) {
		rm_drv_sie_status_clr(&m->hw, RM_SIE_CRC_ERROR);
	}
	if (status & RM_INT_ENDPOINT_ERROR) {
		rm_write(&m->hw.tx_error, RM_ALIAS_NORM,
			 rm_read(&m->hw.tx_error));
		rm_write(&m->hw.rx_error, RM_ALIAS_NORM,
			 rm_read(&m->hw.rx_error));
		rm_drv_sie_status_clr(&m->hw, RM_SIE_ENDPOINT_ERROR);
	}

	/* BUFF_STATUS before SETUP_REQ, so completions are ordered first. */
	if (status & RM_INT_BUFF_STATUS) {
		uint32_t snapshot = rm_read(&m->hw.buf_status);

		/* Snapshot-and-acknowledge up front. */
		rm_write(&m->hw.buf_status, RM_ALIAS_NORM, snapshot);

		for (unsigned int i = 0; i < RM_FUZZ_EPS * 2U; i++) {
			uint8_t ep, dir;

			if (!(snapshot & RM_BIT(i))) {
				continue;
			}

			ep = (uint8_t)(i >> 1U);
			dir = (uint8_t)(i & 1U);
			rm_op_complete(m, ep, dir);
		}
	}

	if (status & RM_INT_SETUP_REQ) {
		rm_drv_sie_status_clr(&m->hw, RM_SIE_SETUP_REC);

		/*
		 * rpi_pico_handle_setup(): cancel both control endpoints, then
		 * force next_pid = 1 on both. The forced value is a legitimate
		 * resynchronisation -- the host also restarts the control
		 * transfer at DATA1 for the data/status stage.
		 */
		rm_op_cancel(m, 0, 0);
		rm_op_cancel(m, 0, 1);
		m->ep[0][0].next_pid = 1;
		m->ep[0][1].next_pid = 1;
		m->ep[0][0].host_pid = 1;
		m->ep[0][1].host_pid = 1;
		m->ep[0][0].queued = false;
		m->ep[0][1].queued = false;
	}
}

/* --- Invariants --------------------------------------------------------- */

struct rm_violation {
	const char *id;
	char detail[192];
	bool hit;
};

static void rm_check_invariants(struct rm_model *m, struct rm_violation *v)
{
	if (v->hit) {
		return;
	}

	/*
	 * F1. No W1C latch may stay set once its handler has run and no new
	 * hardware event has occurred. If one does, the derived interrupt
	 * level never deasserts and the ISR re-enters forever.
	 */
	{
		uint32_t left = rm_ints(&m->hw);

		if (left != 0) {
			v->hit = true;
			v->id = "F1";
			snprintf(v->detail, sizeof(v->detail),
				 "interrupt still asserted after the handler "
				 "ran: INTS=0x%08x (sie_status=0x%08x "
				 "buf_status=0x%08x abort_done=0x%08x)",
				 (unsigned int)left,
				 (unsigned int)m->hw.sie_status.value,
				 (unsigned int)m->hw.buf_status.value,
				 (unsigned int)m->hw.abort_done.value);
			return;
		}
	}

	for (uint8_t ep = 0; ep < RM_FUZZ_EPS; ep++) {
		for (uint8_t dir = 0; dir < 2; dir++) {
			struct rm_ep_state *s = &m->ep[ep][dir];
			struct rm_reg *bc = rm_bc(m, ep, dir);
			bool avail = (rm_read(bc) & RM_BUF_CTRL_AVAIL) != 0;
			bool stalled = (rm_read(bc) & RM_BUF_CTRL_STALL) != 0;

			/*
			 * F2. The driver's toggle and the host's expectation
			 * may not diverge. next_pid advances at queue time, so
			 * every cancel/abort/halt path owes a rollback or an
			 * explicit reset. Divergence is silent and permanent:
			 * the host drops every packet as a duplicate, and a
			 * sequence error raises no BUFF_STATUS bit, so no
			 * completion ever wakes the driver to re-arm.
			 */
			/*
			 * KNOWN BUG, excluded here and asserted explicitly by
			 * test_clear_halt_in_stomps_toggle_of_live_arm below.
			 *
			 * udc_rpi_pico_ep_clear_halt() cancels the endpoint on
			 * the OUT path but NOT on the IN path (it only clears
			 * buf_ctrl.STALL), and then resets next_pid = 0
			 * unconditionally. An IN transfer armed before the halt
			 * is therefore still live in the controller when the
			 * toggle is stomped, and the toggle diverges as soon as
			 * it completes.
			 *
			 * Suppressing it here keeps the fuzzer green for every
			 * OTHER invariant instead of drowning the signal in 61
			 * copies of one known defect. Delete this flag once the
			 * driver cancels on the IN path too, and the fuzzer
			 * regains full coverage of this shape.
			 */
			if (s->clear_halt_stomped_live_arm) {
				continue;
			}

			if (s->enabled && !s->armed && !s->halted &&
			    !s->stall_armed && !s->stall_pending &&
			    s->next_pid != s->host_pid) {
				v->hit = true;
				v->id = "F2";
				snprintf(v->detail, sizeof(v->detail),
					 "ep %u dir %s: data toggle diverged, "
					 "driver next_pid=%u but host expects "
					 "%u -- every subsequent packet is "
					 "discarded as a duplicate and nothing "
					 "recovers it",
					 ep, dir ? "OUT" : "IN",
					 s->next_pid, s->host_pid);
				return;
			}

			/*
			 * F3. An endpoint marked busy must have a buffer
			 * actually handed to the controller.
			 */
			if (s->busy && !avail) {
				v->hit = true;
				v->id = "F3";
				snprintf(v->detail, sizeof(v->detail),
					 "ep %u dir %s: marked busy but "
					 "buf_ctrl=0x%08x has AVAILABLE clear "
					 "-- the controller owns nothing, so "
					 "no completion can arrive to release "
					 "it",
					 ep, dir ? "OUT" : "IN",
					 (unsigned int)rm_read(bc));
				return;
			}

			/*
			 * F4. No state where a buffer is armed for a real
			 * transfer but no completion can ever be delivered.
			 * The stall-response arm (AVAIL set with STALL, on a
			 * halted OUT endpoint) is the one legitimate exception:
			 * it is deliberately armed with no transfer behind it.
			 */
			if (avail && !stalled && !s->armed && !s->stall_armed) {
				v->hit = true;
				v->id = "F4";
				snprintf(v->detail, sizeof(v->detail),
					 "ep %u dir %s: buffer armed in "
					 "hardware (buf_ctrl=0x%08x) but the "
					 "driver has no transfer tracking it, "
					 "so the completion will be dispatched "
					 "with nothing queued to receive it",
					 ep, dir ? "OUT" : "IN",
					 (unsigned int)rm_read(bc));
				return;
			}

			if (s->armed && !s->enabled) {
				v->hit = true;
				v->id = "F4";
				snprintf(v->detail, sizeof(v->detail),
					 "ep %u dir %s: armed while disabled; "
					 "the endpoint control register is "
					 "zeroed, so the completion can never "
					 "be raised",
					 ep, dir ? "OUT" : "IN");
				return;
			}
		}
	}
}

/* --- Sequence generation and replay ------------------------------------- */

#define RM_SEQ_LEN 50

static void rm_gen_step(struct rm_rng *rng, struct rm_step *st)
{
	st->evt = (uint8_t)rm_rand_below(rng, RM_EVT_MAX);
	st->ep = (uint8_t)rm_rand_below(rng, RM_FUZZ_EPS);
	st->dir = (uint8_t)rm_rand_below(rng, 2);
	st->irq_bits = rm_rand(rng);
}

/*
 * Inject the hardware side of an interrupt. Several bits are deliberately
 * allowed to arrive together in the SAME interrupt: an ordering bug of exactly
 * that shape (BUFF_STATUS versus SETUP_REQ) has already been found in this
 * driver.
 */
static void rm_inject_irq(struct rm_model *m, uint32_t bits)
{
	uint32_t sie = 0;

	if (bits & RM_BIT(0)) {
		sie |= RM_SIE_SETUP_REC;
	}
	if (bits & RM_BIT(1)) {
		sie |= RM_SIE_BUS_RESET;
	}
	if (bits & RM_BIT(2)) {
		sie |= RM_SIE_CRC_ERROR;
	}
	if (bits & RM_BIT(3)) {
		sie |= RM_SIE_BIT_STUFF_ERROR;
	}
	if (bits & RM_BIT(4)) {
		sie |= RM_SIE_DATA_SEQ_ERROR;
	}
	if (bits & RM_BIT(5)) {
		sie |= RM_SIE_RX_TIMEOUT;
	}
	if (bits & RM_BIT(6)) {
		sie |= RM_SIE_RX_OVERFLOW;
	}
	if (bits & RM_BIT(7)) {
		sie |= RM_SIE_RESUME;
	}
	if (bits & RM_BIT(8)) {
		sie |= RM_SIE_SUSPENDED;
	}
	if (bits & RM_BIT(9)) {
		sie |= RM_SIE_ENDPOINT_ERROR;
		rm_hw_assert(&m->hw.rx_error, RM_BIT(0));
	}

	rm_hw_assert(&m->hw.sie_status, sie);

	/*
	 * BUFF_STATUS: raise completion bits only for endpoints that actually
	 * have a buffer armed. The controller cannot complete a transfer that
	 * was never handed to it, and modelling otherwise would manufacture
	 * violations that no silicon can produce.
	 */
	for (uint8_t ep = 0; ep < RM_FUZZ_EPS; ep++) {
		for (uint8_t dir = 0; dir < 2; dir++) {
			if (!m->ep[ep][dir].armed) {
				continue;
			}
			if (bits & RM_BIT(16 + ep * 2 + dir)) {
				rm_hw_assert(&m->hw.buf_status,
					     rm_bs_bit(ep, dir));
			}
		}
	}
}

static void rm_model_init(struct rm_model *m)
{
	memset(m, 0, sizeof(*m));
	rm_hw_reset(&m->hw);

	/* Driver enables interrupts exactly as udc_rpi_pico_enable() does. */
	rm_write(&m->hw.inte, RM_ALIAS_NORM,
		 RM_INT_SETUP_REQ | RM_INT_DEV_RESUME_FROM_HOST |
			 RM_INT_DEV_SUSPEND | RM_INT_DEV_CONN_DIS |
			 RM_INT_BUS_RESET | RM_INT_VBUS_DETECT |
			 RM_INT_ERROR_CRC | RM_INT_ERROR_BIT_STUFF |
			 RM_INT_ERROR_RX_OVERFLOW | RM_INT_ERROR_RX_TIMEOUT |
			 RM_INT_ERROR_DATA_SEQ | RM_INT_BUFF_STATUS |
			 RM_INT_ENDPOINT_ERROR);

	/* Control endpoints come up enabled. */
	rm_op_ep_enable(m, 0, 0);
	rm_op_ep_enable(m, 0, 1);
}

static void rm_apply(struct rm_model *m, const struct rm_step *st)
{
	switch (st->evt) {
	case RM_EVT_IRQ:
		rm_inject_irq(m, st->irq_bits);
		rm_op_isr(m);
		break;
	case RM_EVT_ENQUEUE:
		rm_op_enqueue(m, st->ep, st->dir);
		break;
	case RM_EVT_DEQUEUE:
		rm_op_dequeue(m, st->ep, st->dir);
		break;
	case RM_EVT_EP_ENABLE:
		rm_op_ep_enable(m, st->ep, st->dir);
		break;
	case RM_EVT_EP_DISABLE:
		rm_op_ep_disable(m, st->ep, st->dir);
		break;
	case RM_EVT_SET_HALT:
		rm_op_set_halt(m, st->ep, st->dir);
		break;
	case RM_EVT_CLEAR_HALT:
		rm_op_clear_halt(m, st->ep, st->dir);
		break;
	default:
		break;
	}
}

/*
 * Replay the first n steps of a sequence and report whether an invariant
 * broke. Used both for the main sweep and for shrinking.
 */
static bool rm_replay(const struct rm_step *seq, unsigned int n,
		      struct rm_violation *out)
{
	struct rm_model m;
	struct rm_violation v = { 0 };

	rm_model_init(&m);

	for (unsigned int i = 0; i < n; i++) {
		rm_apply(&m, &seq[i]);
		rm_check_invariants(&m, &v);
		if (v.hit) {
			if (out) {
				*out = v;
			}
			return true;
		}
	}

	if (out) {
		*out = v;
	}

	return false;
}

static void rm_print_seq(const struct rm_step *seq, unsigned int n)
{
	for (unsigned int i = 0; i < n; i++) {
		if (seq[i].evt == RM_EVT_IRQ) {
			printf("      %2u: IRQ bits=0x%08x\n", i,
			       (unsigned int)seq[i].irq_bits);
		} else {
			printf("      %2u: %s(ep=%u, %s)\n", i,
			       rm_evt_name(seq[i].evt), seq[i].ep,
			       seq[i].dir ? "OUT" : "IN");
		}
	}
}

/*
 * Delta-debugging shrink: drop each step in turn and keep the drop if the
 * violation still reproduces. Cheap, and it turns a 50-step sequence into the
 * two or three steps that actually matter.
 */
static unsigned int rm_shrink(struct rm_step *seq, unsigned int n)
{
	bool progress = true;

	while (progress && n > 1) {
		progress = false;

		for (unsigned int i = 0; i < n; i++) {
			struct rm_step trial[RM_SEQ_LEN];
			unsigned int k = 0;

			for (unsigned int j = 0; j < n; j++) {
				if (j != i) {
					trial[k++] = seq[j];
				}
			}

			if (rm_replay(trial, k, NULL)) {
				memcpy(seq, trial, k * sizeof(trial[0]));
				n = k;
				progress = true;
				break;
			}
		}
	}

	return n;
}

#define RM_N_SEEDS 2000

ZTEST(usb_regmodel, test_fuzz_isr_and_endpoint_state)
{
	unsigned int failures = 0;
	unsigned int steps_run = 0;

	for (uint32_t seed = 1; seed <= RM_N_SEEDS; seed++) {
		struct rm_step seq[RM_SEQ_LEN];
		struct rm_violation v = { 0 };
		struct rm_rng rng = { .s = seed };

		for (unsigned int i = 0; i < RM_SEQ_LEN; i++) {
			rm_gen_step(&rng, &seq[i]);
		}

		steps_run += RM_SEQ_LEN;

		if (!rm_replay(seq, RM_SEQ_LEN, &v)) {
			continue;
		}

		if (failures == 0) {
			unsigned int n;

			printf("    INVARIANT %s VIOLATED, seed=%u\n", v.id,
			       (unsigned int)seed);
			printf("      %s\n", v.detail);

			n = rm_shrink(seq, RM_SEQ_LEN);
			printf("      minimal reproducing sequence (%u "
			       "steps):\n", n);
			rm_print_seq(seq, n);
		}

		failures++;
	}

	zassert_true(steps_run == RM_N_SEEDS * RM_SEQ_LEN,
		     "the fuzzer must actually have run: %u steps", steps_run);

	zassert_equal(failures, 0,
		      "%u of %u fuzz sequences violated a driver invariant; "
		      "the first failing seed and its minimal reproducer are "
		      "printed above",
		      failures, RM_N_SEEDS);
}

/*
 * A directed counterpart to the random sweep: the specific shape that has
 * already bitten this driver, where BUFF_STATUS and SETUP_REQ are set in the
 * same interrupt. Completions must be dispatched before the setup handler
 * cancels the control endpoints, or the completion is lost.
 */
ZTEST(usb_regmodel, test_simultaneous_buff_status_and_setup_req)
{
	struct rm_model m;
	struct rm_violation v = { 0 };

	rm_model_init(&m);

	/* Arm EP1 IN with a real transfer. */
	rm_op_ep_enable(&m, 1, 0);
	rm_op_enqueue(&m, 1, 0);
	zassert_true(m.ep[1][0].armed, "EP1 IN should be armed");

	/* One interrupt carrying both a completion and a new setup packet. */
	rm_hw_assert(&m.hw.buf_status, rm_bs_bit(1, 0));
	rm_hw_assert(&m.hw.sie_status, RM_SIE_SETUP_REC);

	rm_op_isr(&m);

	zassert_equal(rm_ints(&m.hw), 0,
		      "both latches must be acknowledged in a single pass; "
		      "leftover INTS=0x%08x means the ISR re-enters forever",
		      (unsigned int)rm_ints(&m.hw));
	zassert_false(m.ep[1][0].armed,
		      "the EP1 IN completion must be dispatched even though a "
		      "SETUP arrived in the same interrupt -- handling SETUP "
		      "first would cancel the control endpoints and leave this "
		      "completion unserviced");

	rm_check_invariants(&m, &v);
	zassert_false(v.hit, "invariant %s: %s", v.id ? v.id : "?", v.detail);
}

/*
 * Directed check of the toggle-rollback contract that F2 generalises: arm a
 * transfer, cancel it, and confirm the driver's next_pid returns to what the
 * host still expects.
 */
ZTEST(usb_regmodel, test_cancel_rolls_back_data_toggle)
{
	struct rm_model m;

	rm_model_init(&m);
	rm_op_ep_enable(&m, 1, 1);

	zassert_equal(m.ep[1][1].next_pid, m.ep[1][1].host_pid,
		      "a freshly enabled endpoint starts synchronised");

	rm_op_enqueue(&m, 1, 1);
	zassert_not_equal(m.ep[1][1].next_pid, m.ep[1][1].host_pid,
			  "queueing burns a toggle step before the host has "
			  "seen anything");

	rm_op_dequeue(&m, 1, 1);
	zassert_equal(m.ep[1][1].next_pid, m.ep[1][1].host_pid,
		      "cancelling must roll the toggle back; without this the "
		      "host silently discards every subsequent packet as a "
		      "duplicate, and because a sequence error raises no "
		      "BUFF_STATUS bit there is no completion to trigger "
		      "recovery");
}

/* Repeated arm/cancel must not accumulate toggle drift. */
ZTEST(usb_regmodel, test_repeated_cancel_does_not_drift_toggle)
{
	struct rm_model m;

	rm_model_init(&m);
	rm_op_ep_enable(&m, 2, 0);

	for (int i = 0; i < 16; i++) {
		rm_op_enqueue(&m, 2, 0);
		rm_op_dequeue(&m, 2, 0);
	}

	zassert_equal(m.ep[2][0].next_pid, m.ep[2][0].host_pid,
		      "16 arm/cancel cycles must leave the toggle where it "
		      "started; an off-by-one per cycle would be invisible "
		      "until the sixteenth transfer");
}

/*
 * NEW FINDING, reported by the fuzzer before it was suppressed in the F2 check.
 *
 * udc_rpi_pico_ep_clear_halt() is asymmetric between directions:
 *
 *     if (USB_EP_DIR_IS_OUT(cfg->addr)) {
 *             rpi_pico_ep_cancel(dev, cfg->addr);   <-- tears down the arm
 *     } else {                                       and rolls the toggle back
 *             rpi_pico_bit_clr(buf_ctrl_reg, USB_BUF_CTRL_STALL);
 *     }
 *
 *     ep_data->next_pid = 0;                        <-- unconditional
 *
 * On the OUT path the cancel quiesces the endpoint first, so resetting next_pid
 * is safe. On the IN path nothing is cancelled: a transfer armed before the
 * halt is still owned by the controller, with its PID already committed in
 * buf_ctrl, when next_pid is stomped to 0. When that transfer completes the
 * host's toggle advances and the driver's does not, so the two diverge
 * permanently -- and because a sequence error sets SIE_STATUS.DATA_SEQ_ERROR
 * but raises no BUFF_STATUS bit, no completion is ever delivered to trigger
 * a re-arm. The endpoint goes silent.
 *
 * Reached by the fuzzer from 61 of 2000 seeds; first at seed 39 with the
 * 4-step reproducer ep_enable(1,IN), ep_enqueue(1,IN), ep_clear_halt(1,IN),
 * ep_dequeue(1,IN).
 *
 * This test pins the CURRENT (buggy) behaviour so the finding is not lost.
 * When the driver is fixed -- by cancelling on the IN path too, or by only
 * resetting next_pid when the endpoint is actually idle -- this test will
 * fail, and the fix is to invert the assertion and delete the
 * clear_halt_stomped_live_arm suppression in rm_check_invariants().
 */
ZTEST(usb_regmodel, test_clear_halt_in_stomps_toggle_of_live_arm)
{
	struct rm_model m;

	rm_model_init(&m);
	rm_op_ep_enable(&m, 1, 0);	/* EP1 IN */

	/* Arm a transfer; the toggle step is burned at queue time. */
	rm_op_enqueue(&m, 1, 0);
	zassert_true(m.ep[1][0].armed, "EP1 IN must be armed");
	zassert_equal(rm_read(rm_bc(&m, 1, 0)) & RM_BUF_CTRL_AVAIL,
		      RM_BUF_CTRL_AVAIL, "the controller owns the buffer");

	/* Halt it. The IN path does not cancel, so the arm stays live. */
	rm_op_set_halt(&m, 1, 0);
	zassert_true(m.ep[1][0].armed,
		     "ep_set_halt() on an IN endpoint does not cancel, so the "
		     "previously armed transfer is still owned by the SIE");

	/*
	 * Clear the halt. The IN path now cancels first, so the live arm is
	 * quiesced before the toggle is reset.
	 */
	rm_op_clear_halt(&m, 1, 0);

	zassert_true(m.ep[1][0].armed,
		     "ep_clear_halt() on an IN endpoint still does not cancel, "
		     "so the arm placed before the halt is still live");
	zassert_true(m.ep[1][0].clear_halt_stomped_live_arm,
		     "KNOWN BUG pinned: next_pid is reset while a committed arm "
		     "is in flight. If this assertion starts failing the driver "
		     "has been fixed -- invert both assertions and remove the "
		     "suppression in rm_check_invariants()");

	zassert_equal(m.ep[1][0].next_pid, 0,
		      "the driver now believes the next PID is DATA0");

	/* The in-flight transfer completes: the host's toggle advances. */
	rm_hw_assert(&m.hw.buf_status, rm_bs_bit(1, 0));
	rm_op_isr(&m);

	zassert_equal(m.ep[1][0].host_pid, 1,
		      "the host consumed the packet, so it now expects DATA1");
	zassert_not_equal(m.ep[1][0].next_pid, m.ep[1][0].host_pid,
			  "KNOWN BUG pinned: the driver will arm DATA0 while "
			  "the host expects DATA1, so every subsequent IN "
			  "packet is discarded as a duplicate. If this "
			  "assertion starts failing the driver has been fixed "
			  "-- invert it and remove the suppression in "
			  "rm_check_invariants()");
}

ZTEST_SUITE(usb_regmodel, NULL, NULL, NULL, NULL, NULL);
