/* Native file-level regression. Device services and unrelated setting helpers
 * are substitutes; serialization/parser/reset/profile/apply code is production. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "config_menu_internal.h"
#include "profile_manager.h"
#include "card_control_regs.h"
#include "display_modes.h"
#include "video_mono.h"
#include "video_blur.h"
#include "video_ghosting.h"
#include "video_glow.h"
#include "video_filter_config.h"
#include "video_pixel_mask.h"

/* CONSTANTS */

#define CHECK(condition, message) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %u: %s\n", __LINE__, message); exit(1); } } while (0)

typedef struct {
    char path[CONFIG_MENU_PATH_LEN];
    char bytes[APPLETINI_CFG_MAX];
    unsigned length;
    unsigned writes;
} memory_file_t;
static memory_file_t files[8];
static unsigned file_count;
static uint8_t fail_read;
static unsigned desired_calls, mode_calls, restore_calls;
static uint8_t desired, active;

static memory_file_t *file_for(const char *path, int create)
{
    unsigned index;
    for (index = 0; index < file_count; ++index) {
        if (strcmp(files[index].path, path) == 0) return &files[index];
    }
    if (!create) return NULL;
    CHECK(file_count < 8, "file adapter capacity");
    snprintf(files[file_count].path, sizeof(files[file_count].path), "%s", path);
    return &files[file_count++];
}

static FRESULT config_menu_open_path(FIL *file, const char *path, BYTE mode)
{
    memory_file_t *data = file_for(path, (mode & FA_CREATE_ALWAYS) != 0);
    if (data == NULL) return FR_NO_FILE;
    if ((mode & FA_CREATE_ALWAYS) != 0) data->length = 0;
    file->slot = (unsigned)(data - files);
    file->position = 0;
    return FR_OK;
}
static FRESULT f_read(FIL *file, void *buffer, UINT length, UINT *read)
{
    memory_file_t *data = &files[file->slot];
    if (fail_read) return FR_DISK_ERR;
    *read = data->length < length ? data->length : length;
    memcpy(buffer, data->bytes, *read);
    return FR_OK;
}
static FRESULT f_write(FIL *file, const void *buffer, UINT length, UINT *written)
{
    memory_file_t *data = &files[file->slot];
    CHECK(length < sizeof(data->bytes), "serialized file fits");
    memcpy(data->bytes, buffer, length);
    data->bytes[length] = '\0';
    data->length = length;
    ++data->writes;
    *written = length;
    return FR_OK;
}
static FRESULT f_close(FIL *file) { (void)file; return FR_OK; }
static FRESULT f_sync(FIL *file) { (void)file; return FR_OK; }
static FRESULT f_unlink(const char *path) { (void)path; return FR_OK; }
static FRESULT config_menu_commit_global_cfg(void)
{
    memory_file_t *temp = file_for(APPLETINI_CFG_TMP_PATH, 0);
    memory_file_t *global = file_for(APPLETINI_CFG_PATH, 1);
    CHECK(temp != NULL, "global commit has synced temp data");
    memcpy(global->bytes, temp->bytes, temp->length + 1);
    global->length = temp->length;
    ++global->writes;
    return FR_OK;
}
void config_menu_set_status(config_menu_t *menu, uint8_t error, const char *message)
{
    (void)menu; (void)error; (void)message;
}
static void config_menu_set_sd_error(config_menu_t *menu, const char *message, FRESULT error)
{
    (void)menu; (void)message; (void)error;
}
uint8_t profile_manager_cfg_path(const char *dir, char *out, size_t capacity)
{
    int length = snprintf(out, capacity, "%s/appletini_cfg.txt", dir);
    return length >= 0 && (size_t)length < capacity;
}
void uthernet2_default_config(uthernet2_network_config_t *config)
{
    memset(config, 0, sizeof(*config));
}
void onee_input_service_default_joystick_config(onee_input_joystick_config_t *config)
{
    memset(config, 0, sizeof(*config));
}
void onee_input_service_set_joystick_config(const onee_input_joystick_config_t *config)
{
    (void)config;
}

/* DECLARATIONS */
/* UNRELATED_HELPER_STUBS */
/* PRODUCTION_FUNCTIONS */

static void set_desired(void *context, uint8_t standard)
{
    (void)context;
    desired = standard;
    ++desired_calls;
}
static uint8_t get_active(void *context) { (void)context; return active; }
static uint8_t set_mode(void *context, uint8_t enabled)
{
    (void)context; (void)enabled;
    ++mode_calls;
    return 1;
}
static void restore_mode(void *context, uint8_t enabled)
{
    (void)context; (void)enabled;
    ++restore_calls;
}
static void raw_profile(const char *dir, const char *text)
{
    char path[CONFIG_MENU_PATH_LEN];
    memory_file_t *file;
    CHECK(profile_manager_cfg_path(dir, path, sizeof(path)), "raw fixture path");
    file = file_for(path, 1);
    snprintf(file->bytes, sizeof(file->bytes), "%s", text);
    file->length = (unsigned)strlen(file->bytes);
}
static void verify_load(config_menu_t *menu, const char *dir, uint8_t expected)
{
    unsigned before = desired_calls;
    uint8_t enabled = menu->onee_persisted_enabled;
    CHECK(config_menu_load_profile_settings(menu, dir), "profile load succeeds");
    CHECK(menu->onee_video_50hz == expected && desired == expected,
          "saved profile standard reaches desired callback");
    CHECK(desired_calls == before + 1, "desired standard applied once");
    CHECK(menu->onee_persisted_enabled == enabled, "profile leaves global ONEe enable unchanged");
    CHECK(mode_calls == 0 && restore_calls == 0, "profile never starts or restores ONEe");
    CHECK(strcmp(menu->profile_source_dir, dir) == 0, "loaded profile source selected");
    CHECK(strstr(file_for(APPLETINI_CFG_PATH, 0)->bytes,
                 expected ? "onee.video.standard=PAL\n" : "onee.video.standard=NTSC\n"),
          "working autosave records loaded standard");
}

