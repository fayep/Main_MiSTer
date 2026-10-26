// devinf.cpp -- PDP2011 ".inf" device manifests: map a controller's units to image files.
//
// A disk slot's mounted file is normally a single .dsk (unit 0). A ".inf" instead is a small
// text manifest naming the image for each unit of that controller, so a pack SYSGEN'd for a
// non-zero unit (e.g. RSTS V7.0 wanting its system at DL2) boots from plain, correctly sized
// images -- no padding into a multi-unit container. One manifest per device type; the unit is
// the key, the device type is implied by the slot the manifest is loaded into:
//
//     ; RL units
//     0 = RSTS/V7.0/nk_boot.dsk
//     2 = RSTS/V7.0/nk_system.dsk ro      ; a trailing "ro" protects the image from writes
//
// The same image may be named for more than one unit (one pack aliased to its boot and system
// units); mark it "ro" on units the guest only reads, so an alias is never written through two
// handles. "ro" works for unit 0 (opened read-only) and units 1-3 (writes dropped).
//
// Paths are relative to the manifest's own directory (or absolute). Unit 0 is handled by the
// normal sd_image[slot] path (so its SIMH footer still gives the drive type and size); this
// module holds units 1-3 and serves their sectors. The core addresses unit N at LBA
// N*stride+block; user_io.cpp routes unit>=1 reads/writes here. An unmapped unit reads as zeros
// for now (a per-unit "not present" error is a later phase).
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "devinf.h"
#include "../../file_io.h"

#define NSLOT 3          // disk slots 0 (RK), 1 (RL), 2 (RH)
#define NUNIT 8          // up to 8 units per controller (RK05); RL uses 0-3

// per-slot unit stride in 512-byte blocks (unit N starts at N*stride); 0 = no .inf / no routing
static uint32_t slot_stride[NSLOT];
// units 1-3 (unit 0 is the normal sd_image[slot]); open file or size 0 if absent
static fileTYPE unit_file[NSLOT][NUNIT];
static int unit_open[NSLOT][NUNIT];
static int unit_ro[NSLOT][NUNIT];        // unit mounted read-only (.inf "ro"): drop writes
static uint8_t slot_present[NSLOT];      // bit u: a unit u image is mounted (for the core's UIO word)
static uint8_t slot_romask[NSLOT];       // bit u: unit u is read-only

int pdp2011_inf_active(int slot)
{
	return slot >= 0 && slot < NSLOT && slot_stride[slot] != 0;
}

// the unit an LBA addresses, given the slot's stride (0 if no .inf here)
int pdp2011_inf_unit(int slot, uint64_t lba)
{
	if (!pdp2011_inf_active(slot)) return -1;
	return (int)(lba / slot_stride[slot]);
}

// close any files a previous manifest left open on this slot
void pdp2011_inf_unmount(int slot)
{
	if (slot < 0 || slot >= NSLOT) return;
	for (int u = 1; u < NUNIT; u++)
		if (unit_open[slot][u]) { FileClose(&unit_file[slot][u]); unit_open[slot][u] = 0; unit_ro[slot][u] = 0; }
	slot_stride[slot] = 0;
	slot_present[slot] = 0;
	slot_romask[slot] = 0;
}

static void dirof(const char *path, char *dir, size_t n)
{
	snprintf(dir, n, "%s", path);
	char *s = strrchr(dir, '/');
	if (s) s[1] = 0; else dir[0] = 0;
}

// read the manifest; fill units[0..3] with resolved paths ("" if not named) and ro[0..3] with
// the read-only flag (a trailing "ro" / "readonly" token after the path). Returns unit count.
static int parse_inf(const char *inf_path, char units[NUNIT][1024], int ro[NUNIT])
{
	for (int u = 0; u < NUNIT; u++) { units[u][0] = 0; ro[u] = 0; }
	char dir[1024]; dirof(inf_path, dir, sizeof(dir));
	FILE *f = fopen(getFullPath(inf_path), "r");
	if (!f) { printf("PDP2011 .inf: cannot open %s\n", inf_path); return 0; }
	char line[1200]; int n = 0;
	while (fgets(line, sizeof(line), f))
	{
		char *p = line;
		while (*p && isspace((unsigned char)*p)) p++;
		if (*p == ';' || *p == '#' || !*p) continue;             // comment / blank
		if (!isdigit((unsigned char)*p)) continue;
		int unit = *p - '0';
		char *eq = strchr(p, '=');
		if (unit < 0 || unit >= NUNIT || !eq) continue;
		char *v = eq + 1;
		while (*v && isspace((unsigned char)*v)) v++;
		char *e = v + strlen(v);
		while (e > v && isspace((unsigned char)e[-1])) *--e = 0;  // trim trailing ws/newline
		if (!*v) continue;
		// optional trailing "ro"/"readonly" token (whitespace-separated after the path)
		char *sp = strrchr(v, ' ');
		if (!sp) sp = strrchr(v, '\t');
		if (sp && (!strcasecmp(sp + 1, "ro") || !strcasecmp(sp + 1, "readonly")))
		{
			ro[unit] = 1;
			while (sp > v && isspace((unsigned char)sp[-1])) sp--;
			*sp = 0;                                             // path is everything before the flag
		}
		if (v[0] == '/') snprintf(units[unit], 1024, "%s", v);   // absolute
		else snprintf(units[unit], 1024, "%s%s", dir, v);        // relative to the manifest
		n++;
	}
	fclose(f);
	return n;
}

