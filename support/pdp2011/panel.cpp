#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

#include "panel.h"
#include "../../hardware.h"
#include "../../spi.h"
#include "../../user_io.h"

#define ODT_SOCK "/tmp/pdp2011.odt"

static void oct6(char *dst, unsigned v)
{
	for (int i = 5; i >= 0; i--) {
		dst[i] = '0' + (v & 7);
		v >>= 3;
	}
	dst[6] = 0;
}

struct panel_snap {
	uint16_t r7, psw, ir, flags, data, addr;
};

static void panel_read(struct panel_snap *s)
{
	spi_uio_cmd_cont(UIO_PDP_PANEL);
	s->r7 = spi_w(0);
	s->psw = spi_w(0);
	s->ir = spi_w(0);
	s->flags = spi_w(0);
	s->data = 0;
	s->addr = 0;
	if (s->flags & 8) {
		s->data = spi_w(0);
		s->addr = spi_w(0);
	}
	DisableIO();
}

int pdp2011_panel_banner(char lines[2][32], int force)
{
	static unsigned long next;
	static char cache[2][32];
	static int cached;

	if (!force && cached && next && !CheckTimer(next)) {
		snprintf(lines[0], 32, "%s", cache[0]);
		snprintf(lines[1], 32, "%s", cache[1]);
		return 0;
	}

	struct panel_snap s;
	panel_read(&s);

	next = GetTimer(50);

	char line0[32], line1[32];
	if (s.flags & 1) {
		char pc[7], sw[7], star[7];
		oct6(pc, s.r7);
		oct6(sw, s.psw);
		oct6(star, s.ir);
		snprintf(line0, sizeof(line0), " PC %s PSW %s  %s",
			pc, sw, (s.flags & 0x8000) ? " RUN" : "HALT");
		if (s.flags & 8) {
			char ma[7], d[7];
			oct6(ma, s.addr);
			oct6(d, s.data);
			if (s.flags & 2)
				snprintf(line1, sizeof(line1),
					(s.flags & 4) ? "*PC %s MA %s D   NXM" : "*PC %s MA %s D %s",
					star, ma, d);
			else
				snprintf(line1, sizeof(line1),
					(s.flags & 4) ? "*PC ------ MA %s D   NXM" : "*PC ------ MA %s D %s",
					ma, d);
		} else if (s.flags & 2) {
			snprintf(line1, sizeof(line1), "*PC %s", star);
		} else {
			snprintf(line1, sizeof(line1), "*PC ------");
		}
	} else {
		snprintf(line0, sizeof(line0), " PC ------ PSW ------  ----");
		snprintf(line1, sizeof(line1), "*PC ------");
	}

	int changed = !cached || strcmp(cache[0], line0) || strcmp(cache[1], line1);
	snprintf(cache[0], sizeof(cache[0]), "%s", line0);
	snprintf(cache[1], sizeof(cache[1]), "%s", line1);
	cached = 1;
	snprintf(lines[0], 32, "%s", cache[0]);
	snprintf(lines[1], 32, "%s", cache[1]);
	return force || changed;
}

static void fmt_snap(char *out, size_t n, const struct panel_snap *s)
{
	char pc[7], psw[7], ir[7], ma[7], d[7];
	oct6(pc, s->r7);
	oct6(psw, s->psw);
	oct6(ir, s->ir);
	oct6(ma, s->addr);
	oct6(d, s->data);
	snprintf(out, n, "PC %s PSW %s *PC %s MA %s D %s %s%s\n",
		pc, psw, (s->flags & 2) ? ir : "------",
		ma, (s->flags & 4) ? "NXM   " : d,
		(s->flags & 0x8000) ? "RUN" : "HALT",
		(s->flags & 1) ? "" : " (no panel_dbg)");
}

static void set_sr(uint32_t v)
{
	int sr22 = (v & 017600000) ? 1 : 0;
	uint16_t lo = (uint16_t)(v & 0177777);
	user_io_status_set("[38]", sr22);
	user_io_status_set("[36]", (lo >> 15) & 1);
	user_io_status_set("[35:33]", (lo >> 12) & 7);
	user_io_status_set("[32:30]", (lo >> 9) & 7);
	user_io_status_set("[29:27]", (lo >> 6) & 7);
	user_io_status_set("[26:24]", (lo >> 3) & 7);
	user_io_status_set("[23:21]", lo & 7);
}

