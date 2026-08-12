#!/usr/bin/env python3
"""Build the bootable 32 MB Appletini ProDOS demo disk.

Besides the classic demos, the disk carries the "New Image Modes"
slideshow: one A2IMGVIEW binary that walks HGR, DHGR, and SHR images in
per-format ProDOS folders (/HGR, /DHGR, /SHR). This script generates
the viewer's image table from the IMAGE_FILES manifest below, so the
launcher entry, the viewer, and the folders always agree.

The disk also carries Reboot Camp '83. Its 4 MB FAT12 disk boots
MS-DOS 2.0 on the virtual AD8088 Plus and AUTOEXEC runs HGRCUBE.COM,
which draws an animated cube through the 8088's Apple-memory window.
The tracked AD8088 MSDOS.hdv supplies the known-good bridge, DOS files,
and FAT image; this script only replaces AUTOEXEC.BAT for demo use.

Image formats: 0=SHR (self-describing SDD images, conformance-checked),
1=HGR 8K, 2=HGRi 16K (both pages, woven by the firmware), 3=DHR 16K
(aux 8K then main 8K), 4=DHRi 32K (two .dhr pairs). Two-page legacy
images are self-describing per SDD docs/LEGACY_PAGED_VIDEO_MODES.md:
the A2Li signature + mode byte sit in page 2's first screen hole
(main bank), baked into the file's hole bytes and conformance-checked
here.

Provenance of the SHR demo images (originals in tmp/shr4, untracked):
- beach.3200: byte-identical to the validated shr_testimages copy.
- *.shr4i: legacy SDD files carried the interlace flag at $9DFB; the
  runtime reads ctrl byte 0 at $9DF8, so the flag was moved there.
- gridtest.shr4: rebuilt from a bare pixel dump (SCBs, QuickDraw II
  default 320 palette, ctrl, SHR4 magic added).
- eye320.shr4: copy of shr_testimages/eye320a.shr4 (320-mode RGGB).
- observatory.pal256 and skycity.pal256i: original Appletini showcase art,
  packed with scripts/build_pal256_image.py for the 320x100 PAL256 layout.
Legacy images (originals in tmp/legacy_img) are raw page dumps; the
interlaced ones carry the baked A2Li signal, nothing else was
altered.
"""

from __future__ import annotations

import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
SOFTWARE = REPO / "software"
WEB_DIR = SOFTWARE / "appletini_webserver"
WEB_APP = WEB_DIR / "build" / "A2WEBSRV.SYSTEM"
BROWSER_APP = WEB_DIR / "build" / "A2BROWSE.SYSTEM"
IMG_APP = WEB_DIR / "build" / "A2IMG.SYSTEM"
AD8088_BASE = SOFTWARE / "AD8088 MSDOS.hdv"
AD8088_RETURN_SRC = SOFTWARE / "ad8088_demo_return.a65"
AD8088_RETURN_APP = SOFTWARE / "ad8088_demo_return.bin"
SSDEMO_DISK = SOFTWARE / "SSDEMO.dsk"
BORDER_SRC = SOFTWARE / "border_demo.a65"
BORDER_APP = SOFTWARE / "border_demo.bin"
LAUNCHER_SRC = SOFTWARE / "launcher.a65"
LAUNCHER_APP = SOFTWARE / "launcher.bin"
SPEEDRACE_SRC = SOFTWARE / "speedrace.a65"
SPEEDRACE_APP = SOFTWARE / "speedrace.bin"
RASTER_SRC = SOFTWARE / "rasterdemo.a65"
RASTER_APP = SOFTWARE / "rasterdemo.bin"
TEXT_OVERLAY_SRC = SOFTWARE / "textoverlay.a65"
TEXT_OVERLAY_APP = SOFTWARE / "textoverlay.bin"
LAUNCHER_ASSETS = SOFTWARE / "launcher_assets.a65"
GEN_ASSETS = REPO / "scripts" / "gen_hgr_assets.py"
MANDELBROT_SRC = SOFTWARE / "mandelbrot.bas"
WAVE_BASIC_SRC = SOFTWARE / "wave_animation.bas"
WAVE_CODE_SRC = SOFTWARE / "wave_animation_code.a65"
WAVE_CODE_APP = SOFTWARE / "wave_animation_code.bin"
IMAGES_SHR = SOFTWARE / "shr4_demo_images"
IMAGES_LEGACY = SOFTWARE / "legacy_demo_images"
VIEWER_SRC = SOFTWARE / "a2imgview.a65"
VIEWER_DEMO_SRC = SOFTWARE / "a2imgview_demo.a65"
VIEWER_DEMO_APP = SOFTWARE / "a2imgview_demo.bin"
OUTPUT = SOFTWARE / "Appletini_Demos.po"
TEMP_OUTPUT = SOFTWARE / "Appletini_Demos.tmp.po"

