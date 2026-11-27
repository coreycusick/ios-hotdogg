/*
 * Milestone 2: one boot answers the questions that decide the XNU port.
 *   - CPU feature registers (16K granule, GIC sysreg interface)
 *   - PSCI/SMCCC version and which PSCI calls the Qualcomm TZ offers
 *   - SMP: PSCI CPU_ON for the 7 secondaries, each reports its MPIDR
 *   - GICv3 Group 1 IRQ from the virtual timer (INTID 19 on Qualcomm)
 * Any unexpected exception paints a blue panic screen.
 */
#include <stdint.h>
#include "console.h"
#include "mmu.h"
#include "loader.h"
#include "ufs.h"
#include "board.h"
#include "spmi.h"
#include "bootsel.h"
#include "android.h"
#include "menu.h"
#include "fdt.h"

#define RESTART_BOOTLOADER	0x77665500U
#define PSHOLD			0x0c264000UL

#define GICD_BASE		0x17a00000UL
#define GICR_BASE		0x17a60000UL
#define GICR_SIZE		0x100000UL
#define GICR_PE_SIZE		0x20000UL
#define GICR_TYPER		0x08
#define GICR_WAKER		0x14
#define GICR_SGI		0x10000
#define GICR_IGROUPR0		(GICR_SGI + 0x080)
#define GICR_ISENABLER0		(GICR_SGI + 0x100)
#define GICR_ICENABLER0		(GICR_SGI + 0x180)
#define GICR_IPRIORITYR		(GICR_SGI + 0x400)
#define GICR_ISPENDR0		(GICR_SGI + 0x200)
#define GICR_ISACTIVER0		(GICR_SGI + 0x300)
#define GICR_ICFGR1		(GICR_SGI + 0xc04)
#define VTIMER_INTID		19	/* DT: <GIC_PPI 3 ...>, not the usual 27 */
#define PTIMER_INTID		18	/* DT: <GIC_PPI 2 ...> */
#define SGI_INTID		1

#define PSCI_VERSION		0x84000000UL
#define PSCI_CPU_SUSPEND	0xc4000001UL
#define PSCI_CPU_OFF		0x84000002UL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_MIGRATE_INFO_TYPE	0x84000006UL
#define PSCI_SYSTEM_OFF		0x84000008UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define PSCI_FEATURES		0x8400000aUL
#define PSCI_SYSTEM_SUSPEND	0xc400000eUL
#define PSCI_SYSTEM_RESET2	0xc4000012UL
#define SMCCC_VERSION		0x80000000UL

