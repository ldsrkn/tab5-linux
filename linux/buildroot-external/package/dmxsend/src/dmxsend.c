// SPDX-License-Identifier: GPL-2.0-only
/*
 * dmxsend - DMX512 sender for the M5Stack Tab5 Grove port (Unit DMX).
 *
 * Sends a continuous DMX512 frame stream out of a serial port.  Each frame:
 * a BREAK made of one zero byte at 9600 baud (11 bit times = 1.15 ms of
 * low; DMX512-A allows 88 us..1 s), then the frame at 250000 baud 8N2.
 * The gap between the break and the start code is the two speed-switch
 * ioctls plus one write - well under a millisecond, which the Unit DMX
 * auto-direction transceiver needs (a shell's ~100 ms exec gaps release
 * the bus and mangle the start code).
 *
 * 250000 baud needs termios2/BOTHER: Linux has no B250000 constant, and
 * BusyBox stty cannot set it.  This program uses TCSETS2 only.
 *
 * Two modes:
 *
 * 1. one-shot (the original): stream one universe
 *    dmxsend [-d device] [-t seconds] [-r hz] <level | ch:val,...>
 *    On exit (or Ctrl-C, or -t timeout) one black frame is sent.
 *
 * 2. daemon (dmx): stream the universe and take live updates from a FIFO
 *    dmxsend daemon [-d device] [-r hz]
 *
 *    Control FIFO: /run/dmx.in (created, 0666).  One command per line:
 *      <ch>=<v>       jump channel ch (1..512) to v
 *      <ch>~<v>       fade channel ch to v over the current fade time
 *      all=<v>        jump every channel to v
 *      all~<v>        fade every channel to v
 *      fade <ms>      set the fade time for subsequent ~ commands
 *      black          jump all to 0        black~ fades to 0
 *      full           jump all to 255
 *      dump           write the 512 current values to /run/dmx.out
 *      quit           black frame, then exit
 *    Unknown lines are ignored (forward compatibility).
 *
 *    Status FIFO: /run/dmx.out (created, 0666).  The daemon writes
 *    "dmxd ready" on start and one line of 512 values per "dump":
 *    "C <v1> <v2> ... <v512>".
 *
 * usage: dmxsend [-d device] [-t s] [-r hz] <spec>
 *   spec: <level>                all 512 channels to <level> (0..255)
 *         <ch>:<val>[,<ch>:<val>...] set channels (1-based), rest 0
 * Exit codes: 0 ok, 1 usage/open/ioctl error.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
/* termios2 lives in the kernel header; do not include <termios.h>. */
#include <asm/termbits.h>
/* TCGETS2/TCSETS2 live in the kernel ioctls header, not in libc's
 * <sys/ioctl.h>: uClibc ships an old copy of the TIOC numbers there
 * with no termios2 ioctls, and glibc does not export them portably
 * either.  The kernel headers for riscv do ship <asm/ioctls.h>
 * (asm-generic content) with TCGETS2/TCSETS2; the #ifndefs below are
 * the fallback in case a libc chain pre-empted the kernel guard. */
#include <asm/ioctls.h>
#ifndef TCGETS2
#define TCGETS2 0x802C542A /* _IOR('T', 0x2a, struct termios2); size 44 */
#endif
#ifndef TCSETS2
#define TCSETS2 0x402C542B /* _IOW('T', 0x2b, struct termios2) */
#endif

#define FRAME_BYTES 513 /* start code + 512 slots */
#define IN_FIFO "/run/dmx.in"
#define OUT_FIFO "/run/dmx.out"

static volatile sig_atomic_t running = 1;

static void on_signal(int sig)
{
	(void)sig;
	running = 0;
}

static int set_speed(int fd, unsigned int speed)
{
	struct termios2 t;

	if (ioctl(fd, TCGETS2, &t) < 0)
		return -1;
	t.c_cflag &= ~CBAUD;
	t.c_cflag |= BOTHER;
	t.c_ispeed = t.c_ospeed = speed;
	return ioctl(fd, TCSETS2, &t);
}

