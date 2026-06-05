#ifndef NANO_CONFIG_H
#define NANO_CONFIG_H

#include <sys/types.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>

#define ENABLE_WRAPPING      1
#define ENABLE_TABCOMP       1
#define HAVE_NCURSES_H       1
#define HAVE_TERMIOS_H       0

#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif
#undef  _POSIX_VDISABLE
#define DISABLE_MOUSE       1

#ifndef _
#define _(s)     (s)
#endif
#ifndef P_
#define P_(s,p,n) ((n) == 1 ? (s) : (p))
#endif

#define A_BLINK             0x00000008u
#define NANO_REG_EXTENDED   1
#define REG_STARTEND        8

#define main                nano_main
#define PACKAGE_STRING      "nano 8.4"

#endif
