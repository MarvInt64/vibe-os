static inline char *basename(char *p) { char *q=p; while(*q)q++; while(q>p&&q[-1]!='/')q--; return q; }
static inline char *dirname(char *p) { static char d[2]="."; (void)p; return d; }
