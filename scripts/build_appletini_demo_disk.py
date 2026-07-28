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
IMPOSSIBLE_SRC = SOFTWARE / "impossible.a65"
IMPOSSIBLE_APP = SOFTWARE / "impossible.bin"
LAUNCHER_ASSETS = SOFTWARE / "launcher_assets.a65"
GEN_ASSETS = REPO / "scripts" / "gen_hgr_assets.py"
MANDEL_CORE_SRC = SOFTWARE / "mandel_core.a65"
MANDELBROT_SRC = SOFTWARE / "mandelbrot.bas"
WAVE_BASIC_SRC = SOFTWARE / "wave_animation.bas"
WAVE_CODE_SRC = SOFTWARE / "wave_animation_code.a65"
WAVE_CODE_APP = SOFTWARE / "wave_animation_code.bin"
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
40 IF S = 1 THEN PRINT CHR$(4)"BRUN IMPOSSIBLE"
50 IF S = 2 THEN PRINT CHR$(4)"BRUN SPEEDRACE"
60 IF S = 3 THEN PRINT CHR$(4)"BRUN RASTERDEMO"
70 IF S = 4 THEN PRINT CHR$(4)"RUN MANDELBROT"
80 IF S = 5 THEN PRINT CHR$(4)"RUN WAVE.ANIMATION"
90 IF S = 6 THEN GOSUB 200
100 IF S = 7 THEN PRINT CHR$(4)"-A2BROWSE.SYSTEM"
105 IF S = 9 THEN PRINT CHR$(4)"-A2IMG.SYSTEM"
110 IF S = 8 THEN PRINT CHR$(4)"-A2WEBSRV.SYSTEM"
120 GOTO 10
200 PRINT CHR$(4)"BLOAD SSDEMO"
210 HOME : PRINT "ENABLE SUPERSPRITE IN CONFIG MENU"
220 PRINT "THEN PRESS ANY KEY" : GET A$
230 CALL 24576
240 HOME : PRINT "DISABLE SUPERSPRITE TO RESTORE DISK" : GET A$
250 RETURN
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


def build_assembly_programs() -> None:
    subprocess.run([sys.executable, str(GEN_ASSETS)], check=True)
    exe = shutil.which("acme") or ACME_EXE
    env = dict(os.environ, ACME=ACME_LIB)
    for source, output in (
            (BORDER_SRC, BORDER_APP),
            (WAVE_CODE_SRC, WAVE_CODE_APP),
            (LAUNCHER_SRC, LAUNCHER_APP),
            (SPEEDRACE_SRC, SPEEDRACE_APP),
            (RASTER_SRC, RASTER_APP),
            (IMPOSSIBLE_SRC, IMPOSSIBLE_APP)):
        subprocess.run([exe, "-f", "plain", "-o", str(output), str(source)],
                       cwd=SOFTWARE, env=env, check=True)


def main() -> int:
    required = (AC_JAR, PRODOS_MASTER, SSDEMO_DISK, BORDER_SRC,
                MANDELBROT_SRC, WAVE_BASIC_SRC, WAVE_CODE_SRC,
                LAUNCHER_SRC, SPEEDRACE_SRC, RASTER_SRC, GEN_ASSETS,
                IMPOSSIBLE_SRC, MANDEL_CORE_SRC,
                WEB_DIR / "build.bat")
    missing = [str(path) for path in required if not path.is_file()]
    if shutil.which("java") is None:
        missing.append("java")
    if missing:
        print("Missing required input/tool:\n  " + "\n  ".join(missing),
              file=sys.stderr)
        return 1

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
        ac("-p", str(TEMP_OUTPUT), "IMPOSSIBLE", "BIN", "0x6000",
           stdin=IMPOSSIBLE_APP.read_bytes())

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
                     "RASTERDEMO", "IMPOSSIBLE"):
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
