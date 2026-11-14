/* Force-included before every unit compiled from the fork (see tools/compile_check.py).
 * It only neutralises CodeWarrior syntax; behaviour lives in port/. */
#ifndef PORT_PREFIX_H
#define PORT_PREFIX_H

/* CodeWarrior placed functions and data in named sections to control link order. */
#define __declspec(x)

/* CodeWarrior's typeof. */
#define __decltype__ __typeof__
#define __alloca __builtin_alloca

#include <port_addr.h>

#if !defined(__cplusplus)
/* The SDK poisons its wide-character functions in C when wchar_t is 2 bytes, because
 * they assume 4. The port does not call them: it compiles MSL's own 2-byte versions
 * (src/MSL_C, compile_check.py's msl group), which the fork's wchar.h declares. */
#undef __WCHAR_MAX__
#undef WCHAR_MAX
#undef WCHAR_MIN
#define WCHAR_MAX 0xffff
#define WCHAR_MIN 0
#endif

/* CodeWarrior's PowerPC intrinsics. */
static inline double __fabs(double x) { return __builtin_fabs(x); }
static inline float __fabsf(float x) { return __builtin_fabsf(x); }
static inline double __fnabs(double x) { return -__builtin_fabs(x); }
static inline float __fnabsf(float x) { return -__builtin_fabsf(x); }
static inline int __abs(int x) { return x < 0 ? -x : x; }
static inline int __labs(long x) { return (int)(x < 0 ? -x : x); }
static inline int __cntlzw(unsigned int x) { return x ? __builtin_clz(x) : 32; }
static inline double __frsqrte(double x) { return 1.0 / __builtin_sqrt(x); }
static inline float __fres(float x) { return 1.0f / x; }
static inline unsigned int __rlwimi(unsigned int a, unsigned int s, int sh, int mb, int me) {
    unsigned int rot = sh ? (s << sh) | (s >> (32 - sh)) : s;
    unsigned int mask = mb <= me ? ((0xFFFFFFFFu >> mb) & (0xFFFFFFFFu << (31 - me)))
                                 : ((0xFFFFFFFFu >> mb) | (0xFFFFFFFFu << (31 - me)));
    return (rot & mask) | (a & ~mask);
}
/* __lwbrx and friends are left undefined: what a byte-reversed load means here depends on
 * whether that buffer was byte-swapped at load time, so each use needs its own decision. */

/* CodeWarrior's `register` plus inline PowerPC assembly is gone from the port, so the
 * keyword carries no meaning here; C++03 clang still accepts it. */


#ifdef __cplusplus
/* CodeWarrior builtin used by its library's type traits: bit 0 trivial destructor,
 * bit 1 trivial copy constructor, bit 2 trivial assignment (7 means all three). */
#define __builtin_trivial_members(T) \
    ((__has_trivial_destructor(T) ? 1 : 0) | (__has_trivial_copy(T) ? 2 : 0) | (__has_trivial_assign(T) ? 4 : 0))
#define __builtin_is_pod(T) __is_pod(T)
#endif

#endif
