/*
 * Milestone 4: act as iBoot for xnu.
 *
 * mkpayload.py already resolved the kernel's chained fixups and built the
 * ADT, so this only has to: find the payload in the ABL ramdisk, copy the
 * segments to phys_base + (vmaddr - virt_base), place the ADT and
 * boot_args behind the kernel, and jump to the entry point with the MMU
 * and caches off, x0 = boot_args (physical).
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "loader.h"
#include "board.h"

#define BOOT_LINE_LENGTH	608
#define PAGE_16K		0x4000UL

struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};

struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2 */
struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload header layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

/* MMU off: everything is Device memory, so copy in aligned 8-byte words. */
static void copy64(uintptr_t dst, uintptr_t src, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	volatile const uint64_t *s = (volatile const uint64_t *)src;
	for (uint64_t i = 0; i < (n + 7) / 8; i++)
		d[i] = s[i];
}

static void zero64(uintptr_t dst, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	for (uint64_t i = 0; i < n / 8; i++)
		d[i] = 0;
}

static int magic_ok(const char *m)
{
	static const char want[] = "SWEETXNU";
	for (int i = 0; i < 8; i++)
		if (m[i] != want[i])
			return 0;
	return 1;
}

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);

/* Bytes a payload takes in the ramdisk: up to the end of its last segment or its ADT. */
static uint64_t payload_span(const struct payload *p)
{
	uint64_t end = p->adt_off + p->adt_size;
	for (uint32_t i = 0; i < p->nsegs; i++) {
		uint64_t e = p->segs[i].file_off + p->segs[i].file_size;
		if (e > end)
			end = e;
	}
	return end;
}

/* The n-th payload in [rd_start, rd_end), or 0. */
static uint64_t payload_at(uint64_t rd_start, uint64_t rd_end, int n)
{
	uint64_t at = rd_start;
	for (int k = 0;; k++) {
		if (at + sizeof(struct payload) > rd_end)
			return 0;
		const struct payload *p = (const struct payload *)at;
		if (!magic_ok(p->magic) || p->version != 1 || p->nsegs > 64)
			return 0;
		if (k == n)
			return at;
		/* mktripleboot.py aligns each payload offset relative to the ramdisk.
		 * ABL's initrd-start is not guaranteed to have the same alignment, so
		 * align the offset, then add the ramdisk base back. */
		uint64_t offset = at - rd_start;
		offset = (offset + payload_span(p) + 0xfff) & ~0xfffUL;
		at = rd_start + offset;
	}
}

int loader_count(uintptr_t dtb)
{
	uint64_t rd_start, rd_end;
	if (!fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) ||
	    !fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end))
		return 0;
	int n = 0;
	while (n < 8 && payload_at(rd_start, rd_end, n))
		n++;
	return n;
}

const char *loader_boot(uintptr_t dtb)
{
	return loader_boot_n(dtb, 0);
}

const char *loader_boot_n(uintptr_t dtb, int n)
{
	uint64_t rd_start, rd_end;

	if (!fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) ||
	    !fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end))
		return "no initrd in /chosen";
	con_hex2("INITRD", "lo=", rd_start, "hi=", rd_end);

	uint64_t pay = payload_at(rd_start, rd_end, n);
	if (pay == 0)
		return n == 0 ? "no SWEETXNU payload" : "no such SWEETXNU payload";
	const struct payload *p = (const struct payload *)pay;
	if (p->version != 1 || (p->adt_off & 7) || (pay & 7))
		return "bad payload header";
	/* the payload's own offsets are from its start; the overlap check below covers the whole ramdisk */
	uint64_t base = pay;

	uint64_t pa_of_va_base = p->phys_base - p->virt_base;	/* pa = va + this */
	con_hex2("PHYS", "base=", p->phys_base, "size=", p->mem_size);
	con_hex("VIRT", p->virt_base);
	con_hex("ENTRY", p->entry_va);

#if BOARD_MENU
	/*
	 * 2026-11-25: the payload's RAM window (plus the 1 MiB panic log above it, mkadt.py /pram) must be RAM on this
	 * phone: a 7 Pro has 6, 8 or 12 GB (mkadt.py --ram). Checked before anything is copied.
	 */
	uint64_t ram_top = 0;
	if (fdt_memory_covers(dtb, p->phys_base, p->phys_base + p->mem_size + 0x100000, &ram_top) == 0) {
		con_hex("RAMTOP", ram_top);	/* hidden behind the menu's quiet mode; the reason below is shown */
		return "this phone has less RAM than the payload was built for (mkadt.py --ram)";
	}
#endif

	/* The payload must not overlap where the kernel goes. */
	if (rd_end > p->phys_base && rd_start < p->phys_base + p->mem_size)
		return "ramdisk overlaps kernel memory";

	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		uintptr_t dst = s->vmaddr + pa_of_va_base;
		copy64(dst, base + s->file_off, s->file_size);
		if (s->vmsize > s->file_size)
			zero64(dst + ((s->file_size + 7) & ~7UL), s->vmsize - ((s->file_size + 7) & ~7UL));
	}
	con_hex2("SEGS", "n=", p->nsegs, "end=", p->kernel_end + pa_of_va_base);

	/* ADT, then boot_args, behind the kernel; everything below topOfKernelData is ours. */
	uintptr_t adt_pa = p->kernel_end + pa_of_va_base;
	copy64(adt_pa, base + p->adt_off, p->adt_size);
	uintptr_t ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	uintptr_t top = ba_pa + PAGE_16K;

	zero64(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
	ba->memSize = p->mem_size;
	ba->topOfKernelData = top;
	ba->Video.v_baseAddr = FB_BASE;
	ba->Video.v_display = 1;
	ba->Video.v_rowBytes = FB_STRIDE;
	ba->Video.v_width = FB_W;
	ba->Video.v_height = FB_H;
	ba->Video.v_depth = 32;
	ba->deviceTreeP = adt_pa - pa_of_va_base;	/* xnu reads it with the MMU on: a VA */
	ba->deviceTreeLength = (uint32_t)p->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];
	ba->memSizeActual = p->mem_size;
	con_hex2("BOOTARG", "pa=", ba_pa, "top=", top);

	uintptr_t entry_pa = p->entry_va + pa_of_va_base;
	con_hex("JUMP", entry_pa);
	con_str("XNU", "jumping (fbcon clears screen)");

	/* Everything was written with the MMU and caches off; drop any stale I-cache lines too. */
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)entry_pa)(ba_pa);
	return "xnu returned";
}
