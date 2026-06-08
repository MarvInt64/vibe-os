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

/* net_tls.c — HTTPS via BearSSL, driven over the raw TCP stream in net.c.
 *
 * The whole TLS client lives in the kernel (mirroring net_http_get): BearSSL's
 * I/O callbacks call tcp_stream_send/recv, and net_poll() pumps the NIC.
 *
 * SECURITY (v1): certificates are NOT validated (accept-all). The connection is
 * encrypted but trivially MITM-able. A real trust store (root CAs) is a later
 * step. Entropy comes from RDRAND when available, else an INSECURE TSC seed. */

#include "net.h"
#include "journal.h"
#include "serial.h"
#include "string.h"
#include "timer.h"
#include "types.h"

#include "bearssl.h"

/* ---- entropy ---------------------------------------------------------- */
static int cpu_has_rdrand(void) {
#ifdef ARCH_ARM64
    return 0;
#else
    uint32_t a = 1, b = 0, c = 0, d = 0;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    (void)b; (void)d;
    return (c >> 30) & 1u;
#endif
}
static uint64_t read_tsc(void) {
#ifdef ARCH_ARM64
    return timer_tick_count();
#else
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#endif
}
static int rdrand64(uint64_t *out) {
#ifdef ARCH_ARM64
    (void)out;
    return 0;
#else
    unsigned char ok;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(*out), "=qm"(ok) :: "cc");
    return ok;
#endif
}
static void kgetrandom(unsigned char *buf, int len) {
    static int warned = 0;
    int i;
    if (cpu_has_rdrand()) {
        for (i = 0; i < len; ) {
            uint64_t v = 0; int tries = 0;
            while (!rdrand64(&v) && tries++ < 64) { }
            int c = len - i; if (c > 8) c = 8;
            memcpy(buf + i, &v, (size_t)c);
            i += c;
        }
        return;
    }
    if (!warned) {
        serial_write("NET_TLS: WARNING - no RDRAND, using INSECURE TSC-seeded PRNG\n");
        warned = 1;
    }
    {
        uint64_t s = read_tsc() ^ 0x9e3779b97f4a7c15ULL;
        for (i = 0; i < len; ++i) {
            s ^= s << 13; s ^= s >> 7; s ^= s << 17;
            buf[i] = (unsigned char)(s ^ read_tsc());
        }
    }
}

/* ---- accept-all X.509 engine ----------------------------------------- *
 * Decodes only the end-entity (leaf) certificate to extract its public key
 * (needed for the key exchange), and accepts every chain without validating
 * trust, dates, or hostname. INSECURE by design for v1. */
typedef struct {
    const br_x509_class *vtable;
    br_x509_decoder_context dc;
    int cert_index;
} ax_context;

static void ax_start_chain(const br_x509_class **ctx, const char *server_name) {
    ax_context *c = (ax_context *)(void *)ctx;
    (void)server_name;
    c->cert_index = 0;
}
static void ax_start_cert(const br_x509_class **ctx, uint32_t length) {
    ax_context *c = (ax_context *)(void *)ctx;
    (void)length;
    if (c->cert_index == 0) {
        br_x509_decoder_init(&c->dc, 0, 0);
    }
}
static void ax_append(const br_x509_class **ctx, const unsigned char *buf, size_t len) {
    ax_context *c = (ax_context *)(void *)ctx;
    if (c->cert_index == 0) {
        br_x509_decoder_push(&c->dc, buf, len);
    }
}
static void ax_end_cert(const br_x509_class **ctx) {
    ax_context *c = (ax_context *)(void *)ctx;
    c->cert_index++;
}
static unsigned ax_end_chain(const br_x509_class **ctx) {
    ax_context *c = (ax_context *)(void *)ctx;
    /* Accept regardless of trust; only fail if we couldn't get a usable key. */
    return br_x509_decoder_get_pkey(&c->dc) ? 0 : (unsigned)BR_ERR_X509_NOT_TRUSTED;
}
static const br_x509_pkey *ax_get_pkey(const br_x509_class *const *ctx, unsigned *usages) {
    ax_context *c = (ax_context *)(void *)(uintptr_t)ctx;
    if (usages != 0) {
        *usages = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
    }
    return br_x509_decoder_get_pkey(&c->dc);
}
static const br_x509_class AX_VTABLE = {
    sizeof(ax_context),
    ax_start_chain,
    ax_start_cert,
    ax_append,
    ax_end_cert,
    ax_end_chain,
    ax_get_pkey
};

/* ---- BearSSL contexts (single connection at a time; large -> static) -- */
static br_ssl_client_context  g_sc;
static br_x509_minimal_context g_xc_min;   /* configured by init_full, then overridden */
static ax_context             g_ax;
static br_sslio_context       g_ioc;
/* g_iobuf is handed to BearSSL's engine; bracket it with guard words so we can
 * detect any write past either end (diagnostic for the cross-app crash hunt). */
