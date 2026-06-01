#ifndef VIBEOS_NET_H
#define VIBEOS_NET_H

#include "types.h"

/* Minimal IPv4 networking over the e1000 NIC: ARP, IPv4, ICMP echo (ping).
 * Static configuration matching QEMU user-mode (SLIRP) networking. */

struct net_info {
    uint8_t up;          /* 1 if a NIC was found and initialised */
    uint8_t mac[6];
    uint32_t ip;         /* host byte order */
    uint32_t netmask;    /* host byte order */
    uint32_t gateway;    /* host byte order */
};

/* Initialise the NIC and the stack. Safe to call once at boot. */
void net_init(void);

/* Drain any received frames and process them (ARP replies/requests,
 * ICMP echo replies/requests). Called from the kernel main loop. */
void net_poll(void);

/* Fill out with the current interface configuration. */
void net_get_info(struct net_info *out);

/* Send `count` ICMP echo requests to the dotted IPv4 address (host order),
 * waiting up to `timeout_ms` for each reply. Returns the number of replies
 * received. rtt_ms_out (optional) receives the last RTT in milliseconds. */
int net_ping(uint32_t dst_ip, int count, int timeout_ms, int *rtt_ms_out);

/* Parse "a.b.c.d" into a host-order IPv4 address. Returns 1 on success. */
int net_parse_ipv4(const char *text, uint32_t *out_ip);

/* Resolve a hostname to an IPv4 address via DNS (UDP to the configured DNS
 * server). Returns 1 and sets *out_ip (host order) on success, 0 on failure. */
int net_resolve(const char *hostname, uint32_t *out_ip, int timeout_ms);

/* Perform a blocking HTTP/1.0 GET over TCP to dst_ip:port. The raw response
 * (headers + body) is written into out (up to out_cap bytes). Returns the
 * number of bytes received (>=0) or negative on error. host_header is sent in
 * the Host: line. */
int net_http_get(uint32_t dst_ip, uint16_t port, const char *host_header,
                 const char *path, char *out, int out_cap, int timeout_ms);

/* ---- Raw TCP stream (single connection; drives the TLS layer) ---- */
int tcp_open(uint32_t dst_ip, uint16_t port, int timeout_ms);   /* 0 ok, <0 error */
int tcp_stream_send(const uint8_t *data, int len);              /* bytes sent, <0 error */
int tcp_stream_recv(uint8_t *buf, int len, int timeout_ms);     /* >0 bytes, 0 EOF, <0 timeout */
void tcp_close(void);

/* Blocking HTTPS GET over TLS (BearSSL) to dst_ip:port. Same contract as
 * net_http_get. NOTE: v1 does NOT validate certificates (accept-all) — it is
 * encrypted but MITM-able. Implemented in net_tls.c. */
int net_https_get(uint32_t dst_ip, uint16_t port, const char *host_header,
                  const char *path, char *out, int out_cap, int timeout_ms);

#endif
