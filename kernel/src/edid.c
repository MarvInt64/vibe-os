/* VibeOS — EDID parser (platform-independent).
 *
 * Parses the 128-byte base EDID block and extracts display modes from:
 *   1. Established timings (bits in bytes 35-37)
 *   2. Standard timings (8 × 2-byte entries at bytes 38-53)
 *   3. Detailed timing descriptors (4 × 18-byte blocks at bytes 54-125)
 *
 * See kernel/include/edid.h for the public API.
 */
#include "edid.h"
#include "string.h"

/* Established timing table: bit → {width, height, Hz×100}. */
static const struct edid_mode established_timings[] = {
    /* Byte 35 */
    { 800,  600, 6000},  /* bit 0 */
    { 800,  600, 5600},  /* bit 1 */
    { 640,  480, 7500},  /* bit 2 */
    { 640,  480, 7200},  /* bit 3 */
    { 640,  480, 6700},  /* bit 4 */
    { 640,  480, 6000},  /* bit 5 */
    { 720,  400, 7000},  /* bit 6 */
    { 720,  400, 8800},  /* bit 7 */
    /* Byte 36 */
    {1280, 1024, 7500},  /* bit 0 */
    {1024,  768, 7500},  /* bit 1 */
    {1024,  768, 7000},  /* bit 2 */
    {1024,  768, 6000},  /* bit 3 */
    {1024,  768, 8700},  /* bit 4-7: reserved */
    {   0,    0,    0},
    {   0,    0,    0},
    {   0,    0,    0},
    /* Byte 37 */
    { 832,  624, 7500},  /* bit 0 */
    {1280, 1024, 8500},  /* bit 1 */
    {1280,  960, 8500},  /* bit 2 */
    {1152,  864, 7500},  /* bit 3 */
    {   0,    0,    0},  /* bit 4-7: manufacturer specific */
    {   0,    0,    0},
    {   0,    0,    0},
    {   0,    0,    0},
};

/* Common standard timing resolutions.  Byte value 0x01 = 1280×1024@60 etc.
 * Unlisted values use the GTF/CVT formula. */
static void add_standard_timing(const uint8_t *std, struct edid_info *out) {
    if (std[0] == 0x01 && std[1] == 0x01) return;  /* unused */
    /* Horizontal active = (byte0 + 31) * 8 */
    int hactive = (int)(std[0] + 31) * 8;
    /* Aspect ratio from bits 7-6 of byte1 */
    int ratio = (std[1] >> 6) & 0x03;
    int vactive;
    switch (ratio) {
        case 0: vactive = hactive * 10 / 16; break;  /* 16:10 */
        case 1: vactive = hactive * 3 / 4;   break;  /* 4:3 */
        case 2: vactive = hactive * 4 / 5;   break;  /* 5:4 */
        case 3: vactive = hactive * 9 / 16;  break;  /* 16:9 */
        default: vactive = hactive * 3 / 4; break;
    }
    /* Refresh = (byte1 & 0x3F) + 60 */
    int refresh = ((int)(std[1] & 0x3F) + 60) * 100;
    if (out->mode_count >= 64) return;
    struct edid_mode *m = &out->modes[out->mode_count];
    m->hactive     = (uint16_t)hactive;
    m->vactive     = (uint16_t)vactive;
    m->refresh_hz  = (uint16_t)refresh;
    out->mode_count++;
}

/* Decode a detailed timing descriptor (18 bytes). */
static void add_detailed_timing(const uint8_t *dtd, struct edid_info *out) {
    /* Pixel clock in 10 kHz units (bytes 0-1, little-endian) */
    uint32_t pclk = (uint32_t)dtd[0] | ((uint32_t)dtd[1] << 8);
    if (pclk == 0) return;  /* unused descriptor */

    int hactive = (int)dtd[2] | ((int)(dtd[4] & 0xF0) << 4);
    int hblank  = (int)dtd[3] | ((int)(dtd[4] & 0x0F) << 8);
    int vactive = (int)dtd[5] | ((int)(dtd[7] & 0xF0) << 4);
    int vblank  = (int)dtd[6] | ((int)(dtd[7] & 0x0F) << 8);
    int htotal  = hactive + hblank;
    int vtotal  = vactive + vblank;

    if (hactive <= 0 || vactive <= 0 || htotal <= 0 || vtotal <= 0) return;
    if (out->mode_count >= 64) return;

    /* Refresh = pixel_clock / (htotal * vtotal) in Hz × 100 */
    uint32_t refresh = (pclk * 10000u) / (uint32_t)(htotal * vtotal);

    struct edid_mode *m = &out->modes[out->mode_count];
    m->hactive    = (uint16_t)hactive;
    m->vactive    = (uint16_t)vactive;
    m->refresh_hz = (uint16_t)((refresh + 5) / 10);  /* round to Hz*100 */
    out->mode_count++;
}

