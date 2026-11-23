#ifndef DRIVERS_VIRTIO_INPUT_H
#define DRIVERS_VIRTIO_INPUT_H

#include "kmd_api.h"

/* VirtIO 1.0 Modern PCI + Split Virtqueue helper for VirtIO Input (0x1AF4:0x1052). */

#define VIRTIO_PCI_VENDOR_ID       0x1AF4u
#define VIRTIO_PCI_DEV_INPUT       0x1052u

#define VIRTIO_PCI_CAP_COMMON_CFG  1u
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2u
#define VIRTIO_PCI_CAP_ISR_CFG     3u
#define VIRTIO_PCI_CAP_DEVICE_CFG  4u

#define VIRTIO_STATUS_ACK          0x01u
#define VIRTIO_STATUS_DRIVER       0x02u
#define VIRTIO_STATUS_DRIVER_OK    0x04u
#define VIRTIO_STATUS_FEATURES_OK  0x08u

#define VIRTIO_INPUT_CFG_UNSET     0x00u
#define VIRTIO_INPUT_CFG_ID_NAME   0x01u
#define VIRTIO_INPUT_CFG_ID_SERIAL 0x02u
#define VIRTIO_INPUT_CFG_EV_BITS   0x11u

#define VRING_DESC_F_WRITE         2u
#define VRING_AVAIL_F_NO_INT       1u

typedef struct {
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
} __attribute__((packed)) vio_common_cfg_t;

#define VIO_CFG_OFF_SELECT         0u
#define VIO_CFG_OFF_SUBSEL         1u
#define VIO_CFG_OFF_SIZE           2u
#define VIO_CFG_OFF_DATA           8u

typedef struct {
    uint16_t type;
    uint16_t code;
    uint32_t value;
} __attribute__((packed)) vio_input_event_t;

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) vio_vring_desc_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed)) vio_vring_avail_t;

typedef struct {
    uint32_t id;
    uint32_t len;
} __attribute__((packed)) vio_vring_used_elem_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    vio_vring_used_elem_t ring[];
} __attribute__((packed)) vio_vring_used_t;

typedef struct {
    bool active;
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    uint8_t irq;
    volatile vio_common_cfg_t *common;
    uint32_t common_size;
    volatile uint8_t *notify_base;
    uint32_t notify_size;
    uint32_t notify_mult;
    volatile uint8_t *isr;
    uint32_t isr_size;
    volatile uint8_t *dev_cfg;
    uint32_t dev_cfg_size;

    uint16_t qsz;
    uint16_t notify_off;
    uint16_t last_used;

    void *ring_virt;
    uint64_t ring_phys;
    vio_vring_desc_t  *desc;
    vio_vring_avail_t *avail;
    vio_vring_used_t  *used;

    vio_input_event_t *buf_virt;
    uint64_t buf_phys;

    char *name;
    char *serial;
} vio_input_dev_t;

typedef void (*vio_event_cb_t)(uint16_t type, uint16_t code, int32_t value, void *ctx);

static inline void vio_mb(void) {
    __asm__ volatile("" ::: "memory");
}

static inline void vio_read_cfg_str(volatile uint8_t *cfg, uint8_t sel, char **out_str) {
    if (!out_str) return;
    if (*out_str) {
        kfree(*out_str);
        *out_str = NULL;
    }
    if (!cfg) return;
    cfg[VIO_CFG_OFF_SELECT] = sel;
    cfg[VIO_CFG_OFF_SUBSEL] = 0;
    vio_mb();
    uint8_t sz = cfg[VIO_CFG_OFF_SIZE];
    char *s = (char *)kmalloc((size_t)sz + 1u);
    if (!s) return;
    for (uint8_t i = 0; i < sz; i++) {
        s[i] = (char)cfg[VIO_CFG_OFF_DATA + i];
    }
    s[sz] = '\0';
    *out_str = s;
}

static inline bool vio_has_ev_bit(volatile uint8_t *cfg, uint8_t ev_type, uint16_t code) {
    if (!cfg) return false;
    cfg[VIO_CFG_OFF_SELECT] = VIRTIO_INPUT_CFG_EV_BITS;
    cfg[VIO_CFG_OFF_SUBSEL] = ev_type;
    vio_mb();
    uint8_t sz = cfg[VIO_CFG_OFF_SIZE];
    uint16_t byte_idx = (uint16_t)(code >> 3);
    if (sz == 0 || byte_idx >= sz) return false;
    uint8_t val = cfg[VIO_CFG_OFF_DATA + byte_idx];
    return (val & (uint8_t)(1u << (code & 7u))) != 0u;
}

