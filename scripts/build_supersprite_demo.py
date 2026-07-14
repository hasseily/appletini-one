#!/usr/bin/env python3
"""Build a bootable DOS 3.3 disk for the SuperSprite showcase demo.

    python scripts/build_supersprite_demo.py

Assembles software/supersprite_demo.a65 with ACME, then uses AppleCommander to
drop the binary onto a copy of the DOS 3.3 System Master and rewrite HELLO so
the disk auto-BRUNs the demo on boot. Produces software/SSDEMO.dsk.

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
SRC = SW / "supersprite_demo.a65"
BIN = SW / "supersprite_demo.bin"
DISK = SW / "SSDEMO.dsk"
LOAD = "0x6000"
NAME = "SSDEMO"

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


def acme(exe):
    env = dict(os.environ, ACME=ACME_LIB)
    print("+", exe, "-f plain -o", BIN.name, SRC.name)
    subprocess.run([exe, "-f", "plain", "-o", str(BIN), str(SRC)],
                   check=True, cwd=str(SW), env=env)


def ac(args, *, stdin_bytes=None, check=True):
    return run(["java", "-jar", str(AC_JAR), *args],
               stdin_bytes=stdin_bytes, check=check)


def main() -> int:
    if not SRC.exists():
        print(f"missing source: {SRC}", file=sys.stderr)
        return 1
    if not MASTER.exists():
        print(f"missing DOS 3.3 master image: {MASTER}\n"
              f"  set DOS33_MASTER=<path to a DOS 3.3 System Master .dsk>",
              file=sys.stderr)
        return 1
    if not AC_JAR.exists():
        print(f"missing AppleCommander jar: {AC_JAR}", file=sys.stderr)
        return 1

    # 1. assemble
    exe = "acme" if shutil.which("acme") else ACME_EXE
    acme(exe)

    # 2. fresh disk from the master (boot tracks come along)
    shutil.copy(MASTER, DISK)

    # 3. clear any prior copies (ignore "not found")
    ac(["-d", str(DISK), NAME], check=False)
    ac(["-d", str(DISK), "HELLO"], check=False)

    # 4. add the demo binary (type B, load $6000)
    ac(["-p", str(DISK), NAME, "B", LOAD], stdin_bytes=BIN.read_bytes())

    # 5. auto-run greeting -> BRUN the demo
    ac(["-bas", str(DISK), "HELLO"], stdin_bytes=HELLO.encode("ascii"))

    print(f"\nBuilt {DISK}")
    print("Boot it on the Appletini with the SuperSprite enabled "
          "(config SmartPort tab or UART `ss on`).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
