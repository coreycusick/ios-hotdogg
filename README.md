# iOS 27.2 on the OnePlus 7T Pro / 7T Pro 5G McLaren (hotdog / hotdogg)

This repository brings the iOS 27.2 port from the OnePlus 7 Pro ([ios-guacamole](https://github.com/coreycusick/ios-guacamole))
to the OnePlus 7T Pro family: the same Qualcomm SM8150 platform (Snapdragon 855+, Adreno 640), the same panel and touch
controller, but a different board, memory map and partition layout. This is the `ios27` branch; macOS 27 is on `macos27`.

## Status

**Not booting.** Bring-up has not started: nothing in this repository has run on either device yet.

| Device | Codename | Models | RAM / storage | Status |
|---|---|---|---|---|
| OnePlus 7T Pro 5G McLaren (T-Mobile) | `hotdogg` | HD1925 | 12 GB / 256 GB | **Not booting** (primary target; bring-up not started) |
| OnePlus 7T Pro | `hotdog` | HD1910, HD1911, HD1913 | 8 GB / 256 GB (McLaren Edition 12 GB) | **Not booting** (not started; expected to need only small board changes) |

The iOS 27 port works on the OnePlus 7 Pro (`guacamole`): see the README and `docs/ios27.md` of
[ios-guacamole](https://github.com/coreycusick/ios-guacamole). Most of that work is expected to carry over, but none of it
is confirmed on these phones until they boot.

## Device notes and plan

[`docs/hotdogg.md`](docs/hotdogg.md) has what is the same as the 7 Pro (panel, touch, haptics, PMIC, cameras, audio,
Wi-Fi/Bluetooth) and what differs (the external SDX55 5G modem, moved firmware memory regions, dynamic partitions),
the decisions taken (8 GB for XNU + 3 GB GPU pool; `super`, then `userdata`, then the iOS root, then the macOS root)
and the bring-up order. [`docs/hotdogg-vs-guacamole.txt`](docs/hotdogg-vs-guacamole.txt) is the device-tree
comparison from OnePlus's kernel sources.

## Inherited from the 7 Pro port

The history, tools, shim, userland drivers and kernel diffs (`xnu/ios27/`) are those of ios-guacamole at the point this
repository was created; their documentation (`docs/ios27.md`, `NOTES.md`, `docs/dualboot.md`) describes the 7 Pro.
The `guacamole` remote points at ios-guacamole so fixes can be merged in either direction.

## Security and artifacts

Development on the owner's devices only: code-signing, sandbox and other security paths are modified or stubbed. iOS
firmware, root images, device dumps (EDL backups, `persist`, `modemst*`, `fsg`: they hold the IMEI), credentials and
build trees are not source-controlled.

## Credits

Built on Apple's open-source XNU and related releases, Qualcomm and OnePlus hardware documentation and kernel sources,
[Mesa](https://mesa3d.org) Freedreno/Turnip, [HackiOS](https://github.com/oosexgang/hackios-xnu-kernel),
[ios-topaz](https://github.com/MakrSas/ios-topaz), [ios-flame](https://github.com/NWylynko/ios-flame),
[UniXNU](https://github.com/UniXNU/UniXNU), and [OtVaL228/ios-sweet](https://github.com/OtVaL228/ios-sweet). iOS itself
is Apple's and is not distributed here.
