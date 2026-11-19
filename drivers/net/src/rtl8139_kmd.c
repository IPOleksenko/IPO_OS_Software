#include <driver/net/rtl8139.h>
#include <driver/net/pci.h>
#include <net/net.h>
#include <net/net_state.h>
#include <net/dns.h>
#include <net/icmp.h>
#include <net/socket.h>
#include <kernel/driver.h>
#include <kernel/device.h>
#include <kernel/async.h>
#include <kernel/ext.h>
#include <memory/kmalloc.h>
#include <syscall.h>
#include <string.h>
#include <stdio.h>

typedef uint32_t (*ipo_syscall_handler_t)(uint32_t, uint32_t, uint32_t *);
extern void ipo_register_syscall(uint32_t num, ipo_syscall_handler_t handler);
extern void ipo_unregister_syscall(uint32_t num);
extern bool ipo_syscall_user_buf_ok(const void *ptr, uint32_t length);
extern bool ipo_syscall_user_str_ok(const char *s);

uint32_t g_rtl8139_chan_id = 0u;
static uint32_t s_rtl8139_ext_id = 0u;
static driver_t s_rtl8139_drv;

/* ============================================================================
 * Dynamic Network Syscall Handlers (registered when rtl8139.kmd is loaded,
 * unregistered when rtl8139.kmd is unloaded)
 * ============================================================================ */

static uint32_t kmd_sys_net_dns_resolve(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 3u || !argv || !argv[0] || !argv[1]) return 0u;
    const char *user_hostname = (const char *)(uintptr_t)argv[0];
    if (!ipo_syscall_user_str_ok(user_hostname)) return 0u;
    size_t host_len = strlen(user_hostname);
    char *hostname = (char *)kmalloc(host_len + 1u);
    if (!hostname) return 0u;
    memcpy(hostname, user_hostname, host_len + 1u);

    uint32_t timeout_ms = argv[2];
    uint32_t max_retries = (argc >= 4u) ? argv[3] : 0u;
    uint32_t retry_interval_ms = (argc >= 5u) ? argv[4] : 0u;

    ip4_addr_t resolved_ip = 0u;
    bool ok = dns_resolve_ex(hostname, &resolved_ip, timeout_ms, max_retries, retry_interval_ms);
    kfree(hostname);
    if (!ok) return 0u;
    *(ip4_addr_t *)(uintptr_t)argv[1] = resolved_ip;
    return 1u;
}

static uint32_t kmd_sys_net_icmp_send(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 6u || !argv || (argv[5] > 0u && !argv[4]) || argv[5] > UINT16_MAX) {
        return (uint32_t)-1;
    }
    const void *data = (const void *)(uintptr_t)argv[4];
    if (!ipo_syscall_user_buf_ok(data, argv[5])) return (uint32_t)-1;
    return (uint32_t)icmp_send_echo(argv[0], (uint16_t)argv[1], (uint16_t)argv[2],
                                    (uint8_t)argv[3], data, (uint16_t)argv[5]);
}

static uint32_t kmd_sys_net_icmp_poll(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 4u || !argv || !argv[2]) return 0u;
    icmp_echo_reply_t *reply = (icmp_echo_reply_t *)(uintptr_t)argv[2];
    if (!ipo_syscall_user_buf_ok(reply, sizeof(*reply))) return 0u;
    return icmp_poll_reply((uint16_t)argv[0], (uint16_t)argv[1], reply, argv[3]) ? 1u : 0u;
}

static uint32_t kmd_sys_net_socket(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 3u || !argv) return (uint32_t)-1;
    return (uint32_t)socket((int)argv[0], (int)argv[1], (int)argv[2]);
}

static uint32_t kmd_sys_net_bind(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 3u || !argv || !argv[1] || argv[2] < sizeof(struct sockaddr_in)) return (uint32_t)-1;
    const void *user_addr = (const void *)(uintptr_t)argv[1];
    if (!ipo_syscall_user_buf_ok(user_addr, sizeof(struct sockaddr_in))) return (uint32_t)-1;
    struct sockaddr_in address;
    memcpy(&address, user_addr, sizeof(address));
    return (uint32_t)bind((int)argv[0], (const struct sockaddr *)&address, sizeof(address));
}

static uint32_t kmd_sys_net_listen(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 2u || !argv) return (uint32_t)-1;
    return (uint32_t)listen((int)argv[0], (int)argv[1]);
}

