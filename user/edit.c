/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Marvin Kicha <https://github.com/MarvInt64/> */

/* VibeOS Edit — full-screen text editor */
#define SYS_READ       0
#define SYS_WRITE      1
#define SYS_IOCTL      2
#define SYS_EXIT       4
#define SYS_OPEN       7
#define SYS_CLOSE      8
#define SYS_GETARG     15
#define SYS_WRITE_FILE 16

#define TTY_IOCTL_CLEAR 1

typedef unsigned long  size_t;
typedef long           ssize_t;
typedef unsigned long  uint64_t;
typedef unsigned int   uint32_t;
typedef unsigned char  uint8_t;

static inline ssize_t syscall3(uint64_t n, uint64_t a0, uint64_t a1, uint64_t a2) {
    ssize_t r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a0), "S"(a1), "d"(a2) : "rcx", "r11", "memory");
    return r;
}
static inline ssize_t syscall2(uint64_t n, uint64_t a0, uint64_t a1) {
    ssize_t r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a0), "S"(a1) : "rcx", "r11", "memory");
    return r;
}
static inline ssize_t syscall1(uint64_t n, uint64_t a0) {
    ssize_t r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a0) : "rcx", "r11", "memory");
    return r;
}
static inline void syscall1v(uint64_t n, uint64_t a0) {
    __asm__ volatile("int $0x80" : : "a"(n), "D"(a0) : "rcx", "r11", "memory");
}

/* ── I/O helpers ───────────────────────────────────────────────── */

static void write_buf(const char *s, size_t len) {
    syscall3(SYS_WRITE, 1, (uint64_t)(size_t)s, len);
}
static void write_str(const char *s) {
    size_t l = 0; while (s[l]) l++;
    write_buf(s, l);
}
static void write_char(char c) { write_buf(&c, 1); }
static ssize_t read_char(char *buf, size_t n) {
    return syscall3(SYS_READ, 0, (uint64_t)(size_t)buf, n);
}

/* ── Number → decimal string ───────────────────────────────────── */
static void write_uint(uint32_t n) {
    char b[12]; int i = 0;
    if (n == 0) { write_char('0'); return; }
    while (n) { b[i++] = '0' + (n % 10); n /= 10; }
    while (i--) write_char(b[i]);
}

/* ── ANSI helpers ──────────────────────────────────────────────── */
static void cursor_move(int row, int col) { /* 1-based */
    write_str("\x1b[");
    write_uint((uint32_t)row);
    write_char(';');
    write_uint((uint32_t)col);
    write_char('H');
}
static void clear_screen(void) { write_str("\x1b[2J\x1b[H"); }
static void clear_eol(void)    { write_str("\x1b[K"); }
static void attr_rev(void)     { write_str("\x1b[7m"); }
static void attr_reset(void)   { write_str("\x1b[0m"); }
static void hide_cursor(void)  { write_str("\x1b[?25l"); }
static void show_cursor(void)  { write_str("\x1b[?25h"); }

/* ── Editor state ──────────────────────────────────────────────── */
#define MAX_LINES    256
#define MAX_LINE_LEN 160
#define SCREEN_ROWS   22   /* content rows (rows 3..24) */
#define SCREEN_COLS   78

static char g_lines[MAX_LINES][MAX_LINE_LEN];
static int  g_lens[MAX_LINES];
static int  g_num_lines  = 1;
static int  g_cur_line   = 0;
static int  g_cur_col    = 0;
static int  g_scroll_top = 0;   /* first visible line index */
static int  g_modified   = 0;
static char g_filename[64] = "";
static char g_status_msg[80] = "";

/* flat I/O buffer (load + save) */
static char g_iobuf[MAX_LINES * MAX_LINE_LEN];

/* ── File I/O ──────────────────────────────────────────────────── */
static void load_file(void) {
    ssize_t n, i;
    int col = 0;

    int fd = (int)syscall1(SYS_OPEN, (uint64_t)(size_t)g_filename);
    if (fd < 0) { g_num_lines = 1; g_lens[0] = 0; g_lines[0][0] = '\0'; return; }

    n = syscall3(SYS_READ, (uint64_t)fd, (uint64_t)(size_t)g_iobuf, sizeof(g_iobuf) - 1);
    syscall1v(SYS_CLOSE, (uint64_t)fd);
    if (n <= 0) { g_num_lines = 1; g_lens[0] = 0; g_lines[0][0] = '\0'; return; }
    g_iobuf[n] = '\0';

    g_num_lines = 0;
    col = 0;
    for (i = 0; i < n && g_num_lines < MAX_LINES - 1; ++i) {
        char c = g_iobuf[i];
        if (c == '\n') {
            g_lines[g_num_lines][col] = '\0';
            g_lens[g_num_lines] = col;
            g_num_lines++;
            col = 0;
        } else if (c != '\r' && col < MAX_LINE_LEN - 1) {
            g_lines[g_num_lines][col++] = c;
        }
    }
    /* last line (no trailing newline) */
    g_lines[g_num_lines][col] = '\0';
    g_lens[g_num_lines] = col;
    if (col > 0 || g_num_lines == 0) g_num_lines++;
    if (g_num_lines == 0) g_num_lines = 1;
}

