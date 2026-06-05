/*
 * MIT License
 *
 * Copyright (c) 2026 Marvin Kicha
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "process.h"
#include "tty.h"
#include "syscall.h"
#include "serial.h"

static void tty_touch(struct tty *tty) {
 ++tty->revision;
}

static void tty_copy_text(char *dest, size_t capacity, const char *src) {
 size_t i = 0;

 if (capacity == 0) {
 return;
 }

 while (src[i] != '\0' && i + 1 < capacity) {
 dest[i] = src[i];
 ++i;
 }
 dest[i] = '\0';
}

static void tty_append_display_line(struct tty *tty, const char *text) {
 size_t i;

 serial_write("TTY_APPEND_LINE: tty=");
 serial_write_hex_u64((uint64_t)(uintptr_t)tty);
 serial_write(" line_count=");
 serial_write_hex_u64(tty->line_count);
 serial_write(" text=");
 serial_write(text);
 serial_write("\n");

 if (tty->line_count < TTY_MAX_LINES) {
 tty_copy_text(tty->lines[tty->line_count], TTY_MAX_LINE_CHARS, text);
 ++tty->line_count;
 tty_touch(tty);
 return;
 }

 for (i = 1; i < TTY_MAX_LINES; ++i) {
 tty_copy_text(tty->lines[i - 1], TTY_MAX_LINE_CHARS, tty->lines[i]);
 }
 tty_copy_text(tty->lines[TTY_MAX_LINES - 1], TTY_MAX_LINE_CHARS, text);
 tty_touch(tty);
}

static void tty_flush_partial_line(struct tty *tty) {
 tty->partial_output[tty->partial_length] = '\0';
 if (tty->partial_length > 0) {
 tty_append_display_line(tty, tty->partial_output);
 }
 tty->partial_length = 0;
 tty->partial_output[0] = '\0';
}

static void tty_raw_input_shift_left(struct tty *tty, size_t count) {
 size_t i;

 if (count >= tty->raw_length) {
 tty->raw_length = 0;
 tty->raw_input[0] = '\0';
 return;
 }

 for (i = count; i < tty->raw_length; ++i) {
 tty->raw_input[i - count] = tty->raw_input[i];
 }
 tty->raw_length -= count;
 tty->raw_input[tty->raw_length] = '\0';
}

static void tty_queue_raw_input(struct tty *tty, const char *data, size_t count) {
 size_t i;

 for (i = 0; i < count && tty->raw_length + 1 < TTY_INPUT_CAPACITY; ++i) {
 tty->raw_input[tty->raw_length++] = data[i];
 }
 tty->raw_input[tty->raw_length] = '\0';
}

static void tty_append_output_char(struct tty *tty, char c) {
 if (c == '\r') {
  return;
 }

 if (c == '\n') {
  tty_flush_partial_line(tty);
  return;
 }

 if (c == '\b') {
  if (tty->partial_length > 0) {
   tty->partial_length--;
   tty->partial_output[tty->partial_length] = '\0';
  }
  return;
 }

 if (tty->partial_length + 1 >= TTY_MAX_LINE_CHARS) {
  tty_flush_partial_line(tty);
 }

 tty->partial_output[tty->partial_length++] = c;
 tty->partial_output[tty->partial_length] = '\0';
}

/* ── ANSI Screen Mode Functions ───────────────────────────────────── */

static void screen_clear(struct tty *tty) {
 int row, col;
 for (row = 0; row < TTY_SCREEN_ROWS; ++row) {
 for (col = 0; col < TTY_SCREEN_COLS; ++col) {
 tty->screen[row][col] = ' ';
 tty->screen_attr[row][col] = TTY_ATTR_NORMAL;
 }
 }
}

static void screen_clear_line(struct tty *tty, int row) {
 int col;
 if (row < 0 || row >= TTY_SCREEN_ROWS) return;
 for (col = 0; col < TTY_SCREEN_COLS; ++col) {
 tty->screen[row][col] = ' ';
 tty->screen_attr[row][col] = TTY_ATTR_NORMAL;
 }
}

