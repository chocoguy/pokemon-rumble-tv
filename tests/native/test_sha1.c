/* NETSHA1 (src/RevoEX/net/sha1.c) against the FIPS 180 test vectors, fed whole and in
 * odd-sized pieces. The SDK's version was host-order and only SHA-1 on the Wii. */
#include <revolution/net.h>
#include <stdio.h>
#include <string.h>

static int sFailures;

static void Hex(const u8* digest, char* out) {
    int i;
    for (i = 0; i < NET_SHA1_DIGEST_SIZE; i++) {
        snprintf(out + i * 2, 3, "%02x", digest[i]);
    }
}

static void Check(const char* name, const void* data, u32 length, u32 repeat, u32 piece, const char* expected) {
    NETSHA1Context context;
    u8 digest[NET_SHA1_DIGEST_SIZE];
    char hex[NET_SHA1_DIGEST_SIZE * 2 + 1];
    u32 r;
    u32 offset;
    NETSHA1Init(&context);
    for (r = 0; r < repeat; r++) {
        for (offset = 0; offset < length; offset += piece) {
            u32 n = length - offset < piece ? length - offset : piece;
            NETSHA1Update(&context, (const u8*)data + offset, n);
        }
    }
    NETSHA1GetDigest(&context, digest);
    Hex(digest, hex);
    if (strcmp(hex, expected) != 0) {
        printf("test_sha1: %s (pieces of %u): %s, expected %s\n", name, (unsigned)piece, hex, expected);
        sFailures++;
    }
}

int main(void) {
    static const char* abc = "abc";
    static const char* two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    static const char* a10 = "aaaaaaaaaa";
    u32 piece;
    for (piece = 1; piece <= 64; piece += 7) {
        Check("empty", "", 0, 1, piece, "da39a3ee5e6b4b0d3255bfef95601890afd80709");
        Check("abc", abc, 3, 1, piece, "a9993e364706816aba3e25717850c26c9cd0d89d");
        Check("448 bits", two, (u32)strlen(two), 1, piece, "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    }
    Check("a million a", a10, 10, 100000, 10, "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    if (sFailures == 0) {
        printf("test_sha1: ok\n");
    }
    return sFailures != 0;
}