static void save_file(void) {
    int total = 0, i, j;
    ssize_t r;

    if (!g_filename[0]) {
        write_str("\x1b[24;1H");
        attr_rev(); write_str(" No filename! Use Ctrl+W to set. "); attr_reset();
        return;
    }
    for (i = 0; i < g_num_lines; ++i) {
        for (j = 0; j < g_lens[i]; ++j) {
            if (total < (int)sizeof(g_iobuf) - 1)
                g_iobuf[total++] = g_lines[i][j];
        }
        if (i < g_num_lines - 1 && total < (int)sizeof(g_iobuf) - 1)
            g_iobuf[total++] = '\n';
    }
    r = syscall3(SYS_WRITE_FILE,
                 (uint64_t)(size_t)g_filename,
                 (uint64_t)(size_t)g_iobuf,
                 (uint64_t)total);
    if (r >= 0) {
        g_modified = 0;
        /* build status message */
        {
            const char *msg = "Saved: ";
            int k = 0;
            while (msg[k]) { g_status_msg[k] = msg[k]; k++; }
            const char *fn = g_filename;
            while (*fn && k < 79) { g_status_msg[k++] = *fn++; }
            g_status_msg[k] = '\0';
        }
    } else {
        const char *msg = "ERROR saving file!";
        int k = 0;
        while (msg[k] && k < 79) { g_status_msg[k] = msg[k]; k++; }
        g_status_msg[k] = '\0';
    }
}

/* ── Drawing ───────────────────────────────────────────────────── */
static void draw_header(void) {
    cursor_move(1, 1); attr_rev();
    write_str("  VibeOS Edit  |  ");
    write_str(g_filename[0] ? g_filename : "(new file)");
    if (g_modified) write_str("  [modified]");
    write_str("  |  Ctrl+S=Save  Ctrl+W=Filename  Ctrl+Q=Quit  ");
    attr_reset(); clear_eol();
}

static void draw_ruler(void) {
    cursor_move(2, 1); attr_rev();
    write_str("  Line: ");
    write_uint((uint32_t)(g_cur_line + 1));
    write_str("  Col: ");
    write_uint((uint32_t)(g_cur_col + 1));
    write_str("  Lines: ");
    write_uint((uint32_t)g_num_lines);
    write_str("   ");
    attr_reset(); clear_eol();
}

static void draw_status(void) {
    cursor_move(24, 1); attr_rev();
    if (g_status_msg[0]) {
        write_str("  "); write_str(g_status_msg); write_str("  ");
    } else {
        write_str("  Ready  ");
    }
    attr_reset(); clear_eol();
}

static void draw_lines(void) {
    int screen_row, line_idx;
    for (screen_row = 0; screen_row < SCREEN_ROWS; ++screen_row) {
        cursor_move(screen_row + 3, 1);
        clear_eol();
        line_idx = g_scroll_top + screen_row;
        if (line_idx < g_num_lines) {
            int len = g_lens[line_idx];
            int col_start = 0;
            /* horizontal scroll: show SCREEN_COLS chars starting from col_start */
            if (g_cur_col >= SCREEN_COLS) col_start = g_cur_col - SCREEN_COLS + 1;
            if (col_start > 0) { write_char('<'); col_start++; }
            if (len - col_start > SCREEN_COLS - 1)
                write_buf(g_lines[line_idx] + col_start, (size_t)(SCREEN_COLS - 1));
            else if (len > col_start)
                write_buf(g_lines[line_idx] + col_start, (size_t)(len - col_start));
        } else {
            write_char('~');
        }
    }
}

static void draw_all(void) {
    hide_cursor();
    draw_header();
    draw_ruler();
    draw_lines();
    draw_status();
    /* position hardware cursor */
    {
        int col_start = 0;
        int vis_col;
        if (g_cur_col >= SCREEN_COLS) col_start = g_cur_col - SCREEN_COLS + 1;
        vis_col = g_cur_col - col_start + 1;
        if (col_start > 0) vis_col++; /* account for '<' indicator */
        cursor_move(g_cur_line - g_scroll_top + 3, vis_col);
    }
    show_cursor();
}

/* ── Editing operations ────────────────────────────────────────── */
static void clamp_col(void) {
    if (g_cur_col > g_lens[g_cur_line]) g_cur_col = g_lens[g_cur_line];
}

