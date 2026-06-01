/* umalloc — minimal userspace heap for VibeOS apps.
 *
 * VibeOS userspace has no libc and no allocator: vexui works with a single
 * static canvas, and that is fine for widget apps. Anything that builds dynamic
 * data structures (an HTML DOM, parsed CSS, network buffers, a TLS context)
 * needs a real malloc/free. This is that primitive: a first-fit free-list
 * allocator over a fixed static arena, with coalescing on free. No syscalls —
 * the arena lives in the process's BSS (physical pages are allocated by the
 * kernel loader to fit the image's memsz).
 *
 * Not thread-safe (VibeOS apps are single-threaded/cooperative), which is
 * exactly the model here. */
#ifndef VIBEOS_UMALLOC_H
#define VIBEOS_UMALLOC_H

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long umsize_t;

void *umalloc(umsize_t size);
void  ufree(void *ptr);
void *urealloc(void *ptr, umsize_t size);

/* Diagnostics / coarse control. */
umsize_t umalloc_used(void);      /* bytes currently handed out (payload, rounded) */
umsize_t umalloc_capacity(void);  /* total arena size */

#ifdef __cplusplus
}
#endif

#endif
