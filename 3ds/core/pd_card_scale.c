#include "pd_card_scale.h"

#include <string.h>

#define MAX_W 320

// a * wa + b * wb over den (wa + wb), each channel of 0xRRGGBB. With den a constant the divide is a multiply: the ARM11
// has no divide instruction, and the library's one took most of the time.
static inline __attribute__((always_inline)) uint32_t mix(uint32_t a, uint32_t b, uint32_t wa, uint32_t wb, uint32_t den) {
    uint32_t r = ((a & 0xFF) * wa + (b & 0xFF) * wb) / den;
    uint32_t g = ((a >> 8 & 0xFF) * wa + (b >> 8 & 0xFF) * wb) / den;
    uint32_t bl = ((a >> 16 & 0xFF) * wa + (b >> 16 & 0xFF) * wb) / den;
    return r | g << 8 | bl << 16;
}

// Output pixel o covers [o * den, o * den + den) in num-ths of a source pixel: one source pixel, or the end of one and the
// start of the next. near is den when it's one.
static int source_of(int o, const struct card_scale* k, int* near) {
    int at = o * k->den, i = at / k->num, end = (i + 1) * k->num;
    *near = end - at >= k->den ? k->den : end - at;
    return i;
}

static inline __attribute__((always_inline)) void up(uint32_t* dst, int w, int h, const uint32_t* src, int srcW, int srcH,
                                                   const struct card_scale* k, int den, const short* colI, const unsigned char* colNear) {
    static uint32_t r[2][MAX_W];
    int have[2] = { -1, -1 };
    for (int y = 0; y < h; y++) {
        int near, s = source_of(y, k, &near);
        if (s >= srcH) break;
        for (int i = 0; i < 2; i++) {
            int want = s + i < srcH ? s + i : s;
            if (have[want & 1] != want) {
                const uint32_t* row = src + want * srcW;
                uint32_t* out = r[want & 1];
                for (int x = 0; x < w; x++) {
                    int i0 = colI[x], n = colNear[x];
                    out[x] = n == den || i0 + 1 >= srcW ? row[i0] : mix(row[i0], row[i0 + 1], (uint32_t) n, (uint32_t) (den - n), (uint32_t) den);
                }
                have[want & 1] = want;
            }
        }
        const uint32_t* a = r[s & 1];
        const uint32_t* b = r[(s + 1 < srcH ? s + 1 : s) & 1];
        uint32_t* d = dst + y * w;
        if (near == den) memcpy(d, a, (size_t) w * sizeof(*d));
        else
            for (int x = 0; x < w; x++) d[x] = mix(a[x], b[x], (uint32_t) near, (uint32_t) (den - near), (uint32_t) den);
    }
}

void pd_card_screen_up(uint32_t* dst, int w, int h, const uint32_t* src, int srcW, int srcH, const struct card_scale* k) {
    // The columns' source pixel and weight, worked out once for a scale and width.
    static short colI[MAX_W];
    static unsigned char colNear[MAX_W];
    static int forNum, forDen, forW;
    if (w > MAX_W) w = MAX_W;
    if (forNum != k->num || forDen != k->den || forW != w) {
        for (int x = 0; x < w; x++) {
            int near;
            colI[x] = (short) source_of(x, k, &near);
            colNear[x] = (unsigned char) near;
        }
        forNum = k->num;
        forDen = k->den;
        forW = w;
    }
    switch (k->den) {
    case 3: up(dst, w, h, src, srcW, srcH, k, 3, colI, colNear); break;
    case 4: up(dst, w, h, src, srcW, srcH, k, 4, colI, colNear); break;
    default: up(dst, w, h, src, srcW, srcH, k, k->den, colI, colNear); break;
    }
}
