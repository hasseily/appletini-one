#ifndef LINEAR_TEXT_OVERLAY_H
#define LINEAR_TEXT_OVERLAY_H

#include <stdint.h>

#define LTO_SP_BASE                 0x40020000U
#define LTO_REG(index)              (LTO_SP_BASE + ((index) * 4U))
#define LTO_R_STATE                 LTO_REG(0x20U)
#define LTO_R_ARM0                  LTO_REG(0x21U)
#define LTO_R_ARM1                  LTO_REG(0x22U)
#define LTO_R_ARM2                  LTO_REG(0x23U)
#define LTO_R_CURSOR                LTO_REG(0x24U)
#define LTO_R_ACTIVE0               LTO_REG(0x25U)
#define LTO_R_ACTIVE1               LTO_REG(0x26U)
#define LTO_R_ACTIVE2               LTO_REG(0x27U)
#define LTO_R_CONTROL               LTO_REG(0x28U)

#define LTO_STATE_ARM_REQUEST       (1U << 0)
#define LTO_STATE_FRAME_REQUEST     (1U << 1)
#define LTO_STATE_SHOW_DRAINED      (1U << 2)
#define LTO_STATE_FRAME_CMD_SHIFT   3U
#define LTO_STATE_FRAME_CMD_MASK    (3U << LTO_STATE_FRAME_CMD_SHIFT)
#define LTO_STATE_ARMED_SLOT        (1U << 5)
#define LTO_STATE_ACTIVE_SLOT       (1U << 6)
#define LTO_STATE_BUSY              (1U << 7)
#define LTO_STATE_STALE             (1U << 8)
#define LTO_STATE_CONFIG_ERROR      (1U << 9)
#define LTO_STATE_FRAME_PENDING     (1U << 10)
#define LTO_STATE_ARMED             (1U << 11)
#define LTO_STATE_VISIBLE           (1U << 12)

#define LTO_FRAME_SHOW              1U
#define LTO_FRAME_HIDE              2U
#define LTO_FRAME_OFF               3U

#define LTO_CTL_ACK_ARM             (1U << 0)
#define LTO_CTL_ARM_SLOT            (1U << 1)
#define LTO_CTL_SHOW_DRAINED        (1U << 2)
#define LTO_CTL_ACK_FRAME           (1U << 3)
#define LTO_CTL_MARK_STALE          (1U << 4)

#define LTO_POLL_REFRESH            (1U << 0)
#define LTO_POLL_WAIT_LATCH         (1U << 1)

#define LTO_CONFIG_AUX              (1U << 0)
#define LTO_CONFIG_CP437            (1U << 1)
#define LTO_CONFIG_FONT_8X16        (1U << 2)
#define LTO_CONFIG_BLINK_ATTR       (1U << 3)
#define LTO_CONFIG_TRANSPARENT      (1U << 4)

#define LTO_CURSOR_ENABLE           (1U << 0)
#define LTO_CURSOR_BLINK            (1U << 1)
#define LTO_CURSOR_SHAPE_SHIFT      2U
#define LTO_CURSOR_SHAPE_MASK       (3U << LTO_CURSOR_SHAPE_SHIFT)

#define LTO_SHADOW_SLOT_BYTES       0x0000C000U
#define LTO_SHADOW_SLOT0_ADDR       0x3F0D1000U
#define LTO_SHADOW_SLOT1_ADDR       0x3F0DD000U

typedef struct {
    uint16_t base;
    uint8_t config;
    uint8_t cols;
    uint8_t rows;
    uint16_t origin_x;
    uint16_t origin_y;
    uint8_t scale;
    uint8_t fill_char;
    uint8_t fill_attr;
    uint8_t slot;
} linear_text_overlay_config_t;

void linear_text_overlay_init(void);
uint8_t linear_text_overlay_poll_frame_latch(uint8_t prior_publish_latched);
void linear_text_overlay_draw(uint16_t *framebuffer, uint8_t shr_canvas);

void linear_text_overlay_capture_init(void);
void linear_text_overlay_capture_poll(void);
void linear_text_overlay_capture_on_gap(void);
void linear_text_overlay_capture_on_write(uint32_t decoded_addr,
                                          uint8_t data);
void linear_text_overlay_capture_on_command(uint8_t command);

#endif
