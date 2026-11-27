#!/usr/bin/env python3
"""mkmenuboot.py - one boot image for boot_b with the boot menu, iOS 27 and (later) macOS 27:

  python3 tools/mkmenuboot.py [--board guacamole|hotdogg] STOCK_boot.img SHIM.Image --ios-kernel work/xnu-12377/BUILD/obj-ios26-sweet/... [--macos-kernel work/xnu-12377/BUILD/obj-macos27-sweet/...] IOS27_boot.img [MACOS27_boot.img] OUT.img

2026-11-20: replaces tools/mktripleboot.py after the partition split (iOS 16 removed). IOS27_boot.img /
MACOS27_boot.img are ordinary boot images as tools/guac-mkboot.sh makes them (iOS 27: ufsroot=ios27;
macOS 27: ufsroot=macos). Their ramdisks are the SWEETXNU payloads (kernel + ADT + boot-args); they go
into one ramdisk, iOS 27 first, each starting on a 4 KiB boundary (shim/loader.c payload_at). The menu
(shim/shim.c) shows iOS 27 / macOS 27 / Android; macOS 27 is dimmed when its payload is absent. ABL
leaves about 50 MiB for the ramdisk above the DTB, less the shim's own size (3.5 MB with the menu
pictures): the image is refused past 46 MiB. Writes nothing to
the phone.

2026-11-25 --board (default guacamole): the shim Image and every payload's ADT must be for that phone. A hotdogg
ADT has /sweet-board board = 'hotdogg' (mkadt.py --hotdogg); a guacamole ADT has no /sweet-board. A shim built
with BOARD=X says "this boot image was built for X" (shim.c board_check); older guacamole shims lack the string
and are accepted with a warning. The stock boot image's DTB is the generic SM8150 set on both phones (the board
identity is in dtbo), so it is not checked: the shim checks the phone's real tree at boot instead.
"""
import argparse, os, pathlib, struct, subprocess, sys, tempfile

HDR = "<8s10I16s512s32s1024sIQIIQ"
RAMDISK_ROOM = 46 * 1024 * 1024  # ~50 MiB, less the menu pictures in the shim (3.3 MB, shim/art.S)


