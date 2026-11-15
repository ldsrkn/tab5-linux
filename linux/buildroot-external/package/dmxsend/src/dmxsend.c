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
 * usage: dmxsend [-d device] [-t seconds] [-r hz] <spec>
 *   spec: <level>                all 512 channels to <level> (0..255)
 *         <ch>:<val>[,<ch>:<val>...] set channels (1-based), rest 0
 *   -d device   default /dev/ttyS3 (Tab5 Grove UART)
 *   -t seconds  stop after <seconds>; 0 = until killed (default).  On exit
 *               (or Ctrl-C) one black frame is sent.
 *   -r hz       frame rate, default 40 (DMX512 max is ~44)
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
#include <time.h>
#include <unistd.h>
/* termios2 lives in the kernel header; do not include <termios.h>. */
#include <asm/termbits.h>

#define FRAME_BYTES 513 /* start code + 512 slots */

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

int main(int argc, char **argv)
{
	const char *dev = "/dev/ttyS3";
	int seconds = 0, hz = 40;
	unsigned char frame[FRAME_BYTES];
	struct termios2 t;
	int fd, i, c;
	unsigned long start;

	memset(frame, 0, sizeof frame);

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
			fprintf(stderr,
				"usage: dmxsend [-d device] [-t s] [-r hz] <level | ch:val,...>\n");
			return 1;
		}
	}
	if (optind >= argc) {
		fprintf(stderr,
			"usage: dmxsend [-d device] [-t s] [-r hz] <level | ch:val,...>\n");
		return 1;
	}
	if (strchr(argv[optind], ':')) {
		/* channel list: 1:255,2:128,... (1-based channels) */
		char *spec = argv[optind], *tok, *end;
		for (tok = spec; (end = strchr(tok, ',')) != NULL; tok = end + 1) {
			long ch, val;
			char *sep = strchr(tok, ':');
			if (!sep || sep > end)
				goto badspec;
			*end = '\0';
			*sep = '\0';
			ch = strtol(tok, NULL, 10);
			val = strtol(sep + 1, NULL, 10);
			if (ch < 1 || ch > 512 || val < 0 || val > 255)
				goto badspec;
			frame[ch] = (unsigned char)val;
			continue;
badspec:
			fprintf(stderr, "dmxsend: bad spec: %s\n", spec);
			return 1;
		}
		/* the loop above stops before the last entry; parse it too */
		{
			long ch, val;
			char *sep = strchr(tok, ':');
			if (sep) {
				ch = strtol(tok, NULL, 10);
				val = strtol(sep + 1, NULL, 10);
				if (ch >= 1 && ch <= 512 && val >= 0 && val <= 255)
					frame[ch] = (unsigned char)val;
			}
		}
	} else {
		long level = strtol(argv[optind], NULL, 10);

		if (level < 0 || level > 255) {
			fprintf(stderr, "dmxsend: level 0..255\n");
			return 1;
		}
		for (i = 1; i < FRAME_BYTES; i++)
			frame[i] = (unsigned char)level;
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

	/* black frame on the way out so fixtures do not freeze at level */
	memset(frame, 0, sizeof frame);
	send_frame(fd, frame);
	set_speed(fd, 9600);
	close(fd);
	return 0;
}
