// tapeconv.cpp -- PDP2011: TPC / E11 tape images -> SIMH .tap
//
// SIMH: 32-bit LE length, data padded to even, the same length again;
//       0 = tape mark; 0xFFFFFFFF = end of medium (optional at EOF).
//       (Bit 31 set = error record; the TM11 doesn't use them here.)
// E11:  as SIMH but records of odd length are not padded.
// TPC:  16-bit LE length, data padded to even; 0 = tape mark; no trailer.
// Detection walks the whole file; a format matches when every record is
// consistent and the walk ends exactly at EOF (or at an EOM marker). Failing
// that, a file whose records parse as SIMH up to some point is still SIMH.

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "tapeconv.h"

static int rd(FILE *f, void *b, size_t n) { return fread(b, 1, n, f) == n; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }

static long fsize(FILE *f)
{
	long cur = ftell(f); fseek(f, 0, SEEK_END);
	long n = ftell(f); fseek(f, cur, SEEK_SET);
	return n;
}

// pad: 1 = SIMH (odd records padded), 0 = E11 (not padded).
// Returns the number of complete records before the walk ended; *clean is
// set when it ended exactly at EOF or at an end-of-medium marker.
static int simh_walk(FILE *f, int pad, int *clean)
{
	long n = fsize(f), pos = 0;
	uint8_t h[4], t[4];
	int recs = 0;
	*clean = 0;
	fseek(f, 0, SEEK_SET);
	while (pos < n)
	{
		if (pos + 4 > n || !rd(f, h, 4)) return recs;
		uint32_t len = le32(h);
		pos += 4;
		if (len == 0) continue;                       // tape mark
		if (len == 0xFFFFFFFF) { *clean = 1; return recs; }   // end of medium
		uint32_t l = len & 0x00FFFFFF;
		long skip = l + (pad ? (l & 1) : 0);
		if (pos + skip + 4 > n) return recs;
		fseek(f, skip, SEEK_CUR); pos += skip;
		if (!rd(f, t, 4) || le32(t) != len) return recs;
		pos += 4; recs++;
	}
	*clean = 1;
	return recs;
}

static int is_tpc(FILE *f)
{
	long n = fsize(f), pos = 0;
	uint8_t h[2];
	int recs = 0;
	fseek(f, 0, SEEK_SET);
	while (pos < n)
	{
		if (pos + 2 > n || !rd(f, h, 2)) return 0;
		uint16_t len = le16(h);
		pos += 2;
		if (len == 0) continue;
		long skip = len + (len & 1);
		if (pos + skip > n) return 0;
		fseek(f, skip, SEEK_CUR); pos += skip; recs++;
	}
	return recs > 0 && pos == n;
}

enum tape_format tape_detect(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) return TAPE_UNKNOWN;
	enum tape_format r = TAPE_UNKNOWN;
	int clean, recs = simh_walk(f, 1, &clean), clean_e11;
	if (recs && clean) r = TAPE_SIMH;
	else if (is_tpc(f)) r = TAPE_TPC;
	else if (simh_walk(f, 0, &clean_e11) && clean_e11) r = TAPE_E11;
	// Valid SIMH records followed by padding or junk (seen on real kits,
	// e.g. the RSTS/E 10.1 install tape): mount as is, the TM11 reads it.
	// A TPC tape never gets here: its first 32-bit "length" takes in data
	// bytes, so no trailer matches.
	else if (recs) r = TAPE_SIMH;
	fclose(f);
	return r;
}

const char *tape_format_name(enum tape_format fmt)
{
	switch (fmt)
	{
	case TAPE_SIMH: return "SIMH";
	case TAPE_TPC:  return "TPC";
	case TAPE_E11:  return "E11";
	default:        return "unknown";
	}
}

static int put_rec(FILE *o, const uint8_t *data, uint32_t len)
{
	uint8_t L[4] = { (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16), (uint8_t)(len >> 24) };
	static const uint8_t zero = 0;
	if (fwrite(L, 1, 4, o) != 4) return 0;
	if (len && fwrite(data, 1, len & 0x00FFFFFF, o) != (len & 0x00FFFFFF)) return 0;
	if (len && ((len & 0x00FFFFFF) & 1) && fwrite(&zero, 1, 1, o) != 1) return 0;
	if (len && fwrite(L, 1, 4, o) != 4) return 0;
	return 1;
}

