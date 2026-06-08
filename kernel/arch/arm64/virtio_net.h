#ifndef VIBEOS_ARM64_VIRTIO_NET_H
#define VIBEOS_ARM64_VIRTIO_NET_H

#include <stdint.h>

int virtio_net_init(void);
int virtio_net_ready(void);
const uint8_t *virtio_net_mac(void);
int virtio_net_transmit(const void *frame, uint16_t length);
int virtio_net_poll_receive(void *out_buffer, uint16_t out_capacity);

#endif
