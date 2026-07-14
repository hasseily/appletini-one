#!/usr/bin/env python3
"""Source-level regression tests for Apple reset driven menu behavior."""

from __future__ import annotations

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
APPLE_TOP_SV = REPO_ROOT / "hdl" / "apple" / "apple_top.sv"
APPLE_BUS_WRAPPER_SV = REPO_ROOT / "hdl" / "apple" / "apple_bus_wrapper.sv"
APPLE_TIMING_GEN_SV = REPO_ROOT / "hdl" / "apple" / "apple_timing_gen.sv"
BOOT_MENU_CARD_SV = REPO_ROOT / "hdl" / "apple" / "boot_menu_card.sv"
BOOT_MENU_SERVICE_C = REPO_ROOT / "ps_sources" / "frontend" / "boot_menu_service.c"
BOOT_MENU_SERVICE_H = REPO_ROOT / "ps_sources" / "frontend" / "boot_menu_service.h"
BOOT_MENU_ROM_PATCH_H = REPO_ROOT / "ps_sources" / "frontend" / "boot_menu_rom_patch.h"
BOOT_MENU_C8_MEM = REPO_ROOT / "hdl" / "apple" / "boot_menu_slot7_c8.mem"
BOOT_MENU_ROM_BUILDER = REPO_ROOT / "scripts" / "build_boot_menu_rom.py"
CONFIG_MENU_C = REPO_ROOT / "ps_sources" / "frontend" / "config_menu.c"
FRONTEND_MAIN_C = REPO_ROOT / "ps_sources" / "frontend" / "main.c"
CARD_CONTROL_REGS_H = REPO_ROOT / "ps_sources" / "frontend" / "card_control_regs.h"
BOOT_MENU_SLOT_A65 = REPO_ROOT / "software" / "boot_menu_slot7.a65"


