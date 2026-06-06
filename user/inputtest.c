/* VibeOS arm64 — input poll test. Prints mouse state to serial when it changes. */
#include <stdio.h>
#include <sys/input.h>

int main(void) {
    struct vos_input_state st;
    printf("inputtest: polling for mouse events...\n");
    printf("inputtest: move the mouse or click in the QEMU window\n");

    int last_x = -1, last_y = -1, last_b = -1;
    for (int i = 0; i < 500; i++) {
        if (vos_input_poll(&st) != 0) {
            printf("inputtest: no input device\n");
            return 1;
        }
        if (st.x != last_x || st.y != last_y || st.buttons != last_b) {
            printf("inputtest: x=%d y=%d buttons=%d\n", st.x, st.y, st.buttons);
            last_x = st.x; last_y = st.y; last_b = st.buttons;
        }
        /* Busy-wait ~100ms */
        for (volatile int j = 0; j < 2000000; j++) { }
    }
    printf("inputtest: done\n");
    return 0;
}
