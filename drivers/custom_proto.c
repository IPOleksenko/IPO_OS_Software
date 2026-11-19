#include "kmd_api.h"
#include "../include/kernel/ext.h"

extern int printf(const char *fmt, ...);
extern int snprintf(char *str, size_t size, const char *fmt, ...);

/* ============================================================================
 * IPO-XMesh: Custom Non-IP Experimental Protocol Driver/Extension (.kmd)
 * - EtherType: 0x88B5 (IEEE 802.1 Local Experimental EtherType)
 * - Custom 64-bit Hierarchical Mesh Addressing (cluster_id:node_id)
 * - Custom Hop-Count Routing & Sequence/Ack Transport Header
 * - Built 100% on IPO_OS Universal Data Channel (ipo_chan_*) & Extension API
 * ============================================================================ */

#define XMESH_ETHERTYPE 0x88B5u
#define XMESH_MAGIC     0x584D5348u /* "XMSH" */

typedef struct __attribute__((packed)) {
    uint8_t  dst_mac[6];
    uint8_t  src_mac[6];
    uint16_t ethertype;      /* 0x88B5 in big-endian (0x88, 0xB5) */
    uint32_t magic;          /* XMESH_MAGIC */
    uint32_t src_mesh_addr;  /* Custom 32-bit mesh node address (Cluster:16 | Node:16) */
    uint32_t dst_mesh_addr;  /* Destination mesh node address */
    uint16_t seq_num;
    uint8_t  hop_limit;
    uint8_t  pkt_type;       /* 1 = DATA, 2 = ACK, 3 = ROUTE_BEACON */
    uint16_t payload_len;
} xmesh_hdr_t;

static uint32_t s_xmesh_ext_id = 0u;
static uint32_t s_xmesh_sub_id = 0u;
static uint32_t s_xmesh_chan_id = 0u;
static uint32_t s_local_mesh_addr = 0x00010001u; /* Cluster 1, Node 1 */
static uint32_t s_tx_count = 0u;
static uint32_t s_rx_count = 0u;
static uint16_t s_seq_counter = 1u;
static char    *s_last_rx_msg = NULL;
static uint32_t s_last_rx_cap = 0u;
static uint32_t s_last_rx_src = 0u;

static void xmesh_rx_callback(uint32_t chan_id, const void *frame, uint32_t len, void *ctx) {
    (void)chan_id;
    (void)ctx;
    if (!frame || len < sizeof(xmesh_hdr_t)) return;
    const xmesh_hdr_t *pkt = (const xmesh_hdr_t *)frame;
    if (pkt->magic != XMESH_MAGIC) return;

    s_rx_count++;
    s_last_rx_src = pkt->src_mesh_addr;
    uint32_t plen = pkt->payload_len;
    uint32_t avail = len - (uint32_t)sizeof(xmesh_hdr_t);
    if (plen > avail) plen = avail;

    if (plen + 1u > s_last_rx_cap) {
        uint32_t ncap = (s_last_rx_cap == 0u) ? 64u : s_last_rx_cap;
        while (ncap < plen + 1u) ncap *= 2u;
        char *nb = (char *)krealloc(s_last_rx_msg, ncap);
        if (nb) {
            s_last_rx_msg = nb;
            s_last_rx_cap = ncap;
        }
    }
    if (s_last_rx_msg && s_last_rx_cap > 0u) {
        uint32_t copy_len = (plen < s_last_rx_cap) ? plen : (s_last_rx_cap - 1u);
        const uint8_t *payload = (const uint8_t *)frame + sizeof(xmesh_hdr_t);
        if (copy_len > 0u) memcpy(s_last_rx_msg, payload, copy_len);
        s_last_rx_msg[copy_len] = '\0';
    }

    serial_printf("[xmesh] RX frame #%u: src=0x%08x -> dst=0x%08x seq=%u hops=%u payload='%s'\n",
                  s_rx_count, pkt->src_mesh_addr, pkt->dst_mesh_addr,
                  (unsigned)pkt->seq_num, (unsigned)pkt->hop_limit,
                  s_last_rx_msg ? s_last_rx_msg : "");
}