static void pulse(const char *opt)
{
	user_io_status_set(opt, 0);
	WaitTimer(2);
	user_io_status_set(opt, 1);
	WaitTimer(4);
	user_io_status_set(opt, 0);
	WaitTimer(4);
}

static void ensure_halt(void)
{
	user_io_status_set("[16]", 1);
	WaitTimer(4);
}

/* Resume full-speed execution: drop the Halt toggle so cons_ena is 1
 * before the Continue pulse, otherwise state_halt takes the pulse as a
 * single-step (consolestep) and drops straight back to HALT. */
static void resume_run(void)
{
	user_io_status_set("[16]", 0);
	WaitTimer(4);
	pulse("[17]");
}

static int parse_oct(const char *s, uint32_t *out)
{
	char *end = 0;
	while (*s == ' ' || *s == '\t') s++;
	if (!*s) return 0;
	unsigned long v = strtoul(s, &end, 8);
	if (end == s) return 0;
	*out = (uint32_t)v;
	return 1;
}

static void odt_reply(int fd, const char *s)
{
	if (fd < 0 || !s) return;
	size_t n = strlen(s);
	if (n) (void)write(fd, s, n);
	(void)write(fd, ".\n", 2);
}

/* tracecap.vhd's default DEPTH_LOG2=14 (16384 entries) -- see
 * rtl/tracecap_pkg.vhd / rtl/tracecap.vhd. If that generic ever
 * changes, this needs to change with it. */
#define TRACE_DEPTH 16384

/* Drains the tracecap.vhd/tracecap_dbg.sv event ring (EXT_BUS command
 * 0x51) and writes one line per event to `fd`, terminated the same
 * "body then a lone '.' line" way odt_reply() does -- unlike every
 * other command here this can be many lines, so it writes directly
 * rather than building one reply[] buffer. Word-packing matches
 * rtl/tracecap_dbg.sv's own header comment exactly:
 *   word0=flags, word1=wr_ptr, word2=reserved, then per event:
 *   {kind,id,0}, a_hi, a_lo, b_hi, b_lo, c, kdpar5, kdpar6, kipar5, kipar6.
 * kdpar5/kdpar6/kipar5/kipar6 are disk events only: rl11.vhd/rh11.vhd's
 * live copies of mmu.vhd's KERNEL D-space AND I-space PAR5/PAR6,
 * sampled at the same READ+GO trigger moment as a/b/c -- see
 * tracecap_pkg.vhd's TRACE_D_WIDTH comment. kipar5/kipar6 (17772352/
 * 17772354) are the pair RSTS's real overlay-mapping mechanism actually
 * uses (notes/rsts-init-disasm.md's MAPCOPY_PARAM); kdpar5/kdpar6
 * (17772372/17772374, D-space) were this session's original wrong
 * guess at which pair mattered, kept anyway since something else in
 * RSTS does write real values there too. No standalone PAR-write event
 * kind: PAR5/6 change far too often to log as their own events without
 * drowning every disk event (confirmed on real hardware: 16222 of
 * 16384 ring entries were PAR-write events, zero disk events
 * survived).
 */
/* `start`/`want` window the drain: only events [start, start+want) are
 * formatted and sent. want<0 means "to the end". tracecap_dbg.sv has
 * no seek -- it only walks rd_addr forward one step per spi_w() call
 * -- so a window is implemented by silently discarding the leading
 * words we don't want, then STOPPING once we have `want` events
 * instead of always draining the whole ring. That's a real, useful
 * property on its own (a small window costs only start+want word
 * transfers, not TRACE_DEPTH*10), independent of debugging anything --
 * added after a full 16384-event drain proved unreliable enough in
 * practice to want a cheaper, boundable alternative. */
