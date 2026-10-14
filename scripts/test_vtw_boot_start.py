#!/usr/bin/env python3
"""Exercise the retained boot ROM and production saved-vTW startup services."""

import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import textwrap

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/vtw_boot_start_test"


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


class RomCpu:
    """Small NMOS-6502 runner for these actual ROM bytes, not a rewritten loop.

    The existing VBL routines have separate cycle tests. Here their return is
    mocked to supply one frame, keeping the test focused on the startup hold,
    machine reports, menu events, and first storage instruction.
    """
    def __init__(self, slot, c8, symbols, machine=3, gs_mask=0x80,
                 target=1, exit_kind="timeout", armed=True):
        self.ram = bytearray(65536)
        self.ram[0xC700:0xC800] = slot
        self.ram[0xC800:0xC900] = c8
        self.symbols = symbols
        self.gate = symbols["VTW_BOOT_GATE_CONTROL"] + 1
        self.ram[self.gate] = int(armed)
        self.machine, self.gs_mask = machine, gs_mask
        self.target, self.exit_kind = target, exit_kind
        self.a = self.x = self.y = 0
        self.sp, self.pc, self.z, self.n, self.c = 0xFF, 0xC700, False, False, False
        self.opened = self.pending = self.gs_escape = False
        self.prompts = self.targets = self.gate_passes = self.frames = 0
        self.events = []
        self.halted = False
        self.ram[0xFBB3] = 6 if machine in (2, 3) else 0
        self.ram[0xFBC0] = 0xE0 if machine == 3 else 0

    def flags(self, value):
        self.z, self.n = value == 0, bool(value & 0x80)
        return value

    def push(self, value):
        self.ram[0x100 + self.sp] = value & 255
        self.sp = (self.sp - 1) & 255

    def pop(self):
        self.sp = (self.sp + 1) & 255
        return self.ram[0x100 + self.sp]

    def rts(self):
        self.pc = (self.pop() | (self.pop() << 8)) + 1
        self.pc &= 0xFFFF

    def read(self, address):
        if address == 0xC02D:
            return self.gs_mask
        if address == 0xC000:
            return 0x9B if self.exit_kind == "escape" and self.frames else 0
        if address == 0xC0F0:
            value = 0x10 if self.target == 2 else 0x08
            if self.exit_kind == "menu":
                value |= 0x40
                if self.opened:
                    value |= 0x20
            elif self.exit_kind == "timeout" and self.frames:
                value |= 1
            return value
        if self.pending and address in (0xCA01, 0xCA02):
            page = 0xC6 if self.target == 2 else 0xC7
            return page - 1 if address == 0xCA01 else page
        return self.ram[address]

    def write(self, address, value):
        self.ram[address] = value
        if address != 0xC0F0:
            return
        self.events.append(value)
        if self.gs_escape:
            self.gs_escape = False
        elif value == 0x27:
            self.gs_escape = True
        elif value == 3:
            self.opened = True
        elif value in (2, 4):
            self.pending = True

    def step(self):
        if self.pending and self.pc == (0xC600 if self.target == 2 else 0xC700):
            self.targets += 1
            self.halted = True
            return
        if self.pc == 0xFE1F:
            self.c = self.machine != 4
            self.rts()
            return
        if self.pc == self.symbols["SHOW_PROMPT"]:
            self.prompts += 1
        if self.pc in (self.symbols["VBL_MOUSECARD_SYNC"],
                       self.symbols["WAIT_VBL_START"]):
            self.frames += 1
            self.a, self.y = 5, self.ram[0xCA00]
            self.flags(self.a)
            self.rts()
            return
        if self.pc == self.symbols["VTW_BOOT_GATE_CONTROL"]:
            self.gate_passes += 1
        op = self.ram[self.pc]
        self.pc = (self.pc + 1) & 0xFFFF
        if op in (0xA9, 0xA2, 0xA0, 0xC9, 0x29, 0xD0, 0xF0, 0x10, 0x90):
            operand = self.ram[self.pc]
            self.pc += 1
        elif op in (0xAD, 0xAE, 0xAC, 0x8D, 0x8E, 0x99, 0xB9, 0x20, 0x4C):
            operand = self.ram[self.pc] | (self.ram[self.pc + 1] << 8)
            self.pc += 2
        elif op in (0xA5, 0x85):
            operand = self.ram[self.pc]
            self.pc += 1
        if op in (0xA9, 0xAD, 0xA5, 0xB9):
            self.a = self.flags(operand if op == 0xA9 else
                                self.read((operand + self.y) & 65535) if op == 0xB9 else
                                self.read(operand))
        elif op in (0xA2, 0xAE):
            self.x = self.flags(operand if op == 0xA2 else self.read(operand))
        elif op in (0xA0, 0xAC):
            self.y = self.flags(operand if op == 0xA0 else self.read(operand))
        elif op == 0xC9:
            self.c = self.a >= operand
            self.flags((self.a - operand) & 255)
        elif op == 0x29:
            self.a = self.flags(self.a & operand)
        elif op in (0x8D, 0x85, 0x99):
            self.write((operand + self.y) & 65535 if op == 0x99 else operand, self.a)
        elif op == 0x8E:
            self.write(operand, self.x)
        elif op == 0x20:
            self.push((self.pc - 1) >> 8)
            self.push(self.pc - 1)
            self.pc = operand
        elif op == 0x4C:
            self.pc = operand
        elif op in (0xD0, 0xF0, 0x10, 0x90):
            take = (not self.z if op == 0xD0 else self.z if op == 0xF0 else
                    not self.n if op == 0x10 else not self.c)
            if take:
                self.pc = (self.pc + (operand if operand < 128 else operand - 256)) & 65535
        elif op == 0x48:
            self.push(self.a)
        elif op == 0x68:
            self.a = self.flags(self.pop())
        elif op == 0x60:
            self.rts()
        elif op in (0x8A, 0x98):
            self.a = self.flags(self.x if op == 0x8A else self.y)
        elif op in (0xAA, 0xA8):
            if op == 0xAA:
                self.x = self.flags(self.a)
            else:
                self.y = self.flags(self.a)
        elif op in (0xCA, 0x88):
            if op == 0xCA:
                self.x = self.flags((self.x - 1) & 255)
            else:
                self.y = self.flags((self.y - 1) & 255)
        elif op == 0xC8:
            self.y = self.flags((self.y + 1) & 255)
        elif op == 0x0A:
            self.c = bool(self.a & 128)
            self.a = self.flags((self.a << 1) & 255)
        elif op == 0x38:
            self.c = True
        elif op == 0xB8:
            pass  # BVC below always branches after the ROM's CLV.
        elif op == 0x50:
            offset = self.ram[self.pc]
            self.pc += 1
            self.pc = (self.pc + (offset if offset < 128 else offset - 256)) & 65535
        elif op == 0xEA:
            pass
        else:
            raise AssertionError(f"unhandled ROM opcode {op:02X} at {self.pc - 1:04X}")

    def run(self, limit=5000, until_hold=False):
        for _ in range(limit):
            if self.halted or until_hold and self.gate_passes >= 30:
                return
            self.step()
        raise AssertionError(f"ROM did not finish or hold at {self.pc:04X}")


