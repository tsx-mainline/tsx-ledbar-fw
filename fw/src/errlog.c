// SPDX-License-Identifier: GPL-2.0-or-later
/* A small error log in RAM, shown by the ERRLOG console command. */
#include "tsx.h"

#define ERRLOG_SIZE 16

struct entry {
	uint32_t ms;
	uint16_t subsystem;
	uint16_t cause;
};

static struct entry entries[ERRLOG_SIZE];
static int count;

void errlog_add(uint16_t subsystem, uint16_t cause)
{
	if (count < ERRLOG_SIZE) {
		entries[count].ms = millis();
		entries[count].subsystem = subsystem;
		entries[count].cause = cause;
		count++;
	}
}

int errlog_count(void)
{
	return count;
}

void errlog_clear(void)
{
	count = 0;
}

void errlog_each(void (*fn)(int, uint32_t, uint16_t, uint16_t))
{
	for (int i = 0; i < count; i++)
		fn(i, entries[i].ms, entries[i].subsystem, entries[i].cause);
}
