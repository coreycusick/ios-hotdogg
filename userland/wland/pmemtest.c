/*
 * pmemtest [-m] PA... (2026-11-26): -m maps only (no reads: for memory that must not be read, such as the modem's
 * once it runs). find which step of /dev/pmem direct mapping takes the phone down. For each physical
 * address (32-bit aligned): read it the old way (pread), map its 2 MB window (ioctl PMEM_MAP), read it through the
 * mapping. Before every step a line goes to /dev/console (the kernel's persistent log) and sysctl
 * kern.sweet_log_sync writes the log to flash, so after a crash the last line in outputs/ios27-klog.txt
 * (tools/ios27-klog-pull.sh, from Android) is the step that did it. Reads only; writes nothing to any device.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <unistd.h>

struct pmem_map {
	uint64_t pa, size, uaddr;
};
#define PMEM_MAP	_IOWR('P', 6, struct pmem_map)
#define WIN		0x200000ull

static int con = -1;

static void say(const char *fmt, ...)
{
	char line[256];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n > (int)sizeof(line) - 2)
		n = sizeof(line) - 2;
	line[n++] = '\n';
	fwrite(line, 1, (size_t)n, stdout);
	fflush(stdout);
	if (con >= 0)
		(void)write(con, line, (size_t)n);
	int ms = 800;
	(void)sysctlbyname("kern.sweet_log_sync", NULL, NULL, &ms, sizeof(ms));
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: pmemtest [-m] PA...   (hex, 32-bit aligned; reads only; -m: map only)\n");
		return 2;
	}
	int maponly = argc > 1 && strcmp(argv[1], "-m") == 0;
	con = open("/dev/console", O_WRONLY);
	int fd = open("/dev/pmem", O_RDWR);
	if (fd < 0) {
		perror("/dev/pmem");
		return 1;
	}
	for (int i = 1 + maponly; i < argc; i++) {
		uint64_t pa = strtoull(argv[i], NULL, 16);
		if (pa & 3) {
			say("PMEMTEST %#llx: not 32-bit aligned, skipped", (unsigned long long)pa);
			continue;
		}
		uint32_t v = 0;
		if (!maponly) {
			say("PMEMTEST %#llx: 1 pread", (unsigned long long)pa);
			ssize_t r = pread(fd, &v, 4, (off_t)pa);
			say("PMEMTEST %#llx: 1 pread -> %zd value %#x", (unsigned long long)pa, r, v);
		}
		uint64_t win = pa & ~(WIN - 1), from = win < 0x100000ull ? 0x100000ull : win;
		struct pmem_map m = { .pa = from, .size = WIN - (from - win) };
		say("PMEMTEST %#llx: 2 map window %#llx", (unsigned long long)pa, (unsigned long long)m.pa);
		int e = ioctl(fd, PMEM_MAP, &m);
		say("PMEMTEST %#llx: 2 map -> %d (%s) at %#llx", (unsigned long long)pa, e, e ? strerror(errno) : "ok",
		    (unsigned long long)m.uaddr);
		if (e || maponly || pa < from)
			continue;
		volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)(m.uaddr + (pa - m.pa));
		say("PMEMTEST %#llx: 3 direct read at %p", (unsigned long long)pa, (void *)(uintptr_t)p);
		uint32_t d = *p;
		__asm__ volatile("dmb oshld" ::: "memory");
		say("PMEMTEST %#llx: 3 direct read -> %#x (pread gave %#x) %s", (unsigned long long)pa, d, v,
		    d == v ? "same" : "DIFFERENT");
	}
	say("PMEMTEST done");
	return 0;
}
