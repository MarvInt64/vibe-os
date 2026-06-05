#ifndef VIBEOS_SYS_TIME_H
#define VIBEOS_SYS_TIME_H

#ifdef __cplusplus
extern "C" {
#endif

struct timeval {
    long tv_sec;
    long tv_usec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

int gettimeofday(struct timeval *tv, void *tz);

#ifdef __cplusplus
}
#endif

#endif /* VIBEOS_SYS_TIME_H */
