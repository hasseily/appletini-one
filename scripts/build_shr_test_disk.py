#!/usr/bin/env python3
"""Build the bootable SHR extended-modes test disk (SHR_Test.po).

The disk boots straight into SHRVIEW.SYSTEM, a slideshow that loads
each test image into the right banks (first 32K to aux $2000-$9FFF,
any remaining payload to main $2000+) and turns on SHR via $C029. Any
key advances; ESC quits. Images live in software/shr_testimages/ and
cover SHR-3200, SHR4 RGGB in 320 and 640 mode, PAL256, interlaced
double SHR, and paired-field samples retained for future frame merging. See
scripts/render_shr4.py for the host-side reference decode of the same
files.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
SOFTWARE = REPO / "software"
IMAGES = SOFTWARE / "shr_testimages"
PRODOS_MASTER = SOFTWARE / "ProDOS_2_4_3.po"
VIEWER_SRC = SOFTWARE / "a2imgview.a65"
VIEWER_APP = SOFTWARE / "a2imgview.bin"
OUTPUT = SOFTWARE / "SHR_Test.po"
TEMP_OUTPUT = SOFTWARE / "SHR_Test.tmp.po"
VOLUME = "SHR.TEST"

AC_JAR = Path(os.environ.get(
    "APPLECOMMANDER", r"C:\Users\hasse\tools\AppleCommander-ac-13.0.jar"))
ACME_EXE = os.environ.get("ACME_EXE", r"C:\Users\hasse\tools\acme\acme.exe")
ACME_LIB = os.environ.get("ACME", r"C:\Users\hasse\tools\acme\ACME_Lib")

# (disk name, source file, expected size, expected paged mode) -- names
# must match the viewer's embedded table in a2imgview.a65.
IMAGE_FILES = (
    ("BEACH.3200", "beach.3200", 39168, 0),
    ("EYE320.A", "eye320a.shr4", 32768, 0),
    ("EYE320.B", "eye320b.shr4", 32768, 0),
    ("EYE320.C", "eye320c.shr4", 32768, 0),
    ("EYE640.A", "eye640a.shr4", 32768, 0),
    ("EYE640.B", "eye640b.shr4", 32768, 0),
    ("BANNERS.INT1", "banners1.shr4i", 65536, 1),
    ("BANNERS.INT2", "banners2.shr4i", 65536, 1),
    ("TEST.256", "test256.shr4p", 65536, 2),
    ("TEST.FLIP", "testflip.shrp", 65536, 2),
)

MAGICS = (b"\xd3\xc8\xd2\xb4", b"\xb3\xb2\xb0\xb0")   # 'SHR4', '3200'


def check_conformance(src: str, data: bytes, paged: int) -> None:
    """Images must be self-describing per the SDD ruleset: a magic at
    $9DFC and the paged mode in ctrl byte 0 at $9DF8, in every bank."""
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


def copy_prodos_file(name: str) -> None:
    data = ac("-g", str(PRODOS_MASTER), name, capture=True)
    ac("-p", str(TEMP_OUTPUT), name, "SYS", "0x0000", stdin=data)


def copy_boot_blocks() -> None:
    boot = PRODOS_MASTER.read_bytes()[:1024]
    with TEMP_OUTPUT.open("r+b") as image:
        image.write(boot)


def main() -> int:
    required = [AC_JAR, PRODOS_MASTER, VIEWER_SRC]
    required += [IMAGES / src for _, src, _, _ in IMAGE_FILES]
    missing = [str(path) for path in required if not path.is_file()]
    if shutil.which("java") is None:
        missing.append("java")
    if missing:
        print("Missing required input/tool:\n  " + "\n  ".join(missing),
              file=sys.stderr)
        return 1

    for _, src, size, paged in IMAGE_FILES:
        data = (IMAGES / src).read_bytes()
        if len(data) != size:
            raise RuntimeError(f"{src} is {len(data)} bytes, expected {size}")
        check_conformance(src, data, paged)

    exe = shutil.which("acme") or ACME_EXE
    env = dict(os.environ, ACME=ACME_LIB)
    subprocess.run([exe, "-f", "plain", "-o", str(VIEWER_APP),
                    str(VIEWER_SRC)], cwd=SOFTWARE, env=env, check=True)
    viewer = VIEWER_APP.read_bytes()
    if len(viewer) > 2048:
        raise RuntimeError(
            f"SHRVIEW is {len(viewer)} bytes; the relocation stub only "
            "copies $2000-$27FF (2048 bytes)")

    TEMP_OUTPUT.unlink(missing_ok=True)
    try:
        ac("-pro800", str(TEMP_OUTPUT), VOLUME)
        copy_boot_blocks()

        # ProDOS boots the first .SYSTEM file: the viewer itself.
        copy_prodos_file("PRODOS")
        ac("-p", str(TEMP_OUTPUT), "SHRVIEW.SYSTEM", "SYS", "0x2000",
           stdin=viewer)
        copy_prodos_file("BASIC.SYSTEM")

        for disk_name, src, _, _ in IMAGE_FILES:
            ac("-p", str(TEMP_OUTPUT), disk_name, "BIN", "0x2000",
               stdin=(IMAGES / src).read_bytes())

        if TEMP_OUTPUT.stat().st_size != 800 * 1024:
            raise RuntimeError("AppleCommander did not create an 800 KB image")
        if (TEMP_OUTPUT.read_bytes()[:1024] !=
                PRODOS_MASTER.read_bytes()[:1024]):
            raise RuntimeError("ProDOS boot-block copy failed")
        catalog = ac("-ll", str(TEMP_OUTPUT), capture=True).decode(
            "utf-8", errors="replace")
        for name in ["PRODOS", "SHRVIEW.SYSTEM", "BASIC.SYSTEM"] + \
                [disk_name for disk_name, _, _, _ in IMAGE_FILES]:
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
