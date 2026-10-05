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
#include <algorithm>
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

// Quick check: the first few records parse as SIMH (lengths match their
// trailers). Real TPC/E11 images fail this at once (see below), so a SIMH
// tape is recognised without reading the whole file; the full walk is only
// needed for everything else. Keeps the mount instant: the core may already
// be booting from the tape.
static int simh_quick(FILE *f)
{
	long n = fsize(f), pos = 0;
	uint8_t h[4], t[4];
	int recs = 0, marks = 0;
	fseek(f, 0, SEEK_SET);
	while (pos < n && recs < 8 && marks < 16)
	{
		if (pos + 4 > n || !rd(f, h, 4)) break;
		uint32_t len = le32(h);
		pos += 4;
		if (len == 0) { marks++; continue; }
		if (len == 0xFFFFFFFF) break;
		uint32_t l = len & 0x00FFFFFF;
		long skip = l + (l & 1);
		if (pos + skip + 4 > n) return 0;
		fseek(f, skip, SEEK_CUR); pos += skip;
		if (!rd(f, t, 4) || le32(t) != len) return 0;
		pos += 4; recs++;
	}
	// (An E11 image is identical to SIMH until its first odd-length record;
	// one whose first 8 records are even is taken as SIMH here. Rare: real
	// kits use 512-byte blocks and 80-byte labels.)
	return recs > 0;
}

// Quick TPC check: the first records form a chain of 16-bit lengths with
// even-padded data that stays inside the file.
static int tpc_quick(FILE *f)
{
	long n = fsize(f), pos = 0;
	uint8_t h[2];
	int recs = 0, marks = 0;
	fseek(f, 0, SEEK_SET);
	while (pos < n && recs < 8 && marks < 16)
	{
		if (pos + 2 > n || !rd(f, h, 2)) return 0;
		uint16_t len = le16(h);
		pos += 2;
		if (len == 0) { marks++; continue; }
		long skip = len + (len & 1);
		if (pos + skip > n) return 0;
		fseek(f, skip, SEEK_CUR); pos += skip; recs++;
	}
	return recs > 0;
}

// Quick E11 check: SIMH framing without padding for the first records, and
// at least one odd-length record among them (otherwise it is plain SIMH).
static int e11_quick(FILE *f)
{
	long n = fsize(f), pos = 0;
	uint8_t h[4], t[4];
	int recs = 0, odd = 0;
	fseek(f, 0, SEEK_SET);
	while (pos < n && recs < 8)
	{
		if (pos + 4 > n || !rd(f, h, 4)) return 0;
		uint32_t len = le32(h);
		pos += 4;
		if (len == 0) continue;
		if (len == 0xFFFFFFFF) break;
		uint32_t l = len & 0x00FFFFFF;
		if (pos + (long)l + 4 > n) return 0;
		fseek(f, l, SEEK_CUR); pos += l;
		if (!rd(f, t, 4) || le32(t) != len) return 0;
		pos += 4; recs++; odd |= l & 1;
	}
	return recs > 0 && odd;
}

