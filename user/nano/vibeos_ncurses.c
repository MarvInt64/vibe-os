/* vibeos_ncurses.c — Thin ncurses replacement for nano on VibeOS. */
#include "vibeos_ncurses.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdlib.h>

#define SCR_ROWS 60
#define SCR_COLS 200

static char     scr_chars[SCR_ROWS][SCR_COLS];
static int      scr_rows = 25, scr_cols = 80;
static int      cur_y = 0, cur_x = 0;
static int      initialized = 0;

#define KEYQ_SIZE 64
static int  keyq[KEYQ_SIZE];
static int  keyq_head = 0, keyq_tail = 0;

struct _win_data { int y0, x0, h, w, cur_y, cur_x; };
static struct _win_data win_pool[8];
static int win_count = 0;
static struct _win_data *stdwin = 0;
WINDOW *stdscr = 0;

void nc_push_key(int k) {
    int next = (keyq_head + 1) % KEYQ_SIZE;
    if (next != keyq_tail) { keyq[keyq_head] = k; keyq_head = next; }
}
static int nc_pop_key(void) {
    if (keyq_tail == keyq_head) return -1;
    int k = keyq[keyq_tail];
    keyq_tail = (keyq_tail + 1) % KEYQ_SIZE;
    return k;
}

WINDOW *initscr(void) {
    if (initialized) return stdscr;
    memset(scr_chars, ' ', sizeof(scr_chars));
    initialized = 1;
    win_pool[0].y0=0; win_pool[0].x0=0; win_pool[0].h=scr_rows; win_pool[0].w=scr_cols;
    win_pool[0].cur_y=0; win_pool[0].cur_x=0;
    win_count=1; stdwin=&win_pool[0]; stdscr=(WINDOW*)stdwin;
    return stdscr;
}
int endwin(void) { initialized = 0; return 0; }
WINDOW *newwin(int h, int w, int y, int x) {
    if (win_count>=8) return 0;
    struct _win_data *wd=&win_pool[win_count++];
    wd->y0=y; wd->x0=x; wd->h=h; wd->w=w; wd->cur_y=0; wd->cur_x=0;
    return (WINDOW*)wd;
}
int delwin(WINDOW *w) { (void)w; return 0; }
WINDOW *subwin(WINDOW *orig, int h, int w, int y, int x) {
    if (!orig||win_count>=8) return 0;
    struct _win_data *od=(struct _win_data*)orig;
    struct _win_data *wd=&win_pool[win_count++];
    wd->y0=od->y0+y; wd->x0=od->x0+x; wd->h=h; wd->w=w; wd->cur_y=0; wd->cur_x=0;
    return (WINDOW*)wd;
}
int wmove(WINDOW *w, int y, int x) {
    struct _win_data *wd=(struct _win_data*)w;
    if(y>=0&&y<wd->h) wd->cur_y=y;
    if(x>=0&&x<wd->w) wd->cur_x=x;
    return 0;
}
int curs_set(int v) { (void)v; return 0; }
static void put_char_at(int sy, int sx, int c, uint32_t attr) {
    if(sy>=0&&sy<SCR_ROWS&&sx>=0&&sx<SCR_COLS){ scr_chars[sy][sx]=(char)c; }
}
int mvwaddstr(WINDOW *w, int y, int x, const char *s) {
    struct _win_data *wd=(struct _win_data*)w; int sx=wd->x0+x;
    while(*s){ put_char_at(wd->y0+y, sx, *s, 0); ++sx; ++s; }
    return 0;
}
int mvwaddnstr(WINDOW *w, int y, int x, const char *s, int n) {
    struct _win_data *wd=(struct _win_data*)w; int sx=wd->x0+x, i;
    for(i=0;i<n&&s[i];++i) put_char_at(wd->y0+y, sx+i, s[i], 0);
    return 0;
}
int mvwaddch(WINDOW *w, int y, int x, int c) {
    struct _win_data *wd=(struct _win_data*)w;
    put_char_at(wd->y0+y, wd->x0+x, c, 0);
    return 0;
}
int mvwprintw(WINDOW *w, int y, int x, const char *fmt, ...) {
    char buf[512]; va_list ap; va_start(ap,fmt); vsnprintf(buf,sizeof(buf),fmt,ap); va_end(ap);
    return mvwaddstr(w,y,x,buf);
}