#define TLS_GUARD_MAGIC 0x544c53475541523Dull  /* "TLSGUAR=" */
static struct {
    uint64_t lo_guard;
    unsigned char buf[BR_SSL_BUFSIZE_BIDI];
    uint64_t hi_guard;
} g_iobuf_s = { TLS_GUARD_MAGIC, {0}, TLS_GUARD_MAGIC };
#define g_iobuf (g_iobuf_s.buf)

/* Checked from net_poll(): logs once if BearSSL ever wrote past the I/O buffer
 * (would corrupt adjacent kernel BSS — a candidate cause of one app killing
 * another during a page load). */
void net_tls_check_guard(void) {
    if (g_iobuf_s.lo_guard != TLS_GUARD_MAGIC || g_iobuf_s.hi_guard != TLS_GUARD_MAGIC) {
        static int reported = 0;
        if (!reported) {
            reported = 1;
            journal_log(JOURNAL_FAULT, 0, "TLS IOBUF GUARD CORRUPTED (BearSSL overflow)");
            journal_log_hex(JOURNAL_FAULT, 0, "  lo_guard=", g_iobuf_s.lo_guard);
            journal_log_hex(JOURNAL_FAULT, 0, "  hi_guard=", g_iobuf_s.hi_guard);
        }
    }
}

static int tls_low_read(void *ctx, unsigned char *buf, size_t len) {
    (void)ctx;
    int n = tcp_stream_recv(buf, (int)len, 8000);
    // serial_write("NET_TLS: low_read n=");
    // serial_write_hex_u64((uint64_t)(int64_t)n);
    // serial_write("\n");
    return (n <= 0) ? -1 : n;
}
static int tls_low_write(void *ctx, const unsigned char *buf, size_t len) {
    (void)ctx;
    // serial_write("NET_TLS: low_write len=");
    // serial_write_hex_u64((uint64_t)len);
    // serial_write("\n");
    int n = tcp_stream_send(buf, (int)len);
    // serial_write("NET_TLS: low_write ret=");
    // serial_write_hex_u64((uint64_t)(int64_t)n);
    // serial_write("\n");
    return (n <= 0) ? -1 : n;
}

/* ---- Connection pool (HTTPS keep-alive) -------------------------------- *
 * A single TLS connection is kept open between requests to the same host so
 * follow-up fetches (e.g. all the images on a page) skip the expensive TLS
 * handshake. Safe because net_lock serializes every HTTPS request, so only one
 * runs at a time and there is exactly one TLS context. */
static int      g_tls_open;
static uint32_t g_tls_ip;
static uint16_t g_tls_port;
static char     g_tls_host[256];

static int str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; ++a; ++b; }
    return *a == *b;
}

/* Tear down the pooled TLS connection (TLS close-notify + TCP FIN). */
static void tls_teardown(void) {
    if (g_tls_open) {
        br_sslio_close(&g_ioc);
        g_tls_open = 0;
    }
    tcp_close();
}

/* Open a fresh TLS connection and perform the handshake. Returns 0 or <0. */
static int tls_connect(uint32_t ip, uint16_t port, const char *host, int timeout_ms) {
    unsigned char seed[64];
    size_t i;

    if (tcp_open(ip, port, timeout_ms) != 0) {
        return -2;
    }
    /* init_full wires up all cipher suites + hashes; we then replace the trust
     * validator with our accept-all engine. */
    br_ssl_client_init_full(&g_sc, &g_xc_min, 0, 0);
    g_ax.vtable = &AX_VTABLE;
    br_ssl_engine_set_x509(&g_sc.eng, &g_ax.vtable);
    br_ssl_engine_set_buffer(&g_sc.eng, g_iobuf, sizeof g_iobuf, 1);
    kgetrandom(seed, sizeof seed);
    br_ssl_engine_inject_entropy(&g_sc.eng, seed, sizeof seed);
    if (!br_ssl_client_reset(&g_sc, host, 0)) {
        tcp_close();
        return -3;
    }
    br_sslio_init(&g_ioc, &g_sc.eng, tls_low_read, 0, tls_low_write, 0);

    g_tls_open = 1;
    g_tls_ip = ip;
    g_tls_port = port;
    for (i = 0; i + 1 < sizeof(g_tls_host) && host[i]; ++i) g_tls_host[i] = host[i];
    g_tls_host[(host) ? i : 0] = '\0';
    return 0;
}

