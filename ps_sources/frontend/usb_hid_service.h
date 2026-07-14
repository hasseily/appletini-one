#ifndef USB_HID_SERVICE_H
#define USB_HID_SERVICE_H

#include <stdint.h>

typedef uint16_t usb_hid_menu_source_t;

typedef enum {
    USB_HID_MENU_ACTION_NONE = 0,
    USB_HID_MENU_ACTION_OPEN,
    USB_HID_MENU_ACTION_CLOSE,
    USB_HID_MENU_ACTION_LEFT,
    USB_HID_MENU_ACTION_RIGHT,
    USB_HID_MENU_ACTION_ITEM_UP,
    USB_HID_MENU_ACTION_ITEM_DOWN,
    USB_HID_MENU_ACTION_SELECT,
    USB_HID_MENU_ACTION_NEXT_TAB,
    USB_HID_MENU_ACTION_PREV_TAB,
    USB_HID_MENU_ACTION_SCREENSHOT_A2,
    USB_HID_MENU_ACTION_SCREENSHOT_1080P
} usb_hid_menu_action_t;

#define USB_HID_MENU_SOURCE_NONE 0U
#define USB_HID_MENU_SOURCE_KEY_BASE 0x0100U
#define USB_HID_MENU_SOURCE_KEY_MASK 0x00FFU

typedef struct {
    usb_hid_menu_action_t action;
    usb_hid_menu_source_t source;
} usb_hid_menu_event_t;

typedef struct {
    uint8_t started;
    uint8_t ready;
    uint8_t menu_capture;
    uint8_t active_count;
    uint8_t keyboard_count;
    uint8_t mouse_count;
    uint32_t report_count;
    uint32_t submit_error_count;
    uint32_t transfer_error_count;
    int last_error;
} usb_hid_service_status_t;

usb_hid_menu_source_t usb_hid_menu_source_from_keyboard_usage(uint8_t usage);
uint8_t usb_hid_menu_source_is_keyboard(usb_hid_menu_source_t source);
const char *usb_hid_menu_source_text(usb_hid_menu_source_t source);
int usb_hid_service_init(void);
int usb_hid_service_start(void);
void usb_hid_service_stop(void);
void usb_hid_service_poll(void);
uint32_t usb_hid_service_activity_count(void);
void usb_hid_service_set_sensitivity(uint8_t sensitivity);
void usb_hid_service_set_menu_capture(uint8_t capture);
void usb_hid_service_set_menu_ok_source(usb_hid_menu_source_t source);
void usb_hid_service_set_menu_open_close_source(usb_hid_menu_source_t source);
void usb_hid_service_set_screenshot_sources(usb_hid_menu_source_t a2_source,
                                            usb_hid_menu_source_t full_source);
int usb_hid_service_pop_menu_event(usb_hid_menu_event_t *event);
void usb_hid_service_get_status(usb_hid_service_status_t *status);
void usb_hid_service_dump_status(uint32_t uart_base);

#endif /* USB_HID_SERVICE_H */