/* Send one frame (with its break) and wait until it has clocked out. */
static int send_frame(int fd, const unsigned char *frame)
{
	unsigned char brk = 0;
	ssize_t n;

	if (set_speed(fd, 9600) < 0) {
		perror("dmxsend: TCSETS2 9600");
		return -1;
	}
	n = write(fd, &brk, 1); /* 1.15 ms of low = DMX512-A break */
	if (n != 1) {
		perror("dmxsend: break");
		return -1;
	}
	usleep(1200); /* let the 9600-baud byte finish on the wire */

	if (set_speed(fd, 250000) < 0) {
		perror("dmxsend: TCSETS2 250000");
		return -1;
	}
	n = write(fd, frame, FRAME_BYTES);
	if (n < 0) {
		perror("dmxsend: frame");
		return -1;
	}
	if (n != FRAME_BYTES) {
		fprintf(stderr, "dmxsend: short frame write (%ld)\n", (long)n);
		return -1;
	}
	usleep(23000); /* 513 bytes * 44 us = 22.6 ms on the wire */
	return 0;
}

static int open_port(const char *dev)
{
	struct termios2 t;
	int fd = open(dev, O_RDWR | O_NOCTTY);

	if (fd < 0) {
		perror(dev);
		return -1;
	}
	if (ioctl(fd, TCGETS2, &t) < 0) {
		perror("dmxsend: TCGETS2");
		close(fd);
		return -1;
	}
	t.c_cflag &= ~(CSIZE | PARENB | PARODD | CRTSCTS);
	t.c_cflag |= CS8 | CSTOPB | CREAD;
	t.c_cflag &= ~CBAUD;
	t.c_cflag |= BOTHER;
	t.c_iflag = 0;
	t.c_oflag = 0;
	t.c_lflag = 0;
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	t.c_ispeed = t.c_ospeed = 250000;
	if (ioctl(fd, TCSETS2, &t) < 0) {
		perror("dmxsend: TCSETS2");
		close(fd);
		return -1;
	}
	return fd;
}

/* ------------------------- one-shot mode ------------------------- */

/* Parse "1:255,2:128,..." into frame (1-based channels).  0 on error. */
static int parse_spec(const char *spec, unsigned char *frame)
{
	char *buf = strdup(spec), *tok, *end, *sep;
	long ch, val;

	if (!buf)
		return 0;
	for (tok = buf; (end = strchr(tok, ',')) != NULL; tok = end + 1) {
		*end = '\0';
		sep = strchr(tok, ':');
		if (!sep)
			goto bad;
		*sep = '\0';
		ch = strtol(tok, NULL, 10);
		val = strtol(sep + 1, NULL, 10);
		if (ch < 1 || ch > 512 || val < 0 || val > 255)
			goto bad;
		frame[ch] = (unsigned char)val;
	}
	sep = strchr(tok, ':');
	if (!sep)
		goto bad;
	*sep = '\0';
	ch = strtol(tok, NULL, 10);
	val = strtol(sep + 1, NULL, 10);
	if (ch < 1 || ch > 512 || val < 0 || val > 255)
		goto bad;
	frame[ch] = (unsigned char)val;
	free(buf);
	return 1;
bad:
	free(buf);
	fprintf(stderr, "dmxsend: bad spec: %s\n", spec);
	return 0;
}

static int oneshot(const char *dev, int seconds, int hz,
		   const char *spec)
{
	unsigned char frame[FRAME_BYTES];
	unsigned long start;
	int fd, i;

	memset(frame, 0, sizeof frame);
	if (strchr(spec, ':')) {
		if (!parse_spec(spec, frame))
			return 1;
	} else {
		long level = strtol(spec, NULL, 10);

		if (level < 0 || level > 255) {
			fprintf(stderr, "dmxsend: level 0..255\n");
			return 1;
		}
		for (i = 1; i < FRAME_BYTES; i++)
			frame[i] = (unsigned char)level;
	}

	fd = open_port(dev);
	if (fd < 0)
		return 1;

	fprintf(stderr, "dmxsend: %s, ch1=%d, %d Hz", dev, frame[1], hz);
	if (seconds)
		fprintf(stderr, ", %d s", seconds);
	fprintf(stderr, "  (Ctrl-C = black frame + exit)\n");

	start = (unsigned long)time(NULL);
	while (running) {
		if (send_frame(fd, frame) < 0)
			return 1;
		if (seconds && (unsigned long)time(NULL) - start >= (unsigned long)seconds)
			break;
		usleep(1000000 / hz > 25000 ? (1000000 / hz - 25000) : 1000);
	}

	memset(frame, 0, sizeof frame); /* leave the rig dark */
	send_frame(fd, frame);
	set_speed(fd, 9600);
	close(fd);
	return 0;
}