AC_JAR = Path(os.environ.get(
    "APPLECOMMANDER", r"C:\Users\hasse\tools\AppleCommander-ac-13.0.jar"))
ACME_EXE = os.environ.get("ACME_EXE", r"C:\Users\hasse\tools\acme\acme.exe")
ACME_LIB = os.environ.get("ACME", r"C:\Users\hasse\tools\acme\ACME_Lib")

STARTUP = """10 PRINT CHR$(4)"BRUN LAUNCHER"
20 S = PEEK(768)
30 IF S = 0 THEN HOME : END
40 IF S = 1 THEN PRINT CHR$(4)"-A2IMGVIEW"
50 IF S = 2 THEN PRINT CHR$(4)"BRUN SPEEDRACE"
60 IF S = 3 THEN PRINT CHR$(4)"BRUN RASTERDEMO"
70 IF S = 4 THEN PRINT CHR$(4)"RUN MANDELBROT"
80 IF S = 5 THEN PRINT CHR$(4)"RUN WAVE.ANIMATION"
90 IF S = 6 THEN GOSUB 200
100 IF S = 7 THEN PRINT CHR$(4)"-A2BROWSE.SYSTEM"
110 IF S = 8 THEN PRINT CHR$(4)"-A2WEBSRV.SYSTEM"
115 IF S = 9 THEN PRINT CHR$(4)"-A2IMG.SYSTEM"
117 IF S = 10 THEN PRINT CHR$(4)"BRUN MSDOS.BRIDGE,A2048"
118 IF S = 11 THEN PRINT CHR$(4)"BRUN TEXTOVERLAY"
120 GOTO 10
200 PRINT CHR$(4)"BLOAD SSDEMO"
210 HOME : PRINT "ENABLE SUPERSPRITE IN CONFIG MENU"
220 PRINT "THEN PRESS ANY KEY" : GET A$
230 CALL 24576
240 HOME : PRINT "DISABLE SUPERSPRITE TO RESTORE DISK" : GET A$
250 RETURN
"""

AD8088_AUTOEXEC = (
    b"ECHO OFF\r\n"
    b"PATH C:\\DOS;C:\\PRODOS\r\n"
    b"CLS\r\n"
    b"ECHO APPLETINI ONE AD8088 PLUS DEMO\r\n"
    b"ECHO MS-DOS 2.0 IS RUNNING ON THE 8088\r\n"
    b"ECHO MOVE THE JOYSTICK OR LET IT ROTATE\r\n"
    b"ECHO PRESS ANY KEY TO RETURN TO PRODOS\r\n"
    b"HGRCUBE\r\n"
    b"QUIT\r\n"
)

AD8088_BRIDGE_LOAD = 0x0800
AD8088_RETURN_ADDR = 0x1E00
AD8088_BRIDGE_QUIT = bytes.fromhex("20 00 BF 65 1E 03")
AD8088_BRIDGE_RETURN = bytes.fromhex("4C 00 1E EA EA EA")


FMT_SHR, FMT_HGR, FMT_HGRI, FMT_DHGR, FMT_DHGRI = range(5)
VIDEO7_NONE = 0
VIDEO7_MIX = 2
VIDEO7_MIX_SOURCES = frozenset({"face.dhri"})

