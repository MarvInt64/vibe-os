/*
 * MIT License
 *
 * Copyright (c) 2026 Marvin Kicha
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "net.h"
#include "e1000.h"
#include "process.h"
#include "serial.h"
#include "string.h"
#include "timer.h"

/* ---- Cooperative waiting -------------------------------------------------
 * The network stack blocks for DNS/ARP/TCP. Instead of busy-spinning (which
 * froze the whole desktop during a page load), the wait loops yield the CPU
 * back to the scheduler via process_yield_blocking(): the process is parked on
 * its kernel stack, the main loop keeps polling the NIC and running the
 * desktop + other processes, and we resume here to re-check our condition.
 * With no process context (early boot) we fall back to an inline busy spin. */
static void net_wait_yield(void) {
    if (process_has_current()) {
        process_yield_blocking();
    } else {
        __asm__ volatile("pause");
    }
}

/* ---- Network serialization -----------------------------------------------
 * TCP now has multiple connection slots (g_tcp_conns), so plain HTTP fetches
 * run in parallel on distinct connections. DNS, ping and the single-instance
 * TLS context still share global state; net_lock serializes those callers.
 * It self-heals if the owner dies while parked (e.g. the process was killed
 * mid-request). */
static volatile int      g_net_locked;
static volatile uint32_t g_net_owner_pid;

void net_lock(void) {
    while (g_net_locked) {
        if (g_net_owner_pid != 0 && !process_pid_alive(g_net_owner_pid)) {
            break; /* owner vanished: steal the lock */
        }
        net_poll();
        net_wait_yield();
    }
    g_net_locked = 1;
    g_net_owner_pid = process_current_pid();
}

void net_unlock(void) {
    g_net_owner_pid = 0;
    g_net_locked = 0;
}

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

/* Synchronous TCP client connection. Multiple slots allow concurrent
 * connections (e.g. the browser fetching images in parallel). */
#define NET_MAX_CONNS 4
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
    uint8_t in_use;
    /* HTTP keep-alive: a connection that finished a framed response and may be
     * reused for the next request to the same (remote_ip, remote_port). It
     * stays in_use (occupies its slot) but is available via tcp_find_idle. */
    uint8_t idle;
};
static struct tcp_conn g_tcp_conns[NET_MAX_CONNS];

/* Slot allocation, atomic against concurrent threads. */
static volatile unsigned char g_conn_lock;
static struct tcp_conn *tcp_alloc(void) {
    struct tcp_conn *c = 0;
    int free_slot = -1, idle_slot = -1;
    while (__atomic_test_and_set(&g_conn_lock, __ATOMIC_ACQUIRE)) net_wait_yield();
    for (int i = 0; i < NET_MAX_CONNS; ++i) {
        if (!g_tcp_conns[i].in_use) { if (free_slot < 0) free_slot = i; }
        else if (g_tcp_conns[i].idle && idle_slot < 0) { idle_slot = i; }
    }
    /* Prefer a truly free slot; otherwise evict the first cached idle
     * keep-alive connection (its slot is reclaimed for this new request). */
    int slot = (free_slot >= 0) ? free_slot : idle_slot;
    if (slot >= 0) {
        c = &g_tcp_conns[slot];
        memset(c, 0, sizeof(*c));
        c->in_use = 1;
        c->local_port = (uint16_t)(0xD100u + slot);
        c->state = TCP_CLOSED;
    }
    __atomic_clear(&g_conn_lock, __ATOMIC_RELEASE);
    return c;   /* 0 only if all slots are busy with active (non-idle) requests */
}
static void tcp_free(struct tcp_conn *c) {
    if (c) { c->in_use = 0; c->idle = 0; c->state = TCP_CLOSED; }
}

/* Reuse a cached keep-alive connection to (ip, port) if one is still alive.
 * Returns the connection (marked active, rx sink reset) or 0 if none. */
static struct tcp_conn *tcp_find_idle(uint32_t ip, uint16_t port,
                                      uint8_t *out, int out_cap) {
    struct tcp_conn *c = 0;
    while (__atomic_test_and_set(&g_conn_lock, __ATOMIC_ACQUIRE)) net_wait_yield();
    for (int i = 0; i < NET_MAX_CONNS; ++i) {
        struct tcp_conn *k = &g_tcp_conns[i];
        if (k->in_use && k->idle && k->state == TCP_ESTABLISHED &&
            k->remote_ip == ip && k->remote_port == port) {
            k->idle = 0;
            k->rx_buf = out;
            k->rx_cap = out_cap;
            k->rx_len = 0;
            c = k;
            break;
        }
    }
    __atomic_clear(&g_conn_lock, __ATOMIC_RELEASE);
    return c;
}

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
            net_wait_yield();
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

