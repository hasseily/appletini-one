#!/usr/bin/env python3
"""Source checks for USB-triggered PNG screenshot capture."""

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
FRONTEND = REPO_ROOT / "ps_sources" / "frontend"
SCREENSHOT_C = FRONTEND / "screenshot_service.c"
SCREENSHOT_H = FRONTEND / "screenshot_service.h"
COMPOSITOR_C = FRONTEND / "compositor.c"
COMPOSITOR_H = FRONTEND / "compositor.h"
FRONTEND_MAIN_C = FRONTEND / "main.c"
VITIS_SCRIPT = REPO_ROOT / "scripts" / "create_vitis_workspace.py"
USB_STORAGE_SERVICE_C = FRONTEND / "usb_storage_service.c"
USB_STORAGE_SERVICE_H = FRONTEND / "usb_storage_service.h"


class TestFailure(AssertionError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def test_service_contract_and_paths() -> None:
    header = read(SCREENSHOT_H)
    source = read(SCREENSHOT_C)
    vitis = read(VITIS_SCRIPT)

    require("SCREENSHOT_SERVICE_KIND_A2" in header and
            "SCREENSHOT_SERVICE_KIND_1080P" in header and
            "screenshot_service_result_t" in header and
            "screenshot_service_rect_t" in header and
            "typedef void (*screenshot_service_sd_write_hook_t)(void *ctx);" in header and
            "void screenshot_service_set_sd_write_hook(screenshot_service_sd_write_hook_t hook," in header and
            "void screenshot_service_set_scanlines(uint8_t mode);" in header and
            "void screenshot_service_poll(void);" in header and
            "uint8_t screenshot_service_restore_rect_for_frame(uint16_t *fb," in header and
            "void screenshot_service_draw_overlay(uint16_t *fb);" in header,
            "screenshot service must expose A2/1080p save and overlay restore APIs")
    require('#define SCREENSHOT_DIR "0:/screenshots"' in source and
            'SCREENSHOT_DIR "/%s-%s.png"' in source and
            '"a2"' in source and
            '"1080p"' in source,
            "screenshots must be saved under 0:/screenshots with the expected suffixes")
    require('"%04u%02u%02u-%02u%02u%02u"' in source and
            '"uptime-%010llu"' in source,
            "screenshot filenames must use RTC timestamps with an uptime fallback")
    require("DWORD appletini_fatfs_get_fattime(void)" in source and
            "g_fattime_override_active" in source and
            "fat_timestamp_override_begin(rtc);" in source and
            "fat_timestamp_override_end();" in source and
            "save_a2_png(timestamp, rtc, result)" in source and
            "save_1080p_png(timestamp, rtc, result)" in source,
            "screenshot file creation and modified metadata must use the RTC timestamp")
    require("enable_fatfs_timestamp_hook" in vitis and
            "appletini_fatfs_get_fattime" in vitis and
            "get_fattime()" in vitis,
            "Vitis generator must patch generated FatFs get_fattime for screenshots")


def test_png_writer_is_streaming_and_self_contained() -> None:
    source = read(SCREENSHOT_C)

    require("0x89U, 'P', 'N', 'G'" in source and
            'png_chunk_begin(file, "IHDR"' in source and
            'png_chunk_begin(file, "IDAT"' in source and
            'png_chunk_begin(file, "IEND"' in source,
            "screenshot service must write real PNG chunks")
    require("uint8_t zlib_header[2] = {0x78U, 0x01U};" in source and
            "store_le16(&block_header[1], len16);" in source and
            "store_le16(&block_header[3], (uint16_t)~len16);" in source,
            "PNG IDAT must use valid uncompressed zlib blocks")
    require("crc32_update" in source and
            "adler32_update" in source and
            "fill_png_row(g_png_row, surface, y, width);" in source and
            "static uint8_t g_png_row[1U + (COMP_OUT_WIDTH * 4U)];" in source,
            "PNG writing must stream one converted BGRA-to-RGBA row at a time")


def test_sources_pause_and_capture_expected_surfaces() -> None:
    source = read(SCREENSHOT_C)
    compositor_c = read(COMPOSITOR_C)
    compositor_h = read(COMPOSITOR_H)

    require("compositor_set_paused(1U);" in source and
            "compositor_set_paused(0U);" in source and
            "compositor_request_full_refresh();" in source,
            "screen updates must pause during writes and refresh when the overlay is shown")
    require("void compositor_set_paused(uint8_t paused);" in compositor_h and
            "const uint16_t *compositor_latched_framebuffer(uint8_t *slot_out);" in compositor_h and
            "if (s_paused != 0u) {\n        return 0;\n    }" in compositor_c,
            "compositor must expose pause and latched-frame capture hooks")
    service = read(SCREENSHOT_C)
    require("(surface->base == NULL && surface->base565 == NULL)" in service,
            "PNG encode must accept both BGRA32 (Apple ring) and RGB565 "
            "(output ring) surfaces")
    require("apple_fb_reader_claim()" in source and
            "g_compositor_last_apple_slot" in source and
            "g_compositor_last_apple_mode" in source and
            "APPLE_FB_DISPLAY_MODE_SHR" in source and
            "COMP_APPLE_SHR_WIDTH" in source and
            "width = COMP_APPLE_SHR_WIDTH * 2U;" in source and
            "height = COMP_APPLE_SHR_HEIGHT * 2U;" in source and
            "COMP_APPLE_WIDTH" in source and
            "width = COMP_APPLE_WIDTH * 2U;" in source and
            "height = COMP_APPLE_HEIGHT * 4U;" in source and
            "COMP_APPLE_LEFT_BORDER_PIXELS" in source,
            "A2 screenshots must capture scaled SHR or legacy visible pixels, including the last composited frame")
    require("surface.scale_x = 2U;" in source and
            "surface.scale_y = 2U;" in source and
            "surface.scale_y = 4U;" in source and
            "surface.scanlines_mode = g_scanlines_mode;" in source and
            "surface_scanline_blank(surface, y)" in source,
            "A2 screenshots must stream the current scanline-rendered Apple area")
    require("compositor_latched_framebuffer(&slot)" in source and
            "COMP_OUT_WIDTH" in source and
            "COMP_OUT_HEIGHT" in source,
            "1080p screenshots must capture the latched compositor output")


def test_overlay_frontend_and_build_wiring() -> None:
    source = read(SCREENSHOT_C)
    frontend_main = read(FRONTEND_MAIN_C)
    vitis = read(VITIS_SCRIPT)
    usb_storage_c = read(USB_STORAGE_SERVICE_C)
    usb_storage_h = read(USB_STORAGE_SERVICE_H)

    require('"SCREENSHOT SAVED"' in source and
            "SCREENSHOT_OVERLAY_TICKS" in source and
            "fb16_string_scaled(fb," in source,
            "screenshot service must draw a temporary saved overlay")
    require('#include "usb_storage_service.h"' in source and
            "usb_storage_was_connected = usb_storage_service_disconnect();" in source and
            "screenshot_service_note_local_sd_write_complete();\n        if (usb_storage_was_connected != 0U) {\n            usb_storage_service_connect();" in source and
            "screenshot_service_note_local_sd_write_complete();\n    if (usb_storage_was_connected != 0U) {\n        usb_storage_service_connect();" in source and
            "usb_storage_service_connect();" in source and
            "fr = mount_sd();\n\n    if (fr != FR_OK)" in source,
            "screenshot writes must quiesce USB0 storage, refresh local SD users, and mount SD before mkdir/open")
    require("uint8_t usb_storage_service_disconnect(void);" in usb_storage_h and
            "uint8_t usb_storage_service_disconnect(void)" in usb_storage_c and
            "XUsbPs_StorageFlushPending();" in usb_storage_c and
            "UsbSoftDisconnect(&UsbInstance);" in usb_storage_c and
            "UsbConnected = 0;" in usb_storage_c,
            "USB0 storage service must expose a draining soft-disconnect for local FAT writes")
    require("static uint8_t g_overlay_drawn_slots;" in source and
            "static uint8_t g_overlay_restore_slots;" in source and
            "output_slot_mask_for_fb(fb)" in source and
            "g_overlay_restore_slots |= g_overlay_drawn_slots;" in source and
            "compositor_request_full_refresh();" in source,
            "overlay timeout must request a refresh and restore only slots that were drawn")
    require("(g_overlay_drawn_slots & slot_mask) != 0U" not in source,
            "active screenshot overlay must redraw each composed frame until its timeout")
    require("overlay_show(result->message);" in source,
            "failed screenshots must show the actual failure message on screen")
    require('#include "screenshot_service.h"' in frontend_main and
            "screenshot_service_init();" in frontend_main and
            "screenshot_service_set_sd_write_hook(ui_screenshot_sd_write_complete, NULL);" in frontend_main and
            "smartport_service_reset_media(SMARTPORT_SERVICE_ALL_DEVICES)" in frontend_main and
            "screenshot_service_set_scanlines(g_scanlines_mode_shadow);" in frontend_main and
            "screenshot_service_poll();" in frontend_main and
            "screenshot_service_restore_rect_for_frame(fb, &rect)" in frontend_main and
            "ui_restore_static_rect(fb, rect.x, rect.y, rect.w, rect.h, show_bezel);" in frontend_main and
            "screenshot_service_draw_overlay(fb);" in frontend_main and
            "screenshot_service_save(kind, &g_rtc, &result)" in frontend_main,
            "frontend must initialize, poll, restore, draw, and dispatch screenshot saves")
    require('"../../../ps_sources/frontend/screenshot_service.c"' in vitis,
            "Vitis workspace generator must register the screenshot service source")


TESTS = [
    test_service_contract_and_paths,
    test_png_writer_is_streaming_and_self_contained,
    test_sources_pause_and_capture_expected_surfaces,
    test_overlay_frontend_and_build_wiring,
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
        print(f"{len(TESTS) - len(failures)} of {len(TESTS)} screenshot tests passed; "
              f"{len(failures)} failed")
        return 1
    print(f"{len(TESTS)} screenshot tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
