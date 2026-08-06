#!/usr/bin/env python3
"""Regression tests for the SmartPort PS-side block-device service.

These tests intentionally check source-level contracts for the embedded C
implementation. They can run without Vitis or hardware:

    python scripts/test_smartport_service.py
"""

from __future__ import annotations

import re
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SMARTPORT_C = REPO_ROOT / "ps_sources" / "frontend" / "smartport_service.c"
SMARTPORT_H = REPO_ROOT / "ps_sources" / "frontend" / "smartport_service.h"
SMARTPORT_SV = REPO_ROOT / "hdl" / "apple" / "smartport_card.sv"
VTW_CORE_SV = REPO_ROOT / "hdl" / "apple" / "vtw_core_top.sv"
APPLE_TOP_SV = REPO_ROOT / "hdl" / "apple" / "apple_top.sv"
APPLE_DMA_SV = REPO_ROOT / "hdl" / "apple" / "apple_dma_engine.sv"
PS_DMA_SV = REPO_ROOT / "hdl" / "apple" / "ps_dma_command.sv"
CARD_REGS_H = REPO_ROOT / "ps_sources" / "frontend" / "card_control_regs.h"
FRONTEND_MAIN_C = REPO_ROOT / "ps_sources" / "frontend" / "main.c"
COMPOSITOR_C = REPO_ROOT / "ps_sources" / "frontend" / "compositor.c"
SMARTPORT_ASM = REPO_ROOT / "6502_SMARTPORT.S"
PSDMA_C = REPO_ROOT / "ps_sources" / "lib" / "psdma.c"
PSDMA_H = REPO_ROOT / "ps_sources" / "lib" / "psdma.h"
SHADOW_HOST_SV = REPO_ROOT / "hdl" / "apple" / "vtw_shadow_host_port.sv"


