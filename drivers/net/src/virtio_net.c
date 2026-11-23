#include <driver/net/nic_ops.h>
#include <driver/net/pci.h>
#include <system/timer.h>
#include <ioport.h>
#include <string.h>
#include <stdio.h>

extern void *ipo_hw_map_mmio(uint64_t phys_addr, uint32_t size, uint32_t flags, uint32_t owner_pid);
extern int ipo_hw_unmap_mmio(void *virt_addr, uint32_t size, uint32_t caller_pid);
extern void *ipo_hw_alloc_dma(uint32_t size, uint32_t alignment, uint64_t *out_phys, uint32_t owner_pid);
extern int ipo_hw_free_dma(void *virt_addr, uint32_t caller_pid);
extern int serial_printf(const char *fmt, ...);

#define VNET_VENDOR_ID      0x1AF4u
#define VNET_DEV_LEGACY     0x1000u
#define VNET_DEV_MODERN     0x1041u

#define VNET_BUF_STRIDE     2048u
#define VNET_QUEUE_MAX      64u

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} vnet_desc_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} vnet_avail_t;

typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t len;
} vnet_used_elem_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    vnet_used_elem_t ring[];
} vnet_used_t;

typedef struct __attribute__((packed)) {
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t  device_status;
    uint8_t  config_generation;
    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint32_t queue_desc_lo;
    uint32_t queue_desc_hi;
    uint32_t queue_driver_lo;
    uint32_t queue_driver_hi;
    uint32_t queue_device_lo;
    uint32_t queue_device_hi;
} vnet_common_t;

typedef struct {
    void *ring_va;
    uint64_t ring_phys;
    uint16_t qsz;
    uint16_t last_used;
    vnet_desc_t *desc;
    vnet_avail_t *avail;
    vnet_used_t *used;
    volatile uint16_t *notify_ptr;
    uint8_t *bufs;
    uint64_t bufs_phys;
} vnet_queue_t;

typedef struct {
    bool initialized;
    bool is_modern;
    bool has_status_feat;
    uint32_t hdr_size;
    pci_device_t pdev;
    uint16_t io_base;
    volatile vnet_common_t *common;
    uint32_t common_sz;
    volatile uint8_t *notify_base;
    uint32_t notify_sz;
    uint32_t notify_mult;
    volatile uint8_t *dev_cfg;
    uint32_t dev_cfg_sz;
    uint8_t mac[6];
    vnet_queue_t rxq;
    vnet_queue_t txq;
    uint16_t tx_next_slot;
} vnet_state_t;

static vnet_state_t s_vnet = {0};

static void *vnet_map_cap(const pci_device_t *pdev, uint8_t cap_off, uint32_t *out_len) {
    uint8_t bar_idx = pci_read_byte(pdev->bus, pdev->slot, pdev->func, (uint8_t)(cap_off + 4u));
    if (bar_idx >= 6u || pci_bar_is_io(pdev, bar_idx)) return NULL;
    uint64_t base = pci_get_bar_addr(pdev, bar_idx);
    if (base == 0ULL) return NULL;
    uint32_t off = pci_read_dword(pdev->bus, pdev->slot, pdev->func, (uint8_t)(cap_off + 8u));
    uint32_t len = pci_read_dword(pdev->bus, pdev->slot, pdev->func, (uint8_t)(cap_off + 12u));
    if (len == 0u) len = 4096u;
    if (out_len) *out_len = len;
    return ipo_hw_map_mmio(base + (uint64_t)off, len, 1u, 0u);
}

