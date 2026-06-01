#include "net.h"
#include "e1000.h"
#include "serial.h"
#include "string.h"
#include "timer.h"

/* QEMU user-mode networking defaults. */
#define NET_IP        0x0A000210u /* 10.0.2.16 (avoid SLIRP DHCP .15) */
#define NET_NETMASK   0xFFFFFF00u /* 255.255.255.0 */
#define NET_GATEWAY   0x0A000202u /* 10.0.2.2 */
#define NET_DNS       0x0A000203u /* 10.0.2.3 (SLIRP DNS) */

#define ETHERTYPE_ARP 0x0806u
#define ETHERTYPE_IP  0x0800u
#define IP_PROTO_ICMP 1u
#define IP_PROTO_UDP  17u
#define IP_PROTO_TCP  6u
#define DNS_PORT      53u
#define DNS_SRC_PORT  0xC000u

/* TCP flags */
#define TCP_FIN 0x01u
#define TCP_SYN 0x02u
#define TCP_RST 0x04u
#define TCP_PSH 0x08u
#define TCP_ACK 0x10u

enum tcp_state {
    TCP_CLOSED = 0,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_CLOSING,
    TCP_DONE
};

#define ARP_CACHE_SIZE 8

static struct e1000_device g_nic;
static uint8_t g_mac[6];

struct arp_entry {
    uint32_t ip;     /* host order */
    uint8_t mac[6];
    uint8_t valid;
};
static struct arp_entry g_arp_cache[ARP_CACHE_SIZE];

/* Last received ICMP echo reply, for net_ping() to match against. */
static volatile uint32_t g_last_reply_ip;
static volatile uint16_t g_last_reply_id;
static volatile uint16_t g_last_reply_seq;
static volatile uint8_t g_got_reply;

/* Last received DNS answer, for net_resolve() to match against. */
static volatile uint16_t g_dns_id;
static volatile uint32_t g_dns_result_ip;
static volatile uint8_t g_dns_got;

/* Single synchronous TCP client connection. */
struct tcp_conn {
    int state;
    uint32_t remote_ip;
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t snd_nxt;   /* next sequence we will send */
    uint32_t rcv_nxt;   /* next sequence we expect from peer */
    uint8_t mac[6];
    /* receive sink */
    uint8_t *rx_buf;
    int rx_cap;
    int rx_len;
    uint8_t peer_fin;
};
static struct tcp_conn g_tcp;

