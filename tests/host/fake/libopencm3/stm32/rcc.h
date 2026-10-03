/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Fake for the host tests: RCC_CSR is a variable, the bits are as on the STM32F2. */
#ifndef FAKE_RCC_H
#define FAKE_RCC_H
#include "fake_regs.h"

#define RCC_CSR			fake_rcc_csr
#define RCC_CSR_IWDGRSTF	(1 << 29)
#define RCC_CSR_SFTRSTF		(1 << 28)
#define RCC_CSR_PORRSTF		(1 << 27)
#define RCC_CSR_PINRSTF		(1 << 26)
#define RCC_CSR_RMVF		(1 << 24)
#define RCC_CSR_RESET_FLAGS	(0x7FU << 25)
#endif
