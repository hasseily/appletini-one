#!/usr/bin/env python3
"""Build the bootable 800 KB Appletini ProDOS demo disk."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
SOFTWARE = REPO / "software"
WEB_DIR = SOFTWARE / "appletini_webserver"
WEB_APP = WEB_DIR / "build" / "A2WEBSRV.SYSTEM"
BROWSER_APP = WEB_DIR / "build" / "A2BROWSE.SYSTEM"
PRODOS_MASTER = SOFTWARE / "ProDOS_2_4_3.po"
SSDEMO_DISK = SOFTWARE / "SSDEMO.dsk"
BORDER_SRC = SOFTWARE / "border_demo.a65"
BORDER_APP = SOFTWARE / "border_demo.bin"
OUTPUT = SOFTWARE / "Appletini_Demos.po"
TEMP_OUTPUT = SOFTWARE / "Appletini_Demos.tmp.po"
VOLUME = "APPLETINI.DEMOS"

AC_JAR = Path(os.environ.get(
    "APPLECOMMANDER", r"C:\Users\hasse\tools\AppleCommander-ac-13.0.jar"))
ACME_EXE = os.environ.get("ACME_EXE", r"C:\Users\hasse\tools\acme\acme.exe")
ACME_LIB = os.environ.get("ACME", r"C:\Users\hasse\tools\acme\ACME_Lib")

STARTUP = """10 HOME
20 PRINT "APPLETINI DEMO DISK"
30 PRINT
40 PRINT "1  WEB SERVER"
50 PRINT "2  SUPERSPRITE DEMO"
60 PRINT "3  WEB BROWSER"
70 PRINT "4  BORDER RASTER"
80 PRINT
90 PRINT "SELECT 1, 2, 3 OR 4: ";
100 GET A$: PRINT A$
110 IF A$="1" THEN PRINT CHR$(4)"-A2WEBSRV.SYSTEM"
120 IF A$="3" THEN PRINT CHR$(4)"-A2BROWSE.SYSTEM"
130 IF A$="4" THEN GOTO 240
140 IF A$<>"2" THEN GOTO 90
150 PRINT CHR$(4)"BLOAD SSDEMO"
160 PRINT "ENABLE SUPERSPRITE IN CONFIG MENU"
170 PRINT "THEN PRESS ANY KEY"
180 GET A$
190 CALL 24576
200 HOME
210 PRINT "DISABLE SUPERSPRITE TO RESTORE DISK"
220 GET A$
230 GOTO 10
240 PRINT CHR$(4)"BLOAD BORDERDEMO"
250 CALL 24576
260 GOTO 10
"""


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


def build_border_demo() -> None:
    exe = shutil.which("acme") or ACME_EXE
    env = dict(os.environ, ACME=ACME_LIB)
    subprocess.run([exe, "-f", "plain", "-o", str(BORDER_APP),
                    str(BORDER_SRC)], cwd=SOFTWARE, env=env, check=True)


def main() -> int:
    required = (AC_JAR, PRODOS_MASTER, SSDEMO_DISK, BORDER_SRC,
                WEB_DIR / "build.bat")
    missing = [str(path) for path in required if not path.is_file()]
    if shutil.which("java") is None:
        missing.append("java")
    if missing:
        print("Missing required input/tool:\n  " + "\n  ".join(missing),
              file=sys.stderr)
        return 1

    build_border_demo()

    comspec = os.environ.get("COMSPEC", "cmd.exe")
    subprocess.run([comspec, "/d", "/c", "build.bat"],
                   cwd=WEB_DIR, check=True)
    missing_apps = [path for path in (WEB_APP, BROWSER_APP)
                    if not path.is_file()]
    if missing_apps:
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

        ac("-as", str(TEMP_OUTPUT), "A2WEBSRV.SYSTEM",
           stdin=WEB_APP.read_bytes())
        ac("-as", str(TEMP_OUTPUT), "A2BROWSE.SYSTEM",
           stdin=BROWSER_APP.read_bytes())
        ssdemo = ac("-g", str(SSDEMO_DISK), "SSDEMO", capture=True)
        ac("-p", str(TEMP_OUTPUT), "SSDEMO", "BIN", "0x6000",
           stdin=ssdemo)
        ac("-p", str(TEMP_OUTPUT), "BORDERDEMO", "BIN", "0x6000",
           stdin=BORDER_APP.read_bytes())

        if TEMP_OUTPUT.stat().st_size != 800 * 1024:
            raise RuntimeError("AppleCommander did not create an 800 KB image")
        if (TEMP_OUTPUT.read_bytes()[:1024] !=
                PRODOS_MASTER.read_bytes()[:1024]):
            raise RuntimeError("ProDOS boot-block copy failed")
        catalog = ac("-ll", str(TEMP_OUTPUT), capture=True).decode(
            "utf-8", errors="replace")
        for name in ("PRODOS", "BASIC.SYSTEM", "STARTUP",
                     "A2WEBSRV.SYSTEM", "A2BROWSE.SYSTEM", "SSDEMO",
                     "BORDERDEMO"):
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
