// diskcompat.cpp -- PDP2011: disk images found in the wild
//
// 1. Short images grow. A write past the end of an RK, RL or RH image grows
//    the file (the filesystem zero-fills the gap) up to the drive's nominal
//    size, as SIMH does. Many images in circulation are a few sectors short
//    (e.g. RSTS RP06 images 6 sectors short of 815*19*22*512); writes there
//    used to vanish.
//
// 2. RL01/RL02 bad-sector table. Real packs carry a factory bad-sector file
//    (DEC STD 144) in the first 10 sectors of the last track; RSX BRU,
//    RT-11 and others refuse a pack without one. SIMH writes "no bad
//    sectors" when it creates an image; images made any other way have
//    zeros there. When that area holds nothing (all zero, or past the end of
//    the file), it reads as SIMH's empty table: pack serial, two zero words,
//    177777 to the end of each 256-byte sector. Nothing is written for it,
//    and -- the user's rule -- a write that lands only in that area does not
//    grow a short image. Any real data there is the OS's and passes through.
//
// 3. SIMH's footer. SIMH 4 appends 512 bytes to the images it creates: "simh",
//    the drive type ("RL01", "RM03", ...), sector size and count, a CRC-32,
//    numbers big-endian; the data is sector count x size before it. A valid
//    footer is hidden (the image's size becomes its data size, so it is never
//    read or written as pack data) and its drive type decides the drive.
//
// 4. The drive behind an RL or RH image. The extent of the pack -- its file
//    system's (fssize.cpp: RSTS/E, RT-11, ODS-1, as SIMH's autosize), else the
//    file's size -- picks the drive: with a file system the smallest that holds
//    it (RL01 or RL02; RM03, RP04, RM80, RP06, RM05 or RP07), without one an RH
//    drive whose size the file is close to (1 MB short to 64 KB long), else an
//    RP06 as before. The core is told that drive's nominal
//    size instead of the file size (pdp2011_disk_report_size) and sets the
//    drive type from it; growing a short image stops at the same size.

#include <stdio.h>
#include <string.h>
#include "diskcompat.h"
#include "fssize.h"
#include "../../lib/miniz/miniz.h"

#define RK05_SIZE  2494464ULL       // 203 cyl * 2 surfaces * 12 * 512
#define RL01_SIZE  5242880ULL       // 256 cyl * 2 * 40 * 256
#define RL02_SIZE  10485760ULL
#define RP06_SIZE  174423040ULL     // 815 * 19 * 22 * 512
#define RL_TRACK   (40 * 256ULL)
#define BBT_LEN    (10 * 256ULL)    // first 10 sectors of the last track

// per slot: -1 unknown, 0 the image has data in the table area, 1 serve the empty table
static int bbt_state[3] = { -1, -1, -1 };

// RH drives in order of capacity: rh11's rh_type, cylinders x tracks x sectors x 512
static const struct { const char *name; uint64_t size; } rh_drives[] = {
	{ "RM03",  67420160ULL },     // 823 x  5 x 32
	{ "RP04",  87960576ULL },     // 411 x 19 x 22
	{ "RM80", 124214272ULL },     // 559 x 14 x 31
	{ "RP06", RP06_SIZE },        // 815 x 19 x 22
	{ "RM05", 256196608ULL },     // 823 x 19 x 32
	{ "RP07", 516096000ULL },     // 630 x 32 x 50
};

// without a file system, an image this much shorter or longer than a drive is taken for it
#define RH_SHORT (1024 * 1024ULL)
#define RH_LONG  (64 * 1024ULL)

// per slot: the nominal size of the drive chosen at mount (0: not chosen yet)
static uint64_t slot_nominal[3];

static uint64_t nominal(int slot, uint64_t size)
{
	if (slot >= 0 && slot <= 2 && slot_nominal[slot]) return slot_nominal[slot];
	switch (slot)
	{
	case 0: return RK05_SIZE;
	case 1: return size <= RL01_SIZE ? RL01_SIZE : RL02_SIZE;
	case 2: return RP06_SIZE;
	}
	return 0;
}

static uint64_t bbt_start(fileTYPE *f)
{
	return nominal(1, f->size) - RL_TRACK;
}

