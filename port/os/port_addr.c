/* The 4 GB address window that 32-bit game addresses are relative to (port_addr.h).
 * port/os sets gPortAddrBase when it reserves the game's memory at start-up. */
#include <port_addr.h>
#include <stdio.h>
#include <stdlib.h>

uintptr_t gPortAddrBase;

void PortAddrOutOfWindow(const void* p) {
    fprintf(stderr, "[RED] pointer %p is outside the 32-bit game address window based at %p\n", p, (void*)gPortAddrBase);
    abort();
}
