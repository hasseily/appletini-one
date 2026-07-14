#ifndef USB_STORAGE_SERVICE_H
#define USB_STORAGE_SERVICE_H

#include <stdint.h>

#include "xil_types.h"

typedef struct {
    u64 starts;
    u64 irq_count;
    u64 reset_irqs;
    u64 ui_irqs;
    u64 ue_irqs;
    u64 pc_irqs;
    u64 soft_disconnects;
    u64 soft_connects;
    u64 ep0_setup_events;
    u64 ep0_data_rx_events;
    u64 ep0_rx_failures;
    u64 ep0_other_events;
    u64 ep0_stalls;
    u64 ep0_send_failures;
    u64 class_requests;
    u64 msc_resets;
    u64 clear_feature_halt;
    u64 setup_errors;
    u64 set_address_requests;
    u64 set_configuration_requests;
    u64 get_descriptor_device;
    u64 get_descriptor_config;
    u64 get_descriptor_string;
    u64 get_descriptor_qualifier;
    u64 get_descriptor_other;
    u64 ep1_rx_events;
    u64 ep1_tx_events;
    u64 ep1_rx_failures;
    u64 ep1_other_events;
    u64 ep0_prime_count;
    u64 ep0_prime_failures;
    u64 ep1_prime_count;
    u64 ep1_prime_failures;
    u64 out_packets;
    u64 out_bytes;
    u64 queued_packets;
    u64 queued_bytes;
    u64 processed_packets;
    u64 processed_bytes;
    u64 fast_packets;
    u64 fast_bytes;
    u64 queue_drops;
    u32 queue_high_water;
    u32 last_queue_depth;
    u32 current_config;
    u32 need_ep0_prime;
    u32 last_irq_mask;
    u32 last_setup_bm_request_type;
    u32 last_setup_b_request;
    u32 last_setup_w_value;
    u32 last_setup_w_index;
    u32 last_setup_w_length;
    u32 last_desc_type;
    u32 last_desc_request_len;
    u32 last_desc_reply_len;
    u32 last_ep0_event;
    u32 last_ep1_event;
    u32 last_prime_ep;
    u32 last_prime_dir;
    u32 last_prime_status;
    u32 last_send_ep;
    u32 last_send_len;
    u32 last_send_status;
    u32 last_usb_cmd;
    u32 last_usb_sts;
    u32 last_usb_intr;
    u32 last_deviceaddr;
    u32 last_eplistaddr;
    u32 last_usb_mode;
    u32 last_portsc;
    u32 last_otgsc;
    u32 last_epstat;
    u32 last_epprime;
    u32 last_eprdy;
    u32 last_epcomplete;
    u32 last_epcr0;
    u32 last_epcr1;
    u32 ep1in_dqh;
    u32 ep1in_dqh_cfg;
    u32 ep1in_dqh_cptr;
    u32 ep1in_dqh_next;
    u32 ep1in_dqh_token;
    u32 ep1in_dtds;
    u32 ep1in_head;
    u32 ep1in_head_next;
    u32 ep1in_head_token;
    u32 ep1in_head_buf;
    u32 ep1in_tail;
    u32 ep1in_tail_next;
    u32 ep1in_tail_token;
    u32 ep1in_tail_buf;
    u32 ep1in_requested_bytes;
    u32 ep1in_bytes_txed;
    u32 ep1in_buffer_ptr;
    u32 last_slcr_usb0_clk_ctrl;
    u32 last_slcr_usb1_clk_ctrl;
    u32 last_slcr_usb_rst_ctrl;
    u32 last_usb0_mio[12];
    u32 last_ulpi_view;
} usb_storage_service_stats_t;

int usb_storage_service_init(void);
void usb_storage_service_connect(void);
uint8_t usb_storage_service_disconnect(void);
void usb_storage_service_poll(void);
int usb_storage_service_needs_attention(void);
uint8_t usb_storage_service_consume_host_eject_request(void);
void usb_storage_service_get_stats(usb_storage_service_stats_t *stats);
void usb_storage_service_reset_stats(void);

/* The USB0 controller instance and its GIC wiring are owned here
 * permanently; the SDD personality (usb_sdd_service.c) borrows the
 * instance while active and hands it back through
 * usb_storage_service_connect(). Returns the XUsbPs instance; typed
 * void* so this header stays free of BSP includes. */
void *usb_storage_service_usb_instance(void);

#endif /* USB_STORAGE_SERVICE_H */
