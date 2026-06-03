#ifndef VIBEOS_TIME_H
#define VIBEOS_TIME_H

#ifdef __cplusplus
extern "C" {
#endif

typedef long time_t;

time_t time(time_t *tloc);

#ifdef __cplusplus
}
#endif

#endif
