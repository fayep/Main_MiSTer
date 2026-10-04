// tu58.cpp -- PDP2011: run a TU58 DECtape II emulator on the serial port
//
// With the console on the virtual VT100, the core puts KL11 line 1 (176500,
// vector 300) on the UART, which is /dev/ttyS1 here: the standard TU58
// address (RT-11/RSX DD:, XXDP DD). When the OSD also selects "Line 1 =
// TU58", tu58fs is started on ttyS1; when either setting changes, or another
// core is loaded, it is stopped with SIGTERM, which makes tu58fs save its
// cartridges (patched tu58fs, branch mister-sigterm).
//
// Speed: the KL11 lines run at 19200 (kl1_bps in mister_top) and the core
// declares UART19200, so tu58fs uses 19200 too.
//
// Files, all in games/PDP2011/tu58/: tu58fs (the program), dd0.dsk and
// dd1.dsk (cartridges for units 0 and 1, created as empty RT-11 tapes if
// missing). tu58fs.log in /tmp has the emulator's output.

#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include "../../hardware.h"
#include "../../file_io.h"
#include "../../user_io.h"
#include "../../menu.h"
#include "tu58.h"

#define TU58_DIR  "games/PDP2011/tu58"
#define TU58_BAUD "19200"
#define TU58_PORT "/dev/ttyS1"
#define TU58_PID  "/tmp/tu58fs.pid"
#define TU58_LOG  "/tmp/tu58fs.log"

static pid_t tu58_pid = 0;          // running emulator (child or adopted)
static int adopted = 0;             // started by an earlier Main instance
static int checked_pidfile = 0;
static unsigned long next_check = 0;
static unsigned long next_start = 0;
static int told = 0;                // OSD message shown for the current request

static int is_tu58fs(pid_t pid)
{
	char path[64], comm[32] = {};
	snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	if (!fgets(comm, sizeof(comm), f)) comm[0] = 0;
	fclose(f);
	return !strncmp(comm, "tu58fs", 6);
}

static int alive(void)
{
	if (tu58_pid <= 0) return 0;
	if (!adopted)
	{
		int st;
		pid_t r = waitpid(tu58_pid, &st, WNOHANG);
		if (r == tu58_pid)
		{
			printf("PDP2011 TU58: tu58fs exited (status %d), see " TU58_LOG "\n", st);
			return 0;
		}
		return 1;
	}
	return kill(tu58_pid, 0) == 0 && is_tu58fs(tu58_pid);
}

static void tell(const char *msg)
{
	if (told) return;
	told = 1;
	printf("PDP2011 TU58: %s\n", msg);
	InfoMessage(msg, 4000, "TU58");
}

