"""Virtual SSC printer path checks.

Layers, mirroring scripts/test_mouse_card.py:
  1. Source/text regressions across HDL, PS C, and build registration.
  2. ROM staleness: the committed .mem images must match a fresh build.
  3. py65 firmware execution: the real SSC firmware prints through a
     mocked 6551, including the LF-after-CR DIP behavior.
  4. Host-compiled ImageWriter interpreter unit test.
  5. xsim behavioral run of hdl/sim/tb_ssc_card.sv.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "ssc_card"

sys.path.insert(0, str(Path(__file__).resolve().parent))

import build_ssc_rom  # noqa: E402


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def read_text(path):
    return (ROOT / path).read_text(encoding="utf-8")


def test_sources():
    hdl_sources = read_text("hdl/hdl_sources.txt")
    card = read_text("hdl/apple/ssc_card.sv")
    uthernet = read_text("hdl/apple/uthernet2_card.sv")
    top = read_text("hdl/apple/apple_top.sv")
    regs = read_text("ps_sources/frontend/card_control_regs.h")
    vitis = read_text("scripts/create_vitis_workspace.py")
    frontend = read_text("ps_sources/frontend/main.c")
    config = read_text("ps_sources/frontend/config_menu.c")
    printing = read_text("ps_sources/frontend/config_menu_printing.c")
    service = read_text("ps_sources/frontend/printer_service.c")

    require("apple/ssc_card.sv" in hdl_sources and
            "apple/ssc_slot1_c100.mem" in hdl_sources and
            "apple/ssc_c800.mem" in hdl_sources,
            "SSC card sources must be in hdl_sources.txt")
    require('$readmemh("ssc_slot1_c100.mem", slot_rom);' in card and
            '$readmemh("ssc_c800.mem", c8_rom);' in card,
            "SSC card must load the built ROM images")
    require("(ab_read.addr[3:2] == 2'b01)" in uthernet,
            "Uthernet II must leave the SSC half of the DEVSEL page free")
    require("DIPSW1_VALUE = 8'hE2" in card and
            "DIPSW2_VALUE = 8'h20" in card,
            "SSC DIP switches must select 9600/PPC printer mode with LF-gen")
    require("ACIA_STATUS_VALUE = 8'h10" in card,
            "SSC ACIA status must always report transmit-ready")
    require("assert_irq = 1'b0" in card,
            "the printer SSC never raises IRQ")
    require("(ab_read.addr[10:0] != 11'h7FF)" in card,
            "SSC must never serve the $CFFF release address")
    require("sss.io_select[slot_assign]" in card and
            "!sss.sw_intcxrom" in card,
            "SSC $C800 window must respect ownership and INTCXROM")

    require("ssc_card ssc_card_i" in top and
            ".slot_assign(3'h1)" in top and
            "gate_ab(ab_read, card_ssc_enable)" in top,
            "apple_top must place the SSC in slot 1 behind its feature bit")
    require("apple_bus_write_arbiter #(" in top and
            ".NUM_CLIENTS(13)" in top and
            ".FAST_DATA_CLIENT(2)" in top and
            "ssc_ab_write," in top,
            "the bus arbiter must include the SSC writer")
    require("CARD_CTRL_FEATURE_SSC_ENABLE_BIT = 2" in top,
            "SSC enable must be feature bit 2 (slot 1 stays the Uthernet mask bit)")
    require("CARD_CTRL_REG_SSC_STATUS" in top and
            "CARD_CTRL_REG_SSC_HEAD" in top and
            "CARD_CTRL_REG_SSC_CTRL" in top,
            "apple_top must expose the SSC FIFO drain registers")

    require("#define CARD_CTRL_SSC_STATUS_REG" in regs and
            "#define CARD_CTRL_SSC_HEAD_REG" in regs and
            "#define CARD_CTRL_SSC_CTRL_REG" in regs and
            "CARD_CTRL_FEATURE_SSC_ENABLE_BIT" in regs,
            "PS register header must expose the SSC window")

    for source in ("imagewriter.c", "printer_service.c",
                   "config_menu_printing.c"):
        require(f'"../../../ps_sources/frontend/{source}"' in vitis,
                f"{source} must be registered in the Vitis build")

    require("printer_service_init();" in frontend and
            "printer_service_poll();" in frontend and
            "printer_service_set_checkpoint(usb0_priority_checkpoint);" in frontend,
            "main.c must init, poll, and checkpoint the printer service")
    require("printing.ssc.enabled" in config and
            "CONFIG_BROWSER_TARGET_PRINTOUT" in config and
            '"Printing",' in config,
            "config menu must persist and surface the Printing tab")
    require("config_menu_printing_start_delete" in config and
            "config_menu_printing_start_rename" in config,
            "printout browser must route delete/rename actions")
    require("f_unlink" in printing and "f_rename" in printing,
            "printout actions must delete and rename through FatFs")
    require("PRINTER_SERVICE_DIR" in service and
            "lodepng_encode_memory" in service and
            "LCT_RGB" in service and
            'screenshot_service_show_confirmation("PAGE PRINTED")' in service,
            "printer service must save color PNGs and show a page notice")
    print("PASS test_sources")


def test_rom_is_current():
    slot_data, cont_data, _symbols, _text = build_ssc_rom.build()

    committed_slot = [
        int(line, 16)
        for line in read_text("hdl/apple/ssc_slot1_c100.mem").split()
        if not line.startswith("//")
    ]
    committed_cont = [
        int(line, 16)
        for line in read_text("hdl/apple/ssc_c800.mem").split()
        if not line.startswith("//")
    ]
    require(bytes(slot_data) == bytes(committed_slot),
            "ssc_slot1_c100.mem is stale -- rerun scripts/build_ssc_rom.py")
    require(bytes(cont_data) == bytes(committed_cont),
            "ssc_c800.mem is stale -- rerun scripts/build_ssc_rom.py")
    print("PASS test_rom_is_current")


class FakeAcia:
    """DEVSEL-side model of ssc_card.sv for firmware execution."""

    DIPSW1 = 0xE2
    DIPSW2 = 0x20

    def __init__(self):
        self.captured = []
        self.cmd = 0x00
        self.ctl = 0x00

    def read(self, address):
        idx = address & 0x0F
        if idx == 0x1:
            return self.DIPSW1
        if idx == 0x2:
            return self.DIPSW2
        if idx & 0x8:
            reg = idx & 0x3
            if reg == 0:
                return 0x00          # RDR: never any input
            if reg == 1:
                return 0x10          # TDR empty, DCD/DSR good
            if reg == 2:
                return self.cmd
            return self.ctl
        return 0x00

    def write(self, address, value):
        idx = address & 0x0F
        if idx & 0x8:
            reg = idx & 0x3
            if reg == 0:
                self.captured.append(value)
            elif reg == 1:
                self.cmd &= 0xE0     # programmed reset
            elif reg == 2:
                self.cmd = value
            else:
                self.ctl = value


def test_firmware_prints():
    from py65.devices.mpu6502 import MPU
    from py65.memory import ObservableMemory

    slot_data, cont_data, _symbols, _text = build_ssc_rom.build()

    mem = ObservableMemory(addrWidth=16)
    acia = FakeAcia()
    mem.subscribe_to_read(range(0xC090, 0xC0A0), acia.read)
    mem.subscribe_to_write(range(0xC090, 0xC0A0),
                           lambda addr, value: acia.write(addr, value))
    # ROM space is read-only: BENTER/BLEAVE/ROMSOFF writes must not land.
    mem.subscribe_to_write(range(0xC100, 0xD000), lambda addr, value: None)

    mpu = MPU(memory=mem)
    mem[0xC100:0xC200] = list(slot_data)
    mem[0xC800:0xD000] = list(cont_data)
    mem[0xFDF6] = 0x60      # VIDOUT stub
    mem[0xFF58] = 0x60      # IORTS ($60 sets V through BIT)

    mem[0x36] = 0x00        # CSWL/H -> $C100 (PR#1 state)
    mem[0x37] = 0xC1
    mem[0x38] = 0x1B        # KSWL/H -> monitor keyboard
    mem[0x39] = 0xFD
    mem[0x24] = 0x00        # CH

    def call_c100(a_value):
        mpu.a = a_value
        mpu.x = 0x00
        mpu.y = 0x00
        # JSR frame returning to $0300.
        mem[0x01FF] = 0x02
        mem[0x01FE] = 0xFF
        mpu.sp = 0xFD
        mpu.pc = 0xC100
        for _ in range(2_000_000):
            if mpu.pc == 0x0300:
                return
            mpu.step()
        raise AssertionError("SSC firmware did not return")

    call_c100(0xC1)             # 'A' with the BASIC high bit
    require(acia.captured == [0xC1],
            f"firmware sent {acia.captured}, expected ['A']")
    call_c100(0x8D)             # CR: the DIP block enables LF generation
    require(acia.captured == [0xC1, 0x8D, 0x8A],
            f"firmware sent {acia.captured}, expected CR then generated LF")
    require(mem[0xC100] == slot_data[0] and mem[0xCFF9] == cont_data[0x7F9],
            "ROM bytes changed: a ROM-space write leaked through")
    require((acia.cmd & 0x1F) != 0, "INITACIA never programmed the command reg")
    print("PASS test_firmware_prints")


def run(cmd, log_path):
    completed = subprocess.run(
        cmd, cwd=OUT_DIR, capture_output=True, text=True)
    log_path.write_text(
        (completed.stdout or "") + (completed.stderr or ""),
        encoding="utf-8")
    if completed.returncode != 0:
        name = cmd if isinstance(cmd, str) else cmd[0]
        raise RuntimeError(
            f"{name} failed (exit {completed.returncode}); see {log_path}")
    return completed.stdout or ""


def find_msvc_install():
    import os

    vswhere = (Path(os.environ.get("ProgramFiles(x86)",
                                   r"C:\Program Files (x86)")) /
               "Microsoft Visual Studio" / "Installer" / "vswhere.exe")
    if not vswhere.exists():
        return None
    completed = subprocess.run(
        [str(vswhere), "-latest", "-products", "*",
         "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
         "-property", "installationPath"],
        capture_output=True, text=True)
    lines = (completed.stdout or "").strip().splitlines()
    return lines[0] if lines else None


def test_imagewriter_host():
    sources = [
        str(ROOT / "scripts" / "test_imagewriter_host.c"),
        str(ROOT / "ps_sources" / "frontend" / "imagewriter.c"),
        str(ROOT / "ps_sources" / "lib" / "fb16.c"),
    ]
    include_dirs = [
        str(ROOT / "ps_sources" / "frontend"),
        str(ROOT / "ps_sources" / "lib"),
    ]
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    exe = OUT_DIR / "test_imagewriter_host.exe"

    gcc = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")
    if gcc is not None:
        cmd = [gcc, "-O1", "-Wall", "-Wextra", "-Werror"]
        for inc in include_dirs:
            cmd += ["-I", inc]
        cmd += sources + ["-o", str(exe)]
        run(cmd, OUT_DIR / "host_cc.log")
    else:
        install = find_msvc_install()
        if install is None:
            raise AssertionError(
                "no host C compiler (gcc/clang/MSVC) for interpreter test")
        vcvars = Path(install) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
        # The shared harness prefix neuters GCC-isms (__attribute__) so the
        # production sources compile under MSVC unchanged.
        prefix = ROOT / "scripts" / "host_render_harness" / "harness_prefix.h"
        inc_flags = " ".join(f'/I "{inc}"' for inc in include_dirs)
        src_list = " ".join(f'"{src}"' for src in sources)
        shell = (f'"{vcvars}" && cl /nologo /W3 /O1 /std:c11 '
                 f'/D_CRT_SECURE_NO_WARNINGS /FI "{prefix}" '
                 f'{inc_flags} {src_list} '
                 f'/Fo"{OUT_DIR}\\\\" /Fe"{exe}"')
        run(f'cmd /s /c "{shell}"', OUT_DIR / "host_cc.log")

    stdout = run([str(exe)], OUT_DIR / "host_run.log")
    require("IMAGEWRITER HOST PASS" in stdout,
            "host interpreter test did not pass")
    print("PASS test_imagewriter_host")


def vivado_tool(name):
    bat = shutil.which(f"{name}.bat")
    if bat:
        return bat
    tool = shutil.which(name)
    if tool:
        return tool
    raise FileNotFoundError(f"unable to locate Vivado tool {name}")


def simulate():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for mem in ("ssc_slot1_c100.mem", "ssc_c800.mem"):
        shutil.copyfile(ROOT / "hdl" / "apple" / mem, OUT_DIR / mem)
    run([vivado_tool("xvlog"), "--sv",
         str(ROOT / "hdl" / "globals.sv"),
         str(ROOT / "hdl" / "apple" / "ssc_card.sv"),
         str(ROOT / "hdl" / "sim" / "tb_ssc_card.sv")],
        OUT_DIR / "xvlog.log")
    run([vivado_tool("xelab"), "tb_ssc_card", "-s", "tb_ssc_card_snap",
         "--timescale", "1ns/1ps"], OUT_DIR / "xelab.log")
    stdout = run([vivado_tool("xsim"), "tb_ssc_card_snap", "--runall"],
                 OUT_DIR / "xsim.log")
    if "SSC CARD PASS" not in stdout:
        raise AssertionError("tb_ssc_card did not pass")
    print("PASS simulate")


def main():
    test_sources()
    test_rom_is_current()
    test_firmware_prints()
    test_imagewriter_host()
    try:
        vivado_tool("xvlog")
    except FileNotFoundError:
        print("SKIP simulation: Vivado tools not on PATH", file=sys.stderr)
        return 1
    simulate()
    print("SSC card tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
