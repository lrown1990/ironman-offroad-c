/* The computer's drivers (game 20c6..2131, 2172..23e1): a literal translation
 * of tools/truck_ai.py, on the whole data segment.
 *
 * 20c6 cpu_levels    at the start of a race, from the best human rating (+5c),
 *                    the race index (01a8) and a per-track offset (74fa); 17
 *                    in the attract loop (01ab != 0).
 * 2172 ai_start      +54 = +0 + 1, +74 cleared; CPU trucks take the nearest point.
 * 21f3 nearest_point the layout's script (pointer table 755a), the nearest
 *                    point the checkpoint sections allow.
 * 2189 ai_command    the command byte 1769 uses instead of the keys.
 * 229b next_point    walks the script, running its commands, to the next point.
 * 2314 rubber band   command 5: the ranking again, the level moved toward
 *                    the leading human, the parameters reloaded (14cc).
 * Points are words: low byte = X cell, high byte = Y cell; a first byte with
 * bit 7 set is a command.
 */
#include "offroad.h"

#define u8(n) ((uint8_t)(n))
#define u16(n) ((uint16_t)(n))
static inline int s8(unsigned n) { return (int8_t)(uint8_t)n; }
static inline int s16(unsigned n) { return (int16_t)(uint16_t)n; }
static void fail(Truck *k, int e) { if (!k->error) k->error = e; }

const uint16_t TRUCK_ADDRESSES[4] = {0x4c72, 0x4d34, 0x4df6, 0x4eb8};
#define POINT_LISTS 0x755a
#define CPU_ORDER 0x74f0   /* 4eb8, 4df6, 4d34, 4c72, 0 */
static const uint8_t COMMAND_SIZE[11] = {5, 4, 5, 3, 3, 1, 2, 2, 3, 3, 1};

/* words wrap at the end of the segment; bytes do not (the model's index error) */
static inline unsigned rw(const uint8_t *m, long long a) { return m[a & 65535] | m[(a + 1) & 65535] << 8; }
static inline void ww(uint8_t *m, long long a, unsigned v) { m[a & 65535] = u8(v); m[(a + 1) & 65535] = u8(v >> 8); }
static unsigned mb(Truck *k, const uint8_t *m, long long a) {
    if (a < 0 || a > 65535) { fail(k, OFF_BAD_ADDRESS); return 0; }
    return m[a];
}
static void mset(Truck *k, uint8_t *m, long long a, unsigned v) {
    if (a < 0 || a > 65535) { fail(k, OFF_BAD_ADDRESS); return; }
    m[a] = u8(v);
}

/* 0434:063e, the generator the physics also uses: the seed at DS:0167 */
unsigned ai_random_word(uint8_t *m) {
    unsigned seed = (u16(rw(m, 0x167) + 1) * 171u) % 0x7673;
    ww(m, 0x167, seed);
    return u16(seed - 1);
}

/* 2395: unsigned bytes; lo >= hi wraps around (lo == hi is always true) */
int ai_in_range(unsigned value, unsigned lo, unsigned hi) {
    return lo < hi ? lo <= value && value <= hi : value >= lo || value <= hi;
}

/* 23ad: squared distance in 1/64 units (16 bits); dx, dy left at 7506/7508 */
unsigned ai_distance(uint8_t *m, unsigned di, unsigned point) {
    unsigned dx = u16(((point & 255) << 8) - rw(m, di + 0x79)), dy = u16((point & 0xff00) - rw(m, di + 0x7b));
    ww(m, 0x7506, dx); ww(m, 0x7508, dy);
    long long a = s16(dx) >> 6, b = s16(dy) >> 6;   /* arithmetic, as Python's floor */
    return u16(((a * a) & 65535) + ((b * b) & 65535));
}

/* 14c7 (ten nitro charges) or 14cc: the parameters for CPU level +5b */
void ai_cpu_parameters(Truck *k, uint8_t *m, unsigned di, int nitro) {
    static const unsigned table[8][2] = {{0x63, 0x4fea}, {0x6d, 0x500e}, {0x67, 0x5032}, {0x69, 0x5032},
                                         {0x6b, 0x5056}, {0x65, 0x507a}, {0x71, 0x509e}, {0x6f, 0x50c2}};
    if (nitro) mset(k, m, di + 0x5d, 10);
    unsigned level = u16(mb(k, m, di + 0x5b) * 2);
    if (k->error) return;
    for (int i = 0; i < 8; i++) ww(m, di + table[i][0], rw(m, table[i][1] + level));
}