/* -------------------------- daemon mode -------------------------- */

struct fade {
	int from, to, step, frames_left;
};

struct universe {
	unsigned char val[FRAME_BYTES]; /* [0] unused start code */
	struct fade fade[FRAME_BYTES];
	long fade_ms; /* default for ~ commands */
};

static void uv_set(struct universe *u, int ch, int v)
{
	u->val[ch] = (unsigned char)v;
	u->fade[ch].frames_left = 0;
}

static void uv_fade_start(struct universe *u, int ch, int v, int period_us)
{
	struct fade *f = &u->fade[ch];

	if (u->fade_ms <= 0 || (int)u->fade_ms <= period_us / 1000) {
		uv_set(u, ch, v);
		return;
	}
	f->from = u->val[ch];
	f->to = v;
	f->frames_left = (int)((u->fade_ms * 1000) / period_us);
	f->step = (f->to - f->from) / f->frames_left;
}

static void uv_step(struct universe *u)
{
	int i;

	for (i = 1; i < FRAME_BYTES; i++) {
		struct fade *f = &u->fade[i];

		if (f->frames_left <= 0)
			continue;
		if (f->frames_left == 1) {
			u->val[i] = (unsigned char)f->to;
			f->frames_left = 0;
		} else {
			f->from += f->step;
			u->val[i] = (unsigned char)f->from;
			f->frames_left--;
		}
	}
}

/* Apply one control line.  Returns 0 normally, -1 on "quit". */
static int apply_line(struct universe *u, char *line, int period_us)
{
	char *p = line, *eq, *end;
	long a, b;

	while (*p == ' ')
		p++;
	if (!*p)
		return 0;

	if (!strcmp(p, "quit"))
		return -1;
	if (!strcmp(p, "black")) {
		int i;

		for (i = 1; i < FRAME_BYTES; i++)
			uv_set(u, i, 0);
		return 0;
	}
	if (!strcmp(p, "black~")) {
		int i;

		for (i = 1; i < FRAME_BYTES; i++)
			uv_fade_start(u, i, 0, period_us);
		return 0;
	}
	if (!strcmp(p, "full")) {
		int i;

		for (i = 1; i < FRAME_BYTES; i++)
			uv_set(u, i, 255);
		return 0;
	}
	if (!strncmp(p, "fade ", 5)) {
		u->fade_ms = strtol(p + 5, NULL, 10);
		return 0;
	}
	if (!strcmp(p, "dump"))
		return 1; /* handled by the caller (needs the out fifo) */

	/* <ch><op><v>  where op is = or ~ ;  or  all<op><v> */
	if (!strncmp(p, "all", 3)) {
		int i, fade = 0;

		if (p[3] == '~')
			fade = 1;
		else if (p[3] != '=')
			return 0;
		b = strtol(p + 4, NULL, 10);
		if (b < 0 || b > 255)
			return 0;
		for (i = 1; i < FRAME_BYTES; i++) {
			if (fade)
				uv_fade_start(u, i, (int)b, period_us);
			else
				uv_set(u, i, (int)b);
		}
		return 0;
	}
	eq = strpbrk(p, "=~");
	if (!eq)
		return 0; /* unknown: ignore */
	a = strtol(p, &end, 10);
	if (end != eq || a < 1 || a > 512)
		return 0;
	b = strtol(eq + 1, NULL, 10);
	if (b < 0 || b > 255)
		return 0;
	if (*eq == '~')
		uv_fade_start(u, (int)a, (int)b, period_us);
	else
		uv_set(u, (int)a, (int)b);
	return 0;
}

