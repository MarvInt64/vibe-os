/* vibeos_nano_main.c — VibeOS entry for GNU nano. */
#ifdef main
#undef main
#endif
extern int nano_main(int argc, char **argv);
int main(int argc, char **argv) { return nano_main(argc, argv); }
