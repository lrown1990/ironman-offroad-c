/* The formats of the game's files the start and the race read: the sprite
 * compiler of the load (tools/extract_sprites.py converted_stream) and the
 * planes of a picture (tools/extract_backgrounds.py decode_plane). */
#include <string.h>
#include "offroad.h"

long long sprite_converted(const uint8_t *raw, long long nraw, unsigned rb, unsigned h, unsigned np,
                      unsigned flags, uint8_t *out, long long cap) {
#define OUT(v) do { if (n >= cap) return -1; out[n++] = (uint8_t)(v); } while (0)
#define RAW(i) ((long long)(i) < nraw ? raw[i] : 0)
    long long n = 0;
    if (flags == 0x87 || flags == 0x01) {
        for (unsigned y = 0; y < h; y++) {
            long long row = (long long)y * rb * np, len = (long long)rb * np;
            /* the model slices b[y*rb*np:(y+1)*rb*np]: shorter at the end of raw */
            if (row + len > nraw) len = nraw > row ? nraw - row : 0;
            if (flags & 0x80)
                for (unsigned x = 0; x < rb; x++) {
                    if ((long long)(x + 2 * rb) >= len) return -1;   /* the model's index error */
                    OUT(raw[row + x] | raw[row + x + rb] | raw[row + x + 2 * rb]);
                }
            for (long long i = 0; i < len; i++) OUT(raw[row + i]);
        }
        return n;
    }
    if (flags != 0x0f) return -1;
    uint8_t mask[256];
    if (rb > sizeof mask) return -1;
    for (unsigned y = 0; y < h; y++) {
        unsigned first = 0x80;
        for (unsigned color = 0; color < 15; color++) {
            memset(mask, 255, rb);
            for (unsigned p = 0; p < 4; p++)
                for (unsigned x = 0; x < rb; x++) {
                    long long at = (long long)(y * 4 + p) * rb + x;
                    if (at >= nraw) return -1;
                    unsigned source = raw[at];
                    mask[x] &= color & (1u << p) ? (source ^ 255) : source;
                }
            int any = 0;
            for (unsigned x = 0; x < rb; x++) any |= mask[x];
            if (any) { OUT(first | color); for (unsigned x = 0; x < rb; x++) OUT(mask[x]); first = 0; }
        }
        if (first) { OUT(0x80); for (unsigned x = 0; x < rb; x++) OUT(0); }
    }
    OUT(0xff);
    return n;
#undef OUT
#undef RAW
}

long long file_decode_plane(const uint8_t *data, long long n, long long position, uint8_t out[8000]) {
    memset(out, 0, 8000);
    for (int column = 0; column < 40; column++) {
        int row = 0;
        while (row < 200) {
            if (position >= n) return -1;   /* a truncated literal */
            unsigned value = data[position++];
            out[row * 40 + column] = (uint8_t)(value ^ 255); row++;
            /* a literal on the last row has no repeat count, even for 00/ff */
            if (row == 200) break;
            if (value == 0 || value == 255) {
                if (position >= n) return -1;   /* a truncated run */
                unsigned count = data[position++];
                if (row + (int)count > 200) return -1;   /* a run past the column */
                for (unsigned i = 0; i < count; i++) { out[row * 40 + column] = (uint8_t)(value ^ 255); row++; }
            }
        }
    }
    return position;
}