static void screen_clear_to_eol(struct tty *tty) {
 int col;
 if (tty->cursor_row < 0 || tty->cursor_row >= TTY_SCREEN_ROWS) return;
 for (col = tty->cursor_col; col < TTY_SCREEN_COLS; ++col) {
 tty->screen[tty->cursor_row][col] = ' ';
 tty->screen_attr[tty->cursor_row][col] = TTY_ATTR_NORMAL;
 }
}

static void screen_scroll_up(struct tty *tty) {
 int row, col;
 for (row = 0; row < TTY_SCREEN_ROWS - 1; ++row) {
 for (col = 0; col < TTY_SCREEN_COLS; ++col) {
 tty->screen[row][col] = tty->screen[row + 1][col];
 tty->screen_attr[row][col] = tty->screen_attr[row + 1][col];
 }
 }
 /* Clear last line */
 for (col = 0; col < TTY_SCREEN_COLS; ++col) {
 tty->screen[TTY_SCREEN_ROWS - 1][col] = ' ';
 tty->screen_attr[TTY_SCREEN_ROWS - 1][col] = TTY_ATTR_NORMAL;
 }
}

static void screen_putc(struct tty *tty, char c) {
 if (tty->cursor_row >= TTY_SCREEN_ROWS) {
 screen_scroll_up(tty);
 tty->cursor_row = TTY_SCREEN_ROWS - 1;
 }
 if (tty->cursor_col >= TTY_SCREEN_COLS) {
 tty->cursor_col = 0;
 tty->cursor_row++;
 if (tty->cursor_row >= TTY_SCREEN_ROWS) {
 screen_scroll_up(tty);
 tty->cursor_row = TTY_SCREEN_ROWS - 1;
 }
 }
 if (tty->cursor_row >= 0 && tty->cursor_col >= 0) {
 tty->screen[tty->cursor_row][tty->cursor_col] = c;
 tty->screen_attr[tty->cursor_row][tty->cursor_col] = tty->current_attr;
 tty->cursor_col++;
 }
}

/* ── ANSI Escape Sequence Parser ──────────────────────────────────── */

static void ansi_reset(struct tty *tty) {
 tty->ansi_state = ANSI_STATE_NORMAL;
 tty->ansi_param_count = 0;
 tty->ansi_cmd = 0;
}

static int ansi_param(struct tty *tty, int index, int default_val) {
 if (index <= tty->ansi_param_count && tty->ansi_params[index] > 0) {
 return tty->ansi_params[index];
 }
 return default_val;
}

