#ifndef USB_PHY_INIT_H
#define USB_PHY_INIT_H

#include <stdint.h>

typedef struct {
    uint32_t usb0_clk_ctrl;
    uint32_t usb1_clk_ctrl;
    uint32_t usb_rst_ctrl;
    uint32_t usb0_mio[12];
} usb_phy_status_t;

void usb_phy_early_init(void);
void usb_phy_get_status(usb_phy_status_t *status);

#endif /* USB_PHY_INIT_H */