void ai_cpu_levels(Truck *k, uint8_t *m) {
    unsigned best = 0;
    long long at = CPU_ORDER;
    while (rw(m, at)) {
        unsigned t = rw(m, at);
        at += 2;
        if (!(mb(k, m, t + 2) & 2) && !k->error) {
            unsigned rating = mb(k, m, t + 0x5c);
            if (best < rating) best = rating;
        }
        if (k->error) return;
    }
    unsigned level = u8(m[0x74fa + m[0x1aa]] + u8((m[0x1a8] >> 2) + (best >> 1)));
    if (level & 128) level = 0;
    if (level > 0x11) level = 0x11;
    if (m[0x1ab]) level = 0x11;
    at = CPU_ORDER;
    while (rw(m, at)) {
        unsigned t = rw(m, at);
        at += 2;
        if (mb(k, m, t + 2) & 2) {
            mset(k, m, t + 0x5c, level); mset(k, m, t + 0x5b, level);
            level = level ? level - 1 : 0;
        }
        if (k->error) return;
    }
}

/* 21f3 with the start-up command table 750a; returns AX, the next point */
unsigned ai_nearest_point(Truck *k, uint8_t *m, unsigned di) {
    unsigned si = rw(m, POINT_LISTS + 2 * m[0x1a9]);
    m[0x7520] = 0; ww(m, 0x7502, 0xffff);
    for (long long guard = 0; guard < (1 << 16); guard++) {   /* the model's guard: the original has none */
        unsigned word = rw(m, si);
        if (word & 0x80) {
            unsigned c = word & 0x7f;
            if (c == 10) {
                unsigned a = rw(m, di + 0x50), b = rw(m, 0x7502);
                ww(m, di + 0x50, a > b ? a : b);
                unsigned point = ai_next_point(k, m, di);
                ww(m, di + 0x4e, point);
                return point;
            }
            if (c == 3) {
                unsigned value = mb(k, m, di + 0x41), lo = mb(k, m, si + 1), hi = mb(k, m, si + 2);
                m[0x7520] = ai_in_range(value, lo, hi) ? 0 : 1;
            } else if (c == 6) m[0x7521] = u8(mb(k, m, si + 1));
            else if (c == 7) m[0x7522] = u8(mb(k, m, si + 1));
            else if (c > 10) { fail(k, OFF_NOT_MODELLED); return 0; }   /* outside table 750a */
            if (k->error) return 0;
            si = u16(si + COMMAND_SIZE[c]);
            continue;
        }
        si = u16(si + 2);
        if (m[0x7520]) continue;
        unsigned d = ai_distance(m, di, word);
        if (d < rw(m, 0x7502)) {
            ww(m, 0x7502, d); ww(m, di + 0x4c, rw(m, (long long)si - 2)); ww(m, di + 0x4a, si);
            unsigned r = m[0x7521] >> 1;
            ww(m, di + 0x50, r * r); ww(m, di + 0x52, u8(m[0x7522] + 1) * 2);
        }
    }
    fail(k, OFF_HANG);   /* the scan never reached the end marker */
    return 0;
}

/* 2314 */
void ai_rubber_band(Truck *k, uint8_t *m, unsigned di) {
    if (mb(k, m, di) != 3 || m[0x1ab] || k->error) return;
    int layout = m[0x1a9];
    for (int i = 0; i < 4; i++) {
        uint8_t view[TRUCK_SIZE];
        for (int j = 0; j < TRUCK_SIZE; j++) view[j] = m[TRUCK_ADDRESSES[i] + j];
        rules_normalize_progress(k, m, 65536, view, layout);
        for (int j = 0; j < TRUCK_SIZE; j++) m[TRUCK_ADDRESSES[i] + j] = view[j];
    }
    /* copies of the four records (short at the end of the segment), ranked
     * and written back in order, as the model does */
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
    unsigned leader = 0;
    for (int i = 0; i < 4; i++) {
        leader = rw(m, 0x9cec + 2 * i);
        if (!(mb(k, m, leader + 2) & 2)) break;
    }
    unsigned mine = mb(k, m, di + 0x3c), theirs = mb(k, m, leader + 0x3c), step;
    if (k->error) return;
    if (mine < theirs) step = 5;
    else if (mine > theirs) step = 0xfb;
    else {
        unsigned diff = u8(mb(k, m, di + 0x43) - mb(k, m, leader + 0x43));
        step = m[u16(0x7549 + u8(s8(diff) >> 4))];
    }
    unsigned level = u8(step + mb(k, m, di + 0x5c));
    if (level & 128) level = 0;
    if (level > 0x11) level = 0x11;
    if (k->error) return;
    if (level != mb(k, m, di + 0x5b)) { mset(k, m, di + 0x5b, level); ai_cpu_parameters(k, m, di, 0); }
}