static uint32_t kmd_sys_net_accept(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 4u || !argv) return (uint32_t)-1;
    if (argv[1] && !ipo_syscall_user_buf_ok((void *)(uintptr_t)argv[1], sizeof(struct sockaddr_in))) return (uint32_t)-1;
    if (argv[2] && !ipo_syscall_user_buf_ok((void *)(uintptr_t)argv[2], sizeof(socklen_t))) return (uint32_t)-1;
    struct sockaddr_in address;
    socklen_t address_len = sizeof(address);
    int client_fd = accept_timeout((int)argv[0],
                                   argv[1] ? (struct sockaddr *)&address : NULL,
                                   argv[1] ? &address_len : NULL,
                                   argv[3]);
    if (client_fd >= 0 && argv[1]) {
        memcpy((void *)(uintptr_t)argv[1], &address, sizeof(address));
        if (argv[2]) *(socklen_t *)(uintptr_t)argv[2] = address_len;
    }
    return (uint32_t)client_fd;
}

static uint32_t kmd_sys_net_connect(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 3u || !argv || !argv[1] || argv[2] < sizeof(struct sockaddr_in)) return (uint32_t)-1;
    const void *user_addr = (const void *)(uintptr_t)argv[1];
    if (!ipo_syscall_user_buf_ok(user_addr, sizeof(struct sockaddr_in))) return (uint32_t)-1;
    struct sockaddr_in address;
    memcpy(&address, user_addr, sizeof(address));
    return (uint32_t)connect((int)argv[0], (const struct sockaddr *)&address, sizeof(address));
}

static uint32_t kmd_sys_net_connect_endpoint(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 3u || !argv || !argv[1] || argv[2] > UINT16_MAX) return (uint32_t)-1;
    const char *user_host = (const char *)(uintptr_t)argv[1];
    if (!ipo_syscall_user_str_ok(user_host)) return (uint32_t)-1;
    size_t host_len = strlen(user_host);
    char *host = (char *)kmalloc(host_len + 1u);
    if (!host) return (uint32_t)-1;
    memcpy(host, user_host, host_len + 1u);
    uint32_t ret = (uint32_t)connect_endpoint((int)argv[0], host, (uint16_t)argv[2]);
    kfree(host);
    return ret;
}

static uint32_t kmd_sys_net_send(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 4u || !argv || !argv[1] || argv[2] == 0u) return (uint32_t)-1;
    const void *user_buf = (const void *)(uintptr_t)argv[1];
    if (!ipo_syscall_user_buf_ok(user_buf, argv[2])) return (uint32_t)-1;
    void *buffer = kmalloc(argv[2]);
    if (!buffer) return (uint32_t)-1;
    memcpy(buffer, user_buf, argv[2]);
    ssize_t result = send((int)argv[0], buffer, argv[2], (int)argv[3]);
    kfree(buffer);
    return (uint32_t)result;
}

static uint32_t kmd_sys_net_recv(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 4u || !argv || !argv[1] || argv[2] == 0u) return (uint32_t)-1;
    void *user_buf = (void *)(uintptr_t)argv[1];
    if (!ipo_syscall_user_buf_ok(user_buf, argv[2])) return (uint32_t)-1;
    void *buffer = kmalloc(argv[2]);
    if (!buffer) return (uint32_t)-1;
    ssize_t result = recv((int)argv[0], buffer, argv[2], (int)argv[3]);
    if (result > 0) memcpy(user_buf, buffer, (size_t)result);
    kfree(buffer);
    return (uint32_t)result;
}

static uint32_t kmd_sys_net_close(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 1u || !argv) return (uint32_t)-1;
    return (uint32_t)socket_close((int)argv[0]);
}

static uint32_t kmd_sys_net_poll(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    net_poll();
    return 0u;
}

static uint32_t kmd_sys_net_get_interface(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 1u || !argv || !argv[0]) return 0u;
    net_if_t *netif = net_get_interface();
    if (!netif) return 0u;
    net_if_t *out = (net_if_t *)(uintptr_t)argv[0];
    if (!ipo_syscall_user_buf_ok(out, sizeof(*out))) return 0u;
    memcpy(out, netif, sizeof(*netif));
    return 1u;
}

static uint32_t kmd_sys_net_init(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num; (void)argc; (void)argv;
    return net_init() ? 1u : 0u;
}