class TestFailure(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def read_mem(path: Path) -> list[int]:
    return [int(line.strip(), 16)
            for line in path.read_text(encoding="ascii").splitlines()
            if line.strip()]


def parse_c_int_literal(token: str) -> int:
    return int(token.rstrip("UuLl"), 0)


def define_value(source: str, name: str) -> int:
    prefix = f"#define {name}"
    for line in source.splitlines():
        if line.startswith(prefix):
            parts = line.split()
            require(len(parts) >= 3, f"{name} define must have a value")
            return parse_c_int_literal(parts[2])
    raise TestFailure(f"{name} define must be present")


def python_constant_value(source: str, name: str) -> int:
    prefix = f"{name} = "
    for line in source.splitlines():
        if line.startswith(prefix):
            return int(line.split("=", 1)[1].strip(), 0)
    raise TestFailure(f"{name} constant must be present")


def test_pl_exports_apple_reset_sequence() -> None:
    source = read(APPLE_TOP_SV)

    require("CARD_CTRL_REG_APPLE_RESET_STATUS  = 8'h09" in source,
            "card-control register map must expose Apple reset status")
    require("wire apple_reset_assert_pulse = rstn[1] && apple_reset_prev_q && !ab_read.res;" in source,
            "PL must derive a pulse from Apple RES# assertion")
    require("invalidate_shadow_cache" not in source,
            "CTRL+RESET must not invalidate the PSRAM cache")
    require("apple_reset_seq_q <= apple_reset_seq_q + 8'd1;" in source,
            "Apple reset status must increment a sequence counter on reset assertion")
    require("CARD_CTRL_REG_APPLE_RESET_STATUS:  as_client_rdata_q <= {23'h000000, ab_read.res, apple_reset_seq_q};" in source,
            "PS-visible reset status must include RES# level and reset sequence")


def test_apple_bus_res_is_not_address_phase_sampled() -> None:
    source = read(APPLE_BUS_WRAPPER_SV)
    continuous_start = source.find("// Always re-snap timing-insensitive global pins into the read struct.")
    sample_start = source.find("// Sample the bus at the right phase taps")
    require(continuous_start >= 0 and sample_start > continuous_start,
            "Apple bus wrapper must have a continuous global-pin sample block")
    continuous_block = source[continuous_start:sample_start]
    sample_block = source[sample_start:source.find("else if (addr_phase_sss_ready)", sample_start)]

    require("ab_read_r.res           <= res_filtered;" in continuous_block,
            "RES# must track the synchronized pin continuously")
    require("ab_read_r.res" not in sample_block,
            "RES# must not wait for the address-phase sample point")


def test_frontend_tracks_apple_reset_sequence() -> None:
    source = read(FRONTEND_MAIN_C)
    regs = read(CARD_CONTROL_REGS_H)
    boot_service = read(BOOT_MENU_SERVICE_C)
    boot_service_h = read(BOOT_MENU_SERVICE_H)

    require("#define CARD_CTRL_APPLE_RESET_STATUS_REG   CARD_CTRL_REG_ADDR(0x09U)" in regs and
            "#include \"card_control_regs.h\"" in source,
            "frontend must read the shared card-control Apple reset status register")
    require("static uint8_t g_apple_reset_seq_valid = 0U;" in source and
            "static uint8_t g_apple_reset_seq_last = 0U;" in source,
            "frontend must keep a baseline Apple reset sequence")
    require("static uint8_t card_control_read_apple_reset_seq(void)" in source,
            "frontend must provide a reset sequence reader")
    require("card_control_sync_apple_reset_seq();" in source,
            "frontend init must baseline the current reset sequence")
    require("static void ui_handle_apple_reset(ui_state_t *s, config_menu_t *menu)" in source,
            "frontend must have a dedicated Apple reset hook")
    require("ui_handle_apple_reset(&ui, &config_menu);" in source,
            "main loop must poll for Apple reset while the frontend is running")
    require("config_menu_apply_boot_runtime(menu);" in source,
            "Apple reset hook must re-publish boot handoff and boot slot config")
    require("void boot_menu_service_refresh_machine_policy(void);" in boot_service_h and
            "void boot_menu_service_refresh_machine_policy(void)" in boot_service and
            "boot_menu_service_refresh_machine_policy();" in source,
            "Apple reset path must refresh machine aux/RamWorks policy")
    require(source.index("ui_handle_apple_reset(&ui, &config_menu);") <
            source.index("smartport_service_poll();"),
            "Apple reset config reapply must run before SmartPort command service")


def test_boot_menu_uses_finite_default_until_config_is_published() -> None:
    boot_card = read(BOOT_MENU_CARD_SV)
    boot_service = read(BOOT_MENU_SERVICE_C)
    config_menu = read(CONFIG_MENU_C)
    apply_start = config_menu.find("static void config_menu_apply_boot_runtime_internal")
    apply_end = config_menu.find("\nstatic void config_menu_apply_runtime_internal", apply_start)
    require(apply_start >= 0 and apply_end > apply_start,
            "config menu boot runtime apply function must be present")
    apply_body = config_menu[apply_start:apply_end]

    require("DEFAULT_TIMEOUT_TICKS = 32'd399_000_000" in boot_card and
            "timeout_ticks_q <= DEFAULT_TIMEOUT_TICKS;" in boot_card,
            "PL boot menu reset default must preserve automatic A: exit")
    require("#define BOOT_MENU_DEFAULT_TIMEOUT_TICKS    399000000U" in boot_service and
            "REG_WRITE(BOOT_MENU_REG_TIMEOUT_TICKS, BOOT_MENU_DEFAULT_TIMEOUT_TICKS);" in boot_service,
            "frontend boot service init must preserve the automatic A: exit default")
    require(apply_body.index("DISK2_CONTROL_SLOT") <
            apply_body.index("menu->platform.set_boot_handoff") <
            apply_body.index("menu->platform.set_boot_timeout_ticks"),
            "runtime apply must enable Disk II before publishing the boot handoff")
    require("config_menu_apply_smartport_paths(menu);" in config_menu,
            "config bind must publish SmartPort image paths before SmartPort service init")
    bind_start = config_menu.find("void config_menu_bind_platform")
    bind_end = config_menu.find("\nuint8_t config_menu_is_active", bind_start)
    require(bind_start >= 0 and bind_end > bind_start,
            "config menu bind function must be present")
    bind_body = config_menu[bind_start:bind_end]
    require(bind_body.index("config_menu_apply_boot_runtime_internal(menu, 1U);") <
            bind_body.index("config_menu_apply_smartport_paths(menu);"),
            "config bind must publish boot handoff before preloading SmartPort paths")


def test_frontend_publishes_boot_config_before_slow_services() -> None:
    source = read(FRONTEND_MAIN_C)

    bind_pos = source.find("config_menu_bind_platform(&config_menu, &menu_platform);")
    gic_pos = source.find("if (gic_init() != 0)")
    storage_pos = source.find("if (usb_storage_service_init() != 0)")
    disk2_pos = source.find("(void)disk2_service_init(UART0_BASE);")
    smartport_pos = source.find("int smartport_rc = smartport_service_init(UART0_BASE);")
    runtime_pos = source.find("config_menu_apply_runtime(&config_menu);")
    assets_pos = source.find("config_menu_apply_startup_assets(&config_menu);")
    refresh_pos = source.find("int smartport_reload_rc =")
    ready_pos = source.find("card_control_mark_cpu0_ready();")

    require(bind_pos >= 0 and gic_pos >= 0 and storage_pos >= 0 and
            disk2_pos >= 0 and smartport_pos >= 0,
            "frontend startup markers must be present")
    require(bind_pos < gic_pos and bind_pos < storage_pos and
            bind_pos < disk2_pos and bind_pos < smartport_pos,
            "frontend must publish loaded boot config and SmartPort paths before slow service init")
    require(gic_pos < storage_pos < disk2_pos < smartport_pos,
            "frontend must bring up SD storage before disk services open media")
    require(runtime_pos >= 0 and assets_pos > runtime_pos,
            "full runtime apply must run before startup-only assets")
    require(assets_pos < refresh_pos < ready_pos,
            "SmartPort media must be reopened after startup SD asset loads and before Apple release")


def test_boot_menu_close_reapplies_visible_config() -> None:
    source = read(FRONTEND_MAIN_C)

    require("g_usb_menu_owned == 0U &&\n"
            "        ui_config_menu_has_close_consumer(menu) == 0U &&\n"
            "        ui_input_requests_menu_close(in) != 0U) {\n"
            "        boot_menu_service_request_rom_close();" in source,
            "Apple-owned parent ESC must ask the boot ROM to close instead of hiding only the PS menu")
    require("boot_close_child_suppress" not in source and
            "UI_BOOT_CLOSE_CHILD_SUPPRESS" not in source,
            "child ESC must not rely on suppressing a ROM close after the ROM has already released the Apple")
    require("case BOOT_MENU_EVENT_CLOSE:\n"
            "                    config_menu_apply_runtime(&config_menu);\n"
            "                    ui_set_boot_menu_visible(&ui, &config_menu, 0U);" in source,
            "ROM close events mean the Apple is closing the parent menu, so PS must publish config and hide it")


def test_menu_sd_access_refreshes_smartport_media_before_handoff() -> None:
    source = read(CONFIG_MENU_C)

    require("#define CONFIG_SMARTPORT_ALL_DEVICES 0xFFU" in source,
            "config menu needs the SmartPort all-devices reset selector")
    require("void config_menu_refresh_smartport_media_after_menu_sd(config_menu_t *menu)" in source and
            "reset_smartport_media(menu->platform.ctx," in source and
            "CONFIG_SMARTPORT_ALL_DEVICES);" in source,
            "menu-time SD access must be able to refresh SmartPort media")
    # Assert the shared post-I/O invariant across menu-time SD access sites.
    require("config_menu_set_status(menu, 0U, success_status);\n"
            "    }\n"
            "    config_menu_refresh_smartport_media_after_menu_sd(menu);" in source,
            "config save must leave SmartPort mounted after FatFs remounts")
    require(source.count("config_menu_refresh_smartport_media_after_menu_sd(menu);") >= 3,
            "browser/bezel menu-time SD access sites must refresh SmartPort media")

    active_start = source.find("void config_menu_set_active")
    active_end = source.find("void config_menu_toggle", active_start)
    require(active_start >= 0 and active_end > active_start,
            "config menu activation functions must be present")
    active = source[active_start:active_end]
    require("config_menu_retry_settings_if_needed(menu);" in active and
            "config_menu_refresh_smartport_media_after_menu_sd(menu);" in active and
            active.index("config_menu_retry_settings_if_needed(menu);") <
            active.index("config_menu_refresh_smartport_media_after_menu_sd(menu);"),
            "opening the menu must reclaim FatFs for SmartPort after all entry-time SD access")

    toggle_end = source.find("void config_menu_set_usb_bindings_editable", active_end)
    toggle = source[active_end:toggle_end]
    require("config_menu_set_active(menu," in toggle,
            "alternate menu toggles must use the same SmartPort recovery path")

def test_boot_handoff_manufactures_target_slot_stack_frame() -> None:
    rom = read(BOOT_MENU_SLOT_A65)
    hdl = read(BOOT_MENU_CARD_SV)

    require("SLOTRET_HI = $CA01      ; $Cn - 1, for the manufactured-RTS handoff" in rom and
            "SLOT_CN   = $CA02       ; raw $Cn, pushed at handoff" in rom,
            "boot ROM must keep the manufactured-RTS target bytes in scratch RAM")
    require("lda #>(SLOOP - 1)\n"
            "          pha\n"
            "          lda #<(SLOOP - 1)\n"
            "          pha\n"
            "          lda SLOT_CN\n"
            "          pha\n"
            "          lda SLOTRET_HI\n"
            "          pha\n"
            "          lda #$ff\n"
            "          pha\n"
            "          rts" in rom,
            "boot handoff must enter the target ROM through a manufactured RTS frame")
    require("if (handoff_pending_q && c8_ram_offset == 4'h2) begin\n"
            "                    ab_write_d.wr_data = {4'hC, 1'b0, boot_target_slot};\n"
            "                end else if (handoff_pending_q && c8_ram_offset == 4'h1) begin\n"
            "                    ab_write_d.wr_data = {4'hC, 1'b0, boot_target_slot} - 8'h01;" in hdl,
            "PL scratch RAM must expose only the dynamic $Cn and $Cn-1 handoff bytes")


def test_disk2_slot6_uses_boot_handoff_gate() -> None:
    source = read(APPLE_TOP_SV)

    require(".ab_read(gate_ab(ab_read, card_slot6_enable && disk2_active))" in source and
            ".slot_assign(3'h6)" in source,
            "Disk II slot 6 bus visibility must remain under boot-menu handoff control")


def test_boot_debug_exposes_computed_apple_status() -> None:
    hdl = read(BOOT_MENU_CARD_SV)
    service_h = read(BOOT_MENU_SERVICE_H)
    service_c = read(BOOT_MENU_SERVICE_C)
    frontend_main = read(FRONTEND_MAIN_C)

    require("BM_REG_APPLE_STATUS  = 8'h05" in hdl and
            "BM_REG_APPLE_STATUS:  as_client.rdata = {24'h000000, apple_status_byte};" in hdl,
            "boot-menu PL must expose the exact Apple-side status byte to PS diagnostics")
    require("apple_status_byte;" in service_h and
            "void boot_menu_service_debug_snapshot(boot_menu_debug_snapshot_t *snapshot);" in service_h,
            "boot-menu service must publish a debug snapshot API")
    require("BOOT_MENU_REG_APPLE_STATUS" in service_c and
            "snapshot->apple_status_byte = REG_READ(BOOT_MENU_REG_APPLE_STATUS) & 0xFFU;" in service_c,
            "boot-menu service must read the computed Apple status register")
    require("boot_debug_log_snapshot(\"after config bind\");" in frontend_main and
            "boot_debug_log_snapshot(\"after runtime apply\");" in frontend_main and
            "boot_debug_log_snapshot(\"pre apple release\");" in frontend_main,
            "frontend startup must log boot handoff state before releasing Apple reset")


def test_boot_config_writes_are_accepted_during_apple_reset() -> None:
    hdl = read(BOOT_MENU_CARD_SV)
    reset_pos = hdl.find("if (!ab_read.res) begin")
    config_pos = hdl.find("Boot configuration must be accepted while the PS holds", reset_pos)
    config_end = hdl.find("\n        end\n    end", config_pos)
    require(reset_pos >= 0 and config_pos > reset_pos and config_end > config_pos,
            "boot-menu PL must handle PS boot-config writes after the Apple reset branch")
    config_block = hdl[config_pos:config_end]

    require("if (as_client.awvalid) begin" in config_block and
            "BM_REG_TIMEOUT_TICKS" in config_block and
            "BM_REG_HANDOFF_MODE" in config_block,
            "timeout and handoff writes must be accepted even while Apple reset is asserted")
    require("as_client.awvalid && (as_common.awaddr == BM_REG_CONTROL)" in hdl,
            "runtime menu control writes should remain gated to the active Apple boot window")


def test_aux_probe_drops_provided_ram_and_rejects_floating_echo() -> None:
    rom = read(BOOT_MENU_SLOT_A65)
    hdl = read(BOOT_MENU_CARD_SV)
    apple_top = read(APPLE_TOP_SV)
    service_c = read(BOOT_MENU_SERVICE_C)

    require("aux_probe_pulse = apple_cmd_write_hit && (ab_read.data == 8'h26);" in hdl,
            "boot-menu PL must pulse when the ROM requests a physical aux probe")
    require("if (bm_aux_probe_pulse) begin\n"
            "                aux_provide_en_q <= 1'b0;\n"
            "            end" in apple_top,
            "Apple top must drop Appletini aux RAM before the ROM probes the raw aux slot")

    start = rom.find("mid_aux_probe:")
    end = rom.find("mid_aux_no:", start)
    require(start >= 0 and end > start,
            "boot ROM must keep the IIe aux-card probe path")
    probe = rom[start:end]
    require("jsr" not in probe.lower(),
            "aux probe must not use JSR while ALTZP may move the stack to absent aux RAM")
    require("          lda #CMD_AUX_PROBE\n"
            "          ldy SLOT16\n"
            "          sta BM_IO,y\n"
            "          sta ALTZP_ON" in probe,
            "aux probe must force-drop Appletini aux before enabling ALTZP")
    require("          lda #$AA\n"
            "          sta $00\n"
            "          lda #$55\n"
            "          sta $01\n"
            "          lda #$33\n"
            "          sta $00\n"
            "          lda $01\n"
            "          cmp #$55" in probe,
            "aux probe must prove one aux byte survives an intervening write to another")
    require("          lda $00\n"
            "          cmp #$33\n"
            "          bne mid_aux_no" in probe,
            "aux probe must reject simple floating-bus echo of the last write")
    require("g_aux_provide_applied = 0xFFU;" in service_c and
            "REG_WRITE(CARD_CTRL_AUX_PROBE_REG, 1U);" in service_c,
            "frontend must consume the probe report and rewrite aux policy after the PL force-drop")


def test_boot_rom_pal_patch_is_firmware_applied_before_release() -> None:
    hdl = read(BOOT_MENU_CARD_SV)
    apple_top = read(APPLE_TOP_SV)
    service_c = read(BOOT_MENU_SERVICE_C)
    service_h = read(BOOT_MENU_SERVICE_H)
    patch_h = read(BOOT_MENU_ROM_PATCH_H)
    frontend_main = read(FRONTEND_MAIN_C)

    require("input  logic                    apple_video_mode_valid," in hdl and
            "input  logic                    apple_video_mode_50hz," in hdl and
            "BM_REG_APPLE_TIMING  = 8'h06" in hdl and
            "BM_REG_C8_PATCH      = 8'h07" in hdl,
            "boot-menu PL must expose timing status and a C8 ROM patch register")
    require(".apple_video_mode_valid(video_mode_50hz_valid_q)" in apple_top and
            ".apple_video_mode_50hz(video_mode_50hz)" in apple_top,
            "apple_top must pass the measured PAL/NTSC mode into the boot-menu PL")
    require("apple_timing_word = {\n"
            "            30'd0,\n"
            "            apple_video_mode_50hz,\n"
            "            apple_video_mode_valid\n"
            "        };" in hdl and
            "slot_c8_rom[as_common.wdata[15:8]] <= as_common.wdata[7:0];" in hdl,
            "boot-menu PL must report {valid,50Hz} and accept firmware C8 byte patches")

    require("#define BOOT_MENU_C8_PATCH_DELAY_X_OFFSET" in patch_h and
            "#define BOOT_MENU_C8_PATCH_DELAY_Y_OFFSET" in patch_h and
            "#define BOOT_MENU_C8_PATCH_IIPLUS_VAPOR_X_OFFSET" in patch_h and
            "#define BOOT_MENU_C8_PATCH_IIPLUS_VAPOR_Y_OFFSET" in patch_h and
            "#define BOOT_MENU_C8_DELAY_PAL_X                0x54U" in patch_h and
            "#define BOOT_MENU_C8_DELAY_PAL_Y                0x2FU" in patch_h and
            "#define BOOT_MENU_C8_IIPLUS_VAPOR_X" in patch_h and
            "#define BOOT_MENU_C8_IIPLUS_VAPOR_Y" in patch_h,
            "generated boot-menu patch header must publish the PAL and II+ vapor delay constants")
    require("#include \"boot_menu_rom_patch.h\"" in service_c and
            "BOOT_MENU_REG_APPLE_TIMING" in service_c and
            "BOOT_MENU_REG_C8_PATCH" in service_c and
            "void boot_menu_service_apply_video_rom_patch(void)" in service_h,
            "boot-menu service must own the firmware ROM patch API")
    require("boot_menu_service_set_vbl_delay((uint8_t)BOOT_MENU_C8_DELAY_PAL_X,\n"
            "                                        (uint8_t)BOOT_MENU_C8_DELAY_PAL_Y);" in service_c and
            "boot_menu_service_set_vbl_delay((uint8_t)BOOT_MENU_C8_DELAY_NTSC_X,\n"
            "                                        (uint8_t)BOOT_MENU_C8_DELAY_NTSC_Y);" in service_c,
            "firmware must patch PAL constants and restore NTSC constants when appropriate")
    require("boot_menu_service_set_iiplus_vapor_delay(\n"
            "        (uint8_t)BOOT_MENU_C8_IIPLUS_VAPOR_X,\n"
            "        (uint8_t)BOOT_MENU_C8_IIPLUS_VAPOR_Y);" in service_c,
            "firmware must patch the II+ vaporlock delay constants before release")
    require(frontend_main.find("boot_menu_service_init();") >= 0 and
            frontend_main.find("boot_menu_service_init();") < frontend_main.find("card_control_mark_cpu0_ready();"),
            "boot-menu firmware patch must run before CPU0 releases Apple reset")


def test_vbl_lock_matches_boot_rom_timing() -> None:
    rom = read(BOOT_MENU_SLOT_A65)
    timing = read(APPLE_TIMING_GEN_SV)

    require("localparam logic [6:0] VBL_LOCK_CYCLE = 7'd15;" in timing,
            "VBL lock seed must match the current slot-independent BM_IO,Y write latency")
    require("The ROM writes BM_CMD fourteen 6502 cycles after the calibrated VBL" in timing and
            "next timing-generator tick as cycle 15" in timing,
            "VBL lock comment must document the current post-edge seed calculation")

    sync_start = rom.find("vbl_mousecard_sync:")
    sync_end = rom.find("vbl_emit:", sync_start)
    require(sync_start >= 0 and sync_end > sync_start,
            "boot ROM must keep the VBL sync helper")
    sync = rom[sync_start:sync_end]
    require("vbl_mousecard_sync:\n"
            "          lda VBL_MODE\n"
            "          bne vbl_iiplus_sync\n"
            "          jsr wait_vbl_edge" in sync,
            "RDVBLBAR sync must dispatch to the II+ vaporlock path only when requested")
    require("bne wait_vbl_start" not in sync,
            "RDVBLBAR synchronization must not loop back to wait_vbl_start")

    delay_start = rom.find("delay_frame_back:")
    require(delay_start >= 0, "boot ROM must keep the patchable frame backoff delay")
    delay_end = len(rom)
    delay = rom[delay_start:delay_end]

    require("          txa\n          pha\n" in delay and
            "          pla\n          tax\n          rts" in delay,
            "VBL backoff delay must include the 11-cycle X save/restore padding")
    require("delay_frame_x_opcode:\n"
            "          ldx #$1d" in delay and
            "delay_frame_y_opcode:\n"
            "          ldy #$74" in delay,
            "boot ROM must label the generated delay immediates for firmware patching")
    require("Delay body defaults to 17014 cycles" in rom and
            "17029 cycles" in rom and
            "body 20264 cycles" in rom and
            "20279-cycle PAL probe spacing" in rom,
            "VBL backoff comments must document both NTSC defaults and PAL patch timing")


def test_iiplus_vaporlock_path_is_patchable_and_fits() -> None:
    rom = read(BOOT_MENU_SLOT_A65)
    service_c = read(BOOT_MENU_SERVICE_C)
    patch_h = read(BOOT_MENU_ROM_PATCH_H)
    builder = read(BOOT_MENU_ROM_BUILDER)
    c8 = read_mem(BOOT_MENU_C8_MEM)

    require("VBL_MODE  = $CA04" in rom and
            "VBL_MODE_IIPLUS = $01" in rom,
            "boot ROM must keep a scratch mode bit for the II/II+ VBL path")
    require("VAPOR_READ = TEXT" in rom and
            "VAPOR_MARK = $BA" in rom and
            "VAPOR_SCAN_COUNT = $08" in rom,
            "II+ vaporlock constants must use the prompt-row marker")

    wait_start = rom.find("wait_vbl_start:")
    edge_start = rom.find("wait_vbl_edge:", wait_start)
    require(wait_start >= 0 and edge_start > wait_start,
            "boot ROM must keep wait_vbl_start before wait_vbl_edge")
    wait = rom[wait_start:edge_start]
    require("wait_vbl_start:\n"
            "          lda VBL_MODE\n"
            "          bne wait_vbl_iiplus\n"
            "          jsr wait_vbl_edge" in wait,
            "wait_vbl_start must dispatch II/II+ machines away from RDVBLBAR")

    vapor_start = rom.find("vbl_iiplus_sync:")
    machine_id_start = rom.find("machine_id_report:", vapor_start)
    require(vapor_start >= 0 and machine_id_start > vapor_start,
            "boot ROM must keep the II+ vaporlock helper before machine-id reporting")
    vapor = rom[vapor_start:machine_id_start]
    require("vbl_iiplus_sync:\n"
            "          jsr iiplus_vapor_edge\n"
            "wait_vbl_iiplus:\n"
            "          jsr iiplus_vapor_edge\n"
            "          jmp vbl_emit" in vapor,
            "initial II+ sync must throw away one vaporlock edge and emit on the next")
    require("iiplus_vapor_edge:\n"
            "          ldx #VAPOR_SCAN_COUNT\n"
            "iiplus_vapor_wait:\n"
            "          lda VAPOR_READ\n"
            "          cmp #VAPOR_MARK\n"
            "          bne iiplus_vapor_wait\n"
            "iiplus_vapor_clear:\n"
            "          lda VAPOR_READ\n"
            "          cmp #VAPOR_MARK\n"
            "          beq iiplus_vapor_clear\n"
            "          dex\n"
            "          bne iiplus_vapor_wait" in vapor,
            "II+ vaporlock must count all eight prompt-marker scanline appearances")
    require("iiplus_vapor_x_opcode:\n"
            "          ldx #$" in vapor and
            "iiplus_vapor_outer:\n"
            "iiplus_vapor_y_opcode:\n"
            "          ldy #$" in vapor,
            "II+ vaporlock delay immediates must be labeled for the tunable seed")

    machine = rom[machine_id_start:]
    require("machine_id_report:\n"
            "          lda #$00\n"
            "          sta VBL_MODE" in machine,
            "machine-id report must default to the RDVBLBAR path")
    require("lda #VBL_MODE_IIPLUS\n"
            "          sta VBL_MODE\n"
            "          lda #CMD_MID_IIFAMILY" in machine,
            "non-IIe machines must select the II/II+ vaporlock path")

    require("static void boot_menu_service_set_iiplus_vapor_delay(uint8_t x_count,\n"
            "                                                     uint8_t y_count)" in service_c and
            "BOOT_MENU_C8_PATCH_IIPLUS_VAPOR_X_OFFSET" in service_c and
            "BOOT_MENU_C8_PATCH_IIPLUS_VAPOR_Y_OFFSET" in service_c,
            "frontend must expose a patch helper for the II+ vaporlock delay bytes")

    x_offset = define_value(patch_h, "BOOT_MENU_C8_PATCH_IIPLUS_VAPOR_X_OFFSET")
    y_offset = define_value(patch_h, "BOOT_MENU_C8_PATCH_IIPLUS_VAPOR_Y_OFFSET")
    x_count = define_value(patch_h, "BOOT_MENU_C8_IIPLUS_VAPOR_X")
    y_count = define_value(patch_h, "BOOT_MENU_C8_IIPLUS_VAPOR_Y")
    require(0 < x_offset < 256 and 0 < y_offset < 256,
            "generated II+ vaporlock patch offsets must stay inside the C8 page")
    require(c8[x_offset - 1] == 0xA2 and c8[y_offset - 1] == 0xA0,
            "generated II+ vaporlock patch offsets must point at LDX/LDY immediate bytes")
    require(x_count == python_constant_value(builder, "IIPLUS_VAPOR_DELAY_X") and
            y_count == python_constant_value(builder, "IIPLUS_VAPOR_DELAY_Y"),
            "generated II+ vaporlock defaults must come from the build-script tuning knobs")
    require(0 <= x_count <= 0xFF and 0 <= y_count <= 0xFF,
            "II+ vaporlock delay counts must be single-byte patch values")

    require(len(c8) == 256, "C8 expansion ROM image must remain exactly one page")
    last_used = max((index for index, byte in enumerate(c8) if byte != 0xEA),
                    default=-1)
    require(last_used < 0xE8,
            "C8 expansion ROM must retain at least 24 bytes of slack after the II+ path")


def test_boot_prompt_shows_appletini_name() -> None:
    rom = read(BOOT_MENU_SLOT_A65)

    require("PROMPT_LEN = $0B" in rom and
            "          jsr show_prompt" in rom,
            "boot ROM must draw the full A:APPLETINI prompt through a helper")
    require("show_prompt:\n"
            "          ldy #PROMPT_LEN - 1\n"
            "show_prompt_loop:\n"
            "          lda prompt_text,y\n"
            "          sta PROMPT,y" in rom,
            "boot ROM must render the prompt with an indexed loop")
    require("clear_prompt:\n"
            "          ldy #PROMPT_LEN - 1\n"
            "          lda #$a0\n"
            "clear_prompt_loop:\n"
            "          sta PROMPT,y" in rom,
            "boot ROM must erase the same full prompt span")
    require("prompt_text:\n"
            "          .byte $c1,$ba,$c1,$d0,$d0,$cc,$c5,$d4,$c9,$ce,$c9" in rom,
            "boot ROM prompt text must be A:APPLETINI in Apple text codes")


def test_apple_reset_closes_active_menu() -> None:
    source = read(FRONTEND_MAIN_C)

    require("static void ui_handle_apple_reset(ui_state_t *s, config_menu_t *menu)" in source,
            "frontend must have a dedicated Apple reset hook")
    require("if (reset_seq == g_apple_reset_seq_last) {\n"
            "        return;\n"
            "    }" in source,
            "reset hook must only act on sequence changes")
    require("if (config_menu_is_active(menu)) {\n"
            "        g_usb_menu_owned = 0U;\n"
            "        ui_set_boot_menu_visible(s, menu, 0U);\n"
            "    }" in source,
            "Apple reset must close every visible menu")


def test_usb_close_request_exits_apple_owned_boot_menu_in_rom() -> None:
    rom = read(BOOT_MENU_SLOT_A65)
    hdl = read(BOOT_MENU_CARD_SV)
    service_c = read(BOOT_MENU_SERVICE_C)
    service_h = read(BOOT_MENU_SERVICE_H)
    frontend_main = read(FRONTEND_MAIN_C)

    require("STATUS_PS_CLOSE_REQUEST  = $20" in rom,
            "boot ROM must assign an Apple-visible status bit for PS close requests")
    require("menu_loop:\n"
            "          jsr wait_vbl_start\n"
            "          jsr bm_status\n"
            "          and #STATUS_PS_CLOSE_REQUEST\n"
            "          bne close_menu" in rom,
            "boot ROM menu loop must poll the PS close-request bit and use close_menu")
    require("          jsr bm_key\n"
            "          jmp menu_loop" in rom and
            "          cmp #$1b\n"
            "          bne menu_loop" not in rom,
            "boot ROM must forward ESC to the PS and wait for an explicit PS close request")
    require("logic ps_close_requested_q;" in hdl and
            "apple_status_byte = {\n"
            "            2'd0,\n"
            "            ps_close_requested_q,\n"
            "            handoff_disk2" in hdl,
            "boot-menu PL must expose the PS close request on Apple status bit 5")
    require("if (as_common.wdata[7]) ps_close_requested_q <= 1'b1;" in hdl and
            "ps_close_requested_q <= 1'b0;" in hdl,
            "boot-menu PL must latch and clear the PS close request")
    require("#define BOOT_MENU_CONTROL_REQUEST_ROM_CLOSE (1U << 7)" in service_c and
            "void boot_menu_service_request_rom_close(void)" in service_h and
            "REG_WRITE(BOOT_MENU_REG_CONTROL, BOOT_MENU_CONTROL_REQUEST_ROM_CLOSE);" in service_c,
            "boot-menu service must provide a PS control helper for ROM close")
    require("case USB_HID_MENU_ACTION_CLOSE:\n"
            "        if (config_menu_is_active(menu)) {\n"
            "            if (ui_config_menu_has_close_consumer(menu) != 0U) {\n"
            "                ui_close_config_menu_child(s, menu);\n"
            "                return;\n"
            "            }\n"
            "            if (g_usb_menu_owned == 0U) {\n"
            "                boot_menu_service_request_rom_close();" in frontend_main,
            "USB close must stop child UI first, then ask the ROM to close Apple-owned boot menus")


TESTS = [
    test_pl_exports_apple_reset_sequence,
    test_apple_bus_res_is_not_address_phase_sampled,
    test_frontend_tracks_apple_reset_sequence,
    test_boot_menu_uses_finite_default_until_config_is_published,
    test_frontend_publishes_boot_config_before_slow_services,
    test_boot_menu_close_reapplies_visible_config,
    test_menu_sd_access_refreshes_smartport_media_before_handoff,
    test_boot_handoff_manufactures_target_slot_stack_frame,
    test_disk2_slot6_uses_boot_handoff_gate,
    test_boot_debug_exposes_computed_apple_status,
    test_boot_config_writes_are_accepted_during_apple_reset,
    test_aux_probe_drops_provided_ram_and_rejects_floating_echo,
    test_boot_rom_pal_patch_is_firmware_applied_before_release,
    test_vbl_lock_matches_boot_rom_timing,
    test_iiplus_vaporlock_path_is_patchable_and_fits,
    test_boot_prompt_shows_appletini_name,
    test_apple_reset_closes_active_menu,
    test_usb_close_request_exits_apple_owned_boot_menu_in_rom,
]


def main() -> int:
    failures = []
    for test in TESTS:
        try:
            test()
        except TestFailure as exc:
            failures.append((test.__name__, str(exc)))
            print(f"FAIL {test.__name__}: {exc}")
        else:
            print(f"PASS {test.__name__}")
    if failures:
        print(f"{len(TESTS) - len(failures)} of {len(TESTS)} boot menu reset tests passed; "
              f"{len(failures)} failed")
        return 1
    print(f"{len(TESTS)} boot menu reset tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