static int xmesh_ensure_bound(void) {
    uint32_t cid = ipo_chan_find_by_name("eth0");
    if (cid == 0u) return -1;
    if (s_xmesh_chan_id != cid || s_xmesh_sub_id == 0u) {
        if (s_xmesh_sub_id != 0u) {
            ipo_chan_unsubscribe(s_xmesh_sub_id, 0u);
        }
        s_xmesh_chan_id = cid;
        s_xmesh_sub_id = ipo_chan_subscribe(cid, XMESH_ETHERTYPE, xmesh_rx_callback, NULL, 0u);
    }
    return (s_xmesh_sub_id != 0u) ? 0 : -2;
}

static int xmesh_send_frame(uint32_t dst_mesh_addr, const char *msg) {
    if (xmesh_ensure_bound() != 0) {
        return -1; /* Underlying hardware channel eth0 not available */
    }

    size_t mlen = msg ? strlen(msg) : 0u;
    if (mlen > 0xFFFFu) mlen = 0xFFFFu;
    size_t total_len = sizeof(xmesh_hdr_t) + mlen;
    uint8_t *buf = (uint8_t *)kmalloc(total_len);
    if (!buf) return -1;
    memset(buf, 0, total_len);

    xmesh_hdr_t *hdr = (xmesh_hdr_t *)buf;
    memset(hdr->dst_mac, 0xFF, 6u);

    ipo_chan_info_t cinfo;
    memset(&cinfo, 0, sizeof(cinfo));
    if (ipo_chan_get_info(s_xmesh_chan_id, &cinfo) == 0 && cinfo.hw_addr_len > 0u) {
        uint8_t *hw = (uint8_t *)kmalloc(cinfo.hw_addr_len);
        if (hw) {
            cinfo.hw_addr     = hw;
            cinfo.hw_addr_cap = cinfo.hw_addr_len;
            if (ipo_chan_get_info(s_xmesh_chan_id, &cinfo) == 0) {
                uint32_t copy_hw = (cinfo.hw_addr_len < 6u) ? cinfo.hw_addr_len : 6u;
                memcpy(hdr->src_mac, hw, copy_hw);
            }
            kfree(hw);
        }
    }

    hdr->ethertype     = (uint16_t)((XMESH_ETHERTYPE >> 8) | ((XMESH_ETHERTYPE & 0xFFu) << 8));
    hdr->magic         = XMESH_MAGIC;
    hdr->src_mesh_addr = s_local_mesh_addr;
    hdr->dst_mesh_addr = dst_mesh_addr;
    hdr->seq_num       = s_seq_counter++;
    hdr->hop_limit     = 16u;
    hdr->pkt_type      = 1u;
    hdr->payload_len   = (uint16_t)mlen;
    if (mlen > 0u) memcpy(buf + sizeof(xmesh_hdr_t), msg, mlen);

    int rc = ipo_chan_transmit(s_xmesh_chan_id, buf, (uint32_t)total_len);
    kfree(buf);
    if (rc >= 0) {
        s_tx_count++;
    }
    return rc;
}

static int xmesh_ext_invoke(uint32_t op, uintptr_t a0, uintptr_t a1,
                            uintptr_t a2, uintptr_t a3, void *priv) {
    (void)a2; (void)a3; (void)priv;
    switch (op) {
        case 0u: /* Get status / RX count */
            return (int)s_rx_count;
        case 1u: /* Send custom XMesh packet: a0 = dst_mesh_addr, a1 = msg ptr */
            return xmesh_send_frame((uint32_t)a0, (const char *)a1);
        case 2u: /* Copy last RX message to buffer a0 (max len a1) */
            if (a0 && a1 > 0u) {
                const char *src = s_last_rx_msg ? s_last_rx_msg : "";
                strncpy((char *)a0, src, (size_t)a1 - 1u);
                ((char *)a0)[a1 - 1u] = '\0';
            }
            return (int)s_rx_count;
        case 3u: /* Set local mesh address */
            s_local_mesh_addr = (uint32_t)a0;
            return 0;
        default:
            return -1;
    }
}

