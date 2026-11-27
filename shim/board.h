#pragma once
/*
 * Per-board constants for the shim. Build with BOARD=guacamole (OnePlus 7 Pro) or BOARD=hotdogg
 * (OnePlus 7T Pro 5G McLaren) (see build.sh); the default stays sweet.
 *
 * BOARD_MENU: the boot menu (iOS 27 / macOS 27 / Android) and the board check (shim.c board_check):
 * the tree ABL hands over must have qcom,board-id <8 BOARD_ID_SUBTYPE> and BOARD_PROJECT in its model,
 * or nothing boots (a guacamole image on hotdogg would give XNU RAM that is not there, and back).
 */
#if defined(BOARD_HOTDOGG)
/* OnePlus 7T Pro 5G McLaren (hotdogg, T-Mobile HD1925), SM8150 (docs/hotdogg.md). Same panel (1440x3120
 * Samsung DSC, splash at the same cont_splash_region), touch, PMIC and QUP/TLMM addresses as guacamole.
 * Board-id <8 2>, project 19861 (model "SDX55M MTP 19861 51 52" DVT / "... 53 54" PVT; OnePlusOSS overlays).
 * UNVERIFIED on the phone: IMEM restart_reason and that its ABL also needs the PMIC PON reboot reason (same
 * ABL family as guacamole's, so the same values until a live FDT says otherwise). */
#define BOARD_NAME		"hotdogg"
#define BOARD_MENU		1
#define BOARD_ID_SUBTYPE	2
#define BOARD_PROJECT		"19861"
#define FB_W			1440
#define FB_H			3120
#define IMEM_RESTART_REASON	0x146bf65cUL
#define REBOOT_VIA_PMIC_PON	1
#define PSHOLD_WRITABLE		0
#elif defined(BOARD_GUACAMOLE)
/* OnePlus 7 Pro (guacamole), SM8150. Facts from the live FDT + guac-probe:
 *  - splash: two VIG pipes (720 px halves) scanning one 1440x3120 a8r8g8b8
 *    buffer at cont_splash_region, stride 5760; the panel keeps refreshing
 *    with no MDP writes from us (CTL_START kicks corrupt it: never kick).
 *  - this ABL ignores IMEM restart_reason: reboot-to-fastboot is the PMIC
 *    PON_SOFT_RB_SPARE reason + PSCI SYSTEM_RESET. Writing PS_HOLD from EL1
 *    is XPU-protected (QUALCOMM CrashDump Mode). */
#define BOARD_NAME		"guacamole"
#define BOARD_MENU		1
#define BOARD_ID_SUBTYPE	0
#define BOARD_PROJECT		"18821"	/* live FDT model "MTP 18821 18831 21 22" */
#define FB_W			1440
#define FB_H			3120
#define IMEM_RESTART_REASON	0x146bf65cUL	/* msm-imem@146bf000 + 0x65c (unused by ABL) */
#define REBOOT_VIA_PMIC_PON	1
#define PSHOLD_WRITABLE		0
#else
#define BOARD_NAME		"sweet"
#define BOARD_MENU		0
#define FB_W			1080
#define FB_H			2400
#define IMEM_RESTART_REASON	0x146aa65cUL	/* qcom,msm-imem restart_reason@65c */
#define REBOOT_VIA_PMIC_PON	0
#define PSHOLD_WRITABLE		1
#endif