static uint32_t kmd_sys_net_set_ip(uint32_t num, uint32_t argc, uint32_t *argv) {
    (void)num;
    if (argc < 4u || !argv) return (uint32_t)-1;
    net_set_ip(argv[0], argv[1], argv[2], argv[3]);
    return 0u;
}

static void register_net_syscalls(void) {
    ipo_register_syscall(IPO_SYSCALL_NET_DNS_RESOLVE, kmd_sys_net_dns_resolve);
    ipo_register_syscall(IPO_SYSCALL_NET_ICMP_SEND, kmd_sys_net_icmp_send);
    ipo_register_syscall(IPO_SYSCALL_NET_ICMP_POLL, kmd_sys_net_icmp_poll);
    ipo_register_syscall(IPO_SYSCALL_NET_SOCKET, kmd_sys_net_socket);
    ipo_register_syscall(IPO_SYSCALL_NET_BIND, kmd_sys_net_bind);
    ipo_register_syscall(IPO_SYSCALL_NET_LISTEN, kmd_sys_net_listen);
    ipo_register_syscall(IPO_SYSCALL_NET_ACCEPT, kmd_sys_net_accept);
    ipo_register_syscall(IPO_SYSCALL_NET_CONNECT, kmd_sys_net_connect);
    ipo_register_syscall(IPO_SYSCALL_NET_SEND, kmd_sys_net_send);
    ipo_register_syscall(IPO_SYSCALL_NET_RECV, kmd_sys_net_recv);
    ipo_register_syscall(IPO_SYSCALL_NET_CLOSE, kmd_sys_net_close);
    ipo_register_syscall(IPO_SYSCALL_NET_CONNECT_ENDPOINT, kmd_sys_net_connect_endpoint);
    ipo_register_syscall(IPO_SYSCALL_NET_POLL, kmd_sys_net_poll);
    ipo_register_syscall(IPO_SYSCALL_NET_GET_INTERFACE, kmd_sys_net_get_interface);
    ipo_register_syscall(IPO_SYSCALL_NET_INIT, kmd_sys_net_init);
    ipo_register_syscall(IPO_SYSCALL_NET_SET_IP, kmd_sys_net_set_ip);
}

static void unregister_net_syscalls(void) {
    ipo_unregister_syscall(IPO_SYSCALL_NET_DNS_RESOLVE);
    ipo_unregister_syscall(IPO_SYSCALL_NET_ICMP_SEND);
    ipo_unregister_syscall(IPO_SYSCALL_NET_ICMP_POLL);
    ipo_unregister_syscall(IPO_SYSCALL_NET_SOCKET);
    ipo_unregister_syscall(IPO_SYSCALL_NET_BIND);
    ipo_unregister_syscall(IPO_SYSCALL_NET_LISTEN);
    ipo_unregister_syscall(IPO_SYSCALL_NET_ACCEPT);
    ipo_unregister_syscall(IPO_SYSCALL_NET_CONNECT);
    ipo_unregister_syscall(IPO_SYSCALL_NET_SEND);
    ipo_unregister_syscall(IPO_SYSCALL_NET_RECV);
    ipo_unregister_syscall(IPO_SYSCALL_NET_CLOSE);
    ipo_unregister_syscall(IPO_SYSCALL_NET_CONNECT_ENDPOINT);
    ipo_unregister_syscall(IPO_SYSCALL_NET_POLL);
    ipo_unregister_syscall(IPO_SYSCALL_NET_GET_INTERFACE);
    ipo_unregister_syscall(IPO_SYSCALL_NET_INIT);
    ipo_unregister_syscall(IPO_SYSCALL_NET_SET_IP);
}

/* ============================================================================
 * Universal Hardware Channel Callbacks for eth0
 * ============================================================================ */

static int rtl_chan_tx(const void *frame, uint32_t len, void *priv) {
    (void)priv;
    if (len > 0xFFFFu) return -1;
    return rtl8139_send_packet(frame, (uint16_t)len);
}

static int rtl_chan_rx(void *buf, uint32_t max_len, void *priv) {
    (void)priv;
    return rtl8139_receive_packet(buf, max_len);
}

static int rtl_chan_peek(void *priv) {
    (void)priv;
    return rtl8139_peek_packet_size();
}

static bool rtl_chan_link_up(void *priv) {
    (void)priv;
    return rtl8139_is_link_up();
}