static void trace_dump(int fd, int start, int want)
{
	/* Batched into a big buffer and flushed in chunks rather than one
	 * write() syscall per event line -- draining many events is
	 * already a lot of real SPI transfer work (10 words/event); adding
	 * one socket syscall per event on top of that was real, avoidable
	 * overhead that made a full drain slow enough to need a longer
	 * client-side timeout (see pdp-odt's own comment on this). */
	/* Deliberately small (~40 events/flush, not ~320) so a hang partway
	 * through the loop still shows real progress to the client instead
	 * of looking identical to a hang on the very first transfer -- see
	 * the flush right after the header line below for the same reason. */
	static char buf[2048];
	size_t used = 0;

	/* spi_uio_cmd_cont() itself returns the FPGA's response to the
	 * command word (tracecap_dbg.sv's cnt==0 beat, i.e. `flags`) --
	 * discarding it the way panel_read() discards panel_dbg.sv's own
	 * cnt==0 beat would silently shift every later word back by one,
	 * which is exactly the bug this comment is here to stop someone
	 * (including a future me) from reintroducing. */
	uint16_t flags = spi_uio_cmd_cont(UIO_PDP_TRACE);
	uint16_t wr_ptr = spi_w(0);
	(void)spi_w(0);  /* reserved */

	if (!(flags & 1)) {
		DisableIO();
		odt_reply(fd, "ERR no tracecap_dbg (module not present)\n");
		return;
	}
	int overflowed = (flags >> 1) & 1;
	int total = overflowed ? TRACE_DEPTH : wr_ptr;

	if (start < 0) start = 0;
	if (start > total) start = total;
	int end = (want < 0) ? total : start + want;
	if (end > total) end = total;

	used += snprintf(buf + used, sizeof(buf) - used,
		"count=%d wrap=%d start=%d end=%d\n", total, overflowed, start, end);
	/* Flush the header immediately, before the loop -- otherwise a hang
	 * anywhere in the per-event loop below is indistinguishable from a
	 * hang on the very first transfer (both look like "zero bytes
	 * received" to the client), which defeats debugging exactly the
	 * kind of problem this is here to catch. */
	write(fd, buf, used);
	used = 0;

	/* Skip leading events outside the window -- still real SPI work
	 * (there's no seek), but no formatting/writing, and critically we
	 * stop entirely once `end` is reached rather than draining the
	 * rest of the ring unconditionally. */
	for (int i = 0; i < start; i++) {
		/* fpga_spi() busy-waits on the GPIO ACK with no yield of its
		 * own; a full drain is ~10 of those per event back-to-back and
		 * would hold this single-threaded process's CPU for the whole
		 * drain, starving everything else it owns (and, on a long
		 * enough drain, getting it killed). Give the scheduler a slot
		 * every 32 events -- ~100us each, a few tens of ms total even
		 * for a 16k-deep buffer, imperceptible next to the SPI cost. */
		if ((i & 31) == 0) usleep(100);
		spi_w(0); spi_w(0); spi_w(0); spi_w(0); spi_w(0);
		spi_w(0); spi_w(0); spi_w(0); spi_w(0); spi_w(0);
	}

	for (int i = start; i < end; i++) {
		if ((i & 31) == 0) usleep(100);
		uint16_t w0 = spi_w(0);
		uint16_t a_hi = spi_w(0);
		uint16_t a_lo = spi_w(0);
		uint16_t b_hi = spi_w(0);
		uint16_t b_lo = spi_w(0);
		uint16_t c = spi_w(0);
		uint16_t kdpar5 = spi_w(0);
		uint16_t kdpar6 = spi_w(0);
		uint16_t kipar5 = spi_w(0);
		uint16_t kipar6 = spi_w(0);

		unsigned kind = (w0 >> 12) & 0xF;
		unsigned id = (w0 >> 8) & 0xF;
		uint32_t a = ((uint32_t)(a_hi & 0x3F) << 16) | a_lo;
		uint32_t b = ((uint32_t)(b_hi & 0x3F) << 16) | b_lo;

		/* Longest possible line is well under 128 bytes; leave a safety
		 * margin before flushing so a line is never split mid-write. */
		if (used + 128 > sizeof(buf)) {
			write(fd, buf, used);
			used = 0;
		}
		used += snprintf(buf + used, sizeof(buf) - used,
			"%4d kind=%o id=%o a=%07o b=%07o c=%06o kdpar5=%06o kdpar6=%06o kipar5=%06o kipar6=%06o\n",
			i, kind, id, a, b, c, kdpar5, kdpar6, kipar5, kipar6);
	}
	DisableIO();

	if (used) write(fd, buf, used);
	write(fd, ".\n", 2);
}

