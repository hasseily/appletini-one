#ifndef SCREENSHOT_SERVICE_H
#define SCREENSHOT_SERVICE_H

#include <stdint.h>

#include "../lib/rtc_pcf8563.h"

#define SCREENSHOT_SERVICE_PATH_LEN 96U
#define SCREENSHOT_SERVICE_MESSAGE_LEN 96U

typedef enum {
    SCREENSHOT_SERVICE_KIND_A2 = 0,
    SCREENSHOT_SERVICE_KIND_1080P
} screenshot_service_kind_t;

typedef struct {
    int rc;
    char path[SCREENSHOT_SERVICE_PATH_LEN];
    char message[SCREENSHOT_SERVICE_MESSAGE_LEN];
} screenshot_service_result_t;

typedef struct {
    int x;
    int y;
    int w;
    int h;
} screenshot_service_rect_t;

typedef void (*screenshot_service_sd_write_hook_t)(void *ctx);

void screenshot_service_init(void);
/* Drop screen coordinates cached before an output resolution change. */
void screenshot_service_clear_overlays(void);
void screenshot_service_set_scanlines(uint8_t mode);

/* Feed the FatFS get_fattime() hook (this file owns the FAT time
 * conversion). Called from the main loop's periodic RTC poll so every
 * file write -- disk2 flushes, config saves, screenshots -- carries the
 * real clock instead of the fixed fallback date. Invalid RTC readings
 * are ignored and the last good value stays in effect. */
void screenshot_service_update_fattime_from_rtc(const rtc_pcf8563_time_t *rtc);
void screenshot_service_set_sd_write_hook(screenshot_service_sd_write_hook_t hook,
                                          void *ctx);

/* Fires the registered SD-write hook. Shared by every service that writes
 * the card locally (screenshots, printouts) so the USB0 mass-storage host
 * cache gets invalidated. */
void screenshot_service_note_local_sd_write_complete(void);
int screenshot_service_save(screenshot_service_kind_t kind,
                            const rtc_pcf8563_time_t *rtc,
                            screenshot_service_result_t *result);
/* Snapshot the F1.2.2 capture source, then save bounded row batches from poll().
 * A2 uses raw renderer colors, border selection and fixed 2x/4x legacy or
 * 2x/2x SHR scaling with scanlines. OUTPUT copies the full RGB565 framebuffer.
 * Neither capture replays the FPGA mask; A2 omits compositor effects.
 * Returns zero when queued; errors fill result immediately. Consume the
 * completion before requesting another image. Normal batches close their
 * file before returning; framebuffer ownership never spans polls. */
int screenshot_service_request(screenshot_service_kind_t kind,
                               const rtc_pcf8563_time_t *rtc,
                               screenshot_service_result_t *result);
uint8_t screenshot_service_busy(void);
/* Returns one once per completed or cancelled request. */
uint8_t screenshot_service_take_result(screenshot_service_result_t *result);
/* Cancel a pending save. A cleanup error names the retained .part file. */
int screenshot_service_cancel(void);
void screenshot_service_poll(void);
uint8_t screenshot_service_restore_rect_for_frame(uint16_t *fb,
                                                  screenshot_service_rect_t *rect);
void screenshot_service_draw_overlay(uint16_t *fb);

/* Transient confirmation at the BOTTOM of the display. This is the same
 * overlay slot used by screenshot results. */
void screenshot_service_show_confirmation(const char *text);

/* Transient notice at the TOP of the display (separate instance from the
 * bottom screenshot overlay, so the two never collide). Used for the
 * TransWarp speed-change notices. Auto-expires after a few seconds. */
void screenshot_service_show_notice(const char *text);

#endif /* SCREENSHOT_SERVICE_H */
