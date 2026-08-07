/*
 * Copyright (c) 2026 Blue Clover Devices
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal ztest shim so the USB regression tests also build and run as a
 * plain host binary.
 *
 * The tests are self-contained logic over integers and flags -- no kernel, no
 * drivers -- so they do not need Zephyr to be meaningful. native_sim requires
 * Linux, and this project is developed on macOS, so without this shim the
 * tests could not be run locally at all.
 *
 * Under Zephyr the real ztest is used; this header is only pulled in when
 * building with HOST_TEST defined.
 */

#ifndef USB_REGRESSION_HOST_SHIM_H_
#define USB_REGRESSION_HOST_SHIM_H_

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#ifndef BIT
#define BIT(n) (1UL << (n))
#endif

static int host_tests_run;
static int host_tests_failed;
static const char *host_current_test;
static int host_current_failed;

#define ZTEST_SUITE(name, ...) /* no-op on host */

#define ZTEST(suite, name)                                                     \
	static void suite##_##name(void);                                      \
	static void run_##suite##_##name(void)                                 \
	{                                                                      \
		host_current_test = #suite "::" #name;                         \
		host_current_failed = 0;                                       \
		host_tests_run++;                                              \
		suite##_##name();                                              \
		if (host_current_failed) {                                     \
			host_tests_failed++;                                   \
			printf("  FAIL %s\n", host_current_test);              \
		} else {                                                       \
			printf("  ok   %s\n", host_current_test);              \
		}                                                              \
	}                                                                      \
	static void suite##_##name(void)

#define HOST_FAIL(fmt, ...)                                                    \
	do {                                                                   \
		host_current_failed = 1;                                       \
		printf("    %s:%d: " fmt "\n", __FILE__, __LINE__,             \
		       ##__VA_ARGS__);                                         \
	} while (0)

#define zassert_equal(a, b, msg, ...)                                          \
	do {                                                                   \
		long _a = (long)(a), _b = (long)(b);                           \
		if (_a != _b) {                                                \
			HOST_FAIL("expected %ld == %ld: " msg, _a, _b,         \
				  ##__VA_ARGS__);                              \
		}                                                              \
	} while (0)

#define zassert_not_equal(a, b, msg, ...)                                      \
	do {                                                                   \
		long _a = (long)(a), _b = (long)(b);                           \
		if (_a == _b) {                                                \
			HOST_FAIL("expected %ld != %ld: " msg, _a, _b,         \
				  ##__VA_ARGS__);                              \
		}                                                              \
	} while (0)

#define zassert_true(c, msg, ...)                                              \
	do {                                                                   \
		if (!(c)) {                                                    \
			HOST_FAIL("expected true: " msg, ##__VA_ARGS__);       \
		}                                                              \
	} while (0)

#define zassert_false(c, msg, ...)                                             \
	do {                                                                   \
		if (c) {                                                       \
			HOST_FAIL("expected false: " msg, ##__VA_ARGS__);      \
		}                                                              \
	} while (0)

#endif /* USB_REGRESSION_HOST_SHIM_H_ */