/* Send a TCP segment for connection c with the given flags and optional payload. */
static int tcp_send(struct tcp_conn *c, uint8_t flags, const uint8_t *payload, uint16_t payload_len) {
    uint8_t *f = g_txframe;
    uint8_t *ip = f + 14;
    uint8_t *tcp = ip + 20;
    uint16_t seg_len = (uint16_t)(20 + payload_len);
    uint16_t ip_total = (uint16_t)(20 + seg_len);
    uint16_t i;

    eth_build_header(f, c->mac, ETHERTYPE_IP);

    wr16(tcp + 0, c->local_port);
    wr16(tcp + 2, c->remote_port);
    wr32(tcp + 4, c->snd_nxt);
    wr32(tcp + 8, (flags & TCP_ACK) ? c->rcv_nxt : 0u);
    tcp[12] = 0x50;          /* data offset 5 (20 bytes), no options */
    tcp[13] = flags;
    /* Advertise the actual free receive-buffer space as the TCP window. This
     * applies real flow control: the peer never sends more than we can buffer,
     * so a slow consumer (e.g. the browser worker starved of CPU by taskmgr)
     * throttles the sender instead of overflowing and losing data. A window of
     * 0 pauses the sender; tcp_stream_recv sends a window-update ACK after it
     * drains, reopening the window. */
    {
        int free_space = c->rx_cap - c->rx_len;
        if (free_space < 0) free_space = 0;
        if (free_space > 0xFFFF) free_space = 0xFFFF;
        wr16(tcp + 14, (uint16_t)free_space);
    }
    wr16(tcp + 16, 0);       /* checksum placeholder */
    wr16(tcp + 18, 0);       /* urgent */
    for (i = 0; i < payload_len; ++i) {
        tcp[20 + i] = payload[i];
    }
    wr16(tcp + 16, tcp_checksum(NET_IP, c->remote_ip, tcp, seg_len));

    ip[0] = 0x45; ip[1] = 0;
    wr16(ip + 2, ip_total);
    wr16(ip + 4, 0);
    wr16(ip + 6, 0x4000);
    ip[8] = 64; ip[9] = IP_PROTO_TCP;
    wr16(ip + 10, 0);
    wr32(ip + 12, NET_IP);
    wr32(ip + 16, c->remote_ip);
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
    struct tcp_conn *c = 0;
    int i;

    /* Demux: find the connection slot this segment belongs to. */
    for (i = 0; i < NET_MAX_CONNS; ++i) {
        struct tcp_conn *cc = &g_tcp_conns[i];
        if (cc->in_use && cc->state != TCP_CLOSED &&
            cc->remote_ip == src_ip && cc->remote_port == src_port &&
            cc->local_port == dst_port) {
            c = cc;
            break;
        }
    }
    if (!c) {
        return;
    }

    if (flags & TCP_RST) {
        c->state = TCP_DONE;
        return;
    }

    if (c->state == TCP_SYN_SENT) {
        if ((flags & TCP_SYN) && (flags & TCP_ACK)) {
            c->rcv_nxt = seq + 1u;
            c->snd_nxt += 1u;            /* our SYN consumed one seq */
            tcp_send(c, TCP_ACK, 0, 0);
            c->state = TCP_ESTABLISHED;
        }
        return;
    }

    /* ESTABLISHED / CLOSING: accept in-order data, ACK it. */
    if (payload_len > 0 && seq == c->rcv_nxt) {
        int copy = payload_len;
        if (c->rx_len + copy > c->rx_cap) {
            copy = c->rx_cap - c->rx_len;
        }
        if (copy > 0) {
            memcpy(c->rx_buf + c->rx_len, payload, (size_t)copy);
            c->rx_len += copy;
        }
        c->rcv_nxt += payload_len;
        tcp_send(c, TCP_ACK, 0, 0);
    }

    if (flags & TCP_FIN) {
        c->rcv_nxt += 1u;
        c->peer_fin = 1;
        tcp_send(c, TCP_ACK | TCP_FIN, 0, 0);
        c->snd_nxt += 1u;
        c->state = TCP_DONE;
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

    net_tls_check_guard();   /* diagnostic: catch any BearSSL I/O-buffer overflow */

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
            net_wait_yield();
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
        net_wait_yield();
    }
    return 0;
}

