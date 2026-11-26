# OnePlus 7T Pro 5G McLaren (hotdogg) — device notes

Second device for the iOS 27 / macOS 27 port, alongside the OnePlus 7 Pro (guacamole), which stays the working
reference. This repo was seeded from the ios-guacamole history (branches `ios27` and `macos27`) so fixes can be
merged or cherry-picked between the two.

## The phone
- Retail: OnePlus 7T Pro 5G McLaren, T-Mobile **HD1925**, 12 GB RAM, 256 GB UFS, SIM-unlocked, shipped on Android 12.
- SoC: SM8150 (Snapdragon 855+), Adreno 640: same as guacamole.
- Device tree (OnePlusOSS android_kernel_oneplus_sm8150, branch `SM8150_SDX50M_Q_10.0`, overlays
  `hotdogg-sdx55m-overlay-{dvt,pvt}.dts`): board-id **<8 2>** (guacamole <8 0>), project **19861**
  (guacamole 18821/18831), hw-id 51/52 (DVT) or 53/54 (PVT). Full comparison: `docs/hotdogg-vs-guacamole.txt`.

## Same as guacamole (our drivers should carry over)
- Panel: Samsung OnePlus DSC command-mode panel, 1440x3120, reset TLMM 6, TE TLMM 8, DCS brightness 1-1023.
- Touch: `sec-s6sy761` @0x48 on QUPv3 SE17 I2C, reset TLMM 54, IRQ TLMM 122, pm8150_l17. Only the touch
  project/calibration ID differs (19861).
- Haptics: AW8697 @0x5a (SE7), reset TLMM 116, IRQ TLMM 24; PM8150B LRA.
- PMIC/charger: PM8150/PM8150B/PM8150L/PM8009. Battery 4085 mAh (guacamole 4000).
- Cameras: same four sensor slots and CCI/CSIPHY wiring. Audio: WCD/WSA, same. Wi-Fi/BT: WCN3990 (ICNSS), same.
  Wi-Fi firmware runs on the SoC's own modem DSP (as on guacamole), not on the 5G modem.
- Boot splash region 0x9c000000 (0x2400000): same.

## Different
- **External 5G modem SDX55** on PCIe1/MHI (ESOC). Cellular (LTE and 5G) most likely goes through it; the SoC's
  modem region shrinks to 50 MB. Our kernel never starts PCIe1: the SDX55 stays off. Cellular is out of scope for
  now (no working SIM). Reference for later: OnePlus 8 / 8 Pro / 8T (SM8250 + SDX55).
- **Reserved memory** (modem/DSP block moved down):

  | node | guacamole | hotdogg |
  |---|---|---|
  | pil_adsp_mem | 0x8be00000 +0x01a00000 | 0x8be00000 +0x01e00000 |
  | pil_modem_mem | 0x8d800000 +0x09600000 | 0x8dc00000 +0x03200000 |
  | pil_video_mem | 0x96e00000 +0x00500000 | 0x90e00000 +0x00500000 |
  | pil_slpi_mem | 0x97300000 +0x01400000 | 0x91300000 +0x01500000 |
  | pil_ipa_fw / gsi / gpu | 0x98700000.. | 0x92800000.. |
  | pil_spss_mem | 0x98800000 +0x00100000 | 0x92900000 +0x00100000 |
  | pil_cdsp_mem | 0x98900000 +0x01400000 | 0x92a00000 +0x01400000 |

  The rest (hyp, xbl_aop, smem, removed_regions, camera/wlan/npu PIL, qseecom, cdsp_sec, cont_splash) is the same.
  Size-only pools are placed by the bootloader at boot: take the real map from the phone (`/proc/iomem`).
- **Partitions:** dynamic partitions (`super`), unlike guacamole.

## Decisions
- **Memory:** 8 GB for XNU + **3 GB GPU pool** (guacamole: 5 GB + ~2 GB). Pool first from ranges XNU cannot use,
  topped up from normal RAM. Exact ranges from the phone's live memory map.
- **Partition layout:** `super` untouched (room to grow for Android / One UI 4 later), then `userdata` (shrunk),
  then the **iOS root**, then the **macOS root** at the end. Firmware partitions (modem, persist, modemst, fsg, ...)
  untouched. Exact sectors only from the phone's real GPT, reviewed before anything is written.
- **Shim:** a hotdogg board entry next to guacamole (detected from the DT board-id/project), not a copy. Uses
  hotdogg's own DTB/DTBO from its stock images.
- **Kernel:** for now the shared tree `work/xnu-12377` of the ios-guacamole checkout; a hotdogg board config later.

## Bring-up order
1. Stock Android 12 checks on arrival: model HD1925, hardware, OEM unlocking state, then
   `adb shell 'getprop ro.boot.slot_suffix; getprop ro.boot.project_name; getprop ro.boot.hw_version;
   getprop ro.build.fingerprint; cat /proc/meminfo | head -2; ls -l /dev/block/by-name/'`.
2. **Full EDL backup of Android 12** (all partitions incl. bootloaders, firmware, super, boot, dtbo, persist,
   modemst1/2, fsg) before anything else. Local only, never in git (`backup/` is ignored): some hold the IMEI.
3. Android 12 has no bootloader unlock on this model: downgrade to Android 11 (MSM tool), unlock (patched method;
   removes `reserve.img`, so no delta OTAs afterwards). Do this on one phone first.
4. Optionally restore Android 12 firmware/vendor from the backup (for One UI 4) — only after confirming on hotdogg
   that Android 12's abl keeps an unlocked phone unlocked, and that there is no anti-rollback problem.
5. Recovery via `fastboot boot`: save boot, dtbo, firmware and `/proc/iomem`; read the GPT.
6. Repartition (plan above), then the first `fastboot boot` of our shim, then iOS, then macOS.