# (folder, disk name, source dir, source file, expected size, format,
#  expected SHR paged mode or None)
#
# EYE.320 deliberately runs LAST: RGGB demosaic is the heaviest
# decode; last position keeps a first-decode hiccup off the rest of
# the deck.
IMAGE_FILES = (
    ("HGR", "EYE.I", "legacy", "eye.hgri", 16384, FMT_HGRI, None),
    ("HGR", "FINDER.I", "legacy", "finder.hgri", 16384, FMT_HGRI, None),
    ("DHGR", "FACE.I", "legacy", "face.dhri", 32768, FMT_DHGRI, None),
    ("SHR", "BEACH.3200", "shr", "beach.3200", 39168, FMT_SHR, 0),
    ("SHR", "PORSCHE.INT", "shr", "cartest.shr4i", 65536, FMT_SHR, 1),
    ("SHR", "SKYCITY.INT", "shr", "skycity.pal256i", 65536, FMT_SHR, 1),
    ("SHR", "DRAGONS.INT", "shr", "dragons.shr4i", 65536, FMT_SHR, 1),
    ("SHR", "GOTHOUSES.INT", "shr", "gothouses.shr4i", 65536, FMT_SHR, 1),
    ("SHR", "OBSERVATORY", "shr", "observatory.pal256", 32768, FMT_SHR, 0),
    ("SHR", "EYE.320", "shr", "eye320.shr4", 32768, FMT_SHR, 0),
)

MAGICS = (b"\xd3\xc8\xd2\xb4", b"\xb3\xb2\xb0\xb0")   # 'SHR4', '3200'
A2LI_SIG = b"\xc1\xb2\xcc\xe9"                        # hi-ASCII 'A2Li'


def source_path(src_dir: str, src: str) -> Path:
    return (IMAGES_SHR if src_dir == "shr" else IMAGES_LEGACY) / src


def check_conformance(src: str, data: bytes, paged: int) -> None:
    """SHR images must be self-describing per the SDD ruleset: a magic
    at $9DFC and the paged mode in ctrl byte 0 at $9DF8, in every bank."""
    banks = (0,) if len(data) < 0x10000 else (0, 0x8000)
    for base in banks:
        magic = data[base + 0x7DFC:base + 0x7E00]
        if magic not in MAGICS:
            raise RuntimeError(
                f"{src}: bank at {base:#x} has no SHR4/3200 magic at "
                f"$9DFC (found {magic.hex()})")
        got = data[base + 0x7DF8]
        if got != paged:
            raise RuntimeError(
                f"{src}: bank at {base:#x} paged mode is {got}, "
                f"expected {paged}")


def check_legacy_paged(src: str, data: bytes, fmt: int) -> None:
    """Two-page legacy images are self-describing: the A2Li signature
    plus mode byte ride in page 2's first screen hole, main bank
    (SDD docs/LEGACY_PAGED_VIDEO_MODES.md), so staging page 2 arms the
    renderer with no loader side channel."""
    off = 0x2078 if fmt == FMT_HGRI else 0x6078   # page-2 main +$78
    if data[off:off + 4] != A2LI_SIG:
        raise RuntimeError(
            f"{src}: no A2Li signature at file offset {off:#x}")
    if data[off + 4] not in (1, 2):
        raise RuntimeError(
            f"{src}: A2Li mode byte is {data[off + 4]}, expected 1 or 2")


def generate_demo_viewer() -> None:
    """Swap the image table in a2imgview.a65 for the demo set. Everything
    above names_lo: (loader, dispatch, keys, MLI glue) is reused as-is."""
    source = VIEWER_SRC.read_text(encoding="utf-8")
    head, tail = source.split("names_lo:", 1)
    if not re.search(r"\n\}\s*$", tail):
        raise RuntimeError("a2imgview.a65 layout changed; update this script")

    labels = [f"n_{i}" for i in range(len(IMAGE_FILES))]
    table = ["names_lo:"]
    table.append("    !byte " + ", ".join(f"<{label}" for label in labels))
    table.append("names_hi:")
    table.append("    !byte " + ", ".join(f">{label}" for label in labels))
    table.append("formats:")
    table.append("    !byte " + ", ".join(
        str(fmt) for _, _, _, _, _, fmt, _ in IMAGE_FILES))
    table.append("video7_modes:")
    table.append("    !byte " + ", ".join(
        str(VIDEO7_MIX if src in VIDEO7_MIX_SOURCES else VIDEO7_NONE)
        for _, _, _, src, _, _, _ in IMAGE_FILES))
    table.append(f"image_count = {len(IMAGE_FILES)}")
    table.append("")
    for label, (folder, disk_name, _, _, _, _, _) in zip(labels, IMAGE_FILES):
        path = f"{folder}/{disk_name}"
        table.append(f'{label}: !byte {len(path)} : !text "{path}"')
    table.append("")
    table.append("}")
    VIEWER_DEMO_SRC.write_text(head + "\n".join(table) + "\n",
                               encoding="utf-8")


