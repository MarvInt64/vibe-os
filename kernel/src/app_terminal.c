#include "app.h"
#include "process.h"
#include "render.h"
#include "tty.h"
#include "serial.h"

/* Draw a string in fixed monospace cells so the terminal's columns line up
 * even with the proportional system font. */
static void draw_mono_str(struct framebuffer *fb, int x, int y, const char *s, uint32_t color, int scale) {
	int cw = text_char_advance(scale);
	for (; *s; ++s) {
		if (*s != ' ') draw_glyph_mono(fb, x, y, *s, color, scale);
		x += cw;
	}
}

static void terminal_draw_ansi_screen(const struct terminal_app_state *terminal, const struct app_draw_context *ctx) {
	int row;
	int col;
	int char_width = text_char_advance(ctx->text_scale);
	int line_height = ctx->line_step;
	int base_x = ctx->content_x + (ctx->large_ui ? 6 : 2);
	int base_y = ctx->content_y + (ctx->large_ui ? 6 : 4);
	uint32_t bg_color = 0x000c1420u;
	uint32_t fg_color = 0x00cfe0f4u;

	for (row = 0; row < TTY_SCREEN_ROWS; ++row) {
		int y = base_y + (row * line_height);
		if (y >= ctx->content_y + ctx->content_height) {
			break;
		}

		for (col = 0; col < TTY_SCREEN_COLS; ++col) {
			char text[2];
			char c = terminal->tty.screen[row][col];
			uint8_t attr = terminal->tty.screen_attr[row][col];
			uint32_t cell_bg = (attr & TTY_ATTR_INVERSE) ? fg_color : bg_color;
			uint32_t cell_fg = (attr & TTY_ATTR_INVERSE) ? bg_color : fg_color;
			int x = base_x + (col * char_width);

			if (x >= ctx->content_x + ctx->content_width) {
				break;
			}

			if (cell_bg != bg_color) {
				fb_fill_rect(ctx->fb, x, y, char_width, line_height, cell_bg);
			}

			if (c == '\0' || c == ' ') {
				continue;
			}

			(void)text;
			draw_glyph_mono(ctx->fb, x, y, c, cell_fg, ctx->text_scale);
		}
	}

	if (ctx->focused &&
	    terminal->tty.cursor_row >= 0 &&
	    terminal->tty.cursor_row < TTY_SCREEN_ROWS &&
	    terminal->tty.cursor_col >= 0 &&
	    terminal->tty.cursor_col < TTY_SCREEN_COLS) {
		int cursor_x = base_x + (terminal->tty.cursor_col * char_width);
		int cursor_y = base_y + (terminal->tty.cursor_row * line_height) + line_height - 3;
		fb_fill_rect(ctx->fb, cursor_x, cursor_y, char_width, 2, 0x0064f2ccu);
	}
}

/* Action ids exposed through the dock context menu. */
#define TERM_ACTION_NEW 1

static void terminal_spawn_shell(struct terminal_app_state *terminal) {
	int pid = process_spawn_path("/bin/sh", &TTY_FD_OPS, &terminal->tty);
	serial_write("TERMINAL: spawn_shell pid=");
	serial_write_hex_u64(pid);
	serial_write("\n");
	if (pid > 0) {
		terminal->shell_pid = (uint32_t)pid;
		terminal->shell_running = 1;
	} else {
		terminal->shell_pid = 0;
		terminal->shell_running = 0;
	}
}

/* Tear down the current session and start a fresh shell on a clean TTY. */
static void terminal_restart_shell(struct terminal_app_state *terminal) {
	if (terminal->shell_running && terminal->shell_pid != 0) {
		(void)process_kill(terminal->shell_pid);
	}
	tty_init(&terminal->tty);
	terminal->seen_tty_revision = 0;
	terminal->damage_full = 1;
	terminal_spawn_shell(terminal);
}

static void terminal_app_activate(struct app_instance *app) {
	struct terminal_app_state *terminal = (struct terminal_app_state *)app->state;
	terminal->seen_tty_revision = 0;
	terminal->damage_full = 1;
	/* Reopening from the dock is the user's explicit "start a terminal" gesture:
	 * if the previous shell exited, spin up a fresh one. A live shell is left
	 * untouched (this is just a focus/redraw). */
	if (!terminal->shell_running) {
		tty_init(&terminal->tty);
		terminal_spawn_shell(terminal);
	}
}

static uint32_t terminal_app_window_owner_pid(struct app_instance *app) {
	struct terminal_app_state *terminal = (struct terminal_app_state *)app->state;
	return terminal->shell_running ? terminal->shell_pid : 0;
}

static void terminal_app_window_closed(struct app_instance *app) {
	struct terminal_app_state *terminal = (struct terminal_app_state *)app->state;
	/* The shell died and the compositor hid the window. Drop the session; the
	 * terminal only restarts on an explicit user gesture (dock click / menu). */
	terminal->shell_running = 0;
	terminal->shell_pid = 0;
	tty_init(&terminal->tty);
	terminal->seen_tty_revision = 0;
	terminal->damage_full = 1;
}

static int terminal_app_menu_items(struct app_instance *app, struct winsys_menu_item *out, int max) {
	const char *label = "Neues Terminal";
	int i;
	(void)app;
	if (max < 1) {
		return 0;
	}
	for (i = 0; i + 1 < WINSYS_MENU_LABEL_MAX && label[i]; ++i) {
		out[0].label[i] = label[i];
	}
	out[0].label[i] = '\0';
	out[0].action_id = TERM_ACTION_NEW;
	return 1;
}

