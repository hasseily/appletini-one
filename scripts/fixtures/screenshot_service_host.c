#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/* Execute the production screenshot state machine/PNG encoder against a
 * fault-injected FatFS model. Python independently decodes the PNG artifacts. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/mman.h>
#endif

static int allocation_fails;
static void *snapshot_malloc(size_t size) { return allocation_fails ? NULL : malloc(size); }
#define malloc snapshot_malloc
#include "../../ps_sources/frontend/screenshot_service.c"
#undef malloc

#define MAX_FILES 72
typedef struct { char name[96]; unsigned char *data; size_t size; } mock_file_t;
static mock_file_t files[MAX_FILES];
static int generation, open_handles, mount_calls, write_calls, close_calls;
static int fail_write, short_write, fail_close, fail_rename, fail_unlink;
static int fail_open, fail_seek, fail_directory, not_mounted;
static int create_collisions, rename_collisions;
static unsigned bytes_this_poll, max_bytes_per_poll;
static int usb_connected, disconnects, reconnects, invalidations;
static uint16_t frame[1920 * 1080];
static uint8_t frame_slot, output_mode, apple_slot;
static uint32_t apple_mode, video_settings;
volatile uint32_t g_compositor_last_apple_slot, g_compositor_last_apple_mode;
const uint32_t comp_apple_slot_addr[COMP_APPLE_SLOT_COUNT] = {
    0x20000000U, 0x20100000U, 0x20200000U
};
static int tests;
uint16_t comp_out_width, comp_out_height;
int fb16_width, fb16_height;

static int find_file(const char *name)
{
    for (int i = 0; i < MAX_FILES; ++i) {
        if (strcmp(files[i].name, name) == 0) { return i; }
    }
    return -1;
}

static int file_count(const char *suffix)
{
    int count = 0;
    for (int i = 0; i < MAX_FILES; ++i) {
        size_t n = strlen(files[i].name), s = strlen(suffix);
        if (n >= s && strcmp(files[i].name + n - s, suffix) == 0) { ++count; }
    }
    return count;
}

FRESULT f_mount(FATFS *fs, const char *path, uint8_t now)
{
    (void)path; (void)now;
    ++mount_calls; ++generation; not_mounted = fs == NULL;
    return FR_OK;
}
FRESULT f_mkdir(const char *path)
{
    assert(strcmp(path, SCREENSHOT_DIR) == 0);
    return not_mounted ? FR_NOT_ENABLED : fail_directory ? FR_DISK_ERR : FR_EXIST;
}
FRESULT f_open(FIL *f, const char *name, uint8_t mode)
{
    int i = find_file(name);
    if (fail_open) { return FR_DISK_ERR; }
    assert(not_mounted == 0);
    if (mode & FA_CREATE_NEW) {
        if (create_collisions > 0) { --create_collisions; return FR_EXIST; }
        if (i >= 0) { return FR_EXIST; }
        for (i = 0; i < MAX_FILES && files[i].name[0] != '\0'; ++i) {}
        assert(i < MAX_FILES);
        (void)snprintf(files[i].name, sizeof(files[i].name), "%s", name);
    } else if (i < 0) { return FR_NO_FILE; }
    *f = (FIL){ .index = i, .generation = generation, .open = 1,
                .size = files[i].size };
    ++open_handles;
    return FR_OK;
}
FRESULT f_write(FIL *f, const void *data, UINT size, UINT *written)
{
    assert(f->open && f->generation == generation);
    ++write_calls;
    if (fail_write == write_calls) { *written = 0; return FR_DISK_ERR; }
    if (short_write == write_calls) { size /= 2; }
    mock_file_t *dest = &files[f->index];
    if (f->position + size > dest->size) {
        dest->data = realloc(dest->data, f->position + size);
        assert(dest->data != NULL);
        dest->size = f->position + size;
    }
    memcpy(dest->data + f->position, data, size);
    f->position += size; f->size = dest->size;
    *written = size; bytes_this_poll += size;
    return FR_OK;
}
FRESULT f_lseek(FIL *f, FSIZE_t offset)
{
    assert(f->open && f->generation == generation);
    if (fail_seek) { return FR_DISK_ERR; }
    assert(offset <= f->size); f->position = offset;
    return FR_OK;
}
FRESULT f_close(FIL *f)
{
    assert(f->open && f->generation == generation);
    ++close_calls;
    if (fail_close < 0 || fail_close == close_calls) { return FR_DISK_ERR; }
    f->open = 0; --open_handles;
    return FR_OK;
}
FRESULT f_rename(const char *from, const char *to)
{
    assert(open_handles == 0);
    if (fail_rename) { return FR_DISK_ERR; }
    if (rename_collisions > 0) { --rename_collisions; return FR_EXIST; }
    if (find_file(to) >= 0) { return FR_EXIST; }
    int i = find_file(from); assert(i >= 0);
    (void)snprintf(files[i].name, sizeof(files[i].name), "%s", to);
    return FR_OK;
}
FRESULT f_unlink(const char *name)
{
    assert(open_handles == 0);
    if (fail_unlink) { return FR_DISK_ERR; }
    int i = find_file(name);
    if (i < 0) { return FR_NO_FILE; }
    free(files[i].data); memset(&files[i], 0, sizeof(files[i]));
    return FR_OK;
}
int disk_initialize(int drive) { assert(drive == 0); return 0; }
void XTime_GetTime(XTime *now) { *now = 123456000; }
uint8_t usb_storage_service_disconnect(void)
{
    uint8_t prior = (uint8_t)usb_connected;
    usb_connected = 0; ++disconnects; return prior;
}
void usb_storage_service_connect(void) { assert(!usb_connected); usb_connected = 1; ++reconnects; }
static void invalidate(void *ctx) { assert(ctx == &tests); ++invalidations; }
void compositor_request_full_refresh(void) {}
const uint16_t *compositor_latched_framebuffer(uint8_t *slot)
{
    *slot = frame_slot; return frame_slot < 3 ? frame : NULL;
}
uint8_t compositor_output_mode(void) { return output_mode; }
uint8_t apple_fb_reader_claim(void) { return apple_slot; }
uint32_t apple_fb_reader_display_mode(void) { return apple_mode; }
uint32_t apple_fb_video_settings_get(void) { return video_settings; }
void compositor_pixel_mask_exclude(int x, int y, int w, int h)
{ (void)x; (void)y; (void)w; (void)h; }
uint8_t comp_out_addr_to_slot(uint32_t addr) { (void)addr; return 0U; }
void fb16_fill_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t color)
{ (void)fb; (void)x; (void)y; (void)w; (void)h; (void)color; }
void fb16_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t color)
{ (void)fb; (void)x; (void)y; (void)w; (void)h; (void)color; }
void fb16_string_scaled(uint16_t *fb, int x, int y, const char *s,
                        uint16_t fg, uint16_t bg, int scale)
{ (void)fb; (void)x; (void)y; (void)s; (void)fg; (void)bg; (void)scale; }

