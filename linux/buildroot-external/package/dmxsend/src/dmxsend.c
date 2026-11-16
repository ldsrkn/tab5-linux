// SPDX-License-Identifier: GPL-2.0-only
/*
 * dmxsend v3 - DMX512 sender for the M5Stack Tab5 Grove port (Unit DMX).
 *
 * Builds the DMX break at the final 250000 baud 8N2 rate instead of
 * switching to 9600 baud for a zero-byte break.  On 250k 8N2 one byte takes
 * 11 bit times = 44 us.  Four consecutive zero bytes give ~176 us of
 * line LOW, which matches the DMX512-A break length (DMX_BREAK_LEN_US).
 * A short mark-after-break follows, then the 513-byte frame.
 *
 * The Unit DMX uses a CA-IS3092W auto-direction transceiver.  A 1.15 ms
 * break created by sending one byte at 9600 baud kept the line low for
 * too long and apparently caused the transceiver to drop out of TX mode.
 * This version keeps the whole frame at 250k and does not switch speeds.
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
#include <asm/termbits.h>
#include <asm/ioctls.h>
#ifndef TCGETS2
#define TCGETS2 0x802C542A
#endif
#ifndef TCSETS2
#define TCSETS2 0x402C542B
#endif

#define FRAME_BYTES 513 /* start code + 512 slots */

static volatile sig_atomic_t running = 1;

static void on_signal(int sig)
{
	(void)sig;
	running = 0;
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

/* Send one DMX frame.  Break is four 0x00 bytes at 250k 8N2. */
static int send_frame(int fd, const unsigned char *frame)
{
	unsigned char brk[4] = {0, 0, 0, 0};
	ssize_t n;

	n = write(fd, brk, sizeof brk);
	if (n != (ssize_t)sizeof brk) {
		perror("dmxsend: break");
		return -1;
	}
	usleep(20); /* MAB: keep it short */

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

	fprintf(stderr, "dmxsend v3: %s, ch1=%d, %d Hz", dev, frame[1], hz);
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

	memset(frame, 0, sizeof frame);
	send_frame(fd, frame);
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/ttyS3";
	int seconds = 0, hz = 40, c;

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
	if (optind >= argc) {
usage:
		fprintf(stderr,
			"usage: dmxsend [-d device] [-t s] [-r hz] <level | ch:val,...>\n");
		return 1;
	}
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	return oneshot(dev, seconds, hz, argv[optind]);
}
