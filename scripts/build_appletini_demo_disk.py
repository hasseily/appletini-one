#!/usr/bin/env python3
"""Build the bootable 800 KB Appletini ProDOS demo disk.

Besides the classic demos, the disk carries the "New Image Modes"
slideshow: one A2IMGVIEW binary that walks HGR, DHGR, and SHR images in
per-format ProDOS folders (/HGR, /DHGR, /SHR). This script generates
the viewer's image table from the IMAGE_FILES manifest below, so the
launcher entry, the viewer, and the folders always agree.

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
Legacy images (originals in tmp/legacy_img) are raw page dumps; the
interlaced ones carry the baked A2Li signal, nothing else was
altered.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
SOFTWARE = REPO / "software"
WEB_DIR = SOFTWARE / "appletini_webserver"
WEB_APP = WEB_DIR / "build" / "A2WEBSRV.SYSTEM"
BROWSER_APP = WEB_DIR / "build" / "A2BROWSE.SYSTEM"
IMG_APP = WEB_DIR / "build" / "A2IMG.SYSTEM"
PRODOS_MASTER = SOFTWARE / "ProDOS_2_4_3.po"
SSDEMO_DISK = SOFTWARE / "SSDEMO.dsk"
BORDER_SRC = SOFTWARE / "border_demo.a65"
BORDER_APP = SOFTWARE / "border_demo.bin"
LAUNCHER_SRC = SOFTWARE / "launcher.a65"
LAUNCHER_APP = SOFTWARE / "launcher.bin"
SPEEDRACE_SRC = SOFTWARE / "speedrace.a65"
SPEEDRACE_APP = SOFTWARE / "speedrace.bin"
RASTER_SRC = SOFTWARE / "rasterdemo.a65"
RASTER_APP = SOFTWARE / "rasterdemo.bin"
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
VOLUME = "APPLETINI.DEMOS"

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
120 GOTO 10
200 PRINT CHR$(4)"BLOAD SSDEMO"
210 HOME : PRINT "ENABLE SUPERSPRITE IN CONFIG MENU"
220 PRINT "THEN PRESS ANY KEY" : GET A$
230 CALL 24576
240 HOME : PRINT "DISABLE SUPERSPRITE TO RESTORE DISK" : GET A$
250 RETURN
"""


FMT_SHR, FMT_HGR, FMT_HGRI, FMT_DHGR, FMT_DHGRI = range(5)

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
    ("SHR", "FLUIDART.INT", "shr", "fluidart.shr4i", 65536, FMT_SHR, 1),
    ("SHR", "DRAGONS.INT", "shr", "dragons.shr4i", 65536, FMT_SHR, 1),
    ("SHR", "GOTHOUSES.INT", "shr", "gothouses.shr4i", 65536, FMT_SHR, 1),
    ("SHR", "GRID.TEST", "shr", "gridtest.shr4", 32768, FMT_SHR, 0),
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


def copy_prodos_file(name: str, aux: str) -> None:
    data = ac("-g", str(PRODOS_MASTER), name, capture=True)
    ac("-p", str(TEMP_OUTPUT), name, "SYS", aux, stdin=data)


def copy_boot_blocks() -> None:
    boot = PRODOS_MASTER.read_bytes()[:1024]
    with TEMP_OUTPUT.open("r+b") as image:
        image.write(boot)


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
            (VIEWER_DEMO_SRC, VIEWER_DEMO_APP)):
        subprocess.run([exe, "-f", "plain", "-o", str(output), str(source)],
                       cwd=SOFTWARE, env=env, check=True)
    viewer = VIEWER_DEMO_APP.read_bytes()
    if len(viewer) > 2048:
        raise RuntimeError(
            f"A2IMGVIEW demo is {len(viewer)} bytes; the relocation stub only "
            "copies $2000-$27FF (2048 bytes)")


def main() -> int:
    required = [AC_JAR, PRODOS_MASTER, SSDEMO_DISK, BORDER_SRC,
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
        ac("-pro800", str(TEMP_OUTPUT), VOLUME)
        copy_boot_blocks()

        # PRODOS boots the first .SYSTEM file, so BASIC.SYSTEM precedes the
        # web server and runs the STARTUP launcher.
        copy_prodos_file("PRODOS", "0x0000")
        copy_prodos_file("BASIC.SYSTEM", "0x0000")
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

        # New Image Modes: the viewer SYS (dash-launched by type, so the
        # name needs no .SYSTEM suffix and boot order stays
        # STARTUP-first) plus the per-format image folders.
        ac("-p", str(TEMP_OUTPUT), "A2IMGVIEW", "SYS", "0x2000",
           stdin=VIEWER_DEMO_APP.read_bytes())
        for folder, disk_name, src_dir, src, _, _, _ in IMAGE_FILES:
            ac("-p", str(TEMP_OUTPUT), f"{folder}/{disk_name}", "BIN",
               "0x2000", stdin=source_path(src_dir, src).read_bytes())

        if TEMP_OUTPUT.stat().st_size != 800 * 1024:
            raise RuntimeError("AppleCommander did not create an 800 KB image")
        if (TEMP_OUTPUT.read_bytes()[:1024] !=
                PRODOS_MASTER.read_bytes()[:1024]):
            raise RuntimeError("ProDOS boot-block copy failed")
        catalog = ac("-ll", str(TEMP_OUTPUT), capture=True).decode(
            "utf-8", errors="replace")
        for name in ("PRODOS", "BASIC.SYSTEM", "STARTUP", "MANDELBROT",
                     "WAVE.ANIMATION", "WAVE.CODE",
                     "A2WEBSRV.SYSTEM", "A2BROWSE.SYSTEM", "A2IMG.SYSTEM", "SSDEMO",
                     "BORDERDEMO", "LAUNCHER", "SPEEDRACE",
                     "RASTERDEMO", "A2IMGVIEW",
                     *(disk_name for _, disk_name, _, _, _, _, _
                       in IMAGE_FILES)):
            if name not in catalog:
                raise RuntimeError(f"missing {name} from output catalog")

        os.replace(TEMP_OUTPUT, OUTPUT)
    except BaseException:
        TEMP_OUTPUT.unlink(missing_ok=True)
        raise

    print(f"\nBuilt {OUTPUT} ({OUTPUT.stat().st_size} bytes)\n")
    print(catalog.replace(str(TEMP_OUTPUT), str(OUTPUT)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
