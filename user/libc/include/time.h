#ifndef VIBEOS_TIME_H
#define VIBEOS_TIME_H

#ifdef __cplusplus
extern "C" {
#endif

typedef long time_t;

struct tm {
    int tm_sec;    /* Seconds (0-60) */
    int tm_min;    /* Minutes (0-59) */
    int tm_hour;   /* Hours (0-23) */
    int tm_mday;   /* Day of the month (1-31) */
    int tm_mon;    /* Month (0-11) */
    int tm_year;   /* Year - 1900 */
    int tm_wday;   /* Day of the week (0-6, Sunday = 0) */
    int tm_yday;   /* Day in the year (0-365) */
    int tm_isdst;  /* Daylight saving time */
    long tm_gmtoff; /* Seconds east of UTC */
    const char *tm_zone; /* Timezone abbreviation */
};

time_t time(time_t *tloc);
struct tm *localtime_r(const time_t *timep, struct tm *result);

/* localtime — convenience wrapper using a static buffer (not thread-safe) */
static inline struct tm *localtime(const time_t *timep) {
    static struct tm buf;
    return localtime_r(timep, &buf);
}


#ifdef __cplusplus
}
#endif

#endif