def ac(*args: str, stdin: bytes | None = None,
       capture: bool = False) -> bytes:
    command = ["java", "-jar", str(AC_JAR), *args]
    print("+", " ".join(command))
    result = subprocess.run(
        command,
        input=stdin,
        stdout=subprocess.PIPE if capture else None,
        check=True,
    )
    return result.stdout if capture else b""


def _fat12_root_entry(image: bytes | bytearray, dos_name: bytes) -> tuple[int, int, int, int]:
    """Return (entry offset, first cluster, cluster bytes, FAT offset)."""
    if len(dos_name) != 11:
        raise ValueError("FAT root names must be exactly 11 bytes")
    if len(image) < 512:
        raise RuntimeError("FAT image is too small")

    bytes_per_sector = struct.unpack_from("<H", image, 11)[0]
    sectors_per_cluster = image[13]
    reserved_sectors = struct.unpack_from("<H", image, 14)[0]
    fat_count = image[16]
    root_entries = struct.unpack_from("<H", image, 17)[0]
    sectors_per_fat = struct.unpack_from("<H", image, 22)[0]
    if (bytes_per_sector == 0 or sectors_per_cluster == 0 or
            fat_count == 0 or sectors_per_fat == 0):
        raise RuntimeError("invalid FAT12 BIOS parameter block")

    fat_offset = reserved_sectors * bytes_per_sector
    root_offset = (reserved_sectors + fat_count * sectors_per_fat) * bytes_per_sector
    for entry_offset in range(root_offset,
                              root_offset + root_entries * 32, 32):
        first = image[entry_offset]
        if first == 0x00:
            break
        if first == 0xE5 or image[entry_offset + 11] == 0x0F:
            continue
        if image[entry_offset:entry_offset + 11] == dos_name:
            first_cluster = struct.unpack_from("<H", image, entry_offset + 26)[0]
            return (entry_offset, first_cluster,
                    bytes_per_sector * sectors_per_cluster, fat_offset)
    raise RuntimeError(f"{dos_name.decode('ascii')} is missing from the DOS disk")


def _fat12_chain(image: bytes | bytearray, first_cluster: int,
                 fat_offset: int) -> list[int]:
    if first_cluster < 2:
        raise RuntimeError("FAT file has no data cluster")
    chain = []
    seen = set()
    cluster = first_cluster
    while cluster < 0xFF8:
        if cluster < 2 or cluster == 0xFF7 or cluster in seen:
            raise RuntimeError("invalid FAT12 cluster chain")
        seen.add(cluster)
        chain.append(cluster)
        off = fat_offset + cluster + cluster // 2
        word = image[off] | (image[off + 1] << 8)
        cluster = ((word >> 4) if cluster & 1 else word) & 0x0FFF
    return chain


def read_fat12_root_file(image: bytes, dos_name: bytes) -> bytes:
    entry, first_cluster, cluster_bytes, fat_offset = _fat12_root_entry(
        image, dos_name)
    size = struct.unpack_from("<I", image, entry + 28)[0]
    root_entries = struct.unpack_from("<H", image, 17)[0]
    bytes_per_sector = struct.unpack_from("<H", image, 11)[0]
    sectors_per_cluster = image[13]
    reserved_sectors = struct.unpack_from("<H", image, 14)[0]
    fat_count = image[16]
    sectors_per_fat = struct.unpack_from("<H", image, 22)[0]
    root_sectors = (root_entries * 32 + bytes_per_sector - 1) // bytes_per_sector
    data_offset = (reserved_sectors + fat_count * sectors_per_fat +
                   root_sectors) * bytes_per_sector
    data = bytearray()
    for cluster in _fat12_chain(image, first_cluster, fat_offset):
        off = data_offset + (cluster - 2) * cluster_bytes
        data.extend(image[off:off + cluster_bytes])
    if size > len(data):
        raise RuntimeError("FAT file size exceeds its cluster chain")
    return bytes(data[:size])


