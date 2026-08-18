#!/usr/bin/env python3
"""Static and assembly checks for the Appletini demo disk.

Verifies the launcher/demo sources, the STARTUP dispatcher, and (when
acme is on hand) that every new program assembles and fits its load
address. Does not require java/AppleCommander or the web toolchain.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOFTWARE = ROOT / "software"
ACME_EXE = os.environ.get("ACME_EXE", r"C:\Users\hasse\tools\acme\acme.exe")
ACME_LIB = os.environ.get("ACME", r"C:\Users\hasse\tools\acme\ACME_Lib")
AC_JAR = Path(os.environ.get(
    "APPLECOMMANDER", r"C:\Users\hasse\tools\AppleCommander-ac-13.0.jar"))


class TestFailure(AssertionError):
    pass


def require(cond: bool, message: str) -> None:
    if not cond:
        raise TestFailure(message)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def static_checks() -> None:
    build = read(ROOT / "scripts" / "build_appletini_demo_disk.py")
    launcher = read(SOFTWARE / "launcher.a65")
    speedrace = read(SOFTWARE / "speedrace.a65")
    raster = read(SOFTWARE / "rasterdemo.a65")
    text_overlay = read(SOFTWARE / "textoverlay.a65")

    # STARTUP dispatches every launcher selection and loops back. The
    # New Image Modes viewer owns slot 1; the Impossible demo is gone.
    for token in ('BRUN LAUNCHER', 'BRUN SPEEDRACE',
                  'BRUN RASTERDEMO', 'RUN MANDELBROT',
                  'RUN WAVE.ANIMATION', '-A2BROWSE.SYSTEM',
                  '-A2WEBSRV.SYSTEM',
                  'BRUN MSDOS.BRIDGE,A2048',
                  'BRUN TEXTOVERLAY',
                  '40 IF S = 1 THEN PRINT CHR$(4)"-A2IMGVIEW"',
                  'PEEK(768)', '120 GOTO 10'):
        require(token in build, f"STARTUP must contain {token!r}")
    require('IMPOSSIBLE' not in build,
            "the Impossible demo must be off the disk")

    # The disk build assembles and installs every program, including
    # the New Image Modes viewer and its per-format image folders.
    for token in ('LAUNCHER_SRC', 'SPEEDRACE_SRC', 'RASTER_SRC',
                  'TEXT_OVERLAY_SRC',
                  '"LAUNCHER", "BIN", "0x4000"',
                  '"SPEEDRACE", "BIN", "0x6000"',
                  '"RASTERDEMO", "BIN", "0x6000"',
                  '"TEXTOVERLAY", "BIN", "0x2000"',
                  '"A2IMGVIEW", "SYS", "0x2000"',
                  'generate_demo_viewer', 'IMAGE_FILES',
                  'check_conformance', 'check_legacy_paged',
                  'AD8088_BASE', 'replace_fat12_root_file',
                  'b"AUTOEXECBAT"', 'b"HGRCUBE COM"',
                  'add_ad8088_demo_return', 'AD8088_RETURN_SRC'):
        require(token in build, f"disk build must wire {token!r}")

    # The Reboot Camp base already has BASIC.SYSTEM first. Add the viewer
    # only after copying that image so it cannot take over the boot order.
    require(build.index('shutil.copyfile(AD8088_BASE') <
            build.index('ac("-p", str(TEMP_OUTPUT), "A2IMGVIEW"'),
            "A2IMGVIEW must be added after the bootable AD8088 base image")

    # The launcher menu puts the viewer first.
    require('item0: !text "1  New Image Modes"' in launcher and
            "Impossible" not in launcher,
            "launcher item 1 must be New Image Modes, Impossible gone")
    require("ITEMS      = 11" in launcher and
            'item9: !text "0  AD8088 MS-DOS HGR Cube"' in launcher and
            'item10: !text "T  Linear Text Overlay"' in launcher and
            "select_ad8088:" in launcher and
            "select_text_overlay:" in launcher,
            "launcher must expose the AD8088 and text-overlay demos")
    require("DESC_BAND  = 18" in launcher,
            "launcher help must leave a blank row after item 0")

    for token in ('* = $2000', 'signature: !text "LINTXT"',
                  'lda #CMD_ARM', 'lda #CMD_SHOW', 'lda #CMD_OFF',
                  'BUFFER       = $6000', 'jsr fill_buffer',
                  'draw_vt_screen:', 'draw_cp437_screen:', 'eor #$10',
                  'choose_canvas_layout:', 'jsr read_indexed',
                  'clear_overlay:', 'jsr wait_frame', 'wait_key:'):
        require(token in text_overlay,
                f"text overlay demo must contain {token!r}")

    wait_key = text_overlay.split("wait_key:", 1)[1].split("print_z:", 1)[0]
    require(wait_key.count("sta KBDSTRB") == 1 and
            "lda KBD" in wait_key and "bpl -" in wait_key and
            "lda KBDSTRB" not in wait_key and
            wait_key.index("lda KBD") < wait_key.index("sta KBDSTRB"),
            "text overlay keyboard loop must retain queued keys and clear "
            "the strobe after reading one")

    # The viewer must survive a BASIC.SYSTEM dash launch (empty MLI
    # prefix) and never spin forever on a black screen.
    viewer = read(SOFTWARE / "a2imgview.a65")
    for token in ("set_prefix", "GET_PREFIX", "ON_LINE", "SET_PREFIX",
                  "DEVNUM = $BF30", "count_fail", "cmp #image_count"):
        require(token in viewer,
                f"a2imgview must contain {token!r} (prefix + fail-out)")
    require("A2LI_HOLD  = $FF" in viewer and
            "sta $407C" in viewer and "sta $087C" in viewer and
            "$02E4" not in viewer,
            "a2imgview must use the A2Li holes for its load transaction")
    transaction = viewer.split("show_image:", 1)[1].split(
        "; ---- open ----", 1)[0]
    for sig_addr in ("$4078", "$4079", "$407A", "$407B",
                     "$0878", "$0879", "$087A", "$087B"):
        require(f"sta {sig_addr}" in transaction,
                f"first-load transaction must stamp {sig_addr}")
    require(transaction.index("sta $407B") <
            transaction.index("lda #A2LI_HOLD") <
            transaction.index("sta $407C") and
            transaction.index("sta $087B") <
            transaction.index("lda #A2LI_HOLD") <
            transaction.index("sta $087C"),
            "both A2Li signatures must be complete before either $FF marker")
    require("video7_select_mix:" in viewer and
            "video7_select_normal:" in viewer and
            "video7_modes,x" in viewer and
            "bit DHGRON" in viewer and "bit DHGROFF" in viewer and
            "sta COL80ON" in viewer and "sta COL80OFF" in viewer,
            "a2imgview must select Video-7 state 10 with five AN3 accesses")
    mix_sel = viewer.split("video7_select_mix:", 1)[1].split("rts", 1)[0]
    require(mix_sel.index("sta COL80OFF") < mix_sel.index("sta COL80ON") and
            mix_sel.count("bit DHGROFF") == 2 and
            mix_sel.count("bit DHGRON") == 3,
            "MIX must clock !80COL=1 on the first $C05F edge and 0 on the "
            "second (AppleWin latch F2,F1), committing on the fifth toggle")

    # Hold the last published frame and never expose a half-loaded extended
    # mode. HGRi/DHRi stage page 2 with $FF and commit after close; SHR ends
    # the unused legacy transaction when it takes over.
    require("prepare_hgri:" in viewer and viewer.count("jsr select_hgr") >= 2 and
            "prepare_dhgri:" in viewer and viewer.count("jsr select_dhgr") >= 2,
            "a2imgview must select each legacy mode before loading and restore it after MLI")
    require("copy_paged_main_disarmed:" in viewer and
            "lda $607C" in viewer and "sta paged_mode" in viewer and
            "lda #A2LI_HOLD" in viewer,
            "a2imgview must stage legacy page 2 inside the load transaction")
    dhgri_load = viewer.split("load_dhgri:", 1)[1].split("load_dhgr:", 1)[0]
    require("lda #$60" in dhgri_load and
            "jsr copy_paged_aux_from_6000" in dhgri_load and
            "lda #$40" not in dhgri_load,
            "DHGRi AUX page 2 must stage at $6000 without touching main $407C")
    aux_page_copy = viewer.split("copy_paged_aux_from_6000:", 1)[1].split(
        "; ---- MAIN $6000-$7FFF -> MAIN", 1)[0]
    require("lda #$60" in aux_page_copy and "sta src_ptr+1" in aux_page_copy and
            "lda #$40" in aux_page_copy and "sta dst_ptr+1" in aux_page_copy and
            "sta RAMRDOFF" in aux_page_copy and "sta RAMWRTON" in aux_page_copy,
            "page-2 AUX helper must copy MAIN $6000 to AUX $4000")
    paged_copy = viewer.split("copy_paged_main_disarmed:", 1)[1].split(
        "; ---- data ----", 1)[0]
    require("sta STORE80OFF" in paged_copy and
            paged_copy.index("sta RAMRDOFF") < paged_copy.index("lda $607C") and
            paged_copy.index("sta RAMWRTOFF") < paged_copy.index("lda $607C"),
            "legacy page-2 copy must force main reads and writes")
    hold_store = paged_copy.index("sta $607C")
    require(paged_copy.index("lda #0", hold_store) <
            paged_copy.index("sta src_ptr", hold_store),
            "page-2 copy must restore a zero pointer low byte after storing $FF")
    require(viewer.count("sta $407C               ; final write arms") == 2,
            "HGRi and DHRi must arm A2Li only after their file closes")
    hgr_show = viewer.split("show_hgr:", 1)[1].split("fail_load:", 1)[0]
    dhgr_show = viewer.split("show_dhgr:", 1)[1].split("load_shr:", 1)[0]
    require(hgr_show.index("jsr select_hgr") < hgr_show.index("sta $407C") and
            dhgr_show.index("jsr select_dhgr") < dhgr_show.index("sta $407C"),
            "A2Li mode must be the final write after the restored base mode")
    require("sta $087C               ; close the unused lores transaction" in
            hgr_show and
            "sta $087C               ; close the unused lores transaction" in
            dhgr_show,
            "every successful hires load must close the other A2Li hole")
    for label in ("select_hgr:", "select_dhgr:"):
        routine = viewer.split(label, 1)[1].split("rts", 1)[0]
        require("sta RAMRDOFF" in routine and "sta RAMWRTOFF" in routine,
                f"{label[:-1]} must restore main banking before A2Li arm")
    require(transaction.index("sta RAMRDOFF") < transaction.index("sta $407C") and
            transaction.index("sta RAMWRTOFF") < transaction.index("sta $407C"),
            "A2Li transaction must target main memory")
    shr = viewer.split("load_shr:", 1)[1].split("wait_key:", 1)[0]
    require(shr.index("jsr read_chunk") < shr.index("sta NEWVIDEO") <
            shr.index("jsr copy_aux_range"),
            "SHR must be selected after staging and before the visible AUX copy")
    require(shr.index("sta NEWVIDEO") < shr.index("sta $407C") <
            shr.index("jsr copy_aux_range") and "sta $087C" in shr,
            "SHR takeover must close both legacy load transactions")
    require("sta shr_magic,x" in shr and "sta $9DFC,x" in shr and
            shr.rindex("sta $9DFC,x") > shr.index("jsr mli_close"),
            "SHR extension magic must be restored only after close")
    require("No C029 edge here" in shr and
            "lda #$01\n    sta NEWVIDEO\n    lda #$C1\n    sta NEWVIDEO" not in shr,
            "SHR must stay active after magic; the uncached renderer redraws it")
    quit_path = viewer.split("quit:", 1)[1].split("; ---- Prefix", 1)[0]
    require("sta STORE80OFF" in quit_path and "sta RAMRDOFF" in quit_path and
            "sta RAMWRTOFF" in quit_path and "sta $407C" in quit_path and
            "sta $087C" in quit_path and
            quit_path.index("sta $087C") < quit_path.index("bit TEXTON"),
            "all-failed exit must close both transactions before ProDOS text")
    aux_copy = viewer.split("copy_aux_range:", 1)[1].split("rts", 1)[0]
    require(aux_copy.index("sta STORE80OFF") < aux_copy.index("sta RAMRDOFF") <
            aux_copy.index("sta RAMWRTON"),
            "every AUX copy must force main reads and AUX writes")

    # Launcher: HGR UI at $4000 with generated assets.
    require("* = $4000" in launcher and
            '!source "launcher_assets.a65"' in launcher and
            "hgr_init" in launcher,
            "launcher must be the $4000 HGR build with generated assets")
    # Launcher: selection handoff protocol and detection probe.
    require("SELECTION = $0300" in launcher and
            "sta SELECTION" in launcher,
            "launcher must hand the selection to STARTUP via $0300")
    # Read-only detection: the launcher must never execute slot-7 ROM
    # (a JSR into a lookalike card crashes machines we don't control),
    # and its fingerprint bytes must match the ROM we actually ship.
    require("jsr SP_ENTRY" not in launcher and "$C70D" not in launcher,
            "launcher detection must not execute the slot-7 ROM")
    rom = bytes(int(line.strip(), 16)
                for line in read(ROOT / "hdl" / "apple" /
                                 "smartport_a2retronet_style_c700.mem")
                .splitlines() if line.strip())
    def sig_bytes(label: str) -> list[int]:
        require(label in launcher, f"launcher must define {label}")
        line = launcher.split(label, 1)[1].splitlines()[1]
        require("!byte" in line, f"{label} must be followed by !byte")
        toks = line.split("!byte", 1)[1].split(";")[0]
        return [int(tok.strip().lstrip("$"), 16)
                for tok in toks.split(",") if tok.strip()]

    offsets = sig_bytes("sig_offsets:")
    values = sig_bytes("sig_values:")
    require(len(offsets) == len(values) and len(offsets) >= 5,
            "fingerprint must pair at least five offset/value bytes")
    for off, val in zip(offsets, values):
        require(rom[off] == val,
                f"launcher fingerprint ${off:02X}=${val:02X} does not match "
                f"the shipped ROM (${rom[off]:02X})")

    # Speed race: both TW speed writes and VBL-based timing.
    require("TWSPEED  = $C074" in speedrace and
            speedrace.count("sta TWSPEED") >= 3,
            "speed race must lock 1 MHz, go warp, and restore")
    require("RDVBLBAR" in speedrace,
            "speed race must time passes with RDVBLBAR")
    require("sta row_start" in speedrace and
            "sta row_end" in speedrace and
            "cmp row_end" in speedrace,
            "speed race must render separate equal-work screen halves")

    # Raster demo: every line tail re-arms the video-sync window.
    require("bit RDVBLBAR" in raster,
            "raster tails must touch $C019 for the slowdown region")
    require(raster.count("bit RDVBLBAR") >= 2,
            "both line tails must retrigger the window")

    # No printed string may overflow a 40-column row (screens indent by
    # up to 2 cells; anything longer than 38 wraps into the interleave).
    import re
    for path, src in (("launcher.a65", launcher),
                      ("speedrace.a65", speedrace),
                      ("rasterdemo.a65", raster),
                      ("textoverlay.a65", text_overlay)):
        for m in re.finditer(r'!text "([^"]*)"', src):
            require(len(m.group(1)) <= 38,
                    f"{path}: string longer than 38 columns: "
                    f"{m.group(1)[:30]!r}...")


def image_checks() -> None:
    """Manifest sanity without java: sizes and SHR self-description."""
    sys.path.insert(0, str(ROOT / "scripts"))
    import build_appletini_demo_disk as demo_build
    disk_names = {disk_name for _, disk_name, *_ in demo_build.IMAGE_FILES}
    require("LOGO6" not in disk_names and "PLAYFLD" not in disk_names and
            "FACE.I" in disk_names,
            "demo deck must keep DHGRi FACE.I and omit classic DHGR images")
    require(demo_build.VIDEO7_MIX_SOURCES == {"face.dhri"},
            "FACE.DHRI must be the only demo image marked for Video-7 MIX")
    for _, _, src_dir, src, size, fmt, paged in demo_build.IMAGE_FILES:
        path = demo_build.source_path(src_dir, src)
        require(path.is_file(), f"missing demo image {path}")
        data = path.read_bytes()
        require(len(data) == size,
                f"{src} is {len(data)} bytes, expected {size}")
        if fmt == demo_build.FMT_SHR:
            demo_build.check_conformance(src, data, paged)
        elif fmt in (demo_build.FMT_HGRI, demo_build.FMT_DHGRI):
            demo_build.check_legacy_paged(src, data, fmt)
    demo_build.generate_demo_viewer()
    generated = read(SOFTWARE / "a2imgview_demo.a65")
    mix_index = next(i for i, item in enumerate(demo_build.IMAGE_FILES)
                     if item[3] == "face.dhri")
    modes_line = generated.split("video7_modes:", 1)[1].splitlines()[1]
    modes = [int(value.strip())
             for value in modes_line.split("!byte", 1)[1].split(",")]
    require(modes[mix_index] == demo_build.VIDEO7_MIX and
            sum(mode == demo_build.VIDEO7_MIX for mode in modes) == 1,
            "generated viewer must select Video-7 MIX only for FACE.DHRI")


def fat_helper_checks() -> None:
    """Exercise the in-place AUTOEXEC replacement on a tiny FAT12 image."""
    sys.path.insert(0, str(ROOT / "scripts"))
    import build_appletini_demo_disk as demo_build

    image = bytearray(4 * 512)
    image[11:13] = (512).to_bytes(2, "little")
    image[13] = 1                 # sectors per cluster
    image[14:16] = (1).to_bytes(2, "little")
    image[16] = 1                 # FAT count
    image[17:19] = (16).to_bytes(2, "little")
    image[22:24] = (1).to_bytes(2, "little")
    image[512:515] = bytes((0xF0, 0xFF, 0xFF))
    image[515:517] = bytes((0xFF, 0x0F))  # cluster 2 = end of chain

    entry = 2 * 512
    image[entry:entry + 11] = b"AUTOEXECBAT"
    image[entry + 11] = 0x20
    image[entry + 26:entry + 28] = (2).to_bytes(2, "little")
    old = b"PATH C:\\DOS\r\n"
    image[entry + 28:entry + 32] = len(old).to_bytes(4, "little")
    image[3 * 512:3 * 512 + len(old)] = old

    new = b"ECHO OFF\r\nHGRCUBE\r\nQUIT\r\n"
    patched = demo_build.replace_fat12_root_file(
        bytes(image), b"AUTOEXECBAT", new)
    require(demo_build.read_fat12_root_file(
                patched, b"AUTOEXECBAT") == new,
            "FAT12 helper must replace AUTOEXEC and update its size")
    require(patched[3 * 512 + len(new):4 * 512] ==
            bytes(512 - len(new)),
            "FAT12 helper must clear stale bytes in the allocated cluster")

    bridge = b"before" + demo_build.AD8088_BRIDGE_QUIT + b"after"
    stub = b"return"
    patched_bridge = demo_build.add_ad8088_demo_return(bridge, stub)
    stub_offset = (demo_build.AD8088_RETURN_ADDR -
                   demo_build.AD8088_BRIDGE_LOAD)
    require(demo_build.AD8088_BRIDGE_QUIT not in patched_bridge and
            patched_bridge[stub_offset:] == stub,
            "AD8088 bridge must jump to the BASIC.SYSTEM return stub")


def assembled_viewer_mode_checks(binary: Path, symbol_list: Path) -> None:
    """Run the assembled mode setters against Enhanced //e video switches."""
    import re
    from py65.devices.mpu6502 import MPU

    symbols: dict[str, int] = {}
    for line in symbol_list.read_text(encoding="utf-8").splitlines():
        match = re.match(r"\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*\$([0-9a-fA-F]+)",
                         line)
        if match:
            symbols[match.group(1)] = int(match.group(2), 16)
    for name in ("select_hgr", "select_dhgr", "cur_video7",
                 "copy_paged_aux_from_6000", "copy_paged_main_disarmed",
                 "paged_mode"):
        require(name in symbols, f"assembled viewer has no {name} symbol")

    class EnhancedIIeMemory:
        def __init__(self) -> None:
            self.data = bytearray(65536)
            self.aux = bytearray(65536)
            self.text = True
            self.hires = False
            self.col80 = False
            self.dhires = False
            self.ramwrt = False

        def _access(self, addr: int, write: bool) -> None:
            if write and addr == 0xC00C:
                self.col80 = False
            elif write and addr == 0xC00D:
                self.col80 = True
            elif write and addr == 0xC004:
                self.ramwrt = False
            elif write and addr == 0xC005:
                self.ramwrt = True
            elif addr == 0xC050:
                self.text = False
            elif addr == 0xC051:
                self.text = True
            elif addr == 0xC056:
                self.hires = False
            elif addr == 0xC057:
                self.hires = True
            elif addr in (0xC05E, 0xC05F):
                # On a //e, C05E/F always update DHIRES as well as AN3.
                # IOUDIS gates this path only on the //c.
                self.dhires = addr == 0xC05E

        def __getitem__(self, key):
            if isinstance(key, slice):
                return self.data[key]
            self._access(key, False)
            return self.data[key]

        def __setitem__(self, key, value) -> None:
            if isinstance(key, slice):
                self.data[key] = value
                return
            self._access(key, True)
            if self.ramwrt and 0x0200 <= key < 0xC000:
                self.aux[key] = value
            else:
                self.data[key] = value

    image = binary.read_bytes()

    def run_mode_setter(label: str, video7_mode: int) -> str:
        memory = EnhancedIIeMemory()
        memory.data[0x2000:0x2000 + len(image)] = image
        # The SYS entry copies its $2000-$27FF body to $1000-$17FF. Mirror
        # that relocation so ACME's pseudopc symbols address the real code.
        memory.data[0x1000:0x1800] = memory.data[0x2000:0x2800]
        memory.data[symbols["cur_video7"]] = video7_mode
        mpu = MPU(memory=memory, pc=symbols[label])
        call_depth = 1
        for _ in range(256):
            opcode = memory.data[mpu.pc]
            if opcode == 0x60:             # RTS
                call_depth -= 1
                if call_depth == 0:
                    break
            elif opcode == 0x20:           # JSR abs
                call_depth += 1
            mpu.step()
        else:
            raise TestFailure(f"assembled {label} did not return")

        require(not memory.text and memory.hires,
                f"assembled {label} did not select hi-res graphics")
        base = "DHGR" if memory.col80 and memory.dhires else "HGR"
        return base + "i"  # Both tested image records carry A2Li type 1.

    hgr_i = run_mode_setter("select_hgr", 0)
    dhgr_i = run_mode_setter("select_dhgr", 2)
    require(hgr_i == "HGRi" and dhgr_i == "DHGRi" and hgr_i != dhgr_i,
            "assembled viewer must leave HGRi and DHGRi in distinct modes")
    print(f"PASS assembled viewer modes: {hgr_i} != {dhgr_i}")

    # Execute the real page-2 copy. This catches the load-hold marker's A=$FF
    # leaking into src_ptr/dst_ptr: the former code relied on A already being
    # zero when it initialized both pointer low bytes.
    memory = EnhancedIIeMemory()
    memory.data[0x2000:0x2000 + len(image)] = image
    memory.data[0x1000:0x1800] = memory.data[0x2000:0x2800]
    staged = bytearray(((i * 29 + 7) & 0xFF) for i in range(0x2000))
    staged[0x78:0x7C] = bytes((0xC1, 0xB2, 0xCC, 0xE9))
    staged[0x7C] = 1
    memory.data[0x6000:0x8000] = staged
    mpu = MPU(memory=memory, pc=symbols["copy_paged_main_disarmed"])
    for _ in range(200000):
        if memory.data[mpu.pc] == 0x60:      # outer RTS
            break
        mpu.step()
    else:
        raise TestFailure("assembled page-2 copy did not return")

    expected = bytearray(staged)
    expected[0x7C] = 0xFF
    require(memory.data[0x4000:0x6000] == expected,
            "assembled page-2 copy must stay page-aligned and preserve all bytes")
    require(memory.data[symbols["paged_mode"]] == 1 and
            memory.data[0x607C] == 0xFF,
            "assembled page-2 copy must save mode 1 and stage $FF until commit")
    print("PASS assembled A2Li $FF page-2 transaction copy")

    # Execute the matching page-2 AUX staging helper. MAIN $407C must remain
    # the live $FF marker while bytes move from spare MAIN $6000 to AUX $4000.
    memory = EnhancedIIeMemory()
    memory.data[0x2000:0x2000 + len(image)] = image
    memory.data[0x1000:0x1800] = memory.data[0x2000:0x2800]
    staged = bytes(((i * 17 + 3) & 0xFF) for i in range(0x2000))
    memory.data[0x6000:0x8000] = staged
    memory.data[0x4078:0x407D] = bytes((0xC1, 0xB2, 0xCC, 0xE9, 0xFF))
    mpu = MPU(memory=memory, pc=symbols["copy_paged_aux_from_6000"])
    for _ in range(200000):
        if memory.data[mpu.pc] == 0x60:
            break
        mpu.step()
    else:
        raise TestFailure("assembled page-2 AUX copy did not return")
    require(memory.aux[0x4000:0x6000] == staged,
            "assembled AUX page-2 copy must map MAIN $6000 to AUX $4000")
    require(memory.data[0x4078:0x407D] ==
            bytes((0xC1, 0xB2, 0xCC, 0xE9, 0xFF)),
            "assembled AUX page-2 copy must preserve the main A2Li hold")
    print("PASS assembled DHGRi AUX page-2 staging copy")


