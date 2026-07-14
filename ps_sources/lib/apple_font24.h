#ifndef APPLE_FONT24_H
#define APPLE_FONT24_H

// this font was generated via:
// py scripts/ttf_to_c_font.py --ttf scripts/PrintChar21.ttf --size 24 --name apple_font24 --out-dir vitis_workspace/text_ui_test/

// It is totally optional as the default 7x8 font is embedded inside fb16.c

#include <stdint.h>
#include "fb16.h"

extern const uint8_t apple_font24_data[];
extern const fb16_bitmap_font_t apple_font24;

#endif
