#include <driver/net/nic_ops.h>
#include <driver/net/pci.h>
#include <system/timer.h>
#include <memory/kmalloc.h>
#include <string.h>
#include <stdio.h>

extern void *ipo_hw_map_mmio(uint64_t phys_addr, uint32_t size, uint32_t flags, uint32_t owner_pid);
extern int ipo_hw_unmap_mmio(void *virt_addr, uint32_t size, uint32_t caller_pid);
extern void *ipo_hw_alloc_dma(uint32_t size, uint32_t alignment, uint64_t *out_phys, uint32_t owner_pid);
extern int ipo_hw_free_dma(void *virt_addr, uint32_t caller_pid);
extern int serial_printf(const char *fmt, ...);

#define E1000_REG_CTRL      0x0000u
#define E1000_REG_STATUS    0x0008u
#define E1000_REG_EERD      0x0014u
#define E1000_REG_ICR       0x00C0u
#define E1000_REG_IMC       0x00D8u
#define E1000_REG_RCTL      0x0100u
#define E1000_REG_TCTL      0x0400u
#define E1000_REG_TIPG      0x0410u
#define E1000_REG_RDBAL     0x2800u
#define E1000_REG_RDBAH     0x2804u
#define E1000_REG_RDLEN     0x2808u
#define E1000_REG_RDH       0x2810u
#define E1000_REG_RDT       0x2818u
#define E1000_REG_TDBAL     0x3800u
#define E1000_REG_TDBAH     0x3804u
#define E1000_REG_TDLEN     0x3808u
#define E1000_REG_TDH       0x3810u
#define E1000_REG_TDT       0x3818u
#define E1000_REG_MTA       0x5200u
#define E1000_REG_RAL0      0x5400u
#define E1000_REG_RAH0      0x5404u

#define E1000_RX_DESC_CNT   32u
#define E1000_TX_DESC_CNT   32u
#define E1000_BUF_STRIDE    2048u

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} e1000_rx_desc_t;

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} e1000_tx_desc_t;

typedef struct {
    bool initialized;
    volatile uint8_t *mmio;
    uint32_t mmio_size;
    pci_device_t pdev;
    uint8_t mac[6];

    e1000_rx_desc_t *rx_descs;
    uint64_t rx_descs_phys;
    uint8_t *rx_bufs;
    uint64_t rx_bufs_phys;
    uint32_t rx_cur;

    e1000_tx_desc_t *tx_descs;
    uint64_t tx_descs_phys;
    uint8_t *tx_bufs;
    uint64_t tx_bufs_phys;
    uint32_t tx_cur;
} e1000_state_t;

static e1000_state_t s_e1k = {0};

static inline uint32_t e1k_rd(uint32_t reg) {
    return *(volatile uint32_t *)(s_e1k.mmio + reg);
}

static inline void e1k_wr(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(s_e1k.mmio + reg) = val;
}

static bool e1k_read_eeprom(uint8_t addr, uint16_t *out_val) {
    e1k_wr(E1000_REG_EERD, 1u | ((uint32_t)addr << 8));
    for (uint32_t i = 0u; i < 20000u; i++) {
        uint32_t v = e1k_rd(E1000_REG_EERD);
        if (v & (1u << 4)) {
            *out_val = (uint16_t)(v >> 16);
            return true;
        }
    }
    return false;
}

static void e1000_hw_shutdown(void);

