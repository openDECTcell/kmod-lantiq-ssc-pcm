// SPDX-License-Identifier: GPL-2.0-only
/*
 * ssc_pcm-test - exercise the Lantiq VR9 SSC "SPI-TDM" transport driver.
 *
 * Usage: ssc_pcm-test [step...]
 *   With no arguments: INFO, CONFIG (defaults), then steps 1,2,3.
 *   A numerical argument stops after that step (e.g. "1" only clocks).
 *   "regs" dumps the controller registers; "rx" starts step 4 and waits up
 *   to 2 s for one block, reads on for 3 s, then prints the receive and
 *   error statistics (exit 2 if no block arrived).
 *   "tx" starts step 4, queues four test frames, waits 1 s and prints the
 *   transmit statistics.
 *   "watch" samples STATE/FSTAT ten times, 100 ms apart, after the steps.
 *   "slots=N" configures an N-slot frame instead of the default.
 *   "hold=SECS" starts step 4 and keeps the engine running for SECS
 *   seconds, reading and discarding whatever arrives; the driver logs its
 *   ring health in the kernel log (no peer clock needed for that).
 *   Exits nonzero if any requested step or the read fails.
 */
#include <errno.h>
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

static int fd;

static void dump_regs(void)
{
	struct ssc_pcm_regs r;
	static const char *names[] = {
		"CON", "WHBSTATE", "STATE", "FSTAT", "ID", "RXFCON", "TXFCON",
		"SFCON"
	};
	unsigned int i;

	memset(&r, 0, sizeof(r));
	if (ioctl(fd, SSC_PCM_IOC_REGS, &r) < 0) {
		perror("REGS");
		return;
	}
	printf("regs (step %u, valid 0x%x):\n", r.step, r.valid);
	for (i = 0; i < 8; i++)
		printf("  %-9s = 0x%08x\n", names[i], r.reg[i]);
}

static int do_step(unsigned int n)
{
	int ret = ioctl(fd, SSC_PCM_IOC_STEP, &n);

	printf("step %u: %s\n", n, ret ? strerror(errno) : "ok");
	if (!ret)
		dump_regs();
	return ret;
}