def replace_fat12_root_file(image: bytes, dos_name: bytes,
                            content: bytes) -> bytes:
    """Replace a root file without changing its allocated FAT12 chain."""
    data = bytearray(image)
    entry, first_cluster, cluster_bytes, fat_offset = _fat12_root_entry(
        data, dos_name)
    chain = _fat12_chain(data, first_cluster, fat_offset)
    capacity = len(chain) * cluster_bytes
    if len(content) > capacity:
        raise RuntimeError(
            f"{dos_name.decode('ascii')} needs {len(content)} bytes, "
            f"but its FAT chain holds {capacity}")

    root_entries = struct.unpack_from("<H", data, 17)[0]
    bytes_per_sector = struct.unpack_from("<H", data, 11)[0]
    sectors_per_cluster = data[13]
    reserved_sectors = struct.unpack_from("<H", data, 14)[0]
    fat_count = data[16]
    sectors_per_fat = struct.unpack_from("<H", data, 22)[0]
    root_sectors = (root_entries * 32 + bytes_per_sector - 1) // bytes_per_sector
    data_offset = (reserved_sectors + fat_count * sectors_per_fat +
                   root_sectors) * bytes_per_sector
    padded = content + bytes(capacity - len(content))
    for index, cluster in enumerate(chain):
        off = data_offset + (cluster - 2) * cluster_bytes
        start = index * cluster_bytes
        data[off:off + cluster_bytes] = padded[start:start + cluster_bytes]
    struct.pack_into("<I", data, entry + 28, len(content))
    return bytes(data)


def add_ad8088_demo_return(bridge: bytes, return_stub: bytes) -> bytes:
    """Redirect the bridge's sole ProDOS QUIT call to the demo return stub."""
    if bridge.count(AD8088_BRIDGE_QUIT) != 1:
        raise RuntimeError("AD8088 bridge QUIT sequence changed")
    stub_offset = AD8088_RETURN_ADDR - AD8088_BRIDGE_LOAD
    if len(bridge) > stub_offset:
        raise RuntimeError(
            f"AD8088 bridge grew into the return stub at ${AD8088_RETURN_ADDR:04X}")
    if AD8088_RETURN_ADDR + len(return_stub) > 0x2000:
        raise RuntimeError("AD8088 demo return stub exceeds $1FFF")
    patched = bridge.replace(AD8088_BRIDGE_QUIT,
                             AD8088_BRIDGE_RETURN, 1)
    return patched + bytes(stub_offset - len(patched)) + return_stub


def build_assembly_programs() -> None:
    subprocess.run([sys.executable, str(GEN_ASSETS)], check=True)
    generate_demo_viewer()
    exe = shutil.which("acme") or ACME_EXE
    env = dict(os.environ, ACME=ACME_LIB)
    for source, output in (
            (BORDER_SRC, BORDER_APP),
            (WAVE_CODE_SRC, WAVE_CODE_APP),
            (LAUNCHER_SRC, LAUNCHER_APP),
            (SPEEDRACE_SRC, SPEEDRACE_APP),
            (RASTER_SRC, RASTER_APP),
            (TEXT_OVERLAY_SRC, TEXT_OVERLAY_APP),
            (VIEWER_DEMO_SRC, VIEWER_DEMO_APP),
            (AD8088_RETURN_SRC, AD8088_RETURN_APP)):
        subprocess.run([exe, "-f", "plain", "-o", str(output), str(source)],
                       cwd=SOFTWARE, env=env, check=True)
    viewer = VIEWER_DEMO_APP.read_bytes()
    if len(viewer) > 2048:
        raise RuntimeError(
            f"A2IMGVIEW demo is {len(viewer)} bytes; the relocation stub only "
            "copies $2000-$27FF (2048 bytes)")


