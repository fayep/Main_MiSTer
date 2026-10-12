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
#include <sys/stat.h>

#include "tapelib.h"
#include "tapeconv.h"
#include "../../user_io.h"
#include "../../file_io.h"

#define TL_TYPE0    0x88
#define TL_TYPE1    0xB5
#define TL_TAPE     3           // the TM11's image slot
#define TL_MAXNAMES 256
#define TL_DATAMAX  1400
#define TL_PATHMAX  128         // longest path a request may carry
#define TL_DEPTH    4           // folders below the home directory

static const uint8_t lib_mac[6] = { 0x02, 0x50, 0x44, 0x50, 0x54, 0x4C };

// one path component: letters, digits, . _ - $ +, not hidden
static int is_plain(const char *n, int len)
{
	if (len < 1 || len > 64 || n[0] == '.') return 0;
	for (int i = 0; i < len; i++)
		if (!((n[i] >= 'A' && n[i] <= 'Z') || (n[i] >= 'a' && n[i] <= 'z') || (n[i] >= '0' && n[i] <= '9') ||
		      n[i] == '.' || n[i] == '_' || n[i] == '-' || n[i] == '$' || n[i] == '+'))
			return 0;
	return 1;
}

static int is_tape_name(const char *n)
{
	int len = strlen(n);
	if (len < 5 || !is_plain(n, len)) return 0;
	return !strcasecmp(n + len - 4, ".tap") || !strcasecmp(n + len - 4, ".tpc");
}

// a folder the library shows: plain, not _-prefixed (_Old, _Scratch)
static int is_shown_dir(const char *n)
{
	return n[0] != '_' && is_plain(n, strlen(n));
}

static int is_dir(const char *rel)
{
	struct stat64 *st = getPathStat(rel);
	return st && S_ISDIR(st->st_mode);
}

static int name_cmp(const void *a, const void *b)
{
	const char *x = *(char *const *)a, *y = *(char *const *)b;
	int dx = x[strlen(x) - 1] == '/', dy = y[strlen(y) - 1] == '/';
	if (dx != dy) return dy - dx;           // folders first
	return strcasecmp(x, y);
}

// any tape in rel (a path under /media/fat) or the folders below it
static int has_tapes(const char *rel, int depth)
{
	char full[1100], sub[1100];
	snprintf(full, sizeof(full), "%s", getFullPath(rel));
	DIR *d = opendir(full);
	if (!d) return 0;
	struct dirent *e;
	int found = 0;
	while (!found && (e = readdir(d)))
	{
		if (is_tape_name(e->d_name)) found = 1;
		else if (depth < TL_DEPTH && is_shown_dir(e->d_name))
		{
			snprintf(sub, sizeof(sub), "%s/%s", rel, e->d_name);
			found = is_dir(sub) && has_tapes(sub, depth + 1);
		}
	}
	closedir(d);
	return found;
}

// rel_in ("RSTS/V9.6/x.tap", any case, '/'-separated) -> out, the real path
// under /media/fat ("games/PDP2011/RSTS/V9.6/x.tap"), matching each component
// case-insensitively; want_dir: the last component must be a folder (else a
// tape). Returns 1 when found.
static int resolve(const char *rel_in, char *out, int outlen, int want_dir)
{
	char full[1100];
	int depth = 0;
	snprintf(out, outlen, "%s", HomeDir());
	const char *p = rel_in;
	while (*p == '/') p++;
	if (!*p) return want_dir;
	while (*p)
	{
		const char *s = strchr(p, '/');
		int len = s ? s - p : strlen(p);
		const char *next = s ? s + 1 : p + len;
		while (*next == '/') next++;
		int last = !*next;
		if (!is_plain(p, len) || ++depth > TL_DEPTH + 1) return 0;
		snprintf(full, sizeof(full), "%s", getFullPath(out));
		DIR *d = opendir(full);
		if (!d) return 0;
		struct dirent *e;
		int hit = 0;
		while ((e = readdir(d)))
			if ((int)strlen(e->d_name) == len && !strncasecmp(e->d_name, p, len))
			{
				int l = strlen(out);
				snprintf(out + l, outlen - l, "/%s", e->d_name);
				hit = 1;
				break;
			}
		closedir(d);
		if (!hit) return 0;
		if (!last || want_dir) { if (!is_dir(out)) return 0; }
		else if (is_dir(out) || !is_tape_name(strrchr(out, '/') + 1)) return 0;
		p = next;
	}
	return 1;
}