static bool e1000_hw_init(void) {
    memset(&s_e1k, 0, sizeof(s_e1k));

    uint32_t pcnt = pci_get_count();
    const pci_device_t *found = NULL;
    for (uint32_t i = 0u; i < pcnt; i++) {
        const pci_device_t *cand = pci_get_device(i);
        if (!cand) continue;
        if (cand->vendor_id == 0x8086u &&
            cand->class_code == PCI_CLASS_NETWORK &&
            cand->subclass == PCI_SUBCLASS_ETHERNET &&
            !pci_bar_is_io(cand, 0u)) {
            found = cand;
            break;
        }
    }
    if (!found) return false;

    uint64_t bar0 = pci_get_bar_addr(found, 0u);
    if (bar0 == 0ULL) return false;

    pci_enable_bus_mastering(found);
    s_e1k.mmio_size = 128u * 1024u;
    s_e1k.mmio = (volatile uint8_t *)ipo_hw_map_mmio(bar0, s_e1k.mmio_size, 1u, 0u);
    if (!s_e1k.mmio) return false;
    s_e1k.pdev = *found;

    /* Reset controller */
    e1k_wr(E1000_REG_IMC, 0xFFFFFFFFu);
    e1k_wr(E1000_REG_CTRL, e1k_rd(E1000_REG_CTRL) | (1u << 26));
    for (uint32_t i = 0u; i < 50000u; i++) {
        if ((e1k_rd(E1000_REG_CTRL) & (1u << 26)) == 0u) break;
    }
    e1k_wr(E1000_REG_IMC, 0xFFFFFFFFu);
    (void)e1k_rd(E1000_REG_ICR);

    /* Link up & auto-speed detection */
    uint32_t ctrl = e1k_rd(E1000_REG_CTRL);
    ctrl |= (1u << 6) | (1u << 5); /* SLU | ASDE */
    ctrl &= ~(1u << 3);            /* clear LRST */
    ctrl &= ~(1u << 31);           /* clear PHY_RST */
    e1k_wr(E1000_REG_CTRL, ctrl);

    /* Read MAC from EEPROM or fallback to RAL0/RAH0 */
    uint16_t w0 = 0, w1 = 0, w2 = 0;
    if (e1k_read_eeprom(0u, &w0) && e1k_read_eeprom(1u, &w1) && e1k_read_eeprom(2u, &w2) &&
        !(w0 == 0xFFFFu && w1 == 0xFFFFu && w2 == 0xFFFFu)) {
        s_e1k.mac[0] = (uint8_t)(w0 & 0xFFu);
        s_e1k.mac[1] = (uint8_t)(w0 >> 8);
        s_e1k.mac[2] = (uint8_t)(w1 & 0xFFu);
        s_e1k.mac[3] = (uint8_t)(w1 >> 8);
        s_e1k.mac[4] = (uint8_t)(w2 & 0xFFu);
        s_e1k.mac[5] = (uint8_t)(w2 >> 8);
    } else {
        uint32_t ral = e1k_rd(E1000_REG_RAL0);
        uint32_t rah = e1k_rd(E1000_REG_RAH0);
        s_e1k.mac[0] = (uint8_t)(ral & 0xFFu);
        s_e1k.mac[1] = (uint8_t)((ral >> 8) & 0xFFu);
        s_e1k.mac[2] = (uint8_t)((ral >> 16) & 0xFFu);
        s_e1k.mac[3] = (uint8_t)((ral >> 24) & 0xFFu);
        s_e1k.mac[4] = (uint8_t)(rah & 0xFFu);
        s_e1k.mac[5] = (uint8_t)((rah >> 8) & 0xFFu);
    }

    uint32_t ral = (uint32_t)s_e1k.mac[0] | ((uint32_t)s_e1k.mac[1] << 8) |
                   ((uint32_t)s_e1k.mac[2] << 16) | ((uint32_t)s_e1k.mac[3] << 24);
    uint32_t rah = (uint32_t)s_e1k.mac[4] | ((uint32_t)s_e1k.mac[5] << 8) | (1u << 31);
    e1k_wr(E1000_REG_RAL0, ral);
    e1k_wr(E1000_REG_RAH0, rah);

    for (uint32_t i = 0u; i < 128u; i++) {
        e1k_wr(E1000_REG_MTA + i * 4u, 0u);
    }

    /* Allocate DMA descriptors & rings */
    s_e1k.rx_descs = (e1000_rx_desc_t *)ipo_hw_alloc_dma(4096u, 4096u, &s_e1k.rx_descs_phys, 0u);
    s_e1k.rx_bufs  = (uint8_t *)ipo_hw_alloc_dma(E1000_RX_DESC_CNT * E1000_BUF_STRIDE, 4096u, &s_e1k.rx_bufs_phys, 0u);
    s_e1k.tx_descs = (e1000_tx_desc_t *)ipo_hw_alloc_dma(4096u, 4096u, &s_e1k.tx_descs_phys, 0u);
    s_e1k.tx_bufs  = (uint8_t *)ipo_hw_alloc_dma(E1000_TX_DESC_CNT * E1000_BUF_STRIDE, 4096u, &s_e1k.tx_bufs_phys, 0u);
    if (!s_e1k.rx_descs || !s_e1k.rx_bufs || !s_e1k.tx_descs || !s_e1k.tx_bufs) {
        e1000_hw_shutdown();
        return false;
    }

    for (uint32_t i = 0u; i < E1000_RX_DESC_CNT; i++) {
        s_e1k.rx_descs[i].addr = s_e1k.rx_bufs_phys + (uint64_t)i * E1000_BUF_STRIDE;
        s_e1k.rx_descs[i].status = 0u;
    }
    for (uint32_t i = 0u; i < E1000_TX_DESC_CNT; i++) {
        s_e1k.tx_descs[i].addr = s_e1k.tx_bufs_phys + (uint64_t)i * E1000_BUF_STRIDE;
        s_e1k.tx_descs[i].status = 1u; /* DD initial ready */
    }

    /* Setup RX ring */
    e1k_wr(E1000_REG_RDBAL, (uint32_t)s_e1k.rx_descs_phys);
    e1k_wr(E1000_REG_RDBAH, (uint32_t)(s_e1k.rx_descs_phys >> 32));
    e1k_wr(E1000_REG_RDLEN, E1000_RX_DESC_CNT * (uint32_t)sizeof(e1000_rx_desc_t));
    e1k_wr(E1000_REG_RDH, 0u);
    e1k_wr(E1000_REG_RDT, E1000_RX_DESC_CNT - 1u);
    e1k_wr(E1000_REG_RCTL, (1u << 1) | (1u << 15) | (1u << 26)); /* EN | BAM | SECRC (2048B) */

    /* Setup TX ring */
    e1k_wr(E1000_REG_TDBAL, (uint32_t)s_e1k.tx_descs_phys);
    e1k_wr(E1000_REG_TDBAH, (uint32_t)(s_e1k.tx_descs_phys >> 32));
    e1k_wr(E1000_REG_TDLEN, E1000_TX_DESC_CNT * (uint32_t)sizeof(e1000_tx_desc_t));
    e1k_wr(E1000_REG_TDH, 0u);
    e1k_wr(E1000_REG_TDT, 0u);
    e1k_wr(E1000_REG_TIPG, 0x0060200Au);
    e1k_wr(E1000_REG_TCTL, (1u << 1) | (1u << 3) | (0x10u << 4) | (0x40u << 12));

    s_e1k.rx_cur = 0u;
    s_e1k.tx_cur = 0u;
    s_e1k.initialized = true;

    serial_printf("[e1000] Initialized Intel NIC (%04x:%04x) at %02x:%02x.%x\n",
                  found->vendor_id, found->device_id, found->bus, found->slot, found->func);
    return true;
}

