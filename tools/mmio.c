// SPDX-License-Identifier: GPL-2.0-only
/* mmio - minimal /dev/mem register peek/poke for bring-up.
 * Usage: mmio r <addr>        read 32-bit
 *        mmio w <addr> <val>  write 32-bit
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

int main(int argc, char **argv)
{
	unsigned long addr, page, off, val = 0;
	volatile uint32_t *p;
	int fd, wr;
	void *m;

	if (argc < 3) {
		fprintf(stderr, "usage: mmio r|w <addr> [val]\n");
		return 2;
	}
	wr = argv[1][0] == 'w';
	addr = strtoul(argv[2], NULL, 0);
	if (wr && argc < 4) {
		fprintf(stderr, "value required\n");
		return 2;
	}
	if (wr)
		val = strtoul(argv[3], NULL, 0);

	fd = open("/dev/mem", wr ? O_RDWR | O_SYNC : O_RDONLY | O_SYNC);
	if (fd < 0) {
		perror("/dev/mem");
		return 1;
	}
	page = addr & ~0xfffUL;
	off = addr & 0xfffUL;
	m = mmap(NULL, 0x1000, PROT_READ | (wr ? PROT_WRITE : 0), MAP_SHARED,
		 fd, page);
	if (m == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	p = (volatile uint32_t *)((char *)m + off);
	if (wr) {
		*p = (uint32_t)val;
		printf("0x%08lx = 0x%08x\n", addr, *p);
	} else {
		printf("0x%08x\n", *p);
	}
	munmap(m, 0x1000);
	close(fd);
	return 0;
}
