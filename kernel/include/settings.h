#ifndef VIBEOS_SETTINGS_H
#define VIBEOS_SETTINGS_H

#include "types.h"

/* Settings storage version to handle format changes */
#define SETTINGS_MAGIC 0x56494245u /* "VIBE" */
#define SETTINGS_VERSION 1

struct os_settings {
    uint32_t magic;
    uint32_t version;
    
    /* Audio tuning */
    uint32_t audio_sample_rate;
    uint32_t audio_buffer_size;  /* Bytes per buffer */
    uint32_t dma_buffer_count;   /* Number of buffers in the ring */
    uint32_t master_volume;      /* 0-100 */
    
    /* Reserved for future OS settings (e.g., UI, Network) */
    uint32_t reserved[124];
};

void settings_init(void);
struct os_settings *settings_get(void);
void settings_save(void);

#endif
