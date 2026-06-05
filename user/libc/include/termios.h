#ifndef VIBEOS_TERMIOS_H
#define VIBEOS_TERMIOS_H
#include <sys/types.h>
#include <sys/ioctl.h>
#define TCSANOW 0
#define IEXTEN 0x00400
#define OPOST 0x00001
#define ISIG 0x00080
#define IXON 0x00200
struct termios { unsigned int c_iflag,c_oflag,c_cflag,c_lflag; };
#ifdef __cplusplus
extern "C" {
#endif
static inline int tcgetattr(int fd, struct termios *t) {
    (void)fd; if(t){t->c_iflag=0;t->c_oflag=OPOST;t->c_cflag=0;t->c_lflag=ISIG|IEXTEN;} return 0;
}
static inline int tcsetattr(int fd, int action, const struct termios *t) {
    (void)action; if(!t) return -1;
    int raw = ((t->c_lflag&(ISIG|IEXTEN))==0)?1:0;
    return ioctl(fd, TTY_IOCTL_SET_RAW, raw);
}
#ifdef __cplusplus
}
#endif
#endif
