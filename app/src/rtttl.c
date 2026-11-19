// SPDX-License-Identifier: Apache-2.0

//
// Copyright (c) 2026 Blue Clover Devices
//

/* rtttl.c - RTTTL parser. See rtttl.h for the accepted grammar. */

#include <errno.h>
#include <stdbool.h>

#include "rtttl.h"

#define DEFAULT_DURATION 4U
#define DEFAULT_OCTAVE   6U
#define DEFAULT_BPM      63U

#define MIN_OCTAVE 1U
#define MAX_OCTAVE 8U
#define MIN_BPM    25U
#define MAX_BPM    900U

/*
 * Octave-8 pitches in centi-Hz (C8 = 4186.01 Hz ... B8 = 7902.13 Hz), indexed
 * by semitone above C. Lower octaves are derived by halving, which keeps
 * rounding error below 0.5 Hz across the whole range.
 */
static const uint32_t octave8_centihz[12] = {
	418601, 443492, 469864, 497803, 527404, 558765,
	591991, 627193, 664488, 704000, 745862, 790213,
};

struct cursor {
	const char *base;
	const char *p;
};

static void skip_ws(struct cursor *c)
{
	while (*c->p == ' ' || *c->p == '\t' || *c->p == '\r' || *c->p == '\n') {
		c->p++;
	}
}

