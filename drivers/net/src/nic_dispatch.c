#include <driver/net/rtl8139.h>
#include <driver/net/nic_ops.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

static const net_nic_ops_t *s_active_nic = NULL;

const char *nic_get_driver_name(void) {
    return (s_active_nic && s_active_nic->driver_name) ? s_active_nic->driver_name : "none";
}

bool rtl8139_init(void) {
    if (s_active_nic && s_active_nic->is_present && s_active_nic->is_present()) {
        return true;
    }

    static const net_nic_ops_t *const k_backends[] = {
        &g_rtl8139_nic_ops,
        &g_e1000_nic_ops,
        &g_virtio_net_nic_ops,
    };

    for (size_t i = 0; i < sizeof(k_backends) / sizeof(k_backends[0]); i++) {
        const net_nic_ops_t *cand = k_backends[i];
        if (cand && cand->init && cand->init()) {
            s_active_nic = cand;
            return true;
        }
    }

    s_active_nic = NULL;
    return false;
}

void rtl8139_shutdown(void) {
    if (s_active_nic && s_active_nic->shutdown) {
        s_active_nic->shutdown();
    }
    s_active_nic = NULL;
}

bool rtl8139_is_present(void) {
    return s_active_nic && s_active_nic->is_present && s_active_nic->is_present();
}

bool rtl8139_is_link_up(void) {
    return (s_active_nic && s_active_nic->is_link_up) ? s_active_nic->is_link_up() : false;
}

const uint8_t *rtl8139_get_mac(size_t *out_len) {
    if (s_active_nic && s_active_nic->get_mac) {
        return s_active_nic->get_mac(out_len);
    }
    if (out_len) *out_len = 0u;
    return NULL;
}

size_t rtl8139_get_mac_len(void) {
    return (s_active_nic && s_active_nic->get_mac_len) ? s_active_nic->get_mac_len() : 0u;
}

uint16_t rtl8139_get_vendor_id(void) {
    return (s_active_nic && s_active_nic->get_vendor_id) ? s_active_nic->get_vendor_id() : 0u;
}

uint16_t rtl8139_get_device_id(void) {
    return (s_active_nic && s_active_nic->get_device_id) ? s_active_nic->get_device_id() : 0u;
}

uint16_t rtl8139_get_subsys_vendor_id(void) {
    return (s_active_nic && s_active_nic->get_subsys_vendor_id) ? s_active_nic->get_subsys_vendor_id() : 0u;
}

uint16_t rtl8139_get_subsys_id(void) {
    return (s_active_nic && s_active_nic->get_subsys_id) ? s_active_nic->get_subsys_id() : 0u;
}

uint8_t rtl8139_get_revision_id(void) {
    return (s_active_nic && s_active_nic->get_revision_id) ? s_active_nic->get_revision_id() : 0u;
}

uint8_t rtl8139_get_bus(void) {
    return (s_active_nic && s_active_nic->get_bus) ? s_active_nic->get_bus() : 0u;
}

uint8_t rtl8139_get_slot(void) {
    return (s_active_nic && s_active_nic->get_slot) ? s_active_nic->get_slot() : 0u;
}

uint8_t rtl8139_get_func(void) {
    return (s_active_nic && s_active_nic->get_func) ? s_active_nic->get_func() : 0u;
}

int rtl8139_send_packet(const void *data, uint16_t len) {
    return (s_active_nic && s_active_nic->send_packet) ? s_active_nic->send_packet(data, len) : -1;
}

int rtl8139_receive_packet(void *buf, uint32_t max_len) {
    return (s_active_nic && s_active_nic->receive_packet) ? s_active_nic->receive_packet(buf, max_len) : 0;
}

int rtl8139_peek_packet_size(void) {
    return (s_active_nic && s_active_nic->peek_packet_size) ? s_active_nic->peek_packet_size() : 0;
}

size_t rtl8139_get_hw_rx_ring_size(void) {
    if (s_active_nic && s_active_nic->get_hw_rx_ring_size) {
        size_t v = s_active_nic->get_hw_rx_ring_size();
        if (v > 0u) return v;
    }
    return 8192u;
}

size_t rtl8139_get_hw_tx_descriptor_count(void) {
    if (s_active_nic && s_active_nic->get_hw_tx_descriptor_count) {
        size_t v = s_active_nic->get_hw_tx_descriptor_count();
        if (v > 0u) return v;
    }
    return 4u;
}

size_t rtl8139_get_hw_max_dma_burst(void) {
    if (s_active_nic && s_active_nic->get_hw_max_dma_burst) {
        size_t v = s_active_nic->get_hw_max_dma_burst();
        if (v > 0u) return v;
    }
    return 2048u;
}

size_t rtl8139_get_hw_socket_capacity(void) {
    size_t rx_ring = rtl8139_get_hw_rx_ring_size();
    size_t dma_burst = rtl8139_get_hw_max_dma_burst();
    size_t tx_descs = rtl8139_get_hw_tx_descriptor_count();
    size_t slots = (dma_burst > 0u) ? (rx_ring / dma_burst) : tx_descs;
    if (slots < tx_descs) {
        slots = tx_descs;
    }
    return slots * tx_descs;
}

uint32_t rtl8139_get_hw_max_window(void) {
    size_t rx_ring = rtl8139_get_hw_rx_ring_size();
    size_t tx_descs = rtl8139_get_hw_tx_descriptor_count();
    size_t hw_wnd = rx_ring * tx_descs * sizeof(uint16_t);
    uint32_t wire_max = (uint32_t)((1ULL << (sizeof(uint16_t) * 8u)) - 1ULL);
    if (hw_wnd == 0u || hw_wnd > (size_t)wire_max) {
        return wire_max;
    }
    return (uint32_t)hw_wnd;
}

uint16_t rtl8139_get_hw_mss(void) {
    size_t mtu = 1500u;
    if (!s_active_nic || s_active_nic == &g_rtl8139_nic_ops) {
        size_t rx_buf = rtl8139_get_rx_buffer_size();
        size_t rx_ring = rtl8139_get_hw_rx_ring_size();
        size_t hdr_margin = sizeof(uint32_t) * 4u;
        size_t hw_mtu = (rx_buf > (rx_ring + hdr_margin))
                            ? (rx_buf - rx_ring - hdr_margin)
                            : rtl8139_get_tx_buffer_size();
        if (hw_mtu > 0u && hw_mtu < mtu) {
            mtu = hw_mtu;
        }
    }
    size_t proto_hdrs = 20u + 20u;
    return (uint16_t)((mtu > proto_hdrs) ? (mtu - proto_hdrs) : mtu);
}
