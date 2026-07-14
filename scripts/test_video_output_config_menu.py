#!/usr/bin/env python3
"""Source-level regression tests for boot-menu video output settings.

These tests run without Vitis or hardware:

    python scripts/test_video_output_config_menu.py
"""

from __future__ import annotations

import re
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
CONFIG_MENU_C = REPO_ROOT / "ps_sources" / "frontend" / "config_menu.c"
CONFIG_MENU_H = REPO_ROOT / "ps_sources" / "frontend" / "config_menu.h"
CONFIG_MENU_HELP_C = REPO_ROOT / "ps_sources" / "frontend" / "config_menu_help.c"
CONFIG_MENU_INTERNAL_H = REPO_ROOT / "ps_sources" / "frontend" / "config_menu_internal.h"
CONFIG_MENU_MAIN_TABS_C = REPO_ROOT / "ps_sources" / "frontend" / "config_menu_main_tabs.c"
FRONTEND_MAIN_C = REPO_ROOT / "ps_sources" / "frontend" / "main.c"
APPLE_FB_HANDOFF_C = REPO_ROOT / "ps_sources" / "frontend" / "apple_fb_handoff.c"
APPLE_FB_HANDOFF_H = REPO_ROOT / "ps_sources" / "frontend" / "apple_fb_handoff.h"
APPLE_CYCLE_RENDERER_C = REPO_ROOT / "ps_sources" / "frontend" / "apple_cycle_renderer.c"
CPU1_MAIN_C = REPO_ROOT / "ps_sources" / "frontend_core1" / "main.c"
COMPOSITOR_C = REPO_ROOT / "ps_sources" / "frontend" / "compositor.c"
COMPOSITOR_H = REPO_ROOT / "ps_sources" / "frontend" / "compositor.h"
COMPOSITOR_LAYOUT_H = REPO_ROOT / "ps_sources" / "frontend" / "compositor_layout.h"
FB16_C = REPO_ROOT / "ps_sources" / "lib" / "fb16.c"
DEBUG_OVERLAY_C = REPO_ROOT / "ps_sources" / "frontend" / "debug_overlay.c"
DEBUG_OVERLAY_H = REPO_ROOT / "ps_sources" / "frontend" / "debug_overlay.h"
NTSC_C = REPO_ROOT / "ps_sources" / "frontend" / "appletini_ntsc.c"
NTSC_H = REPO_ROOT / "ps_sources" / "frontend" / "appletini_ntsc.h"
PAL_TIMING_C = REPO_ROOT / "ps_sources" / "frontend" / "apple_pal_video_timing.c"
PAL_TIMING_H = REPO_ROOT / "ps_sources" / "frontend" / "apple_pal_video_timing.h"
VIDEO_OUTPUT_H = REPO_ROOT / "ps_sources" / "frontend" / "video_output.h"
VIDEO_GHOSTING_H = REPO_ROOT / "ps_sources" / "frontend" / "video_ghosting.h"
IMAGE_VERSIONS_H = REPO_ROOT / "ps_sources" / "image_versions.h"
VITIS_SCRIPT = REPO_ROOT / "scripts" / "create_vitis_workspace.py"