def assembly_checks() -> None:
    subprocess.run([sys.executable,
                    str(ROOT / "scripts" / "gen_hgr_assets.py")],
                   check=True)
    exe = shutil.which("acme") or ACME_EXE
    if not Path(exe).is_file():
        print("SKIP assembly checks (acme not found)")
        return
    env = dict(os.environ, ACME=ACME_LIB)
    budgets = {
        "launcher.a65": (0x4000, 0x9600),      # HGR page 1 is the framebuffer
        "speedrace.a65": (0x6000, 0x9600),     # below ProDOS buffers
        "rasterdemo.a65": (0x6000, 0x9600),
        "textoverlay.a65": (0x2000, 0x6000),
        # generated by image_checks(); relocation stub copies 2 KB
        "a2imgview_demo.a65": (0x2000, 0x2800),
        "ad8088_demo_return.a65": (0x1E00, 0x2000),
    }
    with tempfile.TemporaryDirectory() as tmp:
        for name, (org, ceiling) in budgets.items():
            out = Path(tmp) / (name + ".bin")
            symbols = Path(tmp) / (name + ".sym")
            command = [exe, "-f", "plain", "-o", str(out)]
            if name == "a2imgview_demo.a65":
                command.extend(("-l", str(symbols)))
            command.append(name)
            subprocess.run(
                command,
                cwd=SOFTWARE, env=env, check=True,
                capture_output=True)
            size = out.stat().st_size
            require(size > 0, f"{name}: empty output")
            require(org + size <= ceiling,
                    f"{name}: {size} bytes overruns ${ceiling:04X}")
            print(f"PASS {name}: {size} bytes at ${org:04X}")
            if name == "a2imgview_demo.a65":
                assembled_viewer_mode_checks(out, symbols)
                if AC_JAR.is_file() and shutil.which("java"):
                    disk_viewer = subprocess.run(
                        ["java", "-jar", str(AC_JAR), "-g",
                         str(SOFTWARE / "Appletini_Demos.po"), "A2IMGVIEW"],
                        check=True, capture_output=True).stdout
                    require(disk_viewer == out.read_bytes(),
                            "tracked demo disk A2IMGVIEW must match the "
                            "tested assembled viewer")
                    print("PASS tracked disk contains tested A2IMGVIEW")


def main() -> int:
    try:
        static_checks()
        print("PASS static demo-disk checks")
        image_checks()
        print("PASS demo image manifest checks")
        fat_helper_checks()
        print("PASS AD8088 FAT12 helper checks")
        assembly_checks()
        print("demo disk checks passed")
        return 0
    except TestFailure as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    except subprocess.CalledProcessError as exc:
        sys.stderr.buffer.write(exc.stderr or b"")
        print(f"FAIL: assembly error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
