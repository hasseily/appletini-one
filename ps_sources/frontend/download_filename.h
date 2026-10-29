/* SPDX-License-Identifier: MIT */
#ifndef DOWNLOAD_FILENAME_H
#define DOWNLOAD_FILENAME_H
#include <stddef.h>

#define DOWNLOAD_FILENAME_MAX 96U
/* Return an ASCII FAT filename, never a directory/path. preferred is an
 * already decoded suggestion; URL basename escapes are decoded here.
 * The caller owns the fixed destination directory and collision handling.
 * capacity must hold at least "download.bin" plus its terminating NUL. */
int download_filename(const char *preferred, const char *final_url,
                      char *out, size_t capacity);
#endif
