// SPDX-License-Identifier: Apache-2.0

//
// Copyright (c) 2026 Blue Clover Devices
//

/*
 * Host unit tests for app/src/rtttl.c. Build and run with `make test`.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "rtttl.h"

static int failures;
static int checks;

#define CHECK(cond)                                                          \
	do {                                                                 \
		checks++;                                                    \
		if (!(cond)) {                                               \
			failures++;                                          \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                            \
	} while (0)

#define MAX_NOTES 256
static struct rtttl_note notes[MAX_NOTES];

static int parse(const char *s, struct rtttl_error *err)
{
	memset(notes, 0, sizeof(notes));
	return rtttl_parse(s, notes, MAX_NOTES, err);
}

static void test_twinkle(void)
{
	/* The acceptance-criteria tune. */
	int n = parse("twinkle:d=4,o=5,b=120:c,c,g,g,a,a,2g,f,f,e,e,d,d,2c",
		      NULL);

	CHECK(n == 14);
	CHECK(notes[0].freq_hz == 523 && notes[0].duration_ms == 500);
	CHECK(notes[2].freq_hz == 784);
	CHECK(notes[4].freq_hz == 880);
	CHECK(notes[6].freq_hz == 784 && notes[6].duration_ms == 1000);
	CHECK(notes[7].freq_hz == 698);
	CHECK(notes[9].freq_hz == 659);
	CHECK(notes[11].freq_hz == 587);
	CHECK(notes[13].freq_hz == 523 && notes[13].duration_ms == 1000);
}

static void test_ode(void)
{
	/* Built-in "ode" tune from app_shell.c. */
	int n = parse("ode:d=4,o=5,b=140:e,e,f,g,g,f,e,d,c,c,d,e,e.,8d,2d",
		      NULL);

	CHECK(n == 15);
	CHECK(notes[0].freq_hz == 659 && notes[0].duration_ms == 428);
	CHECK(notes[12].duration_ms == 642);
	CHECK(notes[13].freq_hz == 587 && notes[13].duration_ms == 214);
	CHECK(notes[14].duration_ms == 857);
}

static void test_defaults(void)
{
	/* No defaults: d=4, o=6, b=63. */
	int n = parse("x::a", NULL);

	CHECK(n == 1);
	CHECK(notes[0].freq_hz == 1760);
	CHECK(notes[0].duration_ms == 240000 / 63 / 4);

	/* Defaults in any order, partial set, whitespace, upper case. */
	n = parse(" My Tune : B = 120 , O = 4 : 8A , C5 ", NULL);
	CHECK(n == 2);
	CHECK(notes[0].freq_hz == 440 && notes[0].duration_ms == 250);
	CHECK(notes[1].freq_hz == 523 && notes[1].duration_ms == 500);

	/* Empty name. */
	CHECK(parse(":d=8:c", NULL) == 1);
}

static void test_note_forms(void)
{
	int n = parse("t:d=4,o=5,b=120:c#,d#6,a#.,a#6.,a.6,p,8p.,h,b#,e#", NULL);

	CHECK(n == 10);
	CHECK(notes[0].freq_hz == 554);                  /* c#5 */
	CHECK(notes[1].freq_hz == 1245);                 /* d#6 */
	CHECK(notes[2].freq_hz == 932 && notes[2].duration_ms == 750);
	CHECK(notes[3].freq_hz == 1865 && notes[3].duration_ms == 750);
	CHECK(notes[4].freq_hz == 1760 && notes[4].duration_ms == 750);
	CHECK(notes[5].freq_hz == 0 && notes[5].duration_ms == 500);
	CHECK(notes[6].freq_hz == 0 && notes[6].duration_ms == 375);
	CHECK(notes[7].freq_hz == 988);                  /* h = b */
	CHECK(notes[8].freq_hz == 1047);                 /* b#5 = c6 */
	CHECK(notes[9].freq_hz == 698);                  /* e#5 = f5 */
}

static void test_range(void)
{
	CHECK(parse("t:o=1:c", NULL) == 1 && notes[0].freq_hz == 33);
	CHECK(parse("t:o=8:b", NULL) == 1 && notes[0].freq_hz == 7902);
	CHECK(parse("t:b=900:32c", NULL) == 1 && notes[0].duration_ms == 8);
	CHECK(parse("t:b=25:1c.", NULL) == 1 &&
	      notes[0].duration_ms == 14400);
}

static void expect_bad(const char *s, size_t pos)
{
	struct rtttl_error err = { 0 };
	int n = parse(s, &err);

	CHECK(n == -EINVAL);
	CHECK(err.msg != NULL);
	if (err.pos != pos) {
		failures++;
		printf("FAIL \"%s\": error at %zu, expected %zu (%s)\n", s,
		       err.pos, pos, err.msg ? err.msg : "?");
	}
	checks++;
}

static void test_malformed(void)
{
	expect_bad("", 0);
	expect_bad("no colons", 9);
	expect_bad("t:d=4", 5);           /* missing notes section */
	expect_bad("t:d=4:", 6);          /* no notes */
	expect_bad("t:x=4:c", 2);         /* unknown default */
	expect_bad("t:d4:c", 3);          /* missing '=' */
	expect_bad("t:d=:c", 4);          /* missing number */
	expect_bad("t:d=3:c", 5);         /* bad default duration */
	expect_bad("t:o=9:c", 5);
	expect_bad("t:o=0:c", 5);
	expect_bad("t:b=10:c", 6);
	expect_bad("t:b=901:c", 7);
	expect_bad("t:b=99999999999:c", 15);
	expect_bad("t:d=4;o=5:c", 5);     /* wrong separator */
	expect_bad("t::c,", 5);           /* trailing comma */
	expect_bad("t::c,,d", 5);
	expect_bad("t::x", 3);            /* bad note letter */
	expect_bad("t::3c", 4);           /* bad note duration */
	expect_bad("t::p#", 4);           /* sharp pause */
	expect_bad("t::c9", 5);           /* bad octave */
	expect_bad("t::c d", 5);          /* missing comma */
	expect_bad("t::c..", 5);          /* double dot */
	expect_bad("t:o=8:b#", 8);        /* above b8 */
}

static void test_capacity(void)
{
	struct rtttl_note small[3];
	struct rtttl_error err = { 0 };

	CHECK(rtttl_parse("t::c,d,e", small, 3, &err) == 3);
	CHECK(rtttl_parse("t::c,d,e,f", small, 3, &err) == -E2BIG);
	CHECK(rtttl_parse("t::c", small, 0, &err) == -EINVAL);
	CHECK(rtttl_parse(NULL, small, 3, &err) == -EINVAL);
}

static void test_long_tune(void)
{
	/* The longest tune of "c," notes that fits a 512-byte command line. */
	static char buf[512];
	const char *prefix = "bento buzzer play \"t::";
	size_t len = strlen(prefix);
	int expect = 0;

	memcpy(buf, prefix, len);
	while (len + 3 < sizeof(buf) - 1) {
		buf[len++] = 'c';
		buf[len++] = ',';
		expect++;
	}
	buf[len++] = 'c';
	expect++;
	buf[len] = '\0';

	CHECK(parse(buf + strlen("bento buzzer play \""), NULL) == expect);
	CHECK(expect <= MAX_NOTES);
	printf("longest command-line tune: %d notes\n", expect);
}

int main(void)
{
	test_twinkle();
	test_ode();
	test_defaults();
	test_note_forms();
	test_range();
	test_malformed();
	test_capacity();
	test_long_tune();

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