/* Wait until cond() is true or timeout, polling the NIC. Returns 1 if cond
 * became true, 0 on timeout. (cond checked via the connection state.) */
static int tcp_wait_state(struct tcp_conn *c, int want_min_state, int timeout_ms) {
    uint32_t hz = timer_frequency_hz();
    uint64_t deadline = timer_tick_count() + (uint64_t)timeout_ms * hz / 1000u + 1u;
    uint64_t spins = (uint64_t)timeout_ms * 200000u + 1000000u;

    while (timer_tick_count() < deadline && spins-- > 0) {
        net_poll();
        if (c->state >= want_min_state) {
            return 1;
        }
        net_wait_yield();
    }
    return 0;
}

/* Case-insensitive search for `needle` within the first `n` bytes of `hay`.
 * Returns the offset of the match, or -1. */
static int ci_find(const char *hay, int n, const char *needle) {
    int nl = 0; while (needle[nl]) ++nl;
    if (nl == 0) return 0;
    for (int i = 0; i + nl <= n; ++i) {
        int k = 0;
        while (k < nl) {
            char a = hay[i + k], b = needle[k];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
            ++k;
        }
        if (k == nl) return i;
    }
    return -1;
}

/* HTTP/1.1 response framer for keep-alive. Decides whether buf[0..len) holds a
 * COMPLETE response so we can stop reading without waiting for the peer to
 * close. Sets *want_close=1 when the response is not poolable (Connection:
 * close, HTTP/1.0 without keep-alive, or no determinable body length).
 * Handles Content-Length and Transfer-Encoding: chunked. */
static int http_response_complete(const char *buf, int len, int *want_close) {
    int hdr_end;
    int body_off;
    int i;
    *want_close = 0;

    /* Need the full header block first. */
    for (hdr_end = -1, i = 0; i + 3 < len; ++i) {
        if (buf[i]=='\r'&&buf[i+1]=='\n'&&buf[i+2]=='\r'&&buf[i+3]=='\n') { hdr_end = i; break; }
    }
    if (hdr_end < 0) return 0;          /* headers still arriving */
    body_off = hdr_end + 4;

    /* Connection disposition. HTTP/1.1 defaults to keep-alive; 1.0 to close. */
    {
        int is_10 = (ci_find(buf, (hdr_end < 16 ? hdr_end : 16), "http/1.0") >= 0);
        int hdr_close      = ci_find(buf, hdr_end, "connection: close") >= 0;
        int hdr_keepalive  = ci_find(buf, hdr_end, "connection: keep-alive") >= 0;
        *want_close = hdr_close || (is_10 && !hdr_keepalive);
    }

    /* Transfer-Encoding: chunked — robustly framing chunk boundaries is more
     * than v1 needs (and a naive terminator scan risks truncating + poisoning a
     * pooled connection). Treat chunked as non-poolable: let the body be
     * delimited by connection close, exactly like the old behaviour. */
    if (ci_find(buf, hdr_end, "transfer-encoding: chunked") >= 0) {
        *want_close = 1;
        return 0;
    }

    /* Content-Length: complete once we have header + that many body bytes. */
    {
        int cl = ci_find(buf, hdr_end, "content-length:");
        if (cl >= 0) {
            int j = cl + 15;
            long n = 0; int have = 0;
            while (j < hdr_end && (buf[j]==' '||buf[j]=='\t')) ++j;
            while (j < hdr_end && buf[j] >= '0' && buf[j] <= '9') { n = n*10 + (buf[j]-'0'); ++j; have=1; }
            if (have) return (len - body_off) >= n ? 1 : 0;
        }
    }

    /* No length framing → body is delimited by connection close; not poolable. */
    *want_close = 1;
    return 0;
}

