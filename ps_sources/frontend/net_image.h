#ifndef NET_IMAGE_H
#define NET_IMAGE_H
#include <stddef.h>
#include <stdint.h>

/* Picture service for Apple programs: decode a PNG, JPEG or GIF, shrink it
 * to a box and dither it to 1 bit. The Apple gets rows that it can copy to
 * a 560-column monochrome screen without arithmetic.
 *
 * Output: width (LE16, pixels), height (LE16, pixels), then height rows of
 * (width+6)/7 bytes. A byte holds seven pixels, bit 0 leftmost, 1 = white.
 * Bit 7 and the padding pixels of the last byte are 0. */
#define NET_IMAGE_ENCODED_MAX (8U * 1024U * 1024U)
#define NET_IMAGE_PIXELS_MAX  4194304U
#define NET_IMAGE_SIDE_MAX    4096U
#define NET_IMAGE_BOX_W_MAX   560U
#define NET_IMAGE_BOX_H_MAX   384U
#define NET_IMAGE_HEADER      4U
#define NET_IMAGE_OUT_MAX     (NET_IMAGE_HEADER + 80U * NET_IMAGE_BOX_H_MAX)

enum { NET_IMAGE_E_FORMAT = -120, NET_IMAGE_E_SIZE = -121, NET_IMAGE_E_MEMORY = -122 };

/* Returns 0 and the output length, or one of the errors above. The image
 * never grows; it keeps its proportions. Transparent pixels become white,
 * or black when the opaque part of the image is light. */
int net_image_convert(const uint8_t *data, size_t length, unsigned box_w, unsigned box_h,
                      uint8_t out[NET_IMAGE_OUT_MAX], size_t *out_length);
#endif
