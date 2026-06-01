#ifndef VIBEOS_STDINT_H
#define VIBEOS_STDINT_H

typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef signed short int16_t;
typedef unsigned short uint16_t;
typedef signed int int32_t;
typedef unsigned int uint32_t;
typedef signed long int64_t;
typedef unsigned long uint64_t;

typedef signed long intptr_t;
typedef unsigned long uintptr_t;

typedef signed long intmax_t;
typedef unsigned long uintmax_t;

#define INT8_MIN   (-128)
#define INT8_MAX   127
#define UINT8_MAX  255u
#define INT16_MIN  (-32767-1)
#define INT16_MAX  32767
#define UINT16_MAX 65535u
#define INT32_MIN  (-2147483647-1)
#define INT32_MAX  2147483647
#define UINT32_MAX 4294967295u
#define INT64_MIN  (-9223372036854775807l-1l)
#define INT64_MAX  9223372036854775807l
#define UINT64_MAX 18446744073709551615ul

#endif