// per slot: the drive type named by the image's SIMH footer ("" if none)
static char slot_footer[3][17];

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// a valid SIMH footer at the end of the image: its drive type and data size
static int simh_footer(fileTYPE *f, char *drv, uint64_t *data)
{
	uint8_t b[512];
	if ((uint64_t)f->size <= sizeof(b)) return 0;
	__off64_t keep = f->offset;
	int ok = FileSeek(f, f->size - sizeof(b), SEEK_SET) && FileReadAdv(f, b, sizeof(b)) == (int)sizeof(b);
	FileSeek(f, keep, SEEK_SET);
	if (!ok || memcmp(b, "simh", 4) || be32(b + 508) != (uint32_t)mz_crc32(MZ_CRC32_INIT, b, 508)) return 0;
	uint64_t n = (uint64_t)be32(b + 84) * be32(b + 88);            // sector size x sector count
	if (!n || n > (uint64_t)f->size - sizeof(b)) return 0;
	memcpy(drv, b + 68, 16); drv[16] = 0;
	*data = n;
	return 1;
}

// choose the drive for an RL or RH image from the extent of its pack
static void choose_drive(int slot, fileTYPE *f)
{
	char fsname[64] = "";
	const char *drv = "?";
	slot_nominal[slot] = 0;
	if (slot_footer[slot][0])                                    // SIMH says which drive: that is it
	{
		const char *n = slot_footer[slot];
		if (slot == 1 && (!strcmp(n, "RL01") || !strcmp(n, "RL02")))
			slot_nominal[1] = strcmp(n, "RL01") ? RL02_SIZE : RL01_SIZE;
		if (slot == 2)
		{
			if (!strcmp(n, "RP05")) n = "RP04";                     // the same geometry
			for (unsigned i = 0; i < sizeof(rh_drives) / sizeof(rh_drives[0]); i++)
				if (!strcmp(n, rh_drives[i].name)) slot_nominal[2] = rh_drives[i].size;
		}
		if (slot_nominal[slot])
		{
			printf("PDP2011 disk slot %d: %s, SIMH footer: %s -> %s\n", slot, f->name, slot_footer[slot], n);
			return;
		}
		printf("PDP2011 disk slot %d: %s, SIMH footer names %s, not a drive of this controller\n", slot, f->name, slot_footer[slot]);
	}
	uint64_t fs = pdp2011_fs_size(f, fsname, sizeof(fsname));
	uint64_t ext = fs ? fs : (uint64_t)f->size;
	if (slot == 1)
	{
		slot_nominal[1] = ext <= RL01_SIZE ? RL01_SIZE : RL02_SIZE;
		drv = ext <= RL01_SIZE ? "RL01" : "RL02";
	}
	else if (slot == 2)
	{
		const unsigned n = sizeof(rh_drives) / sizeof(rh_drives[0]);
		if (fs)                                                  // a file system: the smallest drive that holds it
		{
			for (unsigned i = 0; i < n; i++)
				if (ext <= rh_drives[i].size) { slot_nominal[2] = rh_drives[i].size; drv = rh_drives[i].name; break; }
		}
		else                                                     // only the file size: a drive it is close to
		{                                                        // (images are often a little short or long)
			for (unsigned i = 0; i < n; i++)
				if (ext + RH_SHORT >= rh_drives[i].size && ext <= rh_drives[i].size + RH_LONG)
					{ slot_nominal[2] = rh_drives[i].size; drv = rh_drives[i].name; break; }
		}
		if (!slot_nominal[2]) { slot_nominal[2] = RP06_SIZE; drv = "RP06 (default)"; }
	}
	else return;
	if (fs) printf("PDP2011 disk slot %d: %s, %llu bytes; %s, %llu blocks -> %s\n", slot, f->name,
		(unsigned long long)f->size, fsname, (unsigned long long)(fs / 512), drv);
	else printf("PDP2011 disk slot %d: %s, %llu bytes; no file system recognised -> %s\n", slot, f->name,
		(unsigned long long)f->size, drv);
}

