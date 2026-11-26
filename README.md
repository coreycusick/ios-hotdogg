# macOS 27 on the OnePlus 7T Pro 5G McLaren (hotdogg)

macOS 27.2 for the OnePlus 7T Pro 5G McLaren, from the OnePlus 7 Pro port
([ios-guacamole](https://github.com/coreycusick/ios-guacamole), branch `macos27`). This is the `macos27` branch; iOS 27
is on `ios27`.

## Status

**Not booting.** Bring-up has not started: nothing in this repository has run on the device yet.

| Device | Codename | Models | RAM / storage | Status |
|---|---|---|---|---|
| OnePlus 7T Pro 5G McLaren (T-Mobile) | `hotdogg` | HD1925 | 12 GB / 256 GB | **Not booting** (bring-up not started) |

macOS 27 works on the OnePlus 7 Pro (`guacamole`): see the `macos27` README and `docs/macos27.md` of ios-guacamole.
It is expected to carry over with the board changes in [`docs/hotdogg.md`](docs/hotdogg.md), but none of it is
confirmed on this phone until it boots. The macOS root goes in its own partition after the iOS root, outside
`super`.

The history, patches (`xnu/macos27/`), tools and documentation are those of ios-guacamole's `macos27` at the point this
repository was created and describe the 7 Pro. Apple artifacts (IPSW, filesystems, kernelcaches, dyld caches, dumps)
never go into git.