int tape_convert(const char *src, const char *dst, enum tape_format fmt)
{
	if (fmt != TAPE_TPC && fmt != TAPE_E11) return 0;
	FILE *f = fopen(src, "rb");
	if (!f) return 0;
	FILE *o = fopen(dst, "wb");
	if (!o) { fclose(f); return 0; }
	long n = fsize(f), pos = 0;
	uint8_t *buf = (uint8_t *)malloc(0x10000 + 2);
	int ok = buf != NULL;
	while (ok && pos < n)
	{
		uint8_t h[4];
		uint32_t len;
		if (fmt == TAPE_TPC)
		{
			if (!rd(f, h, 2)) { ok = 0; break; }
			len = le16(h); pos += 2;
			if (len == 0) { ok = put_rec(o, NULL, 0); continue; }
			uint32_t skip = len + (len & 1);
			if (!rd(f, buf, skip)) { ok = 0; break; }
			pos += skip;
			ok = put_rec(o, buf, len);
		}
		else  // E11: SIMH-like framing, odd records unpadded
		{
			if (!rd(f, h, 4)) { ok = 0; break; }
			len = le32(h); pos += 4;
			if (len == 0) { ok = put_rec(o, NULL, 0); continue; }
			if (len == 0xFFFFFFFF) break;
			uint32_t l = len & 0x00FFFFFF;
			uint8_t *b = (uint8_t *)realloc(buf, l + 2);
			if (!b) { ok = 0; break; }
			buf = b;
			if (!rd(f, buf, l) || !rd(f, h, 4)) { ok = 0; break; }
			pos += l + 4;
			ok = put_rec(o, buf, len);
		}
	}
	if (ok)
	{
		static const uint8_t eom[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
		ok = fwrite(eom, 1, 4, o) == 4;
	}
	free(buf);
	fclose(f);
	if (fclose(o) != 0) ok = 0;
	if (!ok) remove(dst);
	return ok;
}

#ifndef TAPECONV_MAIN
#include <sys/stat.h>
#include "../../file_io.h"
#include "../../menu.h"

#define CONVERTED_DIR "games/PDP2011/.converted"

int pdp2011_tape_prepare(const char *name, char *out, size_t outlen)
{
	char src[1024];
	snprintf(src, sizeof(src), "%s", getFullPath(name));
	enum tape_format fmt = tape_detect(src);
	printf("PDP2011 tape %s: %s\n", name, tape_format_name(fmt));
	if (fmt != TAPE_TPC && fmt != TAPE_E11) return 0;

	struct stat st;
	if (stat(src, &st)) return 0;
	const char *base = strrchr(name, '/');
	base = base ? base + 1 : name;
	snprintf(out, outlen, "%s/%s.%lld-%lld.tap", CONVERTED_DIR, base,
		(long long)st.st_size, (long long)st.st_mtime);

	char dst[1024];
	snprintf(dst, sizeof(dst), "%s", getFullPath(out));
	if (tape_detect(dst) == TAPE_SIMH) return 1;    // converted earlier

	FileCreatePath(CONVERTED_DIR);
	char tmp[1040];
	snprintf(tmp, sizeof(tmp), "%s.part", dst);
	if (!tape_convert(src, tmp, fmt) || rename(tmp, dst))
	{
		remove(tmp);
		printf("PDP2011 tape %s: conversion failed, mounting as is\n", name);
		return 0;
	}
	char msg[64];
	snprintf(msg, sizeof(msg), "%s tape converted\nto SIMH format", tape_format_name(fmt));
	InfoMessage(msg);
	return 1;
}
#endif

#ifdef TAPECONV_MAIN
// Host test: tapeconv <src> [dst]  -- prints the detected format, converts.
int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: %s src [dst]\n", argv[0]); return 2; }
	enum tape_format fmt = tape_detect(argv[1]);
	printf("%s: %s\n", argv[1], tape_format_name(fmt));
	if (argc > 2 && (fmt == TAPE_TPC || fmt == TAPE_E11))
		return tape_convert(argv[1], argv[2], fmt) ? 0 : 1;
	return 0;
}
#endif