static bool vnet_setup_queue(uint16_t qidx, vnet_queue_t *q, bool is_rx) {
    memset(q, 0, sizeof(*q));
    uint16_t qsz = 0u;
    if (s_vnet.is_modern) {
        s_vnet.common->queue_select = qidx;
        __sync_synchronize();
        qsz = s_vnet.common->queue_size;
        if (qsz == 0u) return false;
        if (qsz > VNET_QUEUE_MAX) {
            qsz = VNET_QUEUE_MAX;
            s_vnet.common->queue_size = qsz;
        }
        q->notify_ptr = (volatile uint16_t *)(s_vnet.notify_base +
                            (uint32_t)s_vnet.common->queue_notify_off * s_vnet.notify_mult);
    } else {
        outw((uint16_t)(s_vnet.io_base + 0x0Eu), qidx);
        qsz = inw((uint16_t)(s_vnet.io_base + 0x0Cu));
        if (qsz == 0u) return false;
        if (qsz > VNET_QUEUE_MAX) {
            /* Legacy ring must keep hardware qsz size */
        }
    }

    uint32_t desc_bytes = (uint32_t)qsz * (uint32_t)sizeof(vnet_desc_t);
    uint32_t avail_bytes = 6u + (uint32_t)qsz * 2u;
    uint32_t avail_off = desc_bytes;
    uint32_t used_off = (avail_off + avail_bytes + 4095u) & ~4095u;
    uint32_t used_bytes = 6u + (uint32_t)qsz * (uint32_t)sizeof(vnet_used_elem_t);
    uint32_t ring_total = (used_off + used_bytes + 4095u) & ~4095u;
    uint32_t bufs_total = (uint32_t)qsz * VNET_BUF_STRIDE;

    q->ring_va = ipo_hw_alloc_dma(ring_total, 4096u, &q->ring_phys, 0u);
    q->bufs    = (uint8_t *)ipo_hw_alloc_dma(bufs_total, 4096u, &q->bufs_phys, 0u);
    if (!q->ring_va || !q->bufs) return false;

    q->qsz   = qsz;
    q->desc  = (vnet_desc_t *)q->ring_va;
    q->avail = (vnet_avail_t *)((uint8_t *)q->ring_va + avail_off);
    q->used  = (vnet_used_t *)((uint8_t *)q->ring_va + used_off);
    q->avail->flags = 1u; /* NO_INT */

    if (is_rx) {
        for (uint16_t i = 0u; i < qsz; i++) {
            q->desc[i].addr  = q->bufs_phys + (uint64_t)i * VNET_BUF_STRIDE;
            q->desc[i].len   = VNET_BUF_STRIDE;
            q->desc[i].flags = 2u; /* WRITE */
            q->desc[i].next  = 0u;
            q->avail->ring[i] = i;
        }
        __sync_synchronize();
        q->avail->idx = qsz;
    }

    if (s_vnet.is_modern) {
        s_vnet.common->queue_msix_vector = 0xFFFFu;
        s_vnet.common->queue_desc_lo   = (uint32_t)q->ring_phys;
        s_vnet.common->queue_desc_hi   = (uint32_t)(q->ring_phys >> 32);
        s_vnet.common->queue_driver_lo = (uint32_t)(q->ring_phys + avail_off);
        s_vnet.common->queue_driver_hi = (uint32_t)((q->ring_phys + avail_off) >> 32);
        s_vnet.common->queue_device_lo = (uint32_t)(q->ring_phys + used_off);
        s_vnet.common->queue_device_hi = (uint32_t)((q->ring_phys + used_off) >> 32);
        __sync_synchronize();
        s_vnet.common->queue_enable = 1u;
    } else {
        outl((uint16_t)(s_vnet.io_base + 0x08u), (uint32_t)(q->ring_phys >> 12));
    }
    return true;
}

static void virtio_net_hw_shutdown(void);

