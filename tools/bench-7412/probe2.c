// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bench experiment: start /dev/ssc-pcm0 first, wait for the peer to start
 * clocking (it is booted separately, after us), then send a contact mail
 * (0x22 -> 0x02, prim 0xF600, LE fields) in every slot x in-slot byte order x
 * the given bit shifts, recording all RX to a raw file plus a probe log.
 *
 * usage: probe2 SLOTS RAW LOG SETTLE_MS SHIFT[,SHIFT...]
 */
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "ssc_pcm.h"

#define FPB	32
#define MAXFB	128

static int fd, fb;
static size_t total;
#define KEEPF	2000
static uint8_t keep[KEEPF * MAXFB];
static size_t nkeep;
static FILE *raw;

static long now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void pump(int ms)
{
	static uint8_t buf[sizeof(struct ssc_pcm_block) + MAXFB * FPB];
	size_t need = sizeof(struct ssc_pcm_block) + (size_t)fb * FPB;
	long end = now_ms() + ms;

	for (;;) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		long left = end - now_ms();

		if (left <= 0)
			return;
		if (poll(&p, 1, (int)left) <= 0)
			continue;
		if (read(fd, buf, need) != (ssize_t)need)
			continue;
		fwrite(buf + sizeof(struct ssc_pcm_block), 1, (size_t)fb * FPB, raw);
		if (nkeep + FPB <= KEEPF) {
			memcpy(keep + nkeep * fb, buf + sizeof(struct ssc_pcm_block),
			       (size_t)fb * FPB);
			nkeep += FPB;
		}
		total += FPB;
	}
}

static int getbit(const uint8_t *s, size_t i)
{
	return (s[i >> 3] >> (7 - (i & 7))) & 1;
}

static void putbit(uint8_t *s, size_t i, int v)
{
	if (v)
		s[i >> 3] |= 0x80 >> (i & 7);
	else
		s[i >> 3] &= ~(0x80 >> (i & 7));
}

/* host -> chip element for the parameterless contact mail, LE fields */
static size_t element(uint8_t *e, unsigned seq)
{
	static const uint8_t m[6] = { 0x02, 0x00, 0x22, 0x02, 0x00, 0xF6 };
	unsigned sum = 0, words = (6 + 5) / 2;
	size_t len = 0;

	for (int i = 0; i < 6; i++)
		sum += m[i];
	e[len++] = words & 0xff;
	e[len++] = words >> 8;
	e[len++] = seq & 0xff;
	e[len++] = seq >> 8;
	memcpy(e + len, m, 6);
	len += 6;
	e[len++] = sum & 0xff;
	e[len++] = sum >> 8;
	if (len & 1)
		e[len++] = 0xff;
	return len;
}

int main(int argc, char **argv)
{
	struct ssc_pcm_config cfg = { 0 };
	struct ssc_pcm_tx_idle idle;
	struct ssc_pcm_stats st;
	int shifts[16], nsh = 0, settle, nslot;
	unsigned seq = 1;
	FILE *log;
	char *tok;
	long t0;

	if (argc < 6)
		return 2;
	nslot = atoi(argv[1]);
	fb = nslot * 2;
	settle = atoi(argv[4]);
	static char shiftarg[64];

	snprintf(shiftarg, sizeof(shiftarg), "%s", argv[5]);
	for (tok = strtok(shiftarg, ","); tok && nsh < 16; tok = strtok(NULL, ","))
		shifts[nsh++] = atoi(tok);
	raw = fopen(argv[2], "wb");
	log = fopen(argv[3], "w");
	fd = open("/dev/ssc-pcm0", O_RDWR);
	if (fb > MAXFB || !raw || !log || fd < 0) {
		perror("setup");
		return 1;
	}
	cfg.slots = nslot;
	cfg.frames_per_block = FPB;
	cfg.n_desc = 16;
	if (ioctl(fd, SSC_PCM_IOC_CONFIG, &cfg))
		return perror("CONFIG"), 1;
	memset(idle.frame, 0xff, sizeof(idle.frame));
	ioctl(fd, SSC_PCM_IOC_TX_IDLE, &idle);
	for (__u32 s = 1; s <= 4; s++)
		if (ioctl(fd, SSC_PCM_IOC_STEP, &s))
			return perror("STEP"), 1;
	printf("ready, waiting for the peer clock\n");
	fflush(stdout);

	t0 = now_ms();
	while (!total && now_ms() - t0 < 60000)
		pump(200);
	if (!total) {
		printf("no clock within 60 s\n");
		return 3;
	}
	fprintf(log, "clock after %ld ms\n", now_ms() - t0);
	pump(3000);	/* let the STUB settle */
	nkeep = 0;
	pump(500);
	if (!strcmp(argv[5], "passive")) {
		pump(12000);
		printf("passive frames %zu\n", total);
		fclose(raw);
		return 0;
	}
	if (nsh == 1 && shifts[0] < 0) {
		/* measure: 16-bit lanes read 0xFEFE most often at offset o */
		int best = 0, bo = 0;
		size_t nb = nkeep * fb * 8;

		for (int o = 0; o < 16; o++) {
			int n = 0;

			for (size_t b = o; b + 16 <= nb; b += 16) {
				unsigned v = 0;

				for (int k = 0; k < 16; k++)
					v = v << 1 | getbit(keep, b + k);
				n += v == 0xFEFE;
			}
			if (n > best) {
				best = n;
				bo = o;
			}
		}
		fprintf(log, "lane offset %d (FEFE %d)\n", bo, best);
		printf("lane offset %d (FEFE %d)\n", bo, best);
		nsh = 0;
		for (int d = -1; d <= 1; d++) {
			shifts[nsh++] = (bo + d + 16) % 16;
			shifts[nsh++] = (16 - bo + d + 16) % 16;
		}
	}

	for (int k = 0; k < nsh; k++)
	for (int sw = 0; sw <= 1; sw++)
	for (int slot = 0; slot < nslot; slot++) {
		uint8_t e[32], d0[MAXFB * 12], tx[MAXFB * 12];
		size_t el = element(e, seq), nf = el / 2 + 2;
		size_t fbits = (size_t)fb * 8;
		int sh = shifts[k];

		memset(d0, 0xff, sizeof(d0));
		for (size_t i = 0; i < el / 2; i++) {
			uint8_t a = e[2 * i], b = e[2 * i + 1];

			d0[(i + 1) * fb + slot * 2] = sw ? b : a;
			d0[(i + 1) * fb + slot * 2 + 1] = sw ? a : b;
		}
		memset(tx, 0xff, sizeof(tx));
		for (size_t i = sh; i < nf * fbits; i++)
			putbit(tx, i, getbit(d0, i - sh));
		fprintf(log, "probe seq %u at rx frame %zu: shift %d swap %d slot %d\n",
			seq, total, sh, sw, slot);
		if (write(fd, tx, nf * fb) != (ssize_t)(nf * fb))
			perror("write");
		seq++;
		pump(settle);
	}
	pump(2000);
	memset(&st, 0, sizeof(st));
	ioctl(fd, SSC_PCM_IOC_STATS, &st);
	fprintf(log, "end at rx frame %zu\n", total);
	printf("frames %zu probes %u tx_frames %llu tx_idle %llu rx_irqs %u\n",
	       total, seq - 1, (unsigned long long)st.tx_frames,
	       (unsigned long long)st.tx_idle, st.rx_irqs);
	__u32 z = 0;
	ioctl(fd, SSC_PCM_IOC_STEP, &z);
	fclose(raw);
	fclose(log);
	return 0;
}
