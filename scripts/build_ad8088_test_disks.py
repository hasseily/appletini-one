#!/usr/bin/env python3
"""Build DOS 3.3 and ProDOS validation disks for the virtual AD8088."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
SOFTWARE = REPO / "software"
SOURCE = SOFTWARE / "ad8088_test.a65"
BINARY = SOFTWARE / "ad8088_test.bin"
DOS_MASTER = Path(os.environ.get(
    "DOS33_MASTER", str(SOFTWARE / "DOS 3.3 System Master.dsk")))
PRODOS_MASTER = SOFTWARE / "ProDOS_2_4_3.po"
DOS_OUTPUT = SOFTWARE / "AD8088_Test.dsk"
PRODOS_OUTPUT = SOFTWARE / "AD8088_Test.po"
PRODOS_TEMP = SOFTWARE / "AD8088_Test.tmp.po"
ACME_EXE = os.environ.get("ACME_EXE", r"C:\Users\hasse\tools\acme\acme.exe")
AC_JAR = Path(os.environ.get(
    "APPLECOMMANDER", r"C:\Users\hasse\tools\AppleCommander-ac-13.0.jar"))
PROGRAM = "AD8088.TEST"
LOAD = "0x6000"
HELLO = f'10 PRINT CHR$(4)"BRUN {PROGRAM}"\n'
STARTUP = HELLO


def run(command: list[str], *, data: bytes | None = None,
        capture: bool = False, check: bool = True) -> bytes:
    print("+", " ".join(command))
    result = subprocess.run(command, input=data, check=check,
                            stdout=subprocess.PIPE if capture else None)
    return result.stdout if capture else b""


def ac(*args: str, data: bytes | None = None,
       capture: bool = False, check: bool = True) -> bytes:
    return run(["java", "-jar", str(AC_JAR), *args], data=data,
               capture=capture, check=check)


def build_program() -> None:
    run([ACME_EXE, "-f", "plain", "-o", str(BINARY), str(SOURCE)])
    if not BINARY.is_file() or BINARY.stat().st_size == 0:
        raise RuntimeError("ACME did not produce the AD8088 test binary")


def build_dos() -> None:
    shutil.copyfile(DOS_MASTER, DOS_OUTPUT)
    ac("-d", str(DOS_OUTPUT), "HELLO", check=False)
    ac("-bas", str(DOS_OUTPUT), "HELLO", data=HELLO.encode("ascii"))
    ac("-p", str(DOS_OUTPUT), PROGRAM, "B", LOAD, data=BINARY.read_bytes())
    catalog = ac("-l", str(DOS_OUTPUT), capture=True).decode(
        "utf-8", errors="replace")
    if "HELLO" not in catalog or PROGRAM not in catalog:
        raise RuntimeError("DOS 3.3 AD8088 test disk catalog is incomplete")


def copy_prodos_file(name: str) -> None:
    contents = ac("-g", str(PRODOS_MASTER), name, capture=True)
    ac("-p", str(PRODOS_TEMP), name, "SYS", "0x0000", data=contents)


def build_prodos() -> None:
    PRODOS_TEMP.unlink(missing_ok=True)
    try:
        ac("-pro800", str(PRODOS_TEMP), "AD8088.TEST")
        with PRODOS_TEMP.open("r+b") as image:
            image.write(PRODOS_MASTER.read_bytes()[:1024])
        copy_prodos_file("PRODOS")
        copy_prodos_file("BASIC.SYSTEM")
        ac("-bas", str(PRODOS_TEMP), "STARTUP", data=STARTUP.encode("ascii"))
        ac("-p", str(PRODOS_TEMP), PROGRAM, "BIN", LOAD,
           data=BINARY.read_bytes())
        if PRODOS_TEMP.stat().st_size != 800 * 1024:
            raise RuntimeError("AppleCommander did not create an 800 KB image")
        catalog = ac("-ll", str(PRODOS_TEMP), capture=True).decode(
            "utf-8", errors="replace")
        for name in ("PRODOS", "BASIC.SYSTEM", "STARTUP", PROGRAM):
            if name not in catalog:
                raise RuntimeError(f"ProDOS AD8088 disk is missing {name}")
        os.replace(PRODOS_TEMP, PRODOS_OUTPUT)
    except BaseException:
        PRODOS_TEMP.unlink(missing_ok=True)
        raise


def main() -> int:
    missing = [str(path) for path in
               (SOURCE, DOS_MASTER, PRODOS_MASTER, AC_JAR)
               if not path.is_file()]
    if not Path(ACME_EXE).is_file():
        missing.append(ACME_EXE)
    if shutil.which("java") is None:
        missing.append("java")
    if missing:
        print("Missing required input/tool:\n  " + "\n  ".join(missing),
              file=sys.stderr)
        return 1

    build_program()
    build_dos()
    build_prodos()
    print(f"Built {DOS_OUTPUT} ({DOS_OUTPUT.stat().st_size} bytes)")
    print(f"Built {PRODOS_OUTPUT} ({PRODOS_OUTPUT.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
