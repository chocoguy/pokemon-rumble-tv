/* Libraries the port stubs on purpose (see the plan): the Home Button Menu, the Wii
 * Remote speaker's encoder; plus the quaternion helpers whose SDK versions were
 * paired-single assembly. */
#include <port_os.h>
#include <revolution/hbm/HBMBase.h>
#include <revolution/wenc.h>
#include <revolution/mtx.h>
#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------------------------ */
/* Home Button Menu: opening it closes it again at once, as if the player chose to return
 * to the game. Nothing is drawn and no sound is made. */

static int sHBMOpen;

void HBMCreate(const HBMDataInfo* info) {
    (void)info;
    sHBMOpen = 1;
}

void HBMCreateInfoEx(const HBMDataInfoEx* info) {
    (void)info;
}

void HBMDelete(void) {
    sHBMOpen = 0;
}

void HBMInit(void) {}

HBMSelectBtnNum HBMCalc(const HBMControllerData* controller) {
    (void)controller;
    return sHBMOpen ? HBM_SELECT_HOMEBTN : HBM_SELECT_NULL;
}

void HBMDraw(void) {}

HBMSelectBtnNum HBMGetSelectBtnNum(void) {
    return sHBMOpen ? HBM_SELECT_HOMEBTN : HBM_SELECT_NULL;
}

void HBMSetAdjustFlag(BOOL flag) {
    (void)flag;
}

void HBMStartBlackOut(void) {}

BOOL HBMIsReassignedControllers(void) {
    return FALSE;
}

void HBMCreateSound(void* soundData, void* memBuf, u32 memSize) {
    (void)soundData;
    (void)memBuf;
    (void)memSize;
}

void HBMDeleteSound(void) {}

void HBMUpdateSound(void) {}

/* ------------------------------------------------------------------------------------ */
/* WENC: ADPCM encoding for the Wii Remote speaker, which the port never feeds. */

s32 WENCGetEncodeData(WENCInfo* info, u32 flag, const s16* pcm, s32 samples, u8* out) {
    (void)info;
    (void)flag;
    (void)pcm;
    memset(out, 0, (size_t)(samples + 1) / 2);
    return samples;
}

/* ------------------------------------------------------------------------------------ */
/* Quaternions (the SDK's quat.c, C versions)                                            */

void PSMTXQuat(Mtx m, const Quaternion* q) {
    f32 s, xs, ys, zs, wx, wy, wz, xx, xy, xz, yy, yz, zz;

    s = 2.0f / (q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w);
    xs = q->x * s;
    ys = q->y * s;
    zs = q->z * s;
    wx = q->w * xs;
    wy = q->w * ys;
    wz = q->w * zs;
    xx = q->x * xs;
    xy = q->x * ys;
    xz = q->x * zs;
    yy = q->y * ys;
    yz = q->y * zs;
    zz = q->z * zs;

    m[0][0] = 1.0f - (yy + zz);
    m[0][1] = xy - wz;
    m[0][2] = xz + wy;
    m[0][3] = 0.0f;
    m[1][0] = xy + wz;
    m[1][1] = 1.0f - (xx + zz);
    m[1][2] = yz - wx;
    m[1][3] = 0.0f;
    m[2][0] = xz - wy;
    m[2][1] = yz + wx;
    m[2][2] = 1.0f - (xx + yy);
    m[2][3] = 0.0f;
}

void C_QUATMtx(Quaternion* r, const Mtx m) {
    f32 tr, s;
    s32 i, j, k;
    s32 nxt[3] = {1, 2, 0};
    f32 q[3];

    tr = m[0][0] + m[1][1] + m[2][2];
    if (tr > 0.0f) {
        s = sqrtf(tr + 1.0f);
        r->w = s * 0.5f;
        s = 0.5f / s;
        r->x = (m[2][1] - m[1][2]) * s;
        r->y = (m[0][2] - m[2][0]) * s;
        r->z = (m[1][0] - m[0][1]) * s;
    } else {
        i = 0;
        if (m[1][1] > m[0][0]) {
            i = 1;
        }
        if (m[2][2] > m[i][i]) {
            i = 2;
        }
        j = nxt[i];
        k = nxt[j];
        s = sqrtf((m[i][i] - (m[j][j] + m[k][k])) + 1.0f);
        q[i] = s * 0.5f;
        if (s != 0.0f) {
            s = 0.5f / s;
        }
        r->w = (m[k][j] - m[j][k]) * s;
        q[j] = (m[i][j] + m[j][i]) * s;
        q[k] = (m[i][k] + m[k][i]) * s;
        r->x = q[0];
        r->y = q[1];
        r->z = q[2];
    }
}

void C_QUATSlerp(const Quaternion* p, const Quaternion* q, Quaternion* r, f32 t) {
    f32 theta, sin_th, cos_th, tp, tq;

    cos_th = p->x * q->x + p->y * q->y + p->z * q->z + p->w * q->w;
    tq = 1.0f;
    if (cos_th < 0.0f) {
        cos_th = -cos_th;
        tq = -tq;
    }
    if (cos_th <= 1.0f - 0.00001f) {
        theta = acosf(cos_th);
        sin_th = sinf(theta);
        tp = sinf((1.0f - t) * theta) / sin_th;
        tq *= sinf(t * theta) / sin_th;
    } else {
        tp = 1.0f - t;
        tq *= t;
    }
    r->x = tp * p->x + tq * q->x;
    r->y = tp * p->y + tq * q->y;
    r->z = tp * p->z + tq * q->z;
    r->w = tp * p->w + tq * q->w;
}