static void scroll_to_cursor(void) {
    if (g_cur_line < g_scroll_top) g_scroll_top = g_cur_line;
    if (g_cur_line >= g_scroll_top + SCREEN_ROWS) g_scroll_top = g_cur_line - SCREEN_ROWS + 1;
}

static void insert_char(char c) {
    int i;
    if (g_cur_line >= MAX_LINES) return;
    if (g_lens[g_cur_line] >= MAX_LINE_LEN - 1) return;
    for (i = g_lens[g_cur_line]; i > g_cur_col; --i)
        g_lines[g_cur_line][i] = g_lines[g_cur_line][i - 1];
    g_lines[g_cur_line][g_cur_col++] = c;
    g_lens[g_cur_line]++;
    g_lines[g_cur_line][g_lens[g_cur_line]] = '\0';
    g_modified = 1;
}

static void delete_backward(void) {
    int i;
    if (g_cur_col > 0) {
        g_cur_col--;
        for (i = g_cur_col; i < g_lens[g_cur_line]; ++i)
            g_lines[g_cur_line][i] = g_lines[g_cur_line][i + 1];
        g_lens[g_cur_line]--;
        g_modified = 1;
    } else if (g_cur_line > 0) {
        /* merge with previous line */
        int prev = g_cur_line - 1;
        int plen = g_lens[prev];
        int clen = g_lens[g_cur_line];
        if (plen + clen < MAX_LINE_LEN - 1) {
            for (i = 0; i < clen; ++i)
                g_lines[prev][plen + i] = g_lines[g_cur_line][i];
            g_lines[prev][plen + clen] = '\0';
            g_lens[prev] = plen + clen;
            /* remove current line */
            for (i = g_cur_line; i < g_num_lines - 1; ++i) {
                int j;
                for (j = 0; j <= g_lens[i + 1]; ++j)
                    g_lines[i][j] = g_lines[i + 1][j];
                g_lens[i] = g_lens[i + 1];
            }
            g_num_lines--;
            g_cur_line--;
            g_cur_col = plen;
            g_modified = 1;
        }
    }
}

static void delete_forward(void) {
    int i;
    if (g_cur_col < g_lens[g_cur_line]) {
        for (i = g_cur_col; i < g_lens[g_cur_line]; ++i)
            g_lines[g_cur_line][i] = g_lines[g_cur_line][i + 1];
        g_lens[g_cur_line]--;
        g_modified = 1;
    } else if (g_cur_line < g_num_lines - 1) {
        /* merge next line into this one */
        int nlen = g_lens[g_cur_line + 1];
        int clen = g_lens[g_cur_line];
        if (clen + nlen < MAX_LINE_LEN - 1) {
            for (i = 0; i < nlen; ++i)
                g_lines[g_cur_line][clen + i] = g_lines[g_cur_line + 1][i];
            g_lines[g_cur_line][clen + nlen] = '\0';
            g_lens[g_cur_line] = clen + nlen;
            for (i = g_cur_line + 1; i < g_num_lines - 1; ++i) {
                int j;
                for (j = 0; j <= g_lens[i + 1]; ++j)
                    g_lines[i][j] = g_lines[i + 1][j];
                g_lens[i] = g_lens[i + 1];
            }
            g_num_lines--;
            g_modified = 1;
        }
    }
}

static void insert_newline(void) {
    int i, split;
    if (g_num_lines >= MAX_LINES - 1) return;
    /* shift lines down */
    for (i = g_num_lines; i > g_cur_line + 1; --i) {
        int j;
        for (j = 0; j <= g_lens[i - 1]; ++j) g_lines[i][j] = g_lines[i - 1][j];
        g_lens[i] = g_lens[i - 1];
    }
    split = g_cur_col;
    /* new line = right part */
    {
        int new_len = g_lens[g_cur_line] - split;
        for (i = 0; i < new_len; ++i)
            g_lines[g_cur_line + 1][i] = g_lines[g_cur_line][split + i];
        g_lines[g_cur_line + 1][new_len] = '\0';
        g_lens[g_cur_line + 1] = new_len;
    }
    /* truncate current line */
    g_lines[g_cur_line][split] = '\0';
    g_lens[g_cur_line] = split;
    g_num_lines++;
    g_cur_line++;
    g_cur_col = 0;
    g_modified = 1;
}

/* ── Filename prompt (Ctrl+W) ──────────────────────────────────── */
static void prompt_filename(void) {
    char buf[64];
    int len = 0;
    int i;
    ssize_t n;
    char c;

    cursor_move(24, 1); attr_rev();
    write_str("  Filename: "); attr_reset();
    /* pre-fill with current filename */
    for (i = 0; g_filename[i] && i < 63; ++i) { buf[i] = g_filename[i]; }
    buf[len = i] = '\0';
    write_str(buf);

    while (1) {
        n = read_char(&c, 1);
        if (n <= 0) continue;
        if (c == '\r' || c == '\n') break;
        if ((c == 0x7f || c == 0x08) && len > 0) {
            buf[--len] = '\0';
            write_str("\x1b[1D \x1b[1D");
        } else if (c == 0x1b) {
            /* abort */
            len = -1; break;
        } else if (c >= 0x20 && c < 0x7f && len < 63) {
            buf[len++] = c;
            buf[len] = '\0';
            write_char(c);
        }
    }
    if (len > 0) {
        for (i = 0; i < len; ++i) g_filename[i] = buf[i];
        g_filename[len] = '\0';
        g_status_msg[0] = '\0';
    }
}