static int rtl_ext_invoke(uint32_t op, uintptr_t a0, uintptr_t a1,
                          uintptr_t a2, uintptr_t a3, void *priv) {
    (void)priv;
    switch (op) {
        case 0u: /* Query link status */
            return rtl8139_is_link_up() ? 1 : 0;
        case 1u: { /* Query IPv4 address */
            net_if_t *nif = net_get_interface();
            return nif ? (int)nif->ip : 0;
        }
        case 2u: /* Send raw Ethernet frame */
            return rtl8139_send_packet((const void *)a0, (uint16_t)a1);
        case 3u: /* Set IPv4 config */
            net_set_ip((ip4_addr_t)a0, (ip4_addr_t)a1, (ip4_addr_t)a2, (ip4_addr_t)a3);
            return 0;
        default:
            return -1;
    }
}

/* ============================================================================
 * Driver Hook: Tick Polling & Dynamic Shell Command (`ifconfig`)
 * ============================================================================ */

static void rtl8139_kmd_tick(void) {
    net_poll();
}

static int rtl8139_kmd_command(const char *cmd, int argc, char **argv) {
    if (!cmd || strcmp(cmd, "ifconfig") != 0) return -1;

    if (argc >= 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        printf("Usage: ifconfig [<ip> [netmask] [gateway] [dns]]\n");
        return 0;
    }

    net_if_t *netif = net_get_interface();
    if (!netif) {
        printf("ifconfig: no network interface available\n");
        return 0;
    }

    if (argc >= 2) {
        ip4_addr_t new_ip = 0;
        ip4_addr_t new_mask = netif->netmask;
        ip4_addr_t new_gw = netif->gateway;
        ip4_addr_t new_dns = netif->dns;

        if (!str_to_ip(argv[1], &new_ip)) {
            printf("ifconfig: invalid IP address '%s'\n", argv[1]);
            return 0;
        }
        if (argc >= 3 && !str_to_ip(argv[2], &new_mask)) {
            printf("ifconfig: invalid netmask '%s'\n", argv[2]);
            return 0;
        }
        if (argc >= 4 && !str_to_ip(argv[3], &new_gw)) {
            printf("ifconfig: invalid gateway '%s'\n", argv[3]);
            return 0;
        }
        if (argc >= 5 && !str_to_ip(argv[4], &new_dns)) {
            printf("ifconfig: invalid DNS '%s'\n", argv[4]);
            return 0;
        }

        net_set_ip(new_ip, new_mask, new_gw, new_dns);
        netif = net_get_interface();
        if (!netif) return 0;
        printf("[ifconfig] Network settings updated.\n");
    }

    char *ip_str   = ip_to_str_dyn(netif->ip);
    char *mask_str = ip_to_str_dyn(netif->netmask);
    char *gw_str   = ip_to_str_dyn(netif->gateway);
    char *dns_str  = ip_to_str_dyn(netif->dns);
    char *mac_str  = mac_format_dyn(&netif->mac);

    ipo_chan_info_t cinfo;
    memset(&cinfo, 0, sizeof(cinfo));
    bool has_chan = (g_rtl8139_chan_id != 0u && ipo_chan_get_info(g_rtl8139_chan_id, &cinfo) == 0);
    uint32_t mtu = (has_chan && cinfo.mtu > 0u) ? cinfo.mtu : 1500u;

    printf("eth0: flags=%s  mtu %u  (rtl8139.kmd)\n",
           netif->link_up ? "UP,RUNNING" : "DOWN", mtu);
    printf("        inet %s  netmask %s\n",
           ip_str ? ip_str : "0.0.0.0",
           mask_str ? mask_str : "0.0.0.0");
    printf("        gateway %s  dns %s\n",
           gw_str ? gw_str : "0.0.0.0",
           dns_str ? dns_str : "0.0.0.0");
    printf("        ether %s\n", mac_str ? mac_str : "00:00:00:00:00:00");
    if (has_chan) {
        printf("        RX packets %u  bytes %u  drops %u\n",
               cinfo.rx_packets, cinfo.rx_bytes, cinfo.rx_drops);
        printf("        TX packets %u  bytes %u  errors %u\n",
               cinfo.tx_packets, cinfo.tx_bytes, cinfo.tx_errors);
    }

    if (ip_str)   kfree(ip_str);
    if (mask_str) kfree(mask_str);
    if (gw_str)   kfree(gw_str);
    if (dns_str)  kfree(dns_str);
    if (mac_str)  kfree(mac_str);
    return 0;
}

