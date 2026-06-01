/* net_tls.c — HTTPS via BearSSL, driven over the raw TCP stream in net.c.
 *
 * The whole TLS client lives in the kernel (mirroring net_http_get): BearSSL's
 * I/O callbacks call tcp_stream_send/recv, and net_poll() pumps the NIC.
 *
 * SECURITY (v1): certificates are NOT validated (accept-all). The connection is
 * encrypted but trivially MITM-able. A real trust store (root CAs) is a later
 * step. Entropy comes from RDRAND when available, else an INSECURE TSC seed. */

#include "net.h"
#include "serial.h"
#include "string.h"
#include "types.h"

#include "bearssl.h"

/* ---- entropy ---------------------------------------------------------- */
static int cpu_has_rdrand(void) {
    uint32_t a = 1, b = 0, c = 0, d = 0;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    (void)b; (void)d;
    return (c >> 30) & 1u;
}
static uint64_t read_tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
static int rdrand64(uint64_t *out) {
    unsigned char ok;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(*out), "=qm"(ok) :: "cc");
    return ok;
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
static unsigned char          g_iobuf[BR_SSL_BUFSIZE_BIDI];

static int tls_low_read(void *ctx, unsigned char *buf, size_t len) {
    (void)ctx;
    int n = tcp_stream_recv(buf, (int)len, 8000);
    return (n <= 0) ? -1 : n;
}
static int tls_low_write(void *ctx, const unsigned char *buf, size_t len) {
    (void)ctx;
    int n = tcp_stream_send(buf, (int)len);
    return (n <= 0) ? -1 : n;
}

int net_https_get(uint32_t dst_ip, uint16_t port, const char *host_header,
                  const char *path, const char *user_agent,
                  char *out, int out_cap, int timeout_ms) {
    unsigned char seed[64];
    int total = 0;

    if (out == 0 || out_cap <= 0) {
        return -1;
    }
    if (tcp_open(dst_ip, port, timeout_ms) != 0) {
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

    if (!br_ssl_client_reset(&g_sc, host_header, 0)) {
        tcp_close();
        return -3;
    }
    br_sslio_init(&g_ioc, &g_sc.eng, tls_low_read, 0, tls_low_write, 0);

    /* Send the HTTP/1.0 request. The User-Agent is whatever the calling app
     * supplied; HTTPS connections are not pooled here, so request close. */
    br_sslio_write_all(&g_ioc, "GET ", 4);
    br_sslio_write_all(&g_ioc, path, strlen(path));
    br_sslio_write_all(&g_ioc, " HTTP/1.0\r\nHost: ", 17);
    br_sslio_write_all(&g_ioc, host_header, strlen(host_header));
    if (user_agent && user_agent[0]) {
        br_sslio_write_all(&g_ioc, "\r\nUser-Agent: ", 14);
        br_sslio_write_all(&g_ioc, user_agent, strlen(user_agent));
    }
    br_sslio_write_all(&g_ioc, "\r\nConnection: close\r\n\r\n", 23);
    if (br_sslio_flush(&g_ioc) != 0) {
        tcp_close();
        return -4;
    }

    /* Read the response until close / error / buffer full. */
    for (;;) {
        int rl = br_sslio_read(&g_ioc, (unsigned char *)out + total, (size_t)(out_cap - total));
        if (rl <= 0) {
            break;
        }
        total += rl;
        if (total >= out_cap) {
            break;
        }
    }
    br_sslio_close(&g_ioc);
    tcp_close();

    if (total == 0 && br_ssl_engine_last_error(&g_sc.eng) != 0) {
        serial_write("NET_TLS: handshake/read failed, err=");
        serial_write_hex_u64((uint64_t)br_ssl_engine_last_error(&g_sc.eng));
        serial_write("\n");
        return -5;
    }
    return total;
}
