/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Fake registers for the host tests of the firmware: the libopencm3
 * headers in this directory map the register macros to these variables.
 * The test files define them and model the hardware.
 */
#ifndef FAKE_REGS_H
#define FAKE_REGS_H
#include <stdint.h>

extern uint32_t fake_rcc_csr;
#endif
