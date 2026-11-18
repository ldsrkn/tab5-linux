// SPDX-License-Identifier: GPL-2.0-only
/*
 * dmxsend v4 - DMX512 sender for M5Stack Tab5 Grove UART.
 *
 * Strict DMX512-A frame:
 *   - 250000 baud, 8N2
 *   - BREAK: six 0x00 bytes = ~264 us LOW (within 88 us .. 1 s)
 *   - MAB: one 0xFF byte = ~44 us HIGH (min 12 us)
 *   - Frame: 0x00 start code + 512 slots
 *
 * The whole frame stays at 250k; no baud-rate switching.
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
#include <termios.h>
#include <asm/termbits.h>
#include <asm/ioctls.h>
#ifndef TCGETS2
#define TCGETS2 0x802C542A
#endif
#ifndef TCSETS2
#define TCSETS2 0x402C542B
#endif

#define FRAME_BYTES 513 /* start code + 512 slots */
#define HZ 40

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

static int flush_frame(int fd)
{
	if (tcdrain(fd) < 0) {
		perror("dmxsend: tcdrain");
		return -1;
	}
	return 0;
}

/* Send one DMX frame. */
static int send_frame(int fd, const unsigned char *frame)
{
	unsigned char brk[6] = {0, 0, 0, 0, 0, 0};
	unsigned char mab[1] = {0xFF};
	ssize_t n;

	/* BREAK: keep line LOW for ~264 us */
	n = write(fd, brk, sizeof brk);
	if (n != (ssize_t)sizeof brk) {
		perror("dmxsend: break");
		return -1;
	}

	/* MAB: one stop-bit-time at 250k 8N2 = 44 us HIGH */
	n = write(fd, mab, sizeof mab);
	if (n != (ssize_t)sizeof mab) {
		perror("dmxsend: mab");
		return -1;
	}

	/* Frame */
	n = write(fd, frame, FRAME_BYTES);
	if (n < 0) {
		perror("dmxsend: frame");
		return -1;
	}
	if (n != FRAME_BYTES) {
		fprintf(stderr, "dmxsend: short frame write (%ld)\n", (long)n);
		return -1;
	}

	if (flush_frame(fd) < 0)
		return -1;
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
		   const unsigned char *spec)
{
	unsigned char frame[FRAME_BYTES];
	unsigned long start, frame_us, period_us, sleep_us;
	int fd, i;

	memset(frame, 0, sizeof frame);
	if (strchr((const char *)spec, ':')) {
		if (!parse_spec((const char *)spec, frame))
			return 1;
	} else {
		long level = strtol((const char *)spec, NULL, 10);

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

	/* One frame on the wire:
	 * break 6 bytes + mab 1 byte + frame 513 bytes = 520 bytes * 44 us = 22880 us
	 * plus inter-frame gap to reach the desired Hz.
	 */
	frame_us = (6 + 1 + FRAME_BYTES) * 44;
	period_us = 1000000 / hz;
	sleep_us = (period_us > frame_us) ? (period_us - frame_us) : 1000;

	fprintf(stderr, "dmxsend v4: %s, ch1=%d, %d Hz", dev, frame[1], hz);
	if (seconds)
		fprintf(stderr, ", %d s", seconds);
	fprintf(stderr, "  (Ctrl-C = black frame + exit)\n");

	start = (unsigned long)time(NULL);
	while (running) {
		if (send_frame(fd, frame) < 0)
			return 1;
		usleep(sleep_us);
		if (seconds && (unsigned long)time(NULL) - start >= (unsigned long)seconds)
			break;
	}

	memset(frame, 0, sizeof frame);
	send_frame(fd, frame);
	flush_frame(fd);
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/ttyS3";
	int seconds = 0, hz = HZ, c;

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
	return oneshot(dev, seconds, hz,
		       (const unsigned char *)argv[optind]);
}