#define NR_CPUS			8

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))
#define isb() __asm__ volatile("isb" ::: "memory")

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline uint64_t rd64(uintptr_t a) { return *(volatile uint64_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }

extern char secondary_entry[];

static const char *stage;	/* set at runtime: no absolute pointers in data */
static volatile uint64_t sec_mpidr[NR_CPUS];
static volatile uint64_t sec_probe[4];	/* CPU1 redistributor write test */
static volatile uint32_t irq_count, irq_other, irq_last;
static volatile int irq_src;

/* ICC_SGI1R_EL1 value targeting the calling core only. */
static uint64_t sgi_self(unsigned id)
{
	uint64_t m = rsr("mpidr_el1");
	return ((m >> 16) & 0xff) << 32 |	/* Aff2 */
	       ((m >> 8) & 0xff) << 16 |	/* Aff1 */
	       (uint64_t)id << 24 |
	       (1U << (m & 0xf));		/* target list: Aff0 */
}

static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static uint64_t now(void)
{
	isb();
	return rsr("cntpct_el0");
}

static void delay_ms(unsigned ms)
{
	uint64_t end = now() + rsr("cntfrq_el0") / 1000 * ms;
	while (now() < end)
		;
}

static void __attribute__((noreturn)) reset(unsigned after_s)
{
	con_skip();
	con_str("RESET", after_s > 20 ? "in 40s" : "in 20s");
	delay_ms(after_s * 1000);
#if REBOOT_VIA_PMIC_PON
	if (pon_set_reboot_bootloader())
		con_str("RESET", "PON reason write failed: will boot the OS");
#endif
	wr32(IMEM_RESTART_REASON, RESTART_BOOTLOADER);
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
#if PSHOLD_WRITABLE
	wr32(PSHOLD, 0);
#endif
	for (;;)
		__asm__ volatile("wfe");
}

/* ---- exceptions ---- */

static void irq_handler(void)
{
	uint64_t iar = rsr("icc_iar1_el1");
	uint32_t id = iar & 0xffffff;

	if (id >= 1020 && id <= 1023)
		return;	/* spurious */
	irq_last = id;
	if (id == (uint32_t)irq_src)
		irq_count++;
	else
		irq_other++;
	if (id == VTIMER_INTID)
		wsr("cntv_tval_el0", rsr("cntfrq_el0") / 100);	/* 10 ms */
	else if (id == PTIMER_INTID)
		wsr("cntp_tval_el0", rsr("cntfrq_el0") / 100);
	wsr("icc_eoir1_el1", iar);
	isb();
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	if (slot == 5) {	/* IRQ from current EL, SPx */
		irq_handler();
		return;
	}
	con_quiet = 0;
	con_clear(BLUE);
	switch (slot & 3) {
	case 0: con_str("PANIC", "SYNC"); break;
	case 1: con_str("PANIC", "IRQ"); break;
	case 2: con_str("PANIC", "FIQ"); break;
	default: con_str("PANIC", "SERROR"); break;
	}
	con_hex2("SLOT", "n=", slot, "cpu=", rsr("mpidr_el1"));
	con_str("STAGE", stage);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("SPSR", rsr("spsr_el1"));
	con_hex("X0", regs[0]);
	con_hex("X1", regs[1]);
	con_hex("X30", regs[29]);	/* stashed lr is just above the frame */
	reset(40);
}

/* ---- SMP ---- */

static uintptr_t find_gicr(void);

void secondary_main(uint64_t idx)
{
	sec_mpidr[idx] = rsr("mpidr_el1") | (1ULL << 63);
	if (idx == 1) {
		uintptr_t r = find_gicr();
		if (r) {
			sec_probe[0] = rd32(r + GICR_IGROUPR0);
			wr32(r + GICR_IGROUPR0, 0xffffffff);
			sec_probe[1] = rd32(r + GICR_IGROUPR0);
			wr8(r + GICR_IPRIORITYR + 19, 0xa0);
			sec_probe[2] = rd32(r + GICR_IPRIORITYR + 16);
		}
		sec_probe[3] = r ? 1 : 0xdead;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_CPU_OFF, 0, 0, 0);
}

static void __attribute__((unused)) test_smp(void)
{
	stage = "smp";
	uint64_t self = rsr("mpidr_el1") & 0xffffff;
	int64_t ret[NR_CPUS];

	for (int i = 1; i < NR_CPUS; i++)
		ret[i] = smc(PSCI_CPU_ON, (uint64_t)i << 8, (uintptr_t)secondary_entry, i);
	delay_ms(500);
	for (int i = 1; i < NR_CPUS; i++) {
		char label[] = "CPU0";
		label[3] = '0' + i;
		if (((uint64_t)i << 8) == self)
			con_str(label, "is boot cpu");
		else
			con_hex2(label, "ret=", (uint32_t)ret[i], "mpidr=",
				 (uint32_t)sec_mpidr[i] | (sec_mpidr[i] ? 0 : 0xdead0000));
	}
}

/* ---- GIC + timer ---- */

static uintptr_t find_gicr(void)
{
	uint64_t aff = rsr("mpidr_el1");
	uint32_t want = (aff & 0xffffff) | ((aff >> 8) & 0xff000000);

	for (uintptr_t off = 0; off < GICR_SIZE; off += GICR_PE_SIZE) {
		uint64_t typer = rd64(GICR_BASE + off + GICR_TYPER);
		if ((uint32_t)(typer >> 32) == want)
			return GICR_BASE + off;
		if (typer & (1 << 4))	/* Last */
			break;
	}
	return 0;
}

static void gicd_wait_rwp(void)
{
	for (int i = 0; i < 1000000 && (rd32(GICD_BASE) & (1U << 31)); i++)
		;
}

/* Run one interrupt source for ~300 ms and report what the GIC saw. */
static void irq_window(const char *label, uintptr_t r, int src)
{
	uint64_t f = rsr("cntfrq_el0");
	uint32_t pend = 0, act = 0, isr = 0, hppi = 0x3ff, ist = 0;

	irq_count = irq_other = 0;
	irq_last = 0x3ff;
	irq_src = src;
	if (src == VTIMER_INTID) {
		wsr("cntv_tval_el0", f / 100);
		wsr("cntv_ctl_el0", 1);
	} else if (src == PTIMER_INTID) {
		wsr("cntp_tval_el0", f / 100);
		wsr("cntp_ctl_el0", 1);
	}
	isb();
	/* sample state with IRQs still masked, after the first period */
	uint64_t t = now() + f / 50;
	while (now() < t)
		;
	if (src == SGI_INTID)
		wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
	isb();
	ist = src == PTIMER_INTID ? rsr("cntp_ctl_el0") : rsr("cntv_ctl_el0");
	pend = rd32(r + GICR_ISPENDR0);
	act = rd32(r + GICR_ISACTIVER0);
	isr = rsr("isr_el1");
	hppi = rsr("icc_hppir1_el1");

	__asm__ volatile("msr daifclr, #2" ::: "memory");
	t = now() + f * 3 / 10;
	/* SGI: one already sent above, send 9 more 10 ms apart -> expect 10 */
	uint64_t next = now() + f / 100;
	for (int sent = 1; now() < t;)
		if (src == SGI_INTID && sent < 10 && now() >= next) {
			wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
			isb();
			sent++;
			next += f / 100;
		}
	__asm__ volatile("msr daifset, #2" ::: "memory");
	wsr("cntv_ctl_el0", 0);
	wsr("cntp_ctl_el0", 0);
	isb();

	con_hex2(label, "irqs=", irq_count, "other=", irq_other);
	con_hex2(" pre", "pend=", pend, "act=", act);
	con_hex2(" pre", "isr=", isr, "hppi=", hppi);
	con_hex2(" pre", "tctl=", ist, "last=", irq_last);
}

static void test_gic(void)
{
	stage = "gic sre";
	uint64_t sre = rsr("icc_sre_el1");
	if (!(sre & 1)) {
		wsr("icc_sre_el1", sre | 1);
		isb();
	}
	if (!(rsr("icc_sre_el1") & 1)) {
		con_str("GIC", "no sysreg iface, skip");
		return;
	}

	stage = "gic dist";
	uint32_t dctlr = rd32(GICD_BASE);
	/* Non-secure view (DS=0): bit0 EnableGrp1, bit1 EnableGrp1A, bit4 ARE_NS; Linux writes 0x13 */
	wr32(GICD_BASE, dctlr | 0x13);
	gicd_wait_rwp();
	con_hex2("GICD", "was=", dctlr, "now=", rd32(GICD_BASE));

	stage = "gic redist";
	uintptr_t r = find_gicr();
	if (!r) {
		con_str("GICR", "not found");
		return;
	}
	uint32_t waker = rd32(r + GICR_WAKER);
	if (waker & 2) {
		wr32(r + GICR_WAKER, waker & ~2U);
		for (int i = 0; i < 100000 && (rd32(r + GICR_WAKER) & 4); i++)
			;
	}
	con_hex2("GICR", "base=", r, "waker=", rd32(r + GICR_WAKER));

	stage = "gic config";
	uint32_t grp = rd32(r + GICR_IGROUPR0);
	wr32(r + GICR_IGROUPR0, 0xffffffff);	/* NS view: secure bits are WI */
	uint32_t en_before = rd32(r + GICR_ISENABLER0);
	uint32_t pri_before = rd32(r + GICR_IPRIORITYR + 16);	/* INTIDs 16..19 */
	for (int id = 0; id < 32; id++)
		wr8(r + GICR_IPRIORITYR + id, 0xa0);
	wr32(r + GICR_ISENABLER0, (1U << VTIMER_INTID) | (1U << PTIMER_INTID) | (1U << SGI_INTID));
	con_hex2("ENABLE", "was=", en_before, "now=", rd32(r + GICR_ISENABLER0));
	(void)grp;
	(void)pri_before;

	uint64_t ctlr = rsr("icc_ctlr_el1");
	wsr("icc_ctlr_el1", ctlr & ~2ULL);	/* EOImode 0: EOIR also deactivates */
	wsr("icc_pmr_el1", 0xf0);
	wsr("icc_bpr1_el1", 0);
	wsr("icc_igrpen1_el1", 1);
	isb();
	con_hex2("ICC", "ctlr=", ctlr, "grpen1=", rsr("icc_igrpen1_el1"));

	stage = "gic vtimer";
	irq_window("VTIMER", r, VTIMER_INTID);
	stage = "gic sgi";
	irq_window("SGI1", r, SGI_INTID);
	wr32(r + GICR_ICENABLER0, 0xffffffff);
	isb();
}

/* ---- main ---- */

static void __attribute__((unused)) psci_line(const char *label, uint64_t fn)
{
	con_hex2(label, "feat=", smc(PSCI_FEATURES, fn, 0, 0), 0, 0);
}

static uint64_t scratch[8192];	/* 64KB, lives in our (later cacheable) image */

static uint64_t ticks_fill(uint32_t c)
{
	uint64_t t = now();
	con_clear(c);
	return now() - t;
}

static uint64_t ticks_mem(void)
{
	uint64_t t = now();
	for (int pass = 0; pass < 16; pass++)
		for (int i = 0; i < 8192; i++)
			scratch[i] = scratch[i] * 3 + i;
	return now() - t;
}

static uint32_t to_us(uint64_t ticks)
{
	return ticks * 10 / 192;	/* 19.2 MHz */
}

#if BOARD_MENU
/*
 * 2026-11-25 (hotdogg): is this the phone the image was built for? ABL's tree must have qcom,board-id
 * <8 BOARD_ID_SUBTYPE> and BOARD_PROJECT in its model ("MTP 18821 18831 21 22" on guacamole, "SDX55M MTP 19861
 * 53 54" on hotdogg). 0 = yes, otherwise the reason. Nothing has been written anywhere when it runs.
 */
static const char *board_check(uint64_t dtb)
{
	uintptr_t v;
	uint32_t len;
	if (!fdt_root_prop(dtb, "qcom,board-id", &v, &len) || len < 8)
		return "no qcom,board-id in the bootloader's device tree";
	const volatile uint8_t *b = (const volatile uint8_t *)v;
	uint32_t id0 = (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
	uint32_t id1 = (uint32_t)b[4] << 24 | b[5] << 16 | b[6] << 8 | b[7];
	if (id0 != 8 || id1 != BOARD_ID_SUBTYPE)
		return id1 == 0 ? "this is a OnePlus 7 Pro (board-id 8/0), not " BOARD_NAME :
		       id1 == 2 ? "this is a OnePlus 7T Pro 5G McLaren (board-id 8/2), not " BOARD_NAME :
				  "unknown board-id, not " BOARD_NAME;
	if (!fdt_root_prop(dtb, "model", &v, &len) || len == 0)
		return "no model in the bootloader's device tree";
	const char *want = BOARD_PROJECT;
	const volatile char *m = (const volatile char *)v;
	for (uint32_t i = 0; i < len; i++) {
		uint32_t k = 0;
		while (want[k] && i + k < len && m[i + k] == want[k])
			k++;
		if (!want[k])
			return 0;
	}
	return "the model does not name project " BOARD_PROJECT ": not " BOARD_NAME;
}
#endif

void shim_main(uint64_t dtb)
{
	struct mmu_info mi;

	/* An xnu payload in the ramdisk means: boot it (MMU stays off). */
	stage = "loader";
#if BOARD_MENU
	con_quiet = 1;	/* black until the menu; an error turns the text back on */
	/* No clear here: the menu clears the screen with the MMU on (3 ms instead of ~190 ms). */
#else
	con_clear(MENU_BG);	/* 2026-11-17: was red, a red flash before the menu */
#endif
	con_str("SHIM", "sweet milestone 4: xnu loader");
	con_str("BOARD", BOARD_NAME);
	con_skip();
#if BOARD_MENU
	stage = "board";
	const char *wrong = board_check(dtb);
	if (wrong) {	/* wrong phone for this image: say so, then power off (nothing was written) */
		con_quiet = 0;
		con_clear(BLACK);
		con_str("BOARD", "this boot image was built for " BOARD_NAME);
		con_str("WRONG", wrong);
		con_str("NEXT", "power off in 60 s; hold power + volume up for fastboot");
		delay_ms(60000);
		smc(PSCI_SYSTEM_OFF, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfi");
	}
	/*
	 * Boot menu (menu.c), 2026-11-20: pictures of iOS 27 / macOS 27 / Android, then small Restart and
	 * Power off buttons in the bottom right corner; volume keys to move, power to start.
	 * No Alert Slider: the default is the last choice, kept in the boot menu memory block (ufs.c
	 * ufs_bootmem_*: unallocated space at the end of LUN 0); none kept, iOS 27. It starts after 5 s
	 * unless a key is pressed. If Android cannot be started the reason stays on screen for 15 s,
	 * then iOS 27 boots. Payload 0 = iOS 27 (partition ios27), payload 1 = macOS 27 (partition macos).
	 */
	stage = "bootsel";
	static uint8_t mem[4096] __attribute__((aligned(4096)));
	int memok = !ufs_open() && !ufs_bootmem_read(mem);
	int kept = -1;
	if (memok && mem[0] == 'S' && mem[1] == 'W' && mem[2] == 'B' && mem[3] == 'M' && mem[4] == 1 &&
	    mem[5] < 3 && mem[6] == (uint8_t)~mem[5])
		kept = mem[5];
	/* Filled in code, not with an initializer: a table of string pointers in data would need
	 * relocation, and the shim runs unrelocated. */
	struct menu_entry entries[5];
	int npay = loader_count(dtb);
	entries[0].title = "iOS 27";
	entries[0].detail = npay >= 1 ? "iPhone 14 Pro, partition ios27" : "not in this boot image";
	entries[0].enabled = npay >= 1;
	entries[0].icon = menu_icon_ios27;
	entries[1].title = "macOS 27";
	entries[1].detail = npay >= 2 ? "partition macos" : "not installed yet";
	entries[1].enabled = npay >= 2;
	entries[1].icon = menu_icon_macos27;
	entries[2].title = "Android";
	entries[2].detail = "boot_a";
	entries[2].enabled = 1;
	entries[2].icon = menu_icon_android;
	for (int i = 0; i < 3; i++)
		entries[i].small = 0;
	entries[3].title = "Restart";
	entries[4].title = "Power off";
	for (int i = 3; i < 5; i++) {
		entries[i].detail = "";
		entries[i].enabled = 1;
		entries[i].icon = 0;
		entries[i].small = 1;
	}
	__asm__ volatile("" ::: "memory");
	int def = (kept >= 0 && entries[kept].enabled) ? kept : entries[0].enabled ? 0 : 2;
	/* MMU on for drawing (mmu.c: image Normal WB, framebuffer Normal-NC, everything else Device as
	 * with the MMU off); off again before the UFS write below. Without it the menu still works, slowly. */
	stage = "menu mmu";
	int menu_mmu = mmu_enable(&mi) == 0;
	if (!menu_mmu)
		con_clear(MENU_BG);
	stage = "menu";
	/* Restart / Power off (PSCI) never return when they work; if one does, the menu comes back
	 * with no countdown. Neither is remembered as the default. */
	int choice = menu_choose(entries, 5, def, 5000);
	while (choice >= 3) {
		smc(choice == 3 ? PSCI_SYSTEM_RESET : PSCI_SYSTEM_OFF, 0, 0, 0);
		entries[choice].title = "Failed";
		choice = menu_choose(entries, 5, choice, 0);
	}
	/* The chosen OS's picture while the MMU is still on, then MMU off for everything that follows. */
	/* iOS / macOS: the boot picture when the build has one (tools/mkbootpic.py), else the OS's own picture */
	if (choice == 2 || !menu_bootpic())
		menu_booting(choice == 2 ? menu_icon_android : choice == 1 && entries[1].enabled ? menu_icon_macos27 :
			     menu_icon_ios27);
	if (menu_mmu)
		mmu_disable();
	stage = "bootsel";
	if (memok && choice != kept) {	/* written only when the choice changed */
		for (unsigned i = 0; i < sizeof(mem); i++)
			mem[i] = 0;
		mem[0] = 'S'; mem[1] = 'W'; mem[2] = 'B'; mem[3] = 'M'; mem[4] = 1;
		mem[5] = (uint8_t)choice; mem[6] = (uint8_t)~choice;
		(void)ufs_bootmem_write(mem);
	}
	con_quiet = 1;	/* only the chosen picture until the kernel takes the screen; errors turn text back on */
	if (choice == 2) {
		stage = "android";
		const char *afail = android_boot(dtb);
		con_quiet = 0;
		con_clear(BLACK);
		con_str("ANDROID", afail);
		bootsel_report(dtb, SLIDER_UNKNOWN, 0);
		if (memok) {	/* next boot defaults to iOS 27, not to the Android that failed */
			mem[5] = 0; mem[6] = 0xff;
			(void)ufs_bootmem_write(mem);
		}
		delay_ms(15000);
		con_quiet = 1;
	}
	stage = "loader";
	int pay_index = 0;
	if (choice == 1 && entries[1].enabled) {
		pay_index = 1;
	}
#else
	int pay_index = 0;
#endif
	const char *why = loader_boot_n(dtb, pay_index);
	if (con_quiet) {	/* the iOS loader failed after the boot menu: show why */
		con_quiet = 0;
		con_clear(BLACK);
	}
	con_str("LOADER", why);
	con_skip();

	/* No payload: probe the internal UFS while the MMU is still off. */
	stage = "ufs";
	test_ufs();
	con_skip();
	/* Keep the probe results on screen (later tests clear it); reset after 3 min. */
	con_str("UFS", "probe done, reset in ~3 min");
	delay_ms(180000);
	reset(20);

	stage = "fill off";
	uint64_t fill_off = ticks_fill(BLACK);
	uint64_t mem_off = ticks_mem();

	stage = "mmu enable";
	int rc = mmu_enable(&mi);

	stage = "fill on";
	uint64_t fill_on = rc ? 0 : ticks_fill(BLACK);
	uint64_t mem_on = rc ? 0 : ticks_mem();
	if (rc)
		con_clear(BLACK);

	con_str("SHIM", "sweet milestone 3");
	con_skip();
	con_hex2("DTB", "at=", dtb, "magic=", dtb ? __builtin_bswap32(rd32(dtb)) : 0);
	con_hex2("IMAGE", "lo=", mi.img_lo, "hi=", mi.img_hi);
	con_hex2("MMU", "rc=", rc, "l3=", mi.n_l3);
	con_hex2("TTBR0", "pa=", mi.ttbr, "pa_hi=", mi.ttbr >> 32);
	con_hex("SCTLR0", mi.sctlr_before);
	con_hex("SCTLR1", mi.sctlr_after);
	con_hex("TCR", rsr("tcr_el1"));
	con_hex2("PARANGE", "mmfr0=", rsr("id_aa64mmfr0_el1") & 0xf, "tg16=", (rsr("id_aa64mmfr0_el1") >> 20) & 0xf);
	con_skip();
	con_hex("PAR img", mmu_probe((uintptr_t)shim_main));
	con_hex("PAR fb", mmu_probe(FB_PHYS + 0x1000));
	con_hex("PAR gic", mmu_probe(GICD_BASE));
	con_hex("PAR 12G", mmu_probe(0x300000000UL));
	con_skip();
	con_hex2("FILL us", "off=", to_us(fill_off), "on=", to_us(fill_on));
	con_hex2("MEM us", "off=", to_us(mem_off), "on=", to_us(mem_on));
	con_skip();

	stage = "psci";
	con_hex2("PSCI", "ver=", smc(PSCI_VERSION, 0, 0, 0), "smccc=", smc(SMCCC_VERSION, 0, 0, 0));
	con_skip();
	test_gic();

	stage = "done";
	reset(40);
}
