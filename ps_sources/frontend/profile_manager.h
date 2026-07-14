#ifndef PROFILE_MANAGER_H
#define PROFILE_MANAGER_H

#include <stddef.h>
#include <stdint.h>

#include "ff.h"

#define PROFILE_MANAGER_ROOT "0:/profiles"
#define PROFILE_MANAGER_CFG_NAME "appletini_cfg.txt"
#define PROFILE_MANAGER_THUMB_NAME "thumb.png"
#define PROFILE_MANAGER_THUMB_W 560U
#define PROFILE_MANAGER_THUMB_H 384U
#define PROFILE_MANAGER_PATH_LEN 128U

typedef enum {
    PROFILE_MANAGER_ENTRY_FOLDER = 0,
    PROFILE_MANAGER_ENTRY_PROFILE
} profile_manager_entry_type_t;

typedef struct {
    profile_manager_entry_type_t type;
    char name[PROFILE_MANAGER_PATH_LEN];
    char path[PROFILE_MANAGER_PATH_LEN];
    char cfg_path[PROFILE_MANAGER_PATH_LEN];
    char thumb_path[PROFILE_MANAGER_PATH_LEN];
} profile_manager_entry_t;

FRESULT profile_manager_mount(void);
FRESULT profile_manager_ensure_root(void);
uint8_t profile_manager_is_root(const char *path);
uint8_t profile_manager_parent_path(const char *path, char *out, size_t out_len);
uint8_t profile_manager_join_path(const char *dir,
                                  const char *name,
                                  char *out,
                                  size_t out_len);
uint8_t profile_manager_cfg_path(const char *profile_dir, char *out, size_t out_len);
uint8_t profile_manager_thumb_path(const char *profile_dir, char *out, size_t out_len);
uint8_t profile_manager_is_profile_dir(const char *dir);
FRESULT profile_manager_list_dir(const char *dir,
                                 profile_manager_entry_t *entries,
                                 uint16_t max_entries,
                                 uint16_t *out_count);
uint8_t profile_manager_profile_name_valid(const char *name);
FRESULT profile_manager_create_profile(const char *parent_dir,
                                       const char *name,
                                       char *out_dir,
                                       size_t out_dir_len);
FRESULT profile_manager_rename_profile(const char *profile_dir,
                                       const char *name,
                                       char *out_dir,
                                       size_t out_dir_len);
int profile_manager_load_thumb_bgra32(const char *thumb_path,
                                      uint32_t **out_pixels,
                                      unsigned *out_w,
                                      unsigned *out_h,
                                      char *errbuf,
                                      size_t errbuf_size);
int profile_manager_normalize_thumb_png(const char *src_png_path,
                                        const char *profile_dir,
                                        char *errbuf,
                                        size_t errbuf_size);
void profile_manager_free_bgra32(uint32_t *pixels);

#endif
