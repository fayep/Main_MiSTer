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

#include <stdio.h>
#include <string.h>
#include "diskcompat.h"

#define RK05_SIZE  2494464ULL       // 203 cyl * 2 surfaces * 12 * 512
#define RL01_SIZE  5242880ULL       // 256 cyl * 2 * 40 * 256
#define RL02_SIZE  10485760ULL
#define RP06_SIZE  174423040ULL     // 815 * 19 * 22 * 512
#define RL_TRACK   (40 * 256ULL)
#define BBT_LEN    (10 * 256ULL)    // first 10 sectors of the last track

// per slot: -1 unknown, 0 the image has data in the table area, 1 serve the empty table
static int bbt_state[3] = { -1, -1, -1 };

static uint64_t nominal(int slot, uint64_t size)
{
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
