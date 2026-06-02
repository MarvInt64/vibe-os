/* umalloc - minimal userspace heap for VibeOS apps.
 *
 * VibeOS userspace has no libc and no allocator: vexui works with a single
 * static canvas, and that is fine for widget apps. Anything that builds dynamic
 * data structures (an HTML DOM, parsed CSS, network buffers, a TLS context)
 * needs a real malloc/free. This is that primitive: a first-fit free-list
 * allocator over demand-grown SYS_SBRK heap pages, with coalescing on free.
 * The heap no longer lives in the executable BSS, so each process image only
 * contains code/data/BSS it actually needs at startup.
 *
 * Thread-safe: browser workers and the UI thread share one address space and
 * therefore one heap. */
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
