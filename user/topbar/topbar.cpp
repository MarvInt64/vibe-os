/* topbar — the VibeOS desktop top bar, as a userspace app.
 *
 * A frameless, always-on-top, translucent window pinned to the top-left of the
 * screen (analogous to the dock). Today it renders the VibeOS logo from
 * /icons/vibeos-logo.svg via libvexui/libsvg; drawing in userspace lets it use
 * the full SVG renderer (gradients, glow), which the SSE-less kernel cannot.
 *
 * This is intended to grow into the full top bar: the app menus of the focused
 * window, the clock, and the CPU/UI/MEM indicators (currently drawn by the
 * kernel) belong here. For now the kernel still draws those to the right; only
 * the logo has moved into this app. */

#include "vexui.h"
#include <vibeos.h>

int main() {
    const int x = 6, y = 4;     /* sits inside the 54px kernel top bar */
    const int size = 46;

    vos_log(VOS_LOG_APP, "topbar ready");

    vui_window *win = vui_window_open_ex(
        "Top Bar", size, size,
        VUI_WINDOW_FRAMELESS | VUI_WINDOW_NO_DOCK |
            VUI_WINDOW_POSITIONED | VUI_WINDOW_ALWAYS_ON_TOP |
            VUI_WINDOW_TRANSLUCENT,
        x, y);
    vui_set_clear_color(win, VUI_COLOR_TRANSPARENT);

    vui_widget *logo = vui_image(win, 0, 0, size);
    vui_set_icon_svg_path(logo, "/icons/vibeos-logo.svg");

    vui_run(win);
}