static void odt_cmd(int fd, char *line)
{
	char reply[256];
	struct panel_snap s;
	uint32_t a = 0, d = 0;
	char *cmd = line;
	while (*cmd == ' ' || *cmd == '\t') cmd++;
	char *arg = cmd;
	while (*arg && *arg != ' ' && *arg != '\t') arg++;
	if (*arg) {
		*arg++ = 0;
		while (*arg == ' ' || *arg == '\t') arg++;
	}

	if (!cmd[0] || !strcmp(cmd, "snap") || !strcmp(cmd, "?")) {
		panel_read(&s);
		fmt_snap(reply, sizeof(reply), &s);
		odt_reply(fd, reply);
		return;
	}
	if (!strcmp(cmd, "help")) {
		odt_reply(fd,
			"snap halt run|cont step start\n"
			"load <oct>  exa  dep <oct>  sr <oct>\n"
			"peek <oct>  poke <oct> <oct>\n"
			"r7 <oct>    (17600000 + 177707)\n"
			"trace [<start> <count>]  dump tracecap ring (kind: 1=disk\n"
			"            2=parw); start/count are plain DECIMAL event\n"
			"            indices (not octal -- they're array indices,\n"
			"            not PDP-11 values), and a window costs only\n"
			"            start+count SPI transfers, not the whole ring\n"
			"break <oct> arm a PC-compare breakpoint: halts (like a\n"
			"            manual halt, clearable by cont/run) when PC\n"
			"            next reaches <oct>. Does not halt the CPU to\n"
			"            arm it -- fires on a later, real arrival.\n"
			"unbreak     disarm the breakpoint\n");
		return;
	}
	if (!strcmp(cmd, "trace")) {
		int t_start = 0, t_count = -1;
		if (*arg) sscanf(arg, "%d %d", &t_start, &t_count);
		trace_dump(fd, t_start, t_count);
		return;
	}
	/* break <oct>: arm a PC-compare breakpoint (rtl/brk_compare.vhd) --
	 * halts the CPU (same as a manual halt, clearable by "cont"/"run"
	 * the same way) the next time its PC reaches <oct>. Deliberately
	 * does NOT call ensure_halt() -- the whole point is to arm this
	 * while the CPU keeps running, so it can catch a real, naturally
	 * occurring arrival at that address later. */
	if (!strcmp(cmd, "break")) {
		if (!parse_oct(arg, &a)) { odt_reply(fd, "ERR break <oct>\n"); return; }
		spi_uio_cmd_cont(UIO_PDP_BRK);
		spi_w(1);
		spi_w((uint16_t)a);
		DisableIO();
		odt_reply(fd, "OK\n");
		return;
	}
	if (!strcmp(cmd, "unbreak")) {
		spi_uio_cmd_cont(UIO_PDP_BRK);
		spi_w(0);
		spi_w(0);
		DisableIO();
		odt_reply(fd, "OK\n");
		return;
	}
	if (!strcmp(cmd, "halt")) {
		user_io_status_set("[16]", 1);
		WaitTimer(4);
		panel_read(&s);
		fmt_snap(reply, sizeof(reply), &s);
		odt_reply(fd, reply);
		return;
	}
	/* run / cont / go: resume full-speed execution (e.g. after peek/exa,
	 * which leave the CPU halted). */
	if (!strcmp(cmd, "run") || !strcmp(cmd, "go") ||
	    !strcmp(cmd, "cont") || !strcmp(cmd, "c")) {
		resume_run();
		panel_read(&s);
		fmt_snap(reply, sizeof(reply), &s);
		odt_reply(fd, reply);
		return;
	}
	/* step / s: execute a single instruction and halt again. */
	if (!strcmp(cmd, "step") || !strcmp(cmd, "s")) {
		ensure_halt();
		pulse("[17]");
		panel_read(&s);
		fmt_snap(reply, sizeof(reply), &s);
		odt_reply(fd, reply);
		return;
	}
	if (!strcmp(cmd, "start")) {
		odt_reply(fd, "ERR start resets the CPU; use r7 + cont\n");
		return;
	}
	if (!strcmp(cmd, "sr")) {
		if (!parse_oct(arg, &a)) { odt_reply(fd, "ERR sr <oct>\n"); return; }
		set_sr(a);
		odt_reply(fd, "OK\n");
		return;
	}
	if (!strcmp(cmd, "load") || !strcmp(cmd, "l")) {
		if (!parse_oct(arg, &a)) { odt_reply(fd, "ERR load <oct>\n"); return; }
		ensure_halt();
		set_sr(a);
		pulse("[19]");
		panel_read(&s);
		fmt_snap(reply, sizeof(reply), &s);
		odt_reply(fd, reply);
		return;
	}
	if (!strcmp(cmd, "exa") || !strcmp(cmd, "e")) {
		ensure_halt();
		pulse("[20]");
		panel_read(&s);
		fmt_snap(reply, sizeof(reply), &s);
		odt_reply(fd, reply);
		return;
	}
	if (!strcmp(cmd, "dep") || !strcmp(cmd, "d")) {
		if (!parse_oct(arg, &a)) { odt_reply(fd, "ERR dep <oct>\n"); return; }
		ensure_halt();
		set_sr(a);
		pulse("[37]");
		panel_read(&s);
		fmt_snap(reply, sizeof(reply), &s);
		odt_reply(fd, reply);
		return;
	}
	if (!strcmp(cmd, "peek")) {
		if (!parse_oct(arg, &a)) { odt_reply(fd, "ERR peek <oct>\n"); return; }
		ensure_halt();
		set_sr(a);
		pulse("[19]");
		pulse("[20]");
		panel_read(&s);
		fmt_snap(reply, sizeof(reply), &s);
		odt_reply(fd, reply);
		return;
	}
	if (!strcmp(cmd, "poke")) {
		char *sp = arg;
		while (*sp && *sp != ' ' && *sp != '\t') sp++;
		if (*sp) *sp++ = 0;
		while (*sp == ' ' || *sp == '\t') sp++;
		if (!parse_oct(arg, &a) || !parse_oct(sp, &d)) {
			odt_reply(fd, "ERR poke <oct> <oct>\n");
			return;
		}
		ensure_halt();
		set_sr(a);
		pulse("[19]");
		set_sr(d);
		pulse("[37]");
		panel_read(&s);
		fmt_snap(reply, sizeof(reply), &s);
		odt_reply(fd, reply);
		return;
	}
	if (!strcmp(cmd, "r7")) {
		if (!parse_oct(arg, &a)) { odt_reply(fd, "ERR r7 <oct>\n"); return; }
		ensure_halt();
		set_sr(017600000 | 0177707);
		pulse("[19]");
		set_sr(a & 0177777);
		pulse("[37]");
		set_sr(0);
		panel_read(&s);
		fmt_snap(reply, sizeof(reply), &s);
		odt_reply(fd, reply);
		return;
	}

	snprintf(reply, sizeof(reply), "ERR unknown '%s'\n", cmd);
	odt_reply(fd, reply);
}