static void update_pci_network_device_binding(bool attached) {
    const device_node_t *curr = device_get_list();
    while (curr) {
        if (curr->type == DEVICE_TYPE_NETWORK &&
            curr->res.vendor_id == 0x10ECu && curr->res.device_id == 0x8139u) {
            if (attached) {
                device_set_driver(curr->id, "rtl8139", &s_rtl8139_drv);
                device_set_state(curr->id, DEVICE_STATE_ACTIVE);
            } else {
                device_set_driver(curr->id, "", NULL);
                device_set_state(curr->id, DEVICE_STATE_DISCOVERED);
            }
        }
        curr = curr->next;
    }
}

int kmd_main_init(void) {
    if (!net_init()) {
        printf("[rtl8139.kmd] Error: Failed to initialize network stack\n");
        return -1;
    }

    if (!rtl8139_is_present()) {
        net_shutdown();
        printf("[rtl8139.kmd] Error: RTL8139 hardware not present or failed initialization\n");
        return -2;
    }

    /* Register universal raw-frame channel eth0 */
    ipo_chan_desc_t cdesc;
    memset(&cdesc, 0, sizeof(cdesc));
    cdesc.name = "eth0";
    cdesc.mtu  = 1500u;
    size_t mac_len = 0u;
    const uint8_t *mac = rtl8139_get_mac(&mac_len);
    if (mac && mac_len > 0u) {
        cdesc.hw_addr     = mac;
        cdesc.hw_addr_len = (uint32_t)mac_len;
    }
    cdesc.transmit     = rtl_chan_tx;
    cdesc.poll_rx      = rtl_chan_rx;
    cdesc.peek_rx_size = rtl_chan_peek;
    cdesc.is_link_up   = rtl_chan_link_up;
    g_rtl8139_chan_id  = ipo_chan_register(&cdesc, 0u);

    /* Register modular network stack service in Universal Extension Registry */
    ipo_ext_desc_t edesc;
    memset(&edesc, 0, sizeof(edesc));
    edesc.name        = "net.ipv4_stack";
    edesc.category    = "network";
    edesc.description = "IPO_OS_Software Modular Ethernet/ARP/IPv4/ICMP/UDP/TCP/DNS/DHCP Stack";
    edesc.flags  = IPO_EXT_FLAG_KERNEL | IPO_EXT_FLAG_ACTIVE | IPO_EXT_FLAG_UPDATABLE;
    edesc.invoke = rtl_ext_invoke;
    s_rtl8139_ext_id = ipo_ext_register(&edesc, 0u);

    /* Register dynamic network syscalls (0x1090..0x109F) */
    register_net_syscalls();

    /* Register driver descriptor (continuous polling tick + dynamic ifconfig hook) */
    memset(&s_rtl8139_drv, 0, sizeof(s_rtl8139_drv));
    s_rtl8139_drv.name        = "rtl8139";
    s_rtl8139_drv.description = "Realtek RTL8139 PCI NIC + Modular Network Stack (eth0)";
    s_rtl8139_drv.flags       = DRIVER_FLAG_KERNEL | DRIVER_FLAG_ACTIVE | DRIVER_FLAG_CONTINUOUS_TICK;
    s_rtl8139_drv.on_tick     = rtl8139_kmd_tick;
    s_rtl8139_drv.on_command  = rtl8139_kmd_command;
    driver_register(&s_rtl8139_drv);

    update_pci_network_device_binding(true);
    async_start_task("rtl8139_poll", 10u, rtl8139_kmd_tick);

    printf("[rtl8139.kmd] Network driver and modular stack registered (chan_id=%u, ext_id=%u)\n",
           g_rtl8139_chan_id, s_rtl8139_ext_id);
    return 0;
}

int kmd_main_cleanup(void) {
    async_stop_task("rtl8139_poll");
    unregister_net_syscalls();

    if (s_rtl8139_ext_id != 0u) {
        ipo_ext_unregister(s_rtl8139_ext_id, 0u);
        s_rtl8139_ext_id = 0u;
    }
    if (g_rtl8139_chan_id != 0u) {
        ipo_chan_unregister(g_rtl8139_chan_id, 0u);
        g_rtl8139_chan_id = 0u;
    }

    update_pci_network_device_binding(false);
    driver_unregister_force("rtl8139");
    net_shutdown();

    printf("[rtl8139.kmd] Unloaded network driver & stack; eth0 disabled and resources released.\n");
    return 0;
}
