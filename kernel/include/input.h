#ifndef VIBEOS_INPUT_H
#define VIBEOS_INPUT_H

#include "types.h"

struct mouse_state {
    int x;
    int y;
    int dx;
    int dy;
    int wheel;           /* accumulated scroll-wheel delta this poll (0 if none) */
    uint8_t buttons;
    uint8_t moved;
    uint8_t left_pressed;
    uint8_t left_released;
    uint8_t right_pressed;
    uint8_t right_released;
};

struct keyboard_state {
    char chars[32];
    size_t count;
    uint8_t enter_pressed;
    uint8_t backspace_pressed;
};

void input_init(uint32_t screen_width, uint32_t screen_height);
void input_poll(struct mouse_state *mouse, struct keyboard_state *keyboard);

#endif
