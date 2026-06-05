#ifndef VIBEOS_FENV_H
#define VIBEOS_FENV_H

#ifdef __cplusplus
extern "C" {
#endif

#define FE_DOWNWARD     0x400
#define FE_TONEAREST    0x000
#define FE_TOWARDZERO   0xc00
#define FE_UPWARD       0x800

static inline int fesetround(int round) { (void)round; return 0; }
static inline int fegetround(void) { return FE_TONEAREST; }

#ifdef __cplusplus
}
#endif

#endif /* VIBEOS_FENV_H */