class TestFailure(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def test_declares_eight_independent_smartport_devices() -> None:
    source = read(SMARTPORT_C)
    header = read(SMARTPORT_H)

    require("#define SP_MAX_DEVICES          8U" in source,
            "SmartPort service must define an 8-device table")
    require("} sp_device_t;" in source and
            "static sp_device_t g_devices[SP_MAX_DEVICES];" in source,
            "SmartPort service must store per-device media state")
    for field in (
        "FIL      image_file;",
        "uint8_t  image_open;",
        "uint8_t  read_only;",
        "uint32_t image_data_offset;",
        "uint32_t image_blocks;",
        "char     image_path[SP_IMAGE_PATH_MAX];",
    ):
        require(field in source, f"sp_device_t missing field: {field}")

    require("int smartport_service_set_image_path(uint8_t device, const char *path);" in header,
            "image-path setter must select a SmartPort unit/device")
    require("const char *smartport_service_get_image_path(uint8_t device);" in header,
            "image-path getter must select a SmartPort unit/device")
    require("int smartport_service_reset_media(uint8_t device);" in header,
            "media reset must support one device or all devices")
    require("Device numbers are SmartPort units 1..8" in header,
            "public service API must use one-based SmartPort device numbers")


def test_uses_block_cache_instead_of_full_image_slurp() -> None:
    source = read(SMARTPORT_C)

    require("#define SP_CACHE_BLOCK_COUNT" in source,
            "SmartPort service must declare a bounded block cache")
    require("} sp_cache_entry_t;" in source and
            "static sp_cache_entry_t g_cache[SP_CACHE_BLOCK_COUNT];" in source,
            "SmartPort service must track cached block ownership")
    require("static int sp_cache_get_block(sp_device_t *dev," in source,
            "SmartPort reads/writes must go through a synchronous cache miss path")
    require("static void sp_cache_invalidate_device(uint8_t device_index)" in source,
            "changing media must invalidate only that device's cache entries")

    forbidden = (
        "load_image_into_ddr",
        "#define SP_IMAGE_DDR_SIZE       (32U * 1024U * 1024U)",
        "g_image_buf + block_num * SP_BLOCK_SIZE",
        "for (mb = 0U; mb < SP_IMAGE_DDR_SIZE / 0x100000U; ++mb)",
    )
    for needle in forbidden:
        require(needle not in source,
                f"SmartPort service must not preload complete images: {needle}")


def test_units_select_device_table_entries() -> None:
    source = read(SMARTPORT_C)

    require("static sp_device_t *device_for_sp_unit(uint8_t unit)" in source,
            "SmartPort unit mapping helper missing")
    require("if (unit == 0U || unit > SP_MAX_DEVICES)" in source,
            "SmartPort units 1..8 must be accepted and bounded")
    require("return &g_devices[unit - 1U];" in source,
            "SmartPort unit N must map to device table entry N-1")

    require("static sp_device_t *device_for_blk_unit(uint8_t unit)" in source,
            "ProDOS block-command unit mapping helper missing")
    require("return &g_devices[drive];" in source,
            "ProDOS block-command drive bit must map drive 0/1 to SP1/SP2")

    require("dev = device_for_blk_unit(unit);" in source and
            "dev = device_for_sp_unit(unit);" in source,
            "command execution must select a device from the command unit")


def test_status_reports_present_devices_and_per_device_geometry() -> None:
    source = read(SMARTPORT_C)

    require("static uint8_t smartport_present_count(void)" in source,
            "controller status must count mounted devices")
    require("g_scratch[0] = smartport_present_count();" in source,
            "controller status byte must report mounted SmartPort device count")
    require("static uint16_t build_sp_status(sp_device_t *dev," in source,
            "device status must use the selected device state")
    require("uint32_t blocks = (dev != NULL) ? dev->image_blocks : 0U;" in source,
            "device status must report selected device block count")
    require("uint8_t general = dev->read_only ? 0xFCU : 0xF8U;" in source,
            "device status must report selected device write protection")


def test_command_path_uses_cache_for_reads_and_writes() -> None:
    source = read(SMARTPORT_C)

    require("return (uint8_t *)(uintptr_t)cache_addr;" in source,
            "cache/RAM disk address translation must use absolute DDR pointers")
    require("sp_cache_get_block(dev, block_num, 0U, accelerated," in source,
            "READBLOCK must load/cache the requested device block")
    require("sp_cache_get_block(dev, block_num, 1U, 0U, &cache_addr)" in source,
            "WRITEBLOCK must load/cache the requested device block before updating media")
    require(source.count(
                "sp_response_append_buf(\n"
                "                            cache_ptr_from_addr(cache_addr), SP_BLOCK_SIZE);") == 2,
            "both READBLOCK families must retain the full FIFO fallback")
    require("sp_response_append_buf(g_scratch, length);" in source,
            "SmartPort STATUS payload must stream through the OUT FIFO")
    require("sp_response_commit(accelerated);\n"
            "    smartport_note_activity" in source,
            "each command must publish one complete response before READY")
    require("sp_write_block_to_image(dev, block_num," in source and
            "memcpy(cache_ptr_from_addr(cache_addr), data, SP_BLOCK_SIZE);" in source,
            "WRITEBLOCK must update the selected cached block before persisting media")
    require("f_write(&dev->image_file," in source and
            "data, SP_BLOCK_SIZE, &bw)" in source,
            "WRITEBLOCK must persist the Apple-supplied block to the selected image file")


def test_vtw_uses_readahead_and_word_wide_response_buffer() -> None:
    source = read(SMARTPORT_C)
    gateware = read(SMARTPORT_SV)

    require("#define SP_VTW_READAHEAD_BLOCKS 8U" in source and
            "if (for_write == 0U && allow_readahead != 0U)" in source and
            "f_read(&dev->image_file, dst, (UINT)fill_bytes, &br)" in source,
            "vTW cache misses must fill one bounded multi-block read-ahead line")
    require("const uint8_t accelerated =\n"
            "        ((hw_status & SP_ST_EXEC_VTW) != 0U) ? 1U : 0U;" in source,
            "the PS must select acceleration from the command's latched origin")
    require(source.count("sp_cache_get_block(dev, block_num, 0U, accelerated,") == 2,
            "both ProDOS and SmartPort reads must allow vTW read-ahead")
    require("sp_cache_get_block(dev, block_num, 0U, 0U, &cache_addr)" in source,
            "non-command/debug reads must retain the single-block cache path")
    require("#define SP_R_OUT_PUSH4" in source and
            "static void sp_response_commit(uint8_t accelerated)" in source and
            "REG_WRITE(SP_R_OUT_PUSH4, word);" in source,
            "only accelerated response buffers may use packed output words")
    require("g_response[SP_RESPONSE_MAX] __attribute__((aligned(4)))" in source and
            "sp_response_reset();" in source and
            "sp_response_commit(accelerated);" in source,
            "the service must build the full aligned response before publishing it")
    require("sp_push4_wait_idle" not in source and
            "SP_ST_PUSH4_BUSY" not in source,
            "the PS path must not retain serializer pacing or busy polling")

    require("SP_REG_OUT_PUSH4" in gateware and
            "exec_vtw_q     <= vtw_ctrl_write;" in gateware and
            "logic [31:0] out_fifo" in gateware and
            "out_mem_we    = 4'b1111;" in gateware and
            "out_head = out_word_q[8*out_rd_q[1:0] +: 8]" in gateware,
            "gateware must store whole words and expose them byte-wise to the Apple")
    require("out_pack" not in gateware and
            "exec_vtw_q, 1'b0, ready_q" in gateware,
            "the serializer must be gone and STATUS bit 30 must stay free")
    require("#define SP_R_IN_HEAD4" in source and
            "#define SP_CTL_POP_IN4" in source and
            "REG_READ(SP_R_IN_HEAD4)" in source and
            "sp_ctl(SP_CTL_POP_IN4);" in source and
            "logic [31:0] in_fifo" in gateware and
            "SP_REG_IN_HEAD4" in gateware and
            "axi_pop_in4_accept" in gateware,
            "accelerated writes must drain aligned IN words without changing "
            "the Apple byte protocol")


def test_vtw_direct_read_is_complete_and_fail_closed() -> None:
    source = read(SMARTPORT_C)
    gateware = read(SMARTPORT_SV)
    vtw_core = read(VTW_CORE_SV)
    apple_top = read(APPLE_TOP_SV)
    card_regs = read(CARD_REGS_H)

    build_start = source.find("static uint8_t sp_vtw_build_spans")
    shadow_start = source.find("static uint8_t sp_vtw_shadow_write_span")
    direct_start = source.find("static uint8_t sp_vtw_direct_read_block")
    execute_start = source.find("static void execute_command", direct_start)
    require(direct_start >= 0 and execute_start > direct_start,
            "vTW direct block helper must be present before command dispatch")
    direct = source[direct_start:execute_start]

    require(build_start >= 0 and shadow_start > build_start and
            direct_start > shadow_start,
            "direct block path must preflight all 512 destinations before writing")
    build = source[build_start:shadow_start]
    shadow = source[shadow_start:direct_start]
    require("for (i = 0U; i < SP_VTW_DIRECT_BYTES; ++i)" in build and
            "sp_vtw_build_spans(apple_addr, sss, 1U," in direct,
            "direct read must prove all 512 destination mappings before copying")
    require("bank != spans[span_count - 1U].bank" in build and
            "span_count++" in build and
            "for (i = 0U; i < span_count; ++i)" in direct,
            "legal bank discontinuities must become bounded direct spans")
    require("apple_addr < 0x0200U" in build and
            "(uint32_t)apple_addr + SP_VTW_DIRECT_BYTES > 0x10000UL" in build,
            "direct preflight must reject zero-page/stack and wrapping buffers")
    require("if (bank > 127U)" in source and
            "if (spans[i].bank > 1U)" in direct and
            "sp_vtw_direct_ramworks_block(" in direct,
            "direct mapping must route RamWorks banks 2..127 to the bulk path")
    require(direct.count("sp_vtw_is_live()") >= 2,
            "direct copy must prove vTW live before and after the shadow write")
    require("CARD_CTRL_VTW_SHADOW_ADDR_REG) & 0x3FFFFUL" in shadow and
            "span->phys + (uint32_t)span->length" in shadow,
            "direct copy must verify that the shadow port accepted all bytes")
    require("#define SP_SSS_VTW_WIDE_BIT     (1UL << 21)" in source and
            "const uint8_t aux" in source and "const uint8_t wide" in source and
            "(aux != 0U || wide != 0U)" in source,
            "SmartPort must use the command's exact bank and wide-main state")
    post_start = source.find("static uint8_t sp_vtw_post_span")
    post_end = source.find("static uint8_t sp_vtw_direct_read_block", post_start)
    post = source[post_start:post_end]
    require("CARD_CTRL_VTW_POST_STATS_REG" in post and
            "SP_VTW_POST_CAPACITY - fill" in post and
            "post_credits--" in post and
            "REG_WRITE(CARD_CTRL_VTW_POST_PUSH_REG" in post and
            "CARD_CTRL_VTW_POST_ACCEPT_MASK" in post and
            "post_count" in post,
            "video-range direct loads must reserve queue credits in batches "
            "and prove the final accepted count")
    require(post.count("REG_READ(CARD_CTRL_VTW_POST_STATUS_REG)") <= 3,
            "direct video posting must not read POST_STATUS for each byte")
    require("static uint8_t sp_vtw_wait_video_posts_drained(void)" in post and
            "SP_VTW_POST_DRAIN_TIMEOUT_US" in post and
            "SP_VTW_POST_FILL_MASK) == 0U" in post and
            "sp_vtw_wait_video_posts_drained()" in direct,
            "a direct video read must not complete until its queued "
            "motherboard writes have drained")
    require("assign arm_post_ready = core_active && !eng_post_full && !core_post_req;" in vtw_core and
            "arm_post_we && arm_post_ready" in vtw_core,
            "CPU0 posts must share the ordered vTW queue without replacing a core post")
    require("CARD_CTRL_REG_VTW_POST_PUSH     = 8'h9A" in apple_top and
            "CARD_CTRL_REG_VTW_POST_STATUS   = 8'h9B" in apple_top and
            "vtw_arm_post_accept_count_q" in apple_top and
            "CARD_CTRL_VTW_POST_PUSH_REG" in card_regs and
            "CARD_CTRL_VTW_POST_STATUS_REG" in card_regs,
            "PS and PL must agree on the posted-write command and accepted counter")
    require("CARD_CTRL_REG_VTW_RW_FLUSH      = 8'h9C" in apple_top and
            "arm_rw_flush_req" in vtw_core and
            "rw_flush_active_q" in vtw_core and
            "CARD_CTRL_VTW_RW_FLUSH_REG" in card_regs and
            "sp_vtw_flush_ramworks_cache()" in source,
            "PS and PL must flush and invalidate the vTW RamWorks line before bulk DMA")
    require("psdma_transfer(PSDMA_OWNER_SMARTPORT" in source and
            "PSDMA_DDR_TO_MC" in source and
            "g_vtw_direct_ramworks_count++" in source,
            "RamWorks direct reads must use the owned shared DDR-to-PSRAM service")
    shadow_host = read(SHADOW_HOST_SV)
    require("CARD_CTRL_REG_VTW_SHADOW_DATA4  = 8'h9D" in apple_top and
            "CARD_CTRL_REG_VTW_SHADOW_DATA4_STATUS = 8'h9E" in apple_top and
            "vtw_shadow_host_port vtw_shadow_host_port_i" in apple_top and
            "word_fifo_q [0:1]" in shadow_host and
            "word_accept_count" in shadow_host and
            "CARD_CTRL_VTW_SHADOW_DATA4_ACCEPT_MASK" in shadow,
            "packed shadow writes must use a two-word queue with a final "
            "accepted-count proof")
    require("output logic [21:0]             sp_sss_snapshot" in vtw_core and
            ".vtw_sss_snapshot(vtw_sp_sss_snapshot)" in apple_top and
            "sss_snapshot_q <= vtw_ctrl_write" in read(SMARTPORT_SV),
            "vTW SmartPort commands must latch the core's private switch state")

    require(source.count("sp_vtw_direct_read_block(") == 3,
            "only the helper plus ProDOS and SmartPort READBLOCK may use direct copy")
    require("((direct_completed != 0U) ? SP_CTL_SET_DIRECT : 0U)" in source,
            "READY must advertise direct completion only after a successful copy")
    require("if (axi_set_direct && exec_vtw_q) direct_q <= 1'b1;" in gateware and
            "direct_q       <= 1'b0;" in gateware,
            "gateware must restrict DIRECT to vTW commands and clear it on reset")


def test_vtw_ramworks_dma_freezes_core_and_bounds_polls() -> None:
    source = read(SMARTPORT_C)
    vtw_core = read(VTW_CORE_SV)
    apple_top = read(APPLE_TOP_SV)
    card_regs = read(CARD_REGS_H)
    dma_engine = read(APPLE_DMA_SV)
    dma_command = read(PS_DMA_SV)
    psdma = read(PSDMA_C)

    require("core_res_n && !rw_hold_q" in vtw_core and
            "arm_rw_hold_release" in vtw_core and
            "assign arm_rw_hold_state = rw_hold_q;" in vtw_core,
            "a RamWorks flush must freeze the core via core_en until CPU0 "
            "releases it")
    require("wire rw_flush_unsafe =" in vtw_core and
            "(xstate_q == X_RW_LOOKUP)" in vtw_core and
            "rw_flush_pending_q && !arm_rw_hold_release" in vtw_core and
            "!rw_flush_unsafe" in vtw_core,
            "the flush must wait out the FSM's one-access lookahead before "
            "taking the cache")
    require("if (!enable || !ab_read.res) begin\n"
            "                rw_hold_q <= 1'b0;" in vtw_core and
            "arm_rw_flush_req) begin\n"
            "                    arm_rw_flush_done <= 1'b1;" in vtw_core,
            "session end or Apple RES# must auto-release a frozen core")
    require("rw_release_pending_q" in vtw_core and
            "if (rw_flush_active_q) begin" in vtw_core and
            "rw_release_pending_q <= 1'b1;" in vtw_core and
            "rw_flush_pending_q && !arm_rw_hold_release" in vtw_core and
            "rw_flush_pending_q <= 1'b0;\n"
            "                        arm_rw_flush_done  <= 1'b1;" in vtw_core,
            "release must wait for an active writeback and cancel a pending flush cleanly")
    require("vtw_arm_rw_release_pulse_q" in apple_top and
            "vtw_arm_rw_hold_state," in apple_top,
            "apple_top must expose the release command and held status")
    require("CARD_CTRL_VTW_RW_FLUSH_HELD_BIT" in card_regs and
            "CARD_CTRL_VTW_RW_FLUSH_RELEASE_BIT" in card_regs and
            "sp_vtw_release_core_hold()" in source and
            "CARD_CTRL_VTW_RW_FLUSH_HELD_BIT) != 0U)" in source,
            "the PS must demand HELD on flush completion and release after DMA")
    require("for (;;)" not in source and
            "SP_VTW_FLUSH_POLLS" in source and
            "SP_VTW_DMA_TIMEOUT_US" in source and
            "g_vtw_dma_fault = 1U;" in source,
            "every CPU0 poll must be bounded, with a sticky fault on DMA timeout")
    require("psdma_abort_owned" in psdma and
            "PSDMA_ERR_ABORT" in source and
            "SP_VTW_COPY_FATAL" in source and
            "direct_rc == SP_VTW_COPY_FATAL" in source,
            "a DMA timeout must drain or fail closed, never start byte fallback beside a live DMA")
    require("input  logic                 req_abort" in dma_engine and
            "S_ABORT_DDR_R" in dma_engine and
            "S_ABORT_MC_WAIT" in dma_engine and
            "req_abort_done" in dma_engine,
            "the DMA engine must drain accepted AXI and PSRAM work on abort")
    require("REG_CONTROL" in dma_command and
            "ps_cmd_busy_q" in dma_command and
            "ps_cmd_aborted_q" in dma_command and
            "dma_req_abort_done" in dma_command,
            "the PS DMA command block must expose busy and completed abort state")
    require("CARD_CTRL_VTW_STATUS_ENABLE_EFF" in source and
            "g_vtw_dma_fault = 0U;" in source,
            "the sticky DMA fault must clear only after the vTW session ends")
    require("g_cache[i].block_num >= block_num" in source and
            "g_cache[i].block_num < block_num + fill_blocks" in source,
            "read-ahead fills must retire duplicate copies cached outside "
            "the group")


def test_smartport_latency_and_cache_measurements() -> None:
    source = read(SMARTPORT_C)

    require("XTime_GetTime(&now);\n    g_irq_tick = now;" in source and
            "XTime_GetTime(&dispatch_tick);" in source and
            "XTime_GetTime(&ready_tick);" in source,
            "SmartPort must measure IRQ-to-dispatch and IRQ-to-READY time")
    execute = source[source.find("static void execute_command"):
                     source.find("/* ------------------------------------------------------------------ */\n"
                                 "/* IRQ handler", source.find("static void execute_command"))]
    require(execute.find("XTime_GetTime(&dispatch_tick);") <
            execute.find("const uint32_t hw_status = sp_hw_status();") <
            execute.find("sp_drain(g_cmd_buf"),
            "dispatch time must be sampled before MMIO and command draining")
    require("g_cache_hit_count++" in source and
            "g_cache_miss_count++" in source and
            "g_cache_bypass_count++" in source,
            "cache hit, miss, and RAM-disk bypass paths must be counted")
    require("sd: lat dispatch us" in source and
            "sd: lat ready us" in source and
            "sd: cache hit=" in source,
            "sd status must report the new latency and cache measurements")


def test_vtw_direct_write_preflight_is_fail_closed() -> None:
    source = read(SMARTPORT_C)
    gateware = read(SMARTPORT_SV)
    apple_top = read(APPLE_TOP_SV)
    card_regs = read(CARD_REGS_H)
    shadow_host = read(SHADOW_HOST_SV)
    rom = read(SMARTPORT_ASM)

    require("vtw_ctrl_snap_q <= {ready_q, direct_q, 1'b1, 5'b00000};" in gateware and
            "AND #$20" in rom and "JSR WRCMD" in rom and
            "BVS WRDIRECT" in rom,
            "only the vTW ROM path may preflight and omit a write payload")
    require("SP_FAMILY_PREFLIGHT_BIT" in source and
            "g_vtw_write_preflight.prefix" in source and
            "memmove(g_cmd_buf + g_vtw_write_preflight.prefix_len" in source and
            "direct_write_requested" in source,
            "CPU0 must preserve the drained command prefix across preflight")
    require("sp_vtw_build_spans(apple_addr, sss, 0U," in source and
            "SP_SSS_RAMRD_BIT" in source and
            "sp_vtw_direct_source_eligible" in source,
            "write sources must use the read-side MMU map and prove every span")
    require("sp_vtw_direct_write_source(" in source and
            "if (direct_rc == SP_VTW_COPY_COMPLETE)" in source and
            "write_data = g_scratch;" in source and
            "g_vtw_write_fault_count++" in source,
            "media writes must start only after a complete direct source copy")
    require("CARD_CTRL_REG_VTW_SHADOW_READ4 = 8'h9F" in apple_top and
            "CARD_CTRL_REG_VTW_SHADOW_READ4_DATA = 8'hA0" in apple_top and
            "CARD_CTRL_REG_VTW_SHADOW_READ4_STATUS = 8'hA1" in apple_top and
            "word_read_count" in shadow_host and
            "SH_WORD_CAPTURE" in shadow_host and
            "CARD_CTRL_VTW_SHADOW_READ4_COUNT_MASK" in card_regs,
            "packed shadow reads must expose ready, busy, and a completion proof")
    require("PSDMA_MC_TO_DDR" in source and
            "Xil_DCacheFlushRange((UINTPTR)data, length);" in source and
            "Xil_DCacheInvalidateRange((UINTPTR)data, length);" in source,
            "RamWorks write sources must use coherent PSRAM-to-DDR DMA")


def test_compositor_yields_to_pending_smartport_commands() -> None:
    source = read(SMARTPORT_C)
    header = read(SMARTPORT_H)
    compositor = read(COMPOSITOR_C)

    require("uint8_t smartport_service_has_pending(void);" in header and
            "return (g_cmd_pending_count != 0U) ? 1U : 0U;" in source,
            "long CPU0 work needs a cheap IRQ-updated pending check")
    require("COMPOSITOR_SP_ROWS_PER_SLICE 16" in compositor and
            "compositor_smartport_checkpoint();" in compositor and
            "smartport_service_poll();" in compositor and
            "blit_apple_2x2_serviced" in compositor and
            "blit_apple_2x4_serviced" in compositor,
            "Apple frame copies must service SmartPort between bounded row slices")


def test_rejects_duplicate_image_paths() -> None:
    source = read(SMARTPORT_C)
    header = read(SMARTPORT_H)

    require("#define SMARTPORT_SERVICE_ERR_DUPLICATE_PATH (-3)" in header,
            "service must expose a stable duplicate-path error")
    require("static uint8_t path_ieq(const char *a, const char *b)" in source,
            "duplicate checks must compare paths case-insensitively")
    require("static uint8_t path_eq_char(char c)" in source and
            "if (c == '\\\\') {\n"
            "        c = '/';\n"
            "    }" in source,
            "duplicate checks must treat slash and backslash as equivalent")
    require("static uint8_t sp_image_path_duplicate(uint8_t device_index, const char *path)" in source,
            "service must detect duplicate image paths across SmartPort devices")
    require("if (sp_image_path_duplicate(index, path) != 0U) {\n"
            "        return SMARTPORT_SERVICE_ERR_DUPLICATE_PATH;\n"
            "    }" in source,
            "set_image_path must reject duplicate active image paths")
    require("if (sp_image_path_duplicate(device_index, dev->image_path) != 0U) {\n"
            "        return SMARTPORT_SERVICE_ERR_DUPLICATE_PATH;\n"
            "    }" in source,
            "media load must refuse duplicate configured image paths")


def test_frontend_uses_sp1_for_current_debug_compatibility() -> None:
    source = read(FRONTEND_MAIN_C)

    require("return smartport_service_set_image_path(device, path);" in source,
            "menu path setter should forward one-based SmartPort device numbers")
    require("smartport_service_reset_media(SMARTPORT_SERVICE_ALL_DEVICES)" in source,
            "existing reset command should refresh all SmartPort devices")
    require("smartport_service_read_block(1U, block_num, buffer, count, actual_out)" in source,
            "existing dump/debug reads should continue targeting SP1")


def test_ramdisk_survives_media_refresh_contract() -> None:
    source = read(SMARTPORT_C)

    require("static uint8_t g_ramdisk_state = 0U;" in source,
            "RAM disk mount state must be shared with media reset handling")
    require("if (dev->is_ram != 0U) {\n"
            "        memset(dev, 0, sizeof(*dev));" in source,
            "closing a RAM disk must not call FatFs or leave a synthetic path behind")
    require("rc = load_all_devices();\n"
            "    sp_ramdisk_refresh(uart_base);" in source and
            "if (smartport_present_count() != 0U) {\n"
            "        rc = 0;\n"
            "    }" not in source,
            "SmartPort init must mount the configured RAM disk immediately")
    require("g_ramdisk_state = 0U;\n"
            "        sp_cache_invalidate_device(SMARTPORT_SERVICE_ALL_DEVICES);" in source,
            "all-device media reset must forget any closed RAM disk")
    require("int rc = load_all_devices();\n"
            "            sp_ramdisk_refresh(g_uart_base);\n"
            "            return rc;" in source,
            "all-device media reset must remount RAM32 without hiding a file-load error")


def test_ramdisk_never_claims_or_erases_configured_media() -> None:
    source = read(SMARTPORT_C)

    require("if (g_devices[i].image_open == 0U &&\n"
            "                g_devices[i].image_path[0] == '\\0') {" in source,
            "RAM32 must use only an unconfigured unit, not a configured image that failed to open")
    setter_start = source.find("int smartport_service_set_image_path")
    getter_start = source.find("const char *smartport_service_get_image_path", setter_start)
    require(setter_start >= 0 and getter_start > setter_start,
            "SmartPort image-path setter must be present")
    setter = source[setter_start:getter_start]
    require("if (g_devices[index].is_ram != 0U) {\n"
            "        close_device(&g_devices[index]);\n"
            "        g_ramdisk_state = 0U;\n"
            "    }\n\n"
            "    memcpy(g_devices[index].image_path, path, len + 1U);" in setter,
            "replacing RAM32 must remove it before copying the real image path")
    require("if (g_devices[i].is_ram != 0U) {\n"
            "                close_device(&g_devices[i]);\n"
            "            }" in source,
            "disabling RAM32 must clear its synthetic path and geometry")


def test_ramdisk_does_not_mask_partial_media_failure() -> None:
    source = read(SMARTPORT_C)

    load_start = source.find("static int load_all_devices(void)")
    load_end = source.find("static int sp_cache_get_block", load_start)
    require(load_start >= 0 and load_end > load_start,
            "all-device SmartPort loader must be present")
    load = source[load_start:load_end]
    require("return first_error;" in load and
            "mounted != 0U" not in load,
            "another mounted unit must not turn a partial media load into success")
    require("sd: SP%u open=%u ram=%u ro=%u blocks=%lu path=%.44s" in source,
            "UART SmartPort status must expose each unit's real backend state")


TESTS = [
    test_declares_eight_independent_smartport_devices,
    test_uses_block_cache_instead_of_full_image_slurp,
    test_units_select_device_table_entries,
    test_status_reports_present_devices_and_per_device_geometry,
    test_command_path_uses_cache_for_reads_and_writes,
    test_vtw_uses_readahead_and_word_wide_response_buffer,
    test_vtw_direct_read_is_complete_and_fail_closed,
    test_vtw_ramworks_dma_freezes_core_and_bounds_polls,
    test_smartport_latency_and_cache_measurements,
    test_vtw_direct_write_preflight_is_fail_closed,
    test_compositor_yields_to_pending_smartport_commands,
    test_rejects_duplicate_image_paths,
    test_frontend_uses_sp1_for_current_debug_compatibility,
    test_ramdisk_survives_media_refresh_contract,
    test_ramdisk_never_claims_or_erases_configured_media,
    test_ramdisk_does_not_mask_partial_media_failure,
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
        print(f"{len(TESTS) - len(failures)} of {len(TESTS)} SmartPort tests passed; "
              f"{len(failures)} failed")
        return 1
    print(f"{len(TESTS)} SmartPort tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