static void ansi_execute(struct tty *tty) {
 int params[8];
 int i;

 for (i = 0; i <= tty->ansi_param_count && i < 8; ++i) {
 params[i] = tty->ansi_params[i];
 }
 
 switch (tty->ansi_cmd) {
 case 'H':  /* Cursor Position: ESC[row;colH */
 case 'f':  /* Horizontal and Vertical Position */
  tty->cursor_row = ansi_param(tty, 0, 1) - 1;
  tty->cursor_col = ansi_param(tty, 1, 1) - 1;
  if (tty->cursor_row < 0) tty->cursor_row = 0;
  if (tty->cursor_row >= TTY_SCREEN_ROWS) tty->cursor_row = TTY_SCREEN_ROWS - 1;
  if (tty->cursor_col < 0) tty->cursor_col = 0;
  if (tty->cursor_col >= TTY_SCREEN_COLS) tty->cursor_col = TTY_SCREEN_COLS - 1;
  break;
  
 case 'J':  /* Erase Display */
  if (params[0] == 0) {
   /* Clear from cursor to end of screen */
   screen_clear_to_eol(tty);
   for (i = tty->cursor_row + 1; i < TTY_SCREEN_ROWS; ++i) {
    screen_clear_line(tty, i);
   }
  } else if (params[0] == 1) {
   /* Clear from start to cursor */
   for (i = 0; i < tty->cursor_row; ++i) {
    screen_clear_line(tty, i);
   }
   for (i = 0; i < tty->cursor_col; ++i) {
    tty->screen[tty->cursor_row][i] = ' ';
    tty->screen_attr[tty->cursor_row][i] = TTY_ATTR_NORMAL;
   }
  } else if (params[0] == 2) {
   /* Clear entire screen */
   screen_clear(tty);
  }
  break;
  
 case 'K':  /* Erase Line */
  if (params[0] == 0) {
   /* Clear from cursor to end of line */
   screen_clear_to_eol(tty);
  } else if (params[0] == 1) {
   /* Clear from start to cursor */
   int col;
   for (col = 0; col < tty->cursor_col; ++col) {
    tty->screen[tty->cursor_row][col] = ' ';
    tty->screen_attr[tty->cursor_row][col] = TTY_ATTR_NORMAL;
   }
  } else if (params[0] == 2) {
   /* Clear entire line */
   screen_clear_line(tty, tty->cursor_row);
  }
  break;
  
 case 'A':  /* Cursor Up */
  tty->cursor_row -= ansi_param(tty, 0, 1);
  if (tty->cursor_row < 0) tty->cursor_row = 0;
  break;
  
 case 'B':  /* Cursor Down */
  tty->cursor_row += ansi_param(tty, 0, 1);
  if (tty->cursor_row >= TTY_SCREEN_ROWS) tty->cursor_row = TTY_SCREEN_ROWS - 1;
  break;
  
 case 'C':  /* Cursor Forward */
  tty->cursor_col += ansi_param(tty, 0, 1);
  if (tty->cursor_col >= TTY_SCREEN_COLS) tty->cursor_col = TTY_SCREEN_COLS - 1;
  break;
  
 case 'D':  /* Cursor Back */
  tty->cursor_col -= ansi_param(tty, 0, 1);
  if (tty->cursor_col < 0) tty->cursor_col = 0;
  break;
  
 case 'm':  /* Select Graphic Rendition */
  for (i = 0; i <= tty->ansi_param_count; ++i) {
   int mode = params[i];
   switch (mode) {
   case 0:
    tty->current_attr = TTY_ATTR_NORMAL;
    break;
   case 7:
    tty->current_attr = TTY_ATTR_INVERSE;
    break;
   case 27:
    tty->current_attr = TTY_ATTR_NORMAL;
    break;
   }
  }
  break;
  
 case 's':  /* Save Cursor Position */
  tty->saved_cursor_row = tty->cursor_row;
  tty->saved_cursor_col = tty->cursor_col;
  break;
  
 case 'u':  /* Restore Cursor Position */
  tty->cursor_row = tty->saved_cursor_row;
  tty->cursor_col = tty->saved_cursor_col;
  break;
  
 case 'l':  /* Reset Mode (including cursor visibility) */
 case 'h':  /* Set Mode */
  /* Handle ?25l (hide cursor) and ?25h (show cursor) */
  if (params[0] == 25) {
   /* We don't track cursor visibility separately, but we acknowledge the command */
  }
  break;
 }
 
 ansi_reset(tty);
}

