// Runtime checks for the port's C versions of NW4R math (built by tests/native/run.sh).
#include <nw4r/math/math_types.h>
#include <nw4r/math/math_triangular.h>
#include <stdio.h>

using namespace nw4r::math;

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static bool near(f32 a, f32 b, f32 tol = 1e-4f) { return __builtin_fabsf(a - b) <= tol * (1.0f + __builtin_fabsf(a) + __builtin_fabsf(b)); }
static bool near(const MTX34& a, const MTX34& b, f32 tol = 1e-4f) {
    const f32* pa = &a._00;
    const f32* pb = &b._00;
    for (int i = 0; i < 12; i++) if (!near(pa[i], pb[i], tol)) return false;
    return true;
}

int main() {
    // Table lookups against the C library, in table units (256 per turn).
    for (int i = -600; i <= 600; i += 7) {
        f32 fidx = i * 0.37f;
        f32 rad = fidx * (2.0f * 3.14159265f / 256.0f);
        f32 s, c;
        SinCosFIdx(&s, &c, fidx);
        CHECK(near(s, __builtin_sinf(rad), 1e-3f) && near(c, __builtin_cosf(rad), 1e-3f), "SinCosFIdx(%f) = %f %f", fidx, s, c);
        CHECK(near(s, SinFIdx(fidx), 1e-6f) && near(c, CosFIdx(fidx), 1e-6f), "SinCosFIdx disagrees with SinFIdx/CosFIdx at %f", fidx);
    }

    // RotXYZ is Rz * Ry * Rx.
    {
        f32 x = 20.0f, y = -45.0f, z = 100.0f;
        MTX34 rxyz, rx, ry, rz, t;
        MTX34RotXYZFIdx(&rxyz, x, y, z);
        PSMTXRotRad(rx, 'x', x * (2.0f * 3.14159265f / 256.0f));
        PSMTXRotRad(ry, 'y', y * (2.0f * 3.14159265f / 256.0f));
        PSMTXRotRad(rz, 'z', z * (2.0f * 3.14159265f / 256.0f));
        PSMTXConcat(ry, rx, t);
        PSMTXConcat(rz, t, t);
        CHECK(near(rxyz, t, 2e-3f), "MTX34RotXYZFIdx is not Rz*Ry*Rx");
    }

    // Scale and translation on the right of an arbitrary matrix.
    {
        MTX34 m, s, out, ref;
        MTX34RotXYZFIdx(&m, 10.0f, 50.0f, -30.0f);
        m._03 = 1.0f; m._13 = 2.0f; m._23 = 3.0f;
        VEC3 scale(2.0f, -1.0f, 0.5f), trans(4.0f, 5.0f, -6.0f);
        MTX34Scale(&out, &m, &scale);
        PSMTXScale(s, scale.x, scale.y, scale.z);
        PSMTXConcat(m, s, ref);
        CHECK(near(out, ref), "MTX34Scale is not m * scale");
        MTX34Trans(&out, &m, &trans);
        PSMTXTrans(s, trans.x, trans.y, trans.z);
        PSMTXConcat(m, s, ref);
        CHECK(near(out, ref), "MTX34Trans is not m * translation");
        MTX34Trans(&m, &m, &trans);
        CHECK(near(m, ref), "MTX34Trans in place differs");
    }

    // Inverse transpose of the 3x3 part.
    {
        MTX34 m, inv;
        MTX33 it;
        MTX34RotXYZFIdx(&m, 33.0f, -12.0f, 77.0f);
        m._00 *= 3.0f; m._11 *= 0.25f; m._03 = 9.0f;
        CHECK(MTX34InvTranspose(&it, &m) == TRUE, "InvTranspose reports singular");
        PSMTXInverse(m, inv);
        CHECK(near(it._00, inv._00) && near(it._01, inv._10) && near(it._02, inv._20) &&
              near(it._10, inv._01) && near(it._11, inv._11) && near(it._12, inv._21) &&
              near(it._20, inv._02) && near(it._21, inv._12) && near(it._22, inv._22),
              "InvTranspose is not the transpose of the inverse");
        MTX34Zero(&m);
        CHECK(MTX34InvTranspose(&it, &m) == FALSE, "InvTranspose of zero reports success");
    }

    // Component-wise min and max.
    {
        VEC3 a(1.0f, -2.0f, 3.0f), b(-1.0f, 5.0f, 3.0f), out;
        VEC3Maximize(&out, &a, &b);
        CHECK(out.x == 1.0f && out.y == 5.0f && out.z == 3.0f, "VEC3Maximize");
        VEC3Minimize(&out, &a, &b);
        CHECK(out.x == -1.0f && out.y == -2.0f && out.z == 3.0f, "VEC3Minimize");
    }

    CHECK(near(FrSqrt(4.0f), 0.5f), "FrSqrt");

    printf("%s: %d failure(s)\n", __FILE__, failures);
    return failures != 0;
}