def ramdisk_of(path):
    b = open(path, "rb").read()
    if b[:8] != b"ANDROID!":
        sys.exit(f"{path}: not an Android boot image")
    f = struct.unpack_from(HDR, b, 0)
    ksz, rsz, page = f[1], f[3], f[8]
    off = page * (1 + (ksz + page - 1) // page)
    rd = b[off:off + rsz]
    if rd[:8] != b"SWEETXNU":
        sys.exit(f"{path}: its ramdisk is not a SWEETXNU payload")
    cmd = rd[0x48:0x48 + 608].split(b"\0")[0].decode(errors="replace")
    return rd, cmd


def verify_kernel(payload, kernel_path, os_name, build_dir_name):
    """Require a payload kernel to match its OS-specific build directory in work/xnu-12377."""
    repo = pathlib.Path(__file__).resolve().parent.parent
    kernel_path = pathlib.Path(kernel_path).resolve()
    build_dir = (repo / "work" / "xnu-12377" / "BUILD" / build_dir_name).resolve()
    try:
        kernel_path.relative_to(build_dir)
    except ValueError:
        sys.exit(f"{os_name} kernel must be built under {build_dir}, got {kernel_path}")
    if not kernel_path.is_file():
        sys.exit(f"{os_name} kernel not found: {kernel_path}")

    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
    import mkpayload

    kernel = bytearray(kernel_path.read_bytes())
    macho = mkpayload.parse_macho(kernel)
    mkpayload.apply_chained_fixups(kernel, macho)
    kernel_segs = [s for s in macho["segs"] if s["vmsize"]]
    magic, version, nsegs = struct.unpack_from("<8sII", payload, 0)
    if magic != b"SWEETXNU" or version != 1 or nsegs < len(kernel_segs):
        sys.exit(f"{os_name} payload has an invalid SWEETXNU header or kernel segment table")
    for i, seg in enumerate(kernel_segs):
        vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<4Q", payload, 0x2a8 + 32 * i)
        if (vmaddr, vmsize) != (seg["vmaddr"], seg["vmsize"]):
            sys.exit(f"{os_name} payload kernel segment {i} does not match {kernel_path}")
        embedded = payload[fileoff:fileoff + filesize]
        expected = kernel[seg["fileoff"]:seg["fileoff"] + seg["filesize"]]
        if embedded != expected:
            sys.exit(f"{os_name} payload kernel segment {i} differs from {kernel_path}")
    print(f"{os_name} kernel provenance: matched {len(kernel_segs)} segments from {kernel_path}")


BOARDS = ("guacamole", "hotdogg")


def payload_board(payload, name):
    """The board a SWEETXNU payload's ADT was built for (mkadt.py): 'hotdogg' etc., or 'guacamole' without /sweet-board."""
    adt_off, adt_size = struct.unpack_from("<QQ", payload, 0x20)
    adt = payload[adt_off:adt_off + adt_size]
    if not adt_size or len(adt) != adt_size:
        sys.exit(f"{name} payload: ADT outside the payload")
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
    import adt as adtmod
    root = adtmod.parse(adt)[0]
    for child in root.children:
        props = {k: v for k, _, v in child.props}
        if props.get("name", b"").rstrip(b"\0") == b"sweet-board":
            return props.get("board", b"?").rstrip(b"\0").decode(errors="replace")
    return "guacamole"


def check_shim_board(path, board):
    img = open(path, "rb").read()
    tags = [b for b in BOARDS if b"this boot image was built for " + b.encode() + b"\0" in img]
    if not tags:
        if board != "guacamole":
            sys.exit(f"{path}: not a BOARD={board} shim (build it with BOARD={board} shim/build.sh)")
        print(f"warning: {path} has no board tag (a shim from before 2026-11-25); taken as guacamole", file=sys.stderr)
    elif tags != [board]:
        sys.exit(f"{path} was built for {', '.join(tags)}, not {board}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--board", choices=BOARDS, default="guacamole",
        help="the phone this image is for (default guacamole); shim and payloads must match")
    ap.add_argument("stock")
    ap.add_argument("shim")
    ap.add_argument("--ios-kernel", required=True,
        help="iOS kernel from work/xnu-12377/BUILD/obj-ios26-sweet")
    ap.add_argument("--macos-kernel",
        help="macOS kernel from work/xnu-12377/BUILD/obj-macos27-sweet (required with a macOS payload)")
    ap.add_argument("images", nargs="+")
    a = ap.parse_args()
    if len(a.images) not in (2, 3):
        ap.error("provide iOS boot image, optional macOS boot image, and output path")
    if len(a.images) == 3 and not a.macos_kernel:
        ap.error("--macos-kernel from work/xnu-12377/BUILD/obj-macos27-sweet is required with a macOS payload")
    if len(a.images) == 2 and a.macos_kernel:
        ap.error("--macos-kernel was supplied without a macOS payload")
    stock, shim = a.stock, a.shim
    check_shim_board(shim, a.board)
    out = a.images[-1]
    imgs = a.images[:-1]
    names = ["iOS 27", "macOS 27"][:len(imgs)]
    wants = ["ufsroot=ios27 ", "ufsroot=macos "]
    pays = []
    for index, (name, path, want) in enumerate(zip(names, imgs, wants)):
        p, c = ramdisk_of(path)
        if index == 0:
            verify_kernel(p, a.ios_kernel, "iOS", "obj-ios26-sweet")
        elif index == 1:
            verify_kernel(p, a.macos_kernel, "macOS", "obj-macos27-sweet")
        pb = payload_board(p, name)
        if pb != a.board:
            sys.exit(f"{name} payload's ADT is for {pb}, not {a.board} (mkadt.py{' --' + a.board if a.board != 'guacamole' else ''})")
        if want not in c + " ":
            print(f"warning: {name} payload boot-args have no '{want.strip()}': {c[:120]}...", file=sys.stderr)
        pays.append((name, p, c))
    blob = b""
    for _, p, _ in pays:
        blob += b"\0" * (-len(blob) % 4096) + p
    if len(blob) > RAMDISK_ROOM:
        sys.exit(f"payloads are {len(blob)} B, more than the {RAMDISK_ROOM} B ABL leaves for the ramdisk")
    with tempfile.NamedTemporaryFile(delete=False, suffix=".bin") as t:
        t.write(blob)
        tmp = t.name
    try:
        here = os.path.dirname(os.path.abspath(__file__))
        subprocess.check_call([sys.executable, os.path.join(here, "mkguacboot.py"), stock, shim, out, "--ramdisk", tmp])
    finally:
        os.unlink(tmp)
    print(f"board: {a.board}")
    print(f"payloads: " + ", ".join(f"{n} {len(p)} B" for n, p, _ in pays) + f"; ramdisk {len(blob)} B of {RAMDISK_ROOM}; image {os.path.getsize(out)} B")
    for n, _, c in pays:
        print(f"{n} args:", c)


if __name__ == "__main__":
    main()
