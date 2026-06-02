#ifndef VIBEOS_SETJMP_H
#define VIBEOS_SETJMP_H

#ifdef __cplusplus
extern "C" {
#endif

/* x86-64 System V ABI jmp_buf layout:
 * [0] rbx  [1] rbp  [2] r12  [3] r13  [4] r14  [5] r15  [6] rsp  [7] rip */
typedef unsigned long jmp_buf[8];
typedef unsigned long sigjmp_buf[9]; /* [8] = saved signal mask (unused) */

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

/* POSIX aliases */
#define _setjmp(env)          setjmp(env)
#define _longjmp(env, val)    longjmp((env), (val))
#define sigsetjmp(env, save)  setjmp((jmp_buf *)(env))
#define siglongjmp(env, val)  longjmp((jmp_buf *)(env), (val))

#ifdef __cplusplus
}
#endif

#endif
