// tapelib.cpp -- PDP2011: a tape library the guest drives over the XU, see
// tapelib.h. Mounting goes through user_io_file_mount, exactly as the OSD
// does it: a TPC image is streamed as SIMH (tapeconv.cpp), and the TM11
// sees a media change and re-homes to BOT.

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <dirent.h>

#include "tapelib.h"
#include "tapeconv.h"
#include "../../user_io.h"
#include "../../file_io.h"

#define TL_TYPE0    0x88
#define TL_TYPE1    0xB5
#define TL_TAPE     3           // the TM11's image slot
#define TL_MAXNAMES 256
#define TL_DATAMAX  1400

static const uint8_t lib_mac[6] = { 0x02, 0x50, 0x44, 0x50, 0x54, 0x4C };

static int is_tape_name(const char *n)
{
	int len = strlen(n);
	if (len < 5 || len > 64) return 0;
	if (strcasecmp(n + len - 4, ".tap") && strcasecmp(n + len - 4, ".tpc")) return 0;
	for (const char *p = n; *p; p++)
		if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
		      *p == '.' || *p == '_' || *p == '-' || *p == '$'))
			return 0;
	return n[0] != '.';
}

static int name_cmp(const void *a, const void *b)
{
	return strcasecmp(*(char *const *)a, *(char *const *)b);
}

// the library's tapes, sorted; returns how many (names malloc'd)
static int scan(char **names)
{
	const char *home = getFullPath(HomeDir());
	DIR *d = opendir(home);
	int n = 0;
	if (!d) return 0;
	struct dirent *e;
	while ((e = readdir(d)) && n < TL_MAXNAMES)
		if (is_tape_name(e->d_name)) names[n++] = strdup(e->d_name);
	closedir(d);
	qsort(names, n, sizeof(char *), name_cmp);
	return n;
}

static void put16(uint8_t *p, int v) { p[0] = (v >> 8) & 0xFF; p[1] = v & 0xFF; }
static int get16(const uint8_t *p) { return (p[0] << 8) | p[1]; }

// "name\0format" for what the drive holds now
static int describe(uint8_t *d)
{
	const char *n = get_image_name(TL_TAPE);
	if (!n) return -1;
	int len = snprintf((char *)d, 200, "%s", n) + 1;
	const char *fmt = pdp2011_tape_streaming(TL_TAPE) ? "TPC/E11, streamed as SIMH (read-only)" : "SIMH";
	len += snprintf((char *)d + len, 200, "%s", fmt);
	return len;
}

int pdp2011_tapelib_frame(const uint8_t *f, int len, uint8_t *reply)
{
	if (len < 14 + 10 || f[12] != TL_TYPE0 || f[13] != TL_TYPE1 || memcmp(f + 14, "TLIB", 4))
		return 0;

	const uint8_t *q = f + 14;
	int op = q[4], dlen = get16(q + 8);
	if (dlen > len - 24) dlen = len - 24;
	if (dlen < 0) dlen = 0;

	memcpy(reply, f + 6, 6);                // back to the sender
	memcpy(reply + 6, lib_mac, 6);
	reply[12] = TL_TYPE0; reply[13] = TL_TYPE1;
	uint8_t *r = reply + 14, *d = r + 10;
	memcpy(r, "TLIB", 4);
	r[4] = op | 0x80;
	r[5] = TL_OK;
	r[6] = q[6]; r[7] = q[7];
	int rlen = 0;

	switch (op)
	{
	case 1:     // load
	{
		char want[72], *names[TL_MAXNAMES], path[1100];
		if (dlen < 1 || dlen > 64) { r[5] = TL_BADNAME; break; }
		memcpy(want, q + 10, dlen);
		want[dlen] = 0;
		if (!is_tape_name(want)) { r[5] = TL_BADNAME; break; }
		int n = scan(names), hit = -1;
		for (int i = 0; i < n; i++) if (!strcasecmp(names[i], want)) hit = i;
		if (hit < 0) r[5] = TL_NOFILE;
		else
		{
			snprintf(path, sizeof(path), "%s/%s", HomeDir(), names[hit]);
			printf("[pdp2011-tapelib] load %s\n", path);
			if (!user_io_file_mount(path, TL_TAPE)) r[5] = TL_FAILED;
			else rlen = describe(d);
		}
		for (int i = 0; i < n; i++) free(names[i]);
		if (rlen < 0) { rlen = 0; r[5] = TL_FAILED; }
		break;
	}
	case 2:     // unload
		printf("[pdp2011-tapelib] unload\n");
		user_io_file_mount("", TL_TAPE);
		break;
	case 3:     // directory, from an index
	{
		char *names[TL_MAXNAMES];
		int n = scan(names), first = dlen >= 2 ? get16(q + 10) : 0, here = 0;
		rlen = 4;
		for (int i = first; i < n; i++)
		{
			int l = strlen(names[i]) + 1;
			if (rlen + l > TL_DATAMAX) break;
			memcpy(d + rlen, names[i], l);
			rlen += l;
			here++;
		}
		put16(d, n);
		put16(d + 2, here);
		for (int i = 0; i < n; i++) free(names[i]);
		break;
	}
	case 4:     // status
		rlen = describe(d);
		if (rlen < 0) { rlen = 0; r[5] = TL_EMPTY; }
		break;
	default:
		r[5] = TL_BADOP;
	}

	put16(r + 8, rlen);
	int total = 14 + 10 + rlen;
	if (total < 60) { memset(reply + total, 0, 60 - total); total = 60; }
	return total;
}