/* 229b with the run-time command table 7523; returns the point (AX) */
unsigned ai_next_point(Truck *k, uint8_t *m, unsigned di) {
    unsigned si = rw(m, di + 0x4a);
#define BRANCH(taken) ((taken) ? u16(si + rw(m, si)) : u16(si + 2))
    for (long long guard = 0; guard < (1 << 16); guard++) {   /* the model's guard: the original has none */
        unsigned word = rw(m, si);
        if (!(word & 0x80)) { si = u16(si + 2); ww(m, di + 0x4a, si); return word; }
        unsigned c = word & 0x7f, lo, hi;
        si = u16(si + 1);
        switch (c) {
        case 0:
        case 2:
            lo = mb(k, m, si); hi = mb(k, m, si + 1);
            si = u16(si + 2);
            if (k->error) return 0;
            si = BRANCH(ai_in_range(mb(k, m, di + (c ? 0x5b : 0x41)), lo, hi));
            break;
        case 1: {
            unsigned high = ai_random_word(m) >> 8, limit = mb(k, m, si);
            si = u16(si + 1);
            si = BRANCH(limit < high);
            break;
        }
        case 3: si = u16(si + 2); break;
        case 4: si = BRANCH(1); break;
        case 5: ai_rubber_band(k, m, di); break;
        case 6: { unsigned r = mb(k, m, si) >> 1; si = u16(si + 1); ww(m, di + 0x50, r * r); break; }
        case 7: ww(m, di + 0x52, u8(mb(k, m, si) + 1) * 2); si = u16(si + 1); break;
        case 8:
            lo = mb(k, m, si); hi = mb(k, m, si + 1);
            si = u16(si + 2);
            /* overwritten by 1778 with the returned command; no list uses it */
            if (ai_in_range(mb(k, m, di + 0x41), lo, hi) && !mb(k, m, di + 0x77)) mset(k, m, di + 0x74, mb(k, m, di + 0x74) | 0x40);
            break;
        case 9: si = BRANCH(mb(k, m, di + 0x77) != 0); break;
        case 10: break;   /* 22c2: a plain RET, the walk goes on one byte later */
        default: fail(k, OFF_NOT_MODELLED); return 0;   /* outside table 7523 */
        }
        if (k->error) return 0;
    }
#undef BRANCH
    fail(k, OFF_HANG);
    return 0;
}

/* 2172 */
void ai_start(Truck *k, uint8_t *m, unsigned di) {
    mset(k, m, di + 0x54, u8(mb(k, m, di) + 1)); mset(k, m, di + 0x74, 0);
    if (mb(k, m, di + 2) & 2 && !k->error) ai_nearest_point(k, m, di);
}

/* 2189: the command byte (steering 1/2, throttle 8 up, 4 down) */
unsigned ai_command(Truck *k, uint8_t *m, unsigned di) {
    if (rw(m, 0x1a6) & 0x80) return 0;
    unsigned d = ai_distance(m, di, rw(m, di + 0x4c));
    if (d >= rw(m, di + 0x50)) d = ai_nearest_point(k, m, di);
    if (k->error) return 0;
    ww(m, 0x7502, d);
    d = ai_distance(m, di, rw(m, di + 0x4e));
    ww(m, 0x7504, d);
    if (d <= rw(m, 0x7502)) {
        ww(m, 0x7502, d); ww(m, di + 0x4c, rw(m, di + 0x4e));
        unsigned p = ai_next_point(k, m, di);
        if (k->error) return 0;
        ww(m, di + 0x4e, p);
    }
    unsigned turn = u8(direction32(rw(m, 0x7506), rw(m, 0x7508)) - mb(k, m, di + 0x82));
    unsigned command = u8(turn + 8) <= 16 ? 0 : (turn & 128 ? 2 : 1);
    unsigned target = rw(m, di + 0x52), throttle = rw(m, di + 0x75);
    if (target != throttle) command |= target > throttle ? 8 : 4;
    return command;
}
