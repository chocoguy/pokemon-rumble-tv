/* The port's changes to the game's compiled scripts: analog movement for the player.
 *
 * The player's walking lives in CPlayerPii.pkc, not in C++. Each frame its walk routine
 * (code word 14316) asks whether the stick is pushed at all (GetPadDirValue > 0); if so it
 * walks at a fixed speed, the Pokemon's own (routine 7616: its speed stat x 0.0007, x1.3 or
 * x1.6 on some stages), turns 20% of the way toward the stick's camera-relative angle, and
 * keeps the walk motion playing at a fixed rate (routine 7774 -> 7639, which starts it at
 * the Pokemon's walk-animation rate / 60 and never changes it). How far the stick is
 * pushed made no difference.
 *
 * The patches, applied to a copy of the script after its byte-order fixup:
 *   14332  CALL 7616 -> CALL walkSpeed   (appended): 7616's speed x PortStickWalkScale
 *   14431  CALL 7774 -> CALL walkMotion  (appended): 7774, then the walk motion (global 41)
 *          set to walk-animation rate / 60 x PortStickWalkScale (PortMpSetSpeed)
 *   14388  the turn blend 0.2 -> kTurnBlend
 * PortStickWalkScale and PortMpSetSpeed are port natives appended to the game's native
 * table (ScriptInput.cpp, ScriptModel.cpp). The speed is what the routine also stores as
 * the momentum the Pokemon slides on after the stick is released, so that scales too.
 * Appending code moves nothing: jumps and calls are absolute word indices, and strings
 * are found from the header's sizes.
 *
 * Every word a patch replaces or relies on is checked first (the code size, the call
 * sites, routine 7616, the walk-rate computation in 7639 and the natives they call); a
 * script that differs is loaded unpatched, with a [RED] line. */
#include <port_endian.h>
#include <port_os.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* How far toward the stick's direction the Pokemon turns each frame (the game's 0.2). */
static const float kTurnBlend = 0.4f;

enum {
    OP_MUL = 0x07,
    OP_DIV = 0x08,
    OP_LOAD = 0x19,
    OP_CALL = 0x1A,
    OP_CALL_NATIVE = 0x1B,
    OP_RETURN = 0x1E,
    OP_POP = 0x1F,
    OP_ENTER = 0x20,
    OP_PUSH_THIS = 0x27,
    OP_PUSH_SHORT = 0x40,
    OP_PUSH_FLOAT = 0x41,
};

#define W(op, arg) (((uint32_t)(op) << 24) | ((uint32_t)(arg) & 0xFFFFFF))

/* The .pkc header after PortFixupPKC (NPpu::CCode::SHeader): native words. */
typedef struct {
    uint32_t magic;
    uint32_t stackSize;
    uint32_t codeSize;
    uint32_t wideDataSize;
    uint32_t wideCount;
    uint32_t stringDataSize;
    uint32_t stringCount;
    uint32_t reserved;
} Header;

/* CPlayerPii.pkc as shipped (USA): code size and the words the patches check. */
enum {
    PLAYER_CODE_SIZE = 61272,
    AT_WALK_SPEED_CALL = 14332,
    AT_WALK_MOTION_CALL = 14431,
    AT_TURN_BLEND = 14388,
    WALK_SPEED_FUNC = 7616,
    WALK_MOTION_FUNC = 7774,
    WALK_RATE_AT = 7685, /* in 7639: THIS getPiiProp pprGetWalkAnmRate MUL PUSHF 60 DIV playEx STORE 41 */
    GLOBAL_WALK_MOTION = 41,
};

static float FloatOf(uint32_t w) {
    float f;
    memcpy(&f, &w, 4);
    return f;
}

static uint32_t WordOf(float f) {
    uint32_t w;
    memcpy(&w, &f, 4);
    return w;
}

typedef struct {
    int getPlayerIndex;
    int getPiiProp;
    int walkAnmRate;
    int playEx;
    int padDirValue;
    int walkScale;
    int setSpeed;
} Natives;

