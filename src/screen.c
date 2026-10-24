/* The 320 x 200 picture the monitor shows (tools/graphics.py, screen): the
 * page at the start address, the colours through the attribute registers
 * and the DAC (6-bit components to 8 bits as (v << 2) | (v >> 4)). */
#include "offroad.h"

void gfx_screen(const Gfx *g, uint8_t rgb[320 * 200 * 3]) {
    unsigned mode = g->attr[0x10], select = g->attr[0x14];
    uint8_t colour[16][3];
    for (int value = 0; value < 16; value++) {
        unsigned v = g->attr[value] & 0x3f;
        unsigned index = mode & 0x80 ? ((select & 0x0f) << 4 | (v & 0x0f)) : ((select & 0x0c) << 4 | v);
        for (int k = 0; k < 3; k++) { unsigned c = g->dac[3 * index + k]; colour[value][k] = (uint8_t)((c << 2) | (c >> 4)); }
    }
    uint8_t *out = rgb;
    for (int y = 0; y < 200; y++) {
        unsigned row = g->start + 40u * y;
        for (int xb = 0; xb < 40; xb++) {
            unsigned a = (row + xb) & 0xffff;
            unsigned b0 = g->planes[0][a], b1 = g->planes[1][a], b2 = g->planes[2][a], b3 = g->planes[3][a];
            for (int bit = 7; bit >= 0; bit--) {
                unsigned c = (b0 >> bit & 1) | (b1 >> bit & 1) << 1 | (b2 >> bit & 1) << 2 | (b3 >> bit & 1) << 3;
                *out++ = colour[c][0]; *out++ = colour[c][1]; *out++ = colour[c][2];
            }
        }
    }
}
