#ifndef CHERRYUSB_PLATFORM_H
#define CHERRYUSB_PLATFORM_H

#include <stdint.h>

#define CHERRYUSB_USB1_BUSID 0U

struct usbh_hubport;
struct usb_setup_packet;

typedef struct {
    uint32_t polls;
    uint32_t root_probe_candidates;
    uint32_t root_probe_attempts;
    uint32_t root_probe_synths;
    uint32_t mq_events;
    uint32_t event_calls;
    uint32_t root_event_calls;
    uint32_t port_event_count;
    uint32_t synth_connect_count;
    uint32_t debounce_fail_count;
    uint32_t reset_count;
    uint32_t enumerate_count;
    uint32_t control_count;
    uint32_t control_fail_count;
    uint32_t class_connect_count;
    uint32_t class_connect_fail_count;
    uint32_t ehci_error_count;
    uint32_t ehci_zlp_babble_ignored;
    uint32_t last_root_portsc;
    uint32_t last_ehci_qtd_token;
    uint16_t last_portchange_index;
    uint16_t last_port_status;
    uint16_t last_port_change;
    uint16_t last_debounce_status;
    uint16_t last_debounce_change;
    uint16_t last_debounce_stable_ms;
    uint16_t last_reset_status;
    uint16_t last_reset_change;
    uint16_t last_control_value;
    uint16_t last_control_index;
    uint16_t last_control_length;
    uint16_t last_control_ep0_mps;
    uint16_t last_ehci_qtd_length;
    uint16_t last_ehci_qtd_remaining;
    uint16_t last_ehci_qtd_actual;
    uint8_t last_hub_index;
    uint8_t last_hub_is_root;
    uint8_t last_port;
    uint8_t last_enum_stage;
    uint8_t last_ehci_qtd_index;
    uint8_t last_ehci_qtd_pid;
    uint8_t last_control_request_type;
    uint8_t last_control_request;
    uint8_t last_control_dev_addr;
    uint8_t last_control_speed;
    uint8_t last_class_intf;
    uint8_t last_class_class;
    uint8_t last_class_subclass;
    uint8_t last_class_protocol;
    uint8_t last_class_dev_addr;
    uint8_t last_class_speed;
    uint8_t last_class_started;
    int last_get_status_ret;
    int last_clear_feature_ret;
    int last_debounce_ret;
    int last_set_reset_ret;
    int last_post_reset_status_ret;
    int last_enumerate_ret;
    int last_control_ret;
    int last_class_ret;
    const char *last_class_driver;
} cherryusb_host_debug_t;

uint32_t cherryusb_baremetal_ms(void);
void cherryusb_baremetal_osal_poll(void);
void cherryusb_baremetal_poll_irq(void);
void cherryusb_host_poll(uint8_t busid);
void cherryusb_host_debug_snapshot(uint8_t busid, cherryusb_host_debug_t *snapshot);
void cherryusb_host_debug_note_control(const struct usbh_hubport *hport,
                                       const struct usb_setup_packet *setup,
                                       int ret);
void cherryusb_host_debug_note_ehci_qtd(uint8_t busid,
                                        uint8_t qtd_index,
                                        uint32_t token,
                                        uint16_t length,
                                        uint16_t remaining);
void cherryusb_host_debug_note_ehci_zlp_babble_ignored(uint8_t busid);
void cherryusb_host_debug_note_enum_stage(const struct usbh_hubport *hport,
                                          uint8_t stage);
void cherryusb_host_debug_note_class_connect(const struct usbh_hubport *hport,
                                             uint8_t intf,
                                             const char *driver_name,
                                             int ret,
                                             uint8_t started);
uint32_t cherryusb_usb1_portsc(void);
int cherryusb_printf(const char *fmt, ...);

#endif /* CHERRYUSB_PLATFORM_H */