int main(int argc, char **argv)
{
	struct ssc_pcm_info info;
	struct ssc_pcm_config cfg;
	unsigned int last = SSC_PCM_STEP_TDM;
	unsigned int s;
	int want_rx = 0;
	int want_watch = 0;
	int want_tx = 0;
	unsigned int slots = 0, hold = 0;
	int rc = 0;
	int i;

	fd = open("/dev/ssc-pcm0", O_RDWR);
	if (fd < 0) {
		perror("/dev/ssc-pcm0");
		return 1;
	}

	memset(&info, 0, sizeof(info));
	if (ioctl(fd, SSC_PCM_IOC_INFO, &info) == 0)
		printf("info: v%u name=%s step=%u slots=%u frame_bytes=%u\n",
		       info.version, info.name, info.step, info.slots,
		       info.frame_bytes);
	else
		perror("INFO");

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "rx"))
			want_rx = 1;
		else if (!strcmp(argv[i], "tx"))
			want_tx = 1;
		else if (!strcmp(argv[i], "watch"))
			want_watch = 1;
		else if (!strcmp(argv[i], "regs"))
			dump_regs();
		else if (!strncmp(argv[i], "slots=", 6))
			slots = strtoul(argv[i] + 6, NULL, 0);
		else if (!strncmp(argv[i], "hold=", 5))
			hold = strtoul(argv[i] + 5, NULL, 0);
		else
			last = strtoul(argv[i], NULL, 0);
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.slots = slots;
	if (ioctl(fd, SSC_PCM_IOC_CONFIG, &cfg) == 0)
		printf("config: slots=%u frames_per_block=%u n_desc=%u\n",
		       cfg.slots, cfg.frames_per_block, cfg.n_desc);
	else
		perror("CONFIG");

	for (s = 1; s <= last; s++)
		if (do_step(s)) {
			rc = 1;
			break;
		}

	for (i = 0; want_watch && !rc && i < 10; i++) {
		struct ssc_pcm_regs r;

		usleep(100000);
		memset(&r, 0, sizeof(r));
		if (ioctl(fd, SSC_PCM_IOC_REGS, &r) < 0) {
			perror("REGS");
			rc = 1;
			break;
		}
		printf("watch %d: STATE=0x%08x FSTAT=0x%08x\n", i, r.reg[2],
		       r.reg[3]);
	}

	if (want_tx && !rc) {
		unsigned char frames[4 * 32];
		struct ssc_pcm_stats st;
		size_t len = 4 * info.frame_bytes;
		ssize_t n;

		for (i = 0; i < (int)len; i++)
			frames[i] = i;
		if (do_step(SSC_PCM_STEP_DMA)) {
			rc = 1;
		} else {
			n = write(fd, frames, len);
			printf("tx: wrote %zd of %zu bytes\n", n, len);
			if (n != (ssize_t)len)
				rc = 1;
			sleep(1);
			memset(&st, 0, sizeof(st));
			if (ioctl(fd, SSC_PCM_IOC_STATS, &st) == 0)
				printf("stats: tx_frames=%llu tx_idle=%llu tx_queued=%u\n",
				       (unsigned long long)st.tx_frames,
				       (unsigned long long)st.tx_idle,
				       st.tx_queued);
		}
	}

	if (want_rx && !rc) {
		size_t need = sizeof(struct ssc_pcm_block) + cfg.slots * 2 *
			      cfg.frames_per_block;
		unsigned char *buf = calloc(1, need);
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		struct ssc_pcm_stats st;
		ssize_t n = -1;

		if (do_step(SSC_PCM_STEP_DMA))
			rc = 1;
		else if (poll(&pfd, 1, 2000) <= 0)
			printf("rx: no block within 2 s\n");
		else
			n = read(fd, buf, need);
		if (!rc && n < 0)
			rc = 2;
		if (n > 0) {
			struct ssc_pcm_block *b = (struct ssc_pcm_block *)buf;
			printf("read %zd bytes: magic=0x%08x seq=%u flags=0x%x len=%u\n",
			       n, b->magic, b->seq, b->flags, b->len);
			/* keep reading for 3 s so error counts cover a span */
			for (i = 0; i < 3 * 8000 / (int)cfg.frames_per_block; i++)
				if (poll(&pfd, 1, 1000) <= 0 ||
				    read(fd, buf, need) < 0)
					break;
			printf("rx: read %d more blocks\n", i);
		}
		memset(&st, 0, sizeof(st));
		if (ioctl(fd, SSC_PCM_IOC_STATS, &st) == 0)
			printf("stats: frames=%llu blocks=%llu dropped=%llu overruns=%u tx_underruns=%u rx_irqs=%u tx_irqs=%u polls=%u\n",
			       (unsigned long long)st.frames,
			       (unsigned long long)st.blocks,
			       (unsigned long long)st.dropped, st.overruns,
			       st.tx_underruns, st.rx_irqs, st.tx_irqs,
			       st.polls);
		else
			perror("STATS");
		free(buf);
	}

	if (hold && !rc) {
		size_t need = sizeof(struct ssc_pcm_block) + cfg.slots * 2 *
			      cfg.frames_per_block;
		unsigned char *buf = calloc(1, need);
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		time_t end = time(NULL) + hold;

		if (!buf || do_step(SSC_PCM_STEP_DMA)) {
			rc = 1;
		} else {
			printf("hold: engine running for %u s\n", hold);
			fflush(stdout);
			while (time(NULL) < end)
				if (poll(&pfd, 1, 1000) > 0 &&
				    read(fd, buf, need) < 0)
					break;
		}
		free(buf);
	}

	/* Leave the controller off so the module can be unloaded cleanly. */
	do_step(SSC_PCM_STEP_OFF);
	close(fd);
	return rc;
}