static char lower(char ch)
{
	return (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
}

static bool is_digit(char ch)
{
	return ch >= '0' && ch <= '9';
}

static int fail(const struct cursor *c, struct rtttl_error *err, const char *msg)
{
	if (err != NULL) {
		err->pos = (size_t)(c->p - c->base);
		err->msg = msg;
	}
	return -EINVAL;
}

/* Read an unsigned decimal number. Returns false if there are no digits. */
static bool read_uint(struct cursor *c, uint32_t *out)
{
	uint32_t v = 0;

	if (!is_digit(*c->p)) {
		return false;
	}
	while (is_digit(*c->p)) {
		/* Saturate rather than overflow; range checks reject it later. */
		if (v < 100000U) {
			v = v * 10U + (uint32_t)(*c->p - '0');
		}
		c->p++;
	}
	*out = v;
	return true;
}

static bool valid_duration(uint32_t d)
{
	return d == 1U || d == 2U || d == 4U || d == 8U || d == 16U || d == 32U;
}

/* Semitone above C for a note letter, or -1 for a rest, -2 if invalid. */
static int note_semitone(char ch)
{
	switch (lower(ch)) {
	case 'c': return 0;
	case 'd': return 2;
	case 'e': return 4;
	case 'f': return 5;
	case 'g': return 7;
	case 'a': return 9;
	case 'b': /* fall through */
	case 'h': return 11;
	case 'p': return -1;
	default:  return -2;
	}
}

static uint16_t note_freq(int semitone, uint32_t octave)
{
	uint32_t shift = MAX_OCTAVE - octave;
	uint32_t centihz = octave8_centihz[semitone];

	if (shift > 0U) {
		centihz = (centihz + (1U << (shift - 1U))) >> shift;
	}
	return (uint16_t)((centihz + 50U) / 100U);
}

static int parse_defaults(struct cursor *c, struct rtttl_error *err,
			  uint32_t *dur, uint32_t *oct, uint32_t *bpm)
{
	skip_ws(c);
	if (*c->p == ':') {
		return 0; /* empty defaults section */
	}

	for (;;) {
		char key;
		uint32_t v;

		skip_ws(c);
		key = lower(*c->p);
		if (key != 'd' && key != 'o' && key != 'b') {
			return fail(c, err, "expected d=, o= or b= in defaults");
		}
		c->p++;
		skip_ws(c);
		if (*c->p != '=') {
			return fail(c, err, "expected '=' in defaults");
		}
		c->p++;
		skip_ws(c);
		if (!read_uint(c, &v)) {
			return fail(c, err, "expected a number in defaults");
		}

		if (key == 'd') {
			if (!valid_duration(v)) {
				return fail(c, err, "d must be 1, 2, 4, 8, 16 or 32");
			}
			*dur = v;
		} else if (key == 'o') {
			if (v < MIN_OCTAVE || v > MAX_OCTAVE) {
				return fail(c, err, "o must be 1-8");
			}
			*oct = v;
		} else {
			if (v < MIN_BPM || v > MAX_BPM) {
				return fail(c, err, "b must be 25-900");
			}
			*bpm = v;
		}

		skip_ws(c);
		if (*c->p == ',') {
			c->p++;
			continue;
		}
		if (*c->p == ':') {
			return 0;
		}
		return fail(c, err, "expected ',' or ':' after a default");
	}
}

int rtttl_parse(const char *str, struct rtttl_note *notes, size_t max_notes,
		struct rtttl_error *err)
{
	struct cursor c = { .base = str, .p = str };
	uint32_t def_dur = DEFAULT_DURATION;
	uint32_t def_oct = DEFAULT_OCTAVE;
	uint32_t bpm = DEFAULT_BPM;
	uint32_t whole_ms;
	size_t count = 0;
	int rc;

	if (str == NULL || notes == NULL || max_notes == 0U) {
		return fail(&c, err, "no input");
	}

	/* Name: everything up to the first ':'. */
	while (*c.p != '\0' && *c.p != ':') {
		c.p++;
	}
	if (*c.p != ':') {
		return fail(&c, err, "missing ':' after name");
	}
	c.p++;

	rc = parse_defaults(&c, err, &def_dur, &def_oct, &bpm);
	if (rc) {
		return rc;
	}
	c.p++; /* the ':' before the notes */

	whole_ms = (60000U * 4U) / bpm;

	for (;;) {
		uint32_t dur = def_dur;
		uint32_t oct = def_oct;
		uint32_t ms;
		bool dotted = false;
		int semi;

		skip_ws(&c);
		if (*c.p == '\0') {
			return (count == 0U) ? fail(&c, err, "no notes") :
					       fail(&c, err, "trailing ','");
		}

		if (is_digit(*c.p)) {
			if (!read_uint(&c, &dur) || !valid_duration(dur)) {
				return fail(&c, err,
					    "duration must be 1, 2, 4, 8, 16 or 32");
			}
		}

		semi = note_semitone(*c.p);
		if (semi == -2) {
			return fail(&c, err, "expected a note (a-g, h or p)");
		}
		c.p++;

		if (*c.p == '#') {
			if (semi < 0) {
				return fail(&c, err, "a pause cannot be sharp");
			}
			semi++;
			c.p++;
		}
		if (*c.p == '.') {
			dotted = true;
			c.p++;
		}
		if (is_digit(*c.p)) {
			(void)read_uint(&c, &oct);
			if (oct < MIN_OCTAVE || oct > MAX_OCTAVE) {
				return fail(&c, err, "octave must be 1-8");
			}
		}
		if (*c.p == '.' && !dotted) {
			dotted = true;
			c.p++;
		}

		/* b# wraps to c of the next octave. */
		if (semi == 12) {
			semi = 0;
			if (oct == MAX_OCTAVE) {
				return fail(&c, err, "note above b8");
			}
			oct++;
		}

		ms = whole_ms / dur;
		if (dotted) {
			ms += ms / 2U;
		}

		if (count == max_notes) {
			if (err != NULL) {
				err->pos = (size_t)(c.p - c.base);
				err->msg = "too many notes";
			}
			return -E2BIG;
		}
		notes[count].freq_hz = (semi < 0) ? 0U : note_freq(semi, oct);
		notes[count].duration_ms = (uint16_t)ms;
		count++;

		skip_ws(&c);
		if (*c.p == '\0') {
			return (int)count;
		}
		if (*c.p != ',') {
			return fail(&c, err, "expected ',' between notes");
		}
		c.p++;
	}
}
