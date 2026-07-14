#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xiltimer.h"
#include "xil_cache.h"
#include "xil_mmu.h"
#include "xparameters.h"
#include "xstatus.h"

#include "../image_versions.h"
#include "../fw_update_metadata.h"

#include "../lib/fb16.h"
#include "../lib/framebuffer.h"
#include "../lib/uart.h"
#include "../lib/common.h"
#include "../lib/i2c.h"
#include "../lib/dac_ak4493.h"
#include "../lib/tmp102.h"
#include "../lib/rtc_pcf8563.h"
#include "../lib/qspi_nor.h"

#include "uart_control.h"
#include "card_control_regs.h"
#include "uthernet2_control.h"
#include "bezel_default_png.h"
#include "bezel_loader.h"
#include "boot_menu_service.h"
#include "applicard_service.h"
#include "compositor.h"
#include "compositor_layout.h"
#include "config_menu.h"
#include "debug_overlay.h"
#include "disk2_service.h"
#include "disk2_sound_samples.h"
#include "psram_bench.h"
#include "screenshot_service.h"
#include "smartport_service.h"
#include "usb_hid_service.h"
#include "usb_storage_backend.h"
#include "usb_storage_service.h"
#include "usb_sdd_service.h"
#include "video_ghosting.h"
#include "no_slot_clock_control.h"
#include "gic_init.h"
#include "apple_cycle_egress.h"
#include "apple_cycle_renderer.h"
#include "apple_fb_handoff.h"
#include "../lib/amp.h"

/*
    The AXI registers and PSRAM windows are configured as such:

    localparam [31:0] REG_BASE   = 32'h4500_0000;
    localparam [31:0] PSRAM_BASE = 32'h4600_0000;
    localparam [31:0] PSRAM_MASK = 32'hFF00_0000;

*/

#define APPLE_VIEW_BORDER 8
#define APPLE_VIEW_W ((int)COMP_SUBWIN_WIDTH + (APPLE_VIEW_BORDER*2))
#define APPLE_VIEW_H ((int)COMP_SUBWIN_HEIGHT + (APPLE_VIEW_BORDER*2))
#define APPLE_VIEW_X ((int)COMP_SUBWIN_X_OFF - APPLE_VIEW_BORDER)
#define APPLE_VIEW_Y ((int)COMP_SUBWIN_Y_OFF - APPLE_VIEW_BORDER)

#define UI_BEZEL_SD_PATH "0:/bezel.png"
#define UI_BEZEL_SD_FALLBACK_PATH "0:/bezels/bezel.png"
#define UI_BEZEL_BG_COLOR FB16_RGB(0x14, 0x18, 0x20)
#define UI_DISK_ACTIVITY_HOLD_FRAMES 8U
#define UI_DISK_WRITE_HOLD_FRAMES 16U
#define UI_DISK_ACTIVITY_W 360
#define UI_DISK_ACTIVITY_H 28
#define UI_DISK_ACTIVITY_X (APPLE_VIEW_X + APPLE_VIEW_W - UI_DISK_ACTIVITY_W)
#define UI_DISK_ACTIVITY_Y (APPLE_VIEW_Y - UI_DISK_ACTIVITY_H - 10)
#define USB1_BOOT_SETTLE_QUIET_US 750000U
#define USB1_BOOT_HOLD_TIMEOUT_TICKS 0xFFFFFFFFU

typedef struct {
    uint32_t frame;
    uint32_t key_count;
    uint32_t input_seq;
    ui_key_t last_key;
} ui_state_t;

typedef struct {
    uint8_t active;
    uint32_t last_count;
    XTime last_change;
} usb1_boot_settle_t;

static uint8_t ui_config_menu_has_close_consumer(const config_menu_t *menu);
static uint8_t ui_input_requests_menu_close(ui_input_t input);

static XIicPs g_i2c;
static uint8_t g_i2c_ready = 0U;
static uint8_t g_tmp102_ready = 0U;
static tmp102_temp_t g_temp = {0};
static rtc_pcf8563_time_t g_rtc = {0};
static uint32_t g_fps_x100 = 0U;
static uint32_t g_apple_fps_x100 = 0U;
static uint32_t g_compositor_fps_x100 = 0U;
static uint32_t g_renderer_fps_x100 = 0U;
static uint32_t g_renderer_publish_seq = 0U;
static XTime g_last_draw_ticks = 0U;
static usb1_boot_settle_t g_usb1_boot_settle = {0};
static fw_update_metadata_t g_update_meta = {0};
static uint8_t g_update_meta_valid = 0U;
static uint8_t g_usb_menu_owned = 0U;
static uint16_t *g_bezel_565 = NULL;
static unsigned g_bezel_width = 0U;
static unsigned g_bezel_height = 0U;
static uint32_t g_bezel_generation = 1U;
static uint32_t g_output_slot_bg_generation[COMP_OUT_SLOT_COUNT];
static uint8_t g_output_slot_bg_show_bezel[COMP_OUT_SLOT_COUNT];
static uint8_t g_output_slot_apple_mode[COMP_OUT_SLOT_COUNT];
static uint8_t g_output_slot_debug_dirty[COMP_OUT_SLOT_COUNT];
static char g_bezel_status[160] = "not loaded";

typedef struct {
    uint8_t initialized;
    uint8_t source;
    uint8_t disk2_last_unit;
    uint8_t smartport_last_unit;
    uint32_t disk2_last_read_count;
    uint32_t disk2_last_write_count;
    uint32_t disk2_read_until_frame;
    uint32_t disk2_write_until_frame;
    uint32_t smartport_last_status_count;
    uint32_t smartport_last_read_count;
    uint32_t smartport_last_write_count;
    uint32_t smartport_status_until_frame;
    uint32_t smartport_read_until_frame;
    uint32_t smartport_write_until_frame;
} ui_storage_activity_state_t;

#define UI_STORAGE_SOURCE_DISK2     0U
#define UI_STORAGE_SOURCE_SMARTPORT 1U

static ui_storage_activity_state_t g_storage_activity = {0};

static int ui_vsync_enable = 1;
static volatile uint8_t g_rtc_write_pending = 0U;
static uint8_t g_audio_enable = 1U;
static uint8_t g_audio_mute = 0U;
static uint32_t g_audio_tone_hz = 1000U;
static uint32_t g_audio_amp = 0x00600000U;
static uart_control_t g_uart_control;
static uart_control_t g_uart0_control;
static ui_state_t *g_ui_state = NULL;
static config_menu_t *g_config_menu_state = NULL;

/* Config-menu RAM enable, consumed by the machine aux-provide policy
 * (boot_menu_service). Defaults ON when the menu is not yet up. */
uint8_t appletini_config_ram_enabled(void)
{
    return (g_config_menu_state == NULL) ? 1U
         : (g_config_menu_state->ram_enabled != 0U) ? 1U : 0U;
}

/* SmartPort volatile RAM disk enable. Defaults OFF. */
uint8_t appletini_config_sp_ramdisk(void)
{
    return (g_config_menu_state == NULL) ? 0U
         : (g_config_menu_state->sp_ramdisk_enabled != 0U) ? 1U : 0U;
}

/* RamWorks 8MB expansion enable (RAM tab). Defaults OFF. */
uint8_t appletini_config_ramworks_enabled(void)
{
    return (g_config_menu_state == NULL) ? 0U
         : (g_config_menu_state->ramworks_enabled != 0U) ? 1U : 0U;
}
static uint32_t g_card_slot_enable_mask = CARD_CTRL_SLOT_ENABLE_RESET_MASK;
static uint8_t g_apple_reset_seq_valid = 0U;
static uint8_t g_apple_reset_seq_last = 0U;

static void ui_invalidate_static_backgrounds(void);

#define PSRAM_EXECUTE_CMD (PSRAM_CONTROL_BASE + 0x00U)
#define PSRAM_ADDR (PSRAM_CONTROL_BASE + 0x04U)
#define PSRAM_WRDATA_LO (PSRAM_CONTROL_BASE + 0x08U)
#define PSRAM_WRDATA_HI (PSRAM_CONTROL_BASE + 0x0CU)
#define PSRAM_RDDATA_LO (PSRAM_CONTROL_BASE + 0x10U)
#define PSRAM_RDDATA_HI (PSRAM_CONTROL_BASE + 0x14U)
#define PSRAM_STATUS (PSRAM_CONTROL_BASE + 0x18U)
#define PSRAM_CYCLE_COUNT (PSRAM_CONTROL_BASE + 0x1CU)
#define PSRAM_DELAY (PSRAM_CONTROL_BASE + 0x20U)
#define PSRAM_TMP (PSRAM_CONTROL_BASE + 0x3FCU)

static psram_ui_state_t g_psram = {0};
static uint8_t g_scanlines_mode_shadow = APPLETINI_SCANLINES_OFF;
static uint8_t g_video_ghosting_shadow = APPLETINI_VIDEO_GHOSTING_OFF;
static uint8_t g_text_mono_fg_color_shadow = 0U;
static uint8_t g_text_mono_bg_color_shadow = 0U;
static uint8_t g_display_mono_enable_shadow = 0U;
static uint8_t g_display_mono_color_shadow = APPLE_VIDEO_MONO_WHITE;
static uint8_t g_video_color_mode_shadow = APPLE_VIDEO_COLOR_COMPOSITE_MONITOR;
static uint8_t g_video7_auto_mono_enable_shadow = 1U;
static uint8_t g_border_enabled_shadow = 0U;
static uint8_t g_border_color_shadow = APPLE_VIDEO_IIGS_BORDER_DEFAULT;
static uint8_t g_border_flood_shadow = 0U;
static int8_t g_clean_video_phase_cycles_shadow =
    (int8_t)APPLE_VIDEO_DEFAULT_CLEAN_PHASE_CYCLES;
static int8_t g_pal_video_phase_cycles_shadow =
    (int8_t)APPLE_VIDEO_DEFAULT_PAL_PHASE_CYCLES;

static uint32_t ticks_to_us(XTime ticks)
{
    if (COUNTS_PER_SECOND == 0U) {
        return 0U;
    }
    return (uint32_t)((ticks * 1000000ULL) / (uint64_t)COUNTS_PER_SECOND);
}

static uint32_t ui_softswitch_state(void)
{
    return REG_READ(CARD_CTRL_SOFTSW_STATE_REG);
}

static uint32_t crc32_local_init(void)
{
    return 0xFFFFFFFFU;
}

