#!/usr/bin/env python3
"""Build the bootable AUXSTRESS diagnostic disk.

    python scripts/build_auxstress_disk.py

Assembles software/auxstress.a65 with ACME, then uses AppleCommander to
drop the binary onto a copy of the DOS 3.3 System Master and rewrite HELLO
so the disk auto-BRUNs the meter on boot. Produces software/AUXSTRESS.dsk.

Copy the .dsk to the SD card and mount it in Disk II drive 1 from the boot
menu; the meter starts on boot and any key exits back to DOS.

Tool locations can be overridden with env vars ACME_EXE, ACME (lib dir),
APPLECOMMANDER (jar), and DOS33_MASTER (base .dsk).
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SW = REPO / "software"
SRC = SW / "auxstress.a65"
BIN = SW / "auxstress.bin"
BMSRC = SW / "bankmap.a65"
BMBIN = SW / "bankmap.bin"
BMNAME = "BANKMAP"
# ProDOS 2.4.3 boots into Bitsy Bye: arrows + Return to pick a tool.
PRODOS_PO = SW / "ProDOS_2_4_3.po"
PODISK = SW / "AUXTOOLS.po"
DISK = SW / "AUXSTRESS.dsk"
LOAD = "0x6000"
NAME = "AUXSTRESS"

ACME_EXE = os.environ.get("ACME_EXE", r"C:\Users\hasse\tools\acme\acme.exe")
ACME_LIB = os.environ.get("ACME", r"C:\Users\hasse\tools\acme\ACME_Lib")
AC_JAR = Path(os.environ.get(
    "APPLECOMMANDER", r"C:\Users\hasse\tools\AppleCommander-ac-13.0.jar"))
MASTER = Path(os.environ.get(
    "DOS33_MASTER", str(SW / "DOS 3.3 System Master.dsk")))

HELLO = f'10  PRINT  CHR$(4)"BRUN {NAME}"\n'


def run(cmd, *, stdin_bytes=None, check=True):
    print("+", " ".join(str(c) for c in cmd))
    return subprocess.run(cmd, input=stdin_bytes, check=check)


def acme(src=None, out=None):
    src = src or SRC
    out = out or BIN
    env = dict(os.environ, ACME=ACME_LIB)
    print("+", ACME_EXE, "-f plain -o", out.name, src.name)
    subprocess.run([ACME_EXE, "-f", "plain", "-o", str(out), str(src)],
                   check=True, cwd=str(SW), env=env)


def ac(args, *, stdin_bytes=None, check=True):
    return run(["java", "-jar", str(AC_JAR), *args],
               stdin_bytes=stdin_bytes, check=check)


def main() -> int:
    if not SRC.exists():
        print(f"missing source: {SRC}", file=sys.stderr)
        return 1
    if not MASTER.exists():
        print(f"missing DOS 3.3 master: {MASTER}", file=sys.stderr)
        return 1
    acme()
    acme(BMSRC, BMBIN)
    shutil.copyfile(MASTER, DISK)
    # Replace HELLO so boot auto-BRUNs the meter.
    ac(["-d", str(DISK), "HELLO"], check=False)
    ac(["-bas", str(DISK), "HELLO"], stdin_bytes=HELLO.encode("ascii"))
    # Drop the binary.
    ac(["-d", str(DISK), NAME], check=False)
    ac(["-p", str(DISK), NAME, "B", LOAD], stdin_bytes=BIN.read_bytes())
    # BANKMAP: manual BRUN, not auto-run.
    ac(["-d", str(DISK), BMNAME], check=False)
    ac(["-p", str(DISK), BMNAME, "B", LOAD], stdin_bytes=BMBIN.read_bytes())
    print(f"OK: {DISK}")
    # ProDOS variant: Bitsy Bye menu picks the tool (no auto-run).
    if PRODOS_PO.exists():
        shutil.copyfile(PRODOS_PO, PODISK)
        ac(["-p", str(PODISK), NAME, "BIN", LOAD],
           stdin_bytes=BIN.read_bytes())
        ac(["-p", str(PODISK), BMNAME, "BIN", LOAD],
           stdin_bytes=BMBIN.read_bytes())
        print(f"OK: {PODISK}")
    else:
        print(f"skip ProDOS disk (missing {PRODOS_PO})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