int main(void)
{
    config_menu_t menu;
    memory_file_t *pal, *ntsc;
    unsigned saved_pal_writes, saved_ntsc_writes, before;
    char saved_pal[APPLETINI_CFG_MAX];
    memset(&menu, 0, sizeof(menu));
    menu.platform.set_onee_video_50hz = set_desired;
    menu.platform.get_onee_video_active_50hz = get_active;
    menu.platform.set_onee_mode = set_mode;
    menu.platform.restore_onee_mode_intent = restore_mode;
    menu.onee_persisted_enabled = 1;
    menu.onee_video_50hz = 1;
    CHECK(config_menu_save_profile_settings(&menu, "0:/profiles/PAL"), "save PAL profile");
    pal = file_for("0:/profiles/PAL/appletini_cfg.txt", 0);
    CHECK(strstr(pal->bytes, "onee.video.standard=PAL\n"), "PAL serialized into profile");
    CHECK(strstr(pal->bytes, "onee.standalone.persisted") == NULL,
          "profile omits global enable latch");
    memcpy(saved_pal, pal->bytes, pal->length + 1);
    menu.onee_video_50hz = 0;
    CHECK(config_menu_save_profile_settings(&menu, "0:/profiles/NTSC"), "save NTSC profile");
    ntsc = file_for("0:/profiles/NTSC/appletini_cfg.txt", 0);
    CHECK(strstr(ntsc->bytes, "onee.video.standard=NTSC\n"), "NTSC serialized into profile");
    saved_pal_writes = pal->writes;
    saved_ntsc_writes = ntsc->writes;
    puts("PASS PAL/NTSC profile serialization excludes global enable");

    menu.onee_persisted_enabled = 0;
    active = 0;
    verify_load(&menu, "0:/profiles/PAL", 1);
    CHECK(active == 0 && strcmp(config_menu_onee_video_standard_text(&menu), "PAL (pending reset)") == 0,
          "PAL desired does not immediately switch active NTSC");
    menu.onee_persisted_enabled = 1;
    active = 1;
    verify_load(&menu, "0:/profiles/NTSC", 0);
    CHECK(active == 1 && strcmp(config_menu_onee_video_standard_text(&menu), "NTSC (pending reset)") == 0,
          "NTSC desired does not immediately switch active PAL");
    verify_load(&menu, "0:/profiles/PAL", 1);
    CHECK(pal->writes == saved_pal_writes && ntsc->writes == saved_ntsc_writes &&
          strcmp(pal->bytes, saved_pal) == 0, "loading never overwrites source profiles");
    puts("PASS alternating profile restore, desired callback, pending reset and working autosave");

    raw_profile("0:/profiles/legacy", "appletini.config.version=117\nram.enabled=ON\n");
    menu.onee_video_50hz = 1;
    verify_load(&menu, "0:/profiles/legacy", 0);
    CHECK(config_menu_save_profile_settings(&menu, "0:/profiles/legacy"), "legacy profile can be resaved");
    CHECK(strstr(file_for("0:/profiles/legacy/appletini_cfg.txt", 0)->bytes,
                 "onee.video.standard=NTSC\n"), "legacy resave records explicit default");
    puts("PASS legacy profile defaults NTSC instead of inheriting PAL");

    raw_profile("0:/profiles/forged", "onee.standalone.persisted=ON\nONEE.VIDEO.STANDARD=pal # comment\n");
    menu.onee_persisted_enabled = 0;
    verify_load(&menu, "0:/profiles/forged", 1);
    raw_profile("0:/profiles/forged", "onee.standalone.persisted=OFF\nonee.video.standard=not-a-standard\n");
    menu.onee_persisted_enabled = 1;
    verify_load(&menu, "0:/profiles/forged", 0);
    puts("PASS case-insensitive PAL, invalid fallback, forged enable ON/OFF ignored");

    menu.onee_video_50hz = 1;
    before = desired_calls;
    CHECK(!config_menu_load_profile_settings(&menu, "0:/profiles/missing"), "missing profile reports failure");
    CHECK(menu.onee_video_50hz == 1 && desired_calls == before, "failed open preserves selected standard");
    fail_read = 1;
    CHECK(!config_menu_load_profile_settings(&menu, "0:/profiles/NTSC"), "failed read reports failure");
    CHECK(menu.onee_video_50hz == 1 && desired_calls == before && mode_calls == 0 && restore_calls == 0,
          "failed read preserves standard without platform side effects");
    fail_read = 0;
    puts("PASS failed profile open/read preserve standard and avoid platform writes");

    CHECK(config_menu_save_settings_to_path(&menu, APPLETINI_CFG_PATH, NULL), "global config save succeeds");
    CHECK(strstr(file_for(APPLETINI_CFG_PATH, 0)->bytes, "onee.standalone.persisted=ON\n") &&
          strstr(file_for(APPLETINI_CFG_PATH, 0)->bytes, "onee.video.standard=PAL\n"),
          "global config keeps enable and desired standard");
    puts("PASS global configuration retains enable plus video standard");
    puts("6 ONEe profile video groups passed (native; no hardware)");
    return 0;
}
