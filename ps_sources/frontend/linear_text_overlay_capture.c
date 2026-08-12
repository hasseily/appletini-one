#include "linear_text_overlay.h"

#include <stddef.h>
#include <stdint.h>

#include "../lib/common.h"

static linear_text_overlay_config_t s_capture;
static uint8_t s_capture_valid;

static volatile uint8_t *shadow_slot(uint8_t slot)
{
    const uintptr_t addr = (slot != 0U) ? LTO_SHADOW_SLOT1_ADDR
                                       : LTO_SHADOW_SLOT0_ADDR;
    return (volatile uint8_t *)addr;
}

static void read_armed_config(linear_text_overlay_config_t *cfg)
{
    const uint32_t arm0 = REG_READ(LTO_R_ARM0);
    const uint32_t arm1 = REG_READ(LTO_R_ARM1);
    const uint32_t arm2 = REG_READ(LTO_R_ARM2);

    cfg->base = (uint16_t)(arm0 & 0xFFFFU);
    cfg->config = (uint8_t)((arm0 >> 16) & 0xFFU);
    cfg->cols = (uint8_t)(arm0 >> 24);
    cfg->rows = (uint8_t)(arm1 & 0xFFU);
    cfg->origin_x = (uint16_t)((arm1 >> 8) & 0xFFFFU);
    cfg->origin_y = (uint16_t)(((arm2 & 0xFFU) << 8) |
                              ((arm1 >> 24) & 0xFFU));
    cfg->scale = (uint8_t)((arm2 >> 8) & 0xFFU);
    cfg->fill_char = (uint8_t)((arm2 >> 16) & 0xFFU);
    cfg->fill_attr = (uint8_t)(arm2 >> 24);
}

void linear_text_overlay_capture_init(void)
{
    s_capture_valid = 0U;
}

void linear_text_overlay_capture_poll(void)
{
    const uint32_t state = REG_READ(LTO_R_STATE);

    if ((state & LTO_STATE_ARM_REQUEST) != 0U) {
        uint32_t cells;
        volatile uint16_t *dst;
        uint16_t fill;

        s_capture_valid = 0U;
        read_armed_config(&s_capture);
        s_capture.slot = ((state & LTO_STATE_ACTIVE_SLOT) != 0U) ? 0U : 1U;
        cells = (uint32_t)s_capture.cols * (uint32_t)s_capture.rows;
        dst = (volatile uint16_t *)shadow_slot(s_capture.slot);
        fill = (uint16_t)s_capture.fill_char |
               ((uint16_t)s_capture.fill_attr << 8);
        for (uint32_t i = 0U; i < cells; ++i) {
            dst[i] = fill;
        }
        __asm__ volatile ("dsb sy" ::: "memory");
        s_capture_valid = 1U;
        REG_WRITE(LTO_R_CONTROL,
                  LTO_CTL_ACK_ARM |
                  ((s_capture.slot != 0U) ? LTO_CTL_ARM_SLOT : 0U));
        return;
    }

    if ((state & (LTO_STATE_ARMED | LTO_STATE_BUSY)) == 0U) {
        s_capture_valid = 0U;
    }
}

void linear_text_overlay_capture_on_gap(void)
{
    s_capture_valid = 0U;
    REG_WRITE(LTO_R_CONTROL, LTO_CTL_MARK_STALE);
}

void linear_text_overlay_capture_on_write(uint32_t decoded_addr,
                                          uint8_t data)
{
    uint32_t apple_addr;
    uint32_t end;
    uint8_t aux;

    if (s_capture_valid == 0U) {
        return;
    }
    if ((decoded_addr & 0xFE0000U) != 0U) {
        return;
    }

    aux = ((decoded_addr & 0x010000U) != 0U) ? 1U : 0U;
    if (aux != ((s_capture.config & LTO_CONFIG_AUX) != 0U)) {
        return;
    }

    apple_addr = decoded_addr & 0xFFFFU;
    end = (uint32_t)s_capture.base +
          (2U * (uint32_t)s_capture.cols * (uint32_t)s_capture.rows);
    if (apple_addr < s_capture.base || apple_addr >= end) {
        return;
    }
    shadow_slot(s_capture.slot)[apple_addr - s_capture.base] = data;
}

void linear_text_overlay_capture_on_command(uint8_t command)
{
    if (command != 0x02U) {
        return;
    }

    /* The command record follows all prior memory-write records in the
     * capture ring. This barrier completes those shadow stores before CPU0
     * may accept SHOW at an output-frame edge. */
    __asm__ volatile ("dsb sy" ::: "memory");
    REG_WRITE(LTO_R_CONTROL, LTO_CTL_SHOW_DRAINED);
}