static bool virtio_net_hw_init(void) {
    memset(&s_vnet, 0, sizeof(s_vnet));

    uint32_t pcnt = pci_get_count();
    const pci_device_t *found = NULL;
    for (uint32_t i = 0u; i < pcnt; i++) {
        const pci_device_t *cand = pci_get_device(i);
        if (!cand || cand->vendor_id != VNET_VENDOR_ID) continue;
        if (cand->device_id == VNET_DEV_LEGACY || cand->device_id == VNET_DEV_MODERN) {
            found = cand;
            break;
        }
    }
    if (!found) return false;

    pci_enable_bus_mastering(found);
    uint16_t pci_cmd = pci_read_word(found->bus, found->slot, found->func, 0x04u);
    if ((pci_cmd & 0x0400u) == 0u) {
        pci_write_word(found->bus, found->slot, found->func, 0x04u, (uint16_t)(pci_cmd | 0x0400u));
    }
    s_vnet.pdev = *found;

    uint16_t pci_st = pci_read_word(found->bus, found->slot, found->func, 0x06u);
    if (pci_st & 0x0010u) {
        uint8_t cap_off = (uint8_t)(pci_read_byte(found->bus, found->slot, found->func, 0x34u) & 0xFCu);
        for (int step = 0; step < 48 && cap_off != 0u; step++) {
            uint8_t id   = pci_read_byte(found->bus, found->slot, found->func, cap_off);
            uint8_t next = (uint8_t)(pci_read_byte(found->bus, found->slot, found->func, (uint8_t)(cap_off + 1u)) & 0xFCu);
            uint8_t clen = pci_read_byte(found->bus, found->slot, found->func, (uint8_t)(cap_off + 2u));
            if (id == 0x09u && clen >= 16u) {
                uint8_t ctype = pci_read_byte(found->bus, found->slot, found->func, (uint8_t)(cap_off + 3u));
                if (ctype == 1u && !s_vnet.common) {
                    s_vnet.common = (volatile vnet_common_t *)vnet_map_cap(found, cap_off, &s_vnet.common_sz);
                } else if (ctype == 2u && !s_vnet.notify_base && clen >= 20u) {
                    s_vnet.notify_base = (volatile uint8_t *)vnet_map_cap(found, cap_off, &s_vnet.notify_sz);
                    s_vnet.notify_mult = pci_read_dword(found->bus, found->slot, found->func, (uint8_t)(cap_off + 16u));
                } else if (ctype == 4u && !s_vnet.dev_cfg) {
                    s_vnet.dev_cfg = (volatile uint8_t *)vnet_map_cap(found, cap_off, &s_vnet.dev_cfg_sz);
                }
            }
            cap_off = next;
        }
    }

    s_vnet.is_modern = (s_vnet.common != NULL && s_vnet.notify_base != NULL && s_vnet.dev_cfg != NULL);
    if (s_vnet.is_modern) {
        s_vnet.hdr_size = 12u;
        s_vnet.common->device_status = 0u;
        __sync_synchronize();
        s_vnet.common->device_status = 1u | 2u;
        __sync_synchronize();

        s_vnet.common->device_feature_select = 0u;
        uint32_t f_lo = s_vnet.common->device_feature;
        uint32_t accept_lo = f_lo & ((1u << 5) | (1u << 16)); /* MAC | STATUS */
        s_vnet.has_status_feat = (accept_lo & (1u << 16)) != 0u;
        s_vnet.common->driver_feature_select = 0u;
        s_vnet.common->driver_feature = accept_lo;

        s_vnet.common->device_feature_select = 1u;
        uint32_t f_hi = s_vnet.common->device_feature;
        s_vnet.common->driver_feature_select = 1u;
        s_vnet.common->driver_feature = f_hi & 1u; /* VERSION_1 */
        __sync_synchronize();
        s_vnet.common->device_status = 1u | 2u | 8u;
        __sync_synchronize();
        if ((s_vnet.common->device_status & 8u) == 0u) {
            virtio_net_hw_shutdown();
            return false;
        }
        if (accept_lo & (1u << 5)) {
            for (int m = 0; m < 6; m++) s_vnet.mac[m] = s_vnet.dev_cfg[m];
        } else {
            s_vnet.mac[0] = 0x52u; s_vnet.mac[1] = 0x54u; s_vnet.mac[2] = 0x00u;
            s_vnet.mac[3] = 0x12u; s_vnet.mac[4] = 0x34u; s_vnet.mac[5] = 0x56u;
        }
    } else {
        if (!pci_bar_is_io(found, 0u)) {
            virtio_net_hw_shutdown();
            return false;
        }
        s_vnet.io_base = (uint16_t)pci_get_bar_addr(found, 0u);
        if (s_vnet.io_base == 0u) return false;
        s_vnet.hdr_size = 10u;

        outb((uint16_t)(s_vnet.io_base + 0x12u), 0u);
        outb((uint16_t)(s_vnet.io_base + 0x12u), 1u | 2u);
        uint32_t f = inl(s_vnet.io_base + 0x00u);
        uint32_t accept = f & ((1u << 5) | (1u << 16));
        s_vnet.has_status_feat = (accept & (1u << 16)) != 0u;
        outl(s_vnet.io_base + 0x04u, accept);

        if (accept & (1u << 5)) {
            for (int m = 0; m < 6; m++) {
                s_vnet.mac[m] = inb((uint16_t)(s_vnet.io_base + 0x14u + m));
            }
        } else {
            s_vnet.mac[0] = 0x52u; s_vnet.mac[1] = 0x54u; s_vnet.mac[2] = 0x00u;
            s_vnet.mac[3] = 0x12u; s_vnet.mac[4] = 0x34u; s_vnet.mac[5] = 0x56u;
        }
    }

    if (!vnet_setup_queue(0u, &s_vnet.rxq, true) ||
        !vnet_setup_queue(1u, &s_vnet.txq, false)) {
        virtio_net_hw_shutdown();
        return false;
    }

    if (s_vnet.is_modern) {
        s_vnet.common->device_status = 1u | 2u | 8u | 4u;
        __sync_synchronize();
        *s_vnet.rxq.notify_ptr = 0u;
    } else {
        outb((uint16_t)(s_vnet.io_base + 0x12u), 1u | 2u | 4u);
        __sync_synchronize();
        outw((uint16_t)(s_vnet.io_base + 0x10u), 0u);
    }

    s_vnet.initialized = true;
    serial_printf("[virtio_net] Initialized VirtIO-Net (%s, %04x:%04x)\n",
                  s_vnet.is_modern ? "modern" : "legacy", found->vendor_id, found->device_id);
    return true;
}