int net_http_get(uint32_t dst_ip, uint16_t port, const char *host_header,
                 const char *path, const char *user_agent,
                 char *out, int out_cap, int timeout_ms) {
    const uint8_t *dst_mac;
    char req[640];
    int rlen = 0;
    uint32_t hz = timer_frequency_hz();
    uint64_t deadline;
    uint64_t spins;
    struct tcp_conn *c;
    int result;
    int reused;
    int want_close = 0;

    if (!g_nic.present || out == 0 || out_cap <= 0) {
        return -1;
    }

    /* Keep-alive: reuse a cached connection to this host if one is alive,
     * skipping DNS/ARP and the TCP handshake entirely. */
    c = tcp_find_idle(dst_ip, port, (uint8_t *)out, out_cap);
    reused = (c != 0);

    if (!reused) {
        c = tcp_alloc();
        if (!c) {
            return -1;
        }
        dst_mac = resolve_mac(dst_ip, 1000);
        if (!dst_mac) {
            tcp_free(c);
            return -2;
        }
        c->state = TCP_SYN_SENT;
        c->remote_ip = dst_ip;
        c->remote_port = port;
        c->snd_nxt = 0x2000u;       /* initial send sequence */
        c->rcv_nxt = 0;
        memcpy(c->mac, dst_mac, 6);
        c->rx_buf = (uint8_t *)out;
        c->rx_cap = out_cap;
        c->rx_len = 0;

        /* Three-way handshake: send SYN, wait for ESTABLISHED. */
        tcp_send(c, TCP_SYN, 0, 0);
        if (!tcp_wait_state(c, TCP_ESTABLISHED, timeout_ms)) {
            tcp_free(c);
            return -3;
        }
    }

    /* Build and send an HTTP/1.1 keep-alive request with the app's User-Agent.
     * HTTP/1.1 + Content-Length / chunked framing lets us pool the connection. */
    {
        int i = 0;
        const char *p;
        #define REQ_PUT(s) do { for (p=(s); *p && i<(int)sizeof(req); ++p) req[i++]=*p; } while (0)
        REQ_PUT("GET ");  REQ_PUT(path);
        REQ_PUT(" HTTP/1.1\r\nHost: "); REQ_PUT(host_header);
        if (user_agent && user_agent[0]) { REQ_PUT("\r\nUser-Agent: "); REQ_PUT(user_agent); }
        REQ_PUT("\r\nConnection: keep-alive\r\n\r\n");
        #undef REQ_PUT
        rlen = i;
    }
    tcp_send(c, TCP_PSH | TCP_ACK, (const uint8_t *)req, (uint16_t)rlen);
    c->snd_nxt += (uint32_t)rlen;

    /* Receive until the response is fully framed (Content-Length / chunked),
     * or the peer closes, or we fill the buffer / time out.
     *
     * The deadline is an IDLE timeout: it is pushed forward whenever new bytes
     * arrive. A transfer that is merely slow — e.g. because another process
     * (taskmgr) is sharing the CPU — must not be truncated; only a genuinely
     * stalled connection times out. */
    {
        int last_len = c->rx_len;
        deadline = timer_tick_count() + (uint64_t)timeout_ms * hz / 1000u + 1u;
        spins = (uint64_t)timeout_ms * 200000u + 1000000u;
        while (timer_tick_count() < deadline && spins-- > 0) {
            net_poll();
            if (http_response_complete((const char *)c->rx_buf, c->rx_len, &want_close))
                break;
            if (c->state == TCP_DONE || c->rx_len >= c->rx_cap) {
                want_close = 1;   /* close-delimited or buffer full → cannot pool */
                break;
            }
            if (c->rx_len != last_len) {     /* progress → reset the idle clock */
                last_len = c->rx_len;
                deadline = timer_tick_count() + (uint64_t)timeout_ms * hz / 1000u + 1u;
                spins = (uint64_t)timeout_ms * 200000u + 1000000u;
            }
            net_wait_yield();
        }
    }

    result = c->rx_len;

    /* Pool the connection for the next request to this host when the response
     * was cleanly framed and neither side asked to close; otherwise tear down. */
    if (!want_close && c->state == TCP_ESTABLISHED) {
        c->idle = 1;        /* stays in_use, reusable via tcp_find_idle */
        /* Detach the caller's buffer: it may be freed after we return, and a
         * stray server segment must not write into it. tcp_find_idle re-points
         * rx_buf/rx_cap on reuse; until then handle_tcp sees rx_cap 0 and drops
         * any unexpected payload safely. */
        c->rx_buf = 0;
        c->rx_cap = 0;
        c->rx_len = 0;
        return result;
    }

    /* Close down: if still established, send FIN. */
    if (c->state == TCP_ESTABLISHED) {
        tcp_send(c, TCP_FIN | TCP_ACK, 0, 0);
        c->snd_nxt += 1u;
    }

    result = c->rx_len;
    tcp_free(c);
    return result;
}

