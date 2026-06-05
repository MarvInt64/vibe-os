#ifndef VIBEOS_NCURSES_H
#define VIBEOS_NCURSES_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct { int h, w, y, x; } WINDOW;

#define KEY_UP          0x103
#define KEY_DOWN        0x104
#define KEY_LEFT        0x105
#define KEY_RIGHT       0x106
#define KEY_HOME        0x107
#define KEY_END         0x108
#define KEY_NPAGE       0x109
#define KEY_PPAGE       0x10A
#define KEY_DC          0x10B
#define KEY_IC          0x10C
#define KEY_ENTER       0x10D
#define KEY_BACKSPACE   0x10E
#define KEY_BTAB        0x10F
#define KEY_F(n)        (0x110 + (n) - 1)
#define KEY_F0          0x110
#define KEY_CANCEL      0x201
#define KEY_SLEFT       0x120
#define KEY_SRIGHT      0x121
#define KEY_SDC         0x122
#define KEY_SIC         0x123
#define KEY_SHOME       0x124
#define KEY_SEND        0x125
#define KEY_SNEXT       0x126
#define KEY_SPREVIOUS   0x127
#define KEY_RESIZE      0x200
#define KEY_A1          0x202
#define KEY_A3          0x203
#define KEY_C1          0x204
#define KEY_C3          0x205
#define KEY_SCANCEL     0x206
#define KEY_SSUSPEND    0x207
#define KEY_SUSPEND     0x208
#define KEY_SBEG        0x209
#define KEY_BEG         0x20A
#define KEY_B2          0x20B
#define ERR             (-1)

#define COLOR_BLACK   0
#define COLOR_RED     1
#define COLOR_GREEN   2
#define COLOR_YELLOW  3
#define COLOR_BLUE    4
#define COLOR_MAGENTA 5
#define COLOR_CYAN    6
#define COLOR_WHITE   7

#define A_NORMAL    0x00000000u
#define A_REVERSE   0x00000001u
#define A_BOLD      0x00000002u
#define A_UNDERLINE 0x00000004u
#define COLOR_PAIR(n) ((n) << 8)

extern WINDOW *stdscr;
WINDOW *initscr(void);
int     endwin(void);
WINDOW *newwin(int h, int w, int y, int x);
int     delwin(WINDOW *w);
WINDOW *subwin(WINDOW *orig, int h, int w, int y, int x);

int mvwaddstr(WINDOW *w, int y, int x, const char *s);
int mvwaddnstr(WINDOW *w, int y, int x, const char *s, int n);
int mvwaddch(WINDOW *w, int y, int x, int c);
int mvwprintw(WINDOW *w, int y, int x, const char *fmt, ...);

int wattron(WINDOW *w, int attrs);
int wattroff(WINDOW *w, int attrs);
int waddch(WINDOW *w, int c);
int wstandout(WINDOW *w);
int wstandend(WINDOW *w);

int  wgetch(WINDOW *w);
int  ungetch(int c);
int  keypad(WINDOW *w, int bf);
int  nodelay(WINDOW *w, int bf);
int  meta(WINDOW *w, int bf);
int  raw(void);
int  cbreak(void);
int  noecho(void);
int  echo_func(void);
int  nl(void);
int  nonl(void);

int wmove(WINDOW *w, int y, int x);
int curs_set(int visibility);
int wrefresh(WINDOW *w);
int wnoutrefresh(WINDOW *w);
int doupdate(void);
int scrollok(WINDOW *w, int bf);
int start_color(void);
int init_pair(int pair, int fg, int bg);
int has_colors(void);
int use_default_colors(void);
int assume_default_colors(int fg, int bg);
int mousemask(unsigned long mask, unsigned long *old);
int getmouse(void *event);
extern unsigned long BUTTON1_CLICKED;
extern unsigned long BUTTON1_DOUBLE_CLICKED;
extern unsigned long BUTTON1_PRESSED;
extern unsigned long BUTTON1_RELEASED;

extern int COLS;
extern int LINES;
extern WINDOW *curscr;

int getmaxy(WINDOW *w);
int getmaxx(WINDOW *w);
int getbegy(WINDOW *w);
int getbegx(WINDOW *w);

char *tgetstr(const char *id, char **area);

int  isendwin(void);
int  halfdelay(int tenths);
int  napms(int ms);
int  wredrawln(WINDOW *w, int beg, int num);
int  waddstr(WINDOW *w, const char *s);
int  waddnstr(WINDOW *w, const char *s, int n);
int  wscrl(WINDOW *w, int n);

#define getc(fp)  fgetc(fp)
#define beep()    ((void)0)
#define refresh() wrefresh(stdscr)
#define wclrtoeol(w) ((void)(w))
#define define_key(s, k) ((void)(s),(void)(k))
#define typeahead(fd)    ((void)(fd))

/* called from VexUI to push keys, and from impl to get screen state */
void nc_push_key(int k);
int nc_get_rows(void);
int nc_get_cols(void);
char nc_get_char(int r, int c);
uint32_t nc_get_attr(int r, int c);

#ifdef __cplusplus
}
#endif
#endif
