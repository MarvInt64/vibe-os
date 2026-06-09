/* VibeOS — EDID (Extended Display Identification Data) definitions and parser.
 *
 * EDID is a 128-byte (or 256-byte) block that a display sends over DDC/I2C
 * describing its capabilities: supported resolutions, timings, manufacturer,
 * serial number, etc.  This module parses the base EDID 1.x block and
 * extracts the list of established and detailed timings.
 *
 * Reference: VESA EDID v1.4
 */
#ifndef VIBEOS_EDID_H
#define VIBEOS_EDID_H

#include "types.h"

/* An EDID block is always 128 bytes.  Extension blocks may follow. */
#define EDID_BLOCK_SIZE    128
#define EDID_MAX_EXTENSIONS 4   /* sanity cap */

/* Maximum number of detailed timing descriptors (4 in base block). */
#define EDID_MAX_DTD 4

/* One display mode extracted from EDID. */
struct edid_mode {
    uint16_t hactive;
    uint16_t vactive;
    uint16_t refresh_hz;   /* vertical refresh rate × 100 (e.g. 6000 = 60.00 Hz) */
};

/* Decoded EDID information. */
struct edid_info {
    uint8_t  raw[EDID_BLOCK_SIZE];      /* raw base block */
    uint16_t vendor_id;                  /* compressed 3-letter PnP ID */
    uint16_t product_code;
    uint32_t serial;
    uint8_t  manufacture_week;
    uint16_t manufacture_year;
    uint8_t  edid_version;
    uint8_t  edid_revision;
    uint8_t  max_h_size_cm;              /* physical dimensions */
    uint8_t  max_v_size_cm;

    /* Supported modes from established timings + standard timings + DTDs. */
    struct edid_mode modes[64];
    int     mode_count;

    /* Preferred (native) mode index, or -1. */
    int     preferred_idx;

    /* Display name from descriptor, or empty string. */
    char    display_name[14];
};

/*
 * Parse a raw 128-byte EDID block into an edid_info structure.
 * Returns 0 on success, -1 if the block fails checksum or header validation.
 * This function is platform-independent.
 */
int edid_parse(const uint8_t raw[EDID_BLOCK_SIZE], struct edid_info *out);

#endif /* VIBEOS_EDID_H */
