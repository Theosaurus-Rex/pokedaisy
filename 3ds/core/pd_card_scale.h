// The party cards' screen enlarged by num / den (4/3, 5/4), a pixel blended from its two nearest source pixels the way
// sharp bilinear does on the top screen.
#pragma once

#include <stdint.h>

struct card_scale {
    int num, den, extraW;
};

// [w] x [h] pixels of [dst] from [src] (srcW x srcH). [w] is at most 320.
void pd_card_screen_up(uint32_t* dst, int w, int h, const uint32_t* src, int srcW, int srcH, const struct card_scale* k);