static int listen_fd = -1;
static int client_fd = -1;
static char inbuf[256];
static int inlen;

static void odt_close_client(void)
{
	if (client_fd >= 0) {
		close(client_fd);
		client_fd = -1;
	}
	inlen = 0;
}

static void odt_listen(void)
{
	if (listen_fd >= 0) return;
	unlink(ODT_SOCK);
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) return;
	int fl = fcntl(fd, F_GETFL, 0);
	if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	struct sockaddr_un a;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	strncpy(a.sun_path, ODT_SOCK, sizeof(a.sun_path) - 1);
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		close(fd);
		return;
	}
	chmod(ODT_SOCK, 0666);
	if (listen(fd, 2) < 0) {
		close(fd);
		unlink(ODT_SOCK);
		return;
	}
	listen_fd = fd;
}

void pdp2011_odt_poll()
{
	if (!is_pdp2011()) {
		if (listen_fd >= 0) {
			odt_close_client();
			close(listen_fd);
			listen_fd = -1;
			unlink(ODT_SOCK);
		}
		return;
	}

	odt_listen();
	if (listen_fd < 0) return;

	if (client_fd < 0) {
		int c = accept(listen_fd, 0, 0);
		if (c < 0) return;
		int fl = fcntl(c, F_GETFL, 0);
		if (fl >= 0) fcntl(c, F_SETFL, fl | O_NONBLOCK);
		client_fd = c;
		inlen = 0;
	}

	char tmp[128];
	ssize_t n = read(client_fd, tmp, sizeof(tmp));
	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) return;
		odt_close_client();
		return;
	}
	if (n == 0) {
		odt_close_client();
		return;
	}
	for (ssize_t i = 0; i < n; i++) {
		char ch = tmp[i];
		if (ch == '\r') continue;
		if (ch == '\n') {
			inbuf[inlen] = 0;
			inlen = 0;
			if (inbuf[0]) odt_cmd(client_fd, inbuf);
			else {
				struct panel_snap s;
				char reply[256];
				panel_read(&s);
				fmt_snap(reply, sizeof(reply), &s);
				odt_reply(client_fd, reply);
			}
			continue;
		}
		if (inlen < (int)sizeof(inbuf) - 1)
			inbuf[inlen++] = ch;
	}
}
