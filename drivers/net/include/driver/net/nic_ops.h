#ifndef IPO_DRIVER_NET_NIC_OPS_H
#define IPO_DRIVER_NET_NIC_OPS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *driver_name;
    bool (*init)(void);
    void (*shutdown)(void);
    bool (*is_present)(void);
    bool (*is_link_up)(void);
    const uint8_t *(*get_mac)(size_t *out_len);
    size_t (*get_mac_len)(void);
    uint16_t (*get_vendor_id)(void);
    uint16_t (*get_device_id)(void);
    uint16_t (*get_subsys_vendor_id)(void);
    uint16_t (*get_subsys_id)(void);
    uint8_t (*get_revision_id)(void);
    uint8_t (*get_bus)(void);
    uint8_t (*get_slot)(void);
    uint8_t (*get_func)(void);
    int (*send_packet)(const void *data, uint16_t len);
    int (*receive_packet)(void *buf, uint32_t max_len);
    int (*peek_packet_size)(void);
    size_t (*get_hw_rx_ring_size)(void);
    size_t (*get_hw_tx_descriptor_count)(void);
    size_t (*get_hw_max_dma_burst)(void);
} net_nic_ops_t;

extern const net_nic_ops_t g_rtl8139_nic_ops;
extern const net_nic_ops_t g_e1000_nic_ops;
extern const net_nic_ops_t g_virtio_net_nic_ops;

const char *nic_get_driver_name(void);

#endif /* IPO_DRIVER_NET_NIC_OPS_H */
