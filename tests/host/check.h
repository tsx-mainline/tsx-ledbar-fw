/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The check macro of the host tests: one "ok" or "FAIL" line for each check. */
#ifndef CHECK_H
#define CHECK_H
#include <stdio.h>

static int failures;

#define CHECK(cond, ...) do {						\
	int ok_ = (cond) ? 1 : 0;					\
	printf("%s ", ok_ ? "ok  " : "FAIL");				\
	printf(__VA_ARGS__);						\
	printf("\n");							\
	if (!ok_)							\
		failures++;						\
} while (0)

#define DONE(name) do {							\
	printf("%s: %d failures\n", name, failures);			\
	return failures ? 1 : 0;					\
} while (0)
#endif
