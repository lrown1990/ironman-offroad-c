/* The end of a race, the awards and the Ironman Club records: a literal
 * translation of tools/race_awards.py, on the data segment.
 * 371e finish_race   3752 result preparation x4, 38c2 ranking, 3764 awards x4.
 * 3764 award         human trucks only: the packed-BCD prize by place (table
 *                    9cc8) plus +62, added with DAA to money +0f and score
 *                    +11; the winner's personal best (+13), the track's ten
 *                    winning times (9b3d), the record time and initials
 *                    (9b0d/9b1d); every human: the running mean of +46 per
 *                    track (+23 count, +24 mean) and the ten mean times (9bfd).
 * 38ff sort_times, 391e find_time, 3936 to_bcd4.
 * 2f76 driver_out, 3613 driver_login (object script handlers), 36f6
 * sort_drivers, 367f/36b3 the file ironhi.bin = 'RI' + DS:98b9..9cbc.
 */
#include "offroad.h"

#define u8(n) ((uint8_t)(n))
#define u16(n) ((uint16_t)(n))
static inline int s8(unsigned n) { return (int8_t)(uint8_t)n; }
static inline unsigned rw(const uint8_t *m, long long a) { return m[a & 65535] | m[(a + 1) & 65535] << 8; }
static inline void ww(uint8_t *m, long long a, unsigned v) { m[a & 65535] = u8(v); m[(a + 1) & 65535] = u8(v >> 8); }
static const uint16_t TRUCKS[4] = {0x4c72, 0x4d34, 0x4df6, 0x4eb8};

/* ADD/ADC AL,value then DAA, Intel semantics with AF */
unsigned add_daa(unsigned al, unsigned value, unsigned carry, unsigned *cf_out) {
    int af = (al & 15) + (value & 15) + carry > 15;
    unsigned total = al + value + carry;
    int cf = total > 255;
    al = total & 255;
    unsigned out = al;
    if ((al & 15) > 9 || af) out = (out + 6) & 255;
    if (al > 0x99 || cf) { out = (out + 0x60) & 255; cf = 1; }
    if (cf_out) *cf_out = (unsigned)cf;
    return out;
}

/* 38ff: ascending, unsigned, until a pass makes no swap; zero ends the list */
void awards_sort_times(uint8_t *m, unsigned at) {
    for (;;) {
        int swaps = 0;
        long long p = at;
        for (;;) {
            unsigned a = rw(m, p);
            p = (p + 2) & 65535;
            unsigned b = rw(m, p);
            if (!b) break;
            if (a > b) { ww(m, p, a); ww(m, p - 2, b); swaps++; }
        }
        if (!swaps) return;
    }
}

int awards_find_time(uint8_t *m, unsigned at, unsigned value) {
    int i = 0;
    while (rw(m, at)) {
        if (rw(m, at) == value) return i;
        at = (at + 2) & 65535; i++;
    }
    return -1;
}

static void fail(Truck *k, int e) { if (!k->error) k->error = e; }
static unsigned mb(Truck *k, const uint8_t *m, long long a) {
    if (a < 0 || a > 65535) { fail(k, OFF_BAD_ADDRESS); return 0; }
    return m[a];
}
static void mset(Truck *k, uint8_t *m, long long a, unsigned v) {
    if (a < 0 || a > 65535) { fail(k, OFF_BAD_ADDRESS); return; }
    m[a] = u8(v);
}

/* 3764 for one truck; track is the byte at DS:01aa */
void awards_award(Truck *k, uint8_t *m, unsigned di, unsigned track) {
    if (mb(k, m, di + 2) & 2 || k->error) return;
    unsigned cl = add_daa(m[(0x9cc8 + mb(k, m, di + 0x3b)) & 65535], mb(k, m, di + 0x62), 0, 0), cf;
    for (unsigned field = 0x0f; field <= 0x11; field += 2) {
        unsigned lo = add_daa(mb(k, m, di + field), cl, 0, &cf), hi = add_daa(mb(k, m, di + field + 1), 0, cf, 0);
        mset(k, m, di + field, lo); mset(k, m, di + field + 1, hi);
    }
    if (k->error) return;
    if (!m[di + 0x3b]) {
        unsigned time = rw(m, di + 0x44), best = (di + 0x13 + track * 2) & 65535;
        if (time < rw(m, best)) ww(m, best, time);
        unsigned list = (0x9b3d + track * 0x18) & 65535;
        ww(m, list + 0x14, time); awards_sort_times(m, list);
        unsigned kk = u16(s8(track) * 2);
        if (time < rw(m, 0x9b0d + kk)) {
            ww(m, 0x9b0d + kk, time);
            unsigned name = (0x9b1d + u16(kk * 2)) & 65535;
            ww(m, name, rw(m, di + 5)); m[(name + 2) & 65535] = m[(di + 7)];
        }
    }
    unsigned kk = u16(s8(u8(track * 3)));
    unsigned count = m[(kk + di + 0x23) & 65535];
    unsigned long long mean = rw(m, kk + di + 0x24), n = count + 1;
    mean = (mean * count + rw(m, di + 0x46)) / n;
    count = n & 255;
    if (count & 128) count = ((count >> 1) + 1) & 255;
    m[(kk + di + 0x23) & 65535] = u8(count); ww(m, kk + di + 0x24, (unsigned)mean);
    unsigned list = (0x9bfd + track * 0x18) & 65535;
    ww(m, list + 0x14, mean < 2000 ? packed_decimal(u16(mean * 5)) : 0x9999);
    awards_sort_times(m, list);
}

