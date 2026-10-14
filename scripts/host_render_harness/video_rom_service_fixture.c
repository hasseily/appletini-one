/* Run the real glyph protocol, CPU0 ROM service, CPU1 renderer, and glyph
 * decode together. Only hardware handoff/cache and the capture input are fake. */
#define main capture_harness_main
#include "harness.c"
#undef main

#include <windows.h>
#include "appletini_csbits.h"
#include "glyph_api.h"
#include "video_rom_service.h"

extern const uint8_t apple2e_video_rom[4096];
static uint32_t rom_gen, rom_mode, rom_ack;
static uint8_t menu_rom[4096], custom_rom[4096], next_menu[4096];
static uint8_t rendered_before[COMP_APPLE_SLOT_BYTES];
static unsigned checks;

#define CHECK(value, message) do { ++checks; if (!(value)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, message); exit(1); \
} } while (0)

uint32_t apple_fb_video_rom_gen_get(void) { return rom_gen; }
uint32_t apple_fb_video_rom_mode_get(void) { return rom_mode; }
uint32_t apple_fb_video_rom_ack_get(void) { return rom_ack; }
void apple_fb_video_rom_ack_set(uint32_t gen) { rom_ack = gen; }
void apple_fb_video_rom_publish(uint32_t gen, uint32_t mode)
{
    CHECK(rom_ack == rom_gen, "CPU0 never publishes before CPU1 ACK");
    CHECK(gen != rom_gen, "every change advances generation");
    rom_mode = mode;
    rom_gen = gen;
}

static uint8_t backend_apply(void *ctx, const uint8_t *rom)
{
    (void)ctx;
    return video_rom_service_apply(rom);
}
static void backend_status(void *ctx, uint16_t *flags, uint32_t *generation)
{
    (void)ctx;
    video_rom_service_status(flags, generation);
}
static const glyph_api_backend_t backend = {NULL, backend_apply, backend_status};

static void check_status(uint16_t expected, uint32_t generation)
{
    uint16_t flags = 0xFFFFU;
    uint32_t actual = UINT32_MAX;
    video_rom_service_status(&flags, &actual);
    CHECK(flags == expected, "status flags describe ACKed custom and pending");
    CHECK(actual == generation, "status generation is ACKed, not just requested");
}

static void check_private_rom(const uint8_t *rom)
{
    for (unsigned i = 0U; i < 4096U; ++i) {
        CHECK(appletini_video_rom_read((uint16_t)i) == rom[i],
              "renderer copied the whole ROM into private storage");
    }
    for (unsigned i = 0U; i < 256U; ++i) {
        for (unsigned y = 0U; y < 8U; ++y) {
            CHECK(csbits_enhanced2e[1][i][y] == (uint8_t)(rom[i * 8U + y] ^ 0xFFU),
                  "alternate glyph table rebuild matches source image");
        }
    }
}

static void renderer_frames(void)
{
    legacy_full_frame(SW_TEXT_ONLY);
    legacy_full_frame(SW_TEXT_ONLY);
    legacy_full_frame(SW_TEXT_ONLY);
    CHECK(rom_ack == rom_gen, "real frame boundary acknowledges publication");
    CHECK(s_pub_slot < COMP_APPLE_SLOT_COUNT, "renderer published a complete frame");
    video_rom_service_poll();
}

static void put_word(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
}
static void put_long(uint8_t *p, uint32_t value)
{
    put_word(p, (uint16_t)value); put_word(p + 2U, (uint16_t)(value >> 16));
}
static uint8_t command(uint8_t op, uint32_t id, uint16_t offset,
                       uint16_t count, const uint8_t *data)
{
    uint8_t packet[GLYPH_API_HEADER_SIZE + GLYPH_API_MAX_CHUNK];
    uint16_t length = GLYPH_API_HEADER_SIZE;
    memset(packet, 0, sizeof(packet));
    memcpy(packet, "AGLY", 4U);
    packet[4] = GLYPH_API_MAJOR;
    packet[5] = op;
    put_long(packet + 8U, id);
    put_word(packet + 12U, offset);
    put_word(packet + 14U, count);
    if (data != NULL) {
        const uint16_t extra = op == GLYPH_API_COMMIT ? 4U : count;
        memcpy(packet + GLYPH_API_HEADER_SIZE, data, extra);
        length += extra;
    }
    return glyph_api_execute(packet, length, &backend);
}

