/* __rlwimi against hand-worked PowerPC cases (tests/native/run.sh). */
#include <stdio.h>
static int failures;
#define CHECK(got, want) do { unsigned int g = (got), w = (want); if (g != w) { failures++; printf("FAIL line %d: got %08x want %08x\n", __LINE__, g, w); } } while (0)
int main(void) {
    /* Insert the low 4 bits of s at bits 8-11 (MSB numbering 20..23): sh=8, mb=20, me=23. */
    CHECK(__rlwimi(0xFFFFFFFFu, 0x5u, 8, 20, 23), 0xFFFFF5FFu);
    /* sh = 0, mb..me = 0..31 replaces everything. */
    CHECK(__rlwimi(0x12345678u, 0xCAFEBABEu, 0, 0, 31), 0xCAFEBABEu);
    /* Wrapping mask: mb=28, me=3 covers bits 28-31 and 0-3. */
    CHECK(__rlwimi(0x00000000u, 0xFFFFFFFFu, 0, 28, 3), 0xF000000Fu);
    /* FAST_FLAG_SET(reg, 0x3, 4, 2): shift 4, size 2 -> mb = 26, me = 27. */
    CHECK(__rlwimi(0xFFFFFFFFu, 0x0u, 4, 32 - 4 - 2, 31 - 4), 0xFFFFFFCFu);
    CHECK(__rlwimi(0x00000000u, 0x3u, 4, 32 - 4 - 2, 31 - 4), 0x00000030u);
    printf("%s: %d failure(s)\n", __FILE__, failures);
    return failures != 0;
}
