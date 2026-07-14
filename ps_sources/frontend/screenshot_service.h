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
void screenshot_service_set_scanlines(uint8_t mode);

/* Feed the FatFS get_fattime() hook (this file owns the FAT time
 * conversion). Called from the main loop's periodic RTC poll so every
 * file write -- disk2 flushes, config saves, screenshots -- carries the
 * real clock instead of the fixed fallback date. Invalid RTC readings
 * are ignored and the last good value stays in effect. */
void screenshot_service_update_fattime_from_rtc(const rtc_pcf8563_time_t *rtc);
void screenshot_service_set_sd_write_hook(screenshot_service_sd_write_hook_t hook,
                                          void *ctx);
int screenshot_service_save(screenshot_service_kind_t kind,
                            const rtc_pcf8563_time_t *rtc,
                            screenshot_service_result_t *result);
void screenshot_service_poll(void);
uint8_t screenshot_service_restore_rect_for_frame(uint16_t *fb,
                                                  screenshot_service_rect_t *rect);
void screenshot_service_draw_overlay(uint16_t *fb);

#endif /* SCREENSHOT_SERVICE_H */