/* 371e: layout from DS:01a9, track from DS:01aa, the ranking pointers at DS:9cec */
void awards_finish_race(Truck *k, uint8_t *m) {
    int layout = m[0x1a9];
    unsigned track = m[0x1aa];
    for (int i = 0; i < 4; i++) {
        uint8_t t[TRUCK_SIZE];
        for (int j = 0; j < TRUCK_SIZE; j++) t[j] = m[TRUCKS[i] + j];
        rules_prepare_result(k, m, 65536, t, layout);
        for (int j = 0; j < TRUCK_SIZE; j++) m[TRUCKS[i] + j] = t[j];
        if (k->error) return;
    }
    unsigned pointers[4];
    uint8_t copies[4][TRUCK_SIZE], *trucks[4];
    int len[4], order[4];
    for (int i = 0; i < 4; i++) {
        pointers[i] = rw(m, 0x9cec + 2 * i);
        len[i] = 65536 - (int)pointers[i] < TRUCK_SIZE ? 65536 - (int)pointers[i] : TRUCK_SIZE;
        for (int j = 0; j < len[i]; j++) copies[i][j] = m[pointers[i] + j];
        trucks[i] = copies[i]; order[i] = i;
    }
    rules_rank(k, trucks, len, order);
    if (k->error) return;
    for (int i = 0; i < 4; i++) for (int j = 0; j < len[i]; j++) m[pointers[i] + j] = copies[i][j];
    for (int i = 0; i < 4; i++) ww(m, 0x9cec + 2 * i, pointers[order[i]]);
    for (int i = 0; i < 4; i++) awards_award(k, m, TRUCKS[i], track);
}

static void copy_words(uint8_t *m, long long dst, long long src, int count) {
    for (int i = 0; i < count; i++) ww(m, dst + 2 * i, rw(m, src + 2 * i));
}

/* 36f6: the pointer list at 98b9 (offsets from 98b9), the score word at entry+0a */
void awards_sort_drivers(uint8_t *m) {
    for (;;) {
        int swaps = 0;
        long long si = RECORDS_BLOCK;
        for (;;) {
            unsigned a = rw(m, si);
            si += 2;
            unsigned b = rw(m, si);
            if (!b) break;
            if (rw(m, a + 0x98c3) < rw(m, b + 0x98c3)) { ww(m, si, a); ww(m, si - 2, b); swaps++; }
        }
        if (!swaps) return;
    }
}

/* handler 2f76 (opcode 2a8); SI is the object, its +6 the truck */
void awards_driver_out(Truck *k, uint8_t *m, unsigned si) {
    unsigned di = rw(m, si + 6);
    if (!(m[(di + 2) & 65535] & 2)) {
        m[(di + 2) & 65535] |= 2;
        unsigned e = (rw(m, 0x98cd) + RECORDS_BLOCK) & 65535;
        ww(m, e, rw(m, di + 5)); m[(e + 2) & 65535] = m[(di + 7) & 65535];
        /* identity bytes stored as +5..+7, +0a..+0b, +08..+09, +0c..+0e */
        ww(m, e + 3, rw(m, di + 0xa)); ww(m, e + 5, rw(m, di + 8)); ww(m, e + 7, rw(m, di + 0xc));
        m[(e + 9) & 65535] = m[(di + 0xe) & 65535]; ww(m, e + 0xa, rw(m, di + 0x11));
        copy_words(m, e + 0xc, di + 0x13, 20);
        unsigned old = rw(m, di + 3);
        if (old) copy_words(m, old + 0xc, di + 0x13, 20);
        awards_sort_drivers(m);
    }
    if (m[TRUCKS[0] + 2] & 2 && m[TRUCKS[1] + 2] & 2 && m[TRUCKS[2] + 2] & 2) ww(m, 0x1a6, rw(m, 0x1a6) | 0x118);
    (void)k;
}

/* handler 3613 (opcode 2b0): truck +5..+0e against the first ten bytes of the
 * top ten entries. Found: +3 = entry, 40 bytes of per-track figures restored,
 * object +8 = ffff; otherwise reset, +8 = 0. */
int awards_driver_login(uint8_t *m, unsigned si) {
    unsigned di = rw(m, si + 6);
    for (int kk = 0; kk < 10; kk++) {
        unsigned e = (rw(m, RECORDS_BLOCK + 2 * kk) + RECORDS_BLOCK) & 65535;
        int same = 1;
        for (int i = 0; i < 10 && same; i++) same = m[(di + 5 + i) & 65535] == m[(e + i) & 65535];
        if (same) {
            ww(m, di + 3, e);
            for (int i = 0; i < 0x28; i++) m[(di + 0x13 + i) & 65535] = m[(e + 0xc + i) & 65535];
            ww(m, si + 8, 0xffff);
            return 1;
        }
    }
    ww(m, di + 3, 0);
    for (int i = 0; i < 8; i++) ww(m, di + 0x13 + 2 * i, 0x9999);
    for (int i = 0; i < 12; i++) ww(m, di + 0x23 + 2 * i, 0);
    ww(m, si + 8, 0);
    return 0;
}

void awards_records_file(const uint8_t *m, uint8_t out[2 + RECORDS_SIZE]) {
    out[0] = 'R'; out[1] = 'I';
    for (int i = 0; i < RECORDS_SIZE; i++) out[2 + i] = m[RECORDS_BLOCK + i];
}

/* 367f: 1028 bytes after a matching 'RI'; anything else keeps the table. A
 * file shorter than 1030 bytes makes the original copy whatever follows it
 * in memory: it is refused here, as the model does. */
int awards_load_records(uint8_t *m, const uint8_t *raw, long long n) {
    if (!raw || n < 2 + RECORDS_SIZE || raw[0] != 'R' || raw[1] != 'I') return 0;
    for (int i = 0; i < RECORDS_SIZE; i++) m[RECORDS_BLOCK + i] = raw[2 + i];
    return 1;
}
