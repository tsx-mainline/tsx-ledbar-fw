/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Fake for the host tests: the test file defines these functions. */
#ifndef FAKE_IWDG_H
#define FAKE_IWDG_H
#include <stdint.h>
void iwdg_set_period_ms(uint32_t period);
void iwdg_start(void);
void iwdg_reset(void);
#endif