def rom_tests():
    generator = module("startup_rom_builder", ROOT / "scripts/build_vtw_boot_start_rom.py")
    slot, _reloc, c8, symbols = generator.build()
    assert generator.HEADER.read_bytes() == generator.header_text().encode("ascii")
    for machine in (2, 3):
        for target in (1, 2):
            for exit_kind in ("timeout", "escape", "menu"):
                native = RomCpu(slot, c8, symbols, machine=machine,
                                target=target, exit_kind=exit_kind)
                native.run(until_hold=True)
                assert native.gate_passes == 30 and native.prompts == native.targets == 0
                assert native.events[-1] == 0x20 + machine and not native.pending
                accelerated = RomCpu(slot, c8, symbols, machine=machine,
                                     target=target, exit_kind=exit_kind, armed=False)
                accelerated.run()
                assert accelerated.targets == 1 and accelerated.gate_passes == 1
                assert accelerated.prompts == int(exit_kind != "menu")
                assert accelerated.opened == (exit_kind == "menu")
                # A failed takeover clears one immediate and native ROM resumes.
                native.ram[native.gate] = 0
                native.run()
                assert native.targets == 1 and native.prompts == int(exit_kind != "menu")
    for machine, mask in ((1, 0x80), (4, 0x80), (4, 0x22), (4, 0x23)):
        cpu = RomCpu(slot, c8, symbols, machine=machine, gs_mask=mask)
        cpu.run()
        assert cpu.targets == 1 and cpu.gate_passes == 0
    print("PASS actual ROM bytes: 12 saved-IIe holds, accelerated exits and native cancellations; II+/GS bypass")