static inline bool vio_str_contains_ci(const char *hay, const char *needle) {
    if (!hay || !needle || !*needle) return false;
    for (size_t i = 0; hay[i]; i++) {
        size_t j = 0;
        while (needle[j] && hay[i + j]) {
            char a = hay[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
            if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
            if (a != b) break;
            j++;
        }
        if (!needle[j]) return true;
    }
    return false;
}

static void vio_cleanup_dev(vio_input_dev_t *v) {
    if (!v) return;
    if (v->common) {
        if (v->active) {
            v->common->device_status = 0;
        }
        ipo_hw_unmap_mmio((void *)(uintptr_t)v->common, v->common_size, 0u);
    }
    if (v->notify_base) {
        ipo_hw_unmap_mmio((void *)(uintptr_t)v->notify_base, v->notify_size, 0u);
    }
    if (v->isr) {
        ipo_hw_unmap_mmio((void *)(uintptr_t)v->isr, v->isr_size, 0u);
    }
    if (v->dev_cfg) {
        ipo_hw_unmap_mmio((void *)(uintptr_t)v->dev_cfg, v->dev_cfg_size, 0u);
    }
    if (v->ring_virt) ipo_hw_free_dma(v->ring_virt, 0u);
    if (v->buf_virt)  ipo_hw_free_dma(v->buf_virt, 0u);
    if (v->name)      kfree(v->name);
    if (v->serial)    kfree(v->serial);
    memset(v, 0, sizeof(*v));
}

static bool vio_map_cap_bar(const kmd_pci_device_t *pdev, uint8_t cap_off,
                            void **out_va, uint32_t *out_len) {
    uint8_t bar_idx = pci_read_byte(pdev->bus, pdev->slot, pdev->func, (uint8_t)(cap_off + 4u));
    uint32_t bar_lo = pci_read_dword(pdev->bus, pdev->slot, pdev->func, (uint8_t)(0x10u + bar_idx * 4u));
    if (bar_lo == 0u || (bar_lo & 1u) != 0u) return false; /* Require MMIO BAR */
    uint64_t phys64 = (uint64_t)(bar_lo & ~0xFu);
    if (((bar_lo >> 1) & 3u) == 2u) {
        uint32_t bar_hi = pci_read_dword(pdev->bus, pdev->slot, pdev->func, (uint8_t)(0x14u + bar_idx * 4u));
        phys64 |= ((uint64_t)bar_hi << 32);
    }
    uint32_t cap_offset = pci_read_dword(pdev->bus, pdev->slot, pdev->func, (uint8_t)(cap_off + 8u));
    uint32_t cap_len    = pci_read_dword(pdev->bus, pdev->slot, pdev->func, (uint8_t)(cap_off + 12u));
    if (cap_len == 0u) cap_len = 4096u;
    void *va = ipo_hw_map_mmio(phys64 + (uint64_t)cap_offset, cap_len, IPO_MMIO_CACHE_DISABLE, 0u);
    if (!va) return false;
    *out_va = va;
    if (out_len) *out_len = cap_len;
    return true;
}

typedef bool (*vio_match_fn_t)(const vio_input_dev_t *probe_info);

static bool vio_probe_and_init(vio_input_dev_t *out_dev, vio_match_fn_t match_fn) {
    memset(out_dev, 0, sizeof(*out_dev));
    uint32_t count = pci_get_count();
    for (uint32_t i = 0; i < count; i++) {
        const kmd_pci_device_t *pdev = pci_get_device(i);
        if (!pdev) continue;
        if (pdev->vendor_id != VIRTIO_PCI_VENDOR_ID || pdev->device_id != VIRTIO_PCI_DEV_INPUT) {
            continue;
        }

        uint16_t status = pci_read_word(pdev->bus, pdev->slot, pdev->func, 0x06u);
        if ((status & 0x0010u) == 0u) continue;

        /* Ensure Memory Space (bit 1), Bus Master (bit 2), and Interrupt Disable (bit 10) for polling */
        pci_enable_bus_mastering(pdev);
        uint16_t cmd = pci_read_word(pdev->bus, pdev->slot, pdev->func, 0x04u);
        if ((cmd & 0x0400u) == 0u) {
            pci_write_word(pdev->bus, pdev->slot, pdev->func, 0x04u, (uint16_t)(cmd | 0x0400u));
        }

        vio_input_dev_t tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.bus = pdev->bus;
        tmp.slot = pdev->slot;
        tmp.func = pdev->func;
        tmp.irq = pdev->irq;

        uint8_t cap_off = (uint8_t)(pci_read_byte(pdev->bus, pdev->slot, pdev->func, 0x34u) & 0xFCu);
        uint8_t *visited = NULL;
        size_t visited_cnt = 0u, visited_cap = 0u;
        while (cap_off != 0u) {
            bool seen = false;
            for (size_t vi = 0u; vi < visited_cnt; vi++) {
                if (visited[vi] == cap_off) { seen = true; break; }
            }
            if (seen) break;
            if (visited_cnt >= visited_cap) {
                size_t nc = visited_cap ? visited_cap * 2u : 16u;
                uint8_t *nv = (uint8_t *)kmalloc(nc);
                if (!nv) break;
                if (visited) {
                    memcpy(nv, visited, visited_cnt);
                    kfree(visited);
                }
                visited = nv;
                visited_cap = nc;
            }
            visited[visited_cnt++] = cap_off;
            uint8_t cap_id   = pci_read_byte(pdev->bus, pdev->slot, pdev->func, cap_off);
            uint8_t cap_len  = pci_read_byte(pdev->bus, pdev->slot, pdev->func, (uint8_t)(cap_off + 2u));
            uint8_t cap_next = (uint8_t)(pci_read_byte(pdev->bus, pdev->slot, pdev->func, (uint8_t)(cap_off + 1u)) & 0xFCu);
            if (cap_id == 0x09u && cap_len >= 16u) {
                uint8_t cfg_type = pci_read_byte(pdev->bus, pdev->slot, pdev->func, (uint8_t)(cap_off + 3u));
                void *va = NULL;
                uint32_t len = 0;
                if (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG && !tmp.common) {
                    if (vio_map_cap_bar(pdev, cap_off, &va, &len)) {
                        tmp.common = (volatile vio_common_cfg_t *)va;
                        tmp.common_size = len;
                    }
                } else if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG && !tmp.notify_base && cap_len >= 20u) {
                    if (vio_map_cap_bar(pdev, cap_off, &va, &len)) {
                        tmp.notify_base = (volatile uint8_t *)va;
                        tmp.notify_size = len;
                        tmp.notify_mult = pci_read_dword(pdev->bus, pdev->slot, pdev->func, (uint8_t)(cap_off + 16u));
                    }
                } else if (cfg_type == VIRTIO_PCI_CAP_ISR_CFG && !tmp.isr) {
                    if (vio_map_cap_bar(pdev, cap_off, &va, &len)) {
                        tmp.isr = (volatile uint8_t *)va;
                        tmp.isr_size = len;
                    }
                } else if (cfg_type == VIRTIO_PCI_CAP_DEVICE_CFG && !tmp.dev_cfg) {
                    if (vio_map_cap_bar(pdev, cap_off, &va, &len)) {
                        tmp.dev_cfg = (volatile uint8_t *)va;
                        tmp.dev_cfg_size = len;
                    }
                }
            }
            cap_off = cap_next;
        }
        if (visited) kfree(visited);

        if (!tmp.common || !tmp.notify_base || !tmp.dev_cfg) {
            vio_cleanup_dev(&tmp);
            continue;
        }

        vio_read_cfg_str(tmp.dev_cfg, VIRTIO_INPUT_CFG_ID_NAME, &tmp.name);
        vio_read_cfg_str(tmp.dev_cfg, VIRTIO_INPUT_CFG_ID_SERIAL, &tmp.serial);

        if (match_fn && !match_fn(&tmp)) {
            vio_cleanup_dev(&tmp);
            continue;
        }

        /* Full VirtIO 1.0 reset & feature negotiation */
        tmp.common->device_status = 0;
        vio_mb();
        tmp.common->device_status = VIRTIO_STATUS_ACK;
        vio_mb();
        tmp.common->device_status = VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER;
        vio_mb();

        tmp.common->device_feature_select = 1u;
        vio_mb();
        uint32_t feat_hi = tmp.common->device_feature;
        tmp.common->driver_feature_select = 0u;
        tmp.common->driver_feature = 0u;
        tmp.common->driver_feature_select = 1u;
        tmp.common->driver_feature = feat_hi & 1u; /* VIRTIO_F_VERSION_1 only */
        vio_mb();

        tmp.common->device_status = VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK;
        vio_mb();
        if ((tmp.common->device_status & VIRTIO_STATUS_FEATURES_OK) == 0u) {
            vio_cleanup_dev(&tmp);
            continue;
        }

        /* Configure eventq (queue 0) */
        tmp.common->queue_select = 0u;
        vio_mb();
        uint16_t qsz = tmp.common->queue_size;
        if (qsz == 0u) {
            vio_cleanup_dev(&tmp);
            continue;
        }
        tmp.qsz = qsz;
        tmp.notify_off = tmp.common->queue_notify_off;

        uint32_t desc_bytes = (uint32_t)qsz * (uint32_t)sizeof(vio_vring_desc_t);
        uint32_t avail_bytes = 6u + (uint32_t)qsz * 2u;
        uint32_t avail_off = desc_bytes;
        uint32_t used_off = (avail_off + avail_bytes + 4095u) & ~4095u;
        uint32_t used_bytes = 8u + (uint32_t)qsz * (uint32_t)sizeof(vio_vring_used_elem_t);
        uint32_t ring_total = (used_off + used_bytes + 4095u) & ~4095u;
        uint32_t ev_bytes = (uint32_t)qsz * (uint32_t)sizeof(vio_input_event_t);
        uint32_t buf_total = (ev_bytes + 4095u) & ~4095u;

        tmp.ring_virt = ipo_hw_alloc_dma(ring_total, 4096u, &tmp.ring_phys, 0u);
        tmp.buf_virt  = (vio_input_event_t *)ipo_hw_alloc_dma(buf_total, 4096u, &tmp.buf_phys, 0u);
        if (!tmp.ring_virt || !tmp.buf_virt) {
            vio_cleanup_dev(&tmp);
            continue;
        }

        uint8_t *rv = (uint8_t *)tmp.ring_virt;
        tmp.desc  = (vio_vring_desc_t *)(rv + 0u);
        tmp.avail = (vio_vring_avail_t *)(rv + avail_off);
        tmp.used  = (vio_vring_used_t *)(rv + used_off);

        tmp.avail->flags = VRING_AVAIL_F_NO_INT;
        for (uint16_t idx = 0; idx < qsz; idx++) {
            uint32_t off = (uint32_t)idx * (uint32_t)sizeof(vio_input_event_t);
            tmp.desc[idx].addr  = (uint64_t)(tmp.buf_phys + off);
            tmp.desc[idx].len   = (uint32_t)sizeof(vio_input_event_t);
            tmp.desc[idx].flags = VRING_DESC_F_WRITE;
            tmp.desc[idx].next  = 0u;
            tmp.avail->ring[idx] = idx;
        }
        vio_mb();
        tmp.avail->idx = qsz;

        tmp.common->queue_msix_vector = 0xFFFFu;
        tmp.common->queue_desc_lo     = (uint32_t)(tmp.ring_phys + 0u);
        tmp.common->queue_desc_hi     = (uint32_t)((tmp.ring_phys + 0u) >> 32);
        tmp.common->queue_driver_lo   = (uint32_t)(tmp.ring_phys + avail_off);
        tmp.common->queue_driver_hi   = (uint32_t)((tmp.ring_phys + avail_off) >> 32);
        tmp.common->queue_device_lo   = (uint32_t)(tmp.ring_phys + used_off);
        tmp.common->queue_device_hi   = (uint32_t)((tmp.ring_phys + used_off) >> 32);
        vio_mb();
        tmp.common->queue_enable = 1u;
        vio_mb();

        tmp.common->device_status = VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                                    VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK;
        vio_mb();

        volatile uint16_t *notify = (volatile uint16_t *)(tmp.notify_base +
                                   (uint32_t)tmp.notify_off * tmp.notify_mult);
        *notify = 0u;

        tmp.active = true;
        *out_dev = tmp;
        return true;
    }
    return false;
}

static inline uint32_t vio_poll_events(vio_input_dev_t *v, vio_event_cb_t cb, void *ctx) {
    if (!v || !v->active || !v->used) return 0u;
    if (v->isr) (void)*v->isr;
    vio_mb();

    uint32_t handled = 0u;
    uint16_t u_idx = v->used->idx;
    while (v->last_used != u_idx) {
        uint16_t slot = (uint16_t)(v->last_used % v->qsz);
        uint32_t did = v->used->ring[slot].id;
        if (did < v->qsz) {
            vio_input_event_t ev = v->buf_virt[did];
            if (cb) cb(ev.type, ev.code, (int32_t)ev.value, ctx);

            uint16_t a_slot = (uint16_t)(v->avail->idx % v->qsz);
            v->avail->ring[a_slot] = (uint16_t)did;
            vio_mb();
            v->avail->idx++;
            handled++;
        }
        v->last_used++;
    }
    if (handled > 0u) {
        volatile uint16_t *notify = (volatile uint16_t *)(v->notify_base +
                                   (uint32_t)v->notify_off * v->notify_mult);
        *notify = 0u;
    }
    return handled;
}

#endif /* DRIVERS_VIRTIO_INPUT_H */
