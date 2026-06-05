#ifndef VIBEOS_LOCALE_H
#define VIBEOS_LOCALE_H
#define LC_ALL 0
#define LC_CTYPE 1
static inline char *setlocale(int cat, const char *loc) { (void)cat; (void)loc; return (char*)"C"; }
#endif
