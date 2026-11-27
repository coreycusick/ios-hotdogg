#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include "pmem.h"

static int pm_fd = -1;

/*
 * 2026-11-26: direct access. Memory the kernel does not manage (registers, carve-outs, the 0xe0000000 pool) is
 * mapped into this process in 2 MB windows (ioctl PMEM_MAP, kernel xnu12377-pmem-map.diff) and read and written with
 * plain 32-bit loads and stores, with the barriers the kernel's own path uses, instead of one pread/pwrite system
 * call per access (wland + adspd made ~48,000 a second at idle). A window the kernel refuses (kernel-managed RAM:
 * pm_alloc buffers, which the kernel keeps coherent) or that cannot be mapped stays on pread/pwrite, as does
 * everything on a kernel without PMEM_MAP. OFF unless PMEM_DIRECT=1 is in the environment: the first try (new
 * btbridge, 2026-11-26) took the phone down at once; tools/pmemtest finds the step. Device memory faults on
 * unaligned or paired accesses: every access is a single aligned 32-bit volatile load or store.
 */
struct pmem_map {
	uint64_t pa;
	uint64_t size;
	uint64_t uaddr;
};
#define PMEM_MAP	_IOWR('P', 6, struct pmem_map)
#define DWIN		0x200000ull
#define DWINS		64
#define DLOW		0x100000ull	/* never mapped below this (the kernel refuses it too) */
/*
 * 2026-11-26: windows that stay on pread/pwrite. The apps SMMU (0x15000000): Qualcomm's hypervisor traps every access
 * to it and emulates it, so a direct access saves nothing, and adspd's first direct SMMU access crash-dumped the SoC.
 */
static int dexcluded(uint64_t win)
{
	return win == 0x15000000ull;
}

/*
 * Single 32-bit accesses with a base-only address, as the kernel's /dev/pmem path does (sweet_pmem.c mmio_rd/mmio_wr,
 * sweet_gpu.c): a hypervisor that traps an access can emulate it only from a valid syndrome (ISV=1), which post-indexed,
 * pre-indexed or paired forms (what a C copy loop compiles to) do not give. The SoC crash-dumps instead.
 */
static inline uint32_t mrd(volatile uint32_t *p)
{
	uint32_t v;
	__asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(p) : "memory");
	return v;
}

static inline void mwr(volatile uint32_t *p, uint32_t v)
{
	__asm__ volatile("str %w0, [%1]" : : "r"(v), "r"(p) : "memory");
}
#define DMAX		0x10000		/* bulk transfers (firmware, zero fill) stay on pread/pwrite: once, and */
					/* they would fill the table with windows used only at boot */
static struct {
	uint64_t win, from;		/* from: the first mapped address (win, or DLOW for the window at 0) */
	volatile uint32_t *va;		/* NULL: refused, use pread/pwrite */
} dwin[DWINS];
static int ndwin, direct = -1, dlast, dready;

void pm_direct_on(const char *why)
{
	if (dready)
		return;
	dready = 1;
	const char *e = getenv("PMEM_DIRECT");
	if (e && e[0] == '1') {
		char line[160];
		int n = snprintf(line, sizeof(line), "pmem: direct access on (%s), pid %d\n", why, (int)getpid());
		printf("%s", line);
		/* also into the kernel's persistent log, written to flash now: after a crash it shows when direct began */
		int con = open("/dev/console", O_WRONLY);
		if (con >= 0) {
			(void)write(con, line, (size_t)n);
			close(con);
		}
		int ms = 500;
		(void)sysctlbyname("kern.sweet_log_sync", NULL, NULL, &ms, sizeof(ms));
	}
}

/* The mapped window holding pa (n bytes from pa must stay in it), or NULL: use the system call path. */
static volatile uint32_t *dptr(uint64_t pa, size_t n)
{
	uint64_t win = pa & ~(DWIN - 1);
	if (!dready || direct == 0 || ((pa | n) & 3) || pa + n > win + DWIN || pa < DLOW || dexcluded(win))
		return NULL;
	if (direct < 0) {
		const char *e = getenv("PMEM_DIRECT");
		direct = e && e[0] == '1';	/* 2026-11-26: opt-in until the btbridge crash is understood */
		if (!direct)
			return NULL;
	}
	if (ndwin && dwin[dlast].win == win)
		return dwin[dlast].va ? (volatile uint32_t *)((uintptr_t)dwin[dlast].va + (pa - win)) : NULL;
	for (int i = 0; i < ndwin; i++)
		if (dwin[i].win == win) {
			dlast = i;
			return dwin[i].va ? (volatile uint32_t *)((uintptr_t)dwin[i].va + (pa - win)) : NULL;
		}
	if (ndwin == DWINS)
		return NULL;			/* table full: new windows stay on the system call path */
	/* the window at physical 0 is mapped from DLOW only (mapping its bottom took the phone down, 2026-11-26) */
	uint64_t from = win < DLOW ? DLOW : win;
	struct pmem_map m = { .pa = from, .size = DWIN - (from - win) };
	int r = ioctl(pm_fd, PMEM_MAP, &m);
	if (r < 0 && errno == ENOTTY) {		/* kernel without PMEM_MAP */
		direct = 0;
		return NULL;
	}
	dlast = ndwin;
	dwin[ndwin].win = win;
	/* va is where the window's start would be, so pa - win works for every pa at or above `from` */
	dwin[ndwin].va = r < 0 ? NULL : (volatile uint32_t *)(uintptr_t)(m.uaddr - (from - win));
	dwin[ndwin].from = from;
	ndwin++;
	return dwin[dlast].va ? (volatile uint32_t *)((uintptr_t)dwin[dlast].va + (pa - win)) : NULL;
}