static uint32_t crc32_local_update(uint32_t crc, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t i;
    uint32_t j;
    for (i = 0U; i < len; ++i) {
        crc ^= (uint32_t)p[i];
        for (j = 0U; j < 8U; ++j) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return crc;
}

static uint32_t crc32_local_finish(uint32_t crc)
{
    return crc ^ 0xFFFFFFFFU;
}

static int qspi_read_bytes_direct(uint32_t flash_addr, void *dst, uint32_t len)
{
    qspi_nor_t nor;

    if (qspi_nor_init(&nor, 3U, APPLETINI_FLASH_METADATA_SIZE) != XST_SUCCESS) {
        return XST_FAILURE;
    }
    return qspi_nor_read(&nor, flash_addr, dst, len);
}

static uint8_t update_metadata_read_from_qspi(fw_update_metadata_t *out)
{
    fw_update_metadata_t tmp;
    uint32_t calc_crc;
    volatile const uint8_t *src;
    uint8_t *dst = (uint8_t *)&tmp;
    uint32_t i;

    if (out == NULL) {
        return 0U;
    }

    /* Try QSPI linear window first. */
    src = (volatile const uint8_t *)(uintptr_t)(0xFC000000U + APPLETINI_FLASH_METADATA_OFFSET);
    Xil_DCacheInvalidateRange((INTPTR)src, (INTPTR)sizeof(tmp));
    for (i = 0U; i < (uint32_t)sizeof(tmp); ++i) {
        dst[i] = src[i];
    }

    if (!(tmp.magic == FW_UPDATE_METADATA_MAGIC &&
          tmp.version == FW_UPDATE_METADATA_VERSION &&
          tmp.length_bytes == (uint32_t)sizeof(tmp))) {
        /* Fallback: direct QSPI command-mode read (works even if linear mode is not configured). */
        if (qspi_read_bytes_direct(APPLETINI_FLASH_METADATA_OFFSET, &tmp, (uint32_t)sizeof(tmp)) != XST_SUCCESS) {
            return 0U;
        }
    }

    if (tmp.magic != FW_UPDATE_METADATA_MAGIC ||
        tmp.version != FW_UPDATE_METADATA_VERSION ||
        tmp.length_bytes != (uint32_t)sizeof(tmp)) {
        return 0U;
    }

    calc_crc = crc32_local_init();
    calc_crc = crc32_local_update(calc_crc, &tmp, (uint32_t)sizeof(tmp) - 4U);
    calc_crc = crc32_local_finish(calc_crc);
    if (calc_crc != tmp.crc32) {
        return 0U;
    }

    *out = tmp;
    return 1U;
}

/* Text-mono colors are software-owned settings. Display-mono/video-style
 * settings are
 * published to CPU1 through apple_fb_handoff's shared renderer word. */

static uint8_t text_mono_fg_color_get(void) { return g_text_mono_fg_color_shadow; }
static uint8_t text_mono_bg_color_get(void) { return g_text_mono_bg_color_shadow; }
static void    text_mono_set_colors(uint8_t fg, uint8_t bg)
{
    g_text_mono_fg_color_shadow = fg;
    g_text_mono_bg_color_shadow = bg;
}

static uint8_t display_mono_enable_get(void) { return g_display_mono_enable_shadow; }
static uint8_t display_mono_color_get(void)  { return g_display_mono_color_shadow; }
static uint8_t video_color_mode_get(void) { return g_video_color_mode_shadow; }
static uint8_t video7_auto_mono_enable_get(void) { return g_video7_auto_mono_enable_shadow; }
static int8_t clean_video_phase_cycles_get(void) { return g_clean_video_phase_cycles_shadow; }
static int8_t pal_video_phase_cycles_get(void) { return g_pal_video_phase_cycles_shadow; }

static void video_output_publish(void)
{
    apple_fb_video_settings_set(
        apple_video_settings_pack_border_full(
            g_display_mono_enable_shadow,
            g_display_mono_color_shadow,
            g_video_color_mode_shadow,
            g_video7_auto_mono_enable_shadow,
            g_clean_video_phase_cycles_shadow,
            g_pal_video_phase_cycles_shadow,
            g_border_enabled_shadow,
            g_border_color_shadow,
            g_border_flood_shadow));
}

static void    display_mono_set(uint8_t enable, uint8_t color)
{
    g_display_mono_enable_shadow = (enable != 0U) ? 1U : 0U;
    g_display_mono_color_shadow  = apple_video_mono_color_clamp(color);
    video_output_publish();
}

static void video_output_set(uint8_t mono_enable,
                             uint8_t mono_color,
                             uint8_t color_mode,
                             uint8_t video7_auto_mono_enable,
                             int8_t clean_phase_cycles,
                             int8_t pal_phase_cycles)
{
    g_display_mono_enable_shadow = (mono_enable != 0U) ? 1U : 0U;
    g_display_mono_color_shadow = apple_video_mono_color_clamp(mono_color);
    g_video_color_mode_shadow = apple_video_color_mode_clamp(color_mode);
    g_video7_auto_mono_enable_shadow = (video7_auto_mono_enable != 0U) ? 1U : 0U;
    g_clean_video_phase_cycles_shadow =
        apple_video_timing_phase_clamp(clean_phase_cycles);
    g_pal_video_phase_cycles_shadow =
        apple_video_timing_phase_clamp(pal_phase_cycles);
    video_output_publish();
}

static uint8_t scanlines_get(void) { return g_scanlines_mode_shadow; }
static void    scanlines_set(uint8_t mode)
{
    g_scanlines_mode_shadow = appletini_scanlines_clamp(mode);
    compositor_set_scanlines(g_scanlines_mode_shadow);
    screenshot_service_set_scanlines(g_scanlines_mode_shadow);
}

static uint8_t video_ghosting_get(void) { return g_video_ghosting_shadow; }
static void    video_ghosting_set(uint8_t strength)
{
    g_video_ghosting_shadow = appletini_video_ghosting_clamp(strength);
    compositor_set_video_ghosting(g_video_ghosting_shadow);
}

static void border_set(uint8_t enabled, uint8_t color, uint8_t flood)
{
    g_border_enabled_shadow = (enabled != 0u) ? 1u : 0u;
    g_border_color_shadow = apple_video_iigs_border_color_clamp(color);
    g_border_flood_shadow = (flood != 0u) ? 1u : 0u;
    video_output_publish();
    compositor_set_border(g_border_enabled_shadow, g_border_flood_shadow);
}

static void psram_wait_status() {
    int count = 50;
    while (count > 0) {
        count--;
        uint32_t ret = REG_READ(PSRAM_STATUS);
        uart_puts(UART0_BASE, "status:");
        uart_puthex(UART0_BASE, ret);
        uart_puts(UART0_BASE, "\r\n");
        if ( (ret & 0x0000ffff) != 0) break;
    }
    
}

static void psram_reset(void* ctx) {
    uart_puts(UART0_BASE, "psram reset\r\n");
    REG_WRITE(PSRAM_EXECUTE_CMD,0x66);
    psram_wait_status();
    REG_WRITE(PSRAM_EXECUTE_CMD,0x99);
    psram_wait_status();
}

static void psram_qpi(void* ctx) {
    uart_puts(UART0_BASE, "psram enable qpi cmd\r\n");
    REG_WRITE(PSRAM_EXECUTE_CMD,0x35);
    psram_wait_status();
    uint32_t cycles = REG_READ(PSRAM_CYCLE_COUNT);
    uart_puts(UART0_BASE, " cycles:");
    uart_putdec(UART0_BASE, cycles);
    uart_puts(UART0_BASE, "\r\n");
}

static void psram_qpi_exit(void* ctx) {
    uart_puts(UART0_BASE, "psram disable qpi cmd\r\n");
    REG_WRITE(PSRAM_EXECUTE_CMD,0xF5);
    psram_wait_status();
    uint32_t cycles = REG_READ(PSRAM_CYCLE_COUNT);
    uart_puts(UART0_BASE, " cycles:");
    uart_putdec(UART0_BASE, cycles);
    uart_puts(UART0_BASE, "\r\n");
}

static void psram_toggle_wrap(void* ctx) {
    uart_puts(UART0_BASE, "psram toggle wrap\r\n");
    REG_WRITE(PSRAM_EXECUTE_CMD,0xC0);
    psram_wait_status();
    uint32_t cycles = REG_READ(PSRAM_CYCLE_COUNT);
    uart_puts(UART0_BASE, " cycles:");
    uart_putdec(UART0_BASE, cycles);
    uart_puts(UART0_BASE, "\r\n");
}

static void psram_qspi_read(void* ctx, const char* addr) {
    uint32_t uaddr = strtoul(addr,NULL,16);
    REG_WRITE(PSRAM_ADDR,uaddr);
    REG_WRITE(PSRAM_EXECUTE_CMD,0xEB);
    psram_wait_status();
    uint32_t read_lo = REG_READ(PSRAM_RDDATA_LO);
    uint32_t read_hi = REG_READ(PSRAM_RDDATA_HI);
    uint32_t cycles = REG_READ(PSRAM_CYCLE_COUNT);
    uart_puts(UART0_BASE, "read addr: ");
    uart_puthex(UART0_BASE, uaddr);
    uart_puts(UART0_BASE, " value:");
    uart_puthex(UART0_BASE, read_hi);
    uart_puthex(UART0_BASE, read_lo);
    uart_puts(UART0_BASE, " cycles:");
    uart_putdec(UART0_BASE, cycles);
    uart_puts(UART0_BASE, "\r\n");
}

static void psram_qspi_write(void* ctx, const char* addr, const char* datahi, const char* datalo) {
    uint32_t uaddr = strtoul(addr,NULL,16);
    REG_WRITE(PSRAM_ADDR,uaddr);
    uint32_t uhi = strtoul(datahi, NULL, 16);
    uint32_t ulo = strtoul(datalo, NULL, 16);
    uart_puts (UART0_BASE, "attempt to write: ");
    uart_puthex(UART0_BASE, uhi);
    uart_puthex(UART0_BASE, ulo);
    uart_puts(UART0_BASE, "\r\n");
    REG_WRITE(PSRAM_WRDATA_HI,uhi);
    REG_WRITE(PSRAM_WRDATA_LO,ulo);
    REG_WRITE(PSRAM_EXECUTE_CMD,0x38);
    psram_wait_status();
    uint32_t cycles = REG_READ(PSRAM_CYCLE_COUNT);
    uart_puts(UART0_BASE, "wraddr addr: ");
    uart_puthex(UART0_BASE, uaddr);
    uart_puts(UART0_BASE, " cycles:");
    uart_putdec(UART0_BASE, cycles);
    uart_puts(UART0_BASE, "\r\n");
}

static void psram_spi_read(void* ctx, const char* addr) {
    uart_puts(UART0_BASE,"(");
    uart_puts(UART0_BASE,addr);
    uart_puts(UART0_BASE,")\r\n");
    uint32_t uaddr = strtoul(addr,NULL,16);
    REG_WRITE(PSRAM_ADDR,uaddr);
    REG_WRITE(PSRAM_EXECUTE_CMD,0x0B);
    psram_wait_status();
    uint32_t read_lo = REG_READ(PSRAM_RDDATA_LO);
    uint32_t read_hi = REG_READ(PSRAM_RDDATA_HI);
    uint32_t cycles = REG_READ(PSRAM_CYCLE_COUNT);
    uart_puts(UART0_BASE, "read addr: ");
    uart_puthex(UART0_BASE, uaddr);
    uart_puts(UART0_BASE, " value:");
    uart_puthex(UART0_BASE, read_hi);
    uart_puthex(UART0_BASE, read_lo);
    uart_puts(UART0_BASE, " cycles:");
    uart_putdec(UART0_BASE, cycles);
    uart_puts(UART0_BASE, "\r\n");
}

static void psram_read_id(void* ctx) {
    REG_WRITE(PSRAM_EXECUTE_CMD,0x9F);
    psram_wait_status();
    uint32_t read_lo = REG_READ(PSRAM_RDDATA_LO);
    uint32_t read_hi = REG_READ(PSRAM_RDDATA_HI);
    uint32_t cycles = REG_READ(PSRAM_CYCLE_COUNT);
    uart_puts(UART0_BASE, "read_id: ");
    uart_puthex(UART0_BASE, read_hi);
    uart_puthex(UART0_BASE, read_lo);
    uart_puts(UART0_BASE, " cycles:");
    uart_putdec(UART0_BASE, cycles);
    uart_puts(UART0_BASE, "\r\n");
}

static void psram_spi_write(void* ctx, const char* addr, const char* datahi, const char* datalo) {
    uint32_t uaddr = strtoul(addr,NULL,16);
    REG_WRITE(PSRAM_ADDR,uaddr);
    uint32_t uhi = strtoul(datahi, NULL, 16);
    uint32_t ulo = strtoul(datalo, NULL, 16);
    uart_puts (UART0_BASE, "attempt to write: ");
    uart_puthex(UART0_BASE, uhi);
    uart_puthex(UART0_BASE, ulo);
    uart_puts(UART0_BASE, "\r\n");
    REG_WRITE(PSRAM_WRDATA_HI,uhi);
    REG_WRITE(PSRAM_WRDATA_LO,ulo);
    REG_WRITE(PSRAM_EXECUTE_CMD,0x02);
    psram_wait_status();
    uint32_t cycles = REG_READ(PSRAM_CYCLE_COUNT);
    uart_puts(UART0_BASE, "wraddr addr: ");
    uart_puthex(UART0_BASE, uaddr);
    uart_puts(UART0_BASE, " cycles:");
    uart_putdec(UART0_BASE, cycles);
    uart_puts(UART0_BASE, "\r\n");
}

static void psram_set_delay(void* ctx, const char* delay) {
    uint32_t idelay = strtoul(delay, NULL, 16);
    REG_WRITE(PSRAM_DELAY, idelay);
}

static void psram_scan_delay(void* ctx, const char* addr) {
    uint32_t idelay;
    uart_puts(UART0_BASE,"attempting read scan\r\n");
    for (idelay = 0; idelay < 64; ++idelay) {
        REG_WRITE(PSRAM_DELAY, idelay);
        uint32_t uaddr = strtoul(addr,NULL,16);
        REG_WRITE(PSRAM_ADDR,uaddr);
        REG_WRITE(PSRAM_EXECUTE_CMD,0x9F);
        psram_wait_status();
        uint32_t read_lo = REG_READ(PSRAM_RDDATA_LO);
        uint32_t read_hi = REG_READ(PSRAM_RDDATA_HI);
        uint32_t cycles = REG_READ(PSRAM_CYCLE_COUNT);
        uart_puts(UART0_BASE, "delay: ");
        uart_puthex(UART0_BASE, idelay);
        uart_puts(UART0_BASE, " read addr: ");
        uart_puthex(UART0_BASE, uaddr);
        uart_puts(UART0_BASE, " value:");
        uart_puthex(UART0_BASE, read_hi);
        uart_puthex(UART0_BASE, read_lo);
        uart_puts(UART0_BASE, " cycles:");
        uart_putdec(UART0_BASE, cycles);
        uart_puts(UART0_BASE, "\r\n");
    }
}

static void menu_chime_play(void)
{
    REG_WRITE(CARD_CTRL_MENU_CHIME_REG, 1U);
}

static void audio_apply_config(void)
{
    (void)g_audio_enable;
    (void)g_audio_mute;
    (void)g_audio_tone_hz;
    (void)g_audio_amp;
}

static uint8_t ui_apple_video_50hz(const config_menu_t *menu)
{
    return (menu != NULL && menu->platform.is_apple_video_50hz != NULL) ?
        menu->platform.is_apple_video_50hz(menu->platform.ctx) : 0U;
}

static int control_get_snapshot(void *ctx, uart_control_snapshot_t *snapshot)
{
    (void)ctx;
    if (snapshot == NULL || g_ui_state == NULL) {
        return -1;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->frame_count = g_ui_state->frame;
    snapshot->key_count = g_ui_state->key_count;
    if (g_config_menu_state != NULL) {
        snapshot->config_boot_device = g_config_menu_state->boot_device;
        snapshot->config_boot_timeout_mode = g_config_menu_state->boot_timeout_mode;
        snapshot->config_disk2_slot6_enabled =
            g_config_menu_state->disk2_slot6_enabled;
        snapshot->config_settings_loaded = g_config_menu_state->settings_loaded;
        snapshot->config_session_only = g_config_menu_state->session_only;
    }
    snapshot->vsync_enable = (uint8_t)(ui_vsync_enable ? 1 : 0);
    snapshot->scanlines_mode = scanlines_get();
    snapshot->text_mono_fg_color = text_mono_fg_color_get();
    snapshot->text_mono_bg_color = text_mono_bg_color_get();
    snapshot->display_mono_enable = display_mono_enable_get();
    snapshot->display_mono_color = display_mono_color_get();
    snapshot->apple_fb_slot = g_compositor_last_apple_slot;
    snapshot->apple_fb_mode = g_compositor_last_apple_mode;
    snapshot->apple_video_50hz = ui_apple_video_50hz(g_config_menu_state);
    snapshot->fps_x100 = g_fps_x100;
    snapshot->apple_fps_x100 = g_apple_fps_x100;
    snapshot->apple_fb_blits = g_compositor_apple_frames_drawn;
    snapshot->compositor_frames_published = g_compositor_frames_published;
    snapshot->compositor_frames_skipped = g_compositor_frames_skipped;
    snapshot->fb_frame_count = REG_READ(FB_STATUS_REG);
    snapshot->fb_last_latched = REG_READ(FB_LAST_LATCHED_REG);
    snapshot->i2c_ready = g_i2c_ready;
    snapshot->tmp102_ready = g_tmp102_ready;
    snapshot->temp = g_temp;
    snapshot->rtc = g_rtc;
    snapshot->audio_enable = g_audio_enable;
    snapshot->audio_mute = g_audio_mute;
    snapshot->audio_tone_hz = g_audio_tone_hz;
    snapshot->audio_amp = g_audio_amp;
    snapshot->audio_clkcnt = 0;
    snapshot->audio_q3lvl = 0;
    snapshot->audio_q3tog = 0;
    snapshot->updater_meta_valid = g_update_meta_valid;
    memset(snapshot->updater_golden_version, 0, sizeof(snapshot->updater_golden_version));
    memset(snapshot->updater_firmware_version, 0, sizeof(snapshot->updater_firmware_version));
    if (g_update_meta_valid) {
        (void)snprintf(snapshot->updater_golden_version, sizeof(snapshot->updater_golden_version),
                       "%s", g_update_meta.golden_version);
        (void)snprintf(snapshot->updater_firmware_version, sizeof(snapshot->updater_firmware_version),
                       "%s", APPLETINI_FIRMWARE_IMAGE_VERSION_FULL);
    }
    return 0;
}

static int control_set_rtc(void *ctx, const rtc_pcf8563_time_t *t)
{
    (void)ctx;
    if (!g_i2c_ready || t == NULL) {
        return -1;
    }
    if (rtc_pcf8563_write_time(&g_i2c, t) != 0) {
        return -1;
    }
    g_rtc = *t;
    g_rtc.valid = 1U;
    g_rtc.status = 0U;
    no_slot_clock_control_publish_rtc(&g_rtc);
    g_rtc_write_pending = 1U;
    return 0;
}

static void control_set_vsync(void *ctx, uint8_t enable)
{
    (void)ctx;
    ui_vsync_enable = (enable != 0U) ? 1 : 0;
}

static void control_set_scanlines(void *ctx, uint8_t mode)
{
    (void)ctx;
    scanlines_set(mode);
}

static void control_set_video_ghosting(void *ctx, uint8_t strength)
{
    (void)ctx;
    video_ghosting_set(strength);
}

static uint32_t card_control_normalize_slot_mask(uint32_t slot_mask)
{
    return (slot_mask & CARD_CTRL_SLOT_ENABLE_VALID_MASK) |
           CARD_CTRL_SLOT_ENABLE_REQUIRED_MASK;
}

static void card_control_sync_slot_mask_from_hw(void)
{
    g_card_slot_enable_mask =
        card_control_normalize_slot_mask(REG_READ(CARD_CTRL_SLOT_ENABLE_REG));
}

static void card_control_write_slot_mask(uint32_t slot_mask)
{
    g_card_slot_enable_mask = card_control_normalize_slot_mask(slot_mask);
    REG_WRITE(CARD_CTRL_SLOT_ENABLE_REG, g_card_slot_enable_mask);
}

static uint8_t card_control_read_apple_reset_seq(void)
{
    return (uint8_t)(REG_READ(CARD_CTRL_APPLE_RESET_STATUS_REG) &
                     CARD_CTRL_APPLE_RESET_SEQ_MASK);
}

static uint8_t disk2_sound_volume_clamp(uint8_t volume)
{
    return (volume > CARD_CTRL_DISK2_SOUND_MAX_VOLUME) ?
        CARD_CTRL_DISK2_SOUND_MAX_VOLUME : volume;
}

static uint8_t g_disk2_sound_volume = CARD_CTRL_DISK2_SOUND_DEFAULT_VOLUME;

static uint32_t card_control_pack_disk2_sound_control(uint8_t volume)
{
    const uint8_t clamped = disk2_sound_volume_clamp(volume);

    if (clamped == 0U) {
        return 0U;
    }
    return CARD_CTRL_DISK2_SOUND_ENABLE_BIT |
        ((uint32_t)clamped << CARD_CTRL_DISK2_SOUND_VOLUME_SHIFT);
}

static void card_control_write_disk2_sound_volume(uint8_t volume)
{
    g_disk2_sound_volume = disk2_sound_volume_clamp(volume);
    REG_WRITE(CARD_CTRL_DISK2_SOUND_CONTROL_REG,
              card_control_pack_disk2_sound_control(g_disk2_sound_volume));
}

static void card_control_pulse_disk2_sound_event(uint8_t event)
{
    const uint32_t event_nibble =
        ((uint32_t)(event & CARD_CTRL_DISK2_SOUND_EVENT_MASK) <<
         CARD_CTRL_DISK2_SOUND_EVENT_SHIFT);

    REG_WRITE(CARD_CTRL_DISK2_SOUND_CONTROL_REG,
              card_control_pack_disk2_sound_control(g_disk2_sound_volume) |
              event_nibble);
}

static void card_control_publish_disk2_sound_samples(void)
{
    const uintptr_t sample_addr = (uintptr_t)disk2_sound_samples;

    Xil_DCacheFlushRange((INTPTR)sample_addr, (INTPTR)DISK2_SOUND_SAMPLE_BYTES);
    REG_WRITE(CARD_CTRL_DISK2_SOUND_BASE_REG, (uint32_t)sample_addr);
    card_control_write_disk2_sound_volume(CARD_CTRL_DISK2_SOUND_DEFAULT_VOLUME);
}

static void card_control_sync_apple_reset_seq(void)
{
    g_apple_reset_seq_last = card_control_read_apple_reset_seq();
    g_apple_reset_seq_valid = 1U;
}

static void card_control_init(void)
{
    card_control_sync_slot_mask_from_hw();
    card_control_write_slot_mask(g_card_slot_enable_mask);
    card_control_publish_disk2_sound_samples();
    card_control_sync_apple_reset_seq();
    no_slot_clock_control_init();
}

static void card_control_mark_cpu0_ready(void)
{
    REG_WRITE(RESET_RELEASE_REG,
              RESET_RELEASE_CPU0_READY_BIT);
}

static void control_set_slot_enabled(void *ctx, uint8_t slot, uint8_t enable)
{
    uint32_t slot_mask;
    uint32_t slot_bit;

    (void)ctx;
    if (slot == 0U || slot > 7U) {
        return;
    }
    if (slot == 5U) {
        /* Slot 5 is the Applicard: arm/disarm the PS Z80 service in
         * lockstep with the PL front end. */
        applicard_service_set_enabled(enable);
    }
    slot_bit = 1UL << slot;
    slot_mask = g_card_slot_enable_mask;
    if (enable != 0U) {
        slot_mask |= slot_bit;
    } else {
        slot_mask &= ~slot_bit;
    }
    card_control_write_slot_mask(slot_mask);
}

static const char *boot_debug_handoff_name(uint32_t handoff)
{
    switch (handoff) {
    case BOOT_MENU_HANDOFF_DISK2:
        return "DISK2";
    case BOOT_MENU_HANDOFF_SMARTPORT:
        return "SMARTPORT";
    default:
        return "INVALID";
    }
}

static const char *boot_debug_slot7_mode_name(uint32_t mode)
{
    switch (mode) {
    case 0U:
        return "BOOTMENU";
    case 1U:
        return "SMARTPORT";
    default:
        return "UNKNOWN";
    }
}

static void boot_debug_log_snapshot(const char *label)
{
    char line[192];
    boot_menu_debug_snapshot_t snap;
    const uint32_t slot_mask_raw = REG_READ(CARD_CTRL_SLOT_ENABLE_REG);
    const uint32_t slot_mask_norm = card_control_normalize_slot_mask(slot_mask_raw);
    uint32_t handoff_mode;
    uint32_t slot7_mode;
    uint32_t handoff_pending;
    uint32_t apple_status;

    boot_menu_service_debug_snapshot(&snap);
    handoff_mode = snap.handoff_mode_word & 0x3U;
    slot7_mode = (snap.handoff_mode_word >> 2U) & 0x3U;
    handoff_pending = (snap.handoff_mode_word >> 7U) & 0x1U;
    apple_status = snap.apple_status_byte & 0xFFU;
    if (label == NULL) {
        label = "boot";
    }

    (void)snprintf(line,
                   sizeof(line),
                   "[bootdbg] %s slot_mask raw=0x%08lX norm=0x%08lX shadow=0x%08lX slot6=%lu\r\n",
                   label,
                   (unsigned long)slot_mask_raw,
                   (unsigned long)slot_mask_norm,
                   (unsigned long)g_card_slot_enable_mask,
                   (unsigned long)((slot_mask_norm >> CARD_CTRL_SLOT_DISK2) & 0x1U));
    uart_puts(UART0_BASE, line);

    (void)snprintf(line,
                   sizeof(line),
                   "[bootdbg] %s boot_regs status=0x%08lX timeout=0x%08lX handoff=0x%08lX mode=%s slot7=%s pending=%lu\r\n",
                   label,
                   (unsigned long)snap.status_word,
                   (unsigned long)snap.timeout_ticks,
                   (unsigned long)snap.handoff_mode_word,
                   boot_debug_handoff_name(handoff_mode),
                   boot_debug_slot7_mode_name(slot7_mode),
                   (unsigned long)handoff_pending);
    uart_puts(UART0_BASE, line);

    (void)snprintf(line,
                   sizeof(line),
                   "[bootdbg] %s apple_status=0x%02lX disk2=%lu smartport=%lu eligible=%lu active=%lu timeout_or_closed=%lu\r\n",
                   label,
                   (unsigned long)apple_status,
                   (unsigned long)((apple_status >> 4U) & 0x1U),
                   (unsigned long)((apple_status >> 3U) & 0x1U),
                   (unsigned long)((apple_status >> 2U) & 0x1U),
                   (unsigned long)((apple_status >> 1U) & 0x1U),
                   (unsigned long)(apple_status & 0x1U));
    uart_puts(UART0_BASE, line);
}

static uint8_t control_get_slot_enabled(void *ctx, uint8_t slot)
{
    uint32_t slot_bit;

    (void)ctx;
    if (slot == 0U || slot > 7U) {
        return 0U;
    }
    card_control_sync_slot_mask_from_hw();
    slot_bit = 1UL << slot;
    return ((g_card_slot_enable_mask & slot_bit) != 0U) ? 1U : 0U;
}

static void control_set_phasor_pan(void *ctx, uint32_t pan_lo, uint32_t pan_hi)
{
    (void)ctx;
    REG_WRITE(CARD_CTRL_PHASOR_PAN_LO_REG, pan_lo & 0x00FFFFFFUL);
    REG_WRITE(CARD_CTRL_PHASOR_PAN_HI_REG, pan_hi & 0x00FFFFFFUL);
}

static uint32_t phasor_audio_pack5(int8_t value)
{
    return ((uint32_t)(uint8_t)value) & 0x1FUL;
}

static void control_set_phasor_audio(void *ctx,
                                     int8_t bass,
                                     int8_t mid,
                                     int8_t treble,
                                     int8_t warmth,
                                     int8_t volume,
                                     uint8_t psg_ay_mode,
                                     uint8_t mockingboard_only)
{
    uint32_t packed;

    (void)ctx;
    packed = phasor_audio_pack5(bass) |
             (phasor_audio_pack5(mid) << 5) |
             (phasor_audio_pack5(treble) << 10) |
             (phasor_audio_pack5(warmth) << 15) |
             (phasor_audio_pack5(volume) << 20) |
             (((uint32_t)(psg_ay_mode != 0U)) << 25);
    if (mockingboard_only != 0U) {
        packed |= CARD_CTRL_PHASOR_AUDIO_MOCKINGBOARD_ONLY_BIT;
    }
    REG_WRITE(CARD_CTRL_PHASOR_AUDIO_REG, packed);
}

static void control_set_mouse_sensitivity(void *ctx, uint8_t sensitivity)
{
    (void)ctx;
    usb_hid_service_set_sensitivity(sensitivity);
}

static void control_set_disk2_sound_volume(void *ctx, uint8_t volume)
{
    (void)ctx;
    card_control_write_disk2_sound_volume(volume);
}

static void control_play_disk2_sound_event(void *ctx, uint8_t event)
{
    (void)ctx;
    card_control_pulse_disk2_sound_event(event);
}

static void control_set_clock_enabled(void *ctx, uint8_t enable)
{
    (void)ctx;
    if (enable != 0U) {
        no_slot_clock_control_publish_rtc(&g_rtc);
    }
    no_slot_clock_control_set_enabled(enable);
}

static void control_set_supersprite_enabled(void *ctx, uint8_t enable)
{
    /* Live read-modify-write of the feature register (bit 1), preserving the
     * no-slot-clock bit -- all feature-mask owners RMW from hardware. */
    uint32_t feat = REG_READ(CARD_CTRL_FEATURE_ENABLE_REG);
    (void)ctx;
    if (enable != 0U) {
        feat |= CARD_CTRL_FEATURE_SUPERSPRITE_ENABLE_BIT;
    } else {
        feat &= ~CARD_CTRL_FEATURE_SUPERSPRITE_ENABLE_BIT;
    }
    REG_WRITE(CARD_CTRL_FEATURE_ENABLE_REG, feat);
}

static uint8_t menu_platform_get_scanlines(void *ctx)
{
    (void)ctx;
    return scanlines_get();
}

static uint8_t menu_platform_get_video_ghosting(void *ctx)
{
    (void)ctx;
    return video_ghosting_get();
}

static void menu_platform_set_border(void *ctx,
                                     uint8_t enabled,
                                     uint8_t color,
                                     uint8_t flood)
{
    (void)ctx;
    border_set(enabled, color, flood);
}

static uint8_t menu_platform_get_border_enabled(void *ctx)
{
    (void)ctx;
    return g_border_enabled_shadow;
}

static uint8_t menu_platform_get_border_color(void *ctx)
{
    (void)ctx;
    return g_border_color_shadow;
}

static uint8_t menu_platform_get_border_flood(void *ctx)
{
    (void)ctx;
    return g_border_flood_shadow;
}

static void menu_platform_set_video_output(void *ctx,
                                           uint8_t mono_enable,
                                           uint8_t mono_color,
                                           uint8_t color_mode,
                                           uint8_t video7_auto_mono_enable,
                                           int8_t clean_phase_cycles,
                                           int8_t pal_phase_cycles)
{
    (void)ctx;
    video_output_set(mono_enable,
                     mono_color,
                     color_mode,
                     video7_auto_mono_enable,
                     clean_phase_cycles,
                     pal_phase_cycles);
}

static uint8_t menu_platform_get_video_output_mono(void *ctx)
{
    (void)ctx;
    return display_mono_enable_get();
}

static uint8_t menu_platform_get_video_output_mono_color(void *ctx)
{
    (void)ctx;
    return display_mono_color_get();
}

static uint8_t menu_platform_get_video_output_color_mode(void *ctx)
{
    (void)ctx;
    return video_color_mode_get();
}

static uint8_t menu_platform_get_video7_auto_mono_enabled(void *ctx)
{
    (void)ctx;
    return video7_auto_mono_enable_get();
}

static int8_t menu_platform_get_clean_video_phase_cycles(void *ctx)
{
    (void)ctx;
    return clean_video_phase_cycles_get();
}

static int8_t menu_platform_get_pal_video_phase_cycles(void *ctx)
{
    (void)ctx;
    return pal_video_phase_cycles_get();
}

static uint8_t menu_platform_is_apple_video_50hz(void *ctx)
{
    (void)ctx;
    return boot_menu_service_is_apple_video_50hz();
}

static void menu_platform_set_boot_timeout(void *ctx, uint32_t ticks)
{
    (void)ctx;
    boot_menu_service_set_timeout_ticks(ticks);
}

static void menu_platform_set_boot_handoff(void *ctx, uint8_t handoff)
{
    (void)ctx;
    boot_menu_service_set_handoff((boot_menu_handoff_t)handoff);
}

static int menu_platform_read_rtc(void *ctx, rtc_pcf8563_time_t *t)
{
    int rc;

    (void)ctx;
    if (!g_i2c_ready || t == NULL) {
        return -1;
    }
    rc = rtc_pcf8563_read_time(&g_i2c, t);
    if (rc == 0 && t->valid != 0U) {
        g_rtc = *t;
        no_slot_clock_control_publish_rtc(&g_rtc);
    }
    return rc;
}

static int menu_platform_write_rtc(void *ctx, const rtc_pcf8563_time_t *t)
{
    return control_set_rtc(ctx, t);
}

static int menu_platform_ethernet_read_config(void *ctx,
                                              uthernet2_network_config_t *config)
{
    (void)ctx;
    return uthernet2_read_network_config(config);
}

static int menu_platform_ethernet_write_config(void *ctx,
                                               const uthernet2_network_config_t *config)
{
    (void)ctx;
    return uthernet2_write_network_config(config);
}

static int menu_platform_ethernet_test(void *ctx,
                                       uthernet2_test_result_t *result)
{
    (void)ctx;
    return uthernet2_test(result);
}

static int menu_platform_ethernet_dhcp_acquire(
    void *ctx,
    const uint8_t mac[UTHERNET2_MAC_LEN],
    uthernet2_network_config_t *lease,
    char *detail,
    size_t detail_len)
{
    (void)ctx;
    return uthernet2_dhcp_acquire(mac, lease, detail, detail_len);
}

static void control_set_text_mono_colors(void *ctx, uint8_t fg_color, uint8_t bg_color)
{
    (void)ctx;
    text_mono_set_colors(fg_color, bg_color);
}

static void control_set_display_mono(void *ctx, uint8_t enable, uint8_t color)
{
    (void)ctx;
    display_mono_set(enable, color);
}

static void control_set_audio_enable(void *ctx, uint8_t enable)
{
    (void)ctx;
    g_audio_enable = (enable != 0U) ? 1U : 0U;
    audio_apply_config();
}

static void control_set_audio_mute(void *ctx, uint8_t mute)
{
    (void)ctx;
    g_audio_mute = (mute != 0U) ? 1U : 0U;
    audio_apply_config();
}

static void control_set_audio_tone_hz(void *ctx, uint32_t tone_hz)
{
    (void)ctx;
    if (tone_hz == 0U) {
        tone_hz = 1U;
    }
    g_audio_tone_hz = tone_hz;
    audio_apply_config();
}

static void control_set_audio_amp(void *ctx, uint32_t amp)
{
    (void)ctx;
    g_audio_amp = amp;
    audio_apply_config();
}

static int control_smartport_reset_media(void *ctx)
{
    (void)ctx;
    return smartport_service_reset_media(SMARTPORT_SERVICE_ALL_DEVICES);
}

static void control_set_sdd_stream_enabled(void *ctx, uint8_t enable)
{
    (void)ctx;
    if (enable) {
        if (!usb_sdd_service_active()) {
            (void)usb_sdd_service_start();
        }
    } else {
        usb_sdd_service_stop();
    }
}

static void control_set_usb0_sd_remote_mount(void *ctx, uint8_t enable)
{
    (void)ctx;
    if (enable) {
        if (usb_sdd_service_active()) {
            usb_sdd_service_stop();
        }
        /* Hand the host a fully-flushed card: a dirty Disk II track can
         * stay deferred indefinitely under CP/M (the PCPI BIOS holds the
         * drive enabled), and a flush landing after the host rewrites
         * the FAT would interleave two writers on one filesystem. */
        {
            int rc = 0;
            for (uint32_t attempt = 0U; attempt < 64U; ++attempt) {
                rc = disk2_service_flush_dirty_now();
                if (rc == 0) {
                    break;
                }
            }
            if (rc != 0) {
                uart_puts(UART0_BASE,
                          "usb0 sd remote: dirty disk2 track NOT flushed, "
                          "image file may be stale\r\n");
            }
        }
        usb_storage_service_connect();
    } else {
        (void)usb_storage_service_disconnect();
    }
}

static void control_set_applicard_resource_max(void *ctx, uint8_t maximum)
{
    (void)ctx;
    applicard_service_set_wall_cap(maximum != 0U ?
                                   APPLICARD_WALL_CAP_MAX_US :
                                   APPLICARD_WALL_CAP_STANDARD_US);
}

static int menu_platform_set_smartport_image(void *ctx, uint8_t device, const char *path)
{
    (void)ctx;
    return smartport_service_set_image_path(device, path);
}

static int menu_platform_reset_smartport_media(void *ctx, uint8_t device)
{
    (void)ctx;
    return smartport_service_reset_media(device);
}

static int menu_platform_set_disk2_image(void *ctx, uint8_t drive, const char *path)
{
    (void)ctx;
    return disk2_service_set_image_path(drive, path);
}

static int control_disk2_reset_media(void *ctx, uint8_t drive)
{
    (void)ctx;
    return disk2_service_reset_media(drive);
}

static uint8_t menu_platform_get_disk2_image_read_only(void *ctx, uint8_t drive)
{
    disk2_image_info_t info;

    (void)ctx;
    if (disk2_service_get_image_info(drive, &info) != 0 || info.present == 0U) {
        return 0U;
    }
    return (info.read_only != 0U) ? 1U : 0U;
}

static int control_smartport_read_block(void *ctx,
                                        uint32_t block_num,
                                        uint8_t *buffer,
                                        uint32_t count,
                                        uint32_t *actual_out)
{
    (void)ctx;
    return smartport_service_read_block(1U, block_num, buffer, count, actual_out);
}

static void control_reboot_system(void *ctx)
{
    const uint32_t slcr_unlock = 0xF8000008U;
    const uint32_t slcr_lock = 0xF8000004U;
    const uint32_t slcr_unlock_key = 0x0000DF0DU;
    const uint32_t slcr_lock_key = 0x0000767BU;
    const uint32_t devcfg_multiboot = 0xF800702CU;
    const uint32_t ps_rst_ctrl = 0xF8000200U;

    (void)ctx;

    /* Force next boot from flash base (golden image) by clearing multiboot offset. */
    REG_WRITE(slcr_unlock, slcr_unlock_key);
    REG_WRITE(devcfg_multiboot, 0x00000000U);

    /* Zynq PS soft reset via SLCR. */
    REG_WRITE(ps_rst_ctrl, 0x00000001U);
    REG_WRITE(slcr_lock, slcr_lock_key);
    for (;;) {
        __asm__ volatile("wfi");
    }
}

static const uart_control_ops_t g_uart_control_ops = {
    .ctx = NULL,
    .get_snapshot = control_get_snapshot,
    .set_rtc = control_set_rtc,
    .set_vsync = control_set_vsync,
    .set_scanlines = control_set_scanlines,
    .set_text_mono_colors = control_set_text_mono_colors,
    .set_display_mono = control_set_display_mono,
    .set_audio_enable = control_set_audio_enable,
    .set_audio_mute = control_set_audio_mute,
    .set_audio_tone_hz = control_set_audio_tone_hz,
    .set_audio_amp = control_set_audio_amp,
    .reboot_system = control_reboot_system,
    .psram_qpi = psram_qpi,
    .psram_qpi_exit = psram_qpi_exit,
    .psram_qspi_read = psram_qspi_read,
    .psram_qspi_write = psram_qspi_write,
    .psram_spi_read = psram_spi_read,
    .psram_spi_write = psram_spi_write,
    .psram_reset = psram_reset,
    .psram_toggle_wrap = psram_toggle_wrap,
    .psram_set_delay = psram_set_delay,
    .psram_scan_delay = psram_scan_delay,
    .psram_read_id = psram_read_id,
    .smartport_reset_media = control_smartport_reset_media,
    .smartport_read_block = control_smartport_read_block
};

static void ui_poll_sensors(void)
{
    static XTime last_sense_tick = 0U;
    XTime now_tick = 0U;

    XTime_GetTime(&now_tick);
    if (g_i2c_ready) {
        (void)no_slot_clock_control_poll_apple_write(&g_i2c, &g_rtc);
    }

    if (last_sense_tick == 0U || (now_tick - last_sense_tick) > COUNTS_PER_SECOND) {
        if (g_i2c_ready) {
            (void)rtc_pcf8563_read_time(&g_i2c, &g_rtc);
            no_slot_clock_control_publish_rtc(&g_rtc);
            screenshot_service_update_fattime_from_rtc(&g_rtc);
            if (g_tmp102_ready) {
                (void)tmp102_read_temperature(&g_i2c, &g_temp);
            } else {
                g_temp.valid = 0U;
            }
        } else {
            g_rtc.valid = 0U;
            g_temp.valid = 0U;
        }
        last_sense_tick = now_tick;
    }

    if (g_rtc_write_pending) {
        g_rtc_write_pending = 0U;
        if (g_i2c_ready) {
            (void)rtc_pcf8563_read_time(&g_i2c, &g_rtc);
            no_slot_clock_control_publish_rtc(&g_rtc);
            screenshot_service_update_fattime_from_rtc(&g_rtc);
        }
    }
}

static void usb0_priority_checkpoint(void)
{
    /* Exactly one personality owns USB0 at a time. */
    if (usb_sdd_service_active()) {
        usb_sdd_service_poll();
        return;
    }
    usb_storage_service_poll();
}

static void usb1_boot_settle_begin(void)
{
    XTime now;

    XTime_GetTime(&now);
    g_usb1_boot_settle.active = 1U;
    g_usb1_boot_settle.last_count = usb_hid_service_activity_count();
    g_usb1_boot_settle.last_change = now;
    boot_menu_service_set_timeout_ticks(USB1_BOOT_HOLD_TIMEOUT_TICKS);
    uart_puts(UART0_BASE, "usb1: settling during boot prompt\r\n");
}

static void usb1_boot_settle_poll(config_menu_t *menu)
{
    uint32_t count;
    XTime now;
    char msg[80];

    if (g_usb1_boot_settle.active == 0U) {
        return;
    }

    count = usb_hid_service_activity_count();
    XTime_GetTime(&now);
    if (count != g_usb1_boot_settle.last_count) {
        g_usb1_boot_settle.last_count = count;
        g_usb1_boot_settle.last_change = now;
    }
    if (ticks_to_us(now - g_usb1_boot_settle.last_change) <
        USB1_BOOT_SETTLE_QUIET_US) {
        return;
    }

    g_usb1_boot_settle.active = 0U;
    config_menu_apply_boot_runtime(menu);
    (void)snprintf(msg,
                   sizeof(msg),
                   "usb1: settled during prompt, %lu events\r\n",
                   (unsigned long)count);
    uart_puts(UART0_BASE, msg);
}

static void ui_screenshot_sd_write_complete(void *ctx)
{
    int rc;
    char line[96];

    (void)ctx;
    rc = smartport_service_reset_media(SMARTPORT_SERVICE_ALL_DEVICES);
    if (rc != 0) {
        (void)snprintf(line,
                       sizeof(line),
                       "screenshot: SmartPort media refresh failed rc=%d\r\n",
                       rc);
        uart_puts(UART0_BASE, line);
    }
}

/* Config binding precedes USB storage initialization, so both the first
 * ready state and a later absent->present transition must run the settings
 * retry (0:/appletini_cfg.txt + runtime apply, which
 * pushes SmartPort/Disk II paths and re-probes Disk II, + bezel/video
 * ROM), then open the SmartPort images. Also heals remove/re-insert,
 * where the remount invalidates SmartPort's long-lived FILs. */
static void ui_poll_sd_media_arrival(config_menu_t *menu)
{
    static uint8_t last_ready = 0xFFU;
    const uint8_t ready = (usb_storage_medium_ready() != 0) ? 1U : 0U;
    int rc;

    if (ready == last_ready) {
        return;
    }
    last_ready = ready;
    if (ready == 0U) {
        /* Removal needs no action: per-op failures already degrade to
         * DEVICE_NOT_CONNECTED / medium-not-present. */
        return;
    }

    uart_puts(UART0_BASE, "SD media attached: reloading settings and media\r\n");
    config_menu_retry_settings_if_needed(menu);
    usb0_priority_checkpoint();
    rc = smartport_service_reset_media(SMARTPORT_SERVICE_ALL_DEVICES);
    if (rc == 0) {
        uart_puts(UART0_BASE, "SD media: SmartPort mounted ");
        uart_puts(UART0_BASE, smartport_service_get_image_path(1U));
        uart_puts(UART0_BASE, "\r\n");
    } else {
        uart_puts(UART0_BASE, "SD media: SmartPort media unavailable rc=");
        uart_putdec(UART0_BASE, (uint32_t)(-rc));
        uart_puts(UART0_BASE, "\r\n");
    }
}

static void ui_handle_input(ui_state_t *s, ui_input_t in)
{
    if (!in.pressed) {
        return;
    }

    s->last_key = in.key;
    s->key_count++;
    s->input_seq++;

    if (in.key == UI_KEY_SCANLINES) {
        scanlines_set((uint8_t)((scanlines_get() + 1U) % APPLETINI_SCANLINES_COUNT));
    }
}

static void ui_handle_input_with_config(ui_state_t *s, config_menu_t *menu, ui_input_t in)
{
    if (!in.pressed) {
        return;
    }

    if (config_menu_is_active(menu) &&
        g_usb_menu_owned == 0U &&
        ui_config_menu_has_close_consumer(menu) == 0U &&
        ui_input_requests_menu_close(in) != 0U) {
        boot_menu_service_request_rom_close();
        s->last_key = in.key;
        s->key_count++;
        s->input_seq++;
        return;
    }

    if (config_menu_handle_input(menu, in) != 0U) {
        s->last_key = in.key;
        s->key_count++;
        s->input_seq++;
        return;
    }

    if (config_menu_is_active(menu)) {
        s->last_key = in.key;
        s->key_count++;
        s->input_seq++;
        return;
    }

    ui_handle_input(s, in);
}

static void ui_draw_bezel_image(uint16_t *fb)
{
    unsigned copy_w;
    unsigned copy_h;
    unsigned y;

    if (g_bezel_565 == NULL || g_bezel_width == 0U || g_bezel_height == 0U) {
        return;
    }

    copy_w = g_bezel_width;
    copy_h = g_bezel_height;
    if (copy_w > (unsigned)FB16_WIDTH) {
        copy_w = (unsigned)FB16_WIDTH;
    }
    if (copy_h > (unsigned)FB16_HEIGHT) {
        copy_h = (unsigned)FB16_HEIGHT;
    }

    for (y = 0U; y < copy_h; ++y) {
        memcpy(&fb[(size_t)y * (size_t)FB16_WIDTH],
               &g_bezel_565[(size_t)y * (size_t)g_bezel_width],
               (size_t)copy_w * sizeof(uint16_t));
    }

    /* Fill only the margins the bezel image does not cover. A full-size
     * (1920x1080) bezel leaves no margins, so nothing extra is painted. */
    if (copy_w < (unsigned)FB16_WIDTH) {
        fb16_fill_rect(fb, (int)copy_w, 0,
                       FB16_WIDTH - (int)copy_w, (int)copy_h,
                       UI_BEZEL_BG_COLOR);
    }
    if (copy_h < (unsigned)FB16_HEIGHT) {
        fb16_fill_rect(fb, 0, (int)copy_h,
                       FB16_WIDTH, FB16_HEIGHT - (int)copy_h,
                       UI_BEZEL_BG_COLOR);
    }
}

static void ui_draw_bezel(uint16_t *fb)
{
    /* No clear-then-copy: painting the whole frame solid
     * UI_BEZEL_BG_COLOR before the bezel image lands gives an ~8 MB
     * window during which any scanout overlap (uncapped tearing, latch
     * races, mid-repaint publishes) flashes the screen blue -- observed
     * as blue flashes during config-menu use. The image is painted
     * directly and only uncovered margins get the background color; the
     * clear survives solely for the no-bezel case. */
    if (g_bezel_565 == NULL || g_bezel_width == 0U || g_bezel_height == 0U) {
        fb16_clear(fb, UI_BEZEL_BG_COLOR);
        return;
    }
    ui_draw_bezel_image(fb);
}

static void ui_invalidate_static_backgrounds(void)
{
    memset(g_output_slot_bg_generation, 0, sizeof(g_output_slot_bg_generation));
    memset(g_output_slot_bg_show_bezel, 0, sizeof(g_output_slot_bg_show_bezel));
    memset(g_output_slot_apple_mode, 0, sizeof(g_output_slot_apple_mode));
    memset(g_output_slot_debug_dirty, 0, sizeof(g_output_slot_debug_dirty));
}

static void ui_restore_static_rect(uint16_t *fb,
                                   int x,
                                   int y,
                                   int w,
                                   int h,
                                   uint8_t show_bezel)
{
    int x0 = x;
    int y0 = y;
    int x1 = x + w;
    int y1 = y + h;

    if (fb == NULL || w <= 0 || h <= 0) {
        return;
    }
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > FB16_WIDTH) {
        x1 = FB16_WIDTH;
    }
    if (y1 > FB16_HEIGHT) {
        y1 = FB16_HEIGHT;
    }
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    if (show_bezel == 0U || g_bezel_565 == NULL ||
        g_bezel_width == 0U || g_bezel_height == 0U) {
        fb16_fill_rect(fb, x0, y0, x1 - x0, y1 - y0,
                       (show_bezel != 0U) ? UI_BEZEL_BG_COLOR : FB16_COLOR_BLACK);
        return;
    }

    for (int row = y0; row < y1; ++row) {
        uint16_t *dst = &fb[(size_t)row * (size_t)FB16_WIDTH + (size_t)x0];
        int remaining = x1 - x0;

        if ((unsigned)row >= g_bezel_height || (unsigned)x0 >= g_bezel_width) {
            for (int col = 0; col < remaining; ++col) {
                dst[col] = UI_BEZEL_BG_COLOR;
            }
            continue;
        }

        unsigned copy_w = g_bezel_width - (unsigned)x0;
        if (copy_w > (unsigned)remaining) {
            copy_w = (unsigned)remaining;
        }
        memcpy(dst,
               &g_bezel_565[(size_t)row * (size_t)g_bezel_width + (size_t)x0],
               (size_t)copy_w * sizeof(uint16_t));
        if ((int)copy_w < remaining) {
            for (int col = (int)copy_w; col < remaining; ++col) {
                dst[col] = UI_BEZEL_BG_COLOR;
            }
        }
    }
}

static void ui_restore_screenshot_overlay_if_needed(uint16_t *fb, uint8_t show_bezel)
{
    screenshot_service_rect_t rect;

    if (screenshot_service_restore_rect_for_frame(fb, &rect) == 0U) {
        return;
    }

    ui_restore_static_rect(fb, rect.x, rect.y, rect.w, rect.h, show_bezel);
}

static void ui_prepare_static_background(uint16_t *fb, uint8_t show_bezel)
{
    const uint8_t slot = comp_out_addr_to_slot((uint32_t)(uintptr_t)fb);
    const uint32_t generation = (show_bezel != 0U) ? g_bezel_generation : 1U;

    if (slot == 0xFFU ||
        g_output_slot_bg_generation[slot] != generation ||
        g_output_slot_bg_show_bezel[slot] != show_bezel) {
        if (show_bezel != 0U) {
            ui_draw_bezel(fb);
        } else {
            fb16_clear(fb, FB16_COLOR_BLACK);
        }
        if (slot != 0xFFU) {
            g_output_slot_bg_generation[slot] = generation;
            g_output_slot_bg_show_bezel[slot] = show_bezel;
            g_output_slot_apple_mode[slot] = APPLE_FB_DISPLAY_MODE_LEGACY;
            g_output_slot_debug_dirty[slot] = 0U;
        }
    }
}

static void ui_restore_apple_footprint_if_needed(uint16_t *fb, uint8_t show_bezel)
{
    const uint8_t slot = comp_out_addr_to_slot((uint32_t)(uintptr_t)fb);
    const uint32_t next_mode = apple_fb_reader_published_display_mode();

    if (slot == 0xFFU) {
        return;
    }

    if (g_output_slot_apple_mode[slot] == APPLE_FB_DISPLAY_MODE_SHR &&
        next_mode != APPLE_FB_DISPLAY_MODE_SHR) {
        ui_restore_static_rect(fb,
                               (int)COMP_SUBWIN_SHR_X_OFF,
                               (int)COMP_SUBWIN_SHR_Y_OFF,
                               (int)COMP_SUBWIN_SHR_WIDTH,
                               (int)COMP_SUBWIN_SHR_HEIGHT,
                               show_bezel);
    }

    g_output_slot_apple_mode[slot] =
        (uint8_t)((next_mode == APPLE_FB_DISPLAY_MODE_SHR) ?
                  APPLE_FB_DISPLAY_MODE_SHR : APPLE_FB_DISPLAY_MODE_LEGACY);
}

static void ui_mark_slot_dynamic(uint16_t *fb)
{
    const uint8_t slot = comp_out_addr_to_slot((uint32_t)(uintptr_t)fb);

    if (slot != 0xFFU) {
        g_output_slot_bg_generation[slot] = 0U;
        g_output_slot_apple_mode[slot] = APPLE_FB_DISPLAY_MODE_LEGACY;
        g_output_slot_debug_dirty[slot] = 0U;
    }
}

static void ui_update_fps(const ui_state_t *s)
{
    static XTime last_fps_tick = 0U;
    static uint32_t last_fps_vblank = 0U;
    static uint32_t last_apple_blits = 0U;
    static uint32_t last_compositor_published = 0U;
    static uint32_t last_renderer_publish_seq = 0U;
    XTime now_tick = 0U;
    uint32_t fb_vblank = REG_READ(FB_STATUS_REG);
    uint32_t apple_blits = g_compositor_apple_frames_drawn;
    uint32_t compositor_published = g_compositor_frames_published;
    uint32_t renderer_publish_seq = apple_fb_reader_publish_seq();

    (void)s;

    g_renderer_publish_seq = renderer_publish_seq;
    XTime_GetTime(&now_tick);
    if (last_fps_tick == 0U) {
        last_fps_tick = now_tick;
        last_fps_vblank = fb_vblank;
        last_apple_blits = apple_blits;
        last_compositor_published = compositor_published;
        last_renderer_publish_seq = renderer_publish_seq;
    } else {
        XTime dt = now_tick - last_fps_tick;
        if (dt > (COUNTS_PER_SECOND / 2U)) {
            uint32_t df = fb_vblank - last_fps_vblank;
            uint32_t apple_df = apple_blits - last_apple_blits;
            uint32_t compositor_df =
                compositor_published - last_compositor_published;
            uint32_t renderer_df =
                renderer_publish_seq - last_renderer_publish_seq;
            if (dt != 0U) {
                g_fps_x100 = (uint32_t)(((uint64_t)df * (uint64_t)COUNTS_PER_SECOND * 100ULL) / (uint64_t)dt);
                g_apple_fps_x100 = (uint32_t)(((uint64_t)apple_df * (uint64_t)COUNTS_PER_SECOND * 100ULL) / (uint64_t)dt);
                g_compositor_fps_x100 = (uint32_t)(((uint64_t)compositor_df * (uint64_t)COUNTS_PER_SECOND * 100ULL) / (uint64_t)dt);
                g_renderer_fps_x100 = (uint32_t)(((uint64_t)renderer_df * (uint64_t)COUNTS_PER_SECOND * 100ULL) / (uint64_t)dt);
            }
            last_fps_tick = now_tick;
            last_fps_vblank = fb_vblank;
            last_apple_blits = apple_blits;
            last_compositor_published = compositor_published;
            last_renderer_publish_seq = renderer_publish_seq;
        }
    }
}

static uint8_t ui_frame_active(uint32_t frame, uint32_t until_frame)
{
    if (until_frame == 0U) {
        return 0U;
    }
    return ((int32_t)(frame - until_frame) <= 0) ? 1U : 0U;
}

static void ui_draw_disk_lock_icon(uint16_t *fb,
                                   int x,
                                   int y,
                                   uint8_t present,
                                   uint8_t locked)
{
    const uint32_t fg = FB16_COLOR_ORANGE;

    if (present == 0U || locked == 0U) {
        return;
    }

    fb16_fill_rect(fb, x, y, 20, 18, FB16_COLOR_BLACK);
    fb16_rect(fb, x + 4, y + 8, 12, 9, fg);
    fb16_hline(fb, x + 8, y + 12, 4, fg);
    fb16_vline(fb, x + 10, y + 12, 3, fg);

    fb16_hline(fb, x + 8, y + 3, 4, fg);
    fb16_vline(fb, x + 6, y + 4, 5, fg);
    fb16_vline(fb, x + 13, y + 4, 5, fg);
}

static void ui_draw_disk_activity_light(uint16_t *fb,
                                        int x,
                                        int y,
                                        const char *label,
                                        uint8_t active,
                                        uint32_t color)
{
    uint32_t bg = active ? color : FB16_RGB(0x18, 0x18, 0x18);
    uint32_t fg = active ? FB16_COLOR_BLACK : FB16_COLOR_DARK_GRAY;

    fb16_fill_rect(fb, x, y, 34, 22, bg);
    fb16_rect(fb, x, y, 34, 22, active ? FB16_COLOR_WHITE : FB16_COLOR_DARK_GRAY);
    fb16_string_scaled(fb, x + 10, y + 3, label, fg, bg, 2);
}

static void ui_draw_storage_activity(uint16_t *fb, const ui_state_t *s)
{
    disk2_activity_t disk2_activity;
    smartport_activity_t smartport_activity;
    uint8_t disk2_valid = 0U;
    uint8_t smartport_valid = 0U;
    uint8_t source;
    uint8_t read_active = 0U;
    uint8_t write_active = 0U;
    uint8_t status_active = 0U;
    uint8_t unit;
    uint8_t present;
    uint8_t drive_active;
    uint8_t write_protected;
    uint32_t title_color;
    uint32_t frame_color;
    uint32_t drive_color;
    char line[32];
    int x = UI_DISK_ACTIVITY_X;
    int y = UI_DISK_ACTIVITY_Y;

    if (control_get_slot_enabled(NULL, CARD_CTRL_SLOT_DISK2) != 0U &&
        disk2_service_get_activity(&disk2_activity) == 0) {
        disk2_valid = 1U;
    }
    if (smartport_service_get_activity(&smartport_activity) == 0) {
        smartport_valid = 1U;
    }
    if (disk2_valid == 0U && smartport_valid == 0U) {
        memset(&g_storage_activity, 0, sizeof(g_storage_activity));
        return;
    }

    if (g_storage_activity.initialized == 0U) {
        g_storage_activity.initialized = 1U;
        if (disk2_valid != 0U) {
            g_storage_activity.disk2_last_read_count = disk2_activity.read_count;
            g_storage_activity.disk2_last_write_count = disk2_activity.write_count;
            g_storage_activity.disk2_last_unit =
                (disk2_activity.drive < DISK2_DRIVE_COUNT) ?
                disk2_activity.drive : 0U;
        }
        if (smartport_valid != 0U) {
            g_storage_activity.smartport_last_status_count = smartport_activity.status_count;
            g_storage_activity.smartport_last_read_count = smartport_activity.read_count;
            g_storage_activity.smartport_last_write_count = smartport_activity.write_count;
            g_storage_activity.smartport_last_unit =
                (smartport_activity.device < SMARTPORT_SERVICE_DEVICE_COUNT) ?
                smartport_activity.device : 0U;
        }
    }

    if (disk2_valid != 0U) {
        if (disk2_activity.read_count != g_storage_activity.disk2_last_read_count) {
            g_storage_activity.disk2_last_read_count = disk2_activity.read_count;
            g_storage_activity.disk2_read_until_frame =
                s->frame + UI_DISK_ACTIVITY_HOLD_FRAMES;
            g_storage_activity.source = UI_STORAGE_SOURCE_DISK2;
            g_storage_activity.disk2_last_unit =
                (disk2_activity.drive < DISK2_DRIVE_COUNT) ?
                disk2_activity.drive : 0U;
        }
        if (disk2_activity.write_count != g_storage_activity.disk2_last_write_count) {
            g_storage_activity.disk2_last_write_count = disk2_activity.write_count;
            g_storage_activity.disk2_write_until_frame =
                s->frame + UI_DISK_WRITE_HOLD_FRAMES;
            g_storage_activity.source = UI_STORAGE_SOURCE_DISK2;
            g_storage_activity.disk2_last_unit =
                (disk2_activity.write_drive < DISK2_DRIVE_COUNT) ?
                disk2_activity.write_drive : 0U;
        }
        if (disk2_activity.write_busy != 0U || disk2_activity.write_dirty != 0U) {
            g_storage_activity.disk2_write_until_frame =
                s->frame + UI_DISK_ACTIVITY_HOLD_FRAMES;
            g_storage_activity.source = UI_STORAGE_SOURCE_DISK2;
            g_storage_activity.disk2_last_unit =
                (disk2_activity.write_drive < DISK2_DRIVE_COUNT) ?
                disk2_activity.write_drive : 0U;
        }
    }

    if (smartport_valid != 0U) {
        if (smartport_activity.status_count !=
            g_storage_activity.smartport_last_status_count) {
            g_storage_activity.smartport_last_status_count =
                smartport_activity.status_count;
            g_storage_activity.smartport_status_until_frame =
                s->frame + UI_DISK_ACTIVITY_HOLD_FRAMES;
            g_storage_activity.source = UI_STORAGE_SOURCE_SMARTPORT;
            g_storage_activity.smartport_last_unit =
                (smartport_activity.device < SMARTPORT_SERVICE_DEVICE_COUNT) ?
                smartport_activity.device : 0U;
        }
        if (smartport_activity.read_count !=
            g_storage_activity.smartport_last_read_count) {
            g_storage_activity.smartport_last_read_count =
                smartport_activity.read_count;
            g_storage_activity.smartport_read_until_frame =
                s->frame + UI_DISK_ACTIVITY_HOLD_FRAMES;
            g_storage_activity.source = UI_STORAGE_SOURCE_SMARTPORT;
            g_storage_activity.smartport_last_unit =
                (smartport_activity.device < SMARTPORT_SERVICE_DEVICE_COUNT) ?
                smartport_activity.device : 0U;
        }
        if (smartport_activity.write_count !=
            g_storage_activity.smartport_last_write_count) {
            g_storage_activity.smartport_last_write_count =
                smartport_activity.write_count;
            g_storage_activity.smartport_write_until_frame =
                s->frame + UI_DISK_WRITE_HOLD_FRAMES;
            g_storage_activity.source = UI_STORAGE_SOURCE_SMARTPORT;
            g_storage_activity.smartport_last_unit =
                (smartport_activity.device < SMARTPORT_SERVICE_DEVICE_COUNT) ?
                smartport_activity.device : 0U;
        }
    }

    source = g_storage_activity.source;
    if (source == UI_STORAGE_SOURCE_SMARTPORT) {
        status_active = ui_frame_active(
            s->frame, g_storage_activity.smartport_status_until_frame);
        read_active = ui_frame_active(
            s->frame, g_storage_activity.smartport_read_until_frame);
        write_active = ui_frame_active(
            s->frame, g_storage_activity.smartport_write_until_frame);
        if (smartport_valid == 0U) {
            source = UI_STORAGE_SOURCE_DISK2;
        }
    } else if (disk2_valid == 0U) {
        source = UI_STORAGE_SOURCE_SMARTPORT;
    }

    if (source == UI_STORAGE_SOURCE_SMARTPORT && smartport_valid != 0U) {
        unit = (g_storage_activity.smartport_last_unit <
                SMARTPORT_SERVICE_DEVICE_COUNT) ?
            g_storage_activity.smartport_last_unit : 0U;
        present = ((smartport_activity.present_mask & (uint8_t)(1U << unit)) != 0U) ?
            1U : 0U;
        write_protected = smartport_activity.read_only;
        drive_active = (status_active != 0U || read_active != 0U ||
                        write_active != 0U) ? 1U : 0U;
        title_color = (present != 0U && drive_active != 0U) ?
            FB16_COLOR_WHITE : FB16_COLOR_DARK_GRAY;
        frame_color = (drive_active != 0U) ? FB16_COLOR_SKY : FB16_COLOR_DARK_GRAY;
        drive_color = FB16_COLOR_SKY;
        snprintf(line, sizeof(line), "SMARTPORT SP%u", (unsigned)unit + 1U);
    } else if (disk2_valid != 0U) {
        read_active = ui_frame_active(
            s->frame, g_storage_activity.disk2_read_until_frame);
        write_active = ui_frame_active(
            s->frame, g_storage_activity.disk2_write_until_frame);
        unit = (g_storage_activity.disk2_last_unit < DISK2_DRIVE_COUNT) ?
            g_storage_activity.disk2_last_unit : 0U;
        present = ((disk2_activity.present_mask & (uint8_t)(1U << unit)) != 0U) ?
            1U : 0U;
        drive_active = (disk2_activity.motor_on != 0U ||
                        disk2_activity.spinning != 0U) ? 1U : 0U;
        write_protected = menu_platform_get_disk2_image_read_only(NULL, unit);
        title_color = (disk2_activity.enabled != 0U && present != 0U &&
                       (drive_active != 0U || read_active != 0U ||
                        write_active != 0U)) ?
            FB16_COLOR_WHITE : FB16_COLOR_DARK_GRAY;
        frame_color = (drive_active != 0U) ? FB16_COLOR_GOLD : FB16_COLOR_DARK_GRAY;
        drive_color = FB16_COLOR_GOLD;
        snprintf(line, sizeof(line), "DISK II D%u", (unsigned)unit + 1U);
    } else {
        return;
    }

    fb16_fill_rect(fb, x, y, UI_DISK_ACTIVITY_W, UI_DISK_ACTIVITY_H, FB16_COLOR_BLACK);
    fb16_rect(fb,
              x,
              y,
              UI_DISK_ACTIVITY_W,
              UI_DISK_ACTIVITY_H,
              frame_color);

    fb16_string_scaled(fb, x + 8, y + 6, line, title_color, FB16_COLOR_BLACK, 2);
    ui_draw_disk_lock_icon(fb, x + 198, y + 3, present, write_protected);
    ui_draw_disk_activity_light(fb,
                                x + 232,
                                y + 3,
                                "R",
                                read_active,
                                FB16_COLOR_GREEN);
    ui_draw_disk_activity_light(fb,
                                x + 280,
                                y + 3,
                                "W",
                                write_active,
                                FB16_COLOR_ORANGE);
    if (drive_active != 0U) {
        fb16_fill_rect(fb, x + 328, y + 9, 10, 10, drive_color);
    } else {
        fb16_rect(fb, x + 328, y + 9, 10, 10, FB16_COLOR_DARK_GRAY);
    }
}

static void ui_restore_debug_overlay_regions(uint16_t *fb, uint8_t show_bezel)
{
    for (uint32_t i = 0U; i < debug_overlay_region_count(); ++i) {
        debug_overlay_rect_t rect = debug_overlay_region(i);

        ui_restore_static_rect(fb,
                               rect.x,
                               rect.y,
                               rect.w,
                               rect.h,
                               show_bezel);
    }
}

static uint8_t ui_debug_overlay_slot_dirty(uint16_t *fb)
{
    const uint8_t slot = comp_out_addr_to_slot((uint32_t)(uintptr_t)fb);

    return (slot != 0xFFU && g_output_slot_debug_dirty[slot] != 0U) ?
        1U : 0U;
}

static void ui_note_debug_overlay_drawn(uint16_t *fb)
{
    const uint8_t slot = comp_out_addr_to_slot((uint32_t)(uintptr_t)fb);

    if (slot != 0xFFU) {
        g_output_slot_debug_dirty[slot] = 1U;
    }
}

static void ui_note_debug_overlay_cleared(uint16_t *fb)
{
    const uint8_t slot = comp_out_addr_to_slot((uint32_t)(uintptr_t)fb);

    if (slot != 0xFFU) {
        g_output_slot_debug_dirty[slot] = 0U;
    }
}

static void ui_collect_debug_overlay_snapshot(debug_overlay_snapshot_t *snapshot,
                                              const ui_state_t *s,
                                              const config_menu_t *menu,
                                              uint8_t show_bezel)
{
    if (snapshot == NULL || s == NULL) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    (void)snprintf(snapshot->firmware_version,
                   sizeof(snapshot->firmware_version),
                   "%s",
                   APPLETINI_FIRMWARE_IMAGE_VERSION_SHORT);
    if (g_update_meta_valid != 0U) {
        (void)snprintf(snapshot->golden_version,
                       sizeof(snapshot->golden_version),
                       "%s",
                       g_update_meta.golden_version);
    }
    (void)snprintf(snapshot->bezel_status,
                   sizeof(snapshot->bezel_status),
                   "%s",
                   g_bezel_status);

    snapshot->metadata_valid = g_update_meta_valid;
    snapshot->show_bezel = show_bezel;
    snapshot->usb_menu_owned = g_usb_menu_owned;
    snapshot->config_menu_active = config_menu_is_active(menu);
    snapshot->boot_device = (menu != NULL) ? menu->boot_device : 0U;
    snapshot->settings_loaded = (menu != NULL) ? menu->settings_loaded : 0U;
    snapshot->session_only = (menu != NULL) ? menu->session_only : 0U;
    snapshot->temp = g_temp;
    snapshot->rtc = g_rtc;
    snapshot->ui_frame_count = s->frame;
    snapshot->key_count = s->key_count;
    snapshot->fps_x100 = g_fps_x100;
    snapshot->apple_fps_x100 = g_apple_fps_x100;
    snapshot->compositor_fps_x100 = g_compositor_fps_x100;
    snapshot->renderer_fps_x100 = g_renderer_fps_x100;
    snapshot->draw_us = ticks_to_us(g_last_draw_ticks);

    snapshot->apple_mode = (uint8_t)g_compositor_last_apple_mode;
    snapshot->apple_video_50hz = ui_apple_video_50hz(menu);
    snapshot->scanlines_mode = scanlines_get();
    snapshot->video_output_mono = display_mono_enable_get();
    snapshot->video_mono_color = display_mono_color_get();
    snapshot->video_color_mode = video_color_mode_get();
    snapshot->video7_auto_mono = video7_auto_mono_enable_get();
    snapshot->video_ghosting_strength = video_ghosting_get();

    snapshot->compositor_frames_published = g_compositor_frames_published;
    snapshot->compositor_frames_skipped = g_compositor_frames_skipped;
    snapshot->compositor_apple_blits = g_compositor_apple_frames_drawn;
    snapshot->renderer_publish_seq = g_renderer_publish_seq;
    snapshot->compositor_ui_us = g_compositor_last_ui_us;
    snapshot->compositor_apple_us = g_compositor_last_apple_us;
    snapshot->compositor_sync_us = g_compositor_last_sync_us;
    snapshot->compositor_total_us = g_compositor_last_total_us;
    snapshot->compositor_apple_drawn = g_compositor_last_apple_drawn;
    snapshot->compositor_suppress_apple = g_compositor_last_suppress_apple;
    snapshot->fb_vblank_count = REG_READ(FB_STATUS_REG);
    snapshot->fb_last_latched = REG_READ(FB_LAST_LATCHED_REG);
    snapshot->fb_debug = REG_READ(FB_DEBUG_REG);
    snapshot->fb_debug2 = REG_READ(FB_DEBUG2_REG);
    snapshot->softswitch_state = ui_softswitch_state();

    if (control_get_slot_enabled(NULL, CARD_CTRL_SLOT_DISK2) != 0U &&
        disk2_service_get_activity(&snapshot->disk2_activity) == 0) {
        snapshot->disk2_valid = 1U;
        for (uint8_t drive = 0U; drive < DISK2_DRIVE_COUNT; ++drive) {
            snapshot->disk2_read_only[drive] =
                menu_platform_get_disk2_image_read_only(NULL, drive);
        }
    }
    if (smartport_service_get_activity(&snapshot->smartport_activity) == 0) {
        snapshot->smartport_valid = 1U;
    }
    usb_hid_service_get_status(&snapshot->hid_status);
    usb_storage_service_get_stats(&snapshot->usb_storage_stats);
    snapshot->usb_storage_attention =
        (usb_storage_service_needs_attention() != 0) ? 1U : 0U;
}

/* Single-entry compositor draw callback. Runs the complete UI overlay
 * for one frame into the supplied BGRA32 framebuffer slot. The
 * compositor handles slot rotation and PL handoff; this just paints. */
/* Returns non-zero to ask the compositor to suppress the Apple
 * subwindow blit for this frame: when the boot/config menu is
 * active it owns the entire screen and the menu drawing would be
 * clobbered if we blitted Apple pixels on top. */
static int ui_compose_frame(uint16_t *fb, const ui_state_t *s, const config_menu_t *menu)
{
    XTime t0 = 0U, t1 = 0U;
    const uint8_t show_debugging =
        (menu != NULL && menu->show_debugging != 0U) ? 1U : 0U;
    const uint8_t show_bezel =
        (menu == NULL || menu->show_bezel != 0U) ? 1U : 0U;
    const uint8_t show_disk_activity =
        (menu == NULL || menu->disk2_activity_visible != 0U) ? 1U : 0U;
    const uint8_t draw_disk_activity_after_menu =
        (show_disk_activity != 0U &&
         config_menu_storage_activity_page_visible(menu) != 0U) ? 1U : 0U;

    XTime_GetTime(&t0);

    ui_prepare_static_background(fb, show_bezel);
    ui_restore_apple_footprint_if_needed(fb, show_bezel);
    ui_restore_screenshot_overlay_if_needed(fb, show_bezel);
    if (show_debugging != 0U ||
        ui_debug_overlay_slot_dirty(fb) != 0U) {
        ui_restore_debug_overlay_regions(fb, show_bezel);
        ui_note_debug_overlay_cleared(fb);
    }
    if (show_debugging != 0U) {
        debug_overlay_snapshot_t debug_snapshot;

        ui_collect_debug_overlay_snapshot(&debug_snapshot, s, menu, show_bezel);
        debug_overlay_draw(fb, &debug_snapshot);
        ui_note_debug_overlay_drawn(fb);
    }

    if (show_disk_activity == 0U) {
        ui_restore_static_rect(fb,
                               UI_DISK_ACTIVITY_X,
                               UI_DISK_ACTIVITY_Y,
                               UI_DISK_ACTIVITY_W,
                               UI_DISK_ACTIVITY_H,
                               show_bezel);
        memset(&g_storage_activity, 0, sizeof(g_storage_activity));
    } else if (draw_disk_activity_after_menu == 0U) {
        ui_draw_storage_activity(fb, s);
    }

    int menu_active = (int)config_menu_is_active(menu);
    if (menu_active) {
        config_menu_draw(fb, menu, g_usb_menu_owned);
        ui_mark_slot_dynamic(fb);
    }
    if (draw_disk_activity_after_menu != 0U) {
        ui_draw_storage_activity(fb, s);
    }

    screenshot_service_draw_overlay(fb);
    XTime_GetTime(&t1);
    g_last_draw_ticks = t1 - t0;
    return menu_active;
}

/* Adapter for the compositor's typed-erased callback contract. */
static int ui_compose_thunk(uint16_t *fb,
                            const void *ui_state,
                            const void *config_menu)
{
    /* A forced full refresh invalidates every per-slot background cache so
     * the complete static background is repainted. */
    if (compositor_full_refresh_active() != 0U) {
        ui_invalidate_static_backgrounds();
    }
    return ui_compose_frame(fb,
                            (const ui_state_t *)ui_state,
                            (const config_menu_t *)config_menu);
}

static void ui_set_boot_menu_visible(ui_state_t *s,
                                     config_menu_t *menu,
                                     uint8_t active)
{
    (void)s;
    config_menu_set_usb_owned(
        menu,
        (uint8_t)(active != 0U && g_usb_menu_owned != 0U));
    config_menu_set_usb_bindings_editable(
        menu,
        (uint8_t)(active != 0U && g_usb_menu_owned == 0U));
    if (active != 0U) {
        menu_chime_play();
    }
    config_menu_set_active(menu, active);
}

static void ui_handle_apple_reset(ui_state_t *s, config_menu_t *menu)
{
    const uint8_t reset_seq = card_control_read_apple_reset_seq();

    if (g_apple_reset_seq_valid == 0U) {
        g_apple_reset_seq_last = reset_seq;
        g_apple_reset_seq_valid = 1U;
        return;
    }
    if (reset_seq == g_apple_reset_seq_last) {
        return;
    }

    g_apple_reset_seq_last = reset_seq;
    if (menu != NULL) {
        config_menu_apply_boot_runtime(menu);
    }
    boot_menu_service_refresh_machine_policy();
    if (config_menu_is_active(menu)) {
        g_usb_menu_owned = 0U;
        ui_set_boot_menu_visible(s, menu, 0U);
    }
}

static ui_input_t ui_make_input(ui_key_t key)
{
    ui_input_t input;

    input.key = key;
    input.pressed = (key != UI_KEY_NONE) ? 1U : 0U;
    input.ascii = 0U;
    return input;
}

static uint8_t ui_config_menu_has_close_consumer(const config_menu_t *menu)
{
    if (menu == NULL || !config_menu_is_active(menu)) {
        return 0U;
    }
    return (config_menu_usb0_sd_remote_active(menu) != 0U ||
            menu->usb_binding_capture != CONFIG_MENU_USB_BIND_CAPTURE_NONE ||
            menu->browser_active != 0U ||
            menu->profile_carousel_active != 0U ||
            menu->profile_name_editor_active != 0U) ? 1U : 0U;
}

static uint8_t ui_input_requests_menu_close(ui_input_t input)
{
    if (input.pressed == 0U) {
        return 0U;
    }
    return (input.key == UI_KEY_BACK ||
            input.key == UI_KEY_ESC ||
            input.key == UI_KEY_MENU) ? 1U : 0U;
}

static void ui_close_config_menu_child(ui_state_t *s, config_menu_t *menu)
{
    ui_handle_input_with_config(s, menu, ui_make_input(UI_KEY_ESC));
}

static ui_key_t ui_key_from_usb_menu_source(const config_menu_t *menu,
                                            usb_hid_menu_source_t source)
{
    return config_menu_translate_usb_binding(menu, source);
}

static void ui_sync_usb_menu_capture(config_menu_t *menu)
{
    if (!config_menu_is_active(menu)) {
        g_usb_menu_owned = 0U;
    }
    config_menu_set_usb_owned(
        menu,
        (uint8_t)(config_menu_is_active(menu) && g_usb_menu_owned != 0U));
    config_menu_set_usb_bindings_editable(
        menu,
        (uint8_t)(config_menu_is_active(menu) && g_usb_menu_owned == 0U));
    usb_hid_service_set_menu_ok_source(config_menu_usb_ok_binding_source(menu));
    usb_hid_service_set_menu_open_close_source(
        config_menu_usb_open_close_binding_source(menu));
    usb_hid_service_set_screenshot_sources(
        config_menu_usb_screenshot_a2_binding_source(menu),
        config_menu_usb_screenshot_1080p_binding_source(menu));
    usb_hid_service_set_menu_capture(config_menu_is_active(menu));
}

static void ui_save_screenshot(screenshot_service_kind_t kind)
{
    screenshot_service_result_t result;
    char line[180];
    const char *kind_text =
        (kind == SCREENSHOT_SERVICE_KIND_A2) ? "a2" : "1080p";
    const int rc = screenshot_service_save(kind, &g_rtc, &result);

    if (rc == 0) {
        (void)snprintf(line,
                       sizeof(line),
                       "screenshot %s saved: %s\r\n",
                       kind_text,
                       result.path);
    } else {
        (void)snprintf(line,
                       sizeof(line),
                       "screenshot %s failed: %s\r\n",
                       kind_text,
                       (result.message[0] != '\0') ? result.message : "error");
    }
    uart_puts(UART0_BASE, line);
}

static void ui_handle_usb_menu_event(ui_state_t *s,
                                     config_menu_t *menu,
                                     const usb_hid_menu_event_t *event)
{
    ui_key_t key;

    if (event == NULL) {
        return;
    }

    switch (event->action) {
    case USB_HID_MENU_ACTION_OPEN:
        if (!config_menu_is_active(menu)) {
            g_usb_menu_owned = 1U;
            ui_set_boot_menu_visible(s, menu, 1U);
        }
        return;
    case USB_HID_MENU_ACTION_CLOSE:
        if (config_menu_is_active(menu)) {
            if (ui_config_menu_has_close_consumer(menu) != 0U) {
                ui_close_config_menu_child(s, menu);
                return;
            }
            if (g_usb_menu_owned == 0U) {
                boot_menu_service_request_rom_close();
            } else {
                g_usb_menu_owned = 0U;
                ui_set_boot_menu_visible(s, menu, 0U);
            }
        }
        return;
    default:
        break;
    }

    if (config_menu_is_active(menu) &&
        config_menu_usb_binding_capture_action(menu) !=
        CONFIG_MENU_USB_BIND_CAPTURE_NONE) {
        (void)config_menu_capture_usb_binding(menu, event->source);
        return;
    }

    switch (event->action) {
    case USB_HID_MENU_ACTION_SCREENSHOT_A2:
        ui_save_screenshot(SCREENSHOT_SERVICE_KIND_A2);
        return;
    case USB_HID_MENU_ACTION_SCREENSHOT_1080P:
        ui_save_screenshot(SCREENSHOT_SERVICE_KIND_1080P);
        return;
    default:
        break;
    }

    if (!config_menu_is_active(menu)) {
        return;
    }

    key = ui_key_from_usb_menu_source(menu, event->source);
    if (key == UI_KEY_NONE) {
        return;
    }

    ui_handle_input_with_config(s, menu, ui_make_input(key));
}

static void ui_set_bezel(uint16_t *pixels,
                         unsigned width,
                         unsigned height,
                         const char *source)
{
    if (g_bezel_565 != NULL) {
        bezel_loader_free_rgb565(g_bezel_565);
    }
    g_bezel_565 = pixels;
    g_bezel_width = width;
    g_bezel_height = height;
    ++g_bezel_generation;
    if (g_bezel_generation == 0U) {
        g_bezel_generation = 1U;
    }
    ui_invalidate_static_backgrounds();
    (void)snprintf(g_bezel_status,
                   sizeof(g_bezel_status),
                   "%s %ux%u",
                   (source != NULL) ? source : "bezel",
                   width,
                   height);
}

static int ui_load_bezel_file(const char *path, const char *source)
{
    uint16_t *pixels = NULL;
    unsigned width = 0U;
    unsigned height = 0U;
    char err[160];

    if (path == NULL || path[0] == '\0') {
        return -1;
    }

    if (bezel_loader_load_png_rgb565(path,
                                     &pixels,
                                     &width,
                                     &height,
                                     err,
                                     sizeof(err)) == 0) {
        ui_set_bezel(pixels, width, height, source);
        uart_puts(UART0_BASE, "loaded ");
        uart_puts(UART0_BASE, (source != NULL) ? source : "file");
        uart_puts(UART0_BASE, " bezel ");
        uart_putdec(UART0_BASE, width);
        uart_puts(UART0_BASE, "x");
        uart_putdec(UART0_BASE, height);
        uart_puts(UART0_BASE, " from ");
        uart_puts(UART0_BASE, path);
        uart_puts(UART0_BASE, "\r\n");
        return 0;
    }

    uart_puts(UART0_BASE, (source != NULL) ? source : "file");
    uart_puts(UART0_BASE, " bezel unavailable from ");
    uart_puts(UART0_BASE, path);
    uart_puts(UART0_BASE, ": ");
    uart_puts(UART0_BASE, err);
    uart_puts(UART0_BASE, "\r\n");
    return -1;
}

static int ui_load_embedded_bezel(void)
{
    uint16_t *pixels = NULL;
    unsigned width = 0U;
    unsigned height = 0U;
    char err[160];

    if (bezel_loader_decode_png_rgb565(bezel_default_png,
                                       bezel_default_png_len,
                                       "Assets/bezel_default.png",
                                       &pixels,
                                       &width,
                                       &height,
                                       err,
                                       sizeof(err)) == 0) {
        ui_set_bezel(pixels, width, height, "default");
        uart_puts(UART0_BASE, "loaded default bezel ");
        uart_putdec(UART0_BASE, width);
        uart_puts(UART0_BASE, "x");
        uart_putdec(UART0_BASE, height);
        uart_puts(UART0_BASE, "\r\n");
        return 0;
    }

    (void)snprintf(g_bezel_status, sizeof(g_bezel_status), "solid fallback");
    uart_puts(UART0_BASE, "default bezel unavailable: ");
    uart_puts(UART0_BASE, err);
    uart_puts(UART0_BASE, "\r\n");
    return -1;
}

static int ui_load_auto_bezel(const char *skip_path)
{
    static const char * const auto_paths[] = {
        UI_BEZEL_SD_PATH,
        UI_BEZEL_SD_FALLBACK_PATH
    };

    for (uint32_t i = 0U; i < (sizeof(auto_paths) / sizeof(auto_paths[0])); ++i) {
        const char *auto_path = auto_paths[i];

        if (skip_path != NULL && strcmp(skip_path, auto_path) == 0) {
            continue;
        }
        if (ui_load_bezel_file(auto_path, "SD") == 0) {
            return 0;
        }
    }

    return -1;
}

static int ui_apply_bezel_path(const char *path)
{
    if (path != NULL && path[0] != '\0') {
        if (ui_load_bezel_file(path, "custom") == 0) {
            return 0;
        }
        if (ui_load_auto_bezel(path) == 0) {
            return -1;
        }
        (void)ui_load_embedded_bezel();
        return -1;
    }

    if (ui_load_auto_bezel(NULL) == 0) {
        return 0;
    }
    return ui_load_embedded_bezel();
}

static int menu_platform_set_bezel_path(void *ctx, const char *path)
{
    (void)ctx;
    return ui_apply_bezel_path(path);
}

int main(void)
{
    ui_state_t ui;
    config_menu_t config_menu;
    uint32_t uart_budget;
    uint8_t gic_ready = 0U;
    uint8_t smartport_service_started = 0U;

    uart_init_both(921600U);

    memset(&ui, 0, sizeof(ui));
    config_menu_init(&config_menu);

    uart_puts(UART0_BASE, "\r\n==============================\r\n");
    uart_puts(UART0_BASE, "text_ui_test main start\r\n");
    uart_puts(UART0_BASE, "Firmware image: " APPLETINI_FIRMWARE_IMAGE_VERSION_FULL "\r\n");
    uart_puts(UART0_BASE, "Debug UART: UART0\r\n");
    uart_puts(UART0_BASE, "Control UART: UART1 + UART0\r\n");
    uart_puts(UART0_BASE, "==============================\r\n");

    g_update_meta_valid = update_metadata_read_from_qspi(&g_update_meta);

    /* Reset shared renderer handoff/control words before CPU1 can read them.
     * CPU1 applies its own MMU mapping but must not clear CPU0-published
     * settings after this point. */
    apple_fb_handoff_init();

    /* Prepare the second Cortex-A9. amp_release_core1() copies the
     * core-1 image (embedded as .rodata.core1_blob via lib/core1_
     * blob.S) into DDR at 0x20000000, dcache-flushes it, and wakes CPU1. */
    (void)amp_release_core1();
    g_ui_state = &ui;
    g_config_menu_state = &config_menu;
    uart_control_bind_config_menu(&config_menu);
    uart_control_init(&g_uart_control, UART1_BASE, UART0_BASE);
    uart_control_init(&g_uart0_control, UART0_BASE, UART0_BASE);
    boot_menu_service_init();
    card_control_init();
    screenshot_service_init();
    screenshot_service_set_sd_write_hook(ui_screenshot_sd_write_complete, NULL);

    {
        config_menu_platform_t menu_platform;

        memset(&menu_platform, 0, sizeof(menu_platform));
        menu_platform.ctx = NULL;
        menu_platform.set_scanlines = control_set_scanlines;
        menu_platform.get_scanlines = menu_platform_get_scanlines;
        menu_platform.set_video_ghosting = control_set_video_ghosting;
        menu_platform.get_video_ghosting = menu_platform_get_video_ghosting;
        menu_platform.set_border = menu_platform_set_border;
        menu_platform.get_border_enabled = menu_platform_get_border_enabled;
        menu_platform.get_border_color = menu_platform_get_border_color;
        menu_platform.get_border_flood = menu_platform_get_border_flood;
        menu_platform.set_video_output = menu_platform_set_video_output;
        menu_platform.get_video_output_mono = menu_platform_get_video_output_mono;
        menu_platform.get_video_output_mono_color = menu_platform_get_video_output_mono_color;
        menu_platform.get_video_output_color_mode = menu_platform_get_video_output_color_mode;
        menu_platform.get_video7_auto_mono_enabled =
            menu_platform_get_video7_auto_mono_enabled;
        menu_platform.get_clean_video_phase_cycles =
            menu_platform_get_clean_video_phase_cycles;
        menu_platform.get_pal_video_phase_cycles =
            menu_platform_get_pal_video_phase_cycles;
        menu_platform.is_apple_video_50hz = menu_platform_is_apple_video_50hz;
        menu_platform.set_boot_timeout_ticks = menu_platform_set_boot_timeout;
        menu_platform.set_boot_handoff = menu_platform_set_boot_handoff;
        menu_platform.set_clock_enabled = control_set_clock_enabled;
        menu_platform.set_supersprite_enabled = control_set_supersprite_enabled;
        menu_platform.set_sdd_stream_enabled = control_set_sdd_stream_enabled;
        menu_platform.set_usb0_sd_remote_mount = control_set_usb0_sd_remote_mount;
        menu_platform.set_slot_enabled = control_set_slot_enabled;
        menu_platform.get_slot_enabled = control_get_slot_enabled;
        menu_platform.set_applicard_resource_max = control_set_applicard_resource_max;
        menu_platform.set_phasor_pan = control_set_phasor_pan;
        menu_platform.set_phasor_audio = control_set_phasor_audio;
        menu_platform.set_mouse_sensitivity = control_set_mouse_sensitivity;
        menu_platform.set_disk2_sound_volume = control_set_disk2_sound_volume;
        menu_platform.play_disk2_sound_event = control_play_disk2_sound_event;
        menu_platform.set_smartport_image_path = menu_platform_set_smartport_image;
        menu_platform.reset_smartport_media = menu_platform_reset_smartport_media;
        menu_platform.set_disk2_image_path = menu_platform_set_disk2_image;
        menu_platform.reset_disk2_media = control_disk2_reset_media;
        menu_platform.get_disk2_image_read_only = menu_platform_get_disk2_image_read_only;
        menu_platform.set_bezel_path = menu_platform_set_bezel_path;
        menu_platform.read_rtc = menu_platform_read_rtc;
        menu_platform.write_rtc = menu_platform_write_rtc;
        menu_platform.ethernet_read_config = menu_platform_ethernet_read_config;
        menu_platform.ethernet_write_config = menu_platform_ethernet_write_config;
        menu_platform.ethernet_test = menu_platform_ethernet_test;
        menu_platform.ethernet_dhcp_acquire = menu_platform_ethernet_dhcp_acquire;
        config_menu_bind_platform(&config_menu, &menu_platform);
        boot_debug_log_snapshot("after config bind");
    }

    if (gic_init() != 0) {
        uart_puts(UART0_BASE, "gic_init failed\r\n");
    } else {
        gic_ready = 1U;
        uart_puts(UART0_BASE, "gic_init OK\r\n");
    }

    /* Egress init programs the PL apple_cycle_egress configuration
     * registers (cfg_enable, ring base, producer-pointer addr,
     * consumer pointer). It is write-once at boot; doing it on
     * core 0 (here) and not on CPU1 avoids both cores fighting
     * over the producer pointer slot at startup. CPU1 only polls
     * the ring after this init completes. */
    if (apple_cycle_egress_init() != 0) {
        uart_puts(UART0_BASE, "apple_cycle_egress_init failed\r\n");
    } else {
        uart_puts(UART0_BASE, "apple_cycle_egress_init OK\r\n");
    }

    if (i2c_init(&g_i2c) == 0) {
        g_i2c_ready = 1U;
        if (dac_reset_ak4493(&g_i2c) != 0) {
            uart_puts(UART0_BASE, "DAC reset failed (audio muted)\r\n");
        }
        if (tmp102_init(&g_i2c) == 0) {
            g_tmp102_ready = 1U;
        }
    }
    if (!g_i2c_ready) {
        uart_puts(UART0_BASE, "I2C init failed (RTC/TMP102 disabled)\r\n");
    } else if (!g_tmp102_ready) {
        uart_puts(UART0_BASE, "TMP102 init failed (temp unavailable)\r\n");
    }
    if (g_i2c_ready) {
        (void)rtc_pcf8563_read_time(&g_i2c, &g_rtc);
        no_slot_clock_control_publish_rtc(&g_rtc);
    }
    if (usb_storage_service_init() != 0) {
        uart_puts(UART0_BASE, "USB storage service: disabled\r\n");
    }
    (void)usb_sdd_service_init();
    /* Center the PSRAM read-capture delay before any data is staged so the
     * sampling point remains valid across boards and temperature. */
    (void)psram_calibrate_dcount(UART0_BASE);
    /* Arm the reset-forensics stickies (they latch the boot reset). */
    REG_WRITE(0x40000190U, 1U);
    (void)disk2_service_init(UART0_BASE);
    applicard_service_init(UART0_BASE);
    applicard_service_set_checkpoint(usb0_priority_checkpoint);

    uart_control_print_help(&g_uart_control, &g_uart_control_ops);
    uart_control_print_help(&g_uart0_control, &g_uart_control_ops);

    /* MMU MMIO marking for the AXI register region. The compositor
     * picks up its own slot setup from compositor_init() below. */
    {
#if defined(DEVICE_MEMORY)
        const uint32_t mmu_attr = DEVICE_MEMORY;
#elif defined(STRONG_ORDERED)
        const uint32_t mmu_attr = STRONG_ORDERED;
#else
        const uint32_t mmu_attr = NORM_NONCACHE;
#endif
        const uint32_t section_size = 0x00100000U;
        const uint32_t bytes = 0x40000000U;
        for (uint32_t off = 0; off < bytes; off += section_size) {
            Xil_SetTlbAttributes(0x40000000U + off, mmu_attr);
        }
    }
    control_set_scanlines(NULL, config_menu.scanlines_mode);
    control_set_video_ghosting(NULL, config_menu.video_ghosting_strength);

    if (gic_ready == 0U) {
        uart_puts(UART0_BASE, "SmartPort service: skipped (GIC unavailable)\r\n");
    } else {
        int smartport_rc = smartport_service_init(UART0_BASE);
        if (smartport_rc != -100) {
            smartport_service_started = 1U;
        }
        if (smartport_rc == 0) {
            uart_puts(UART0_BASE, "SmartPort service: mounted ");
            uart_puts(UART0_BASE, smartport_service_get_image_path(1U));
            uart_puts(UART0_BASE, "\r\n");
        } else {
            uart_puts(UART0_BASE, "SmartPort service: media unavailable rc=");
            uart_putdec(UART0_BASE, (uint32_t)(-smartport_rc));
            uart_puts(UART0_BASE, " (sd synth still available)\r\n");
        }
    }
    (void)usb_hid_service_init();
    int hid_rc = usb_hid_service_start();
    if (hid_rc == 0) {
        uart_puts(UART0_BASE, "usb1 start: ok\r\n");
    } else {
        uart_puts(UART0_BASE, "usb1 start: failed rc=");
        uart_putdec(UART0_BASE, (uint32_t)(-hid_rc));
        uart_puts(UART0_BASE, "\r\n");
    }

    config_menu_apply_runtime(&config_menu);
    boot_debug_log_snapshot("after runtime apply");
    /* The remaining boot-init steps (asset loads, SmartPort media reload,
     * PSRAM benchmark, compositor init) block for a while before the main
     * loop's storage poll starts. If the host has already enumerated USB0 and
     * issued a READ, that deferred request would otherwise sit unserviced for
     * the whole stretch (~700 ms first-read stall). Pump the USB0 storage poll
     * between the heavy steps so early reads are serviced promptly. */
    usb0_priority_checkpoint();
    config_menu_apply_startup_assets(&config_menu);
    usb0_priority_checkpoint();
    if (smartport_service_started != 0U) {
        int smartport_reload_rc =
            smartport_service_reset_media(SMARTPORT_SERVICE_ALL_DEVICES);
        if (smartport_reload_rc == 0) {
            uart_puts(UART0_BASE, "SmartPort service: media refreshed after startup assets\r\n");
        } else {
            uart_puts(UART0_BASE, "SmartPort service: media refresh failed rc=");
            uart_putdec(UART0_BASE, (uint32_t)(-smartport_reload_rc));
            uart_puts(UART0_BASE, "\r\n");
        }
    }
    usb0_priority_checkpoint();

    if (g_update_meta_valid) {
        uart_puts(UART0_BASE, "Updater meta boot: ");
        uart_puts(UART0_BASE, g_update_meta.golden_version);
        uart_puts(UART0_BASE, "\r\n");
        uart_puts(UART0_BASE, "Updater local firmware: ");
        uart_puts(UART0_BASE, APPLETINI_FIRMWARE_IMAGE_VERSION_FULL);
        uart_puts(UART0_BASE, "\r\n");
    } else {
        uart_puts(UART0_BASE, "Updater meta: unavailable/invalid\r\n");
    }
    audio_apply_config();
    usb0_priority_checkpoint();
    psram_bench_runtime_map();
    psram_bench_init_defaults(&g_psram);
    (void)psram_bench_startup(&g_psram);
    usb0_priority_checkpoint();

    /* Initialize RGB565 output-frame production before the main loop starts
     * publishing completed compositor slots to fb_reader. */
    compositor_init(ui_compose_thunk);
    compositor_set_draw_context(&ui, &config_menu);
    usb0_priority_checkpoint();
    if (hid_rc == 0) {
        usb1_boot_settle_begin();
    }
    boot_debug_log_snapshot("pre apple release");
    card_control_mark_cpu0_ready();

    /* USB0 is detached by default. The USB tab starts the SD-card bridge
     * only as a modal maintenance state; SDD remains the persistent opt-in
     * USB0 personality. */
    uint8_t usb0_modal_was_active = 0U;
    uint8_t usb0_modal_redraw_pending = 0U;
    uint32_t usb0_modal_last_input_seq = 0U;

    while (1) {
        uart_budget = 32U;
        if (config_menu_usb0_sd_remote_active(&config_menu) != 0U) {
            if (usb0_modal_was_active == 0U) {
                usb0_modal_was_active = 1U;
                usb0_modal_last_input_seq = ui.input_seq;
                usb0_modal_redraw_pending = 1U;
            }

            usb_storage_service_poll();
            if (usb_storage_service_consume_host_eject_request() != 0U) {
                config_menu_usb0_sd_remote_host_ejected(&config_menu);
                usb0_modal_redraw_pending = 1U;
            }

            if (config_menu_usb0_sd_remote_active(&config_menu) != 0U) {
                usb_hid_service_poll();
                usb1_boot_settle_poll(&config_menu);
            }
            if (config_menu_usb0_sd_remote_active(&config_menu) != 0U) {
                uint32_t boot_menu_budget = 8U;

                do {
                    const boot_menu_event_t boot_event = boot_menu_service_poll();

                    if (boot_event.type == BOOT_MENU_EVENT_NONE) {
                        break;
                    }

                    switch (boot_event.type) {
                    case BOOT_MENU_EVENT_OPEN:
                        g_usb_menu_owned = 0U;
                        ui_set_boot_menu_visible(&ui, &config_menu, 1U);
                        break;
                    case BOOT_MENU_EVENT_CLOSE:
                        config_menu_apply_runtime(&config_menu);
                        ui_set_boot_menu_visible(&ui, &config_menu, 0U);
                        usb0_modal_redraw_pending = 1U;
                        break;
                    case BOOT_MENU_EVENT_INPUT:
                        ui_handle_input_with_config(&ui, &config_menu, boot_event.input);
                        if (ui.input_seq != usb0_modal_last_input_seq) {
                            usb0_modal_last_input_seq = ui.input_seq;
                            usb0_modal_redraw_pending = 1U;
                        }
                        break;
                    default:
                        break;
                    }

                    usb_storage_service_poll();
                    if (config_menu_usb0_sd_remote_active(&config_menu) == 0U) {
                        usb0_modal_redraw_pending = 1U;
                        break;
                    }
                    boot_menu_budget--;
                } while (boot_menu_budget != 0U);
            }
            if (config_menu_usb0_sd_remote_active(&config_menu) != 0U) {
                uint32_t usb_menu_budget = 8U;

                do {
                    usb_hid_menu_event_t usb_event;

                    if (!usb_hid_service_pop_menu_event(&usb_event)) {
                        break;
                    }

                    ui_handle_usb_menu_event(&ui, &config_menu, &usb_event);
                    if (ui.input_seq != usb0_modal_last_input_seq) {
                        usb0_modal_last_input_seq = ui.input_seq;
                        usb0_modal_redraw_pending = 1U;
                    }
                    usb_storage_service_poll();
                    if (config_menu_usb0_sd_remote_active(&config_menu) == 0U) {
                        usb0_modal_redraw_pending = 1U;
                        break;
                    }
                    usb_menu_budget--;
                } while (usb_menu_budget != 0U);
            }
            ui_sync_usb_menu_capture(&config_menu);
            if (config_menu_usb0_sd_remote_active(&config_menu) != 0U) {
                do {
                    uart_control_event_t control_event;
                    ui_input_t in;

                    control_event = uart_control_poll(&g_uart0_control, &g_uart_control_ops);
                    in = control_event.input;
                    ui_handle_input_with_config(&ui, &config_menu, in);
                    if (ui.input_seq != usb0_modal_last_input_seq) {
                        usb0_modal_last_input_seq = ui.input_seq;
                        usb0_modal_redraw_pending = 1U;
                    }
                    usb_storage_service_poll();
                    if (config_menu_usb0_sd_remote_active(&config_menu) == 0U) {
                        usb0_modal_redraw_pending = 1U;
                        break;
                    }

                    control_event = uart_control_poll(&g_uart_control, &g_uart_control_ops);
                    in = control_event.input;
                    ui_handle_input_with_config(&ui, &config_menu, in);
                    if (ui.input_seq != usb0_modal_last_input_seq) {
                        usb0_modal_last_input_seq = ui.input_seq;
                        usb0_modal_redraw_pending = 1U;
                    }
                    usb_storage_service_poll();
                    if (config_menu_usb0_sd_remote_active(&config_menu) == 0U) {
                        usb0_modal_redraw_pending = 1U;
                        break;
                    }

                    if (!uart_control_has_pending_input(&g_uart0_control) &&
                        !uart_control_has_pending_input(&g_uart_control)) {
                        break;
                    }
                    uart_budget--;
                } while (uart_budget != 0U);
            }
            ui_sync_usb_menu_capture(&config_menu);
            if (usb0_modal_redraw_pending != 0U &&
                (config_menu_usb0_sd_remote_active(&config_menu) == 0U ||
                 usb_storage_service_needs_attention() == 0)) {
                static uint32_t last_seen_vblank_modal = 0u;
                uint32_t cur_vblank = REG_READ(FB_STATUS_REG);
                if (cur_vblank != last_seen_vblank_modal) {
                    last_seen_vblank_modal = cur_vblank;
                    ui.frame++;
                    ui_update_fps(&ui);
                }
                usb_storage_service_poll();
                compositor_request_full_refresh();
                (void)compositor_tick();
                usb_storage_service_poll();
                usb0_modal_redraw_pending = 0U;
            }
            if (config_menu_usb0_sd_remote_active(&config_menu) == 0U) {
                usb0_modal_was_active = 0U;
                usb0_modal_last_input_seq = ui.input_seq;
            }
            continue;
        }

        ui_handle_apple_reset(&ui, &config_menu);
        boot_menu_service_refresh_machine_policy();
        usb0_priority_checkpoint();
        if (usb_sdd_service_active() ||
            usb_storage_service_needs_attention() == 0) {
            usb_hid_service_poll();
        }
        usb1_boot_settle_poll(&config_menu);
        usb0_priority_checkpoint();
        ui_poll_sensors();
        usb0_priority_checkpoint();
        smartport_service_poll();
        usb0_priority_checkpoint();
        disk2_service_poll();
        usb0_priority_checkpoint();
        applicard_service_poll();
        usb0_priority_checkpoint();
        ui_poll_sd_media_arrival(&config_menu);
        usb0_priority_checkpoint();
        screenshot_service_poll();
        usb0_priority_checkpoint();
        /* apple_cycle_egress_poll() and the per-cycle renderer
         * dispatch run on CPU1 in AMP mode -- not here. */
        {
            uint32_t boot_menu_budget = 8U;

            do {
                const boot_menu_event_t boot_event = boot_menu_service_poll();

                if (boot_event.type == BOOT_MENU_EVENT_NONE) {
                    break;
                }

                switch (boot_event.type) {
                case BOOT_MENU_EVENT_OPEN:
                    g_usb_menu_owned = 0U;
                    ui_set_boot_menu_visible(&ui, &config_menu, 1U);
                    break;
                case BOOT_MENU_EVENT_CLOSE:
                    config_menu_apply_runtime(&config_menu);
                    ui_set_boot_menu_visible(&ui, &config_menu, 0U);
                    break;
                case BOOT_MENU_EVENT_INPUT:
                    ui_handle_input_with_config(&ui, &config_menu, boot_event.input);
                    break;
                default:
                    break;
                }

                usb0_priority_checkpoint();
                boot_menu_budget--;
            } while (boot_menu_budget != 0U);
        }
        {
            uint32_t usb_menu_budget = 8U;

            do {
                usb_hid_menu_event_t usb_event;

                if (!usb_hid_service_pop_menu_event(&usb_event)) {
                    break;
                }

                ui_handle_usb_menu_event(&ui, &config_menu, &usb_event);
                usb0_priority_checkpoint();
                usb_menu_budget--;
            } while (usb_menu_budget != 0U);
        }
        ui_sync_usb_menu_capture(&config_menu);
        usb0_priority_checkpoint();
        do {
            uart_control_event_t control_event;
            ui_input_t in;

            control_event = uart_control_poll(&g_uart0_control, &g_uart_control_ops);
            in = control_event.input;

            ui_handle_input_with_config(&ui, &config_menu, in);
            usb0_priority_checkpoint();

            control_event = uart_control_poll(&g_uart_control, &g_uart_control_ops);
            in = control_event.input;

            ui_handle_input_with_config(&ui, &config_menu, in);
            usb0_priority_checkpoint();

            if (!uart_control_has_pending_input(&g_uart0_control) &&
                !uart_control_has_pending_input(&g_uart_control)) {
                break;
            }
            uart_budget--;
        } while (uart_budget != 0U);
        ui_sync_usb_menu_capture(&config_menu);

        /* Keep UI animation time tied to scanout, but let the compositor
         * run every loop. It normally returns immediately; when CPU1
         * publishes a fresh Apple frame before the next HDMI vblank, this
         * lets us supersede the pending output frame instead of adding a
         * full extra frame of latency. */
        {
            static uint32_t last_seen_vblank = 0u;
            uint32_t cur_vblank = REG_READ(FB_STATUS_REG);
            if (cur_vblank != last_seen_vblank) {
                last_seen_vblank = cur_vblank;
                ui.frame++;
                ui_update_fps(&ui);
            }
            usb0_priority_checkpoint();
            (void)compositor_tick();
            usb0_priority_checkpoint();
        }


    }
}