class TestFailure(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def config_version(source: str) -> int:
    match = re.search(r"#define APPLETINI_CFG_VERSION\s+(\d+)U", source)
    require(match is not None, "config version define must be present")
    return int(match.group(1))


def has_define(source: str, name: str, value: str) -> bool:
    return re.search(
        rf"^\s*#define\s+{re.escape(name)}\s+{re.escape(value)}\b",
        source,
        re.MULTILINE,
    ) is not None


def test_shared_video_output_contract() -> None:
    header = read(VIDEO_OUTPUT_H)
    handoff_h = read(APPLE_FB_HANDOFF_H)
    handoff_c = read(APPLE_FB_HANDOFF_C)

    require("#define APPLE_VIDEO_COLOR_IDEALIZED          0U" in header and
            "#define APPLE_VIDEO_COLOR_RGB                1U" in header and
            "#define APPLE_VIDEO_COLOR_COMPOSITE_MONITOR  2U" in header and
            "#define APPLE_VIDEO_COLOR_TV                 3U" in header and
            "#define APPLE_VIDEO_COLOR_PAL_ACCURATE_COMPOSITE 4U" in header and
            "#define APPLE_VIDEO_COLOR_PAL_ACCURATE_TV        5U" in header and
            "#define APPLE_VIDEO_COLOR_COUNT              6U" in header,
            "video-output header must define the clean and PAL-accurate render/color modes")
    require("APPLE_VIDEO_SETTINGS_DEFAULT" in header and
            "APPLE_VIDEO_COLOR_COMPOSITE_MONITOR" in header,
            "packed video settings must default to Composite Monitor")
    require("#define APPLE_VIDEO_SETTINGS_COLOR_MODE_MASK   0xFU" in header and
            "#define APPLE_VIDEO_SETTINGS_VIDEO7_AUTO_MONO_SHIFT 8U" in header and
            "#define APPLE_VIDEO_SETTINGS_CLEAN_PHASE_SHIFT 12U" in header and
            "#define APPLE_VIDEO_SETTINGS_PAL_PHASE_SHIFT   20U" in header and
            "APPLE_VIDEO_DEFAULT_CLEAN_PHASE_CYCLES" in header and
            "APPLE_VIDEO_DEFAULT_PAL_PHASE_CYCLES" in header and
            "static inline uint32_t apple_video_settings_pack_ex" in header and
            "static inline uint32_t apple_video_settings_pack_full" in header and
            "apple_video_settings_video7_auto_mono_enabled" in header and
            "apple_video_settings_clean_phase_cycles" in header and
            "apple_video_settings_pal_phase_cycles" in header and
            "apple_video_color_mode_is_pal_accurate" in header,
            "packed video settings must carry six modes, Video-7 auto-mono, and timing phase biases")
    require("void apple_fb_video_settings_set(uint32_t settings);" in handoff_h and
            "uint32_t apple_fb_video_settings_get(void);" in handoff_h,
            "CPU0/CPU1 handoff must expose video settings accessors")
    require("#define HANDOFF_VIDEO_SETTINGS_ADDR  0xFFFF100CU" in handoff_c,
            "video settings must live in the existing strongly ordered OCM handoff block")
    require("*s_video_settings = APPLE_VIDEO_SETTINGS_DEFAULT;" in handoff_c,
            "handoff init must publish Composite Monitor as the safe default")
    require("*s_video_settings = apple_video_settings_normalize(settings);" in handoff_c,
            "handoff setter must preserve and normalize every packed video setting bit")


def test_menu_persists_video_output_settings() -> None:
    header = read(CONFIG_MENU_H)
    source = read(CONFIG_MENU_C)
    ghosting_h = read(VIDEO_GHOSTING_H)

    require("void (*set_video_output)(void *ctx," in header and
            "uint8_t video7_auto_mono_enable,\n"
            "                             int8_t clean_phase_cycles,\n"
            "                             int8_t pal_phase_cycles);" in header and
            "uint8_t (*get_video_output_color_mode)(void *ctx);" in header and
            "uint8_t (*get_video7_auto_mono_enabled)(void *ctx);" in header and
            "int8_t (*get_clean_video_phase_cycles)(void *ctx);" in header and
            "int8_t (*get_pal_video_phase_cycles)(void *ctx);" in header and
            "void (*set_video_ghosting)(void *ctx, uint8_t strength);" in header and
            "uint8_t (*get_video_ghosting)(void *ctx);" in header and
            "void (*set_border)(void *ctx," in header and
            "uint8_t (*is_apple_video_50hz)(void *ctx);" in header,
            "menu platform must expose video, ghosting, and border runtime callbacks")
    require("uint8_t video_output_mono;" in header and
            "uint8_t video_mono_color;" in header and
            "uint8_t video_color_mode;" in header and
            "uint8_t video7_auto_mono_enabled;" in header and
            "uint8_t video_ghosting_strength;" in header and
            "uint8_t border_enabled;" in header and
            "uint8_t border_color;" in header and
            "uint8_t border_flood;" in header and
            "video_phosphor" not in header and
            "video_soften" not in header and
            "int8_t clean_video_phase_cycles;" in header and
            "int8_t pal_video_phase_cycles;" in header,
            "menu state must carry video output, ghosting strength, mono color, color mode, Video-7 auto-mono, and fixed timing phases")
    require(config_version(source) >= 100,
            "clean config version must include the persisted ghosting strength")
    require("#define CONFIG_DEFAULT_VIDEO_COLOR_MODE APPLE_VIDEO_COLOR_COMPOSITE_MONITOR" in source,
            "new configs must default to Composite Monitor")
    require("#define CONFIG_DEFAULT_VIDEO7_AUTO_MONO_ENABLED 1U" in source,
            "new configs must preserve existing Video-7 auto-mono behavior by default")
    require("#define CONFIG_DEFAULT_VIDEO_GHOSTING_STRENGTH APPLETINI_VIDEO_GHOSTING_OFF" in source and
            has_define(ghosting_h, "APPLETINI_VIDEO_GHOSTING_OFF", "0U") and
            has_define(ghosting_h, "APPLETINI_VIDEO_GHOSTING_LIGHT", "1U") and
            has_define(ghosting_h, "APPLETINI_VIDEO_GHOSTING_MEDIUM", "2U") and
            has_define(ghosting_h, "APPLETINI_VIDEO_GHOSTING_STRONG", "3U") and
            has_define(ghosting_h, "APPLETINI_VIDEO_GHOSTING_MAX",
                       "APPLETINI_VIDEO_GHOSTING_STRONG") and
            "APPLETINI_VIDEO_GHOSTING_HEAVY" not in ghosting_h and
            "APPLETINI_VIDEO_GHOSTING_HIGH" not in ghosting_h and
            'return "Medium";' in ghosting_h and
            'return "Strong";' in ghosting_h and
            "appletini_video_ghosting_clamp" in ghosting_h and
            "appletini_video_ghosting_name" in ghosting_h,
            "ghosting must default off and expose Off/Light/Medium/Strong")
    require('"video.output=%s\\n"' in source and
            '"video.mono.color=%s\\n"' in source and
            '"video.color.mode=%s\\n"' in source and
            '"video.video7.monochrome=%s\\n"' in source and
            '"video.ghosting=%s\\n"' in source and
            '"video.border.enabled=%s\\n"' in source and
            '"video.border.color=%u\\n"' in source and
            '"video.border.outside=%s\\n"' in source,
            "saved config must include the documented video output, ghosting, and border settings")
    require('"video_clean_phase_cycles=%d\\n"' not in source and
            '"video_pal_phase_cycles=%d\\n"' not in source,
            "saved config must not expose hidden phase tuning")
    require('strcmp(key, "video.output") == 0' in source and
            'strcmp(key, "video.mono.color") == 0' in source and
            'strcmp(key, "video.color.mode") == 0' in source and
            'strcmp(key, "video.video7.monochrome") == 0' in source and
            'strcmp(key, "video.ghosting") == 0' in source and
            'strcmp(key, "video.border.enabled") == 0' in source and
            'strcmp(key, "video.border.color") == 0' in source and
            'strcmp(key, "video.border.outside") == 0' in source,
            "config loader must parse user-visible video-output and ghosting settings only")
    require('strcmp(key, "video_clean_phase_cycles") == 0' not in source and
            'strcmp(key, "video_pal_phase_cycles") == 0' not in source and
            "config_menu_video_phase_value" not in source,
            "config loader must not expose internal phase tuning keys")
    require("config_menu_reset_settings_only(menu);" in source and
            "config_menu_coerce_video_ghosting(menu);" in source,
            "config loading must apply defaults before parsing and coercion afterward")
    require('"PAL Accurate Composite"' in source and
            '"PAL Accurate TV"' in source and
            '"pal_accurate_composite"' in source and
            '"pal_accurate_tv"' in source,
            "menu and config text must expose both PAL-accurate color modes")


def test_border_flood_excludes_bezel_and_debugging() -> None:
    source = read(CONFIG_MENU_C)
    main_tabs = read(CONFIG_MENU_MAIN_TABS_C)
    coerce = source[source.index("static void config_menu_coerce_border"):
                    source.index("static void config_menu_apply_border")]

    require("if (menu->border_flood != 0u) {\n"
            "        menu->show_bezel = 0U;\n"
            "        menu->show_debugging = 0U;\n"
            "    }" in coerce and
            source.count("config_menu_coerce_border(menu);") >= 3,
            "flood must override bezel and debugging after normal and profile config loads")
    require("hgr_draw_check_item_dimmed : hgr_draw_check_item" in main_tabs and
            "hgr_draw_value_item_dimmed : hgr_draw_value_item" in main_tabs,
            "flood must visibly disable bezel and debugging controls")
    require(source.count("menu->item_focus >= CONFIG_VIDEO_ITEM_SHOW_BEZEL") >= 2 and
            source.count("menu->item_focus <= CONFIG_VIDEO_ITEM_DEBUG") >= 2,
            "disabled flood-conflicting controls must reject adjustment and activation")


def test_video_help_overrides_every_row() -> None:
    source = read(CONFIG_MENU_HELP_C)
    internal = read(CONFIG_MENU_INTERNAL_H)
    video_help = source[source.index("HELP(video,"):
                        source.index("/*  SMARTPORT")]
    items = re.findall(
        r"^#define (CONFIG_VIDEO_ITEM_(?!COUNT\b)[A-Z0-9_]+)\s+\d+U$",
        internal,
        re.MULTILINE,
    )
    count = re.search(r"^#define CONFIG_VIDEO_ITEM_COUNT\s+(\d+)U$",
                      internal, re.MULTILINE)
    overrides = re.findall(r"OVERRIDE\((CONFIG_VIDEO_ITEM_[A-Z0-9_]+),", video_help)

    require(count is not None and len(items) == int(count.group(1)),
            "Video item definitions must match CONFIG_VIDEO_ITEM_COUNT")
    require(overrides == items,
            "every Video row must have exactly one help override in menu order")
    require(re.search(r'"\s*\n\s*"', video_help) is None,
            "Video help lines must be comma-separated, not concatenated")
    for line in re.findall(r'^\s*"([^"]*)', video_help, re.MULTILINE):
        require(len(line) <= 100,
                f"Video help line exceeds 100 characters: {line}")


def test_boot_menu_groups_boot_and_video_settings() -> None:
    source = read(CONFIG_MENU_C)
    internal = read(CONFIG_MENU_INTERNAL_H)
    main_tabs = read(CONFIG_MENU_MAIN_TABS_C)
    boot_draw = main_tabs[
        main_tabs.index("void config_menu_draw_boot_settings"):
        main_tabs.index("void config_menu_draw_video")
    ]
    video_draw = main_tabs[
        main_tabs.index("void config_menu_draw_video"):
        main_tabs.index("void config_menu_draw_clock")
    ]
    ghosting_draw = source[
        source.index("void hgr_draw_video_ghosting_item"):
        source.index("static void config_menu_draw_tabs")
    ]
    help_draw = source[
        source.index("static void config_menu_draw_help"):
        source.index("static void config_menu_blit_scaled_bgra")
    ]

    require("CONFIG_TAB_PROFILES = 0," in internal and
            "CONFIG_TAB_BOOT_SETTINGS," in internal and
            "CONFIG_TAB_VIDEO," in internal and
            internal.index("CONFIG_TAB_PROFILES = 0,") <
            internal.index("CONFIG_TAB_BOOT_SETTINGS,") <
            internal.index("CONFIG_TAB_VIDEO,"),
            "boot menu tabs must keep profiles above boot settings and video as distinct pages")
    require('"Profiles"' in source and '"Boot Settings"' in source and '"Video"' in source and
            source.index('"Profiles"') < source.index('"Boot Settings"') < source.index('"Video"'),
            "tab labels must draw Profiles above Boot Settings and Video pages")
    require("case CONFIG_TAB_BOOT_SETTINGS:\n        return CONFIG_MENU_BOOT_ITEM_COUNT;" in source,
            "boot settings tab must contain boot controls and USB menu bindings")
    require("case CONFIG_TAB_VIDEO:\n        return CONFIG_VIDEO_ITEM_COUNT;" in source and
            "#define CONFIG_VIDEO_ITEM_COUNT        12U" in internal,
            "video tab must contain output, effects, border, bezel, and ROM controls")
    require('"Boot menu"' in boot_draw and
            '"Boot device"' in boot_draw and
            '"USB MENU BINDINGS"' in boot_draw and
            '"Show debugging"' not in boot_draw and
            '"Show bezel"' not in boot_draw and
            '"Bezel"' not in boot_draw,
            "boot settings tab must draw only boot rows and USB menu bindings")
    require('"Video output"' in video_draw and
            '"Video-7 mono"' in video_draw and
            '"Scanlines"' in video_draw and
            "hgr_draw_video_ghosting_item" in video_draw and
            '"IIgs border (VidHD $C034)"' in video_draw and
            '"Border color"' in video_draw and
            '"Outside ring"' in video_draw and
            '"Phosphor ghosting"' in source and
            '"Phosphor blur"' not in video_draw and
            '"Horizontal soften"' not in video_draw and
            '"Show debugging"' in video_draw and
            '"Show bezel"' in video_draw and
            '"Bezel"' in video_draw and
            '"Video ROM"' in video_draw and
            "config_menu_video_variant_label(menu)" in video_draw,
            "video tab must draw video output, effects, bezel controls, and the video ROM override")
    require(video_draw.index('"Scanlines"') <
            video_draw.index("hgr_draw_video_ghosting_item") <
            video_draw.index('"IIgs border (VidHD $C034)"') <
            video_draw.index('"Border color"') <
            video_draw.index('"Outside ring"') <
            video_draw.index('"Video ROM"') <
            video_draw.index('"Show bezel"') <
            video_draw.index('"Bezel"') <
            video_draw.index('"Show debugging"'),
            "video tab must order ghosting below Scanlines, then Video ROM, bezel controls, and Show debugging")
    require("y + (row_h * 8)" in video_draw and
            "y + (row_h * 10)" in video_draw,
            "video tab must leave a blank row between Video ROM and Show bezel")
    require('"Clean phase"' not in main_tabs and
            '"PAL phase"' not in main_tabs and
            "config_menu_adjust_video_phase" not in source,
            "video tab must not expose manual clean/PAL phase adjustment")
    require("return (mono != 0U) ? \"Monochrome\" : \"Color\";" in source,
            "video-output row must toggle between Monochrome and Color")
    require("APPLE_VIDEO_MONO_WHITE,\n"
            "        APPLE_VIDEO_MONO_GREEN,\n"
            "        APPLE_VIDEO_MONO_AMBER" in source,
            "mono color cycling must be white, green, amber")
    require("static uint8_t config_menu_pal_accurate_modes_allowed(const config_menu_t *menu)" in source and
            "menu->platform.is_apple_video_50hz(menu->platform.ctx)" in source and
            "static uint8_t config_menu_next_color_mode(const config_menu_t *menu," in source and
            "config_menu_video_color_mode_allowed(menu, mode)" in source and
            "config_menu_next_color_mode(menu, menu->video_color_mode, delta)" in source and
            "config_menu_next_color_mode(menu, menu->video_color_mode, 1)" in source,
            "color mode row must skip PAL Accurate modes unless the Apple timing register reports 50Hz PAL")
    require("config_menu_video_pal_accurate_help_visible(menu) != 0U" in help_draw and
            '"PAL Accurate modes do not support SHR."' in help_draw and
            "PAL Accurate modes do not support SHR" not in video_draw,
            "PAL Accurate help text must only show for selected PAL Accurate modes in the shared lower help panel")
    require("menu->video7_auto_mono_enabled =\n"
            "            (menu->video7_auto_mono_enabled != 0U) ? 0U : 1U;" in source and
            "config_menu_video7_auto_mono_text(\n"
            "                            menu->video7_auto_mono_enabled)" in main_tabs,
            "Video-7 row must toggle and draw the auto-mono setting")
    require("hgr_draw_video_ghosting_item" in video_draw and
            "cmui_slider" not in ghosting_draw and
            '"Phosphor ghosting"' in ghosting_draw and
            '"(lower FPS)"' in ghosting_draw and
            "CMUI_COLOR_WARN" in ghosting_draw and
            "cmui_text_clipped" in ghosting_draw and
            "menu->video_ghosting_strength =\n"
            "                (uint8_t)((menu->video_ghosting_strength + 1U) %" in source and
            "APPLETINI_VIDEO_GHOSTING_MAX + 1U" in source,
            "video ghosting row must draw like Scanlines, warn about FPS, and cycle through strength settings")
    require("config_menu_apply_video_output(menu);" in source and
            "config_menu_apply_video_ghosting(menu);" in source,
            "runtime apply must publish video and ghosting changes immediately")


def test_frontend_wires_settings_to_cpu1_renderer() -> None:
    frontend_main = read(FRONTEND_MAIN_C)
    renderer = read(APPLE_CYCLE_RENDERER_C)
    config_header = read(CONFIG_MENU_H)
    config_source = read(CONFIG_MENU_C)

    require("static uint8_t g_video_color_mode_shadow = APPLE_VIDEO_COLOR_COMPOSITE_MONITOR;" in frontend_main and
            "static uint8_t g_video7_auto_mono_enable_shadow = 1U;" in frontend_main and
            "static uint8_t g_video_ghosting_shadow = APPLETINI_VIDEO_GHOSTING_OFF;" in frontend_main and
            "static int8_t g_clean_video_phase_cycles_shadow" in frontend_main and
            "static int8_t g_pal_video_phase_cycles_shadow" in frontend_main,
            "frontend shadow state must default to Composite Monitor with Video-7 auto-mono, no ghosting, and timing defaults")
    require("apple_fb_video_settings_set(\n"
            "        apple_video_settings_pack_border_full(" in frontend_main,
            "frontend must publish packed video settings through the shared handoff")
    require("static void menu_platform_set_video_output(void *ctx," in frontend_main and
            "uint8_t video7_auto_mono_enable,\n"
            "                                           int8_t clean_phase_cycles,\n"
            "                                           int8_t pal_phase_cycles)" in frontend_main and
            "video_output_set(mono_enable,\n"
            "                     mono_color,\n"
            "                     color_mode,\n"
            "                     video7_auto_mono_enable,\n"
            "                     clean_phase_cycles,\n"
            "                     pal_phase_cycles);" in frontend_main,
            "boot menu platform callback must update the shared renderer settings")
    require("static uint8_t video_ghosting_get(void) { return g_video_ghosting_shadow; }" in frontend_main and
            "static void    video_ghosting_set(uint8_t strength)" in frontend_main and
            "g_video_ghosting_shadow = appletini_video_ghosting_clamp(strength);" in frontend_main and
            "compositor_set_video_ghosting(g_video_ghosting_shadow);" in frontend_main and
            "static void control_set_video_ghosting(void *ctx, uint8_t strength)" in frontend_main and
            "static uint8_t menu_platform_get_video_ghosting(void *ctx)" in frontend_main,
            "frontend must keep a clamped ghosting shadow and publish it to the compositor")
    require("menu_platform.set_video_output = menu_platform_set_video_output;" in frontend_main and
            "menu_platform.set_video_ghosting = control_set_video_ghosting;" in frontend_main and
            "menu_platform.set_border = menu_platform_set_border;" in frontend_main and
            "menu_platform.get_video_ghosting = menu_platform_get_video_ghosting;" in frontend_main and
            "menu_platform.get_video_output_color_mode = menu_platform_get_video_output_color_mode;" in frontend_main and
            "menu_platform.get_video7_auto_mono_enabled =\n"
            "            menu_platform_get_video7_auto_mono_enabled;" in frontend_main and
            "menu_platform.get_clean_video_phase_cycles =\n"
            "            menu_platform_get_clean_video_phase_cycles;" in frontend_main and
            "menu_platform.get_pal_video_phase_cycles =\n"
            "            menu_platform_get_pal_video_phase_cycles;" in frontend_main and
            "menu_platform.is_apple_video_50hz = menu_platform_is_apple_video_50hz;" in frontend_main and
            "boot_menu_service_is_apple_video_50hz()" in frontend_main,
            "frontend must bind the video-output callbacks into the config menu")
    require("control_set_video_ghosting(NULL, config_menu.video_ghosting_strength);" in frontend_main and
            "snapshot->video_ghosting_strength = video_ghosting_get();" in frontend_main,
            "frontend must apply configured ghosting at boot and report it in the debug snapshot")
    require("void config_menu_draw(uint16_t *fb, const config_menu_t *menu, uint8_t usb_owned);" in config_header and
            "config_menu_draw(fb, menu, g_usb_menu_owned);" in frontend_main and
            "cmui_header(fb,\n"
            "                \"Appletini\",\n"
            "                APPLETINI_FIRMWARE_IMAGE_VERSION_FULL,\n"
            "                usb_owned);" in config_source,
            "menu header must switch ownership state and show the firmware version")
    require("static void apply_video_settings_if_changed(void)" in renderer and
            "const uint32_t settings = apple_fb_video_settings_get();" in renderer and
            "apple_video_settings_video7_auto_mono_enabled(settings)" in renderer and
            "apple_video_settings_clean_phase_cycles(settings)" in renderer and
            "apple_video_settings_pal_phase_cycles(settings)" in renderer and
            "appletini_ntsc_set_video_output(" in renderer and
            "apple_pal_video_set_video_output(" in renderer,
            "CPU1 renderer must consume the shared video settings")
    require("apple_pal_video_mode_is_active(s_render_color_mode)" in renderer and
            "apple_pal_video_on_cycle(pal_line, pal_cycle, sw);" in renderer and
            "s_clean_capture_phase_cycles" in renderer and
            "s_pal_capture_phase_cycles" in renderer and
            "frame-boundary handling above still uses raw" in renderer and
            "apply_video_settings_if_changed();" in renderer,
            "CPU1 renderer must apply settings at frame start and dispatch clean/PAL modes with independent phase calibration")


def test_ntsc_core_implements_modes_and_mono_tints() -> None:
    header = read(NTSC_H)
    source = read(NTSC_C)

    require("void appletini_ntsc_set_video_output(uint8_t mono_enable," in header,
            "NTSC core must expose a runtime video-output setter")
    require("static uint8_t s_video_color_mode = APPLE_VIDEO_COLOR_COMPOSITE_MONITOR;" in source,
            "NTSC core must default to Composite Monitor")
    require("const atn_bgra_t g_aAppleWinBaseColors[16]" in source and
            "static const uint8_t g_aAppleWinPhaseColorIndex[ATN_NUM_PHASES][16]" in source,
            "NTSC core must provide AppleWin-derived crisp Idealized/RGB color paths")
    require("case APPLE_VIDEO_COLOR_IDEALIZED:" in source and
            "case APPLE_VIDEO_COLOR_RGB:" in source and
            "case APPLE_VIDEO_COLOR_TV:" in source and
            "case APPLE_VIDEO_COLOR_COMPOSITE_MONITOR:" in source,
            "NTSC color lookup must handle all clean render/color modes")
    require("APPLE_VIDEO_COLOR_PAL_ACCURATE" not in source,
            "PAL-accurate modes must not be folded into the NTSC color decoder")
    require("case APPLE_VIDEO_MONO_AMBER:" in source and
            "case APPLE_VIDEO_MONO_GREEN:" in source and
            "case APPLE_VIDEO_MONO_WHITE:" in source,
            "NTSC mono lookup must tint white, green, and amber")
    require("return (s_video_mono_enable != 0U) ?\n"
            "        atn_pack_bgra(atn_tint_mono(v)) :\n"
            "        atn_pack_bgra(v);" in source,
            "mono tint color must only affect forced monochrome mode")
    require("if (!atn_get_color_burst() || s_video_mono_enable != 0U) {" in source,
            "forced monochrome must override colorburst rendering")


def test_composite_scratch_rows_have_right_edge_guard() -> None:
    ntsc_h = read(NTSC_H)
    layout_h = read(COMPOSITOR_LAYOUT_H)
    renderer = read(APPLE_CYCLE_RENDERER_C)
    compositor = read(COMPOSITOR_C)

    require("#define ATN_SCRATCH_LEFT_BORDER_PIXELS  4u" in ntsc_h and
            "#define ATN_SCRATCH_RIGHT_BORDER_PIXELS 4u" in ntsc_h,
            "NTSC scratch rows must reserve left and right guard pixels")
    require("ATN_SCRATCH_LEFT_BORDER_PIXELS + \\\n"
            "                                         ATN_SCRATCH_WIDTH + \\\n"
            "                                         ATN_SCRATCH_RIGHT_BORDER_PIXELS" in ntsc_h,
            "NTSC scratch row stride must include both guard regions")
    require("#define COMP_APPLE_LEFT_BORDER_PIXELS  4u" in layout_h and
            "#define COMP_APPLE_RIGHT_BORDER_PIXELS 4u" in layout_h,
            "compositor layout must mirror the renderer's guarded row shape")
    require("COMP_APPLE_LEFT_BORDER_PIXELS + \\\n"
            "                                        COMP_APPLE_VISIBLE_WIDTH + \\\n"
            "                                        COMP_APPLE_RIGHT_BORDER_PIXELS" in layout_h,
            "compositor source stride must include both guard regions")
    require("+ (int)ATN_SCRATCH_LEFT_BORDER_PIXELS" in renderer,
            "renderer must start visible rows after the left guard")
    require("static int applewin_visible_left_shift(render_mode_t mode)" in renderer and
            "return 3;" in renderer and
            "return 2;" in renderer,
            "renderer must centralize AppleWin's shifted visible-window offsets")
    require("static void emit_shifted_right_edge_pixels(render_mode_t mode)" in renderer and
            "atn_emit_blank_pixels((uint8_t)left_shift);" in renderer,
            "renderer must fill only the shifted tail pixels needed for the last visible columns")
    require("void atn_emit_blank_pixels(uint8_t count);" in ntsc_h and
            "void atn_emit_blank_pixels(uint8_t count)" in read(NTSC_C),
            "NTSC core must expose bounded blank-pixel emission for right-edge fill")
    require("src_base + COMP_APPLE_LEFT_BORDER_PIXELS" in compositor,
            "compositor must skip only the left guard before blitting visible pixels")


def test_compositor_can_supersede_pending_frames_for_latency() -> None:
    frontend_main = read(FRONTEND_MAIN_C)
    handoff_h = read(APPLE_FB_HANDOFF_H)
    handoff_c = read(APPLE_FB_HANDOFF_C)
    compositor_h = read(REPO_ROOT / "ps_sources" / "frontend" / "compositor.h")
    compositor = read(COMPOSITOR_C)

    require("uint32_t apple_fb_reader_publish_seq(void);" in handoff_h and
            "uint32_t apple_fb_reader_publish_seq(void)" in handoff_c,
            "handoff must expose the Apple publish sequence for latency-aware compositor pacing")
    require("int compositor_tick(void);" in compositor_h and
            "int compositor_tick(void)" in compositor,
            "compositor_tick must report whether it actually published")
    require("s_composited_apple_seq" in compositor and
            "const int fresh_apple_frame" in compositor and
            "!prior_publish_latched && !fresh_apple_frame" in compositor,
            "compositor must allow a fresh Apple frame to supersede a pending output frame")
    require("(void)compositor_tick();" in frontend_main and
            "ui.frame++;" in frontend_main and
            "lets us supersede the pending output frame" in frontend_main,
            "main loop must call compositor every loop while keeping UI time tied to scanout")


def test_scaled_apple_blits_avoid_uncached_output_readback() -> None:
    fb16 = read(FB16_C)
    vitis = read(VITIS_SCRIPT)

    require("memcpy(drow1, drow0" not in fb16 and
            "memcpy(drow2, drow0" not in fb16 and
            "memcpy(drow3, drow0" not in fb16,
            "scaled Apple blits must not copy from noncached output rows")
    require("static uint16_t s_blit_2x_row[FB16_BLIT_2X_MAX_SRC_W * 2]" in fb16 and
            "void fb16_expand_2x_row_bgra32src(uint16_t *dst, const uint32_t *src," in fb16 and
            "fb16_expand_2x_row_bgra32src(s_blit_2x_row, srow, src_w);" in fb16 and
            "memcpy(drow0, s_blit_2x_row, row_bytes);" in fb16 and
            "memcpy(drow1, s_blit_2x_row, row_bytes);" in fb16 and
            "memcpy(drow2, s_blit_2x_row, row_bytes);" in fb16 and
            "memcpy(drow3, s_blit_2x_row, row_bytes);" in fb16,
            "2x Apple blits must expand into cacheable scratch and copy active rows contiguously")
    require("#if defined(__ARM_NEON)" in fb16 and
            "#include <arm_neon.h>" in fb16 and
            "vld4_u8((const uint8_t *)(src + x))" in fb16 and
            "vsriq_n_u16" in fb16 and
            "vzipq_u16(px, px)" in fb16 and
            "for (; x < src_w; ++x)" in fb16,
            "2x Apple row expansion must NEON-narrow BGRA32 to 565 with a scalar fallback")
    require("def set_userconfig_mfpu" in vitis and
            "if not token.startswith(\"-mfpu=\")" in vitis and
            "Enable NEON on frontend" in vitis and
            "set_userconfig_mfpu(\"frontend\", \"-mfpu=neon\")" in vitis,
            "Vitis workspace generation must reproduce the frontend NEON build flag from git")


def test_compositor_ghosting_is_optional_and_cache_friendly() -> None:
    compositor_h = read(COMPOSITOR_H)
    compositor = read(COMPOSITOR_C)
    debug_h = read(DEBUG_OVERLAY_H)
    debug_c = read(DEBUG_OVERLAY_C)
    ghosting_h = read(VIDEO_GHOSTING_H)

    require(has_define(ghosting_h, "APPLETINI_VIDEO_GHOSTING_OFF", "0U") and
            has_define(ghosting_h, "APPLETINI_VIDEO_GHOSTING_LIGHT", "1U") and
            has_define(ghosting_h, "APPLETINI_VIDEO_GHOSTING_MEDIUM", "2U") and
            has_define(ghosting_h, "APPLETINI_VIDEO_GHOSTING_STRONG", "3U") and
            has_define(ghosting_h, "APPLETINI_VIDEO_GHOSTING_MAX",
                       "APPLETINI_VIDEO_GHOSTING_STRONG") and
            "APPLETINI_VIDEO_GHOSTING_HEAVY" not in ghosting_h and
            "APPLETINI_VIDEO_GHOSTING_HIGH" not in ghosting_h,
            "ghosting must expose Off/Light/Medium/Strong strengths")
    require("void compositor_set_video_ghosting(uint8_t strength);" in compositor_h and
            "uint8_t compositor_video_ghosting(void);" in compositor_h and
            "static uint8_t               s_video_ghosting_strength = APPLETINI_VIDEO_GHOSTING_OFF;" in compositor and
            "strength = appletini_video_ghosting_clamp(strength);" in compositor,
            "compositor must expose and clamp optional ghosting strength")
    require("static uint32_t s_effect_history[EFFECT_HISTORY_PIXELS]" in compositor and
            "static uint32_t s_effect_row[COMP_APPLE_SHR_WIDTH]" in compositor and
            "static uint16_t s_effect_2x_row[COMP_APPLE_SHR_WIDTH * 2U]" in compositor,
            "ghosting blits must use cacheable scratch/history buffers instead of output-row readback")
    require("const uint32_t v = fb16_from_bgra32(p);" in compositor and
            "((uint32_t *)s_effect_2x_row)[x] = (v << 16) | v;" in compositor,
            "ghosting must blend in 8:8:8 scratch and emit the doubled 565 "
            "pair in the same pass (no separate expansion pass)")
    require("static inline uint8_t effect_decay_numer(uint32_t p, uint8_t strength)" in compositor and
            "(p & 0x00808080U)" in compositor and
            "(p & 0x00202020U)" in compositor and
            "EFFECT_NUMER_BRIGHT_LIGHT" in compositor and
            "EFFECT_NUMER_BRIGHT_MEDIUM" in compositor and
            "EFFECT_NUMER_BRIGHT_STRONG" in compositor and
            "k_effect_numer_bright[strength]" in compositor and
            "k_effect_numer_knee[strength]" in compositor and
            "k_effect_numer_tail[strength]" in compositor and
            "const uint8_t decay_numer = effect_decay_numer(*hist, strength);" in compositor and
            "\"usub8 %0, %2, %3\\n\\t\"" in compositor and
            "\"sel %1, %2, %3\\n\\t\"" in compositor and
            "effect_max_rgb(p, effect_scale_64(*hist, decay_numer))" in compositor and
            "*hist = p;" in compositor and
            "effect_mix_3_1" not in compositor and
            "effect_row_weight" not in compositor and
            "effect_write_row" not in compositor,
            "ghosting pipeline must use packed piecewise temporal history and remove blur/soften/phosphor falloff")
    require("if (s_video_ghosting_strength != APPLETINI_VIDEO_GHOSTING_OFF) {\n"
            "            blit_apple_ghosting_2x(fb," in compositor and
            "fb16_blit_2x2_scanlines(fb," in compositor and
            "if (s_video_ghosting_strength != APPLETINI_VIDEO_GHOSTING_OFF) {\n"
            "        blit_apple_ghosting_2x(fb," in compositor and
            "fb16_blit_2x4_scanlines(fb," in compositor,
            "normal Apple blits must stay on the existing fast path until ghosting is enabled")
    require("effect_clear_history();" in compositor and
            "s_force_full_refresh = 1u;" in compositor,
            "changing ghosting must reset temporal state and force a fresh composite")
    require("uint8_t video_ghosting_strength;" in debug_h and
            '"Ghosting %s"' in debug_c and
            "appletini_video_ghosting_name(s->video_ghosting_strength)" in debug_c and
            "APPLETINI_VIDEO_EFFECT" not in debug_c,
            "debug overlay must report the ghosting strength only")


def test_pal_accurate_renderer_model_is_registered() -> None:
    header = read(PAL_TIMING_H)
    source = read(PAL_TIMING_C)
    renderer = read(APPLE_CYCLE_RENDERER_C)
    cpu1 = read(CPU1_MAIN_C)
    vitis = read(VITIS_SCRIPT)

    require("uint8_t apple_pal_video_mode_is_active(uint8_t color_mode);" in header and
            "void apple_pal_video_on_cycle(uint32_t line," in header and
            "void apple_pal_video_preroll_line0_cycle(uint32_t cycle," in header and
            "void apple_pal_video_resync(void);" in header and
            "uint8_t apple_pal_video_end_frame(void);" in header,
            "PAL timing model must expose a line-buffered cycle API and frame-complete status")
    require("#define PAL_LINE_CYCLES          65u" in source and
            "#define PAL_SCANNER_HBL_CYCLES   25u" in source and
            "#define PAL_RENDER_SAMPLES       ATN_ACTIVE_WIDTH" in source and
            "#define PAL_OUTPUT_LATENCY_SAMPLES 14u" in source and
            "#define PAL_OUTPUT_LATENCY_DOUBLE  7u" in source and
            "#define PAL_RENDER_WORK_SAMPLES  (PAL_RENDER_SAMPLES + PAL_OUTPUT_LATENCY_SAMPLES)" in source and
            "#define PAL_RENDER_TICKS         (PAL_RENDER_WORK_SAMPLES * 2u)" in source,
            "PAL timing model must use the Accurapple 65-cycle line and 28 MHz render grid")
    require("#define PAL_SW_GR       0u" in source and
            "#define PAL_SW_MIXED    1u" in source and
            "#define PAL_SW_AN3      2u" in source and
            "#define PAL_SW_TEXT     3u" in source and
            "#define PAL_SW_80COL    4u" in source and
            "#define PAL_SW_HIRES    5u" in source and
            "#define PAL_SW_ALTCHAR  6u" in source and
            "#define PAL_SW_PAGE2    7u" in source,
            "PAL timing model must preserve Accurapple soft-switch bit positions")
    require("C05E sets DHIRES and clears" in source and
            "return (uint8_t)(ace_sw_bit(sw, ACE_SWB_DHIRES_BIT) == 0u);" in source,
            "PAL timing model must translate captured DHIRES polarity to Accurapple AN3 polarity")
    require("delayed_gr2p_hal.future_time = i + 24;" in source and
            "delay_segb = pal_is_set(ssw_old, PAL_SW_HIRES) ? 32 : 30;" in source and
            "delay_gr_time_p = delay_segb + 2;" in source and
            "delayed_segb.future_time = i + 2;" in source,
            "PAL timing model must implement the AN3, TEXT/graphics, and HIRES delays")
    require("pal_build_sw_by_byte(line, next, sw_by_byte);" in source and
            "(uint8_t)((i + 44) / 28);" in source and
            "(uint8_t)((i + 16) / 28);" in source and
            "soft_switches = sw_by_byte[t->idx_load44];" in source and
            "ssw80col = pal_is_set(sw_by_byte[t->idx_load16], PAL_SW_80COL);" in source and
            "ldps_s1" in source and "ldps_s6" in source and
            "vid7m_s1" in source and "vid7m_t123" in source,
            "PAL timing model must port soft-switch sampling, LDPS, and VID7M equations")
    require("pal_read_rom_byte(" in source and
            "appletini_video_rom_read(addr)" in source and
            "uint8_t scanned_bytes[PAL_BYTES_PER_LINE * 2u];" in source and
            "line->scanned_bytes[(x * 2u) + 0u] = g_main_bank[addr];" in source and
            "line->scanned_bytes[(x * 2u) + 1u] = g_aux_bank[addr];" in source and
            "cycle < PAL_SCANNER_HBL_CYCLES" in source and
            "line->ram_sample_mask |= (1ULL << x);" in source,
            "PAL timing model must sample scanned RAM into the line buffer at consumed cycle positions")
    require("s_cache_valid" not in source and
            "s_cache_pixels" not in source and
            "PAL_SIG_BYTES" not in source,
            "PAL timing model must not use a scanline pixel cache")
    require("s_capture_enabled" in source and
            "s_frame_ready_pending" in source and
            "s_preroll_line" in source and
            "s_current_line = s_preroll_line;" in source and
            "apple_pal_video_preroll_line0_cycle" in source and
            "s_frame_lines_rendered != PAL_VISIBLE_LINES" in source and
            "g_pal_frames_dropped++;" in source and
            "g_pal_frames_published++;" in source and
            "apple_pal_video_mode_is_active(s_color_mode) == 0u" in source and
            "line->cycle_mask_lo == 0xFFFFFFFFFFFFFFFFULL" in source and
            "line->ram_sample_mask == PAL_RAM_SAMPLE_MASK" in source,
            "PAL timing model must publish only complete delayed frames without stale scanlines")
    require("#define PAL_RENDER_SAMPLES       ATN_ACTIVE_WIDTH" in source and
            "static uint32_t s_render_line[PAL_RENDER_WORK_SAMPLES];" in source and
            "&s_render_line[latency]" in source and
            "PAL_OUTPUT_LATENCY_DOUBLE : PAL_OUTPUT_LATENCY_SAMPLES" in source and
            "PAL_RENDER_SAMPLES * sizeof(uint32_t)" in source and
            "PAL_ENABLE_FLOAT_TV_DECODE" not in source and
            "pal_decode_composite_pixel" not in source and
            "pal_write_rgb_line" not in source and
            "s_composite" not in source and
            "s_colour_burst" not in source and
            "s_filters" not in source and
            "s_pal_hue_color_tv_packed : s_pal_hue_monitor_packed" in source and
            "s_pal_bw_color_tv_packed" in source,
            "PAL Accurate TV must use the AppleWin color-TV table and copy the latency-shifted visible window")
    require("pal_sw_lookahead_uniform" in source and
            "pal_render_composite_line_uniform" in source and
            "g_pal_fast_lines++;" in source and
            "g_pal_slow_lines++;" in source and
            "byte_group" in source and
            "pos == 12u" in source and
            "ldps_s = (uint8_t)(gr2p | segb | not_vid7);" in source and
            "if (vid7m_s != 0u)" in source and
            "if (dst == NULL)" in source,
            "PAL renderer must keep an exact byte-group constant-soft-switch fast path plus fallback renderer")
    require("#define PAL_RENDER_TIMING_PROFILE 0u" in source and
            "#if PAL_RENDER_TIMING_PROFILE" in source and
            "g_pal_render_ticks_total" in source and
            "g_pal_render_ticks_max" in source and
            "pal_profile_record_render_ticks" in source,
            "PAL renderer timing diagnostics must be available but compiled out by default")
    require("#define PAL_FLUSH_MAX_LINES      64u" in source and
            "g_pal_end_queue_total" in source and
            "g_pal_end_queue_max" in source and
            "g_pal_end_lines_drained" in source,
            "PAL renderer must expose frame-end queue/drain diagnostics")
    require("#define CPU1_STATUS_UART 0" in cpu1 and
            "#if CPU1_STATUS_UART" in cpu1 and
            "cpu1_emit_status(dt_ms);" in cpu1 and
            "palus=" in cpu1 and
            "palend=" in cpu1,
            "CPU1 health-line diagnostics must remain available but compile out by default")
    require("#include \"apple_pal_video_timing.h\"" in renderer and
            "apple_pal_video_resync();" in renderer and
            "apple_pal_video_on_cycle(pal_line, pal_cycle, sw);" in renderer and
            "pal_positive_phase_preroll_cycle(line," in renderer and
            "apple_pal_video_preroll_line0_cycle(pal_preroll_cycle, sw);" in renderer and
            "raw_line < (uint32_t)ATN_SCANNER_Y_DISPLAY" in renderer and
            "const uint8_t pal_frame_ready = apple_pal_video_end_frame();" in renderer and
            "if (pal_frame_ready != 0u) {" in renderer and
            "s_pal_capture_phase_cycles" in renderer and
            "capture_to_scanner_phase(line,\n"
            "                             cycle,\n"
            "                             s_clean_capture_phase_cycles," in renderer,
            "cycle renderer must include, dispatch, and only publish completed PAL frames")
    require("../../../ps_sources/frontend/apple_pal_video_timing.c" in vitis,
            "frontend_core1 Vitis build must compile the PAL timing source")


TESTS = [
    test_shared_video_output_contract,
    test_menu_persists_video_output_settings,
    test_border_flood_excludes_bezel_and_debugging,
    test_video_help_overrides_every_row,
    test_boot_menu_groups_boot_and_video_settings,
    test_frontend_wires_settings_to_cpu1_renderer,
    test_ntsc_core_implements_modes_and_mono_tints,
    test_composite_scratch_rows_have_right_edge_guard,
    test_compositor_can_supersede_pending_frames_for_latency,
    test_scaled_apple_blits_avoid_uncached_output_readback,
    test_compositor_ghosting_is_optional_and_cache_friendly,
    test_pal_accurate_renderer_model_is_registered,
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
        print(f"{len(TESTS) - len(failures)} of {len(TESTS)} video-output tests passed; "
              f"{len(failures)} failed")
        return 1
    print(f"{len(TESTS)} video-output tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
