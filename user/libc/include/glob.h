#define GLOB_NOSORT 4
typedef struct { size_t gl_pathc; char **gl_pathv; } glob_t;
static inline int glob(const char *p, int f, void *e, glob_t *g) { (void)p;(void)f;(void)e;(void)g; return GLOB_NOSORT; }
static inline void globfree(glob_t *g) { (void)g; }