enum tape_format tape_detect(const char *path)
{
	FILE *f = fopen(path, "rbe");
	if (!f) return TAPE_UNKNOWN;
	if (simh_quick(f)) { fclose(f); return TAPE_SIMH; }
	// E11 before TPC: E11 data read as 16-bit lengths can look like a valid
	// TPC chain, while a TPC image never has E11's repeated 32-bit lengths.
	if (e11_quick(f)) { fclose(f); return TAPE_E11; }
	if (tpc_quick(f)) { fclose(f); return TAPE_TPC; }
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
	FILE *f = fopen(src, "rbe");
	if (!f) return 0;
	FILE *o = fopen(dst, "wbe");
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

// ---- streaming: serve a TPC/E11 image to the TM11 as a SIMH stream ----
//
// The core reads the tape slot as a SIMH .tap file in 512-byte blocks. For a
// TPC or E11 image nothing is converted: an index of records (where each one
// starts in the virtual SIMH stream and in the source) is built from the
// record headers as far as reads reach, and each block is assembled on the
// fly. Past the last record comes an end-of-medium marker (FFFFFFFF), which
// the TM11 reports as EOT. Rewinds and reverse spacing reuse the index.

#include <vector>

struct tape_rec
{
	uint64_t voff;   // offset of this record's SIMH header in the virtual stream
	uint64_t soff;   // offset of its data in the source file
	uint32_t len;    // data length; 0 = tape mark
};

struct tape_stream
{
	FILE *f = NULL;
	enum tape_format fmt = TAPE_UNKNOWN;
	long fsz = 0;
	std::vector<tape_rec> idx;
	uint64_t next_soff = 0;   // source offset of the next unindexed header
	uint64_t next_voff = 0;   // virtual offset where it will start
	int done = 0;             // all records indexed; the EOM follows at next_voff
};

static tape_stream streams[4];

static uint64_t vsize(uint32_t len) { return len ? 8 + len + (len & 1) : 4; }

// Index one more source record; returns 0 at the end of the source.
static int index_next(tape_stream &t)
{
	if (t.done) return 0;
	uint8_t h[4];
	uint32_t len;
	uint64_t data, after;
	if (fseek(t.f, t.next_soff, SEEK_SET)) { t.done = 1; return 0; }
	if (t.fmt == TAPE_TPC)
	{
		if (t.next_soff + 2 > (uint64_t)t.fsz || !rd(t.f, h, 2)) { t.done = 1; return 0; }
		len = le16(h);
		data = t.next_soff + 2;
		after = data + len + (len & 1);
	}
	else  // E11: SIMH framing, odd records not padded
	{
		if (t.next_soff + 4 > (uint64_t)t.fsz || !rd(t.f, h, 4)) { t.done = 1; return 0; }
		len = le32(h);
		if (len == 0xFFFFFFFF) { t.done = 1; return 0; }
		len &= 0x00FFFFFF;
		data = t.next_soff + 4;
		after = data + len + (len ? 4 : 0);
	}
	if (after > (uint64_t)t.fsz) { t.done = 1; return 0; }   // truncated record: end here
	t.idx.push_back({ t.next_voff, data, len });
	t.next_voff += vsize(len);
	t.next_soff = after;
	return 1;
}

void pdp2011_tape_attach(int slot, const char *path, enum tape_format fmt)
{
	tape_stream &t = streams[slot];
	if (t.f) fclose(t.f);
	t = tape_stream();
	if (fmt != TAPE_TPC && fmt != TAPE_E11) return;
	t.f = fopen(path, "rbe");
	if (!t.f) return;
	t.fmt = fmt;
	t.fsz = fsize(t.f);
	printf("PDP2011 tape: serving %s image %s as SIMH\n", tape_format_name(fmt), path);
}

void pdp2011_tape_detach(int slot)
{
	tape_stream &t = streams[slot];
	if (t.f) fclose(t.f);
	t = tape_stream();
}

int pdp2011_tape_streaming(int slot)
{
	return slot >= 0 && slot < 4 && streams[slot].f != NULL;
}

// Fill buf with len bytes of the virtual SIMH stream starting at off.
int pdp2011_tape_read(int slot, uint64_t off, uint8_t *buf, uint32_t len)
{
	tape_stream &t = streams[slot];
	if (!t.f) return 0;
	while (!t.done && t.next_voff <= off + len) index_next(t);

	// first record that ends after off
	size_t lo = 0, hi = t.idx.size();
	while (lo < hi)
	{
		size_t mid = (lo + hi) / 2;
		if (t.idx[mid].voff + vsize(t.idx[mid].len) <= off) lo = mid + 1; else hi = mid;
	}

	uint32_t o = 0;
	for (size_t i = lo; o < len && i < t.idx.size(); i++)
	{
		const tape_rec &r = t.idx[i];
		uint8_t hdr[4] = { (uint8_t)r.len, (uint8_t)(r.len >> 8), (uint8_t)(r.len >> 16), (uint8_t)(r.len >> 24) };
		uint64_t rs = r.voff, re = r.voff + vsize(r.len);
		uint64_t p = off + o;
		while (p < re && o < len)
		{
			uint64_t k = p - rs;                       // position inside the SIMH record
			uint32_t pad = r.len & 1;
			if (k < 4) { buf[o++] = hdr[k]; p++; continue; }
			if (r.len == 0) break;
			if (k < 4 + (uint64_t)r.len)
			{
				uint32_t n = (uint32_t)std::min<uint64_t>(4 + r.len - k, len - o);
				if (fseek(t.f, r.soff + (k - 4), SEEK_SET) || fread(buf + o, 1, n, t.f) != n)
					memset(buf + o, 0, n);
				o += n; p += n; continue;
			}
			if (pad && k == 4 + (uint64_t)r.len) { buf[o++] = 0; p++; continue; }
			buf[o++] = hdr[k - 4 - r.len - pad]; p++;
		}
	}
	if (o < len) memset(buf + o, 0xFF, len - o);         // end of medium, and past it
	return 1;
}

#ifdef TAPECONV_MAIN
// Host test: tapeconv <src> [dst]  -- prints the detected format, converts.
int main(int argc, char **argv)
{
	if (argc == 4 && !strcmp(argv[1], "--stream"))
	{
		// read the virtual SIMH stream in random-sized chunks at random-ish
		// order (forward, then a re-read from the start) and write it out
		enum tape_format fmt = tape_detect(argv[2]);
		pdp2011_tape_attach(0, argv[2], fmt);
		if (!pdp2011_tape_streaming(0)) { fprintf(stderr, "not streamable (%s)\n", tape_format_name(fmt)); return 1; }
		FILE *o = fopen(argv[3], "wb");
		uint8_t buf[70000];
		uint64_t off = 0;
		srand(1);
		for (;;)
		{
			uint32_t n = 1 + rand() % 65536;
			pdp2011_tape_read(0, off, buf, n);
			uint32_t i;
			for (i = 0; i + 4 <= n; i++)                      // stop after the EOM marker
				if (buf[i] == 0xFF && buf[i+1] == 0xFF && buf[i+2] == 0xFF && buf[i+3] == 0xFF
				    && off + i == streams[0].next_voff && streams[0].done) break;
			if (i + 4 <= n) { fwrite(buf, 1, i + 4, o); break; }
			fwrite(buf, 1, n, o); off += n;
			if (off > 4000000000ULL) break;
		}
		fclose(o);
		// spot re-read from the start must match what was written
		uint8_t a[512], b[512];
		FILE *chk = fopen(argv[3], "rb");
		for (uint64_t pos = 0; ; pos += 4093)
		{
			if (fseek(chk, pos, SEEK_SET) || fread(a, 1, 512, chk) != 512) break;
			pdp2011_tape_read(0, pos, b, 512);
			if (memcmp(a, b, 512)) { printf("re-read mismatch at %llu\n", (unsigned long long)pos); return 1; }
		}
		fclose(chk);
		pdp2011_tape_detach(0);
		return 0;
	}
	if (argc < 2) { fprintf(stderr, "usage: %s src [dst] | --stream src dst\n", argv[0]); return 2; }
	enum tape_format fmt = tape_detect(argv[1]);
	printf("%s: %s\n", argv[1], tape_format_name(fmt));
	if (argc > 2 && (fmt == TAPE_TPC || fmt == TAPE_E11))
		return tape_convert(argv[1], argv[2], fmt) ? 0 : 1;
	return 0;
}
#endif
