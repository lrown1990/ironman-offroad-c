/* The checkpoint and lap service (1f83, 1fdc), the result preparation (3866)
 * and the ranking (38c2): a literal translation of tools/race_rules.py. */
#include "offroad.h"

#define u8(n) ((uint8_t)(n))
#define u16(n) ((uint16_t)(n))
static inline int s8(unsigned n) { return (int8_t)(uint8_t)n; }
static inline unsigned W(const uint8_t *b, unsigned o) { return b[o] | b[o + 1] << 8; }
static inline void put(uint8_t *b, unsigned o, long long v) { b[o] = (uint8_t)v; b[o + 1] = (uint8_t)(v >> 8); }
static void fail(Truck *k, int e) { if (!k->error) k->error = e; }
static unsigned dw(Truck *k, const uint8_t *d, int n, long long o) {
    if (o < 0 || o + 2 > n) { fail(k, OFF_BAD_ADDRESS); return 0; }
    return W(d, (unsigned)o);
}
static unsigned db(Truck *k, const uint8_t *d, int n, long long o) {
    if (o < 0 || o >= n) { fail(k, OFF_BAD_ADDRESS); return 0; }
    return d[o];
}

/* 23e2..2466: unsigned ratio thresholds after signed absolute values; AL
 * only (the caller multiplies by a byte). Not a floating-point atan2. */
uint8_t direction32(unsigned ax, unsigned bx) {
    static const unsigned bounds[8] = {4, 5, 6, 8, 11, 17, 26, 81};
    ax = u16(ax); bx = u16(bx);
    unsigned quadrant = 0;
    if (ax & 32768) { ax = u16(-ax); quadrant = 0xa0; }
    if (bx & 32768) { bx = u16(-bx); quadrant ^= 0xe0; }
    int swapped = ax < bx;
    if (swapped) { unsigned t = ax; ax = bx; bx = t; }
    unsigned long long numerator = ax * 4ul, divisor = bx ? bx : 1;
    unsigned long long quotient = divisor <= numerator >> 16 ? 65535 : numerator / divisor;
    unsigned sector = 0;
    for (int i = 0; i < 8; i++) if (quotient <= bounds[i]) { sector = 8 - i; break; }
    if (swapped) sector = u8(16 - sector);
    int carry = (quadrant & 128) != 0;
    quadrant = u8(quadrant * 2);
    if (carry) sector = u8(32 - sector);
    return u8(u8(sector * 2) | quadrant);
}

unsigned packed_decimal(long long value) {
    value %= 10000;
    if (value < 0) value += 10000;   /* Python's modulo */
    unsigned out = 0;
    for (int i = 0, p = 1; i < 4; i++, p *= 10) out |= (unsigned)((value / p) % 10) << (4 * i);
    return out;
}

int rules_records(Truck *k, const uint8_t *data, int ndata, int layout, uint16_t *out) {
    long long at = dw(k, data, ndata, 0x6b24 + 2 * layout);
    int n = 0;
    while (!k->error) {
        out[n++] = u16(at);
        if (db(k, data, ndata, at + 6) & 128) return n;
        at += 7;
        if (n > 255) { fail(k, OFF_NOT_MODELLED); break; }   /* an unterminated table */
    }
    return n;
}

void rules_reset_route(Truck *k, const uint8_t *data, int ndata, uint8_t *t, int layout) {
    unsigned at = dw(k, data, ndata, 0x6b24 + 2 * layout);
    put(t, 0x3d, at); put(t, 0x3f, at); t[0x41] = 0; put(t, 0x42, 0);
}

void rules_initialize(Truck *k, const uint8_t *data, int ndata, uint8_t *t, int layout) {
    t[0x3c] = 0;
    put(t, 0x44, 0); put(t, 0x46, 0); put(t, 0x48, 0);
    rules_reset_route(k, data, ndata, t, layout);
}

/* 1fdc: the candidate coordinate's HIGH bytes; the edges are included unevenly */
int rules_region(Truck *k, const uint8_t *data, int ndata, unsigned at, unsigned x, unsigned y) {
    if ((long long)at + 7 > ndata) { fail(k, OFF_BAD_ADDRESS); return -1; }
    int xmin = data[at], xmax = data[at + 1], ymax = data[at + 2], ymin = data[at + 3];
    unsigned base = data[at + 4], scale = data[at + 5], kind = data[at + 6];
    int xx = (x >> 8) & 255, yy = (y >> 8) & 255;
    if (!(xmin <= xx && xx < xmax && ymin <= yy && yy <= ymax)) return -1;
    kind &= 15;
    unsigned distance;
    if (kind < 4) {
        int d[4] = {xx - xmin, ymax - yy, xmax - xx, yy - ymin};
        distance = (unsigned)d[kind];
    } else {
        int pairs[8][2] = {{yy - ymin, xx - xmin}, {xx - xmin, ymax - yy}, {ymax - yy, xmax - xx}, {xmax - xx, yy - ymin},
                           {ymax - yy, xx - xmin}, {xx - xmin, yy - ymin}, {yy - ymin, xmax - xx}, {xmax - xx, ymax - yy}};
        int a = pairs[kind - 4][0], b = pairs[kind - 4][1];
        distance = u16(direction32(u16(s8(u8(a))), u16(s8(u8(b)))) * scale * 4) >> 8;
    }
    return u8(distance + base);
}

