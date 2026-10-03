/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Linker script of the tsx-ledbar firmware (STM32F205RC, 256 KB flash,
 * 96 KB RAM). The application region starts at 0x08020000 (flash sector 5
 * and up). The first 256 bytes of RAM stay out of .data and .bss: the
 * bootloader mailbox is at 0x200000F4 and the two guard words follow it.
 *
 * TSX_ROM_ORIGIN can be set with --defsym for the QEMU build, which runs
 * without the bootloader and starts at 0x08000000.
 */

TSX_ROM_ORIGIN = DEFINED(TSX_ROM_ORIGIN) ? TSX_ROM_ORIGIN : 0x08020000;

MEMORY
{
	rom (rx)  : ORIGIN = TSX_ROM_ORIGIN, LENGTH = 128K
	ram (rwx) : ORIGIN = 0x20000100, LENGTH = 96K - 256
}

/* bootloader mailbox and guard words, outside every section */
tsx_mailbox = 0x200000F4;
tsx_guard_magic = 0x200000F8;
tsx_guard_state = 0x200000FC;

EXTERN (vector_table)
ENTRY(reset_handler)

SECTIONS
{
	.text : {
		KEEP(*(.vectors))	/* 0x184 bytes: 16 + 81 entries */
		KEEP(*(.tsx_header))	/* CSIGN header at 0x184 */
		*(.text*)
		. = ALIGN(4);
		*(.rodata*)
		. = ALIGN(4);
	} >rom

	.preinit_array : {
		. = ALIGN(4);
		__preinit_array_start = .;
		KEEP (*(.preinit_array))
		__preinit_array_end = .;
	} >rom
	.init_array : {
		. = ALIGN(4);
		__init_array_start = .;
		KEEP (*(SORT(.init_array.*)))
		KEEP (*(.init_array))
		__init_array_end = .;
	} >rom
	.fini_array : {
		. = ALIGN(4);
		__fini_array_start = .;
		KEEP (*(.fini_array))
		KEEP (*(SORT(.fini_array.*)))
		__fini_array_end = .;
	} >rom

	.ARM.extab : {
		*(.ARM.extab*)
	} >rom
	.ARM.exidx : {
		__exidx_start = .;
		*(.ARM.exidx*)
		__exidx_end = .;
	} >rom

	. = ALIGN(4);
	_etext = .;

	.data : {
		_data = .;
		*(.data*)
		. = ALIGN(4);
		_edata = .;
	} >ram AT >rom
	_data_loadaddr = LOADADDR(.data);

	.bss : {
		*(.bss*)
		*(COMMON)
		. = ALIGN(4);
		_ebss = .;
	} >ram

	/DISCARD/ : { *(.eh_frame) }

	. = ALIGN(4);
	end = .;
}

PROVIDE(_stack = ORIGIN(ram) + LENGTH(ram));