static void terminal_app_menu_action(struct app_instance *app, uint32_t action_id) {
	struct terminal_app_state *terminal = (struct terminal_app_state *)app->state;
	if (action_id == TERM_ACTION_NEW) {
		terminal_restart_shell(terminal);
	}
}

static int terminal_app_handle_keyboard(struct app_instance *app, const struct keyboard_state *keyboard) {
	struct terminal_app_state *terminal = (struct terminal_app_state *)app->state;
	int dirty;

	dirty = tty_handle_keyboard(&terminal->tty, keyboard);

	if (!dirty) {
		return 0;
	}

	if (keyboard->enter_pressed) {
		terminal->damage_full = 1;
		terminal->damage_input = 0;
	} else {
		terminal->damage_input = 1;
	}

	return 1;
}

static int terminal_app_needs_redraw(struct app_instance *app) {
	struct terminal_app_state *terminal = (struct terminal_app_state *)app->state;

	/* Only repaint when something actually changed. Returning 1 every frame
	 * forced a full-window blit into the live framebuffer on every timer tick,
	 * which is unsynchronised with the display and shows up as a shimmering /
	 * tearing "heat haze" over the terminal window. */
	return terminal->damage_full ||
	       terminal->damage_input ||
	       terminal->tty.revision != terminal->seen_tty_revision;
}

static int terminal_app_consume_damage(struct app_instance *app, const struct app_draw_context *ctx, struct rect *damage_rect) {
	struct terminal_app_state *terminal = (struct terminal_app_state *)app->state;

	damage_rect->x = 0;
	damage_rect->y = 0;
	damage_rect->width = ctx->window_width;
	damage_rect->height = ctx->window_height;
	terminal->damage_full = 0;
	terminal->damage_input = 0;
	terminal->seen_tty_revision = terminal->tty.revision;
	return 1;
}

static void terminal_app_draw(const struct app_instance *app, const struct app_draw_context *ctx) {
	struct terminal_app_state *terminal = (struct terminal_app_state *)app->state;

	char input_line[TTY_MAX_LINE_CHARS];
	int line_y = ctx->content_y;
	int visible_lines = (ctx->content_height - (ctx->large_ui ? 42 : 28)) / ctx->line_step;
	int start_line = 0;
	size_t i;
	size_t out = 0;

	fb_fill_rect(ctx->fb, ctx->content_x, ctx->content_y, ctx->content_width, ctx->content_height, 0x000c1420u);
	fb_draw_rect(ctx->fb, ctx->content_x, ctx->content_y, ctx->content_width, ctx->content_height, 2, 0x00263a54u);

	if (terminal->tty.ansi_mode) {
		terminal_draw_ansi_screen(terminal, ctx);
		return;
	}

	if ((int)terminal->tty.line_count > visible_lines - 2) {
		start_line = (int)terminal->tty.line_count - (visible_lines - 2);
	}

	for (i = (size_t)start_line; i < terminal->tty.line_count; ++i) {
		draw_mono_str(ctx->fb, ctx->content_x + (ctx->large_ui ? 6 : 2), line_y + (ctx->large_ui ? 6 : 4), terminal->tty.lines[i], 0x00cfe0f4u, ctx->text_scale);
		line_y += ctx->line_step;
	}

	if (terminal->tty.partial_length > 0) {
		for (i = 0; i < terminal->tty.partial_length && out + 1 < sizeof(input_line); ++i) {
			input_line[out++] = terminal->tty.partial_output[i];
		}
	}
	if (terminal->tty.input_length > 0) {
		for (i = 0; i < terminal->tty.input_length && out + 1 < sizeof(input_line); ++i) {
			input_line[out++] = terminal->tty.input[i];
		}
	}
	input_line[out] = '\0';

	{
		int input_x = ctx->content_x + (ctx->large_ui ? 6 : 2);
		int input_y = line_y + (ctx->large_ui ? 6 : 4);

		draw_mono_str(ctx->fb, input_x, input_y, input_line, 0x00f7fbffu, ctx->text_scale);

		if (ctx->focused) {
			int char_width = text_char_advance(ctx->text_scale);
			int cursor_x = input_x + (int)(out * char_width);
			fb_fill_rect(ctx->fb, cursor_x, input_y + text_line_height(ctx->text_scale) - 2, char_width, 3, 0x0064f2ccu);
		}
	}
}

static const struct app_vtable TERMINAL_APP_VTABLE = {
	terminal_app_activate,
	terminal_app_handle_keyboard,
	terminal_app_needs_redraw,
	terminal_app_consume_damage,
	terminal_app_draw,
	terminal_app_window_owner_pid,
	terminal_app_window_closed,
	terminal_app_menu_items,
	terminal_app_menu_action
};

void app_init_terminal(struct app_instance *app, struct terminal_app_state *state) {
	tty_init(&state->tty);
	fd_table_init(&state->fd_table);
	fd_bind(&state->fd_table, 0, &TTY_FD_OPS, &state->tty);
	fd_bind(&state->fd_table, 1, &TTY_FD_OPS, &state->tty);
	fd_bind(&state->fd_table, 2, &TTY_FD_OPS, &state->tty);
	state->syscalls.fd_table = &state->fd_table;
	state->seen_tty_revision = 0;
	state->last_line_count = 0;
	state->last_partial_length = 0;
	state->last_input_length = 0;
	state->damage_full = 1;
	state->damage_input = 0;
	state->shell_pid = 0;
	state->shell_running = 0;
	app->vtable = &TERMINAL_APP_VTABLE;
	app->state = state;
	terminal_spawn_shell(state);
}