static void tty_process_ansi_char(struct tty *tty, char c) {
 switch (tty->ansi_state) {
 case ANSI_STATE_NORMAL:
  if (c == 0x1b) {
   tty->ansi_state = ANSI_STATE_ESCAPE;
  } else {
   if (c == '\n') {
    tty->cursor_col = 0;
    tty->cursor_row++;
    if (tty->cursor_row >= TTY_SCREEN_ROWS) {
     screen_scroll_up(tty);
     tty->cursor_row = TTY_SCREEN_ROWS - 1;
    }
   } else if (c == '\r') {
    tty->cursor_col = 0;
   } else if (c == '\b' || c == 0x7f) {
    if (tty->cursor_col > 0) {
     tty->cursor_col--;
    }
   } else if (c == '\t') {
    tty->cursor_col = (tty->cursor_col + 4) & ~3;
    if (tty->cursor_col >= TTY_SCREEN_COLS) {
     tty->cursor_col = TTY_SCREEN_COLS - 1;
    }
   } else if (c >= 0x20 && c < 0x7f) {
    screen_putc(tty, c);
   }
  }
  break;
  
 case ANSI_STATE_ESCAPE:
  if (c == '[') {
   tty->ansi_state = ANSI_STATE_BRACKET;
   tty->ansi_param_count = 0;
   tty->ansi_params[0] = 0;
  } else if (c == 'c') {
   /* Reset */
   screen_clear(tty);
   tty->cursor_row = 0;
   tty->cursor_col = 0;
   ansi_reset(tty);
  } else if (c == '7') {
   /* Save cursor */
   tty->saved_cursor_row = tty->cursor_row;
   tty->saved_cursor_col = tty->cursor_col;
   ansi_reset(tty);
  } else if (c == '8') {
   /* Restore cursor */
   tty->cursor_row = tty->saved_cursor_row;
   tty->cursor_col = tty->saved_cursor_col;
   ansi_reset(tty);
  } else if (c == 'M') {
   /* Scroll up (RI - Reverse Index) */
   if (tty->cursor_row == 0) {
    screen_scroll_up(tty);
   } else {
    tty->cursor_row--;
   }
   ansi_reset(tty);
  } else {
   ansi_reset(tty);
  }
  break;
  
 case ANSI_STATE_BRACKET:
  if (c >= '0' && c <= '9') {
   tty->ansi_params[tty->ansi_param_count] = c - '0';
   tty->ansi_state = ANSI_STATE_PARAMS;
  } else if (c == '?') {
   /* Private mode sequence like ?25l */
   tty->ansi_params[0] = 0;
   tty->ansi_state = ANSI_STATE_PARAMS;
  } else if (c == ';') {
   tty->ansi_params[tty->ansi_param_count++] = 0;
   if (tty->ansi_param_count >= 8) {
    ansi_reset(tty);
   }
  } else {
   tty->ansi_cmd = c;
   ansi_execute(tty);
  }
  break;
  
 case ANSI_STATE_PARAMS:
  if (c >= '0' && c <= '9') {
   tty->ansi_params[tty->ansi_param_count] = tty->ansi_params[tty->ansi_param_count] * 10 + (c - '0');
  } else if (c == ';') {
   tty->ansi_param_count++;
   if (tty->ansi_param_count >= 8) {
    ansi_reset(tty);
   } else {
    tty->ansi_params[tty->ansi_param_count] = 0;
   }
  } else {
   tty->ansi_cmd = c;
   ansi_execute(tty);
  }
  break;
 }
}

static void tty_commit_input_line(struct tty *tty) {
 char line[TTY_MAX_LINE_CHARS];
 size_t out = 0;
 size_t i;

 for (i = 0; i < tty->partial_length && out + 1 < sizeof(line); ++i) {
 line[out++] = tty->partial_output[i];
 }
 for (i = 0; i < tty->cooked_length && out + 1 < sizeof(line); ++i) {
 line[out++] = tty->cooked_line[i];
 }
 line[out] = '\0';

 if (out > 0) {
 tty_append_display_line(tty, line);
 }

 tty->partial_length = 0;
 tty->partial_output[0] = '\0';
}