// resolve unit 0's image path for the normal mount; returns 0 if the manifest names no unit 0.
// *ro (if non-NULL) gets unit 0's read-only flag, so the caller opens sd_image accordingly.
int pdp2011_inf_unit0(const char *inf_path, char *out, size_t n, int *ro)
{
	char units[NUNIT][1024]; int r[NUNIT];
	if (!parse_inf(inf_path, units, r) || !units[0][0]) return 0;
	snprintf(out, n, "%s", units[0]);
	if (ro) *ro = r[0];
	return 1;
}

// set up a slot from its manifest: open units 1-3, remember the stride. Call after unit 0 mounts.
// writable is the slot's overall writability; a unit's "ro" flag forces it read-only regardless.
void pdp2011_inf_mount(int slot, const char *inf_path, uint32_t stride_blocks, int writable)
{
	pdp2011_inf_unmount(slot);
	if (slot < 0 || slot >= NSLOT) return;
	char units[NUNIT][1024]; int ro[NUNIT];
	parse_inf(inf_path, units, ro);
	slot_stride[slot] = stride_blocks;
	slot_present[slot] = 0; slot_romask[slot] = 0;
	for (int u = 0; u < NUNIT; u++) if (units[u][0]) { slot_present[slot] |= 1 << u; if (ro[u]) slot_romask[slot] |= 1 << u; }
	for (int u = 1; u < NUNIT; u++)
	{
		if (!units[u][0]) continue;
		int w = writable && !ro[u];
		if (FileOpenEx(&unit_file[slot][u], units[u], w ? (O_RDWR | O_SYNC) : O_RDONLY))
		{
			unit_open[slot][u] = 1;
			unit_ro[slot][u] = !w;
			printf("PDP2011 .inf slot %d unit %d: %s (%llu bytes)%s\n", slot, u, units[u],
				(unsigned long long)unit_file[slot][u].size, unit_ro[slot][u] ? " read-only" : "");
		}
		else printf("PDP2011 .inf slot %d unit %d: cannot open %s\n", slot, u, units[u]);
	}
}

// serve a unit>=1 read; byte offset within the unit = (lba % stride)*blksz. zeros if absent.
int pdp2011_inf_read(int slot, int unit, uint64_t off_in_unit, uint8_t *buf, uint32_t len)
{
	memset(buf, 0, len);
	if (slot < 0 || slot >= NSLOT || unit < 1 || unit >= NUNIT || !unit_open[slot][unit]) return 1;
	fileTYPE *f = &unit_file[slot][unit];
	if (off_in_unit >= (uint64_t)f->size) return 1;              // past this pack: zeros
	if (FileSeek(f, off_in_unit, SEEK_SET)) FileReadAdv(f, buf, len);
	return 1;
}

int pdp2011_inf_write(int slot, int unit, uint64_t off_in_unit, const uint8_t *buf, uint32_t len)
{
	if (slot < 0 || slot >= NSLOT || unit < 1 || unit >= NUNIT || !unit_open[slot][unit]) return 1;
	if (unit_ro[slot][unit]) return 1;                           // read-only: drop the write
	fileTYPE *f = &unit_file[slot][unit];
	if (off_in_unit >= (uint64_t)f->size) return 1;              // past this pack: drop
	if (FileSeek(f, off_in_unit, SEEK_SET)) FileWriteAdv(f, (void *)buf, len);
	return 1;
}

uint32_t pdp2011_inf_stride(int slot)
{
	return pdp2011_inf_active(slot) ? slot_stride[slot] : 0;
}

// a plain single .dsk (no .inf) is one pack: unit 0 present, the rest absent; unit 0 write-protected
// iff the mount is not writable.
void pdp2011_dev_plain(int slot, int present, int writable)
{
	if (slot < 0 || slot >= NSLOT) return;
	slot_present[slot] = present ? 1 : 0;
	slot_romask[slot] = (present && !writable) ? 1 : 0;
}

// per-slot present/read-only bitmasks for the UIO word (bit u = unit u)
void pdp2011_dev_bits(int slot, uint8_t *present, uint8_t *ro)
{
	if (slot < 0 || slot >= NSLOT) { if (present) *present = 0; if (ro) *ro = 0; return; }
	if (present) *present = slot_present[slot];
	if (ro) *ro = slot_romask[slot];
}
