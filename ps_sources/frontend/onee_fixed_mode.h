#ifndef ONEE_FIXED_MODE_H
#define ONEE_FIXED_MODE_H

#include <stdint.h>

#include "card_control_regs.h"

static inline uint8_t onee_usb_fixed_mode_active(uint32_t onee_status)
{
    const uint32_t active_mask =
        CARD_CTRL_ONEE_STATUS_REQUEST_BIT |
        CARD_CTRL_ONEE_STATUS_EFFECTIVE_BIT;

    return ((onee_status & active_mask) != 0U) ? 1U : 0U;
}

#endif /* ONEE_FIXED_MODE_H */
