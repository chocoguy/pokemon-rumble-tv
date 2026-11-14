/* The port's 16-bit wide-character functions (MSL's, from the fork). Built by run.sh. */
#include <wchar.h>
#include <wstring.h>
#include <wctype.h>
#include <stdio.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int same(const wchar_t* a, const char* b) {
    while (*a && *b && *a == (unsigned char)*b) { a++; b++; }
    return *a == 0 && *b == 0;
}

int main(void) {
    wchar_t buf[64];
    const wchar_t hello[] = {'h', 'e', 'l', 'l', 'o', 0};
    long l = 42;
    int n;

    CHECK(sizeof(wchar_t) == 2, "wchar_t is %d bytes", (int)sizeof(wchar_t));
    CHECK(wcslen(hello) == 5, "wcslen");
    CHECK(wcscmp(hello, hello) == 0 && wcsncmp(hello, buf, 0) == 0, "wcscmp");
    wmemcpy(buf, hello, 6);
    CHECK(wcscmp(buf, hello) == 0, "wmemcpy");

    n = swprintf(buf, 64, L"%d-%ld-%u-%ls", -7, l, 3000000000u, hello);
    CHECK(same(buf, "-7-42-3000000000-hello") && n == 22, "swprintf gave %d chars", n);
    swprintf(buf, 64, L"%ld", 123);  /* a Wii-era call: an int passed for %ld */
    CHECK(same(buf, "123"), "%%ld with an int argument");
    swprintf(buf, 64, L"%04x|%-3d|", 0xbeef, 5);
    CHECK(same(buf, "beef|5  |"), "width and flags");

    CHECK(iswspace(L' ') && !iswspace(0x3000), "MSL classifies only below 256");
    CHECK(towupper(L'a') == L'A' && towupper(0x3042) == 0x3042, "towupper");
    CHECK(wcstol(L"  -123x", 0, 10) == -123, "wcstol");

    printf("%s: %d failure(s)\n", __FILE__, failures);
    return failures != 0;
}
