// fssize.cpp -- PDP2011: how much of a disk image its file system uses
//
// The drive behind a disk image is chosen from the extent of the pack, and images in the wild
// are often shorter than their drive: SIMH stores a pack only as far as it has been written.
// So, like SIMH's autosize, look at the file system first: RSTS/E ([0,1]SATT.SYS, the
// allocation bitmap), RT-11 (the directory's extent) and Files-11 ODS-1 as used by RSX (the
// storage control block). The detection is ported from SIMH's sim_disk.c:
//
//   Copyright (c) 2011, Mark Pizzolato
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this
//   software and associated documentation files (the "Software"), to deal in the Software
//   without restriction, including without limitation the rights to use, copy, modify, merge,
//   publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons
//   to whom the Software is furnished to do so, subject to the following conditions: The above
//   copyright notice and this permission notice shall be included in all copies or substantial
//   portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
//   EXPRESS OR IMPLIED. (SIMH's RSTS detection is by Mark Pizzolato and John Forecast.)

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "fssize.h"

// read n 512-byte blocks at lba; what lies past the end of the image reads as zeros
static int rdblk(fileTYPE *f, uint64_t lba, void *buf, int n)
{
	uint64_t off = lba * 512, len = (uint64_t)n * 512;
	memset(buf, 0, len);
	if (off >= (uint64_t)f->size) return 1;
	if (off + len > (uint64_t)f->size) len = f->size - off;
	if (!FileSeek(f, off, SEEK_SET)) return 0;
	return FileReadAdv(f, buf, (int)len) == (int)len;
}

// ---------------------------------------------------------------- RSTS/E

#pragma pack(push,1)
struct RSTS_PACKLABEL { uint16_t pk_mb01, pk_mbm1, pk_mdcn, pk_plvl, pk_ppcs, pk_pstat, pk_packid[2]; };
struct RSTS_MFDLABEL { uint16_t ml_ulnk, ml_mbm1, ml_reserved1, ml_reserved2, ml_pcs, ml_pstat, ml_packid[2]; };
union RSTS_ROOT { RSTS_MFDLABEL rt_mfd; RSTS_PACKLABEL rt_pack; uint8_t rt_block[512]; };
struct RSTS_BLOCKETTE { uint16_t b_ulnk, b_mbm1, b_r1, b_r2, b_r3, b_r4, b_lppn, b_lid; };
struct RSTS_UNAME { uint16_t un_ulnk, un_unam, un_r1, un_r2, un_ustat, un_uacnt, un_uaa, un_uar; };
struct RSTS_FNAME { uint16_t fn_ulnk, fn_unam[3], fn_ustat, fn_uacnt, fn_uaa, fn_uar; };
struct RSTS_ACNT { uint16_t ac_ulnk, ac_udla, ac_usiz, ac_udc, ac_utc, ac_urts[2], ac_uclus; };
struct RSTS_RETR { uint16_t rt_ulnk, rt_uent[7]; };
struct RSTS_DCMAP { uint16_t dc_clus, dc_map[7]; };
#pragma pack(pop)

#define PK_LVL0   0000
#define PK_LVL11  0401
#define PK_LVL12  0402
#define PK_UC_NEW 0020000
#define MB_ID     0051064
#define GB_ID     0026264
#define UB_ID     0102064
#define DC_MASK   0077777
#define DL_ENO    0000760
#define DL_CLO    0007000
#define DL_BLO    0170000
#define BLOCKETTE_SZ 16
#define MAP_OFFSET   (31 * BLOCKETTE_SZ)
#define SATT0 0073374
#define SATT1 0076400
#define SATT2 0075273

struct rsts_ctx { fileTYPE *f; int dcshift; int pcs; char packid[8]; RSTS_DCMAP map; };