static int daemon_mode(const char *dev, int hz)
{
	struct universe u;
	int period_us = 1000000 / hz;
	int in_fd = -1, out_fd, fd, i, r, got;
	static char inbuf[8192], line[256];
	static int fill = 0;
	static int want_dump = 0;

	memset(&u, 0, sizeof u);
	u.fade_ms = 0;

	if (mkfifo(IN_FIFO, 0666) < 0 && errno != EEXIST) {
		perror("dmxsend: mkfifo " IN_FIFO);
		return 1;
	}
	if (mkfifo(OUT_FIFO, 0666) < 0 && errno != EEXIST) {
		perror("dmxsend: mkfifo " OUT_FIFO);
		return 1;
	}
	/* O_RDWR on the in-fifo: no EOF spin when no writer is attached */
	in_fd = open(IN_FIFO, O_RDWR | O_NONBLOCK);
	if (in_fd < 0) {
		perror("dmxsend: open " IN_FIFO);
		return 1;
	}
	out_fd = open(OUT_FIFO, O_RDWR | O_NONBLOCK);
	if (out_fd < 0) {
		perror("dmxsend: open " OUT_FIFO);
		close(in_fd);
		return 1;
	}

	fd = open_port(dev);
	if (fd < 0) {
		close(in_fd);
		close(out_fd);
		return 1;
	}

	{
		const char ready[] = "dmxd ready\n";

		write(out_fd, ready, sizeof ready - 1);
	}
	fprintf(stderr, "dmxd: %s, %d Hz, fifo %s (Ctrl-C = black + exit)\n",
		dev, hz, IN_FIFO);

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	while (running) {
		/* drain the control fifo */
		while ((r = read(in_fd, inbuf, sizeof inbuf)) > 0) {
			for (i = 0; i < r; i++) {
				if (inbuf[i] == '\n') {
					line[fill] = '\0';
					{
						int rc = apply_line(&u, line, period_us);

						if (rc < 0)
							running = 0;
						else if (rc == 1)
							want_dump = 1;
					}
					fill = 0;
				} else if (fill < (int)sizeof line - 1) {
					line[fill++] = inbuf[i];
				}
			}
		}
		if (want_dump) {
			char out[4096];
			int off = 0, ch;

			want_dump = 0;
			off += snprintf(out + off, sizeof out - off, "C");
			for (ch = 1; ch < FRAME_BYTES; ch++)
				off += snprintf(out + off, sizeof out - off,
						" %d", u.val[ch]);
			off += snprintf(out + off, sizeof out - off, "\n");
			write(out_fd, out, off);
		}

		uv_step(&u);
		if (send_frame(fd, u.val) < 0)
			break;
		usleep(period_us > 25000 ? period_us - 25000 : 1000);
	}

	/* leave the rig dark */
	got = 1;
	for (i = 1; i < FRAME_BYTES; i++)
		got &= u.val[i] == 0;
	if (!got) {
		memset(u.val, 0, sizeof u.val);
		send_frame(fd, u.val);
	}
	{
		const char bye[] = "dmxd bye\n";

		write(out_fd, bye, sizeof bye - 1);
	}
	set_speed(fd, 9600);
	close(fd);
	close(in_fd);
	close(out_fd);
	return 0;
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/ttyS3";
	int seconds = 0, hz = 40, c;
	int daemon = 0;

	while ((c = getopt(argc, argv, "d:t:r:")) != -1) {
		switch (c) {
		case 'd':
			dev = optarg;
			break;
		case 't':
			seconds = atoi(optarg);
			break;
		case 'r':
			hz = atoi(optarg);
			break;
		default:
			goto usage;
		}
	}
	if (optind < argc && !strcmp(argv[optind], "daemon")) {
		daemon = 1;
		optind++;
	}
	if (optind >= argc && !daemon) {
usage:
		fprintf(stderr,
			"usage: dmxsend [-d device] [-t s] [-r hz] <level | ch:val,...>\n"
			"       dmxsend [-d device] [-r hz] daemon   (live control via " IN_FIFO ")\n");
		return 1;
	}
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	if (daemon)
		return daemon_mode(dev, hz);
	return oneshot(dev, seconds, hz, argv[optind]);
}