unsigned rules_update(Truck *k, const uint8_t *data, int ndata, uint8_t *t, unsigned x, unsigned y, int layout, unsigned flags) {
    put(t, 0x44, W(t, 0x44) + 1);
    unsigned at = W(t, 0x3f);
    int progress = rules_region(k, data, ndata, at, x, y);
    if (progress < 0) {
        progress = rules_region(k, data, ndata, W(t, 0x3d), x, y);
        if (progress >= 0) put(t, 0x42, progress);
        return flags;
    }
    put(t, 0x42, progress); put(t, 0x3d, at); put(t, 0x3f, at + 7); t[0x41] = u8(t[0x41] + 1);
    if (db(k, data, ndata, at + 6) & 128) {
        put(t, 0x46, W(t, 0x44)); t[0x3c] = u8(t[0x3c] + 1); flags |= 0x20;
        if (t[0x3c] == 3) flags |= 1;
        if (t[0x3c] == 4) flags |= 2;
        rules_reset_route(k, data, ndata, t, layout);
    }
    return flags;
}

void rules_normalize_progress(Truck *k, const uint8_t *data, int ndata, uint8_t *t, int layout) {
    unsigned long long total = dw(k, data, ndata, 0x9ccc + layout * 2), progress = W(t, 0x42);
    put(t, 0x42, total <= progress ? 65535 : (progress << 16) / total);
}

/* 3866: the partial lap extrapolated, the cap, the mean and the packed counter */
void rules_result_time(uint8_t *t) {
    unsigned laps = t[0x3c], elapsed = u16(W(t, 0x44) - W(t, 0x46)), fraction = W(t, 0x42);
    if (fraction) {
        unsigned value = 2000;
        if (fraction > elapsed) value = u16((((unsigned long long)elapsed << 16) | 2001) / fraction + W(t, 0x46));
        put(t, 0x44, value < 2000 ? value : 2000);
        laps += 1;
    }
    put(t, 0x48, laps ? W(t, 0x44) / laps : 65535);
    unsigned value = W(t, 0x44);
    put(t, 0x46, value);
    put(t, 0x44, value < 2000 ? packed_decimal(value * 5) : 0x9999);
}

void rules_rating(Truck *k, const uint8_t *data, int ndata, uint8_t *t, int layout) {
    if (t[2] & 2) return;
    unsigned table = dw(k, data, ndata, 0x7290 + layout * 2), rating = 0;
    while (rating < 17 && dw(k, data, ndata, table + rating * 2) >= W(t, 0x48) && !k->error) rating++;
    rating = u8(rating - db(k, data, ndata, 0x7280 + layout));
    unsigned v = rating & 128 ? 0 : rating;
    t[0x5c] = u8(v < 17 ? v : 17);
}

void rules_prepare_result(Truck *k, const uint8_t *data, int ndata, uint8_t *t, int layout) {
    t[2] &= 0xf6;
    rules_normalize_progress(k, data, ndata, t, layout);
    rules_result_time(t);
    rules_rating(k, data, ndata, t, layout);
}

/* 38c2: stable, descending on (laps, normalized progress); the rank bytes follow */
void rules_rank(Truck *k, uint8_t *trucks[4], const int len[4], int order[4]) {
    for (;;) {
        int changed = 0;
        for (int j = 0; j < 3; j++) {
            int ia = order[j], ib = order[j + 1];
            if (len[ia] < 0x44 || len[ib] < 0x44) { fail(k, OFF_BAD_ADDRESS); return; }
            uint8_t *a = trucks[ia], *b = trucks[ib];
            if (a[0x3c] < b[0x3c] || (a[0x3c] == b[0x3c] && W(a, 0x42) < W(b, 0x42))) {
                order[j] = ib; order[j + 1] = ia;
                a[0x3b] = u8(a[0x3b] + 1); b[0x3b] = u8(b[0x3b] - 1); changed = 1;
            }
        }
        if (!changed) return;
    }
}