static void e1000_hw_shutdown(void) {
    if (s_e1k.mmio) {
        e1k_wr(E1000_REG_RCTL, 0u);
        e1k_wr(E1000_REG_TCTL, 0u);
        e1k_wr(E1000_REG_IMC, 0xFFFFFFFFu);
        ipo_hw_unmap_mmio((void *)(uintptr_t)s_e1k.mmio, s_e1k.mmio_size, 0u);
    }
    if (s_e1k.rx_descs) ipo_hw_free_dma(s_e1k.rx_descs, 0u);
    if (s_e1k.rx_bufs)  ipo_hw_free_dma(s_e1k.rx_bufs, 0u);
    if (s_e1k.tx_descs) ipo_hw_free_dma(s_e1k.tx_descs, 0u);
    if (s_e1k.tx_bufs)  ipo_hw_free_dma(s_e1k.tx_bufs, 0u);
    memset(&s_e1k, 0, sizeof(s_e1k));
}

static bool e1000_hw_is_present(void) { return s_e1k.initialized; }

static bool e1000_hw_is_link_up(void) {
    if (!s_e1k.initialized || !s_e1k.mmio) return false;
    return (e1k_rd(E1000_REG_STATUS) & 0x02u) != 0u;
}

static const uint8_t *e1000_hw_get_mac(size_t *out_len) {
    if (out_len) *out_len = s_e1k.initialized ? 6u : 0u;
    return s_e1k.initialized ? s_e1k.mac : NULL;
}

static size_t e1000_hw_get_mac_len(void) { return s_e1k.initialized ? 6u : 0u; }
static uint16_t e1000_hw_get_vendor_id(void) { return s_e1k.pdev.vendor_id; }
static uint16_t e1000_hw_get_device_id(void) { return s_e1k.pdev.device_id; }
static uint16_t e1000_hw_get_subsys_vendor_id(void) { return s_e1k.pdev.subsys_vendor_id; }
static uint16_t e1000_hw_get_subsys_id(void) { return s_e1k.pdev.subsys_id; }
static uint8_t e1000_hw_get_revision_id(void) { return s_e1k.pdev.revision_id; }
static uint8_t e1000_hw_get_bus(void) { return s_e1k.pdev.bus; }
static uint8_t e1000_hw_get_slot(void) { return s_e1k.pdev.slot; }
static uint8_t e1000_hw_get_func(void) { return s_e1k.pdev.func; }

