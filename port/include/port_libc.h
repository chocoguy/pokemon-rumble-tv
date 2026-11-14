/* Additions to the system C library that the game expects from Metrowerks' MSL.
 *
 * wchar_t is 16 bits in this build (-fshort-wchar), as on the Wii, because the game's
 * text files, fonts and string literals are UTF-16. The system's wide-character
 * functions assume 32 bits, so port/libc provides the ones the game calls; they are
 * linked ahead of libSystem's. */
#ifndef PORT_LIBC_H
#define PORT_LIBC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
}
#endif

#endif