static uint16_t pattern(unsigned x, unsigned y)
{
    return (uint16_t)(((x * 3U + y * 5U) & 31U) << 11U |
                      ((x * 7U + y * 11U) & 63U) << 5U |
                      ((x * 13U + y * 17U) & 31U));
}
static void set_frame(unsigned w, unsigned h)
{
    comp_out_width = (uint16_t)w; comp_out_height = (uint16_t)h;
    fb16_width = (int)w; fb16_height = (int)h;
    for (unsigned y = 0; y < h; ++y) {
        for (unsigned x = 0; x < w; ++x) { frame[y * w + x] = pattern(x, y); }
    }
}
static void map_apple_frames(void)
{
    void *address = (void *)(uintptr_t)comp_apple_slot_addr[0];
    const size_t bytes = COMP_APPLE_SLOT_COUNT * COMP_APPLE_SLOT_BYTES;
#ifdef _WIN32
    /* Match the production 32-bit physical-address ABI in a 64-bit host test. */
    __declspec(dllimport) void *__stdcall VirtualAlloc(void *, size_t, unsigned long, unsigned long);
    assert(VirtualAlloc(address, bytes, 0x3000UL, 4UL) == address);
#else
    assert(mmap(address, bytes, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == address);
#endif
}
static void set_apple_frame(uint32_t mode)
{
    apple_mode = mode;
    const unsigned stride = mode == APPLE_FB_DISPLAY_MODE_SHR ? 640U : 624U;
    for (unsigned slot = 0; slot < COMP_APPLE_SLOT_COUNT; ++slot) {
        uint32_t *pixels = (uint32_t *)(uintptr_t)comp_apple_slot_addr[slot];
        for (unsigned i = 0; i < COMP_APPLE_SLOT_BYTES / 4U; ++i) {
            unsigned x = i % stride, y = i / stride;
            pixels[i] = (((x ^ y) & 255U) << 24) |
                        (((x * 3U + y * 5U + 1U) & 255U) << 16) |
                        (((x * 7U + y * 11U + 2U) & 255U) << 8) |
                        ((x * 13U + y * 17U + 3U) & 255U);
        }
    }
}
static void reset_case(void)
{
    assert(!screenshot_service_busy());
    assert(g_shot.pixels == NULL);
    memset(&g_shot, 0, sizeof(g_shot));
    for (int i = 0; i < MAX_FILES; ++i) { free(files[i].data); }
    memset(files, 0, sizeof(files));
    generation = open_handles = mount_calls = write_calls = close_calls = 0;
    fail_write = short_write = fail_close = fail_rename = fail_unlink = 0;
    fail_open = fail_seek = fail_directory = not_mounted = 0;
    create_collisions = rename_collisions = 0;
    allocation_fails = 0;
    bytes_this_poll = max_bytes_per_poll = 0;
    usb_connected = 1; disconnects = reconnects = invalidations = 0;
    frame_slot = 1; output_mode = DISPLAY_MODE_DEFAULT;
    apple_slot = 1; video_settings = 0;
    g_compositor_last_apple_slot = APPLE_FB_NO_SLOT;
    g_compositor_last_apple_mode = APPLE_FB_DISPLAY_MODE_LEGACY;
    set_frame(11, 7);
    screenshot_service_init();
    screenshot_service_set_sd_write_hook(invalidate, &tests);
    ++tests;
}
static const rtc_pcf8563_time_t rtc = {
    .valid = 1, .year = 2026, .month = 10, .day = 3, .hour = 12, .min = 34, .sec = 56
};
static void queue(screenshot_service_kind_t kind)
{
    screenshot_service_result_t result;
    assert(screenshot_service_request(kind, &rtc, &result) == 0);
    assert(result.rc == 0 && screenshot_service_busy());
    assert(open_handles == 0 && disconnects == 0);
    assert(g_fattime_override_active == 0);
}
static void step(void)
{
    bytes_this_poll = 0;
    screenshot_service_poll();
    if (bytes_this_poll > max_bytes_per_poll) { max_bytes_per_poll = bytes_this_poll; }
    assert(bytes_this_poll <= 2U * (6U + 4U * g_shot.width));
    assert(g_fattime_override_active == 0);
    if (fail_close >= 0) { assert(open_handles == 0); }
}
static screenshot_service_result_t finish(int success)
{
    unsigned watchdog = 0;
    while (screenshot_service_busy()) { step(); assert(++watchdog < 2000); }
    screenshot_service_result_t result;
    assert(screenshot_service_take_result(&result) == 1);
    assert(screenshot_service_take_result(NULL) == 0);
    assert((result.rc == 0) == success);
    assert(g_shot.pixels == NULL);
    assert(disconnects == invalidations);
    assert(reconnects <= disconnects);
    if (!success) { assert(file_count(".png") == 0); }
    return result;
}
static void export_png(const char *directory, const char *name,
                       const screenshot_service_result_t *result)
{
    char path[1024];
    int i = find_file(result->path); assert(i >= 0);
    (void)snprintf(path, sizeof(path), "%s/%s.png", directory, name);
    FILE *out = fopen(path, "wb"); assert(out != NULL);
    assert(fwrite(files[i].data, 1, files[i].size, out) == files[i].size);
    assert(fclose(out) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    map_apple_frames();
    screenshot_service_result_t result;
    /* These are the F1.2.2 dimensions and sample locations, independent of
     * the output mode and its composed picture rectangle. */
    for (unsigned layout = 0; layout < 3; ++layout) {
        for (unsigned scanlines = 0; scanlines < 4; ++scanlines) {
            reset_case();
            set_apple_frame(layout == 2 ? APPLE_FB_DISPLAY_MODE_SHR :
                                         APPLE_FB_DISPLAY_MODE_LEGACY);
            video_settings = layout == 1 ? (1U << APPLE_VIDEO_SETTINGS_BORDER_ENABLE_SHIFT) : 0U;
            screenshot_service_set_scanlines((uint8_t)scanlines);
            frame_slot = 255; /* Raw Apple capture needs no completed output. */
            output_mode = 5;
            queue(SCREENSHOT_SERVICE_KIND_A2);
            assert(g_shot.surface.base565 == NULL);
            assert(g_shot.surface.scale_x == 2U);
            memset((void *)(uintptr_t)comp_apple_slot_addr[0], 0xFF,
                   COMP_APPLE_SLOT_COUNT * COMP_APPLE_SLOT_BYTES);
            video_settings ^= 1U << APPLE_VIDEO_SETTINGS_BORDER_ENABLE_SHIFT;
            screenshot_service_set_scanlines(0);
            result = finish(1);
            assert(strstr(result.path, "20261003-123456-a2-") != NULL);
            assert(file_count(".part") == 0 && mount_calls == 0);
            assert(disconnects == 1 && reconnects == 1 && invalidations == 1);
            char name[40];
            snprintf(name, sizeof(name), "raw-%u-scan-%u", layout, scanlines);
            export_png(argv[1], name, &result);
        }
    }
    reset_case(); set_apple_frame(APPLE_FB_DISPLAY_MODE_LEGACY_I);
    queue(SCREENSHOT_SERVICE_KIND_A2); result = finish(1);
    export_png(argv[1], "raw-interlace-baseline", &result);

    reset_case(); set_apple_frame(APPLE_FB_DISPLAY_MODE_SHR);
    apple_slot = APPLE_FB_NO_SLOT;
    g_compositor_last_apple_slot = 2;
    g_compositor_last_apple_mode = APPLE_FB_DISPLAY_MODE_SHR;
    queue(SCREENSHOT_SERVICE_KIND_A2); result = finish(1);
    export_png(argv[1], "raw-fallback", &result);

    reset_case(); set_frame(1920, 1080); queue(SCREENSHOT_SERVICE_KIND_1080P);
    memset(frame, 0xFF, sizeof(frame));
    while (screenshot_service_busy()) {
        step(); assert(open_handles == 0);
        ++generation; /* Another service remounts between each batch. */
    }
    result = finish(1); export_png(argv[1], "full", &result);
    assert(max_bytes_per_poll == 15372);
    assert(strstr(result.path, "-1080p-") != NULL);

    reset_case(); set_frame(1360, 768); output_mode = 5;
    queue(SCREENSHOT_SERVICE_KIND_1080P); result = finish(1);
    export_png(argv[1], "wide", &result); assert(strstr(result.path, "-1360x768-") != NULL);

    reset_case(); queue(SCREENSHOT_SERVICE_KIND_1080P);
    comp_out_width = 17; comp_out_height = 11;
    result = finish(1); export_png(argv[1], "frozen-stride", &result);

    reset_case(); not_mounted = 1; queue(SCREENSHOT_SERVICE_KIND_1080P);
    result = finish(1); assert(mount_calls == 1);

    reset_case(); usb_connected = 0; queue(SCREENSHOT_SERVICE_KIND_1080P);
    result = finish(1); assert(disconnects == 1 && reconnects == 0);

    reset_case(); queue(SCREENSHOT_SERVICE_KIND_1080P);
    assert(screenshot_service_request(SCREENSHOT_SERVICE_KIND_1080P, &rtc, &result) != 0);
    assert(screenshot_service_cancel() == 0); result = finish(0);
    assert(disconnects == 0);

    reset_case(); queue(SCREENSHOT_SERVICE_KIND_1080P);
    while (screenshot_service_busy()) { step(); }
    assert(screenshot_service_request(SCREENSHOT_SERVICE_KIND_1080P, &rtc, &result) != 0);
    result = finish(1); /* A rejected request must preserve the pending result. */

    reset_case(); queue(SCREENSHOT_SERVICE_KIND_1080P); step();
    fail_open = 1; result = finish(0); assert(file_count(".part") == 0);

    reset_case(); queue(SCREENSHOT_SERVICE_KIND_1080P); step();
    ++files[find_file(g_shot.temporary)].size; /* A changed file cannot resume. */
    result = finish(0); assert(file_count(".part") == 0);

    reset_case(); queue(SCREENSHOT_SERVICE_KIND_1080P); step();
    assert(f_unlink(g_shot.temporary) == FR_OK);
    result = finish(0); assert(file_count(".part") == 0);

    for (int phase = 0; phase < 4; ++phase) {
        reset_case(); queue(SCREENSHOT_SERVICE_KIND_1080P);
        for (int i = 0; i <= phase; ++i) { step(); }
        assert(screenshot_service_busy());
        assert(screenshot_service_cancel() == 0); result = finish(0);
        assert(file_count(".part") == 0 && invalidations == 1 && usb_connected);
    }

    reset_case(); queue(SCREENSHOT_SERVICE_KIND_1080P); result = finish(1);
    const int total_writes = write_calls, total_closes = close_calls;
    for (int short_mode = 0; short_mode < 2; ++short_mode) {
        for (int call = 1; call <= total_writes; ++call) {
            reset_case(); queue(SCREENSHOT_SERVICE_KIND_1080P);
            if (short_mode) { short_write = call; } else { fail_write = call; }
            result = finish(0); assert(file_count(".part") == 0 && usb_connected);
        }
    }
    for (int call = 1; call <= total_closes; ++call) {
        reset_case(); queue(SCREENSHOT_SERVICE_KIND_1080P); fail_close = call;
        result = finish(0); assert(file_count(".part") == 0 && usb_connected);
    }
    reset_case(); queue(SCREENSHOT_SERVICE_KIND_1080P); fail_close = -1;
    result = finish(0); assert(file_count(".part") == 1 && open_handles == 1);
    assert(strstr(result.message, "CLEANUP FAILED") && strstr(result.path, ".part"));

    reset_case(); queue(SCREENSHOT_SERVICE_KIND_1080P); step(); fail_unlink = 1;
    assert(screenshot_service_cancel() != 0); result = finish(0);
    assert(file_count(".part") == 1 && strstr(result.path, ".part"));

    for (int failure = 0; failure < 6; ++failure) {
        reset_case(); queue(SCREENSHOT_SERVICE_KIND_1080P);
        if (failure == 0) { fail_open = 1; }
        if (failure == 1) { fail_directory = 1; }
        if (failure == 2) { fail_rename = 1; }
        if (failure == 3) { step(); fail_seek = 1; }
        if (failure == 4) { create_collisions = 64; }
        if (failure == 5) { rename_collisions = 64; }
        result = finish(0); assert(file_count(".part") == 0 && usb_connected);
    }

    reset_case(); create_collisions = 3; rename_collisions = 3;
    queue(SCREENSHOT_SERVICE_KIND_1080P); result = finish(1);
    assert(file_count(".png") == 1 && file_count(".part") == 0);

    reset_case(); queue(SCREENSHOT_SERVICE_KIND_1080P); step();
    /* Real final-name collision: preserve its exact contents. */
    FIL other;
    assert(f_open(&other, g_shot.final, FA_CREATE_NEW | FA_WRITE) == FR_OK);
    UINT written;
    assert(f_write(&other, "KEEP", 4, &written) == FR_OK && written == 4);
    assert(f_close(&other) == FR_OK);
    const int existing = other.index;
    result = finish(1);
    assert(file_count(".png") == 2 && files[existing].size == 4);
    assert(memcmp(files[existing].data, "KEEP", 4) == 0);

    reset_case();
    char old_part[96];
    (void)snprintf(old_part, sizeof(old_part), SCREENSHOT_DIR
                   "/20261003-123456-1080p-%08lx.png.part", (unsigned long)g_shot_serial);
    assert(f_open(&other, old_part, FA_CREATE_NEW | FA_WRITE) == FR_OK);
    assert(f_write(&other, "KEEP", 4, &written) == FR_OK);
    assert(f_close(&other) == FR_OK);
    queue(SCREENSHOT_SERVICE_KIND_1080P); result = finish(1);
    assert(file_count(".part") == 1 && find_file(old_part) >= 0);
    assert(files[find_file(old_part)].size == 4);

    for (int bad = 0; bad < 5; ++bad) {
        reset_case();
        if (bad == 0) { frame_slot = 255; }
        if (bad == 1) { comp_out_width = 0; }
        if (bad == 2) { comp_out_height = COMP_OUT_MAX_HEIGHT + 1U; }
        if (bad == 3) { allocation_fails = 1; }
        assert(screenshot_service_request(bad == 4 ? (screenshot_service_kind_t)88 :
                                         SCREENSHOT_SERVICE_KIND_1080P, &rtc, &result) != 0);
        assert(!screenshot_service_busy() && disconnects == 0 && g_shot.pixels == NULL);
    }
    for (int bad = 0; bad < 2; ++bad) {
        reset_case(); set_apple_frame(APPLE_FB_DISPLAY_MODE_LEGACY);
        if (bad == 0) { apple_slot = APPLE_FB_NO_SLOT; }
        else { allocation_fails = 1; }
        assert(screenshot_service_request(SCREENSHOT_SERVICE_KIND_A2, &rtc, &result) != 0);
        assert(!screenshot_service_busy() && disconnects == 0 && g_shot.pixels == NULL);
    }
    reset_case();
    assert(screenshot_service_save(SCREENSHOT_SERVICE_KIND_1080P, NULL, &result) == 0);
    assert(strstr(result.path, "uptime-0000000123-1080p-") != NULL);
    reset_case();
    printf("PASS screenshot service: %d cases; every PNG write/close boundary; "
           "max 15372 bytes per row poll; baseline raw Apple/output pixels; immutable snapshot/remount/collision/cleanup\n", tests);
    return 0;
}
