/* Xep128: Minimalistic Enterprise-128 emulator with focus on "exotic" hardware
   Copyright (C)2015,2016 LGB (Gábor Lénárt) <lgblgblgb@gmail.com>
   http://xep128.lgb.hu/

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include "xepem.h"
#include "xep_rom_syms.h"

#define COBUF ((char*)(memory + xep_rom_addr + xepsym_cobuf - 0xC000))
#define SET_XEPSYM_BYTE(sym, value) memory[xep_rom_addr + (sym) - 0xC000] = (value)
#define SET_XEPSYM_WORD(sym, value) do {	\
	SET_XEPSYM_BYTE(sym, (value) & 0xFF);	\
	SET_XEPSYM_BYTE((sym) + 1, (value) >> 8);	\
} while(0)
#define BIN2BCD(bin) ((((bin) / 10) << 4) | ((bin) % 10))

static const char EXOS_NEWLINE[] = "\r\n";
Uint8 exos_version = 0;
Uint8 exos_info[8];

#define EXOS_ADDR(n)		(0x3FC000 | ((n) & 0x3FFF))
#define EXOS_BYTE(n)		memory[EXOS_ADDR(n)]
#define EXOS_GET_WORD(n)	(EXOS_BYTE(n) | (EXOS_BYTE((n) + 1) << 8))



void exos_get_status_line ( char *buffer )
{
	Uint8 *s = memory + EXOS_ADDR(EXOS_GET_WORD(0xBFF6));
	int a = 40;
	while (a--)
		*(buffer++) = *(s++) & 0x7F;
	*buffer = '\0';
}


void xep_set_time_consts ( char *descbuffer )
{
	time_t now = emu_getunixtime();
	struct tm *t = localtime(&now);
	SET_XEPSYM_BYTE(xepsym_settime_hour,    BIN2BCD(t->tm_hour));
	SET_XEPSYM_WORD(xepsym_settime_minsec,  (BIN2BCD(t->tm_min) << 8) | BIN2BCD(t->tm_sec));
	SET_XEPSYM_BYTE(xepsym_setdate_year,    BIN2BCD(t->tm_year - 80));
	SET_XEPSYM_WORD(xepsym_setdate_monday,  (BIN2BCD(t->tm_mon + 1) << 8) | BIN2BCD(t->tm_mday));
	if (descbuffer)
		sprintf(descbuffer, "%04d-%02d-%02d %02d:%02d:%02d",
			t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
			t->tm_hour, t->tm_min, t->tm_sec
		);
	SET_XEPSYM_WORD(xepsym_jump_on_rom_entry, xepsym_set_time);
}



static int exos_cmd_name_match ( const char *that, Uint16 addr )
{
	if (strlen(that) != Z80_B) return 0;
	while (*that)
		if (*(that++) != read_cpu_byte(addr++))
			return 0;
	return 1;
}



static void xep_exos_command_trap ( void )
{
	Uint8 c = Z80_C, b = Z80_B;
	Uint16 de = Z80_DE;
	int size;
	*COBUF = 0; // no ans by def
	DEBUG("XEP: COMMAND TRAP: C=%02Xh, B=%02Xh, DE=%04Xh" NL, c, b, de);
	/* restore exos command handler jump address */
	SET_XEPSYM_WORD(xepsym_jump_on_rom_entry, xepsym_print_xep_buffer);
	switch (c) {
		case 2: // EXOS command
			if (exos_cmd_name_match("XEP", de + 1)) {
				char buffer[256];
				char *p = buffer;
				b = read_cpu_byte(de) - 3;
				de += 4;
				while (b--)
					*(p++) = read_cpu_byte(de++);
				*p = '\0';
				monitor_execute(
					buffer,			// input buffer
					1,			// source system (XEP ROM)
					COBUF,			// output buffer (directly into the co-buffer area!)
					xepsym_cobuf_size - 1,	// max allowed output size
					EXOS_NEWLINE		// newline delimiter requested (for EXOS we use this fixed value! unlike with console/monitor where it's host-OS dependent!)
				);
				Z80_A = 0;
				Z80_C = 0;
			}
			break;
		case 3: // EXOS help
			if (!b) {
				// eg on :HELP (ROM list) we patch the request as ROMNAME monitor command ...
				monitor_execute("ROMNAME", 1, COBUF, xepsym_cobuf_size - 1, EXOS_NEWLINE);
				Z80_A = 0;
			} else if (exos_cmd_name_match("XEP", de + 1)) {
				monitor_execute("HELP", 1, COBUF, xepsym_cobuf_size - 1, EXOS_NEWLINE);
				Z80_A = 0;
				Z80_C = 0;
			}
			break;
		case 8:	// Initialization
			// Tell XEP ROM to set EXOS date/time with setting we will provide here
			xep_set_time_consts(NULL);
			SET_XEPSYM_WORD(xepsym_jump_on_rom_entry, xepsym_system_init);
			break;
		case 1:	// Cold reset
			SET_XEPSYM_WORD(xepsym_jump_on_rom_entry, xepsym_cold_reset);
			break;
	}
	size = strlen(COBUF);
	if (size)
		DEBUG("XEP: ANSWER: [%d bytes] = \"%s\"" NL, size, COBUF);
	// just a sanity check, monitor_execute() would not allow - in theory ... - to store more data than specified (by MPRINTF)
	if (size > xepsym_cobuf_size - 1) {
		ERROR_WINDOW("FATAL: XEP ROM answer is too large, %d bytes.", size);
		exit(1);
	}
	SET_XEPSYM_WORD(xepsym_print_size, size);	// set print-out size (0 = no print)
}



void xep_rom_trap ( Uint16 pc, Uint8 opcode )
{
	xep_rom_write_support(0);	// to be safe, let's switch writable XEP ROM off (maybe it was enabled by previous trap?)
	DEBUG("XEP: ROM trap at PC=%04Xh OPC=%02Xh" NL, pc, opcode);
	if (opcode != xepsym_ed_trap_opcode) {
		ERROR_WINDOW("FATAL: Unknown ED-trap opcode in XEP ROM: PC=%04Xh ED_OP=%02Xh", pc, opcode);
		exit(1);
	}
	switch (pc) {
		case xepsym_trap_enable_rom_write:
			xep_rom_write_support(1);	// special ROM request to enable ROM write ... Danger Will Robinson!!
			break;
		case xepsym_trap_exos_command:
			xep_exos_command_trap();
			break;
		case xepsym_trap_on_system_init:
			exos_version = Z80_B;	// store EXOS version number we got ...
			memcpy(exos_info, memory + ((xepsym_exos_info_struct & 0x3FFF) | (xep_rom_seg << 14)), 8);
			//EXOS_BYTE(0xBFEF) = 1; // use this, to skip Enterprise logo when it would come :-)
			break;
		default:
			ERROR_WINDOW("FATAL: Unknown ED-trap location in XEP ROM: PC=%04Xh (ED_OP=%02Xh)", pc, opcode);
			exit(1);
	}
}

