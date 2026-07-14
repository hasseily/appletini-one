"""Consistency checks for the PCPI Applicard (Z80) virtual card on slot 5.

Static checks: PL module wiring (apple_top / appletini_yarz_top / crossbar
slot 7), PS service + build registration, config persistence, UART command.

Behavioral checks: a Python reference model of the PL latch/flag contract
(derived from MAME a2applicard.cpp + the GZ80 B00 schematic) exercised with
the access patterns the PCPI 6502 driver and the Z80 boot ROM use, plus the
GZ80 bank-register page mapping replicated from applicard_z80.c.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def read_text(path):
    return (ROOT / path).read_text(encoding="utf-8")


# ---------------------------------------------------------------------------
# Reference model of applicard_card.sv (6502 + AXI views)
# ---------------------------------------------------------------------------

class ApplicardPl:
    """The PL card: two latches, two flags, sticky reset/NMI, seq counter."""

    def __init__(self):
        self.toz80 = 0
        self.to6502 = 0
        self.f_z80 = False
        self.f_6502 = False
        self.reset_req = False
        self.nmi_req = False
        self.seq = 0

    def _reset_action(self):
        self.toz80 = 0
        self.to6502 = 0
        self.f_z80 = False
        self.f_6502 = False
        self.reset_req = True
        self.seq = (self.seq + 1) & 0xFF

    def apple_read(self, offset):
        offset &= 0xF
        if offset == 0:
            self.f_6502 = False
            return self.to6502
        if offset == 1:
            return self.toz80
        if offset == 2:
            return 0x80 if self.f_z80 else 0x00
        if offset == 3:
            return 0x80 if self.f_6502 else 0x00
        if offset == 5:
            self._reset_action()
            return 0xFF
        if offset == 7:
            self.nmi_req = True
            return 0xFF
        return 0xFF

    def apple_write(self, offset, data):
        offset &= 0xF
        if offset == 1:
            self.toz80 = data & 0xFF
            self.f_z80 = True
            self.seq = (self.seq + 1) & 0xFF
        elif offset == 5:
            self._reset_action()
        elif offset == 7:
            self.nmi_req = True

    def axi_status(self):
        return ((self.toz80 << 16) | (self.seq << 8) |
                (0x8 if self.nmi_req else 0) | (0x4 if self.reset_req else 0) |
                (0x2 if self.f_6502 else 0) | (0x1 if self.f_z80 else 0))

    def axi_to6502(self, data):
        self.to6502 = data & 0xFF
        self.f_6502 = True

    def axi_control(self, value):
        if value & 0x1 and ((value >> 8) & 0xFF) == self.seq:
            self.f_z80 = False
        if value & 0x2:
            self.reset_req = False
        if value & 0x4:
            self.nmi_req = False


def model_protocol():
    pl = ApplicardPl()

    # Idle: PCPI detection probes the readback latch first.
    require(pl.apple_read(2) == 0x00, "F_Z80 must idle clear")
    require(pl.apple_read(3) == 0x00, "F_6502 must idle clear")
    pl.apple_write(1, 0xA5)
    require(pl.apple_read(1) == 0xA5, "TOZ80 readback (PCPI detection)")
    require(pl.apple_read(2) == 0x80, "F_Z80 set by $C0D1 write")

    # PS consume with stale seq must be rejected.
    st = pl.axi_status()
    stale = ((st >> 8) + 1) & 0xFF
    pl.axi_control(0x1 | (stale << 8))
    require(pl.f_z80, "stale ACK must not consume")
    pl.axi_control(0x1 | (((st >> 8) & 0xFF) << 8))
    require(not pl.f_z80, "seq-matched ACK consumes")
    require((st >> 16) & 0xFF == 0xA5, "STATUS carries TOZ80 data")

    # Z80 -> 6502 direction: deposit, flag, consume-on-read.
    pl.axi_to6502(0x5A)
    require(pl.apple_read(3) == 0x80, "F_6502 set by deposit")
    require(pl.apple_read(0) == 0x5A, "TO6502 data")
    require(pl.apple_read(3) == 0x00, "read consumed F_6502")

    # $C0D5 reset: in-cycle clear + sticky request + seq bump kills
    # any in-flight stale ACK.
    pl.apple_write(1, 0x44)
    pre_reset_seq = pl.seq
    require(pl.apple_read(5) == 0xFF, "reset read returns $FF")
    require(pl.reset_req, "RESET_REQ latched")
    require(not pl.f_z80 and not pl.f_6502, "reset clears flags")
    require(pl.apple_read(1) == 0x00, "reset clears TOZ80 latch")
    pl.axi_control(0x2)
    pl.apple_write(1, 0x55)
    pl.axi_control(0x1 | (pre_reset_seq << 8))
    require(pl.f_z80, "pre-reset seq ACK must be stale after reset")

    # NMI sticky on read or write of $C0D7.
    pl.apple_read(7)
    require(pl.nmi_req, "NMI_REQ latched by read")
    pl.axi_control(0x4)
    pl.apple_write(7, 0)
    require(pl.nmi_req, "NMI_REQ latched by write")


# ---------------------------------------------------------------------------
# GZ80 banked page mapping replicated from applicard_z80.c
# ---------------------------------------------------------------------------

def page_map(bank_reg, rom_mapped):
    """Return (kind, bank, page_in_bank) for each of the eight 8KB pages."""
    bank = (bank_reg >> 1) & 0x1F  # bits 1-3 on GZ80; bits 1-5 here (2 MB)
    common = bank_reg & 0x40
    pages = []
    for p in range(8):
        if rom_mapped and p < 4:
            pages.append(("rom", None, None))
        elif common and p >= 4:
            pages.append(("ram", 0, p))
        else:
            pages.append(("ram", bank, p))
    return pages


def model_banking():
    # Reset: ROM shadow over the whole lower 32K, bank 0 upper.
    pages = page_map(0x00, True)
    require(all(k == "rom" for k, _, _ in pages[:4]), "ROM shadow low 32K")
    require(all(b == 0 for _, b, _ in pages[4:]), "bank 0 upper at reset")

    # ROM out, bank 3 selected (bank register bits 1-3).
    pages = page_map(3 << 1, False)
    require(all(b == 3 for _, b, _ in pages), "whole 64K on bank 3")

    # Common area: upper 32K pinned to bank 0.
    pages = page_map((5 << 1) | 0x40, False)
    require(all(b == 5 for _, b, _ in pages[:4]), "lower 32K on bank 5")
    require(all(b == 0 for _, b, _ in pages[4:]), "common upper 32K on bank 0")

    # GZ80 compatibility window: bank values 0-7 (bits 1-3) behave exactly
    # like the real card's 512 KB.
    banks = {page_map(b << 1, False)[0][1] for b in range(8)}
    require(banks == set(range(8)), "all 8 GZ80 banks addressable")

    # Extension: bits 4-5 widen the field to 32 banks = 2 MB. GZ80 software
    # never sets them, so the compat window is untouched.
    banks = {page_map(b << 1, False)[0][1] for b in range(32)}
    require(banks == set(range(32)), "all 32 x 64K banks addressable (2 MB)")
    z80_h = read_text("ps_sources/frontend/applicard_z80.h")
    require("#define APPLICARD_Z80_BANKS        32U" in z80_h,
            "emulator must provide 32 banks")


# ---------------------------------------------------------------------------
# Static wiring checks
# ---------------------------------------------------------------------------

def check_hdl():
    card = read_text("hdl/apple/applicard_card.sv")
    top = read_text("hdl/apple/apple_top.sv")
    wrapper = read_text("hdl/appletini_yarz_top.sv")
    hdl_sources = read_text("hdl/hdl_sources.txt")
    tb = read_text("hdl/sim/tb_applicard_card.sv")

    require("apple/applicard_card.sv" in hdl_sources,
            "applicard_card.sv missing from hdl_sources.txt")

    # Slot 5 DEVSEL decode and the documented side effects.
    require("(ab_read.addr[7:4] == (4'h8 + {1'b0, slot_assign}))" in card,
            "slot I/O decode must follow the shared $C0(8+slot)X pattern")
    require("as_common.wdata[15:8] == toz80_seq_q" in card,
            "PS consume must be seq-matched")
    require("axi_read_addr_q <= as_common.araddr" in card,
            "AXI rdata must be registered (axidouble OPT_REGISTERED)")

    # apple_top wiring: enable bit 5, gate_ab, arbiter membership.
    require("wire card_slot5_enable = card_slot_enable_mask_q[5];" in top,
            "slot 5 enable wire missing")
    require("gate_ab(ab_read, card_slot5_enable)" in top,
            "applicard must be gated bus-deaf when disabled")
    require(".slot_assign(3'h5)" in top, "applicard must claim slot 5")
    require("applicard_ab_write" in top.split("client_writes")[1],
            "applicard_ab_write missing from write arbiter")
    require("NUM_CLIENTS(11)" in top, "arbiter NUM_CLIENTS must be 11")

    # Crossbar slave 7 handoff.
    require(".applicard_as_client(as_clients[7])" in wrapper,
            "applicard must own AxiSimple slave 7")
    require("assign as_clients[7].rdata = 0;" not in wrapper,
            "AxiSimple slave 7 must not be tied off")

    require("tb_applicard_card" in tb, "testbench must exist")


def check_ps():
    service = read_text("ps_sources/frontend/applicard_service.c")
    z80 = read_text("ps_sources/frontend/applicard_z80.c")
    regs = read_text("ps_sources/frontend/applicard_regs.h")
    frontend = read_text("ps_sources/frontend/main.c")
    uart = read_text("ps_sources/frontend/uart_control.c")
    config = read_text("ps_sources/frontend/config_menu.c")
    config_h = read_text("ps_sources/frontend/config_menu.h")
    vitis = read_text("scripts/create_vitis_workspace.py")
    z80user = read_text("third_party/z80emu/z80user.h")

    require("0x40070000U" in regs, "PS register base must be crossbar slave 7")

    # Z80 RAM is PS-only cached DDR in the free window.
    require("0x3F600000U" in read_text(
        "ps_sources/frontend/applicard_service.h"),
        "Z80 RAM base must live in the free DDR window")

    # Service is initialized and polled from the CPU0 main loop.
    require("applicard_service_init(UART0_BASE);" in frontend,
            "main() must init the applicard service")
    require("applicard_service_poll();" in frontend,
            "main loop must poll the applicard service")
    require("applicard_service_set_enabled(enable);" in frontend,
            "slot-5 enable must arm/disarm the service")

    # Boot ROM is embedded only: no SD file required, and deliberately no
    # SD override -- a stray user file named APPLICARD.ROM must never be
    # able to change the card's behavior.
    rom_c = read_text("ps_sources/frontend/applicard_rom.c")
    require("1d461000" in rom_c, "embedded ROM must be the CRC-verified v9 dump")
    require("applicard_rom, APPLICARD_Z80_ROM_SIZE" in service,
            "service must map the embedded ROM")
    require("APPLICARD.ROM" not in service and "f_open" not in service,
            "service must not read any ROM from SD")

    # Handshake dispatch essentials.
    require("APPLICARD_CONTROL_ACK_TOZ80(APPLICARD_STATUS_SEQ(st))" in z80,
            "IN $20 must ACK with the sequence number")
    require("case 0xC0U:" in z80, "GZ80 bank register port missing")
    require("APPLICARD_Z80_IDLE_STREAK" in z80, "idle governor missing")

    # UART command group + persistence.
    require('str_ieq(argv[0], "z80")' in uart, "z80 UART command missing")
    require('"applicard.slot5.enabled"' in config,
            "config parse key missing")
    require("applicard.slot5.enabled=%s" in config, "config save key missing")
    require("applicard_slot5_enabled" in config_h, "config field missing")
    require("APPLICARD_CONTROL_SLOT" in config,
            "config must apply slot 5 at boot")

    # Config-menu tab (below Ethernet) with enable toggle + help entry.
    internal_h = read_text("ps_sources/frontend/config_menu_internal.h")
    help_c = read_text("ps_sources/frontend/config_menu_help.c")
    tabs_c = read_text("ps_sources/frontend/config_menu_device_tabs.c")
    require("CONFIG_TAB_ETHERNET,\n    CONFIG_TAB_APPLICARD," in internal_h,
            "Applicard tab must sit below Ethernet")
    require('"Z80 Applicard"' in config, "tab label missing")
    require("config_menu_draw_applicard(fb, menu, x, y, w);" in config,
            "tab draw dispatch missing")
    require("config_menu_set_applicard_enabled(menu,\n                menu->applicard_slot5_enabled ? 0U : 1U);"
            in config, "tab toggle handler missing")
    require("void config_menu_draw_applicard" in tabs_c, "tab draw fn missing")
    require("TAB_WITH_OVERRIDES(CONFIG_TAB_APPLICARD," in help_c,
            "help entry missing")

    # Resource-usage item: menu <-> service wall-cap plumbing + persistence.
    require('"applicard.resource.max"' in config,
            "resource parse key missing")
    require("applicard.resource.max=%s" in config, "resource save key missing")
    require("set_applicard_resource_max" in read_text(
        "ps_sources/frontend/config_menu.h"), "platform callback missing")
    require("APPLICARD_WALL_CAP_MAX_US" in frontend,
            "main.c must map resource setting to wall cap")

    # The idle streak accumulates across slices so it can reach its threshold.
    burst = service[service.index("Compute-greedy scheduling"):]
    require("g_ctx.status_streak = 0U" not in burst,
            "streak must not reset inside the burst loop")

    # Remote mount hands the host a flushed card even when CP/M holds the
    # drive motor (dirty-track flush at mount entry).
    require("disk2_service_flush_dirty_now" in frontend,
            "mount entry must flush pending disk2 writes")

    # File timestamps come from the RTC cache, not the fixed fallback.
    shot = read_text("ps_sources/frontend/screenshot_service.c")
    require("g_fattime_cached_valid" in shot, "fattime RTC cache missing")
    require("screenshot_service_update_fattime_from_rtc(&g_rtc);" in frontend,
            "main loop must feed the fattime cache")

    # Build registration: sources + include dir for the vendored core.
    for needle in (
        "ps_sources/frontend/applicard_service.c",
        "ps_sources/frontend/applicard_z80.c",
        "third_party/z80emu/z80emu.c",
        "third_party/z80emu",
    ):
        require(needle in vitis, f"{needle} missing from Vitis build")

    # The vendored core must bind to the applicard context, not zextest.
    require("applicard_z80.h" in z80user,
            "z80user.h must bind the applicard context")
    require("zextest" not in z80user, "upstream zextest binding left behind")


def main():
    model_protocol()
    model_banking()
    check_hdl()
    check_ps()
    print("test_applicard_card: all checks passed")


if __name__ == "__main__":
    main()