/* A range in pieces that each stay inside one window: 0 when every piece went direct, else -1 (nothing done). */
static int dread(uint64_t pa, void *buf, size_t len)
{
	if ((len & 3) || len >= DMAX)
		return -1;
	/* every window first: a register read must not happen twice (direct, then again through the kernel) */
	for (uint64_t a = pa; a < pa + len; a = (a & ~(DWIN - 1)) + DWIN)
		if (!dptr(a, 4))
			return -1;
	uint32_t *out = buf;
	while (len) {
		size_t n = DWIN - (pa & (DWIN - 1));
		if (n > len)
			n = len;
		volatile uint32_t *p = dptr(pa, n);
		if (!p)
			return -1;
		for (size_t i = 0; i < n / 4; i++)
			out[i] = mrd(p + i);
		out += n / 4;
		pa += n;
		len -= n;
	}
	__asm__ volatile("dmb oshld" ::: "memory");
	return 0;
}

static int dwrite(uint64_t pa, const void *buf, size_t len)
{
	if ((len & 3) || len >= DMAX)
		return -1;
	/* every window first, so a range is either all direct or all through the kernel */
	for (uint64_t a = pa; a < pa + len; a = (a & ~(DWIN - 1)) + DWIN)
		if (!dptr(a, 4))
			return -1;
	const uint32_t *in = buf;
	__asm__ volatile("dmb oshst" ::: "memory");
	while (len) {
		size_t n = DWIN - (pa & (DWIN - 1));
		if (n > len)
			n = len;
		volatile uint32_t *p = dptr(pa, n);
		for (size_t i = 0; i < n / 4; i++)
			mwr(p + i, in[i]);
		in += n / 4;
		pa += n;
		len -= n;
	}
	__asm__ volatile("dsb osh" ::: "memory");
	return 0;
}

void pm_init(void)
{
	if (pm_fd >= 0)
		return;
	pm_fd = open("/dev/pmem", O_RDWR);
	if (pm_fd < 0) {
		fprintf(stderr, "/dev/pmem: %s (kernel 0094+, boot-arg sweetpmem=1, run as root)\n",
			strerror(errno));
		exit(1);
	}
}

int pm_rd(uint64_t pa, void *buf, size_t len)
{
	if (((uintptr_t)buf & 3) == 0 && dread(pa, buf, len) == 0)
		return 0;
	ssize_t n = pread(pm_fd, buf, len, (off_t)pa);
	if (n != (ssize_t)len) {
		fprintf(stderr, "pmem: read %zu at %llx: %s\n", len, (unsigned long long)pa,
			n < 0 ? strerror(errno) : "short");
		return -1;
	}
	return 0;
}

int pm_wr(uint64_t pa, const void *buf, size_t len)
{
	if (((uintptr_t)buf & 3) == 0 && dwrite(pa, buf, len) == 0)
		return 0;
	ssize_t n = pwrite(pm_fd, buf, len, (off_t)pa);
	if (n != (ssize_t)len) {
		fprintf(stderr, "pmem: write %zu at %llx: %s\n", len, (unsigned long long)pa,
			n < 0 ? strerror(errno) : "short");
		return -1;
	}
	return 0;
}

uint32_t rd32(uint64_t pa)
{
	uint32_t v;
	return pm_rd(pa, &v, 4) ? 0xdeadbeef : v;
}

void wr32(uint64_t pa, uint32_t v)
{
	pm_wr(pa, &v, 4);
}

void wr64(uint64_t pa, uint64_t v)
{
	wr32(pa, (uint32_t)v);
	wr32(pa + 4, (uint32_t)(v >> 32));
}

uint64_t pm_alloc(uint64_t size, uint64_t align)
{
	struct pmem_alloc a = { .size = size, .align = align };
	if (ioctl(pm_fd, PMEM_ALLOC, &a) < 0) {
		fprintf(stderr, "pmem: alloc %llu: %s\n", (unsigned long long)size, strerror(errno));
		return 0;
	}
	return a.pa;
}

int pm_free(uint64_t pa)
{
	return ioctl(pm_fd, PMEM_FREE, &pa);
}

void pm_smc(uint64_t x[8])
{
	struct pmem_smc s;
	memcpy(s.x, x, sizeof(s.x));
	if (ioctl(pm_fd, PMEM_SMC, &s) < 0) {
		fprintf(stderr, "pmem: smc: %s\n", strerror(errno));
		x[0] = (uint64_t)-1;
		return;
	}
	memcpy(x, s.x, 4 * sizeof(uint64_t));
}

int pm_list(struct pmem_list *l)
{
	return ioctl(pm_fd, PMEM_LIST, l);
}

void delay_ms(unsigned ms)
{
	usleep(ms * 1000);
}

int wait_bit(uint64_t pa, uint32_t mask, int set, unsigned ms)
{
	for (unsigned i = 0; i <= ms; i++) {
		if (!!(rd32(pa) & mask) == set)
			return 0;
		if (i < ms)
			delay_ms(1);
	}
	return -1;
}

int pm_rpmh(const char *res, unsigned n, const uint32_t *off, const uint32_t *val)
{
	struct pmem_rpmh r;
	memset(&r, 0, sizeof(r));
	snprintf(r.res, sizeof(r.res), "%s", res);
	r.n = n;
	for (unsigned i = 0; i < n && i < 4; i++) {
		r.offset[i] = off[i];
		r.value[i] = val[i];
	}
	return ioctl(pm_fd, PMEM_RPMH, &r) < 0 ? errno : 0;
}
