#ifndef BEZEL_LOADER_H
#define BEZEL_LOADER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int bezel_loader_decode_png_rgb565(const unsigned char *png_data,
                                   size_t png_size,
                                   const char *label,
                                   uint16_t **out_pixels,
                                   unsigned *out_w,
                                   unsigned *out_h,
                                   char *errbuf,
                                   size_t errbuf_size);

int bezel_loader_load_png_rgb565(const char *path,
                                 uint16_t **out_pixels,
                                 unsigned *out_w,
                                 unsigned *out_h,
                                 char *errbuf,
                                 size_t errbuf_size);

void bezel_loader_free_rgb565(uint16_t *pixels);

#ifdef __cplusplus
}
#endif

#endif
