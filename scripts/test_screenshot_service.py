#!/usr/bin/env python3
"""Run the real screenshot service, then independently decode its PNG output."""
from pathlib import Path
import os
import shutil
import struct
import subprocess
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]


def check_integration() -> None:
    """Retain the existing frontend/build checks around the executable test."""
    frontend = ROOT / "ps_sources/frontend"
    main = (frontend / "main.c").read_text(encoding="utf-8")
    generator = (ROOT / "scripts/create_vitis_workspace.py").read_text(encoding="utf-8")
    usb = (frontend / "usb_storage_service.c").read_text(encoding="utf-8")
    service = (frontend / "screenshot_service.c").read_text(encoding="utf-8")
    for removed in ("compositor_frame_picture_rect", "compositor_frame_output_size",
                    "compositor_frame_pixel_mask", "video_pixel_mask_frame_rgb565"):
        assert removed not in service, ("capture must retain F1.2.2 sources", removed)
    assert "apple_fb_reader_claim()" in service
    for token in (
        '#include "screenshot_service.h"', "screenshot_service_init();",
        "screenshot_service_set_sd_write_hook(ui_screenshot_sd_write_complete, NULL);",
        "smartport_service_reset_media(SMARTPORT_SERVICE_ALL_DEVICES)",
        "screenshot_service_update_fattime_from_rtc(&g_rtc);",
        "screenshot_service_poll();", "screenshot_service_take_result(&screenshot_result)",
        "screenshot_service_restore_rect_for_frame(fb, &rect)",
        "ui_restore_static_rect(fb, rect.x, rect.y, rect.w, rect.h, show_bezel);",
        "screenshot_service_draw_overlay(fb);", "screenshot_service_request(kind, &g_rtc, &result)",
    ):
        assert token in main, ("frontend screenshot wiring", token)
    from test_joystick_config_menu import function
    for name in ("control_set_usb0_sd_remote_mount", "control_set_ethernet_ftp_sd_remote"):
        assert "screenshot_service_cancel();" in function(main, name), name
    immediate = function(main, "ui_save_screenshot")
    assert "screenshot_service_show_confirmation(result.message)" in immediate
    event = function(main, "ui_handle_usb_menu_event")
    guard = event[event.index('"STOP SD SHARING FIRST"'):]
    assert "s->input_seq++;" in guard[:120]
    assert "config_menu_usb0_sd_remote_active(menu)" in event
    assert "config_menu_ethernet_ftp_sd_remote_active(menu)" in event
    for token in ('"../../../ps_sources/frontend/screenshot_service.c"',
                  "enable_fatfs_timestamp_hook", "appletini_fatfs_get_fattime", "get_fattime()"):
        assert token in generator, ("Vitis screenshot wiring", token)
    for token in ("uint8_t usb_storage_service_disconnect(void)",
                  "XUsbPs_StorageFlushPending();", "UsbSoftDisconnect(&UsbInstance);",
                  "UsbConnected = 0;"):
        assert token in usb, ("USB storage disconnect", token)
    print("PASS frontend, overlay, SD invalidation, USB draining, timestamp and build wiring")


def check_png(path: Path, width: int, height: int, raw_layout: int | None = None,
              scanlines: int = 0) -> None:
    image = path.read_bytes()
    assert image[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat, chunks = 8, bytearray(), []
    while pos < len(image):
        length = struct.unpack_from(">I", image, pos)[0]
        kind = image[pos + 4:pos + 8]
        data = image[pos + 8:pos + 8 + length]
        crc = struct.unpack_from(">I", image, pos + 8 + length)[0]
        assert crc == zlib.crc32(kind + data), (path.name, kind, "CRC")
        chunks.append(kind)
        if kind == b"IHDR":
            assert struct.unpack(">IIBBBBB", data) == (width, height, 8, 6, 0, 0, 0)
        elif kind == b"IDAT":
            idat.extend(data)
        else:
            assert kind == b"IEND" and length == 0
        pos += 12 + length
    assert pos == len(image) and chunks == [b"IHDR", b"IDAT", b"IEND"]
    raw = zlib.decompress(idat)  # Also checks deflate structure and Adler-32.
    assert len(raw) == height * (1 + 4 * width)
    for y in range(height):
        start = y * (1 + 4 * width)
        assert raw[start] == 0
        expected = bytearray()
        for x in range(width):
            if raw_layout is None:
                r, g, b = (x * 3 + y * 5) & 31, (x * 7 + y * 11) & 63, (x * 13 + y * 17) & 31
                rgb = [(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)]
            else:
                # Independent F1.2.2 sampling rules (3101934 save_a2_png):
                # active legacy starts32,16; border starts4,0; SHR starts0,0.
                sy_scale = 2 if raw_layout == 2 else 4
                sx = x // 2 + (32 if raw_layout == 0 else 4 if raw_layout == 1 else 0)
                sy = y // sy_scale + (16 if raw_layout == 0 else 0)
                blank = (scanlines >= 2 and y % 2 == 1) if sy_scale == 2 else (
                    scanlines != 0 and y % 4 >= 4 - scanlines)
                rgb = [0, 0, 0] if blank else [
                    (sx * 3 + sy * 5 + 1) & 255,
                    (sx * 7 + sy * 11 + 2) & 255,
                    (sx * 13 + sy * 17 + 3) & 255]
            expected.extend((*rgb, 255))
        assert raw[start + 1:start + 1 + 4 * width] == expected, (path.name, y)
    print(f"PASS decoded {path.name}: {width}x{height}, every RGBA pixel/CRC/Adler checked")


def main() -> int:
    check_integration()
    compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc")
    if compiler is None:
        bundled = Path("E:/AMDDesignTools/2025.2/tps/mingw/10.0.0/win64.o/nt/bin/gcc.exe")
        if bundled.is_file():
            compiler = str(bundled)
    if compiler is None:
        raise RuntimeError("A native C compiler is required")
    environment = dict(os.environ, PATH=str(Path(compiler).parent) + os.pathsep + os.environ.get("PATH", ""))
    with tempfile.TemporaryDirectory(prefix="appletini-screenshot-") as directory:
        output = Path(directory)
        executable = output / "screenshot_host"
        command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
                   "-I", str(ROOT / "scripts/fixtures/screenshot_bsp"),
                   str(ROOT / "scripts/fixtures/screenshot_service_host.c"),
                   str(ROOT / "ps_sources/lib/crc32.c"), "-o", str(executable)]
        if os.environ.get("SCREENSHOT_SANITIZE", "0" if os.name == "nt" else "1") != "0":
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, env=environment)
        subprocess.run([str(executable), str(output)], check=True, env=environment)
        for layout, dimensions in enumerate(((1120, 768), (1232, 896), (1280, 800))):
            for scanlines in range(4):
                check_png(output / f"raw-{layout}-scan-{scanlines}.png", *dimensions,
                          raw_layout=layout, scanlines=scanlines)
        check_png(output / "raw-interlace-baseline.png", 1120, 768, raw_layout=0)
        check_png(output / "raw-fallback.png", 1280, 800, raw_layout=2)
        check_png(output / "full.png", 1920, 1080)
        check_png(output / "wide.png", 1360, 768)
        check_png(output / "frozen-stride.png", 11, 7)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