// a folder's entries, sorted, folders first as "NAME/"; returns how many (names malloc'd)
static int scan(const char *rel, char **names)
{
	char full[1100], sub[1100];
	snprintf(full, sizeof(full), "%s", getFullPath(rel));
	DIR *d = opendir(full);
	int n = 0;
	if (!d) return 0;
	struct dirent *e;
	while ((e = readdir(d)) && n < TL_MAXNAMES)
	{
		if (is_tape_name(e->d_name)) names[n++] = strdup(e->d_name);
		else if (is_shown_dir(e->d_name))
		{
			snprintf(sub, sizeof(sub), "%s/%s", rel, e->d_name);
			if (is_dir(sub) && has_tapes(sub, 1))
			{
				int l = strlen(e->d_name) + 2;
				names[n] = (char *)malloc(l);
				snprintf(names[n++], l, "%s/", e->d_name);
			}
		}
	}
	closedir(d);
	qsort(names, n, sizeof(char *), name_cmp);
	return n;
}

static void put16(uint8_t *p, int v) { p[0] = (v >> 8) & 0xFF; p[1] = v & 0xFF; }
static int get16(const uint8_t *p) { return (p[0] << 8) | p[1]; }

// the path (relative to the library) of the tape TLOAD last put on the drive:
// Main keeps only the file name of a mounted image
static char loaded[TL_PATHMAX + 64];

// "path\0format" for what the drive holds now: the library path when TLOAD
// loaded it, else (mounted from the OSD) the file name
static int describe(uint8_t *d)
{
	const char *n = get_image_name(TL_TAPE);
	if (!n) return -1;
	const char *b = strrchr(loaded, '/');
	if (loaded[0] && !strcmp(b ? b + 1 : loaded, n)) n = loaded;
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
	case 1:     // load: data = the tape's path in the library
	{
		char want[TL_PATHMAX + 1], path[1100];
		if (dlen < 1 || dlen > TL_PATHMAX) { r[5] = TL_BADNAME; break; }
		memcpy(want, q + 10, dlen);
		want[dlen] = 0;
		const char *base = strrchr(want, '/');
		if (!is_tape_name(base ? base + 1 : want)) { r[5] = TL_BADNAME; break; }
		if (!resolve(want, path, sizeof(path), 0)) r[5] = TL_NOFILE;
		else
		{
			printf("[pdp2011-tapelib] load %s\n", path);
			int hl = strlen(HomeDir());
			snprintf(loaded, sizeof(loaded), "%s", path + hl + 1);
			if (!user_io_file_mount(path, TL_TAPE)) { r[5] = TL_FAILED; loaded[0] = 0; }
			else rlen = describe(d);
		}
		if (rlen < 0) { rlen = 0; r[5] = TL_FAILED; }
		break;
	}
	case 2:     // unload
		printf("[pdp2011-tapelib] unload\n");
		user_io_file_mount("", TL_TAPE);
		break;
	case 3:     // directory: data = first index (2), then the folder's path (none: the top)
	{
		char *names[TL_MAXNAMES], want[TL_PATHMAX + 1], rel[1100];
		int first = dlen >= 2 ? get16(q + 10) : 0, plen = dlen - 2, here = 0;
		if (plen > TL_PATHMAX) { r[5] = TL_BADNAME; break; }
		if (plen < 0) plen = 0;
		memcpy(want, q + 12, plen);
		want[plen] = 0;
		if (!resolve(want, rel, sizeof(rel), 1)) { r[5] = TL_NODIR; break; }
		int n = scan(rel, names);
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
