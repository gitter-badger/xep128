/* Xep128: Minimalistic Enterprise-128 emulator with focus on "exotic" hardware
   Copyright (C)2016 LGB (Gábor Lénárt) <lgblgblgb@gmail.com>
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


static const Uint8 xep_rom_image[] = {
#include "xep_rom.hex"
};
int xep_rom_seg = -1;
int xep_rom_addr;
const char *rom_name_tab[0x100];
static const char xep_rom_description[] = "(Xep128-internal)";



static FILE *sram_open ( int seg, const char *mode, char *path )
{
	char fn[64];
	sprintf(fn, "@sram-%02X.seg", seg);
	return open_emu_file(fn, mode, path);
}



int sram_save_segment ( int seg )
{
	int a;
	char path[PATH_MAX + 1];
	FILE *f = sram_open(seg, "wb", path);
	DEBUGPRINT("MEM: SRAM: saving SRAM segment %02Xh to file %s" NL, seg, path);
	if (!f) {
		ERROR_WINDOW("Cannot create file for saving SRAM segment %02Xh because of file I/O error: %s\nFile name was: %s", seg, ERRSTR(), path);
		return 1;
	}
	a = fwrite(memory + (seg << 14), 0x4000, 1, f);
	if (a != 1)
		ERROR_WINDOW("Cannot save SRAM segment %02Xh because of file I/O error: %s\nFile name was: %s", seg, ERRSTR(), path);
	fclose(f);
	return a != 1;
}



int sram_load_segment ( int seg )
{
	int a;
	char path[PATH_MAX + 1];
	FILE *f = sram_open(seg, "rb", path);
	DEBUGPRINT("MEM: SRAM: loading SRAM segment %02Xh from file %s" NL, seg, path);
	if (!f) {
		ERROR_WINDOW("Cannot open file for loading SRAM segment %02Xh because of file I/O error: %s\nFile name was: %s", seg, ERRSTR(), path);
		return 1;
	}
	a = fread(memory + (seg << 14), 0x4000, 1, f);
	if (a != 1)
		ERROR_WINDOW("Cannot load SRAM segment %02Xh because of file I/O error: %s\nFile name was: %s", seg, ERRSTR(), path);
	fclose(f);
	return a != 1;
}



int sram_save_all_segments ( void )
{
	int a, ret = 0;
	for (a = 0; a < 0x100; a++ )
		if (memory_segment_map[a] == SRAM_SEGMENT)
			ret += sram_save_segment(a);
	return ret;
}



/* This function also re-initializes the whole memory! Do not call it after you defined RAM for the system, but only before! */
int roms_load ( void )
{
	int seg, last = 0;
	char path[PATH_MAX + 1];
	for (seg = 0; seg < 0x100; seg++ ) {
		memory_segment_map[seg] = (seg >= 0xFC ? VRAM_SEGMENT : UNUSED_SEGMENT);	// 64K VRAM is default, you cannot override that!
		rom_name_tab[seg] = NULL;
	}
	memset(memory, 0xFF, 0x400000);
	for (seg = 0; seg < 0x100; seg++ ) {
		void *option = config_getopt("rom", seg, NULL);
		if (option) {
			const char *name;
			int lseg = seg;
			FILE *f;
			config_getopt_pointed(option, &name);
			DEBUG("CONFIG: ROM: segment %02Xh file %s" NL, seg, name);
			f = open_emu_file(name, "rb", path);
			if (f == NULL) {
				ERROR_WINDOW("Cannot open ROM image \"%s\" (to be used from segment %02Xh): %s", name, seg, ERRSTR());
				if (!strcmp(name, COMBINED_ROM_FN)) { // this should be the auto-install functionality, with downloading stuff?
				}
				return -1;
			}
			DEBUG("CONFIG: ROM: ... file path is %s" NL, path);
			rom_name_tab[seg] = SDL_strdup(path);
			check_malloc(rom_name_tab[seg]);
			for (;;) {
				int ret;
				// Note: lseg overflow is not needed to be tested, as VRAM marks will stop reading of ROM image in the worst case ...
				if (memory_segment_map[lseg] != UNUSED_SEGMENT) {
					fclose(f);
					ERROR_WINDOW("While reading ROM image \"%s\" into segment %02Xh: already used segment (\"%s\")!", path, lseg, memory_segment_map[lseg]);
					return -1;
				}
				ret = fread(memory + (lseg << 14), 1, 0x4000, f);
				if (ret)
					DEBUG("CONFIG: ROM: ... trying read 0x4000 bytes in segment %02Xh, result is %d" NL, lseg, ret);
				if (ret < 0) {
					ERROR_WINDOW("Cannot read ROM image \"%s\" (to be used in segment %02Xh): %s", path, lseg, ERRSTR());
					fclose(f);
					return -1;
				} else if (ret == 0) {
					if (lseg == seg) {
						fclose(f);
						ERROR_WINDOW("Null-sized ROM image \"%s\" (to be used in segment %02Xh).", path, lseg);
						return -1;
					}
					break;
				} else if (ret != 0x4000) {
					fclose(f);
					ERROR_WINDOW("Bad ROM image \"%s\": not multiple of 16K bytes!", path);
					return -1;
				}
				memory_segment_map[lseg] = ROM_SEGMENT;
				if (lseg > last)
					last = lseg;
				if (ret != 0x4000)
					break;
				lseg++;
			}
			fclose(f);
		} else if (!seg) {
			ERROR_WINDOW("Fatal ROM image error: No ROM defined for segment 00h, no EXOS is requested!");
			return -1;
		}
	}
	/* search place for XEP ROM */
	if (memory_segment_map[last + 1] == UNUSED_SEGMENT) {	// TODO: XEP ROM may not work with non-Zozo EXOSes as they check only some segments with special alignments
		last++;
		xep_rom_seg = last;
		xep_rom_addr = last << 14;
		memcpy(memory + xep_rom_addr, xep_rom_image, sizeof xep_rom_image);
		memory_segment_map[last] = ROM_SEGMENT;
		rom_name_tab[last] = xep_rom_description;
		DEBUGPRINT("CONFIG: ROM: XEP internal ROM image installed in segment %02Xh" NL, last);
	}
	if (xep_rom_seg == -1) {
		ERROR_WINDOW("XEP internal ROM image cannot be installed. Xep128 will work, but :XEP comamnds won't!");
	}
	/* end of the game :) */
	DEBUGPRINT("CONFIG: ROM: DONE. Last used segment is %02Xh." NL, last);
	return last << 14;
}

