/* SPDX-License-Identifier: Apache-2.0 */

//
// Copyright (c) 2026 Blue Clover Devices
//

/*
 * rtttl.h - RTTTL (Ring Tone Text Transfer Language) parser.
 *
 * Pure C with no Zephyr dependencies, so it can be unit-tested on the host
 * (see tests/rtttl).
 *
 * Format:  <name>:<defaults>:<notes>
 *   name      Any text without ':' (may be empty).
 *   defaults  Comma-separated d=<duration>, o=<octave>, b=<bpm>, in any
 *             order, any subset. Missing values use d=4, o=6, b=63.
 *   notes     Comma-separated [duration]<note>[#][.][octave][.]
 *             duration  1, 2, 4, 8, 16 or 32 (fraction of a whole note)
 *             note      a-g, h (= b), or p (pause/rest)
 *             #         sharp
 *             .         dotted (1.5x length); accepted before or after the
 *                       octave
 *             octave    1-8; c5 is 523 Hz (scientific pitch notation)
 *
 * Whitespace between tokens is ignored, and letters are case-insensitive.
 */

#ifndef RTTTL_H_
#define RTTTL_H_

#include <stddef.h>
#include <stdint.h>

/* One parsed note. freq_hz == 0 means a rest. */
struct rtttl_note {
	uint16_t freq_hz;
	uint16_t duration_ms;
};

/* Where and why a parse failed. pos is a byte offset into the input. */
struct rtttl_error {
	size_t pos;
	const char *msg;
};

/*
 * @brief Parse an RTTTL string into a note array.
 *
 * The whole string is validated before this returns, so a caller can reject
 * a malformed tune without playing any of it.
 *
 * @param str        NUL-terminated RTTTL string.
 * @param notes      Output array.
 * @param max_notes  Capacity of @p notes.
 * @param err        Optional; filled in on failure.
 * @return Number of notes (>= 1) on success, or a negative errno:
 *         -EINVAL for a malformed string, -E2BIG if it has more than
 *         @p max_notes notes.
 */
int rtttl_parse(const char *str, struct rtttl_note *notes, size_t max_notes,
		struct rtttl_error *err);

#endif /* RTTTL_H_ */
