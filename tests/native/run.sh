#!/bin/sh
# Build and run the native unit tests on this Mac.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
out="$root/build/tests"
mkdir -p "$out"
fork="$root/pokemon-rumble-decomp-tv"
flags="-arch arm64 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fshort-wchar -fno-strict-aliasing -ffp-contract=off -fwrapv -fsigned-char -DPORT=1 -DNW4R_LITTLE_ENDIAN=1 -include $root/port/include/port_prefix.h -I $root/port/include -I $fork/include"
status=0

xcrun clang $flags -std=gnu99 -Wall -Wno-unknown-pragmas -o "$out/test_mtx" "$root/tests/native/test_mtx.c" \
    "$fork/src/RVL_SDK/mtx/mtx.c" "$fork/src/RVL_SDK/mtx/vec.c" "$fork/src/RVL_SDK/mtx/mtxvec.c" "$root/port/os/port_addr.c" -lm
"$out/test_mtx" || status=1

for f in mtx vec mtxvec; do
    xcrun clang $flags -std=gnu99 -w -c -o "$out/sdk_$f.o" "$fork/src/RVL_SDK/mtx/$f.c"
done
xcrun clang $flags -std=gnu99 -w -c -o "$out/port_addr.o" "$root/port/os/port_addr.c"
xcrun clang++ $flags -x c++ -std=gnu++03 -nostdinc++ -fdelayed-template-parsing -w -o "$out/test_nw4r_math" \
    "$root/tests/native/test_nw4r_math.cpp" "$fork/src/nw4r/math/math_types.cpp" "$fork/src/nw4r/math/math_triangular.cpp" \
    "$fork/src/nw4r/math/math_arithmetic.cpp" -x none "$out"/sdk_*.o "$out/port_addr.o"
"$out/test_nw4r_math" || status=1

xcrun clang $flags -std=gnu99 -w -o "$out/test_rlwimi" "$root/tests/native/test_rlwimi.c"
"$out/test_rlwimi" || status=1

msl=""
for f in wprintf wstring wmem wcstoul wstrtold wide_decimal locale extras ansi_fp strnicmp mbstring wscanf ctype_data wide_ctype_data floating_constants; do
    xcrun clang $flags -std=gnu99 -w -c -o "$out/msl_$f.o" "$fork/src/MSL_C/$f.c" || status=1
    msl="$msl $out/msl_$f.o"
done
xcrun clang $flags -std=gnu99 -w -o "$out/test_msl_wide" "$root/tests/native/test_msl_wide.c" $msl "$out/port_addr.o"
"$out/test_msl_wide" || status=1

# Layout of structs overlaid on file data (compile-time only).
xcrun clang++ $flags -x c++ -std=gnu++03 -nostdinc++ -fdelayed-template-parsing -w -fsyntax-only "$root/tests/compile/layout_checks.cpp" \
    && echo "layout_checks.cpp: ok" || status=1

exit $status
