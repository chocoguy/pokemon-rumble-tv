/* 32-bit game addresses on a 64-bit host.
 *
 * The game keeps pointers in 32-bit slots in many places: inside loaded files, in
 * script values, in type ids, and in GX registers and display lists. On the Wii those
 * were addresses in the cached segment (0x80000000 and up), and some code relies on
 * that, for example by treating a TPL field below 0x80000000 as a file offset that has
 * not been resolved yet.
 *
 * So the port keeps that address map. Every object the game can point at (the
 * executable's own data, the game heaps, and the stacks of game threads, which come
 * from the heaps) lies in one 1 GB window starting at gPortAddrBase. A pointer at offset
 * `o` in the window has the 32-bit address 0x80000000 + o (cached), 0xC0000000 + o
 * (uncached) or `o` (physical), and all three lead back to the same pointer.
 *
 * Use these only where a pointer must fit a 32-bit slot. Ordinary pointer arithmetic
 * uses uintptr_t.
 */
#ifndef PORT_ADDR_H
#define PORT_ADDR_H

#include <stdint.h>

#define PORT_ADDR_CACHED 0x80000000u
#define PORT_ADDR_UNCACHED 0xC0000000u
#define PORT_ADDR_WINDOW_SIZE 0x40000000u /* 1 GB */

#ifdef __cplusplus
extern "C" {
#endif

extern uintptr_t gPortAddrBase;
void PortAddrOutOfWindow(const void* p) __attribute__((noreturn));

/* Physical address: the offset in the window. */
static inline uint32_t PortPhys(const void* p) {
    uintptr_t offset = (uintptr_t)p - gPortAddrBase;
    if (offset >= PORT_ADDR_WINDOW_SIZE) {
        PortAddrOutOfWindow(p);
    }
    return (uint32_t)offset;
}

/* Cached address, as a pointer value read back from a 32-bit slot; NULL stays 0. */
static inline uint32_t PortAddr(const void* p) {
    return p == 0 ? 0u : PORT_ADDR_CACHED | PortPhys(p);
}

/* Any of the three forms back to a pointer; 0 is NULL. */
static inline void* PortPtrRaw(uint32_t addr) {
    return addr == 0 ? (void*)0 : (void*)(gPortAddrBase + (addr & (PORT_ADDR_WINDOW_SIZE - 1)));
}

#ifdef __cplusplus
}
#endif

/* A 4-byte pointer slot in a struct shared by C and C++ code: Ptr32<T> in C++ (below),
 * the raw 32-bit address in C (use PortPtrRaw to follow it). */
#ifdef __cplusplus
#define PORT_PTR32(T) Ptr32<T>
#else
#define PORT_PTR32(T) uint32_t
#endif

#ifdef __cplusplus
/* PortPtr<const char*>(value) */
template <typename T> inline T PortPtr(uint32_t addr) {
    return static_cast<T>(PortPtrRaw(addr));
}

template <typename T> struct PortRef { typedef T& type; };
template <> struct PortRef<void> { typedef void type; };
template <> struct PortRef<const void> { typedef void type; };

/* A pointer member of a struct that is laid over file data. In the file the slot is 4
 * bytes and holds an offset until the loader resolves it in place; afterwards it holds a
 * 32-bit game address. Ptr32 keeps the slot 4 bytes and reads like a pointer.
 *
 * It has no constructors and no copy assignment, so structs containing it stay POD.
 * Raw() is the slot as stored, for code that reads the unresolved offset. */
template <typename T> struct Ptr32 {
    uint32_t mAddr;

    T* get() const { return static_cast<T*>(PortPtrRaw(mAddr)); }
    operator T*() const { return get(); }
    T* operator->() const { return get(); }
    typename PortRef<T>::type operator*() const { return *get(); }
    Ptr32& operator=(T* p) {
        mAddr = PortAddr(p);
        return *this;
    }
    uint32_t Raw() const { return mAddr; }
    void SetRaw(uint32_t value) { mAddr = value; }
};
#endif

#endif
