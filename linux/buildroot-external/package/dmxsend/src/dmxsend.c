// SPDX-License-Identifier: GPL-2.0-only
/*
 * dmxsend - DMX512 sender for the M5Stack Tab5 Grove port (Unit DMX).
 *
 * Streams a continuous DMX512 frame stream out of a serial port.  Each
 * frame: a BREAK made of one zero byte at 9600 baud (11 bit times =
 * 1.15 ms of low; DMX512-A allows 88 us..1 s), then the frame at
 * 250000 baud 8N2.  The gap between the break and the start code is
 * two speed-switch ioctls plus one write - well under a millisecond,
 * which the Unit DMX auto-direction transceiver needs (a shell's
 * ~100 ms exec gaps release the bus and mangle the start code).
 *
 * 250000 baud needs termios2/BOTHER: Linux has no B250000 constant,
 * and BusyBox stty cannot set it.  This program uses TCSETS2 only.
 *
 * Modes:
 *   dmxsend [-d dev] [-t s] [-r hz] <level | ch:val,...>
 *       one-shot mode: stream the given universe until -t seconds
 *       (0 = until killed), then one black frame and exit.
 *   dmxsend -F <fifo> [-d dev] [-r hz] [initial spec]
 *       daemon mode: create and open <fifo> (O_RDWR, so writers can
 *       come and go), stream the current universe at ~<hz> frames/s
 *       forever and apply line commands as they arrive:
 *         set <ch> <v>                 one channel (1-based)
 *         set <ch>:<v>[,<ch>:<v>...]   several channels per line
 *         fill <v> / zero             all channels
 *         snap <1024 hex chars>       whole universe (512 bytes)
 *         quit                        black frame and exit
 *
 * Exit codes: 0 ok, 1 usage/open/ioctl error.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
/* termios2 lives in the kernel header; do not include <termios.h>. */
#include <asm/termbits.h>

#define FRAME_BYTES 513 /* start code + 512 slots */
#define LINE_MAX    1200

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

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Apply one command line to the universe.  Returns -1 on "quit". */
static int apply_line(unsigned char *frame, char *line)
{
	if (!strncmp(line, "quit", 4))
		return -1;
	if (!strncmp(line, "set ", 4)) {
		char *p = line + 4;

		while (*p) {
			long ch, val;

			ch = strtol(p, &p, 10);
			while (*p == ' ' || *p == ':' || *p == ',')
				p++;
			if (*p < '0' || *p > '9')
				break;
			val = strtol(p, &p, 10);
			if (ch >= 1 && ch <= 512) {
				if (val < 0)
					val = 0;
				if (val > 255)
					val = 255;
				frame[ch] = (unsigned char)val;
			}
			while (*p == ' ' || *p == ',')
				p++;
		}
	} else if (!strncmp(line, "fill ", 5)) {
		long val = strtol(line + 5, NULL, 10);
		int i;

		if (val < 0)
			val = 0;
		if (val > 255)
			val = 255;
		for (i = 1; i < FRAME_BYTES; i++)
			frame[i] = (unsigned char)val;
	} else if (!strncmp(line, "zero", 4)) {
		memset(frame, 0, FRAME_BYTES);
	} else if (!strncmp(line, "snap ", 5)) {
		const char *p = line + 5;
		int i;

		for (i = 0; i < 512 && p[0] && p[1]; i++, p += 2) {
			int hi = hexval(p[0]), lo = hexval(p[1]);

			if (hi < 0 || lo < 0)
				break;
			frame[i] = (unsigned char)(hi * 16 + lo);
		}
	}
	return 0;
}

/* Drain complete lines from buf[0..*len); apply each.  -1 on quit. */
static int drain_lines(unsigned char *frame, char *buf, int *len)
{
	int start = 0;

	while (running) {
		int i;

		for (i = start; i < *len; i++) {
			if (buf[i] == '\n' || buf[i] == '\r')
				break;
		}
		if (i >= *len)
			break;
		buf[i] = '\0';
		if (apply_line(frame, buf + start) < 0)
			return -1;
		start = i + 1;
	}
	if (start) {
		memmove(buf, buf + start, *len - start);
		*len -= start;
	}
	return 0;
}