static ssize_t tty_fd_read(void *object, void *buffer, size_t count) {
 struct tty *tty = (struct tty *)object;
 char *out = (char *)buffer;
 size_t to_copy;

 if (tty == 0 || buffer == 0 || count == 0) {
 return 0;
 }

 if (tty->raw_length > 0) {
 to_copy = tty->raw_length;
 if (to_copy > count) {
 to_copy = count;
 }
 for (size_t i = 0; i < to_copy; ++i) {
 out[i] = tty->raw_input[i];
 }
 tty_raw_input_shift_left(tty, to_copy);
 return (ssize_t)to_copy;
 }

 if (!tty->line_ready) {
 return 0;
 }

 /* When a cooked line becomes ready, drain it into the raw ring (with a
  * trailing '\n') so partial reads can pick it up byte-by-byte. The old
  * code copied `count-1` chars and unconditionally appended '\n', which
  * meant a 1-byte read got '\n' first and lost the rest of the line —
  * `su` (and any other char-by-char reader) would then see an empty
  * password and reject it. */
 tty_commit_input_line(tty);

 to_copy = tty->cooked_length;
 if (to_copy + 1 > sizeof(tty->raw_input)) {
 to_copy = sizeof(tty->raw_input) - 1;
 }
 for (size_t i = 0; i < to_copy; ++i) {
 tty->raw_input[i] = tty->cooked_line[i];
 }
 tty->raw_input[to_copy] = '\n';
 tty->raw_length = to_copy + 1;

 tty->line_ready = 0;
 tty->cooked_length = 0;
 tty->cooked_line[0] = '\0';
 tty->waiter_pid = 0;

 /* Now fall through to the raw path which honours `count`. */
 to_copy = tty->raw_length;
 if (to_copy > (size_t)count) {
 to_copy = count;
 }
 for (size_t i = 0; i < to_copy; ++i) {
 out[i] = tty->raw_input[i];
 }
 tty_raw_input_shift_left(tty, to_copy);
 return (ssize_t)to_copy;
}

static ssize_t tty_fd_write(void *object, const void *buffer, size_t count) {
 const char *text = (const char *)buffer;
 struct tty *tty = (struct tty *)object;
 size_t i;

 if (tty == 0 || buffer == 0) {
 return -SYSCALL_EINVAL;
 }

 serial_write("TTY_FD_WRITE: tty=");
 serial_write_hex_u64((uint64_t)(uintptr_t)tty);
 serial_write(" count=");
 serial_write_hex_u64(count);
 serial_write(" ansi_mode=");
 serial_write_hex_u64(tty->ansi_mode);
 serial_write("\n");

 /* Check if we should switch to ANSI mode (if we see escape sequences) */
 if (!tty->ansi_mode) {
 for (i = 0; i < count; ++i) {
 if (text[i] == 0x1b) {
 tty->ansi_mode = 1;
 screen_clear(tty);
 tty->cursor_row = 0;
 tty->cursor_col = 0;
 break;
 }
 }
 }

 if (tty->ansi_mode) {
 /* Process characters through ANSI parser */
 for (i = 0; i < count; ++i) {
 tty_process_ansi_char(tty, text[i]);
 }
 tty_touch(tty);
 } else {
 /* Legacy scroll mode */
 for (i = 0; i < count; ++i) {
 tty_append_output_char(tty, text[i]);
 }
 if (count > 0) {
 tty_touch(tty);
 }
 }

 serial_write("TTY_FD_WRITE: done\n");

 return (ssize_t)count;
}

static int tty_fd_ioctl(void *object, uint32_t request, uintptr_t arg) {
 struct tty *tty = (struct tty *)object;

 if (tty == 0) {
 return -SYSCALL_EINVAL;
 }

 switch (request) {
 case TTY_IOCTL_CLEAR:
 tty_reset(tty);
 return 0;
 case TTY_IOCTL_SET_RAW:
 /* Switch between cooked (line-buffered + echo) and raw (immediate,
  * no echo) input.  Drop any half-typed cooked line so the mode
  * change starts from a clean slate. */
 tty->raw_input_mode = (arg != 0) ? 1 : 0;
 tty->input_length = 0;
 tty->input[0] = '\0';
 return 0;
 default:
 return -SYSCALL_ENOSYS;
 }
}

const struct fd_ops TTY_FD_OPS = {
 tty_fd_read,
 tty_fd_write,
 tty_fd_ioctl
};

void tty_init(struct tty *tty) {
 tty->line_count = 0;
 tty->input_length = 0;
 tty->input[0] = '\0';
 tty->cooked_length = 0;
 tty->cooked_line[0] = '\0';
 tty->line_ready = 0;
 tty->raw_length = 0;
 tty->raw_input[0] = '\0';
 tty->partial_length = 0;
 tty->partial_output[0] = '\0';
 tty->revision = 1;
 tty->waiter_pid = 0;
 tty->raw_input_mode = 0;
 
 /* ANSI/screen mode init */
 tty->cursor_row = 0;
 tty->cursor_col = 0;
 tty->saved_cursor_row = 0;
 tty->saved_cursor_col = 0;
 tty->current_attr = TTY_ATTR_NORMAL;
 tty->ansi_mode = 0;
 tty->ansi_state = ANSI_STATE_NORMAL;
 tty->ansi_param_count = 0;
 screen_clear(tty);
}

