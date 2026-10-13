/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Fake for the host tests: the test file defines these functions. */
#ifndef FAKE_DWT_H
#define FAKE_DWT_H
#include <stdbool.h>
#include <stdint.h>

bool dwt_enable_cycle_counter(void);
uint32_t dwt_read_cycle_counter(void);
#endif