PREAMBLE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint32_t test_reg_read(uint32_t address);
static void test_reg_write(uint32_t address, uint32_t value);
void uart_puts(uint32_t base, const char *text);
void uart_puthex(uint32_t base, uint32_t value);
void uart_putdec(uint32_t base, uint32_t value);
uint8_t onee_service_isolation_confirmed(void);
uint8_t appletini_config_ram_enabled(void);
uint8_t appletini_config_ramworks_enabled(void);
void disk2_service_set_enabled(uint8_t enabled);
#define COMMON_H
#define UART_H
#define DISK2_SERVICE_H
#define ONEE_SERVICE_H
#define UART_CONTROL_H
#define UART0_BASE 0U
typedef enum { UI_KEY_NONE, UI_KEY_UP, UI_KEY_DOWN, UI_KEY_LEFT, UI_KEY_RIGHT,
    UI_KEY_ENTER, UI_KEY_BACK, UI_KEY_TOGGLE, UI_KEY_SCANLINES, UI_KEY_TAB,
    UI_KEY_SHIFT_TAB, UI_KEY_PAGE_UP, UI_KEY_PAGE_DOWN, UI_KEY_SPACE,
    UI_KEY_ESC, UI_KEY_MENU } ui_key_t;
typedef struct { ui_key_t key; uint8_t pressed,ascii; } ui_input_t;
#define REG_READ(a) test_reg_read((uint32_t)(a))
#define REG_WRITE(a,v) test_reg_write((uint32_t)(a),(uint32_t)(v))
#include "../../ps_sources/frontend/boot_menu_service.c"
#include "../../ps_sources/frontend/vtw_service.c"
const uint8_t apple2e_cpu_rom[16384] = {0};
static uint32_t card_regs[256], boot_regs[8];
static uint8_t c8_rom[256];
static XTime now_ticks, enabled_at;
static unsigned ctrl_sticks,core_echo,core_writes,reset_writes,shadow_writes;
static unsigned first_core_ctrl,first_core_target,slot7_hidden,restore_before_echo;
static unsigned begin_scan, trace_gate_cleared;
static uint8_t configured_ram, aux_write_sticks = 1U, isolated;
static uint8_t expect_native_c3_before_release;
static uint32_t reg_index(uint32_t address) { return (address-APPLE_DEBUG_BASE)/4U; }
static uint32_t test_reg_read(uint32_t address) {
    if(address>=BOOT_MENU_MMIO_BASE&&address<=BOOT_MENU_REG_C8_PATCH)
        return boot_regs[(address-BOOT_MENU_MMIO_BASE)/4U];
    assert(reg_index(address)<256U);
    if(address==CARD_CTRL_VTW_STATUS_REG) {
        uint32_t ctrl=card_regs[reg_index(CARD_CTRL_VTW_CTRL_REG)];
        uint32_t status=0;
        if(ctrl&CARD_CTRL_VTW_CTRL_ENABLE_BIT) {
            status|=CARD_CTRL_VTW_STATUS_ENABLE_EFF;
            if(now_ticks-enabled_at>=4000U&&!(ctrl&CARD_CTRL_VTW_CTRL_APPLE_RES_BIT))
                status|=CARD_CTRL_VTW_STATUS_BUS_OWNED;
            if(core_echo&&(ctrl&CARD_CTRL_VTW_CTRL_CORE_RUN_BIT))
                status|=CARD_CTRL_VTW_STATUS_CORE_RUN;
        }
        return status;
    }
    return card_regs[reg_index(address)];
}
static void test_reg_write(uint32_t address,uint32_t value) {
    if(address==BOOT_MENU_REG_C8_PATCH) {
        if(((value>>8)&255U)==BOOT_MENU_VTW_C8_GATE_OFFSET && !(value&255U) &&
           expect_native_c3_before_release)
            assert(c8_rom[BOOT_MENU_NATIVE_C3_OFFSET]==1);
        c8_rom[(value>>8)&255U]=(uint8_t)value;
        if(((value>>8)&255U)==BOOT_MENU_VTW_C8_GATE_OFFSET&&(value&255U)==0U)
            trace_gate_cleared=1U;
        return;
    }
    if(address>=BOOT_MENU_MMIO_BASE&&address<BOOT_MENU_REG_C8_PATCH) {
        if(address==BOOT_MENU_REG_HANDOFF_MODE&&g_vtw_boot_scan_override) {
            if(value==BOOT_MENU_HANDOFF_SMARTPORT) begin_scan=1U;
            else if(core_writes&&!core_echo) ++restore_before_echo;
        }
        boot_regs[(address-BOOT_MENU_MMIO_BASE)/4U]=value;
        return;
    }
    assert(reg_index(address)<256U);
    if(address==CARD_CTRL_AUX_PROVIDE_REG && !aux_write_sticks) return;
    if(address==CARD_CTRL_AUX_PROBE_REG) {
        card_regs[reg_index(address)] &= ~2U;
        return;
    }
    if(address==CARD_CTRL_VTW_CTRL_REG) {
        uint32_t old=card_regs[reg_index(address)];
        if(!ctrl_sticks) return;
        if((value&CARD_CTRL_VTW_CTRL_ENABLE_BIT)&&!(old&CARD_CTRL_VTW_CTRL_ENABLE_BIT))
            enabled_at=now_ticks;
        if((value&CARD_CTRL_VTW_CTRL_APPLE_RES_BIT)&&!(old&CARD_CTRL_VTW_CTRL_APPLE_RES_BIT)) {
            ++reset_writes;boot_regs[0]=0;
        }
        if((value&CARD_CTRL_VTW_CTRL_CORE_RUN_BIT)&&!(old&CARD_CTRL_VTW_CTRL_CORE_RUN_BIT)) {
            ++core_writes;first_core_ctrl=value;
            first_core_target=boot_regs[4]&3U;
            slot7_hidden=first_core_target==BOOT_MENU_HANDOFF_DISK2;
            assert(!g_vtw_boot_gate_armed);
            assert(!c8_rom[BOOT_MENU_VTW_C8_GATE_OFFSET] || isolated);
            assert(!c8_rom[BOOT_MENU_NATIVE_C3_OFFSET]);
        }
    }
    if(address==CARD_CTRL_VTW_SHADOW_DATA_REG) ++shadow_writes;
    card_regs[reg_index(address)]=value;
}
void XTime_GetTime(XTime *value) { *value=now_ticks; }
void uart_puts(uint32_t base,const char *text) { (void)base;(void)text; }
void uart_puthex(uint32_t base,uint32_t value) { (void)base;(void)value; }
void uart_putdec(uint32_t base,uint32_t value) { (void)base;(void)value; }
uint8_t appletini_config_ram_enabled(void) { return configured_ram; }
uint8_t appletini_config_ramworks_enabled(void) { return 0; }
uint8_t onee_service_isolation_confirmed(void) { return isolated; }
void disk2_service_set_enabled(uint8_t enabled) { (void)enabled; }
static void fixture(unsigned target) {
    memset(card_regs,0,sizeof(card_regs));memset(boot_regs,0,sizeof(boot_regs));
    memset(c8_rom,0,sizeof(c8_rom));now_ticks=enabled_at=0;
    ctrl_sticks=core_echo=1;core_writes=reset_writes=shadow_writes=0;
    first_core_ctrl=first_core_target=slot7_hidden=restore_before_echo=0;
    begin_scan=trace_gate_cleared=0;
    configured_ram=isolated=0;aux_write_sticks=1U;
    expect_native_c3_before_release=0;
    g_machine_id_raw=g_aux_card_present=g_aux_probe_seen=g_machine_forced=0;
    g_iigs_seen=g_machine_identity_fault=g_iigs_policy_fault_seen=0;
    g_iigs_slot_config_valid=g_iigs_external_slot_mask=0;
    g_machine_mode_applied=g_aux_provide_applied=g_ramworks_applied=0xFFU;
    g_intent_enabled=0;g_speed_mode=CARD_CTRL_VTW_SPEED_DIVIDED;
    g_pace_divider=19U;g_announced_wait=g_announced_handoff_wait=0;
    g_take_polls=g_sessions_started=0;
    boot_menu_service_init();boot_regs[4]=target;
    vtw_service_init(0);vtw_service_set_enabled(1);
    vtw_service_prepare_boot_start();
    assert(g_vtw_boot_gate_armed && c8_rom[BOOT_MENU_VTW_C8_GATE_OFFSET]==1);
}
static void report(unsigned id,unsigned timeout) {
    if(id==2U || id==3U) card_regs[reg_index(CARD_CTRL_AUX_PROBE_REG)]=2U;
    boot_regs[0]=((0x20U+id)<<24)|(id<<12)|
        (timeout ? BOOT_MENU_STATUS_TIMEOUT_EXPIRED :
         BOOT_MENU_STATUS_BOOT_ELIGIBLE|BOOT_MENU_STATUS_WINDOW_ACTIVE);
    boot_menu_service_refresh_machine_policy();
}
static void millis(unsigned value) { now_ticks+=(XTime)value*(COUNTS_PER_SECOND/1000U); }
static unsigned speed(void) {
    return (card_regs[reg_index(CARD_CTRL_VTW_CTRL_REG)]>>CARD_CTRL_VTW_CTRL_SPEED_SHIFT)&
        CARD_CTRL_VTW_CTRL_SPEED_MASK;
}
static void reach_load(void) {
    assert(vtw_service_boot_start_pending());
    vtw_service_poll();assert(g_state==VTW_ST_TAKE_BUS);
    /* Tight CPU0 loops cannot mistake 64 polls for the hardware's 80 cycles. */
    for(unsigned i=0;i<128;++i) vtw_service_poll();
    assert(g_state==VTW_ST_TAKE_BUS && g_vtw_boot_gate_armed);
    millis(1);vtw_service_poll();assert(g_state==VTW_ST_RES_HOLD);
    millis(99);vtw_service_poll();assert(g_state==VTW_ST_RES_HOLD);
    millis(1);vtw_service_poll();assert(g_state==VTW_ST_RES_SETTLE);
    millis(49);vtw_service_poll();assert(g_state==VTW_ST_RES_SETTLE);
    millis(1);vtw_service_poll();assert(g_state==VTW_ST_LOAD_ROM);
}
static void single_boot(void) {
    for(unsigned target=1;target<=2;++target) for(unsigned timeout=0;timeout<=1;++timeout) {
        fixture(target);report(3,timeout);reach_load();
        vtw_service_poll();
        assert(g_state==VTW_ST_RUN&&!vtw_service_boot_start_pending());
        assert(reset_writes==1&&shadow_writes==16386&&core_writes==1);
        assert(begin_scan&&!slot7_hidden&&first_core_target==1&&boot_regs[4]==target);
        assert(trace_gate_cleared&&speed()==CARD_CTRL_VTW_SPEED_1MHZ);
        assert(g_boot_menu_speed_hold&&!g_boot_start_active&&!restore_before_echo);
        unsigned resets=reset_writes,rom=shadow_writes;
        vtw_service_set_speed(CARD_CTRL_VTW_SPEED_FULL,37U);
        assert(speed()==CARD_CTRL_VTW_SPEED_1MHZ&&g_speed_mode==CARD_CTRL_VTW_SPEED_FULL);
        boot_regs[0]=(0x23U<<24)|(3U<<12)|
            BOOT_MENU_STATUS_BOOT_ELIGIBLE|BOOT_MENU_STATUS_MENU_REQUESTED;
        assert(boot_menu_service_initial_menu_active());
        for(unsigned i=0;i<100;++i) vtw_service_poll();
        assert(g_boot_menu_speed_hold&&speed()==CARD_CTRL_VTW_SPEED_1MHZ);
        boot_regs[0]=0;boot_regs[4]=target|4U;
        assert(!boot_menu_service_initial_menu_active());
        assert(vtw_service_boot_handoff_pending()&&!vtw_service_boot_start_pending());
        vtw_service_poll();assert(!g_boot_menu_speed_hold&&speed()==CARD_CTRL_VTW_SPEED_FULL);
        assert(!vtw_service_boot_handoff_pending());
        assert(reset_writes==resets&&shadow_writes==rom&&core_writes==1);
    }
    puts("PASS production startup: one reset/ROM load/core release; checked Disk II restoration; prompt at 1 MHz; live speed on handoff");
}
static void failures(void) {
    fixture(2);report(3,0);ctrl_sticks=0;
    vtw_service_poll();millis(49);vtw_service_poll();assert(g_state==VTW_ST_TAKE_BUS);
    millis(1);vtw_service_poll();assert(g_state==VTW_ST_IDLE&&!g_vtw_boot_gate_armed);
    assert(!vtw_service_boot_start_pending()&&!c8_rom[BOOT_MENU_VTW_C8_GATE_OFFSET]);
    fixture(2);report(3,0);reach_load();core_echo=0;vtw_service_poll();
    assert(g_state==VTW_ST_BOOT_RELEASE&&boot_regs[4]==1&&!restore_before_echo);
    boot_menu_service_set_handoff(BOOT_MENU_HANDOFF_SMARTPORT);
    assert(boot_regs[4]==1&&g_vtw_boot_saved_handoff==BOOT_MENU_HANDOFF_SMARTPORT);
    boot_menu_service_set_handoff(BOOT_MENU_HANDOFF_DISK2);
    assert(boot_regs[4]==1&&g_vtw_boot_saved_handoff==BOOT_MENU_HANDOFF_DISK2);
    millis(1);vtw_service_poll();assert(boot_regs[4]==1);
    core_echo=1;vtw_service_poll();assert(g_state==VTW_ST_RUN&&boot_regs[4]==2);
    fixture(2);report(3,0);reach_load();core_echo=0;vtw_service_poll();
    millis(50);vtw_service_poll();
    assert(g_state==VTW_ST_IDLE&&!g_vtw_boot_gate_armed&&boot_regs[4]==2);
    assert(!(card_regs[reg_index(CARD_CTRL_VTW_CTRL_REG)]&CARD_CTRL_VTW_CTRL_ENABLE_BIT));
    fixture(1);report(3,0);reach_load();vtw_service_poll();
    boot_regs[4]=5U;ctrl_sticks=0;
    assert(vtw_service_boot_handoff_pending()&&!vtw_service_boot_start_pending());
    unsigned reset_count=reset_writes,rom_count=shadow_writes;
    vtw_service_poll();
    assert(g_state==VTW_ST_RUN&&g_boot_menu_speed_hold&&speed()==CARD_CTRL_VTW_SPEED_1MHZ);
    assert(vtw_service_boot_handoff_pending()&&!vtw_service_boot_start_pending());
    assert(reset_writes==reset_count&&shadow_writes==rom_count);
    ctrl_sticks=1;vtw_service_poll();
    assert(!g_boot_menu_speed_hold&&!vtw_service_boot_handoff_pending());
    assert(speed()==CARD_CTRL_VTW_SPEED_DIVIDED&&reset_writes==reset_count);
    for(unsigned phase=0;phase<6;++phase) {
        fixture(2);report(3,0);
        if(phase) { vtw_service_poll(); }
        if(phase>=2) { millis(1);vtw_service_poll(); }
        if(phase>=3) { millis(100);vtw_service_poll(); }
        if(phase>=4) { millis(50);vtw_service_poll(); }
        if(phase>=5) { core_echo=0;vtw_service_poll(); }
        vtw_service_set_enabled(0);
        assert(g_state==VTW_ST_IDLE&&!g_vtw_boot_gate_armed&&!vtw_service_boot_start_pending());
        assert(!g_boot_menu_speed_hold&&!g_vtw_boot_scan_override&&boot_regs[4]==2);
        assert(!(card_regs[reg_index(CARD_CTRL_VTW_CTRL_REG)]&CARD_CTRL_VTW_CTRL_ENABLE_BIT));
    }
    puts("PASS startup timeouts, delayed CORE_RUN echo, and cancellation in six phases release the native ROM hold");
}
static void safety(void) {
    fixture(1);vtw_service_poll();
    assert(g_state==VTW_ST_IDLE&&g_vtw_boot_gate_armed&&!vtw_service_boot_start_pending());
    report(4,0);vtw_service_poll();
    assert(g_state==VTW_ST_IDLE&&!g_vtw_boot_gate_armed&&!core_writes&&!reset_writes);
    fixture(1);report(1,0);vtw_service_poll();
    assert(g_state==VTW_ST_IDLE&&!g_vtw_boot_gate_armed&&!reset_writes);
    boot_regs[4]=5U;vtw_service_poll();assert(g_state==VTW_ST_TAKE_BUS&&!g_boot_start_active);
    fixture(1);report(3,0);vtw_service_poll();
    boot_regs[7]=BOOT_MENU_MACHINE_ID_FAULT;
    boot_menu_service_refresh_machine_policy();vtw_service_poll();
    assert(g_state==VTW_ST_IDLE&&!g_vtw_boot_gate_armed&&!g_boot_menu_speed_hold);
    /* Both saved intents ON must not trap the private Enhanced IIe ROM. */
    fixture(2);
    card_regs[reg_index(CARD_CTRL_ONEE_MODE_REG)]=
        (CARD_CTRL_ONEE_STATUS_SIGNATURE<<CARD_CTRL_ONEE_STATUS_SIGNATURE_SHIFT)|
        CARD_CTRL_ONEE_STATUS_REQUEST_BIT|CARD_CTRL_ONEE_STATUS_EFFECTIVE_BIT|
        CARD_CTRL_ONEE_STATUS_ISOLATED_BIT|CARD_CTRL_ONEE_STATUS_SELECTED_BIT|
        CARD_CTRL_ONEE_STATUS_HDL_PRESENT_BIT;
    isolated=1;
    assert(vtw_service_onee_start(1));
    assert(!g_vtw_boot_gate_armed&&!g_boot_menu_speed_hold&&!vtw_service_boot_start_pending());
    assert(speed()==CARD_CTRL_VTW_SPEED_DIVIDED&&g_intent_enabled);
    report(3,0);
    assert(!g_native_boot_policy_pending&&!c8_rom[BOOT_MENU_VTW_C8_GATE_OFFSET]);
    assert(!c8_rom[BOOT_MENU_NATIVE_C3_OFFSET]);
    puts("PASS UNKNOWN, IIgs, II+, identity-fault and mixed saved ONE//e/vTW intent safety");
}
static void native_fixture(unsigned ram) {
    fixture(1);
    vtw_service_set_enabled(0);
    configured_ram=(uint8_t)ram;
    expect_native_c3_before_release=(uint8_t)ram;
    assert(!g_vtw_boot_gate_armed&&g_native_boot_policy_pending);
    assert(c8_rom[BOOT_MENU_VTW_C8_GATE_OFFSET]==1);
    assert(!c8_rom[BOOT_MENU_NATIVE_C3_OFFSET]);
}
static void native_policy(void) {
    for(unsigned id=2;id<=3;++id) {
        native_fixture(1);
        /* Identity alone must not release native code before its aux probe. */
        boot_regs[0]=((0x20U+id)<<24)|(id<<12)|BOOT_MENU_STATUS_BOOT_ELIGIBLE;
        boot_menu_service_refresh_machine_policy();
        assert(g_native_boot_policy_pending&&!g_aux_probe_seen);
        assert(c8_rom[BOOT_MENU_VTW_C8_GATE_OFFSET]&&!c8_rom[BOOT_MENU_NATIVE_C3_OFFSET]);
        report(id,0);
        assert(g_aux_probe_seen&&!g_native_boot_policy_pending);
        assert(card_regs[reg_index(CARD_CTRL_AUX_PROVIDE_REG)]==1);
        assert(!c8_rom[BOOT_MENU_VTW_C8_GATE_OFFSET]&&c8_rom[BOOT_MENU_NATIVE_C3_OFFSET]==1);
        /* Repeat polling/warm probe must retain a working selector. */
        for(unsigned i=0;i<10;++i) boot_menu_service_refresh_machine_policy();
        report(id,1);
        assert(!g_native_boot_policy_pending&&c8_rom[BOOT_MENU_NATIVE_C3_OFFSET]==1);
        configured_ram=0;boot_menu_service_refresh_machine_policy();
        assert(!c8_rom[BOOT_MENU_NATIVE_C3_OFFSET]);
        configured_ram=1;boot_menu_service_refresh_machine_policy();
        assert(c8_rom[BOOT_MENU_NATIVE_C3_OFFSET]==1);
        /* Failed aux-provide readback keeps native code held, selector off. */
        native_fixture(1);aux_write_sticks=0;report(id,0);
        assert(g_native_boot_policy_pending&&c8_rom[BOOT_MENU_VTW_C8_GATE_OFFSET]);
        assert(!c8_rom[BOOT_MENU_NATIVE_C3_OFFSET]);
        card_regs[reg_index(CARD_CTRL_AUX_PROVIDE_REG)]=1;aux_write_sticks=1;
        boot_menu_service_refresh_machine_policy();
        assert(!g_native_boot_policy_pending&&!c8_rom[BOOT_MENU_VTW_C8_GATE_OFFSET]);
        assert(c8_rom[BOOT_MENU_NATIVE_C3_OFFSET]==1);
        /* A physical aux card and disabled RAM preserve motherboard choice. */
        native_fixture(1);
        expect_native_c3_before_release=0;
        card_regs[reg_index(CARD_CTRL_AUX_PROBE_REG)]=3;
        boot_regs[0]=((0x20U+id)<<24)|(id<<12)|BOOT_MENU_STATUS_BOOT_ELIGIBLE;
        boot_menu_service_refresh_machine_policy();
        assert(g_aux_card_present&&!g_native_boot_policy_pending);
        assert(!c8_rom[BOOT_MENU_NATIVE_C3_OFFSET]&&!c8_rom[BOOT_MENU_VTW_C8_GATE_OFFSET]);
        assert(!card_regs[reg_index(CARD_CTRL_AUX_PROVIDE_REG)]);
        native_fixture(0);report(id,0);
        assert(!g_native_boot_policy_pending&&!c8_rom[BOOT_MENU_NATIVE_C3_OFFSET]);
        /* Saved-vTW owns its hold; a cancelled takeover restores native C3. */
        fixture(1);configured_ram=1;report(id,0);
        assert(!g_native_boot_policy_pending&&g_vtw_boot_gate_armed);
        assert(c8_rom[BOOT_MENU_VTW_C8_GATE_OFFSET]&&!c8_rom[BOOT_MENU_NATIVE_C3_OFFSET]);
        expect_native_c3_before_release=1;
        ctrl_sticks=0;vtw_service_poll();millis(50);vtw_service_poll();
        assert(!g_vtw_boot_gate_armed&&!c8_rom[BOOT_MENU_VTW_C8_GATE_OFFSET]);
        assert(c8_rom[BOOT_MENU_NATIVE_C3_OFFSET]==1);
    }
    for(unsigned id=1;id<=4;id+=3) {
        native_fixture(1);expect_native_c3_before_release=0;report(id,0);
        assert(!g_native_boot_policy_pending&&!c8_rom[BOOT_MENU_VTW_C8_GATE_OFFSET]);
        assert(!c8_rom[BOOT_MENU_NATIVE_C3_OFFSET]);
    }
    native_fixture(1);report(3,0);
    boot_menu_service_force_machine_mode(CARD_MACHINE_MODE_IIGS);
    boot_menu_service_refresh_machine_policy();
    assert(!c8_rom[BOOT_MENU_NATIVE_C3_OFFSET]);
    /* Each physical identity can transition to the isolated Enhanced ROM. */
    for(unsigned id=0;id<=4;id+=id==0?1:3) {
        fixture(1);if(id) report(id,0);
        isolated=1;
        card_regs[reg_index(CARD_CTRL_ONEE_MODE_REG)]=
            (CARD_CTRL_ONEE_STATUS_SIGNATURE<<CARD_CTRL_ONEE_STATUS_SIGNATURE_SHIFT)|
            CARD_CTRL_ONEE_STATUS_REQUEST_BIT|CARD_CTRL_ONEE_STATUS_EFFECTIVE_BIT|
            CARD_CTRL_ONEE_STATUS_ISOLATED_BIT|CARD_CTRL_ONEE_STATUS_SELECTED_BIT|
            CARD_CTRL_ONEE_STATUS_HDL_PRESENT_BIT;
        assert(vtw_service_onee_start(1));
        report(3,0);
        assert(!g_native_boot_policy_pending&&!c8_rom[BOOT_MENU_VTW_C8_GATE_OFFSET]);
        assert(!c8_rom[BOOT_MENU_NATIVE_C3_OFFSET]);
    }
    /* A physical II+ bypasses the native hold, then accelerates after handoff. */
    fixture(1);configured_ram=1;report(1,0);vtw_service_poll();
    assert(!g_native_boot_policy_pending&&!g_vtw_boot_gate_armed);
    boot_regs[4]=5U;vtw_service_poll();
    assert(g_state==VTW_ST_TAKE_BUS);
    millis(1);vtw_service_poll();millis(100);vtw_service_poll();
    millis(50);vtw_service_poll();vtw_service_poll();
    assert(g_state==VTW_ST_RUN&&!c8_rom[BOOT_MENU_VTW_C8_GATE_OFFSET]);
    assert(!c8_rom[BOOT_MENU_NATIVE_C3_OFFSET]);
    puts("PASS production native C3 policy: fresh probe, delayed MMIO, physical aux, RAM off, warm repeats, vTW rollback, II+/GS/ONEe safety");
}
int main(void) { single_boot();failures();safety();native_policy();return 0; }
'''


def native_tests():
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "xiltimer.h").write_text(textwrap.dedent("""\
        #ifndef XILTIMER_H
        #define XILTIMER_H
        #include <stdint.h>
        typedef uint64_t XTime;
        /* Match the BSP's unparenthesized division. */
        #define COUNTS_PER_SECOND 100000000U / 2U
        void XTime_GetTime(XTime *value);
        #endif
        """), encoding="utf-8")
    harness = OUT / "startup.c"
    harness.write_text(PREAMBLE, encoding="utf-8")
    compiler = shutil.which("gcc") or "C:/msys64/ucrt64/bin/gcc.exe"
    env = dict(os.environ, PATH=str(Path(compiler).parent) + os.pathsep + os.environ.get("PATH", ""))
    executable = OUT / "startup.exe"
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O0",
                    "-I", str(OUT), str(harness), "-o", str(executable)], check=True, env=env)
    subprocess.run([str(executable)], check=True, env=env)


def main_tests():
    main = (ROOT / "ps_sources/frontend/main.c").read_text()
    start = main[main.index("int main(void)"):]
    assert start.index("vtw_service_prepare_boot_start();") < start.index("card_control_mark_cpu0_ready();")
    loop = start[start.index("while (1) {"):]
    gate = loop.index("if (vtw_service_boot_start_pending() != 0U)")
    assert gate < loop.index("config_menu_poll_ethernet(&config_menu);")
    assert gate < loop.index("usb_hid_service_poll();") < loop.index("smartport_service_poll();")
    handoff = loop.index("if (vtw_service_boot_handoff_pending() != 0U)")
    assert gate < handoff < loop.index("config_menu_poll_ethernet(&config_menu);")
    speed_branch = loop[handoff:loop.index("config_menu_poll_ethernet(&config_menu);")]
    assert "vtw_service_poll();" in speed_branch and "continue;" not in speed_branch
    print("PASS CPU0 startup ordering: gate before Apple release, takeover before USB/storage")


if __name__ == "__main__":
    rom_tests()
    native_tests()
    main_tests()
