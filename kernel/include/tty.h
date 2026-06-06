#ifndef VIBEOS_TTY_H
#define VIBEOS_TTY_H

#include "fd.h"
#include "input.h"
#include "types.h"

#define TTY_MAX_LINES 64
#define TTY_MAX_LINE_CHARS 120
#define TTY_INPUT_CAPACITY 96

/* Virtual screen for ANSI terminal emulation */
#define TTY_SCREEN_ROWS 25
#define TTY_SCREEN_COLS 80

enum tty_ioctl_request {
 TTY_IOCTL_CLEAR = 1,
 /* Raw input mode: deliver keyboard bytes immediately (no line buffering)
  * and suppress local echo, so a line editor in userspace can handle
  * editing/history itself.  arg != 0 enables, arg == 0 restores cooked mode. */
 TTY_IOCTL_SET_RAW = 2,
 /* Query raw mode: places current raw_input_mode (0 or 1) at *arg. */
 TTY_IOCTL_GET_RAW = 3
};

/* Character attributes */
#define TTY_ATTR_NORMAL   0x00
#define TTY_ATTR_INVERSE  0x01

/* ANSI parser states */
enum ansi_state {
 ANSI_STATE_NORMAL = 0,
 ANSI_STATE_ESCAPE,
 ANSI_STATE_BRACKET,
 ANSI_STATE_PARAMS
};

struct tty {
 /* Scrollback buffer (for non-ANSI mode) */
 char lines[TTY_MAX_LINES][TTY_MAX_LINE_CHARS];
 size_t line_count;

 /* Input buffer */
 char input[TTY_INPUT_CAPACITY];
 size_t input_length;
 char cooked_line[TTY_INPUT_CAPACITY];
 size_t cooked_length;
 uint8_t line_ready;
 char raw_input[TTY_INPUT_CAPACITY];
 size_t raw_length;

 /* Partial output line (for non-ANSI mode) */
 char partial_output[TTY_MAX_LINE_CHARS];
 size_t partial_length;

 /* Virtual screen for ANSI mode */
 char screen[TTY_SCREEN_ROWS][TTY_SCREEN_COLS];
 uint8_t screen_attr[TTY_SCREEN_ROWS][TTY_SCREEN_COLS];
 int cursor_row;
 int cursor_col;
 int saved_cursor_row;
 int saved_cursor_col;
 uint8_t current_attr;
 uint8_t ansi_mode;  /* 0 = scroll mode, 1 = screen mode */

 /* ANSI parser state */
 enum ansi_state ansi_state;
 int ansi_params[8];
 int ansi_param_count;
 char ansi_cmd;

 uint32_t revision;
 uint32_t waiter_pid;

 /* When set, keyboard input is delivered raw (byte-by-byte, no echo) via the
  * raw_input ring instead of the cooked line buffer.  Toggled by
  * TTY_IOCTL_SET_RAW so a userspace line editor can own editing + history. */
 uint8_t raw_input_mode;
};

extern const struct fd_ops TTY_FD_OPS;

void tty_init(struct tty *tty);
int  tty_handle_keyboard(struct tty *tty, const struct keyboard_state *keyboard);

/*
 * Read any available bytes from the serial port (COM1) and feed them
 * into the TTY as if they came from a keyboard.  Handles backspace (0x7F),
 * CR→LF conversion, and Ctrl+C signalling.  Characters are echoed back to
 * serial so the remote terminal can see what is being typed.
 *
 * Returns 1 if the TTY was modified (caller should re-render), 0 otherwise.
 */
int  tty_handle_serial_input(struct tty *tty);

void tty_reset(struct tty *tty);

#endif
