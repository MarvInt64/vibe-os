#define CODESET 0
static inline char *nl_langinfo(int i) { (void)i; return (char*)"UTF-8"; }