static int FindNatives(Natives* n) {
    n->getPlayerIndex = PortScriptNativeIndex("method_getPlayerIndex");
    n->getPiiProp = PortScriptNativeIndex("method_getPiiProp");
    n->walkAnmRate = PortScriptNativeIndex("method_pprGetWalkAnmRate");
    n->playEx = PortScriptNativeIndex("method_playEx");
    n->padDirValue = PortScriptNativeIndex("GetPadDirValue");
    n->walkScale = PortScriptNativeIndex("PortStickWalkScale");
    n->setSpeed = PortScriptNativeIndex("PortMpSetSpeed");
    return n->getPlayerIndex >= 0 && n->getPiiProp >= 0 && n->walkAnmRate >= 0 && n->playEx >= 0 &&
           n->padDirValue >= 0 && n->walkScale >= 0 && n->setSpeed >= 0;
}

/* Whether CPlayerPii.pkc's code is the one the patches were written against. */
static int PlayerScriptMatches(const uint32_t* code, uint32_t words, const Natives* n) {
    static const struct {
        uint32_t at;
        uint32_t op;
        int arg; /* -1: a float, checked separately */
    } kExpect[] = {
        {14322, OP_PUSH_THIS, 0},
        {14332, OP_CALL, WALK_SPEED_FUNC},
        {14387, OP_PUSH_FLOAT, 0},
        {14431, OP_CALL, WALK_MOTION_FUNC},
        {7616, OP_ENTER, 0},
        {7618, OP_PUSH_THIS, 0},
        {7620, OP_PUSH_FLOAT, 0},
        {7622, OP_MUL, 0},
        {7623, OP_RETURN, 0},
        {7774, OP_ENTER, 0},
        {7685, OP_PUSH_THIS, 0},
        {7688, OP_MUL, 0},
        {7689, OP_PUSH_FLOAT, 0},
        {7691, OP_DIV, 0},
    };
    size_t i;
    if (words != PLAYER_CODE_SIZE / 4) {
        return 0;
    }
    for (i = 0; i < sizeof(kExpect) / sizeof(kExpect[0]); i++) {
        if (code[kExpect[i].at] != W(kExpect[i].op, kExpect[i].arg)) {
            return 0;
        }
    }
    return code[14323] == W(OP_CALL_NATIVE, n->getPlayerIndex) && code[14324] == W(OP_CALL_NATIVE, n->padDirValue) &&
           code[7617] == 0x01000000u && code[7775] == 0x01000000u && FloatOf(code[7621]) == 0.0007f &&
           FloatOf(code[AT_TURN_BLEND]) == 0.2f && code[7686] == W(OP_CALL_NATIVE, n->getPiiProp) &&
           code[7687] == W(OP_CALL_NATIVE, n->walkAnmRate) && FloatOf(code[7690]) == 60.0f &&
           code[7692] == W(OP_CALL_NATIVE, n->playEx) && code[7693] == W(0x0A, GLOBAL_WALK_MOTION);
}

/* The appended routines; returns their length in words. `base` is the first new word's
 * index. Each runs in a frame like 7616's (ENTER: frame base 1, no arguments, no locals). */