static int xmesh_on_command(const char *cmd, int argc, char **argv) {
    if (!cmd || strcmp(cmd, "xmesh") != 0) return -1;

    if (argc >= 2 && strcmp(argv[1], "send") == 0) {
        const char *msg = (argc >= 3) ? argv[2] : "HELLO_XMESH_PROTOCOL";
        int rc = xmesh_send_frame(0x00010002u, msg);
        if (rc < 0) {
            printf("[xmesh] Error: Cannot transmit frame (no active hardware channel 'eth0', rc=%d)\n", rc);
        } else {
            printf("[xmesh] Transmitted custom EtherType 0x88B5 frame (src=0x%08x -> dst=0x00010002, payload='%s')\n",
                   s_local_mesh_addr, msg);
            printf("[xmesh] Stats: TX=%u RX=%u LastRX='%s' (from 0x%08x)\n",
                   s_tx_count, s_rx_count, s_last_rx_msg ? s_last_rx_msg : "", s_last_rx_src);
        }
        return 0;
    }

    xmesh_ensure_bound();
    printf("=== IPO-XMesh Custom Experimental Protocol (EtherType 0x88B5) ===\n");
    printf("  Local Mesh Addr: 0x%08x (Cluster %u, Node %u)\n",
           s_local_mesh_addr, s_local_mesh_addr >> 16, s_local_mesh_addr & 0xFFFFu);
    printf("  Bound Channel:   %s (chan_id=%u, sub_id=%u)\n",
           s_xmesh_chan_id ? "eth0" : "unbound", s_xmesh_chan_id, s_xmesh_sub_id);
    printf("  Frames TX / RX:  %u TX, %u RX\n", s_tx_count, s_rx_count);
    printf("  Last RX Payload: '%s' (from 0x%08x)\n",
           (s_last_rx_msg && s_last_rx_msg[0]) ? s_last_rx_msg : "<none>", s_last_rx_src);
    return 0;
}

static driver_t s_xmesh_drv = {
    .name        = "custom_proto",
    .description = "IPO-XMesh Custom Non-IP Protocol Extension (EtherType 0x88B5)",
    .flags       = DRIVER_FLAG_KERNEL | DRIVER_FLAG_ACTIVE,
    .on_command  = xmesh_on_command,
};

int kmd_main_init(void) {
    if (s_last_rx_msg) {
        kfree(s_last_rx_msg);
        s_last_rx_msg = NULL;
        s_last_rx_cap = 0u;
    }
    xmesh_ensure_bound();

    ipo_ext_desc_t edesc;
    memset(&edesc, 0, sizeof(edesc));
    edesc.name        = "net.xmesh";
    edesc.category    = "protocol";
    edesc.description = "IPO-XMesh Custom Non-IP Protocol over Universal Frame Bus";
    edesc.flags  = IPO_EXT_FLAG_KERNEL | IPO_EXT_FLAG_ACTIVE | IPO_EXT_FLAG_UPDATABLE;
    edesc.invoke = xmesh_ext_invoke;
    s_xmesh_ext_id = ipo_ext_register(&edesc, 0u);

    driver_register(&s_xmesh_drv);
    printf("[custom_proto.kmd] Loaded IPO-XMesh custom protocol (ext_id=%u, chan_id=%u)\n",
           s_xmesh_ext_id, s_xmesh_chan_id);
    return 0;
}

int kmd_main_cleanup(void) {
    if (s_xmesh_sub_id != 0u) {
        ipo_chan_unsubscribe(s_xmesh_sub_id, 0u);
        s_xmesh_sub_id = 0u;
    }
    if (s_xmesh_ext_id != 0u) {
        ipo_ext_unregister(s_xmesh_ext_id, 0u);
        s_xmesh_ext_id = 0u;
    }
    if (s_last_rx_msg) {
        kfree(s_last_rx_msg);
        s_last_rx_msg = NULL;
        s_last_rx_cap = 0u;
    }
    driver_unregister_force("custom_proto");
    printf("[custom_proto.kmd] Unloaded IPO-XMesh custom protocol extension.\n");
    return 0;
}