int tty_handle_keyboard(struct tty *tty, const struct keyboard_state *keyboard) {
 size_t i;
 int dirty = 0;

 /* Ctrl+C (0x03): interrupt this terminal's foreground job and drop the byte. */
 for (i = 0; i < keyboard->count; ++i) {
 if (keyboard->chars[i] == 0x03) {
 process_interrupt_terminal(tty);
 return 0;
 }
 }

 /* Raw delivery: used both for ANSI screen mode and when a userspace line
  * editor has requested raw input via TTY_IOCTL_SET_RAW.  Bytes are queued
  * immediately with no local echo and no line buffering. */
 if (tty->ansi_mode || tty->raw_input_mode) {
 char raw_bytes[32];
 size_t raw_count = 0;

 if (keyboard->backspace_pressed) {
 raw_bytes[raw_count++] = '\b';
 }
 for (i = 0; i < keyboard->count && raw_count < sizeof(raw_bytes); ++i) {
 raw_bytes[raw_count++] = keyboard->chars[i];
 }
 if (keyboard->enter_pressed && raw_count < sizeof(raw_bytes)) {
 raw_bytes[raw_count++] = '\r';
 }

 if (raw_count > 0) {
 tty_queue_raw_input(tty, raw_bytes, raw_count);
 process_wake_tty_reader(tty);
 dirty = 1;
 }

 if (dirty) {
 tty_touch(tty);
 }

 return dirty;
 }

 if (keyboard->backspace_pressed && tty->input_length > 0) {
 --tty->input_length;
 tty->input[tty->input_length] = '\0';
 dirty = 1;
 }

 for (i = 0; i < keyboard->count; ++i) {
 if (tty->input_length + 1 < TTY_INPUT_CAPACITY) {
 tty->input[tty->input_length++] = keyboard->chars[i];
 tty->input[tty->input_length] = '\0';
 dirty = 1;
 }
 }

 if (keyboard->enter_pressed) {
 size_t j;

 serial_write("TTY_ENTER: tty=");
 serial_write_hex_u64((uint64_t)(uintptr_t)tty);
 serial_write(" waiter_pid=");
 serial_write_hex_u64(tty->waiter_pid);
 serial_write(" input=");
 serial_write(tty->input);
 serial_write("\n");

 tty->cooked_length = tty->input_length;
 for (j = 0; j < tty->input_length && j + 1 < TTY_INPUT_CAPACITY; ++j) {
 tty->cooked_line[j] = tty->input[j];
 }
 tty->cooked_line[tty->input_length] = '\0';
 tty->line_ready = 1;
 tty->input_length = 0;
 tty->input[0] = '\0';
 process_wake_tty_reader(tty);
 dirty = 1;
 }

 if (dirty) {
 tty_touch(tty);
 }

 return dirty;
}

void tty_reset(struct tty *tty) {
 tty->line_count = 0;
 tty->input_length = 0;
 tty->input[0] = '\0';
 tty->cooked_length = 0;
 tty->cooked_line[0] = '\0';
 tty->line_ready = 0;
 tty->raw_length = 0;
 tty->raw_input[0] = '\0';
 tty->partial_length = 0;
 tty->partial_output[0] = '\0';
 tty->waiter_pid = 0;
 
 /* Reset ANSI state */
 tty->cursor_row = 0;
 tty->cursor_col = 0;
 tty->saved_cursor_row = 0;
 tty->saved_cursor_col = 0;
 tty->current_attr = TTY_ATTR_NORMAL;
 tty->ansi_mode = 0;
 tty->ansi_state = ANSI_STATE_NORMAL;
 tty->ansi_param_count = 0;
 screen_clear(tty);
 
 tty_touch(tty);
}