static void virtio_net_hw_shutdown(void) {
    if (s_vnet.is_modern && s_vnet.common) {
        s_vnet.common->device_status = 0u;
    } else if (s_vnet.io_base != 0u) {
        outb((uint16_t)(s_vnet.io_base + 0x12u), 0u);
    }
    if (s_vnet.common)      ipo_hw_unmap_mmio((void *)(uintptr_t)s_vnet.common, s_vnet.common_sz, 0u);
    if (s_vnet.notify_base) ipo_hw_unmap_mmio((void *)(uintptr_t)s_vnet.notify_base, s_vnet.notify_sz, 0u);
    if (s_vnet.dev_cfg)     ipo_hw_unmap_mmio((void *)(uintptr_t)s_vnet.dev_cfg, s_vnet.dev_cfg_sz, 0u);
    if (s_vnet.rxq.ring_va) ipo_hw_free_dma(s_vnet.rxq.ring_va, 0u);
    if (s_vnet.rxq.bufs)    ipo_hw_free_dma(s_vnet.rxq.bufs, 0u);
    if (s_vnet.txq.ring_va) ipo_hw_free_dma(s_vnet.txq.ring_va, 0u);
    if (s_vnet.txq.bufs)    ipo_hw_free_dma(s_vnet.txq.bufs, 0u);
    memset(&s_vnet, 0, sizeof(s_vnet));
}

static bool virtio_net_hw_is_present(void) { return s_vnet.initialized; }

static bool virtio_net_hw_is_link_up(void) {
    if (!s_vnet.initialized) return false;
    if (!s_vnet.has_status_feat) return true;
    if (s_vnet.is_modern && s_vnet.dev_cfg) {
        uint16_t st = *(volatile uint16_t *)(s_vnet.dev_cfg + 6u);
        return (st & 1u) != 0u;
    }
    if (!s_vnet.is_modern && s_vnet.io_base != 0u) {
        return (inw((uint16_t)(s_vnet.io_base + 0x1Au)) & 1u) != 0u;
    }
    return true;
}

static const uint8_t *virtio_net_hw_get_mac(size_t *out_len) {
    if (out_len) *out_len = s_vnet.initialized ? 6u : 0u;
    return s_vnet.initialized ? s_vnet.mac : NULL;
}

static size_t virtio_net_hw_get_mac_len(void) { return s_vnet.initialized ? 6u : 0u; }
static uint16_t virtio_net_hw_get_vendor_id(void) { return s_vnet.pdev.vendor_id; }
static uint16_t virtio_net_hw_get_device_id(void) { return s_vnet.pdev.device_id; }
static uint16_t virtio_net_hw_get_subsys_vendor_id(void) { return s_vnet.pdev.subsys_vendor_id; }
static uint16_t virtio_net_hw_get_subsys_id(void) { return s_vnet.pdev.subsys_id; }
static uint8_t virtio_net_hw_get_revision_id(void) { return s_vnet.pdev.revision_id; }
static uint8_t virtio_net_hw_get_bus(void) { return s_vnet.pdev.bus; }
static uint8_t virtio_net_hw_get_slot(void) { return s_vnet.pdev.slot; }
static uint8_t virtio_net_hw_get_func(void) { return s_vnet.pdev.func; }

static int virtio_net_hw_send_packet(const void *data, uint16_t len) {
    if (!s_vnet.initialized || !data || len == 0u) return -1;
    if ((uint32_t)len + s_vnet.hdr_size > VNET_BUF_STRIDE) return -1;

    uint16_t slot = s_vnet.tx_next_slot % s_vnet.txq.qsz;
    s_vnet.tx_next_slot++;

    uint8_t *buf = s_vnet.txq.bufs + (uint32_t)slot * VNET_BUF_STRIDE;
    memset(buf, 0, s_vnet.hdr_size);
    memcpy(buf + s_vnet.hdr_size, data, len);

    s_vnet.txq.desc[slot].addr  = s_vnet.txq.bufs_phys + (uint64_t)slot * VNET_BUF_STRIDE;
    s_vnet.txq.desc[slot].len   = s_vnet.hdr_size + (uint32_t)len;
    s_vnet.txq.desc[slot].flags = 0u;
    s_vnet.txq.desc[slot].next  = 0u;

    uint16_t aidx = s_vnet.txq.avail->idx;
    s_vnet.txq.avail->ring[aidx % s_vnet.txq.qsz] = slot;
    __sync_synchronize();
    s_vnet.txq.avail->idx = (uint16_t)(aidx + 1u);
    __sync_synchronize();

    if (s_vnet.is_modern) {
        *s_vnet.txq.notify_ptr = 1u;
    } else {
        outw((uint16_t)(s_vnet.io_base + 0x10u), 1u);
    }

    uint32_t start = timer_millis();
    while (s_vnet.txq.used->idx == s_vnet.txq.last_used) {
        if (timer_millis() - start > 100u) break;
        __sync_synchronize();
    }
    s_vnet.txq.last_used = s_vnet.txq.used->idx;
    return (int)len;
}