/* Send one keep-alive request over the (already-handshaked) connection and read
 * the response into out, framed by Content-Length so we know the end without
 * the peer closing. Sets *want_close if the response/headers preclude pooling.
 * Returns bytes read, or <0 on a send/handshake error. */
static int tls_request(const char *host_header, const char *path,
                       const char *user_agent, char *out, int out_cap,
                       int *want_close) {
    int total = 0, hdr_end = -1, body_off = -1;
    long content_len = -1;
    *want_close = 0;

    br_sslio_write_all(&g_ioc, "GET ", 4);
    br_sslio_write_all(&g_ioc, path, strlen(path));
    br_sslio_write_all(&g_ioc, " HTTP/1.1\r\nHost: ", 17);
    br_sslio_write_all(&g_ioc, host_header, strlen(host_header));
    if (user_agent && user_agent[0]) {
        br_sslio_write_all(&g_ioc, "\r\nUser-Agent: ", 14);
        br_sslio_write_all(&g_ioc, user_agent, strlen(user_agent));
    }
    br_sslio_write_all(&g_ioc, "\r\nConnection: keep-alive\r\n\r\n", 28);
    if (br_sslio_flush(&g_ioc) != 0) {
        serial_write("NET_TLS: flush/handshake failed, err=");
        serial_write_hex_u64((uint64_t)br_ssl_engine_last_error(&g_sc.eng));
        serial_write("\n");
        return -4;   /* connection unusable (e.g. server closed a pooled conn) */
    }

    for (;;) {
        int rl = br_sslio_read(&g_ioc, (unsigned char *)out + total,
                               (size_t)(out_cap - total));
        if (rl <= 0) { *want_close = 1; break; }   /* peer closed or error */
        total += rl;

        /* Once the header block is in, parse framing. */
        if (hdr_end < 0) {
            int i;
            for (i = 0; i + 3 < total; ++i) {
                if (out[i]=='\r'&&out[i+1]=='\n'&&out[i+2]=='\r'&&out[i+3]=='\n') {
                    hdr_end = i; body_off = i + 4; break;
                }
            }
            if (hdr_end >= 0) {
                /* Content-Length (case-insensitive scan of the header block). */
                int j;
                for (j = 0; j < hdr_end; ++j) {
                    const char *h = out + j;
                    const char *n = "content-length:";
                    int k = 0;
                    while (n[k]) { char a=h[k]; if(a>='A'&&a<='Z')a+=32; if(a!=n[k])break; ++k; }
                    if (!n[k]) {
                        int p = j + 15; long v = 0; int have = 0;
                        while (p < hdr_end && (out[p]==' '||out[p]=='\t')) ++p;
                        while (p < hdr_end && out[p]>='0'&&out[p]<='9') { v=v*10+(out[p]-'0'); ++p; have=1; }
                        if (have) content_len = v;
                        break;
                    }
                }
                /* chunked or no length → cannot frame for keep-alive. */
                if (content_len < 0) *want_close = 1;
            }
        }

        if (content_len >= 0 && body_off >= 0 &&
            (long)(total - body_off) >= content_len) {
            break;   /* full response received; leave connection open */
        }
        if (total >= out_cap) { *want_close = 1; break; }
    }
    return total;
}

int net_https_get(uint32_t dst_ip, uint16_t port, const char *host_header,
                  const char *path, const char *user_agent,
                  char *out, int out_cap, int timeout_ms) {
    int total;
    int want_close = 0;
    int reuse;

    if (out == 0 || out_cap <= 0) {
        return -1;
    }

    /* Reuse a pooled connection to the same host if it is still alive. */
    reuse = (g_tls_open && g_tls_ip == dst_ip && g_tls_port == port &&
             str_eq(g_tls_host, host_header) && tcp_stream_alive());

    if (!reuse) {
        int r;
        if (g_tls_open) tls_teardown();
        r = tls_connect(dst_ip, port, host_header, timeout_ms);
        if (r != 0) return r;
    }

    total = tls_request(host_header, path, user_agent, out, out_cap, &want_close);

    /* A reused connection that the server had silently dropped fails at the
     * first write/read: retry once on a fresh connection. */
    if (reuse && total <= 0) {
        tls_teardown();
        if (tls_connect(dst_ip, port, host_header, timeout_ms) != 0) return -2;
        total = tls_request(host_header, path, user_agent, out, out_cap, &want_close);
    }

    if (want_close || total <= 0) {
        tls_teardown();
    }   /* else: keep the connection pooled for the next same-host request */

    if (total == 0 && br_ssl_engine_last_error(&g_sc.eng) != 0) {
        // serial_write("NET_TLS: handshake/read failed, err=");
        // serial_write_hex_u64((uint64_t)br_ssl_engine_last_error(&g_sc.eng));
        // serial_write("\n");
        return -5;
    }
    return total;
}