/* ── Main input loop ───────────────────────────────────────────── */
static void run_editor(void) {
    char buf[8];
    ssize_t n;

    draw_all();

    while (1) {
        n = read_char(buf, sizeof(buf));
        if (n <= 0) continue;

        g_status_msg[0] = '\0';

        /* Escape sequence */
        if (buf[0] == 0x1b && n >= 3 && buf[1] == '[') {
            char code = buf[2];
            if (code == 'A') { /* Up */
                if (g_cur_line > 0) { g_cur_line--; clamp_col(); }
            } else if (code == 'B') { /* Down */
                if (g_cur_line < g_num_lines - 1) { g_cur_line++; clamp_col(); }
            } else if (code == 'C') { /* Right */
                if (g_cur_col < g_lens[g_cur_line]) g_cur_col++;
                else if (g_cur_line < g_num_lines - 1) { g_cur_line++; g_cur_col = 0; }
            } else if (code == 'D') { /* Left */
                if (g_cur_col > 0) g_cur_col--;
                else if (g_cur_line > 0) { g_cur_line--; g_cur_col = g_lens[g_cur_line]; }
            } else if (code == 'H') { /* Home */
                g_cur_col = 0;
            } else if (code == 'F') { /* End */
                g_cur_col = g_lens[g_cur_line];
            } else if (code == '5' && n >= 4 && buf[3] == '~') { /* PgUp */
                g_cur_line -= SCREEN_ROWS;
                if (g_cur_line < 0) g_cur_line = 0;
                clamp_col();
            } else if (code == '6' && n >= 4 && buf[3] == '~') { /* PgDn */
                g_cur_line += SCREEN_ROWS;
                if (g_cur_line >= g_num_lines) g_cur_line = g_num_lines - 1;
                clamp_col();
            } else if (code == '3' && n >= 4 && buf[3] == '~') { /* Delete */
                delete_forward();
            }
        } else {
            char c = buf[0];
            if (c == 0x11 || c == 0x03) { /* Ctrl+Q / Ctrl+C */
                if (g_modified) {
                    /* warn once */
                    write_str("\x1b[24;1H");
                    attr_rev();
                    write_str("  Unsaved changes! Press Ctrl+Q again to quit.  ");
                    attr_reset();
                    g_modified = 0; /* second press will quit */
                    scroll_to_cursor();
                    draw_all();
                    continue;
                }
                break;
            } else if (c == 0x13) { /* Ctrl+S */
                save_file();
            } else if (c == 0x17) { /* Ctrl+W — set filename */
                prompt_filename();
            } else if (c == 0x01) { /* Ctrl+A — beginning of line */
                g_cur_col = 0;
            } else if (c == 0x05) { /* Ctrl+E — end of line */
                g_cur_col = g_lens[g_cur_line];
            } else if (c == 0x0b) { /* Ctrl+K — delete to end of line */
                g_lens[g_cur_line] = g_cur_col;
                g_lines[g_cur_line][g_cur_col] = '\0';
                g_modified = 1;
            } else if (c == '\r' || c == '\n') {
                insert_newline();
            } else if (c == 0x7f || c == 0x08) { /* Backspace */
                delete_backward();
            } else if (c >= 0x20 && c < 0x7f) {
                insert_char(c);
            } else if (c == '\t') {
                /* insert 4 spaces */
                insert_char(' '); insert_char(' ');
                insert_char(' '); insert_char(' ');
            }
        }
        scroll_to_cursor();
        draw_all();
    }
}

/* ── Entry point ───────────────────────────────────────────────── */
void _start(void) {
    /* Get filename from spawn arg */
    syscall2(SYS_GETARG, (uint64_t)(size_t)g_filename, (uint64_t)sizeof(g_filename));

    /* Init blank buffer */
    g_num_lines = 1;
    g_lens[0] = 0;
    g_lines[0][0] = '\0';

    /* Load file if provided */
    if (g_filename[0]) {
        load_file();
    }

    clear_screen();
    run_editor();

    /* Restore terminal */
    syscall3(SYS_IOCTL, 1, TTY_IOCTL_CLEAR, 0);
    write_str("VibeOS Edit closed.\r\n");

    syscall1v(SYS_EXIT, 0);
}