static uint32_t Append(uint32_t* out, uint32_t base, const Natives* n, uint32_t* walkSpeed, uint32_t* walkMotion) {
    uint32_t k = 0;

    /* walkSpeed: return 7616() * PortStickWalkScale(this.getPlayerIndex()) */
    *walkSpeed = base + k;
    out[k++] = W(OP_ENTER, 0);
    out[k++] = 0x01000000u;
    out[k++] = W(OP_CALL, WALK_SPEED_FUNC);
    out[k++] = W(OP_PUSH_THIS, 0);
    out[k++] = W(OP_CALL_NATIVE, n->getPlayerIndex);
    out[k++] = W(OP_CALL_NATIVE, n->walkScale);
    out[k++] = W(OP_MUL, 0);
    out[k++] = W(OP_RETURN, 0);

    /* walkMotion: 7774(); PortMpSetSpeed(this, g41, this.getPiiProp().pprGetWalkAnmRate() / 60
     *             * PortStickWalkScale(this.getPlayerIndex())); return 0 */
    *walkMotion = base + k;
    out[k++] = W(OP_ENTER, 0);
    out[k++] = 0x01000000u;
    out[k++] = W(OP_CALL, WALK_MOTION_FUNC);
    out[k++] = W(OP_POP, 0);
    out[k++] = W(OP_PUSH_THIS, 0);
    out[k++] = W(OP_LOAD, GLOBAL_WALK_MOTION);
    out[k++] = W(OP_PUSH_THIS, 0);
    out[k++] = W(OP_CALL_NATIVE, n->getPiiProp);
    out[k++] = W(OP_CALL_NATIVE, n->walkAnmRate);
    out[k++] = W(OP_PUSH_FLOAT, 0);
    out[k++] = WordOf(60.0f);
    out[k++] = W(OP_DIV, 0);
    out[k++] = W(OP_PUSH_THIS, 0);
    out[k++] = W(OP_CALL_NATIVE, n->getPlayerIndex);
    out[k++] = W(OP_CALL_NATIVE, n->walkScale);
    out[k++] = W(OP_MUL, 0);
    out[k++] = W(OP_CALL_NATIVE, n->setSpeed);
    out[k++] = W(OP_POP, 0);
    out[k++] = W(OP_PUSH_SHORT, 0);
    out[k++] = W(OP_RETURN, 0);
    return k;
}

#define MAX_APPENDED 64

static const void* PatchPlayer(const void* file, size_t* size) {
    static void* sPatched;
    static size_t sPatchedSize;
    static int sReported;
    const Header* h = (const Header*)file;
    const uint32_t* code = (const uint32_t*)(h + 1);
    uint32_t appended[MAX_APPENDED];
    uint32_t walkSpeed, walkMotion, extra;
    size_t tail;
    Natives n;
    uint8_t* out;
    Header* oh;
    uint32_t* ocode;

    if (sPatched != NULL) {
        *size = sPatchedSize;
        return sPatched;
    }
    if (*size < sizeof(Header) || h->codeSize > *size - sizeof(Header) || !FindNatives(&n) ||
        !PlayerScriptMatches(code, h->codeSize / 4, &n)) {
        PortRedOnce(&sReported, "script patch: CPlayerPii.pkc is not the expected version; movement left as is");
        return file;
    }
    extra = Append(appended, h->codeSize / 4, &n, &walkSpeed, &walkMotion);
    tail = *size - sizeof(Header) - h->codeSize; /* wide strings and strings */

    out = (uint8_t*)PortGameAlloc(*size + extra * 4);
    oh = (Header*)out;
    *oh = *h;
    oh->codeSize = h->codeSize + extra * 4;
    ocode = (uint32_t*)(oh + 1);
    memcpy(ocode, code, h->codeSize);
    memcpy(ocode + h->codeSize / 4, appended, extra * 4);
    memcpy((uint8_t*)(ocode + oh->codeSize / 4), (const uint8_t*)(code + h->codeSize / 4), tail);

    ocode[AT_WALK_SPEED_CALL] = W(OP_CALL, walkSpeed);
    ocode[AT_WALK_MOTION_CALL] = W(OP_CALL, walkMotion);
    ocode[AT_TURN_BLEND] = WordOf(kTurnBlend);

    sPatched = out;
    sPatchedSize = *size + extra * 4;
    *size = sPatchedSize;
    PortLog("script patch: CPlayerPii.pkc walks by stick distance (%u words appended)", extra);
    return sPatched;
}

const void* PortPatchScript(const char* name, const void* file, size_t* size) {
    const char* base = strrchr(name, '/');
    base = base != NULL ? base + 1 : name;
    if (getenv("PORT_NO_SCRIPT_PATCH") != NULL) {
        return file;
    }
    if (strcmp(base, "CPlayerPii.pkc") == 0) {
        return PatchPlayer(file, size);
    }
    return file;
}