uint64_t pdp2011_disk_report_size(int slot, fileTYPE *f)
{
	if (slot < 1 || slot > 2 || !f || !f->size || !slot_nominal[slot]) return f ? (uint64_t)f->size : 0;
	return slot_nominal[slot];
}

static int bbt_empty(fileTYPE *f)
{
	uint8_t b[BBT_LEN];
	uint64_t s = bbt_start(f);
	memset(b, 0, sizeof(b));
	if ((uint64_t)f->size > s)
	{
		__off64_t keep = f->offset;
		uint32_t n = (uint32_t)((uint64_t)f->size - s < BBT_LEN ? (uint64_t)f->size - s : BBT_LEN);
		if (FileSeek(f, s, SEEK_SET)) FileReadAdv(f, b, n);
		FileSeek(f, keep, SEEK_SET);
	}
	for (uint32_t i = 0; i < BBT_LEN; i++) if (b[i]) return 0;
	return 1;
}

void pdp2011_disk_mounted(int slot, fileTYPE *f)
{
	if (slot < 0 || slot > 2) return;
	bbt_state[slot] = -1;
	slot_nominal[slot] = 0;
	slot_footer[slot][0] = 0;
	uint64_t data;
	if (f && f->size && simh_footer(f, slot_footer[slot], &data))
	{
		printf("PDP2011 disk slot %d: %s has a SIMH footer (%s, %llu data bytes): hidden\n", slot, f->name,
			slot_footer[slot], (unsigned long long)data);
		f->size = data;                                          // the pack ends where the footer begins
	}
	if ((slot == 1 || slot == 2) && f && f->size) choose_drive(slot, f);
	if (slot == 1 && f && f->size)
	{
		bbt_state[1] = bbt_empty(f);
		printf("PDP2011 RL image %s: %llu bytes, bad-sector table %s\n", f->name,
			(unsigned long long)f->size, bbt_state[1] ? "missing (empty one served)" : "present");
	}
}

int pdp2011_disk_write(int slot, fileTYPE *f, uint64_t off, const uint8_t *buf, uint32_t len)
{
	if (slot < 0 || slot > 2 || !f || !f->size) return 0;
	uint64_t size = f->size, end = off + len;

	if (slot == 1)
	{
		uint64_t s = bbt_start(f);
		if (off < s + BBT_LEN && end > s) bbt_state[1] = -1;   // the OS writes its own table: recheck
		if (end > size && off >= s && end <= s + RL_TRACK)
			return 1;                                         // only the table area of a short image: don't grow
	}

	if (end <= size) return 0;                                   // inside the image: normal path
	uint64_t nom = nominal(slot, size);
	if (off >= nom) return 1;                                    // past the drive: drop, as before
	if (end > nom) len = (uint32_t)(nom - off);

	// grow: write at off (the filesystem fills any gap with zeros)
	if (!FileSeek(f, off, SEEK_SET)) return 1;
	if (FileWriteAdv(f, (void *)buf, len) && (uint64_t)f->size < off + len)
		f->size = off + len;
	printf("PDP2011 disk slot %d: grew image to %llu bytes\n", slot, (unsigned long long)f->size);
	return 1;
}

void pdp2011_disk_read_fixup(int slot, fileTYPE *f, uint64_t off, uint8_t *buf, uint32_t len)
{
	if (slot != 1 || !f || !f->size) return;
	uint64_t s = bbt_start(f);
	if (off >= s + BBT_LEN || off + len <= s) return;            // read misses the table area
	if (bbt_state[1] < 0) bbt_state[1] = bbt_empty(f);
	if (!bbt_state[1]) return;

	// pack serial from the file name (stable for a given image), MSB of word 1 clear
	uint32_t serial = 0x1234;
	for (const char *p = f->name; *p; p++) serial = serial * 31 + (uint8_t)*p;
	serial &= 0x7FFFFFFF;
	for (uint64_t a = (off > s ? off : s); a < off + len && a < s + BBT_LEN; a++)
	{
		uint32_t k = (uint32_t)((a - s) % 256);              // byte within the 256-byte sector
		uint8_t v;
		if (k < 4) v = (uint8_t)(serial >> (8 * k));
		else if (k < 8) v = 0;
		else v = 0xFF;
		buf[a - off] = v;
	}
}