static void tu58_start(void)
{
	if (GetUARTMode() != 0)
	{
		tell("Serial port is in use:\nset UART mode to None\nfor the TU58.");
		return;
	}

	char dir[1024], exe[1100], d0[1100], d1[1100];
	FileCreatePath(TU58_DIR);
	snprintf(dir, sizeof(dir), "%s", getFullPath(TU58_DIR));
	snprintf(exe, sizeof(exe), "%s/tu58fs", dir);
	snprintf(d0, sizeof(d0), "%s/dd0.dsk", dir);
	snprintf(d1, sizeof(d1), "%s/dd1.dsk", dir);
	if (access(exe, X_OK))
	{
		tell("TU58 emulator missing:\n" TU58_DIR "/tu58fs");
		return;
	}

	pid_t pid = fork();
	if (pid < 0) return;
	if (pid == 0)
	{
		setsid();
		int fd = open(TU58_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
		fd = open("/dev/null", O_RDONLY);
		if (fd >= 0) { dup2(fd, 0); close(fd); }
		// -n: don't send <INIT> flags until the host talks. Otherwise they
		// pile up in the KL11 and XXDP's driver reads them instead of the
		// <CONTINUE> it waits for. Existing cartridges are opened as they
		// are; a missing one is created as an empty RT-11 tape (the
		// filesystem option also makes tu58fs check that filesystem on
		// every sync, so it is given only for new files).
		const char *argv[24];
		int n = 0;
		argv[n++] = "tu58fs";
		argv[n++] = "-bk";
		argv[n++] = "-n";
		argv[n++] = "-p"; argv[n++] = TU58_PORT;
		argv[n++] = "-b"; argv[n++] = TU58_BAUD;
		const char *dsk[2] = { d0, d1 };
		const char *unit[2] = { "0", "1" };
		int missing = 0;
		for (int u = 0; u < 2; u++)
		{
			if (access(dsk[u], F_OK)) { missing++; continue; }
			argv[n++] = "-d"; argv[n++] = unit[u]; argv[n++] = "w"; argv[n++] = dsk[u];
		}
		if (missing)
		{
			argv[n++] = "-rt11";
			for (int u = 0; u < 2; u++)
			{
				if (!access(dsk[u], F_OK)) continue;
				argv[n++] = "-d"; argv[n++] = unit[u]; argv[n++] = "c"; argv[n++] = dsk[u];
			}
		}
		argv[n] = NULL;
		execv(exe, (char *const *)argv);
		_exit(127);
	}

	tu58_pid = pid;
	adopted = 0;
	FILE *f = fopen(TU58_PID, "w");
	if (f) { fprintf(f, "%d\n", (int)pid); fclose(f); }
	printf("PDP2011 TU58: started tu58fs (pid %d) on " TU58_PORT " at " TU58_BAUD "\n", (int)pid);
}

static void tu58_stop(void)
{
	kill(tu58_pid, SIGTERM);
	for (int i = 0; i < 60 && alive(); i++) usleep(50000);   // it saves the cartridges first
	if (alive())
	{
		printf("PDP2011 TU58: tu58fs did not exit, killing it\n");
		kill(tu58_pid, SIGKILL);
		if (!adopted) waitpid(tu58_pid, NULL, 0);
	}
	printf("PDP2011 TU58: stopped tu58fs (pid %d)\n", (int)tu58_pid);
	tu58_pid = 0;
	adopted = 0;
	unlink(TU58_PID);
}

void pdp2011_tu58_poll(int active)
{
	if (next_check && !CheckTimer(next_check)) return;
	next_check = GetTimer(500);

	// An emulator left running by the Main instance before a core load.
	if (!checked_pidfile)
	{
		checked_pidfile = 1;
		FILE *f = fopen(TU58_PID, "r");
		int pid = 0;
		if (f) { if (fscanf(f, "%d", &pid) != 1) pid = 0; fclose(f); }
		if (pid > 0 && kill(pid, 0) == 0 && is_tu58fs(pid))
		{
			tu58_pid = pid;
			adopted = 1;
			printf("PDP2011 TU58: found running tu58fs (pid %d)\n", pid);
		}
		else unlink(TU58_PID);
	}

	if (tu58_pid > 0 && !alive())
	{
		tu58_pid = 0;
		adopted = 0;
		unlink(TU58_PID);
		next_start = GetTimer(10000);    // don't respawn a failing emulator in a loop
	}

	int want = active && !user_io_status_get("[2]") && user_io_status_get("[17]");
	if (!want) told = 0;

	if (want && tu58_pid <= 0 && (!next_start || CheckTimer(next_start))) tu58_start();
	else if (!want && tu58_pid > 0) tu58_stop();
}

void pdp2011_tu58_before_restart(const char *rbf_path)
{
	if (rbf_path && strcasestr(rbf_path, "PDP2011")) return;   // the next Main adopts it

	if (!checked_pidfile)
	{
		// never polled in this Main: an emulator may still be running from before
		FILE *f = fopen(TU58_PID, "r");
		int pid = 0;
		if (f) { if (fscanf(f, "%d", &pid) != 1) pid = 0; fclose(f); }
		if (pid > 0 && kill(pid, 0) == 0 && is_tu58fs(pid)) { tu58_pid = pid; adopted = 1; }
		checked_pidfile = 1;
	}
	if (tu58_pid > 0 && alive()) tu58_stop();
}
