#ifndef VIBEOS_SYS_IOCTL_H
#define VIBEOS_SYS_IOCTL_H
#include <sys/types.h>

/* TTY ioctl requests (match kernel tty.h) */
#define TTY_IOCTL_CLEAR    1
#define TTY_IOCTL_SET_RAW  2   /* arg != 0 = raw, arg == 0 = cooked */
#define TTY_IOCTL_GET_RAW  3   /* query raw mode: reads *arg = 0/1 */

/* Terminal window size */
#define TIOCGWINSZ  0x5413
struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };

#ifdef __cplusplus
extern "C" {
#endif

int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif

#endif