def main() -> int:
    required = [AC_JAR, AD8088_BASE, AD8088_RETURN_SRC,
                SSDEMO_DISK, BORDER_SRC,
                TEXT_OVERLAY_SRC,
                MANDELBROT_SRC, WAVE_BASIC_SRC, WAVE_CODE_SRC,
                LAUNCHER_SRC, SPEEDRACE_SRC, RASTER_SRC, GEN_ASSETS,
                VIEWER_SRC, WEB_DIR / "build.bat"]
    required += [source_path(d, s) for _, _, d, s, _, _, _ in IMAGE_FILES]
    missing = [str(path) for path in required if not path.is_file()]
    if shutil.which("java") is None:
        missing.append("java")
    if missing:
        print("Missing required input/tool:\n  " + "\n  ".join(missing),
              file=sys.stderr)
        return 1

    for _, _, src_dir, src, size, fmt, paged in IMAGE_FILES:
        data = source_path(src_dir, src).read_bytes()
        if len(data) != size:
            raise RuntimeError(f"{src} is {len(data)} bytes, expected {size}")
        if fmt == FMT_SHR:
            check_conformance(src, data, paged)
        elif fmt in (FMT_HGRI, FMT_DHGRI):
            check_legacy_paged(src, data, fmt)

    build_assembly_programs()

    comspec = os.environ.get("COMSPEC", "cmd.exe")
    web_build = subprocess.run([comspec, "/d", "/c", "build.bat"],
                               cwd=WEB_DIR)
    missing_apps = [path for path in (WEB_APP, BROWSER_APP, IMG_APP)
                    if not path.is_file()]
    if web_build.returncode != 0:
        if missing_apps:
            print("Web demo build failed and no prebuilt apps exist:\n  " +
                  "\n  ".join(str(path) for path in missing_apps),
                  file=sys.stderr)
            return 1
        print("WARNING: web toolchain unavailable; using the prebuilt "
              "A2WEBSRV/A2BROWSE apps", file=sys.stderr)
    elif missing_apps:
        print("Web demo build did not create:\n  " +
              "\n  ".join(str(path) for path in missing_apps),
              file=sys.stderr)
        return 1

    TEMP_OUTPUT.unlink(missing_ok=True)
    try:
        # Reboot Camp supplies a complete 32 MB ProDOS disk. It already has
        # PRODOS, BASIC.SYSTEM, the AD8088 bridge, and the DOS disk image.
        # Keep its MSDOS volume name because the bridge uses absolute paths.
        shutil.copyfile(AD8088_BASE, TEMP_OUTPUT)
        ac("-d", str(TEMP_OUTPUT), "STARTUP")

        # The stock bridge exits through ProDOS QUIT and lands in Bitsy Bye.
        # The demo copy reloads BASIC.SYSTEM instead, which runs STARTUP and
        # brings the user back to this disk's launcher.
        bridge = ac("-g", str(AD8088_BASE), "MSDOS.BRIDGE", capture=True)
        bridge = add_ad8088_demo_return(
            bridge, AD8088_RETURN_APP.read_bytes())
        ac("-d", str(TEMP_OUTPUT), "MSDOS.BRIDGE")
        ac("-p", str(TEMP_OUTPUT), "MSDOS.BRIDGE", "BIN", "0x0800",
           stdin=bridge)

        # AUTOEXEC starts the HGR cube and QUIT returns to ProDOS when a key
        # ends the animation. HGRCUBE.COM itself stays byte-for-byte from the
        # tracked, hardware-tested Reboot Camp image.
        msdos_hdd = ac("-g", str(AD8088_BASE), "MSDOS.HDD", capture=True)
        if not read_fat12_root_file(msdos_hdd, b"HGRCUBE COM"):
            raise RuntimeError("Reboot Camp DOS disk has an empty HGRCUBE.COM")
        msdos_hdd = replace_fat12_root_file(
            msdos_hdd, b"AUTOEXECBAT", AD8088_AUTOEXEC)
        ac("-d", str(TEMP_OUTPUT), "MSDOS.HDD")
        ac("-p", str(TEMP_OUTPUT), "MSDOS.HDD", "BIN", "0x0000",
           stdin=msdos_hdd)

        # BASIC.SYSTEM remains the first .SYSTEM file and runs this STARTUP.
        ac("-bas", str(TEMP_OUTPUT), "STARTUP",
           stdin=STARTUP.encode("ascii"))
        ac("-bas", str(TEMP_OUTPUT), "MANDELBROT",
           stdin=MANDELBROT_SRC.read_bytes())
        ac("-bas", str(TEMP_OUTPUT), "WAVE.ANIMATION",
           stdin=WAVE_BASIC_SRC.read_bytes())
        ac("-p", str(TEMP_OUTPUT), "WAVE.CODE", "BIN", "0x0300",
           stdin=WAVE_CODE_APP.read_bytes())

        ac("-as", str(TEMP_OUTPUT), "A2WEBSRV.SYSTEM",
           stdin=WEB_APP.read_bytes())
        ac("-as", str(TEMP_OUTPUT), "A2BROWSE.SYSTEM",
           stdin=BROWSER_APP.read_bytes())
        ac("-as", str(TEMP_OUTPUT), "A2IMG.SYSTEM",
           stdin=IMG_APP.read_bytes())
        ssdemo = ac("-g", str(SSDEMO_DISK), "SSDEMO", capture=True)
        ac("-p", str(TEMP_OUTPUT), "SSDEMO", "BIN", "0x6000",
           stdin=ssdemo)
        ac("-p", str(TEMP_OUTPUT), "BORDERDEMO", "BIN", "0x6000",
           stdin=BORDER_APP.read_bytes())
        ac("-p", str(TEMP_OUTPUT), "LAUNCHER", "BIN", "0x4000",
           stdin=LAUNCHER_APP.read_bytes())
        ac("-p", str(TEMP_OUTPUT), "SPEEDRACE", "BIN", "0x6000",
           stdin=SPEEDRACE_APP.read_bytes())
        ac("-p", str(TEMP_OUTPUT), "RASTERDEMO", "BIN", "0x6000",
           stdin=RASTER_APP.read_bytes())
        ac("-p", str(TEMP_OUTPUT), "TEXTOVERLAY", "BIN", "0x2000",
           stdin=TEXT_OVERLAY_APP.read_bytes())

        # New Image Modes: the viewer SYS (dash-launched by type, so the
        # name needs no .SYSTEM suffix and boot order stays
        # STARTUP-first) plus the per-format image folders.
        ac("-p", str(TEMP_OUTPUT), "A2IMGVIEW", "SYS", "0x2000",
           stdin=VIEWER_DEMO_APP.read_bytes())
        for folder, disk_name, src_dir, src, _, _, _ in IMAGE_FILES:
            ac("-p", str(TEMP_OUTPUT), f"{folder}/{disk_name}", "BIN",
               "0x2000", stdin=source_path(src_dir, src).read_bytes())

        if TEMP_OUTPUT.stat().st_size != AD8088_BASE.stat().st_size:
            raise RuntimeError("AppleCommander changed the 32 MB image size")
        if (TEMP_OUTPUT.read_bytes()[:1024] !=
                AD8088_BASE.read_bytes()[:1024]):
            raise RuntimeError("Reboot Camp ProDOS boot blocks changed")
        catalog = ac("-ll", str(TEMP_OUTPUT), capture=True).decode(
            "utf-8", errors="replace")
        for name in ("PRODOS", "BASIC.SYSTEM", "STARTUP", "MANDELBROT",
                     "WAVE.ANIMATION", "WAVE.CODE",
                     "A2WEBSRV.SYSTEM", "A2BROWSE.SYSTEM", "A2IMG.SYSTEM", "SSDEMO",
                     "BORDERDEMO", "LAUNCHER", "SPEEDRACE",
                     "RASTERDEMO", "TEXTOVERLAY", "A2IMGVIEW", "MSDOS.BRIDGE",
                     "MSDOS.HDD", "IO.SYS", "MSDOS.SYS",
                     *(disk_name for _, disk_name, _, _, _, _, _
                       in IMAGE_FILES)):
            if name not in catalog:
                raise RuntimeError(f"missing {name} from output catalog")

        built_hdd = ac("-g", str(TEMP_OUTPUT), "MSDOS.HDD", capture=True)
        if read_fat12_root_file(built_hdd, b"AUTOEXECBAT") != AD8088_AUTOEXEC:
            raise RuntimeError("AD8088 demo AUTOEXEC verification failed")
        if not read_fat12_root_file(built_hdd, b"HGRCUBE COM"):
            raise RuntimeError("AD8088 demo is missing HGRCUBE.COM")

        os.replace(TEMP_OUTPUT, OUTPUT)
    except BaseException:
        TEMP_OUTPUT.unlink(missing_ok=True)
        raise

    print(f"\nBuilt {OUTPUT} ({OUTPUT.stat().st_size} bytes)\n")
    print(catalog.replace(str(TEMP_OUTPUT), str(OUTPUT)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
