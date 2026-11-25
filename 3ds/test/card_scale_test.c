// The party cards' enlargement against the divide-per-pixel version it replaced: every pixel equal, at every scale.
// cc -O2 -I3ds/core 3ds/test/card_scale_test.c 3ds/core/pd_card_scale.c && ./a.out
#include "pd_card_scale.h"

#include <stdio.h>
#include <stdlib.h>

static uint32_t old_mix(uint32_t a, uint32_t b, int wa, int wb) {
    uint32_t out = 0;
    for (int s = 0; s <= 16; s += 8) out |= (((a >> s & 0xFF) * (uint32_t) wa + (b >> s & 0xFF) * (uint32_t) wb) / (uint32_t) (wa + wb)) << s;
    return out;
}

static int old_source_of(int o, const struct card_scale* k, int* near) {
    int at = o * k->den, i = at / k->num, end = (i + 1) * k->num;
    *near = end - at >= k->den ? k->den : end - at;
    return i;
}

static void old_widen_row(const uint32_t* src, int srcW, uint32_t* dst, int w, const struct card_scale* k) {
    for (int x = 0; x < w; x++) {
        int near, i = old_source_of(x, k, &near);
        dst[x] = near == k->den || i + 1 >= srcW ? src[i] : old_mix(src[i], src[i + 1], near, k->den - near);
    }
}

static void old_screen_up(uint32_t* dst, int w, int h, const uint32_t* px, int srcW, int srcH, const struct card_scale* k) {
    static uint32_t r[2][320];
    int have[2] = { -1, -1 };
    for (int y = 0; y < h; y++) {
        int near, s = old_source_of(y, k, &near);
        if (s >= srcH) break;
        for (int i = 0; i < 2; i++) {
            int want = s + i < srcH ? s + i : s;
            if (have[want & 1] != want) {
                old_widen_row(px + want * srcW, srcW, r[want & 1], w, k);
                have[want & 1] = want;
            }
        }
        const uint32_t* a = r[s & 1];
        const uint32_t* b = r[(s + 1 < srcH ? s + 1 : s) & 1];
        uint32_t* d = dst + y * w;
        for (int x = 0; x < w; x++) d[x] = near == k->den ? a[x] : old_mix(a[x], b[x], near, k->den - near);
    }
}

int main(void) {
    static const struct card_scale scales[] = { { 4, 3, 0 }, { 5, 4, 0 }, { 1, 1, 0 }, { 3, 2, 0 } };
    static uint32_t src[400 * 400], a[320 * 240], b[320 * 240];
    srand(7);
    long checked = 0;
    for (int round = 0; round < 400; round++) {
        const struct card_scale* k = &scales[round % 4];
        int w = round < 8 ? 320 : 1 + rand() % 320, h = round < 8 ? 240 : 1 + rand() % 240;
        int srcW = (w * k->den + k->num - 1) / k->num + 1, srcH = (h * k->den + k->num - 1) / k->num + 1;
        for (int i = 0; i < srcW * srcH; i++) src[i] = (uint32_t) rand() << 16 ^ (uint32_t) rand();
        for (int i = 0; i < w * h; i++) a[i] = b[i] = 0xDEADBEEF;
        old_screen_up(a, w, h, src, srcW, srcH, k);
        pd_card_screen_up(b, w, h, src, srcW, srcH, k);
        for (int i = 0; i < w * h; i++) {
            if (a[i] != b[i]) {
                printf("MISMATCH round %d scale %d/%d %dx%d at %d: %08X vs %08X\n", round, k->num, k->den, w, h, i, a[i], b[i]);
                return 1;
            }
            checked++;
        }
    }
    printf("ok: %ld pixels equal\n", checked);
    return 0;
}