static int virtio_net_hw_peek_packet_size(void) {
    if (!s_vnet.initialized) return 0;
    __sync_synchronize();
    if (s_vnet.rxq.last_used == s_vnet.rxq.used->idx) return 0;
    uint16_t uslot = (uint16_t)(s_vnet.rxq.last_used % s_vnet.rxq.qsz);
    uint32_t total = s_vnet.rxq.used->ring[uslot].len;
    if (total <= s_vnet.hdr_size) return 0;
    return (int)(total - s_vnet.hdr_size);
}

static int virtio_net_hw_receive_packet(void *buf, uint32_t max_len) {
    if (!s_vnet.initialized || !buf || max_len == 0u) return 0;
    for (;;) {
        __sync_synchronize();
        if (s_vnet.rxq.last_used == s_vnet.rxq.used->idx) return 0;

        uint16_t uslot = (uint16_t)(s_vnet.rxq.last_used % s_vnet.rxq.qsz);
        uint32_t id    = s_vnet.rxq.used->ring[uslot].id;
        uint32_t total = s_vnet.rxq.used->ring[uslot].len;
        s_vnet.rxq.last_used++;

        uint32_t copied = 0u;
        if (id < s_vnet.rxq.qsz) {
            if (total > s_vnet.hdr_size) {
                uint32_t plen = total - s_vnet.hdr_size;
                copied = (plen < max_len) ? plen : max_len;
                memcpy(buf, s_vnet.rxq.bufs + id * VNET_BUF_STRIDE + s_vnet.hdr_size, copied);
            }
            uint16_t aidx = s_vnet.rxq.avail->idx;
            s_vnet.rxq.avail->ring[aidx % s_vnet.rxq.qsz] = (uint16_t)id;
            __sync_synchronize();
            s_vnet.rxq.avail->idx = (uint16_t)(aidx + 1u);
            __sync_synchronize();
            if (s_vnet.is_modern) {
                *s_vnet.rxq.notify_ptr = 0u;
            } else {
                outw((uint16_t)(s_vnet.io_base + 0x10u), 0u);
            }
        }
        if (copied > 0u) return (int)copied;
    }
}

static size_t virtio_net_hw_rx_ring_size(void) {
    return (size_t)(s_vnet.rxq.qsz ? s_vnet.rxq.qsz : 16u) * VNET_BUF_STRIDE;
}
static size_t virtio_net_hw_tx_desc_count(void) { return (size_t)(s_vnet.txq.qsz ? s_vnet.txq.qsz : 16u); }
static size_t virtio_net_hw_max_dma_burst(void) { return 2048u; }

const net_nic_ops_t g_virtio_net_nic_ops = {
    .driver_name = "virtio-net",
    .init = virtio_net_hw_init,
    .shutdown = virtio_net_hw_shutdown,
    .is_present = virtio_net_hw_is_present,
    .is_link_up = virtio_net_hw_is_link_up,
    .get_mac = virtio_net_hw_get_mac,
    .get_mac_len = virtio_net_hw_get_mac_len,
    .get_vendor_id = virtio_net_hw_get_vendor_id,
    .get_device_id = virtio_net_hw_get_device_id,
    .get_subsys_vendor_id = virtio_net_hw_get_subsys_vendor_id,
    .get_subsys_id = virtio_net_hw_get_subsys_id,
    .get_revision_id = virtio_net_hw_get_revision_id,
    .get_bus = virtio_net_hw_get_bus,
    .get_slot = virtio_net_hw_get_slot,
    .get_func = virtio_net_hw_get_func,
    .send_packet = virtio_net_hw_send_packet,
    .receive_packet = virtio_net_hw_receive_packet,
    .peek_packet_size = virtio_net_hw_peek_packet_size,
    .get_hw_rx_ring_size = virtio_net_hw_rx_ring_size,
    .get_hw_tx_descriptor_count = virtio_net_hw_tx_desc_count,
    .get_hw_max_dma_burst = virtio_net_hw_max_dma_burst,
};