static const char rad50[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ$.%0123456789";
static void r50asc(uint16_t v, char *b) { b[2] = rad50[v % 050]; v /= 050; b[1] = rad50[v % 050]; b[0] = rad50[(v / 050) % 050]; }

static int rsts_clus_ok(uint16_t size, uint16_t min)
{
	if (size >= min) for (int i = 0; i < 16; i++) if (size == (1 << i)) return 1;
	return 0;
}

static int rsts_rdblk(rsts_ctx *c, uint16_t cluster, uint16_t block, void *buf)
{
	return rdblk(c->f, ((uint64_t)cluster << c->dcshift) + block, buf, 1);
}

static int rsts_rdent(rsts_ctx *c, uint16_t link, void *buf)
{
	uint16_t block = (link & DL_BLO) >> 12, dcn = (link & DL_CLO) >> 9, ent = (link & DL_ENO) >> 4;
	uint8_t t[512];
	if (dcn == 7 || ent == 31 || block > (c->map.dc_clus & DC_MASK)) return 0;
	if (!rsts_rdblk(c, c->map.dc_map[dcn], block, t)) return 0;
	memcpy(buf, &t[ent * BLOCKETTE_SZ], BLOCKETTE_SZ);
	return 1;
}

static int rsts_valid(const RSTS_BLOCKETTE *b, uint16_t lppn, uint16_t id, int any_link)
{
	return (any_link || b->b_ulnk == 0 || b->b_ulnk == 1) && b->b_mbm1 == 0177777 && !b->b_r1 && !b->b_r2 &&
		!b->b_r3 && !b->b_r4 && b->b_lppn == lppn && b->b_lid == id;
}

static int rsts_find01(rsts_ctx *c, uint16_t *ufd)
{
	uint16_t dcs = 1 << c->dcshift;
	RSTS_ROOT root;
	uint16_t buf[256];
	if (!rsts_rdblk(c, 1, 0, &root)) return 0;
	if (root.rt_pack.pk_mbm1 != 0177777 || !rsts_clus_ok(root.rt_pack.pk_ppcs, dcs)) return 0;
	c->pcs = root.rt_pack.pk_ppcs;
	r50asc(root.rt_pack.pk_packid[0], &c->packid[0]);
	r50asc(root.rt_pack.pk_packid[1], &c->packid[3]);
	c->packid[6] = 0;
	if (!isalnum((unsigned char)c->packid[0])) return 0;
	for (int i = 1; i < 6; i++)                                 // 1-6 alphanumerics, then spaces
	{
		if (isalnum((unsigned char)c->packid[i])) continue;
		if (c->packid[i] != ' ') return 0;
		for (int j = i; j < 6; j++) if (c->packid[j] != ' ') return 0;
		break;
	}
	if ((root.rt_pack.pk_pstat & PK_UC_NEW) == 0)                // RSTS up to V7: MFD label
	{
		if (dcs > 16) return 0;
		memcpy(&c->map, &root.rt_block[MAP_OFFSET], BLOCKETTE_SZ);
		uint16_t link = root.rt_mfd.ml_ulnk;
		RSTS_UNAME u;
		do {
			if (!rsts_rdent(c, link, &u)) return 0;
			if (u.un_unam == ((0 << 8) | 1)) { *ufd = u.un_uar; return 1; }
		} while ((link = u.un_ulnk) != 0);
		return 0;
	}
	switch (root.rt_pack.pk_plvl)                               // V8 and later: pack label
	{
	case PK_LVL11: if (dcs > 16) return 0; break;
	case PK_LVL12: if (dcs > 64) return 0; break;
	default: return 0;
	}
	uint16_t mfd = root.rt_pack.pk_mdcn, gfd;
	if (!rsts_rdblk(c, mfd, 0, buf) || !rsts_valid((RSTS_BLOCKETTE *)buf, 0177777, MB_ID, 0)) return 0;
	if (!rsts_rdblk(c, mfd, 1, buf) || (gfd = buf[0]) == 0) return 0;
	if (!rsts_rdblk(c, gfd, 0, buf) || !rsts_valid((RSTS_BLOCKETTE *)buf, (0 << 8) | 0377, GB_ID, 0)) return 0;
	if (!rsts_rdblk(c, gfd, 1, buf) || (*ufd = buf[1]) == 0) return 0;
	return 1;
}

static int rsts_satt(rsts_ctx *c, uint16_t uaa, uint16_t uar, uint64_t *blocks_out)
{
	static uint8_t bitmap[8192];
	RSTS_ACNT acnt;
	RSTS_RETR retr;
	if (!uar || !rsts_rdent(c, uaa, &acnt)) return 0;
	uint16_t blocks = acnt.ac_usiz, offset = 0;
	if (!rsts_clus_ok(acnt.ac_uclus, c->pcs) || blocks > 16 || blocks == 0) return 0;
	memset(bitmap, 0xFF, sizeof(bitmap));
	do {
		if (!rsts_rdent(c, uar, &retr)) return 0;
		for (int i = 0; i < 7; i++)
		{
			uint16_t fcl = retr.rt_uent[i];
			if (!fcl) goto scan;
			for (int j = 0; j < acnt.ac_uclus; j++)
			{
				if (!blocks || offset >= sizeof(bitmap)) goto scan;
				if (!rsts_rdblk(c, fcl, j, &bitmap[offset])) return 0;
				offset += 512; blocks--;
			}
		}
	} while ((uar = retr.rt_ulnk) != 0);
scan:
	blocks = 0;
	for (int i = sizeof(bitmap) - 1; i != 0; i--)
		if (bitmap[i] != 0xFF)
		{
			blocks = i * 8;
			for (int j = 7; j >= 0; j--) if ((bitmap[i] & (1 << j)) == 0) { blocks += j + 1; break; }
			break;
		}
	// the bitmap marks whole clusters, so the pack is more than `blocks` clusters and at most one
	// more (SIMH reports the upper bound, which overshoots an RP06 by 2 blocks at cluster 16):
	// report the smallest pack the bitmap allows
	*blocks_out = (uint64_t)blocks * c->pcs + 1;
	return 1;
}

static uint64_t rsts_size(fileTYPE *f, char *name, int namelen)
{
	rsts_ctx c;
	memset(&c, 0, sizeof(c));
	c.f = f;
	for (c.dcshift = 0; c.dcshift < 8; c.dcshift++)
	{
		uint16_t ufd;
		uint8_t buf[512];
		if (!rsts_find01(&c, &ufd) || !rsts_rdblk(&c, ufd, 0, buf)) continue;
		if (!rsts_valid((RSTS_BLOCKETTE *)buf, (0 << 8) | 1, UB_ID, 1)) continue;
		uint16_t link = ((RSTS_BLOCKETTE *)buf)->b_ulnk;
		memcpy(&c.map, &buf[MAP_OFFSET], BLOCKETTE_SZ);
		RSTS_FNAME fn;
		do {
			if (!rsts_rdent(&c, link, &fn)) break;
			uint64_t blocks;
			if (fn.fn_unam[0] == SATT0 && fn.fn_unam[1] == SATT1 && fn.fn_unam[2] == SATT2 &&
				rsts_satt(&c, fn.fn_uaa, fn.fn_uar, &blocks))
			{
				snprintf(name, namelen, "RSTS/E pack %s, cluster %d", c.packid, c.pcs);
				return blocks * 512;
			}
		} while ((link = fn.fn_ulnk) != 0);
	}
	return 0;
}

// ---------------------------------------------------------------- RT-11

#pragma pack(push,1)
struct RT11_HOME {
	uint8_t bbtable[130], unused1[2], initrestore[38], bup[18], unused2[260];
	uint16_t reserved1, reserved2;
	uint8_t unused3[14];
	uint16_t clustersize, firstdir, sysver;
	uint8_t volid[12], owner[12], sysid[12], unused4[2];
	uint16_t checksum;
};
struct RT11_DIRHDR { uint16_t count, next, highest, extra, start; };
struct RT11_DIRENT { uint16_t status, fname1, fname2, ftype, length, jobchannel, creationdate; };
#pragma pack(pop)
#define SYSVER_V3A 36521
#define SYSVER_V04 36434
#define SYSVER_V05 36435
#define DE_EOS     0004000

static uint64_t rt11_size(fileTYPE *f, char *name, int namelen)
{
	RT11_HOME home;
	uint8_t dir[1024];
	uint64_t best = 0;
	for (int part = 0; part < 256; part++)                       // RT-11 V5 partitions are 65536 blocks
	{
		uint32_t base = (uint32_t)part << 16;
		if ((uint64_t)base * 512 >= (uint64_t)f->size) break;
		if (!rdblk(f, base + 1, &home, 1)) break;
		int single = !memcmp(home.sysid, "DECRT11A    ", 12) && part == 0 &&
			(home.sysver == SYSVER_V3A || home.sysver == SYSVER_V04);
		int multi = !memcmp(home.sysid, "DECRT11A    ", 12) && home.sysver == SYSVER_V05;
		if (!single && !multi && !(part == 0 && !memcmp(home.sysid, "DECVMSEXCHNG", 12))) { if (part == 0) return 0; break; }
		uint16_t seg = 1, segmax = 0, highest = 0;
		uint8_t seen[32];
		memset(seen, 0, sizeof(seen));
		int ok = 1;
		do {
			if (seg > 31 || seen[seg]++) { ok = 0; break; }
			if (!rdblk(f, base + home.firstdir + (seg - 1) * 2, dir, 2)) { ok = 0; break; }
			RT11_DIRHDR *h = (RT11_DIRHDR *)dir;
			if (seg == 1) { segmax = h->highest; if (segmax > 31) { ok = 0; break; } }
			int esz = sizeof(RT11_DIRENT) + h->extra, off = sizeof(RT11_DIRHDR);
			uint16_t blk = h->start;
			while (1024 - off >= esz)
			{
				RT11_DIRENT *e = (RT11_DIRENT *)&dir[off];
				if (e->status & DE_EOS) break;
				if (((blk + e->length) & 0xFFFF) < blk) { ok = 0; break; }
				blk += e->length; off += esz;
			}
			if (!ok) break;
			if (blk > highest) highest = blk;
			seg = h->next;
			if (seg > segmax) { ok = 0; break; }
		} while (seg != 0);
		if (!ok) break;
		best = (uint64_t)(base + highest) * 512;
		if (single) break;
	}
	if (best) snprintf(name, namelen, "RT-11 volume");
	return best;
}

// ---------------------------------------------------------------- Files-11 ODS-1 (RSX)

#pragma pack(push,1)
struct ODS1_HOME {
	uint16_t ibmapsize; uint32_t ibmaplbn; uint16_t maxfiles, cluster, devtype, structlev;
	uint8_t volname[12], fill1[4]; uint16_t volowner, protect, volchar, fileprot;
	uint8_t fill2[6], window, extend, lru_lim, fill3[11]; uint16_t checksum1;
	uint8_t credate[14], fill4[382]; uint32_t serialnum; uint8_t fill5[12], volname2[12], ownername[12], format[12], fill6[2];
	uint16_t checksum2;
};
struct ODS1_RETR {
	uint8_t ex_segnum, ex_rvn; uint16_t ex_filnum, ex_filseq; uint8_t countsize, lbnsize, inuse, avail;
	struct { uint8_t highlbn, count; uint16_t lowlbn; } ptr[4];
};
struct ODS1_SCB { uint8_t unused[3], bitmapblks; struct { uint16_t freeblks, freeptr; } blk[1]; };
#pragma pack(pop)

static uint16_t ods_sum(const void *b, int words) { uint16_t s = 0; const uint16_t *w = (const uint16_t *)b; for (int i = 0; i < words; i++) s += w[i]; return s; }

static uint64_t ods1_size(fileTYPE *f, char *name, int namelen)
{
	ODS1_HOME home;
	uint16_t hdr[256];
	uint8_t scbbuf[512];
	if (!rdblk(f, 1, &home, 1)) return 0;
	if (!home.ibmapsize || !home.ibmaplbn || !home.maxfiles || home.cluster != 1 ||
		(home.structlev != 0401 && home.structlev != 0402) ||
		home.checksum1 != ods_sum(&home, (int)(((char *)&home.checksum1 - (char *)&home) / 2)) ||
		home.checksum2 != ods_sum(&home, (int)(((char *)&home.checksum2 - (char *)&home) / 2)))
		return 0;
	uint32_t ibmap = ((home.ibmaplbn << 16) | ((home.ibmaplbn >> 16) & 0xFFFF));
	if (!rdblk(f, (uint64_t)ibmap + home.ibmapsize + 1, hdr, 1)) return 0;   // BITMAP.SYS header
	if (ods_sum(hdr, 255) != hdr[255]) return 0;
	ODS1_RETR *r = (ODS1_RETR *)(hdr + ((uint8_t *)hdr)[1]);
	uint32_t scblbn = ((uint32_t)r->ptr[0].highlbn << 16) + r->ptr[0].lowlbn;
	if (!rdblk(f, scblbn, scbbuf, 1)) return 0;
	ODS1_SCB *s = (ODS1_SCB *)scbbuf;
	int k = s->bitmapblks < 127 ? s->bitmapblks : 0;
	uint64_t blocks = ((uint64_t)s->blk[k].freeblks << 16) + s->blk[k].freeptr;
	snprintf(name, namelen, "Files-11 ODS-1 volume %.12s", home.volname);
	return blocks * 512;
}

uint64_t pdp2011_fs_size(fileTYPE *f, char *name, int namelen)
{
	if (!f || !f->size) return 0;
	__off64_t keep = f->offset;
	uint64_t n = ods1_size(f, name, namelen);
	if (!n) n = rsts_size(f, name, namelen);
	if (!n) n = rt11_size(f, name, namelen);
	FileSeek(f, keep, SEEK_SET);
	return n;
}