static int e1000_hw_send_packet(const void *data, uint16_t len) {
    if (!s_e1k.initialized || !data || len == 0u || len > E1000_BUF_STRIDE) return -1;
    uint32_t cur = s_e1k.tx_cur;
    e1000_tx_desc_t *td = &s_e1k.tx_descs[cur];

    uint32_t start = timer_millis();
    while ((td->status & 0x01u) == 0u) {
        if (timer_millis() - start > 100u) return -1;
        __sync_synchronize();
    }

    uint8_t *buf = s_e1k.tx_bufs + cur * E1000_BUF_STRIDE;
    memcpy(buf, data, len);
    td->length = (len < 60u) ? 60u : len;
    if (len < 60u) memset(buf + len, 0, (size_t)(60u - len));
    td->cmd = (1u << 0) | (1u << 1) | (1u << 3); /* EOP | IFCS | RS */
    td->status = 0u;
    __sync_synchronize();

    s_e1k.tx_cur = (cur + 1u) % E1000_TX_DESC_CNT;
    e1k_wr(E1000_REG_TDT, s_e1k.tx_cur);
    return (int)len;
}

static int e1000_hw_peek_packet_size(void) {
    if (!s_e1k.initialized) return 0;
    __sync_synchronize();
    e1000_rx_desc_t *rd = &s_e1k.rx_descs[s_e1k.rx_cur];
    if ((rd->status & 0x01u) == 0u) return 0;
    return (int)rd->length;
}

static int e1000_hw_receive_packet(void *buf, uint32_t max_len) {
    if (!s_e1k.initialized || !buf || max_len == 0u) return 0;
    __sync_synchronize();
    uint32_t cur = s_e1k.rx_cur;
    e1000_rx_desc_t *rd = &s_e1k.rx_descs[cur];
    if ((rd->status & 0x01u) == 0u) return 0;

    uint16_t pkt_len = rd->length;
    uint32_t copy_len = (pkt_len < max_len) ? (uint32_t)pkt_len : max_len;
    if ((rd->errors & 0x97u) == 0u && pkt_len > 0u) {
        memcpy(buf, s_e1k.rx_bufs + cur * E1000_BUF_STRIDE, copy_len);
    } else {
        copy_len = 0u;
    }

    rd->status = 0u;
    rd->errors = 0u;
    __sync_synchronize();
    s_e1k.rx_cur = (cur + 1u) % E1000_RX_DESC_CNT;
    e1k_wr(E1000_REG_RDT, cur);
    return (int)copy_len;
}

static size_t e1000_hw_rx_ring_size(void) { return E1000_RX_DESC_CNT * E1000_BUF_STRIDE; }
static size_t e1000_hw_tx_desc_count(void) { return E1000_TX_DESC_CNT; }
static size_t e1000_hw_max_dma_burst(void) { return 2048u; }

const net_nic_ops_t g_e1000_nic_ops = {
    .driver_name = "e1000",
    .init = e1000_hw_init,
    .shutdown = e1000_hw_shutdown,
    .is_present = e1000_hw_is_present,
    .is_link_up = e1000_hw_is_link_up,
    .get_mac = e1000_hw_get_mac,
    .get_mac_len = e1000_hw_get_mac_len,
    .get_vendor_id = e1000_hw_get_vendor_id,
    .get_device_id = e1000_hw_get_device_id,
    .get_subsys_vendor_id = e1000_hw_get_subsys_vendor_id,
    .get_subsys_id = e1000_hw_get_subsys_id,
    .get_revision_id = e1000_hw_get_revision_id,
    .get_bus = e1000_hw_get_bus,
    .get_slot = e1000_hw_get_slot,
    .get_func = e1000_hw_get_func,
    .send_packet = e1000_hw_send_packet,
    .receive_packet = e1000_hw_receive_packet,
    .peek_packet_size = e1000_hw_peek_packet_size,
    .get_hw_rx_ring_size = e1000_hw_rx_ring_size,
    .get_hw_tx_descriptor_count = e1000_hw_tx_desc_count,
    .get_hw_max_dma_burst = e1000_hw_max_dma_burst,
};