int main(void)
{
    uint8_t crc[4];
    uint8_t *shared = (uint8_t *)(uintptr_t)APPLE_VIDEO_ROM_OVERRIDE_ADDR;
    CHECK(VirtualAlloc(shared, 4096U, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) == shared,
          "map the production shared ROM address in the x86 fixture");
    check_status(0U, 0U);
    CHECK(video_rom_service_apply(custom_rom) == GLYPH_API_UNAVAILABLE,
          "uninitialized service rejects upload");
    video_rom_service_init();
    check_status(GLYPH_API_AVAILABLE, 0U);
    CHECK(apple_cycle_renderer_init() == 0, "real CPU1 renderer initializes");
    s_settings = apple_video_settings_pack_border_full(
        0U, 0U, APPLE_VIDEO_COLOR_RGB, 1U, 1U, 0, 0, 0U, 0U, 0U);
    memset(s_main_mem + 0x400U, 0xC0U, 1024U);

    memcpy(menu_rom, apple2e_video_rom, sizeof(menu_rom));
    memcpy(custom_rom, apple2e_video_rom, sizeof(custom_rom));
    memcpy(next_menu, apple2e_video_rom, sizeof(next_menu));
    for (unsigned y = 0U; y < 8U; ++y) {
        menu_rom[0x600U + y] = 0xFFU;       /* normal @: empty */
        custom_rom[0x600U + y] = 0x80U;     /* normal @: solid seven dots */
        next_menu[0x600U + y] = 0xAAU;      /* normal @: striped */
    }
    video_rom_service_set_menu(menu_rom);
    check_status(GLYPH_API_AVAILABLE | GLYPH_API_PENDING, 0U);
    renderer_frames();
    check_status(GLYPH_API_AVAILABLE, 1U);
    check_private_rom(menu_rom);
    memcpy(rendered_before, s_slot_mem[s_pub_slot], sizeof(rendered_before));

    /* Staged chunks leave the currently rendered menu image alone. */
    CHECK(command(GLYPH_API_BEGIN, 123U, 0U, 0U, NULL) == GLYPH_API_OK,
          "BEGIN through real glyph core");
    for (unsigned offset = 0U; offset < 4096U; offset += 512U) {
        CHECK(command(GLYPH_API_WRITE, 123U, (uint16_t)offset, 512U,
                      custom_rom + offset) == GLYPH_API_OK,
              "chunk WRITE through real glyph core");
        check_private_rom(menu_rom);
        CHECK(rom_gen == 1U, "partial uploads never publish");
    }
    put_long(crc, glyph_api_crc32(custom_rom, 4096U));
    CHECK(command(GLYPH_API_COMMIT, 123U, 0U, 0U, crc) == GLYPH_API_OK,
          "whole upload COMMIT queues image");
    check_status(GLYPH_API_AVAILABLE | GLYPH_API_PENDING, 1U);
    CHECK(memcmp(shared, custom_rom, 4096U) == 0, "commit copies staged bytes");
    CHECK(video_rom_service_apply(next_menu) == GLYPH_API_BUSY,
          "stalled renderer gives BUSY on rapid second commit");
    CHECK(video_rom_service_apply(NULL) == GLYPH_API_BUSY,
          "stalled renderer gives BUSY on rapid restore");
    /* C029 updates call the renderer settings path in the middle of a frame.
     * They must not apply a pending character image there. */
    feed(rec_io(0xC029U, 0x00U));
    CHECK(rom_ack == 1U, "midframe settings change does not ACK a ROM");
    check_private_rom(menu_rom);
    renderer_frames();
    check_status(GLYPH_API_AVAILABLE | GLYPH_API_CUSTOM_ACTIVE, 2U);
    check_private_rom(custom_rom);
    CHECK(memcmp(rendered_before, s_slot_mem[s_pub_slot], sizeof(rendered_before)) != 0,
          "unchanged Apple text bytes produce different visible pixels with new glyphs");
    apple_cycle_renderer_reset_local_video_state();
    check_private_rom(custom_rom);
    renderer_frames();
    check_status(GLYPH_API_AVAILABLE | GLYPH_API_CUSTOM_ACTIVE, 2U);

    CHECK(command(GLYPH_API_RESTORE, 0U, 0U, 0U, NULL) == GLYPH_API_OK,
          "RESTORE through real glyph core");
    check_status(GLYPH_API_AVAILABLE | GLYPH_API_CUSTOM_ACTIVE | GLYPH_API_PENDING, 2U);
    renderer_frames();
    check_private_rom(menu_rom);
    check_status(GLYPH_API_AVAILABLE, 3U);
    CHECK(memcmp(rendered_before, s_slot_mem[s_pub_slot], sizeof(rendered_before)) == 0,
          "RESTORE recreates menu-selected text pixels");

    /* Menu changes can supersede each other while CPU1 owns a published image. */
    CHECK(video_rom_service_apply(custom_rom) == GLYPH_API_OK, "direct upload accepted");
    memset(custom_rom, 0x55, sizeof(custom_rom));
    CHECK(shared[0x600U] == 0x80U, "backend takes ownership by copying caller image");
    video_rom_service_set_menu(next_menu);
    video_rom_service_set_menu(NULL);
    video_rom_service_set_menu(menu_rom);
    CHECK(shared[0x600U] == 0x80U, "menu does not overwrite buffer until CPU1 ACK");
    check_status(GLYPH_API_AVAILABLE | GLYPH_API_PENDING, 3U);
    legacy_full_frame(SW_TEXT_ONLY);
    CHECK(rom_ack == 4U, "CPU1 ACKs unchanged in-flight custom image");
    video_rom_service_poll();
    check_status(GLYPH_API_AVAILABLE | GLYPH_API_CUSTOM_ACTIVE | GLYPH_API_PENDING, 4U);
    CHECK(rom_gen == 5U && memcmp(shared, menu_rom, 4096U) == 0,
          "only latest queued menu selection publishes after ACK");
    renderer_frames();
    check_private_rom(menu_rom);
    check_status(GLYPH_API_AVAILABLE, 5U);

    video_rom_service_set_menu(NULL);
    CHECK(rom_gen == 6U && rom_mode == APPLE_FB_VIDEO_ROM_BUILT_IN,
          "built-in mode has a fresh generation");
    renderer_frames();
    check_private_rom(apple2e_video_rom);
    CHECK(video_rom_service_apply(next_menu) == GLYPH_API_OK, "upload over built-in menu");
    renderer_frames();
    CHECK(video_rom_service_apply(NULL) == GLYPH_API_OK, "restore built-in menu");
    renderer_frames();
    check_private_rom(apple2e_video_rom);
    check_status(GLYPH_API_AVAILABLE, 8U);
    video_rom_service_status(NULL, NULL);

    /* Exercise wrap without treating zero as a ROM-mode selector. */
    rom_gen = UINT32_MAX; rom_ack = UINT32_MAX;
    video_rom_service_init();
    video_rom_service_set_menu(menu_rom);
    CHECK(rom_gen == 1U && rom_mode == APPLE_FB_VIDEO_ROM_OVERRIDE,
          "generation wrap keeps image selection separate");
    renderer_frames();
    check_private_rom(menu_rom);
    printf("video ROM service: %u checks, real glyph protocol + renderer pixels PASS\n", checks);
    return 0;
}
