/* Runtime checks for the port's C versions of the SDK matrix and vector functions.
 * Built and run by tests/native/run.sh. */
#include <revolution/mtx.h>
#include <math.h>
#include <stdio.h>

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int near(f32 a, f32 b) { return fabsf(a - b) <= 1e-5f * (1.0f + fabsf(a) + fabsf(b)); }

static int mtx_near(const Mtx a, const Mtx b) {
    int i, j;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 4; j++)
            if (!near(a[i][j], b[i][j])) return 0;
    return 1;
}

int main(void) {
    Mtx id, a, b, ab, inv, t, r;
    Vec v = {1.0f, 2.0f, 3.0f}, w, axis = {0.0f, 0.0f, 2.0f};

    PSMTXIdentity(id);

    /* A rotation, scale and translation, and its inverse. */
    PSMTXRotRad(a, 'y', 0.7f);
    PSMTXScaleApply(a, a, 2.0f, 3.0f, 0.5f);
    PSMTXTransApply(a, a, 4.0f, -5.0f, 6.0f);
    CHECK(PSMTXInverse(a, inv) == 1, "inverse of a regular matrix reports singular");
    PSMTXConcat(a, inv, t);
    CHECK(mtx_near(t, id), "a * inverse(a) is not the identity");
    PSMTXConcat(inv, a, t);
    CHECK(mtx_near(t, id), "inverse(a) * a is not the identity");

    PSMTXScale(t, 1.0f, 0.0f, 1.0f);
    CHECK(PSMTXInverse(t, inv) == 0, "inverse of a singular matrix reports success");

    /* Concatenation applies the right-hand matrix first, and the result may alias. */
    PSMTXTrans(b, 1.0f, 0.0f, 0.0f);
    PSMTXRotRad(r, 'z', (f32)M_PI / 2.0f);
    PSMTXConcat(r, b, ab);          /* translate, then rotate */
    PSMTXMultVec(ab, &(Vec){0.0f, 0.0f, 0.0f}, &w);
    CHECK(near(w.x, 0.0f) && near(w.y, 1.0f) && near(w.z, 0.0f), "concat order: got %f %f %f", w.x, w.y, w.z);
    PSMTXCopy(r, t);
    PSMTXConcat(t, b, t);
    CHECK(mtx_near(t, ab), "concat into its own source differs");

    /* Rotations follow the right-hand rule, as on the Wii. */
    PSMTXRotRad(r, 'x', (f32)M_PI / 2.0f);
    PSMTXMultVecSR(r, &(Vec){0.0f, 1.0f, 0.0f}, &w);
    CHECK(near(w.z, 1.0f), "x rotation sends +y to %f %f %f", w.x, w.y, w.z);
    PSMTXRotRad(r, 'y', (f32)M_PI / 2.0f);
    PSMTXMultVecSR(r, &(Vec){0.0f, 0.0f, 1.0f}, &w);
    CHECK(near(w.x, 1.0f), "y rotation sends +z to %f %f %f", w.x, w.y, w.z);
    PSMTXRotAxisRad(t, &axis, (f32)M_PI / 2.0f);
    PSMTXRotRad(r, 'z', (f32)M_PI / 2.0f);
    CHECK(mtx_near(t, r), "axis rotation about +z differs from the z rotation");

    /* Point versus direction transforms, and the array forms. */
    {
        Vec src[3] = {{1, 2, 3}, {-1, 0, 4}, {0, 0, 0}}, dst[3], one;
        int i;
        PSMTXMultVecArray(a, src, dst, 3);
        for (i = 0; i < 3; i++) {
            PSMTXMultVec(a, &src[i], &one);
            CHECK(near(one.x, dst[i].x) && near(one.y, dst[i].y) && near(one.z, dst[i].z), "array transform %d differs", i);
        }
        PSMTXMultVec(a, &src[2], &one);
        CHECK(near(one.x, 4.0f) && near(one.y, -5.0f) && near(one.z, 6.0f), "origin should map to the translation");
        PSMTXMultVecSR(a, &src[2], &one);
        CHECK(near(one.x, 0.0f) && near(one.y, 0.0f) && near(one.z, 0.0f), "direction transform must ignore translation");
    }
    {
        Mtx src[2], dst[2];
        PSMTXCopy(a, src[0]);
        PSMTXCopy(b, src[1]);
        PSMTXConcatArray(r, src, dst, 2);
        PSMTXConcat(r, a, t);
        CHECK(mtx_near(dst[0], t), "concat array element 0 differs");
        PSMTXConcat(r, b, t);
        CHECK(mtx_near(dst[1], t), "concat array element 1 differs");
    }

    /* Vectors */
    CHECK(near(PSVECMag(&v), sqrtf(14.0f)), "magnitude");
    CHECK(near(PSVECMag(&(Vec){0, 0, 0}), 0.0f), "magnitude of zero");
    PSVECNormalize(&v, &w);
    CHECK(near(PSVECMag(&w), 1.0f), "normalised length %f", PSVECMag(&w));
    CHECK(near(PSVECDotProduct(&v, &(Vec){4, -5, 6}), 12.0f), "dot product");
    PSVECCrossProduct(&(Vec){1, 0, 0}, &(Vec){0, 1, 0}, &w);
    CHECK(near(w.z, 1.0f) && near(w.x, 0.0f) && near(w.y, 0.0f), "x cross y should be z");
    w = v;
    PSVECCrossProduct(&w, &(Vec){0, 0, 1}, &w);
    CHECK(near(w.x, 2.0f) && near(w.y, -1.0f) && near(w.z, 0.0f), "aliased cross product: %f %f %f", w.x, w.y, w.z);
    PSVECAdd(&v, &v, &w);
    CHECK(near(w.x, 2.0f) && near(w.y, 4.0f) && near(w.z, 6.0f), "add");
    CHECK(near(PSVECSquareDistance(&v, &(Vec){1, 0, 0}), 13.0f), "square distance");

    printf("%s: %d failure(s)\n", __FILE__, failures);
    return failures != 0;
}