int wgetch(WINDOW *w) {
    (void)w;
    int k = nc_pop_key();
    if (k >= 0) return k;
    unsigned char c;
    if (read(0, &c, 1) == 1) {
        if (c == '\r' || c == '\n') return KEY_ENTER;
        if (c == '\b' || c == 0x7f) return KEY_BACKSPACE;
        if (c == '\t') return '\t';
        if (c == 0x1b) {
            unsigned char seq[8]; int n = 0;
            while (n < 6) { ssize_t r = read(0, &seq[n], 1); if (r <= 0) break; ++n;
                if (seq[0]=='[' && n>=2 && seq[n-1]>='A' && seq[n-1]<='Z') break;
                if (seq[0]=='[' && n>=2 && seq[n-1]>='a' && seq[n-1]<='z') break;
                if (n==1 && seq[0]!='[') break;
            }
            if (n==0) return 0x1b;
            if (seq[0]=='[' && n>=2) {
                switch(seq[n-1]) {
                    case 'A': return KEY_UP; case 'B': return KEY_DOWN;
                    case 'C': return KEY_RIGHT; case 'D': return KEY_LEFT;
                    case 'H': return KEY_HOME; case 'F': return KEY_END;
                }
            }
            if (n==1 && seq[0]>='a' && seq[0]<='z') return (int)(seq[0]-'a'+1);
            return 0x1b;
        }
        if (c < 0x20) return c;
        return (int)c;
    }
    sched_yield_();
    return -1;
}
int ungetch(int c) { keyq_tail=(keyq_tail-1+KEYQ_SIZE)%KEYQ_SIZE; keyq[keyq_tail]=c; return 0; }
int keypad(WINDOW *w, int bf)    { (void)w;(void)bf; return 0; }
int nodelay(WINDOW *w, int bf)   { (void)w;(void)bf; return 0; }
int meta(WINDOW *w, int bf)      { (void)w;(void)bf; return 0; }
int raw(void)                     { return 0; }
int cbreak(void)                  { return 0; }
int noecho(void)                  { return 0; }
int echo_func(void)               { return 0; }
int nl(void)                      { return 0; }
int nonl(void)                    { return 0; }
int wattron(WINDOW *w, int attrs)  { (void)w;(void)attrs; return 0; }
int wattroff(WINDOW *w, int attrs) { (void)w;(void)attrs; return 0; }
int waddch(WINDOW *w, int c)       { (void)w;(void)c; return 0; }
int wstandout(WINDOW *w)  { (void)w; return 0; }
int wstandend(WINDOW *w)  { (void)w; return 0; }
int wrefresh(WINDOW *w)       { (void)w; return 0; }
int wnoutrefresh(WINDOW *w)   { (void)w; return 0; }
int doupdate(void)            { return 0; }
int scrollok(WINDOW *w, int bf) { (void)w;(void)bf; return 0; }
int start_color(void)                     { return 0; }
int init_pair(int p, int fg, int bg)      { (void)p;(void)fg;(void)bg; return 0; }
int has_colors(void)                      { return 0; }
int use_default_colors(void)              { return 0; }
int assume_default_colors(int fg, int bg) { (void)fg;(void)bg; return 0; }
unsigned long BUTTON1_CLICKED=0,BUTTON1_DOUBLE_CLICKED=0,BUTTON1_PRESSED=0,BUTTON1_RELEASED=0;
int mousemask(unsigned long m, unsigned long *o) { (void)m; if(o)*o=0; return 0; }
int getmouse(void *e)                            { (void)e; return 0; }
int getmaxy(WINDOW *w) { return w?((struct _win_data*)w)->h:scr_rows; }
int getmaxx(WINDOW *w) { return w?((struct _win_data*)w)->w:scr_cols; }
int getbegy(WINDOW *w) { return w?((struct _win_data*)w)->y0:0; }
int getbegx(WINDOW *w) { return w?((struct _win_data*)w)->x0:0; }
int COLS=80, LINES=25;
WINDOW *curscr=0;
char *tgetstr(const char *id, char **area) { (void)id;(void)area; return 0; }
int isendwin(void)              { return !initialized; }
int halfdelay(int t)            { (void)t; return 0; }
int napms(int ms)               { (void)ms; return 0; }
int wredrawln(WINDOW *w, int b, int n) { (void)w;(void)b;(void)n; return 0; }
int waddstr(WINDOW *w, const char *s) {
    struct _win_data *wd=(struct _win_data*)w; int x=wd->cur_x;
    while(*s){ put_char_at(wd->y0+wd->cur_y, wd->x0+x, *s, 0); ++x; ++s; }
    wd->cur_x=x; return 0;
}
int waddnstr(WINDOW *w, const char *s, int n) {
    struct _win_data *wd=(struct _win_data*)w; int x=wd->cur_x, i;
    for(i=0;i<n&&s[i];++i) put_char_at(wd->y0+wd->cur_y, wd->x0+x+i, s[i], 0);
    wd->cur_x=x+i; return 0;
}
int wscrl(WINDOW *w, int n) { (void)w;(void)n; return 0; }
int nc_get_rows(void) { return scr_rows; }
int nc_get_cols(void) { return scr_cols; }
char nc_get_char(int r, int c) { return (r>=0&&r<SCR_ROWS&&c>=0&&c<SCR_COLS)?scr_chars[r][c]:' '; }
uint32_t nc_get_attr(int r, int c) { (void)r;(void)c; return 0; }
