#!/usr/bin/env python3
"""Virtual AppleMouse card regression: firmware + gateware fix batch.

Three layers:
 1. Static: the committed .mem must equal a fresh build_mouse_rom assembly
    (regenerate, never hand-edit), with the AppleMouse ID bytes, the eight
    standard vectors, and the internal READCLAMP entry at $Cn1A.
 2. Functional: py65 executes the actual firmware against a Python model
    of the mouse_card registers, checking the protocol fixes: SERVEMOUSE
    always releases IRQ but masks $0E before claiming an interrupt,
    READMOUSE clears status without releasing IRQ, INITMOUSE resets clamps
    and leaves the mode byte alone, READCLAMP returns clamps through the
    protocol screen holes, CLAMPMOUSE passes full 16-bit values.
 3. Simulation: tb_mouse_card.sv covers the gateware side (VBL interrupts
    without the enable bit, independent IRQ/status acknowledgement, the
    SERVE/READ race, distinct movement-state bits, 16-bit clamps, the
    AppleWin min>max rule, and CMD $03 defaults).
    Requires xvlog/xelab/xsim on PATH.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_mouse_rom as builder  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build" / "mouse_card"
BASE = 0xC200


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


# ---------------------------------------------------------------------------
# Layer 1: static checks
# ---------------------------------------------------------------------------

def load_mem() -> bytes:
    data = []
    for line in (ROOT / "hdl" / "apple" / "mouse_card_slot2.mem").read_text(
            encoding="utf-8").splitlines():
        line = line.split("//")[0].strip()
        data.extend(int(tok, 16) for tok in line.split())
    return bytes(data)


def static_checks() -> bytes:
    page = load_mem()
    require(len(page) == 256, "mouse ROM must be exactly 256 bytes")
    require(page == builder.build(),
            "mouse_card_slot2.mem is stale -- rerun scripts/build_mouse_rom.py")
    sig = {0x05: 0x38, 0x07: 0x18, 0x0B: 0x01, 0x0C: 0x20, 0xFB: 0xD6}
    for off, val in sig.items():
        require(page[off] == val, f"AppleMouse ID byte ${off:02X} != ${val:02X}")
    for i in range(9):
        require(0x1B <= page[0x12 + i] <= 0xFA,
                f"entry vector {i} points outside the code region")
    core = (ROOT / "hdl" / "apple" / "mouse_card.sv").read_text(encoding="utf-8")
    require("CMD_CLAMP_DEFAULTS = 8'h03" in core.replace("  ", " ") or
            "CMD_CLAMP_DEFAULTS" in core,
            "mouse_card must implement the CMD $03 clamp-defaults command")
    require("clamp_norm" in core and "min_value + max_value" in core,
            "mouse_card must implement the AppleWin min>max normalization")
    require("vblank_start_pulse && mode_q[MODE_VBL_IRQ_BIT]" in core,
            "VBL pending must not be gated on the mouse enable bit")
    # Prove the split ACK contract from executable RTL, not comments:
    # READMOUSE ($01) consumes status, while SERVEMOUSE ($02) releases IRQ.
    rtl = re.sub(r"/\*.*?\*/|//[^\n]*", "", core, flags=re.DOTALL)
    ack_match = re.search(
        r"\bIO_ACK\s*:\s*begin(.*?)\n\s*end\s*\n\s*default\s*:",
        rtl, flags=re.DOTALL)
    require(ack_match is not None, "mouse_card IO_ACK case not found")
    ack_body = ack_match.group(1)
    status_ack = re.search(
        r"if\s*\(\s*ab_read\.data\[0\]\s*\)\s*begin(.*?)\bend\b",
        ack_body, flags=re.DOTALL)
    irq_ack = re.search(
        r"if\s*\(\s*ab_read\.data\[1\]\s*\)\s*begin(.*?)\bend\b",
        ack_body, flags=re.DOTALL)
    require(status_ack is not None and irq_ack is not None,
            "mouse_card must decode both ACK bits independently")
    status_body = status_ack.group(1)
    irq_body = irq_ack.group(1)
    for signal in ("movement_since_read_q", "movement_irq_pending_q",
                   "button_pending_q", "vbl_pending_q"):
        require(f"{signal} <= 1'b0;" in status_body,
                f"READMOUSE status ACK does not clear {signal}")
    require("prev_buttons_q <= buttons_q;" in status_body,
            "READMOUSE status ACK does not latch previous buttons")
    require(re.search(r"\birq_pending_q\b", status_body) is None,
            "READMOUSE status ACK incorrectly releases IRQ")
    require("irq_pending_q <= 1'b0;" in irq_body,
            "SERVEMOUSE IRQ ACK does not release IRQ")
    require(not any(re.search(rf"\b{signal}\b", irq_body) for signal in
                    ("movement_since_read_q", "movement_irq_pending_q",
                     "button_pending_q", "vbl_pending_q")),
            "SERVEMOUSE IRQ ACK incorrectly consumes status causes")
    require("ab_write_d.assert_irq = apple_bus_active && irq_pending_q;" in rtl,
            "mouse IRQ output must be driven by its independent IRQ latch")
    require("IO_X_HI: mouse_x_q[15:8] <= ab_read.data;" in core and
            "IO_Y_HI: mouse_y_q[15:8] <= ab_read.data;" in core,
            "absolute position writes must store raw (CLEARMOUSE sets 0,0 "
            "regardless of the clamp minimum)")
    usb = (ROOT / "ps_sources" / "frontend" / "usb_hid_service.c").read_text(
        encoding="utf-8")
    require("0x3FF" not in usb,
            "usb_hid_service still masks mouse values to 10 bits")
    print("PASS static checks")
    return page


# ---------------------------------------------------------------------------
# Layer 2: functional firmware execution under py65
# ---------------------------------------------------------------------------

class CardModel:
    """Python mirror of the mouse_card register file."""

    def __init__(self):
        self.status = 0x00
        self.pos = {"xlo": 0, "xhi": 0, "ylo": 0, "yhi": 0}
        self.clamps = {0: [0, 1023], 1: [0, 1023]}   # axis -> [min, max]
        self.axis = 0
        self.stage = {"minlo": 0, "minhi": 0, "maxlo": 0, "maxhi": 0}
        self.mode_writes = []
        self.ack_writes = []
        self.cmd_writes = []
        self.irq_pending = False

    def read(self, reg: int) -> int:
        if reg == 0x0:
            return self.status
        if reg == 0x1:
            return self.pos["xlo"]
        if reg == 0x2:
            return self.pos["xhi"]
        if reg == 0x3:
            return self.pos["ylo"]
        if reg == 0x4:
            return self.pos["yhi"]
        if reg == 0x8:
            return self.clamps[self.axis][0] & 0xFF
        if reg == 0x9:
            return (self.clamps[self.axis][0] >> 8) & 0xFF
        if reg == 0xA:
            return self.clamps[self.axis][1] & 0xFF
        if reg == 0xB:
            return (self.clamps[self.axis][1] >> 8) & 0xFF
        return 0x00

    def write(self, reg: int, value: int) -> None:
        if reg == 0x1:
            self.pos["xlo"] = value
        elif reg == 0x2:
            self.pos["xhi"] = value
        elif reg == 0x3:
            self.pos["ylo"] = value
        elif reg == 0x4:
            self.pos["yhi"] = value
        elif reg == 0x7:
            self.axis = value & 1
        elif reg == 0x8:
            self.stage["minlo"] = value
        elif reg == 0x9:
            self.stage["minhi"] = value
        elif reg == 0xA:
            self.stage["maxlo"] = value
        elif reg == 0xB:
            self.stage["maxhi"] = value
        elif reg == 0xC:
            self.cmd_writes.append(value)
            if value == 0x02:
                lo = self.stage["minlo"] | (self.stage["minhi"] << 8)
                hi = self.stage["maxlo"] | (self.stage["maxhi"] << 8)
                if lo > hi:
                    lo, hi = 0, (lo + hi) & 0xFFFF
                self.clamps[self.axis] = [lo, hi]
            elif value == 0x03:
                self.clamps[0] = [0, 1023]
                self.clamps[1] = [0, 1023]
        elif reg == 0xE:
            self.mode_writes.append(value)
        elif reg == 0xF:
            self.ack_writes.append(value)
            if value & 0x01:
                # READMOUSE consumes interrupt reasons and moved-since-read,
                # then makes the current buttons the previous buttons.
                current = self.status & 0x90
                self.status = current
                if current & 0x80:
                    self.status |= 0x40
                if current & 0x10:
                    self.status |= 0x01
            if value & 0x02:
                # SERVEMOUSE releases the output latch without altering the
                # reason byte which READMOUSE still has to return.
                self.irq_pending = False


def run_routine(page: bytes, model: CardModel, entry_off: int, a: int,
                ram: dict | None = None, x: int = 0, y: int = 0):
    from py65.devices.mpu65c02 import MPU
    from py65.memory import ObservableMemory

    mem = ObservableMemory()

    def io_read(address):
        return model.read(address & 0xF)

    def io_write(address, value):
        model.write(address & 0xF, value)

    mem.subscribe_to_read(range(0xC0A0, 0xC0B0), io_read)
    mem.subscribe_to_write(range(0xC0A0, 0xC0B0), io_write)

    mpu = MPU(memory=mem)
    for i, byte in enumerate(page):
        mem.write(BASE + i, [byte])
    if ram:
        for addr, val in ram.items():
            mem.write(addr, [val])

    sentinel = 0x03F0
    mpu.pc = BASE + page[0x12 + entry_off]
    mpu.a = a
    mpu.x = x
    mpu.y = y
    # Arrange for the routine's RTS to land on the sentinel: with SP=$FB,
    # RTS pulls lo from $01FC and hi from $01FD, then adds one.
    mem.write(0x01FC, [(sentinel - 1) & 0xFF])
    mem.write(0x01FD, [((sentinel - 1) >> 8) & 0xFF])
    mpu.sp = 0xFB
    steps = 0
    while mpu.pc != sentinel:
        mpu.step()
        steps += 1
        if steps > 10000:
            raise RuntimeError("firmware routine did not return")
    carry = mpu.p & 0x01
    return carry, mpu, mem


def functional_checks(page: bytes) -> None:
    V_SET, V_SRV, V_RD, V_CLR, V_POS, V_CLA, V_HOM, V_INI, V_RCL = range(9)

    # SERVEMOUSE: state bits alone (button held = $C0) must NOT claim, but
    # the original card still releases IRQ to retire a reasonless service.
    model = CardModel()
    model.status = 0xC0
    model.irq_pending = True
    carry, _, mem = run_routine(page, model, V_SRV, 0)
    require(carry == 1, "SERVEMOUSE claimed an interrupt for state bits only")
    require(model.ack_writes == [0x02],
            "reasonless SERVEMOUSE did not release IRQ")
    require(not model.irq_pending and model.status == 0xC0,
            "SERVEMOUSE did not release IRQ while preserving status")
    require(mem[0x077A] == 0xC0, "SERVEMOUSE must still update the hole")

    # SERVEMOUSE: real interrupt bits claim and ack.
    model = CardModel()
    model.status = 0x4A          # button-state noise + VBL/move int bits
    model.irq_pending = True
    carry, _, mem = run_routine(page, model, V_SRV, 0)
    require(carry == 0, "SERVEMOUSE missed a real interrupt")
    require(model.ack_writes == [0x02], "SERVEMOUSE must ack with $02")
    require(not model.irq_pending and model.status == 0x4A,
            "SERVEMOUSE consumed status while releasing IRQ")

    # SETMOUSE: valid + invalid modes.
    model = CardModel()
    carry, _, mem = run_routine(page, model, V_SET, 0x0F)
    require(carry == 0 and model.mode_writes == [0x0F] and
            mem[0x07FA] == 0x0F, "SETMOUSE $0F failed")
    model = CardModel()
    carry, _, _ = run_routine(page, model, V_SET, 0x10)
    require(carry == 1 and model.mode_writes == [],
            "SETMOUSE must reject modes >= $10")

    # INITMOUSE: clamps reset via CMD $03, position zeroed, mode untouched.
    model = CardModel()
    model.clamps[0] = [5, 100]
    model.clamps[1] = [7, 200]
    model.pos = {"xlo": 0x34, "xhi": 0x02, "ylo": 0x10, "yhi": 0x01}
    model.status = 0x2E
    model.irq_pending = True
    carry, _, _ = run_routine(page, model, V_INI, 0)
    require(carry == 0, "INITMOUSE must return carry clear")
    require(0x03 in model.cmd_writes, "INITMOUSE must issue CMD $03")
    require(model.clamps[0] == [0, 1023] and model.clamps[1] == [0, 1023],
            "INITMOUSE did not reset the clamps")
    require(model.pos == {"xlo": 0, "xhi": 0, "ylo": 0, "yhi": 0},
            "INITMOUSE did not zero the position")
    require(model.mode_writes == [],
            "INITMOUSE wrote the mode register (protocol violation)")
    require(0x01 in model.ack_writes,
            "INITMOUSE must clear status without releasing IRQ")
    require(model.status == 0 and model.irq_pending,
            "INITMOUSE status clear incorrectly released IRQ")

    # READCLAMP (internal $Cn1A entry, TN#7 protocol): the caller sets the
    # $0478 selector from $4E down to $47. Each call returns one byte in
    # $0578, and the caller stores the first result at Byte[7], working
    # backward through Byte[0].
    model = CardModel()
    model.clamps[0] = [0x0102, 0x0304]     # X min/max
    model.clamps[1] = [0x0506, 0x0708]     # Y min/max
    returned = []
    result = [0] * 8
    for byte_ptr, sel in zip(range(7, -1, -1), range(0x4E, 0x46, -1)):
        carry, _, mem = run_routine(
            page, model, V_RCL, 0,
            ram={0x0478: sel, 0x04F8: 0},
            x=0xC2, y=0x20)
        require(carry == 0, f"READCLAMP selector ${sel:02X} returned carry set")
        require(mem[0x0478] == sel,
                "READCLAMP must not modify the caller's selector")
        returned.append(mem[0x0578])
        result[byte_ptr] = mem[0x0578]
    require(returned == [0x08, 0x04, 0x07, 0x03,
                         0x06, 0x02, 0x05, 0x01],
            f"READCLAMP selector order wrong: {[hex(b) for b in returned]}")
    require(result == [0x01, 0x05, 0x02, 0x06,
                       0x03, 0x07, 0x04, 0x08],
            f"READCLAMP result buffer wrong: {[hex(b) for b in result]}")
    for bad in (0x46, 0x4F, 0x00):
        model = CardModel()
        carry, _, _ = run_routine(
            page, model, V_RCL, 0, ram={0x0478: bad, 0x04F8: 0},
            x=0xC2, y=0x20)
        require(carry == 1,
                f"READCLAMP must reject selector ${bad:02X}")

    # CLEARMOUSE with a nonzero clamp minimum: the position must really
    # reach 0,0 (the RTL stores absolute writes raw; regression for the
    # clamp-on-high-byte-write bug).
    model = CardModel()
    model.clamps[0] = [5, 100]
    model.clamps[1] = [7, 200]
    model.pos = {"xlo": 0x40, "xhi": 0x00, "ylo": 0x50, "yhi": 0x00}
    carry, _, mem = run_routine(page, model, V_CLR, 0)
    require(carry == 0, "CLEARMOUSE must return carry clear")
    require(model.pos == {"xlo": 0, "xhi": 0, "ylo": 0, "yhi": 0},
            f"CLEARMOUSE position not zeroed under clamps: {model.pos}")
    require(mem[0x047A] == 0 and mem[0x057A] == 0,
            "CLEARMOUSE holes not zeroed")

    # CLAMPMOUSE: full 16-bit staging + normalization (in the model).
    model = CardModel()
    holes = {0x0478: 0xEC, 0x0578: 0xFF, 0x04F8: 0x2B, 0x05F8: 0x01}
    carry, _, _ = run_routine(page, model, V_CLA, 0, ram=holes)
    require(carry == 0, "CLAMPMOUSE valid axis must return carry clear")
    require(model.clamps[0] == [0, 0x0117],
            f"CLAMPMOUSE normalization wrong: {model.clamps[0]}")

    # READMOUSE: registers land in the holes and only status is cleared.
    # SERVEMOUSE exclusively owns release of the IRQ output latch.
    model = CardModel()
    model.pos = {"xlo": 0x11, "xhi": 0x02, "ylo": 0x33, "yhi": 0x01}
    model.status = 0x2A
    model.irq_pending = True
    carry, _, mem = run_routine(page, model, V_RD, 0)
    require(carry == 0 and model.ack_writes == [0x01],
            "READMOUSE must clear status without releasing IRQ")
    require(model.status == 0 and model.irq_pending,
            "READMOUSE did not clear status independently of IRQ")
    require(mem[0x047A] == 0x11 and mem[0x057A] == 0x02 and
            mem[0x04FA] == 0x33 and mem[0x05FA] == 0x01 and
            mem[0x077A] == 0x2A, "READMOUSE holes wrong")

    print("PASS functional firmware checks")


# ---------------------------------------------------------------------------
# Layer 3: gateware bench
# ---------------------------------------------------------------------------

def vivado_tool(name: str) -> str:
    bat = shutil.which(f"{name}.bat")
    if bat:
        return bat
    tool = shutil.which(name)
    if tool:
        return tool
    raise FileNotFoundError(f"unable to locate Vivado tool {name}")


def run(cmd: list[str], log: Path) -> str:
    completed = subprocess.run(
        cmd, cwd=OUT_DIR, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log.write_text(completed.stdout, encoding="utf-8")
    if completed.returncode != 0:
        print(completed.stdout)
        raise RuntimeError(f"{Path(cmd[0]).name} failed")
    return completed.stdout


def simulate() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ROOT / "hdl" / "apple" / "mouse_card_slot2.mem",
                    OUT_DIR / "mouse_card_slot2.mem")
    run([vivado_tool("xvlog"), "--sv",
         str(ROOT / "hdl" / "globals.sv"),
         str(ROOT / "hdl" / "apple" / "mouse_card.sv"),
         str(ROOT / "hdl" / "sim" / "tb_mouse_card.sv")],
        OUT_DIR / "xvlog.log")
    run([vivado_tool("xelab"), "tb_mouse_card", "-s", "tb_mouse_card_snap",
         "--timescale", "1ns/1ps"],
        OUT_DIR / "xelab.log")
    stdout = run([vivado_tool("xsim"), "tb_mouse_card_snap", "--runall"],
                 OUT_DIR / "xsim.log")
    if "MOUSE CARD PASS" not in stdout:
        print(stdout)
        raise AssertionError("tb_mouse_card did not pass")
    print("PASS tb_mouse_card simulation")


def main() -> int:
    page = static_checks()
    functional_checks(page)
    try:
        vivado_tool("xvlog")
    except FileNotFoundError:
        print("SKIP simulation: Vivado tools not on PATH", file=sys.stderr)
        return 1
    simulate()
    print("test_mouse_card: ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