/* Parse the channel spec ("255" or "1:255,7:0") into frame[]. */
static int parse_spec(char *spec, unsigned char *frame)
{
	memset(frame, 0, FRAME_BYTES);

	if (strchr(spec, ':')) {
		char *p = spec;

		while (*p) {
			long ch, val;

			ch = strtol(p, &p, 10);
			if (*p != ':')
				return -1;
			p++;
			val = strtol(p, &p, 10);
			if (ch < 1 || ch > 512 || val < 0 || val > 255)
				return -1;
			frame[ch] = (unsigned char)val;
			if (*p == ',')
				p++;
		}
		return 0;
	} else {
		long level = strtol(spec, NULL, 10);
		int i;

		if (level < 0 || level > 255)
			return -1;
		for (i = 1; i < FRAME_BYTES; i++)
			frame[i] = (unsigned char)level;
		return 0;
	}
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/ttyS3";
	const char *fifo = NULL;
	char *spec = NULL;
	int seconds = 0, hz = 40;
	unsigned char frame[FRAME_BYTES];
	struct termios2 t;
	int fd, c;

	while ((c = getopt(argc, argv, "d:t:r:F:")) != -1) {
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
		case 'F':
			fifo = optarg;
			break;
		default:
			goto usage;
		}
	}
	if (optind < argc)
		spec = argv[optind];
	if (!spec && !fifo) {
		fprintf(stderr, "dmxsend: nothing to send\n");
		goto usage;
	}

	fd = open(dev, O_RDWR | O_NOCTTY);
	if (fd < 0) {
		perror(dev);
		return 1;
	}

	/* raw 8N2; speed set per frame via BOTHER */
	if (ioctl(fd, TCGETS2, &t) < 0) {
		perror("dmxsend: TCGETS2");
		return 1;
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
		return 1;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	if (spec && parse_spec(spec, frame) < 0) {
		fprintf(stderr, "dmxsend: bad spec: %s\n", spec);
		return 1;
	}
	if (!spec)
		memset(frame, 0, sizeof frame);

	if (fifo) {
		/* daemon mode: stream the universe, apply fifo commands */
		char buf[LINE_MAX];
		int len = 0, ff;
		long period_us = 1000000L / (hz > 1 ? hz : 40);

		if (mkfifo(fifo, 0666) < 0 && errno != EEXIST) {
			perror("dmxsend: mkfifo");
			return 1;
		}
		/* O_RDWR: no EOF when writers come and go */
		ff = open(fifo, O_RDWR);
		if (ff < 0) {
			perror(fifo);
			return 1;
		}
		fprintf(stderr, "dmxsend: daemon on %s, fifo %s, %d Hz\n",
			dev, fifo, hz);

		while (running) {
			struct timeval tv;
			fd_set rfds;
			long spent_us = 0;
			int r;

			if (send_frame(fd, frame) < 0)
				return 1;
			spent_us = 24200; /* break + frame time */

			while (running &&
			       spent_us < period_us) {
				long left = period_us - spent_us;

				FD_ZERO(&rfds);
				FD_SET(ff, &rfds);
				tv.tv_sec = left / 1000000L;
				tv.tv_usec = left % 1000000L;
				r = select(ff + 1, &rfds, NULL, NULL, &tv);
				if (r < 0) {
					if (errno == EINTR)
						break;
					perror("dmxsend: select");
					return 1;
				}
				if (r == 0)
					break; /* next frame due */
				r = read(ff, buf + len, sizeof buf - len - 1);
				if (r < 0) {
					if (errno == EINTR)
						break;
					perror("dmxsend: read fifo");
					return 1;
				}
				if (r == 0)
					continue;
				len += r;
				buf[len] = '\0';
				if (drain_lines(frame, buf, &len) < 0)
					running = 0;
				if ((unsigned)len >= sizeof buf - 1) {
					/* overflow: drop the partial line */
					len = 0;
				}
			}
		}
		close(ff);
	} else {
		unsigned long start;

		fprintf(stderr, "dmxsend: %s, ch1=%d, %d Hz", dev, frame[1], hz);
		if (seconds)
			fprintf(stderr, ", %d s", seconds);
		fprintf(stderr, "  (Ctrl-C = black frame + exit)\n");

		start = (unsigned long)time(NULL);
		while (running) {
			if (send_frame(fd, frame) < 0)
				return 1;
			if (seconds &&
			    (unsigned long)time(NULL) - start >= (unsigned long)seconds)
				break;
			usleep(1000000 / hz > 25000 ? (1000000 / hz - 25000)
						    : 1000);
		}
	}

	/* black frame on the way out so fixtures do not freeze at level */
	memset(frame, 0, sizeof frame);
	send_frame(fd, frame);
	set_speed(fd, 9600);
	close(fd);
	return 0;

usage:
	fprintf(stderr,
		"usage: dmxsend [-d device] [-t s] [-r hz] [-F fifo] [<level | ch:val,...>]\n");
	return 1;
}
