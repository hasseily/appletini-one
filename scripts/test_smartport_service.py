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
FRONTEND_MAIN_C = REPO_ROOT / "ps_sources" / "frontend" / "main.c"


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
    require("sp_cache_get_block(dev, block_num, 0U, &cache_addr)" in source,
            "READBLOCK must load/cache the requested device block")
    require("sp_cache_get_block(dev, block_num, 1U, &cache_addr)" in source,
            "WRITEBLOCK must load/cache the requested device block before updating media")
    require("sp_push_buf(cache_ptr_from_addr(cache_addr), SP_BLOCK_SIZE);" in source,
            "READBLOCK must stream the selected cache block through the OUT FIFO")
    require("sp_push_buf(g_scratch, length);" in source,
            "SmartPort STATUS payload must stream through the OUT FIFO")
    require("sp_write_block_to_image(dev, block_num," in source and
            "memcpy(cache_ptr_from_addr(cache_addr), data, SP_BLOCK_SIZE);" in source,
            "WRITEBLOCK must update the selected cached block before persisting media")
    require("f_write(&dev->image_file," in source and
            "data, SP_BLOCK_SIZE, &bw)" in source,
            "WRITEBLOCK must persist the Apple-supplied block to the selected image file")


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