/* ---- Raw TCP stream API (used by the TLS layer in net_tls.c) ----
 * Split into open / send / recv / close so an external state machine
 * (BearSSL) can drive I/O. The TLS layer is single-instance and serialized
 * by net_lock in process.c; it gets its own slot via tcp_alloc so it never
 * collides with parallel net_http_get connections.
 * Received bytes land in g_tcp_stream_rx; tcp_stream_recv() drains them. */
static uint8_t g_tcp_stream_rx[32768];
static struct tcp_conn *g_stream_conn;

int tcp_open(uint32_t dst_ip, uint16_t port, int timeout_ms) {
    const uint8_t *dst_mac;
    struct tcp_conn *c;

    if (!g_nic.present) {
        return -1;
    }
    c = tcp_alloc();
    if (!c) {
        return -1;
    }
    g_stream_conn = c;

    dst_mac = resolve_mac(dst_ip, 1000);
    if (!dst_mac) {
        tcp_free(c);
        g_stream_conn = 0;
        return -2;
    }
    c->state = TCP_SYN_SENT;
    c->remote_ip = dst_ip;
    c->remote_port = port;
    c->snd_nxt = 0x2000u;
    c->rcv_nxt = 0;
    memcpy(c->mac, dst_mac, 6);
    c->rx_buf = g_tcp_stream_rx;
    c->rx_cap = (int)sizeof(g_tcp_stream_rx);
    c->rx_len = 0;

    tcp_send(c, TCP_SYN, 0, 0);
    if (!tcp_wait_state(c, TCP_ESTABLISHED, timeout_ms)) {
        tcp_free(c);
        g_stream_conn = 0;
        return -3;
    }
    return 0;
}

int tcp_stream_send(const uint8_t *data, int len) {
    struct tcp_conn *c = g_stream_conn;
    int off = 0;
    if (c == 0 || data == 0 || len < 0) {
        return -1;
    }
    while (off < len) {
        int chunk = len - off;
        if (chunk > 1400) chunk = 1400;
        tcp_send(c, TCP_PSH | TCP_ACK, data + off, (uint16_t)chunk);
        c->snd_nxt += (uint32_t)chunk;
        off += chunk;
        net_poll();   /* pick up ACKs / incoming data between segments */
    }
    return len;
}

int tcp_stream_recv(uint8_t *buf, int len, int timeout_ms) {
    struct tcp_conn *c = g_stream_conn;
    uint32_t hz = timer_frequency_hz();
    uint64_t deadline = timer_tick_count() + (uint64_t)timeout_ms * hz / 1000u + 1u;
    uint64_t spins = (uint64_t)timeout_ms * 200000u + 1000000u;

    if (c == 0 || buf == 0 || len <= 0) {
        return -1;
    }
    for (;;) {
        net_poll();
        if (c->rx_len > 0) {
            int was = c->rx_len;
            int n = c->rx_len;
            if (n > len) n = len;
            memcpy(buf, c->rx_buf, (size_t)n);
            if (c->rx_len - n > 0) {
                memmove(c->rx_buf, c->rx_buf + n, (size_t)(c->rx_len - n));
            }
            c->rx_len -= n;
            /* If the buffer had filled up (sender may be window-blocked), send a
             * window-update ACK now that we freed space, so the peer resumes. */
            if (c->state == TCP_ESTABLISHED && was >= c->rx_cap - 2048) {
                tcp_send(c, TCP_ACK, 0, 0);
            }
            return n;
        }
        if (c->state == TCP_DONE) {
            return 0;   /* peer closed, nothing buffered */
        }
        if (timer_tick_count() >= deadline || spins-- == 0) {
            return -1;  /* timeout */
        }
        net_wait_yield();
    }
}

void tcp_close(void) {
    struct tcp_conn *c = g_stream_conn;
    if (c == 0) {
        return;
    }
    if (c->state == TCP_ESTABLISHED) {
        tcp_send(c, TCP_FIN | TCP_ACK, 0, 0);
        c->snd_nxt += 1u;
    }
    tcp_free(c);
    g_stream_conn = 0;
}

/* True if the TLS stream connection is still open and usable for another
 * request (used by net_tls.c to decide whether to reuse a pooled connection).
 * net_poll() first so a server-side FIN that just arrived is reflected. */
int tcp_stream_alive(void) {
    net_poll();
    return g_stream_conn != 0 && g_stream_conn->state == TCP_ESTABLISHED;
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