/* Check EDID header (bytes 0-7 must be 00 FF FF FF FF FF FF 00). */
static int valid_header(const uint8_t raw[128]) {
    return raw[0] == 0x00 && raw[1] == 0xFF && raw[2] == 0xFF &&
           raw[3] == 0xFF && raw[4] == 0xFF && raw[5] == 0xFF &&
           raw[6] == 0xFF && raw[7] == 0x00;
}

/* Verify EDID checksum: sum of all 128 bytes must be 0 mod 256. */
static int valid_checksum(const uint8_t raw[128]) {
    int sum = 0;
    for (int i = 0; i < 128; i++) sum += raw[i];
    return (sum & 0xFF) == 0;
}

int edid_parse(const uint8_t raw[EDID_BLOCK_SIZE], struct edid_info *out) {
    if (!raw || !out) return -1;
    if (!valid_header(raw)) return -1;
    if (!valid_checksum(raw)) return -1;

    /* Copy raw block */
    for (int i = 0; i < EDID_BLOCK_SIZE; i++)
        out->raw[i] = raw[i];

    out->mode_count    = 0;
    out->preferred_idx = -1;
    out->display_name[0] = '\0';

    /* Vendor / product / serial (bytes 8-15) */
    out->vendor_id = (uint16_t)((raw[8] >> 2) & 0x1F) |
                     (uint16_t)((raw[8] & 0x03) << 14) |
                     (uint16_t)(raw[9] << 7) |
                     (uint16_t)((raw[10] >> 2) << 2);
    out->product_code = (uint16_t)raw[10] | ((uint16_t)raw[11] << 8);
    out->serial = (uint32_t)raw[12] | ((uint32_t)raw[13] << 8) |
                  ((uint32_t)raw[14] << 16) | ((uint32_t)raw[15] << 24);

    /* EDID version (bytes 18-19) */
    out->edid_version  = raw[18];
    out->edid_revision = raw[19];

    /* Physical size (bytes 21-22) */
    out->max_h_size_cm = raw[21];
    out->max_v_size_cm = raw[22];

    /* Manufacture week/year (bytes 16-17) */
    out->manufacture_week  = raw[16];
    out->manufacture_year  = (uint16_t)((int)raw[17] + 1990);

    /* ---- Established timings (bytes 35-37) ---- */
    for (int byte = 0; byte < 3; byte++) {
        uint8_t bits = raw[35 + byte];
        for (int bit = 0; bit < 8; bit++) {
            if (bits & (1 << bit)) {
                const struct edid_mode *et = &established_timings[byte * 8 + bit];
                if (et->hactive == 0) continue;
                if (out->mode_count >= 64) break;
                out->modes[out->mode_count++] = *et;
            }
        }
    }

    /* ---- Standard timings (bytes 38-53, 8 entries of 2 bytes) ---- */
    for (int i = 0; i < 8; i++)
        add_standard_timing(&raw[38 + i * 2], out);

    /* ---- Detailed timing descriptors (bytes 54-125, 4 × 18 bytes) ---- */
    for (int i = 0; i < 4; i++) {
        const uint8_t *dtd = &raw[54 + i * 18];
        /* Check if this is a display descriptor (tag 0x00) */
        if (dtd[0] == 0 && dtd[1] == 0 && dtd[2] == 0 &&
            dtd[3] == 0xFD) {
            /* Monitor range limits — skip */
            continue;
        }
        if (dtd[0] == 0 && dtd[1] == 0 && dtd[2] == 0 &&
            dtd[3] == 0xFC) {
            /* Display name */
            for (int j = 0; j < 13; j++)
                out->display_name[j] = (char)dtd[5 + j];
            out->display_name[13] = '\0';
            /* Trim trailing whitespace/newlines */
            for (int j = 12; j >= 0 && (out->display_name[j] == ' ' ||
                 out->display_name[j] == '\n' || out->display_name[j] == '\0'); j--)
                out->display_name[j] = '\0';
            continue;
        }
        if (dtd[0] == 0 && dtd[1] == 0 && dtd[2] == 0) {
            /* Other descriptor tag — skip */
            continue;
        }
        /* It's a detailed timing descriptor */
        add_detailed_timing(dtd, out);
    }

    /* Preferred mode is the first DTD (if any). */
    if (out->mode_count > 0)
        out->preferred_idx = 0;

    return 0;
}