static const uint8_t BROADCAST_MAC[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

/* ---- byte-order helpers ---- */
static uint16_t hton16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static uint16_t ntoh16(uint16_t v) { return hton16(v); }
static uint32_t hton32(uint32_t v) {
    return ((v & 0xffu) << 24) | ((v & 0xff00u) << 8) | ((v & 0xff0000u) >> 8) | ((v >> 24) & 0xffu);
}
static uint32_t ntoh32(uint32_t v) { return hton32(v); }

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static uint16_t ip_checksum(const void *data, size_t length) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    size_t i;

    for (i = 0; i + 1 < length; i += 2) {
        sum += (uint32_t)((p[i] << 8) | p[i + 1]);
    }
    if (i < length) {
        sum += (uint32_t)(p[i] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return (uint16_t)(~sum & 0xffffu);
}

/* ---- ARP cache ---- */
static const uint8_t *arp_lookup(uint32_t ip) {
    int i;
    for (i = 0; i < ARP_CACHE_SIZE; ++i) {
        if (g_arp_cache[i].valid && g_arp_cache[i].ip == ip) {
            return g_arp_cache[i].mac;
        }
    }
    return 0;
}

static void arp_insert(uint32_t ip, const uint8_t *mac) {
    int i;
    int slot = 0;
    for (i = 0; i < ARP_CACHE_SIZE; ++i) {
        if (g_arp_cache[i].valid && g_arp_cache[i].ip == ip) {
            slot = i;
            break;
        }
        if (!g_arp_cache[i].valid) {
            slot = i;
        }
    }
    g_arp_cache[slot].ip = ip;
    memcpy(g_arp_cache[slot].mac, mac, 6);
    g_arp_cache[slot].valid = 1;
}

/* ---- frame construction ---- */
static uint8_t g_txframe[1518];

static void eth_build_header(uint8_t *frame, const uint8_t *dst_mac, uint16_t ethertype) {
    memcpy(frame, dst_mac, 6);
    memcpy(frame + 6, g_mac, 6);
    wr16(frame + 12, ethertype);
}

static void send_arp_request(uint32_t target_ip) {
    uint8_t *f = g_txframe;
    uint8_t *arp;

    eth_build_header(f, BROADCAST_MAC, ETHERTYPE_ARP);
    arp = f + 14;
    wr16(arp + 0, 1);              /* htype Ethernet */
    wr16(arp + 2, ETHERTYPE_IP);   /* ptype IPv4 */
    arp[4] = 6;                    /* hlen */
    arp[5] = 4;                    /* plen */
    wr16(arp + 6, 1);              /* oper request */
    memcpy(arp + 8, g_mac, 6);     /* sender mac */
    wr32(arp + 14, NET_IP);        /* sender ip */
    memset(arp + 18, 0, 6);        /* target mac unknown */
    wr32(arp + 24, target_ip);     /* target ip */

    e1000_transmit(&g_nic, f, 14 + 28);
}

static void send_arp_reply(uint32_t target_ip, const uint8_t *target_mac) {
    uint8_t *f = g_txframe;
    uint8_t *arp;

    eth_build_header(f, target_mac, ETHERTYPE_ARP);
    arp = f + 14;
    wr16(arp + 0, 1);
    wr16(arp + 2, ETHERTYPE_IP);
    arp[4] = 6;
    arp[5] = 4;
    wr16(arp + 6, 2);              /* oper reply */
    memcpy(arp + 8, g_mac, 6);
    wr32(arp + 14, NET_IP);
    memcpy(arp + 18, target_mac, 6);
    wr32(arp + 24, target_ip);

    e1000_transmit(&g_nic, f, 14 + 28);
}

/* Resolve the next-hop MAC for dst_ip (gateway if off-subnet). Sends an ARP
 * request and polls for the reply up to timeout_ms. Returns pointer to a
 * cached MAC or 0 on failure. */
static const uint8_t *resolve_mac(uint32_t dst_ip, int timeout_ms) {
    uint32_t next_hop = ((dst_ip & NET_NETMASK) == (NET_IP & NET_NETMASK)) ? dst_ip : NET_GATEWAY;
    const uint8_t *mac = arp_lookup(next_hop);
    uint64_t deadline;
    uint32_t hz = timer_frequency_hz();

    if (mac) {
        return mac;
    }

    send_arp_request(next_hop);
    deadline = timer_tick_count() + (uint64_t)timeout_ms * hz / 1000u + 1u;
    /* Interrupts are disabled inside the int 0x80 handler, so the timer may not
     * advance; bound the wait with a spin counter too so we can never hang. */
    {
        uint64_t spins = (uint64_t)timeout_ms * 200000u + 1000000u;
        while (timer_tick_count() < deadline && spins-- > 0) {
            net_poll();
            mac = arp_lookup(next_hop);
            if (mac) {
                return mac;
            }
            __asm__ volatile("pause");
        }
    }
    return 0;
}

/* Compute the TCP checksum over the pseudo-header + TCP segment. */
static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip, const uint8_t *seg, uint16_t seg_len) {
    uint32_t sum = 0;
    uint16_t i;

    sum += (src_ip >> 16) & 0xffffu;
    sum += src_ip & 0xffffu;
    sum += (dst_ip >> 16) & 0xffffu;
    sum += dst_ip & 0xffffu;
    sum += IP_PROTO_TCP;
    sum += seg_len;

    for (i = 0; i + 1 < seg_len; i += 2) {
        sum += (uint32_t)((seg[i] << 8) | seg[i + 1]);
    }
    if (i < seg_len) {
        sum += (uint32_t)(seg[i] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return (uint16_t)(~sum & 0xffffu);
}

/* Send a TCP segment for g_tcp with the given flags and optional payload. */
static int tcp_send(uint8_t flags, const uint8_t *payload, uint16_t payload_len) {
    uint8_t *f = g_txframe;
    uint8_t *ip = f + 14;
    uint8_t *tcp = ip + 20;
    uint16_t seg_len = (uint16_t)(20 + payload_len);
    uint16_t ip_total = (uint16_t)(20 + seg_len);
    uint16_t i;

    eth_build_header(f, g_tcp.mac, ETHERTYPE_IP);

    wr16(tcp + 0, g_tcp.local_port);
    wr16(tcp + 2, g_tcp.remote_port);
    wr32(tcp + 4, g_tcp.snd_nxt);
    wr32(tcp + 8, (flags & TCP_ACK) ? g_tcp.rcv_nxt : 0u);
    tcp[12] = 0x50;          /* data offset 5 (20 bytes), no options */
    tcp[13] = flags;
    wr16(tcp + 14, 0x4000);  /* window */
    wr16(tcp + 16, 0);       /* checksum placeholder */
    wr16(tcp + 18, 0);       /* urgent */
    for (i = 0; i < payload_len; ++i) {
        tcp[20 + i] = payload[i];
    }
    wr16(tcp + 16, tcp_checksum(NET_IP, g_tcp.remote_ip, tcp, seg_len));

    ip[0] = 0x45; ip[1] = 0;
    wr16(ip + 2, ip_total);
    wr16(ip + 4, 0);
    wr16(ip + 6, 0x4000);
    ip[8] = 64; ip[9] = IP_PROTO_TCP;
    wr16(ip + 10, 0);
    wr32(ip + 12, NET_IP);
    wr32(ip + 16, g_tcp.remote_ip);
    wr16(ip + 10, ip_checksum(ip, 20));

    return e1000_transmit(&g_nic, f, 14 + ip_total);
}

/* Process one inbound TCP segment addressed to our connection. */
static void handle_tcp(const uint8_t *ip, const uint8_t *tcp, uint16_t seg_len) {
    uint16_t src_port = rd16(tcp + 0);
    uint16_t dst_port = rd16(tcp + 2);
    uint32_t seq = rd32(tcp + 4);
    uint8_t flags = tcp[13];
    uint8_t data_off = (uint8_t)((tcp[12] >> 4) * 4u);
    uint16_t payload_len = (seg_len > data_off) ? (uint16_t)(seg_len - data_off) : 0u;
    const uint8_t *payload = tcp + data_off;
    uint32_t src_ip = rd32(ip + 12);

    if (g_tcp.state == TCP_CLOSED) {
        return;
    }
    if (src_ip != g_tcp.remote_ip || src_port != g_tcp.remote_port || dst_port != g_tcp.local_port) {
        return;
    }

    if (flags & TCP_RST) {
        g_tcp.state = TCP_DONE;
        return;
    }

    if (g_tcp.state == TCP_SYN_SENT) {
        if ((flags & TCP_SYN) && (flags & TCP_ACK)) {
            g_tcp.rcv_nxt = seq + 1u;
            g_tcp.snd_nxt += 1u;            /* our SYN consumed one seq */
            tcp_send(TCP_ACK, 0, 0);
            g_tcp.state = TCP_ESTABLISHED;
        }
        return;
    }

    /* ESTABLISHED / CLOSING: accept in-order data, ACK it. */
    if (payload_len > 0 && seq == g_tcp.rcv_nxt) {
        int copy = payload_len;
        if (g_tcp.rx_len + copy > g_tcp.rx_cap) {
            copy = g_tcp.rx_cap - g_tcp.rx_len;
        }
        if (copy > 0) {
            memcpy(g_tcp.rx_buf + g_tcp.rx_len, payload, (size_t)copy);
            g_tcp.rx_len += copy;
        }
        g_tcp.rcv_nxt += payload_len;
        tcp_send(TCP_ACK, 0, 0);
    }

    if (flags & TCP_FIN) {
        g_tcp.rcv_nxt += 1u;
        g_tcp.peer_fin = 1;
        tcp_send(TCP_ACK | TCP_FIN, 0, 0);
        g_tcp.snd_nxt += 1u;
        g_tcp.state = TCP_DONE;
    }
}

static void send_icmp_echo(uint32_t dst_ip, const uint8_t *dst_mac, uint16_t id, uint16_t seq) {
    uint8_t *f = g_txframe;
    uint8_t *ip;
    uint8_t *icmp;
    uint16_t ip_total;
    int i;

    eth_build_header(f, dst_mac, ETHERTYPE_IP);
    ip = f + 14;
    icmp = ip + 20;

    /* ICMP echo request: type 8, code 0, 32 bytes payload. */
    icmp[0] = 8;
    icmp[1] = 0;
    wr16(icmp + 2, 0); /* checksum placeholder */
    wr16(icmp + 4, id);
    wr16(icmp + 6, seq);
    for (i = 0; i < 32; ++i) {
        icmp[8 + i] = (uint8_t)('a' + (i % 26));
    }
    {
        uint16_t csum = ip_checksum(icmp, 8 + 32);
        wr16(icmp + 2, csum);
    }

    ip_total = 20 + 8 + 32;
    ip[0] = 0x45;          /* version 4, IHL 5 */
    ip[1] = 0;             /* TOS */
    wr16(ip + 2, ip_total);
    wr16(ip + 4, 0);       /* id */
    wr16(ip + 6, 0x4000);  /* don't fragment */
    ip[8] = 64;            /* TTL */
    ip[9] = IP_PROTO_ICMP;
    wr16(ip + 10, 0);      /* checksum placeholder */
    wr32(ip + 12, NET_IP);
    wr32(ip + 16, dst_ip);
    {
        uint16_t csum = ip_checksum(ip, 20);
        wr16(ip + 10, csum);
    }

    e1000_transmit(&g_nic, f, 14 + ip_total);
}

/* ---- inbound processing ---- */
static void handle_arp(const uint8_t *frame, uint16_t len) {
    const uint8_t *arp = frame + 14;
    uint16_t oper;
    uint32_t sender_ip;
    uint32_t target_ip;

    if (len < 14 + 28) {
        return;
    }
    oper = rd16(arp + 6);
    sender_ip = rd32(arp + 14);
    target_ip = rd32(arp + 24);

    /* Learn the sender mapping in all cases. */
    arp_insert(sender_ip, arp + 8);

    if (oper == 1 && target_ip == NET_IP) {
        /* Someone is asking for us — reply. */
        send_arp_reply(sender_ip, arp + 8);
    }
}

static void handle_icmp(const uint8_t *frame, const uint8_t *ip, const uint8_t *icmp, uint16_t icmp_len) {
    uint8_t type = icmp[0];
    uint32_t src_ip = rd32(ip + 12);

    if (type == 0) {
        /* Echo reply — record for net_ping(). */
        g_last_reply_ip = src_ip;
        g_last_reply_id = rd16(icmp + 4);
        g_last_reply_seq = rd16(icmp + 6);
        g_got_reply = 1;
    } else if (type == 8) {
        /* Echo request — answer it so others can ping us. */
        const uint8_t *src_mac = frame + 6;
        uint8_t *f = g_txframe;
        uint8_t *oip;
        uint8_t *oicmp;
        uint16_t ip_total = (uint16_t)(20 + icmp_len);

        if (ip_total + 14u > sizeof(g_txframe)) {
            return;
        }
        eth_build_header(f, src_mac, ETHERTYPE_IP);
        oip = f + 14;
        oicmp = oip + 20;
        memcpy(oicmp, icmp, icmp_len);
        oicmp[0] = 0; /* echo reply */
        wr16(oicmp + 2, 0);
        wr16(oicmp + 2, ip_checksum(oicmp, icmp_len));

        oip[0] = 0x45; oip[1] = 0;
        wr16(oip + 2, ip_total);
        wr16(oip + 4, 0);
        wr16(oip + 6, 0x4000);
        oip[8] = 64; oip[9] = IP_PROTO_ICMP;
        wr16(oip + 10, 0);
        wr32(oip + 12, NET_IP);
        wr32(oip + 16, src_ip);
        wr16(oip + 10, ip_checksum(oip, 20));

        e1000_transmit(&g_nic, f, 14 + ip_total);
    }
}

static void handle_ip(const uint8_t *frame, uint16_t len) {
    const uint8_t *ip = frame + 14;
    uint8_t ihl;
    uint16_t total;
    uint32_t dst_ip;

    if (len < 14 + 20) {
        return;
    }
    ihl = (uint8_t)((ip[0] & 0x0fu) * 4u);
    total = rd16(ip + 2);
    dst_ip = rd32(ip + 16);

    if (dst_ip != NET_IP) {
        return;
    }
    if (ip[9] == IP_PROTO_ICMP && total >= ihl + 8) {
        handle_icmp(frame, ip, ip + ihl, (uint16_t)(total - ihl));
    } else if (ip[9] == IP_PROTO_TCP && total >= ihl + 20) {
        handle_tcp(ip, ip + ihl, (uint16_t)(total - ihl));
    } else if (ip[9] == IP_PROTO_UDP && total >= ihl + 8) {
        const uint8_t *udp = ip + ihl;
        uint16_t src_port = rd16(udp + 0);
        const uint8_t *dns;

        /* Only care about DNS responses (source port 53) to our query port. */
        if (src_port != DNS_PORT || rd16(udp + 2) != DNS_SRC_PORT) {
            return;
        }
        dns = udp + 8;
        {
            uint16_t id = rd16(dns + 0);
            uint16_t ancount = rd16(dns + 6);
            uint16_t qdcount = rd16(dns + 4);
            const uint8_t *p = dns + 12;
            uint16_t q;
            uint16_t a;

            /* Skip the question section. */
            for (q = 0; q < qdcount; ++q) {
                while (*p != 0) {
                    if ((*p & 0xC0u) == 0xC0u) { p += 2; goto qdone; }
                    p += (*p) + 1;
                }
                p += 1; /* zero label */
            qdone:
                p += 4; /* qtype + qclass */
            }

            /* Walk answers, take the first A record. */
            for (a = 0; a < ancount; ++a) {
                uint16_t type;
                uint16_t rdlength;
                if ((*p & 0xC0u) == 0xC0u) {
                    p += 2;
                } else {
                    while (*p != 0) { p += (*p) + 1; }
                    p += 1;
                }
                type = rd16(p + 0);
                rdlength = rd16(p + 8);
                if (type == 1 && rdlength == 4) {
                    g_dns_result_ip = rd32(p + 10);
                    g_dns_id = id;
                    g_dns_got = 1;
                    return;
                }
                p += 10 + rdlength;
            }
        }
    }
}

static uint8_t g_rxframe[2048];

void net_poll(void) {
    int n;

    if (!g_nic.present) {
        return;
    }

    while ((n = e1000_poll_receive(&g_nic, g_rxframe, sizeof(g_rxframe))) > 0) {
        uint16_t ethertype;

        if (n < 14) {
            continue;
        }
        ethertype = rd16(g_rxframe + 12);
        if (ethertype == ETHERTYPE_ARP) {
            handle_arp(g_rxframe, (uint16_t)n);
        } else if (ethertype == ETHERTYPE_IP) {
            handle_ip(g_rxframe, (uint16_t)n);
        }
    }
}

void net_init(void) {
    memset(g_arp_cache, 0, sizeof(g_arp_cache));
    g_got_reply = 0;

    if (!e1000_init(&g_nic)) {
        serial_write("NET: no network card; networking disabled\n");
        return;
    }
    memcpy(g_mac, g_nic.mac, 6);
    serial_write("NET: interface up, ip=10.0.2.16 gw=10.0.2.2\n");
}

void net_get_info(struct net_info *out) {
    memset(out, 0, sizeof(*out));
    out->up = g_nic.present;
    memcpy(out->mac, g_mac, 6);
    out->ip = NET_IP;
    out->netmask = NET_NETMASK;
    out->gateway = NET_GATEWAY;
}

int net_ping(uint32_t dst_ip, int count, int timeout_ms, int *rtt_ms_out) {
    int replies = 0;
    int seq;
    uint16_t id = 0x1234;
    uint32_t hz = timer_frequency_hz();
    const uint8_t *dst_mac;

    if (!g_nic.present) {
        return -1;
    }

    dst_mac = resolve_mac(dst_ip, 1000);
    if (!dst_mac) {
        serial_write("NET: ARP resolution failed\n");
        return -2;
    }

    for (seq = 0; seq < count; ++seq) {
        uint64_t start = timer_tick_count();
        uint64_t deadline = start + (uint64_t)timeout_ms * hz / 1000u + 1u;
        uint64_t spins = (uint64_t)timeout_ms * 200000u + 1000000u;

        g_got_reply = 0;
        send_icmp_echo(dst_ip, dst_mac, id, (uint16_t)seq);

        while (timer_tick_count() < deadline && spins-- > 0) {
            net_poll();
            if (g_got_reply && g_last_reply_ip == dst_ip && g_last_reply_seq == (uint16_t)seq) {
                uint64_t elapsed = timer_tick_count() - start;
                if (rtt_ms_out) {
                    *rtt_ms_out = (int)(elapsed * 1000u / hz);
                }
                ++replies;
                break;
            }
            __asm__ volatile("pause");
        }
    }

    return replies;
}

/* Encode a hostname into DNS label format at dst: "heise.de" ->
 * 05 h e i s e 02 d e 00. Returns the number of bytes written, or 0. */
static int dns_encode_name(const char *host, uint8_t *dst, int dst_cap) {
    int out = 0;
    const char *seg = host;
    const char *p = host;

    for (;;) {
        if (*p == '.' || *p == '\0') {
            int len = (int)(p - seg);
            if (len <= 0 || len > 63 || out + len + 1 >= dst_cap) {
                return 0;
            }
            dst[out++] = (uint8_t)len;
            while (seg < p) {
                dst[out++] = (uint8_t)*seg++;
            }
            if (*p == '\0') {
                break;
            }
            seg = p + 1;
        }
        ++p;
    }
    if (out + 1 >= dst_cap) {
        return 0;
    }
    dst[out++] = 0; /* root label */
    return out;
}

static int send_dns_query(const char *host, uint16_t id, const uint8_t *dst_mac) {
    uint8_t *f = g_txframe;
    uint8_t *ip;
    uint8_t *udp;
    uint8_t *dns;
    int qname_len;
    uint16_t udp_len;
    uint16_t ip_total;

    eth_build_header(f, dst_mac, ETHERTYPE_IP);
    ip = f + 14;
    udp = ip + 20;
    dns = udp + 8;

    /* DNS header: id, flags=0x0100 (recursion desired), qdcount=1. */
    wr16(dns + 0, id);
    wr16(dns + 2, 0x0100);
    wr16(dns + 4, 1);
    wr16(dns + 6, 0);
    wr16(dns + 8, 0);
    wr16(dns + 10, 0);

    qname_len = dns_encode_name(host, dns + 12, 256);
    if (qname_len <= 0) {
        return -1;
    }
    wr16(dns + 12 + qname_len + 0, 1); /* qtype A */
    wr16(dns + 12 + qname_len + 2, 1); /* qclass IN */

    udp_len = (uint16_t)(8 + 12 + qname_len + 4);
    wr16(udp + 0, DNS_SRC_PORT);
    wr16(udp + 2, DNS_PORT);
    wr16(udp + 4, udp_len);
    wr16(udp + 6, 0); /* checksum 0 = not computed (allowed for IPv4 UDP) */

    ip_total = (uint16_t)(20 + udp_len);
    ip[0] = 0x45; ip[1] = 0;
    wr16(ip + 2, ip_total);
    wr16(ip + 4, 0);
    wr16(ip + 6, 0x4000);
    ip[8] = 64; ip[9] = IP_PROTO_UDP;
    wr16(ip + 10, 0);
    wr32(ip + 12, NET_IP);
    wr32(ip + 16, NET_DNS);
    wr16(ip + 10, ip_checksum(ip, 20));

    return e1000_transmit(&g_nic, f, 14 + ip_total);
}

int net_resolve(const char *hostname, uint32_t *out_ip, int timeout_ms) {
    const uint8_t *dst_mac;
    uint16_t id = 0xABCD;
    uint32_t hz = timer_frequency_hz();
    uint64_t deadline;
    uint64_t spins;

    if (!g_nic.present || hostname == 0 || out_ip == 0) {
        return 0;
    }

    /* DNS server is off-subnet (10.0.2.3) -> goes via the gateway's MAC. */
    dst_mac = resolve_mac(NET_DNS, 1000);
    if (!dst_mac) {
        return 0;
    }

    g_dns_got = 0;
    if (send_dns_query(hostname, id, dst_mac) < 0) {
        return 0;
    }

    deadline = timer_tick_count() + (uint64_t)timeout_ms * hz / 1000u + 1u;
    spins = (uint64_t)timeout_ms * 200000u + 1000000u;
    while (timer_tick_count() < deadline && spins-- > 0) {
        net_poll();
        if (g_dns_got && g_dns_id == id) {
            *out_ip = g_dns_result_ip;
            return 1;
        }
        __asm__ volatile("pause");
    }
    return 0;
}

/* Wait until cond() is true or timeout, polling the NIC. Returns 1 if cond
 * became true, 0 on timeout. (cond checked via the connection state.) */
static int tcp_wait_state(int want_min_state, int timeout_ms) {
    uint32_t hz = timer_frequency_hz();
    uint64_t deadline = timer_tick_count() + (uint64_t)timeout_ms * hz / 1000u + 1u;
    uint64_t spins = (uint64_t)timeout_ms * 200000u + 1000000u;

    while (timer_tick_count() < deadline && spins-- > 0) {
        net_poll();
        if (g_tcp.state >= want_min_state) {
            return 1;
        }
        __asm__ volatile("pause");
    }
    return 0;
}

int net_http_get(uint32_t dst_ip, uint16_t port, const char *host_header,
                 const char *path, char *out, int out_cap, int timeout_ms) {
    const uint8_t *dst_mac;
    char req[512];
    int rlen = 0;
    uint32_t hz = timer_frequency_hz();
    uint64_t deadline;
    uint64_t spins;

    if (!g_nic.present || out == 0 || out_cap <= 0) {
        return -1;
    }

    dst_mac = resolve_mac(dst_ip, 1000);
    if (!dst_mac) {
        return -2;
    }

    /* Initialise the connection. */
    memset(&g_tcp, 0, sizeof(g_tcp));
    g_tcp.state = TCP_SYN_SENT;
    g_tcp.remote_ip = dst_ip;
    g_tcp.remote_port = port;
    g_tcp.local_port = 0xD000u;
    g_tcp.snd_nxt = 0x1000u;       /* initial send sequence */
    g_tcp.rcv_nxt = 0;
    memcpy(g_tcp.mac, dst_mac, 6);
    g_tcp.rx_buf = (uint8_t *)out;
    g_tcp.rx_cap = out_cap;
    g_tcp.rx_len = 0;

    /* Three-way handshake: send SYN, wait for ESTABLISHED. */
    tcp_send(TCP_SYN, 0, 0);
    if (!tcp_wait_state(TCP_ESTABLISHED, timeout_ms)) {
        return -3;
    }

    /* Build and send the HTTP request. */
    {
        const char *p;
        int i = 0;
        const char *parts[6];
        int pi;
        parts[0] = "GET "; parts[1] = path; parts[2] = " HTTP/1.0\r\nHost: ";
        parts[3] = host_header; parts[4] = "\r\nConnection: close\r\n\r\n"; parts[5] = 0;
        for (pi = 0; parts[pi] != 0; ++pi) {
            for (p = parts[pi]; *p && i < (int)sizeof(req); ++p) {
                req[i++] = *p;
            }
        }
        rlen = i;
    }
    tcp_send(TCP_PSH | TCP_ACK, (const uint8_t *)req, (uint16_t)rlen);
    g_tcp.snd_nxt += (uint32_t)rlen;

    /* Collect the response until the peer closes or we time out / fill up. */
    deadline = timer_tick_count() + (uint64_t)timeout_ms * hz / 1000u + 1u;
    spins = (uint64_t)timeout_ms * 200000u + 1000000u;
    while (timer_tick_count() < deadline && spins-- > 0) {
        net_poll();
        if (g_tcp.state == TCP_DONE || g_tcp.rx_len >= g_tcp.rx_cap) {
            break;
        }
        __asm__ volatile("pause");
    }

    return g_tcp.rx_len;
}

/* ---- Raw TCP stream API (used by the TLS layer in net_tls.c) ----
 * Same single-connection g_tcp model as net_http_get, but split into open /
 * send / recv / close so an external state machine (BearSSL) can drive I/O.
 * Received bytes land in g_tcp_stream_rx; tcp_stream_recv() drains them. */
static uint8_t g_tcp_stream_rx[32768];

int tcp_open(uint32_t dst_ip, uint16_t port, int timeout_ms) {
    const uint8_t *dst_mac;

    if (!g_nic.present) {
        return -1;
    }
    dst_mac = resolve_mac(dst_ip, 1000);
    if (!dst_mac) {
        return -2;
    }
    memset(&g_tcp, 0, sizeof(g_tcp));
    g_tcp.state = TCP_SYN_SENT;
    g_tcp.remote_ip = dst_ip;
    g_tcp.remote_port = port;
    g_tcp.local_port = 0xD100u;
    g_tcp.snd_nxt = 0x2000u;
    g_tcp.rcv_nxt = 0;
    memcpy(g_tcp.mac, dst_mac, 6);
    g_tcp.rx_buf = g_tcp_stream_rx;
    g_tcp.rx_cap = (int)sizeof(g_tcp_stream_rx);
    g_tcp.rx_len = 0;

    tcp_send(TCP_SYN, 0, 0);
    if (!tcp_wait_state(TCP_ESTABLISHED, timeout_ms)) {
        return -3;
    }
    return 0;
}

int tcp_stream_send(const uint8_t *data, int len) {
    int off = 0;
    if (data == 0 || len < 0) {
        return -1;
    }
    while (off < len) {
        int chunk = len - off;
        if (chunk > 1400) chunk = 1400;
        tcp_send(TCP_PSH | TCP_ACK, data + off, (uint16_t)chunk);
        g_tcp.snd_nxt += (uint32_t)chunk;
        off += chunk;
        net_poll();   /* pick up ACKs / incoming data between segments */
    }
    return len;
}

int tcp_stream_recv(uint8_t *buf, int len, int timeout_ms) {
    uint32_t hz = timer_frequency_hz();
    uint64_t deadline = timer_tick_count() + (uint64_t)timeout_ms * hz / 1000u + 1u;
    uint64_t spins = (uint64_t)timeout_ms * 200000u + 1000000u;

    if (buf == 0 || len <= 0) {
        return -1;
    }
    for (;;) {
        net_poll();
        if (g_tcp.rx_len > 0) {
            int n = g_tcp.rx_len;
            if (n > len) n = len;
            memcpy(buf, g_tcp.rx_buf, (size_t)n);
            if (g_tcp.rx_len - n > 0) {
                memmove(g_tcp.rx_buf, g_tcp.rx_buf + n, (size_t)(g_tcp.rx_len - n));
            }
            g_tcp.rx_len -= n;
            return n;
        }
        if (g_tcp.state == TCP_DONE) {
            return 0;   /* peer closed, nothing buffered */
        }
        if (timer_tick_count() >= deadline || spins-- == 0) {
            return -1;  /* timeout */
        }
        __asm__ volatile("pause");
    }
}

void tcp_close(void) {
    if (g_tcp.state == TCP_ESTABLISHED) {
        tcp_send(TCP_FIN | TCP_ACK, 0, 0);
        g_tcp.snd_nxt += 1u;
    }
    g_tcp.state = TCP_CLOSED;
}

int net_parse_ipv4(const char *text, uint32_t *out_ip) {
    uint32_t parts[4] = {0, 0, 0, 0};
    int idx = 0;
    int digits = 0;
    const char *p = text;

    if (text == 0) {
        return 0;
    }
    while (*p) {
        if (*p >= '0' && *p <= '9') {
            parts[idx] = parts[idx] * 10u + (uint32_t)(*p - '0');
            if (parts[idx] > 255u) {
                return 0;
            }
            ++digits;
        } else if (*p == '.') {
            if (digits == 0 || idx == 3) {
                return 0;
            }
            ++idx;
            digits = 0;
        } else {
            return 0;
        }
        ++p;
    }
    if (idx != 3 || digits == 0) {
        return 0;
    }
    *out_ip = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return 1;
}
