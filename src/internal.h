/* What the pieces of the race and the screens share: the model's ways of
 * reaching memory (see race.c) and the lists. Not part of the interface. */
#ifndef OFFROAD_INTERNAL_H
#define OFFROAD_INTERNAL_H
#include <stdlib.h>
#include "offroad.h"

#define u8(n) ((uint8_t)(n))
#define u16(n) ((uint16_t)(n))
static inline int s8(unsigned n) { return (int8_t)(uint8_t)n; }
static inline int s16(unsigned n) { return (int16_t)(uint16_t)n; }
static inline unsigned rw(const uint8_t *m, long long a) { return m[a & 65535] | m[(a + 1) & 65535] << 8; }
static inline void ww(uint8_t *m, long long a, unsigned v) { m[a & 65535] = u8(v); m[(a + 1) & 65535] = u8(v >> 8); }
static inline unsigned W(const uint8_t *b, unsigned o) { return b[o] | b[o + 1] << 8; }
static inline void put(uint8_t *b, unsigned o, long long v) { b[o] = (uint8_t)v; b[o + 1] = (uint8_t)(v >> 8); }

static const unsigned SUPPORTS[4] = {0x8a, 0x98, 0xa6, 0xb4};
static const unsigned ANGLES[4] = {0xa000, 0x6000, 0xe000, 0x2000};

#define ERR (c->k->error)
static inline void fail(Cycle *c, int e) { if (!c->k->error) c->k->error = e; }

/* the segment: W/put do not wrap */
static inline unsigned MW(Cycle *c, long long a) {
    if (a < 0 || a + 2 > 65536) { fail(c, OFF_BAD_ADDRESS); return 0; }
    return W(c->mem, (unsigned)a);
}
static inline void MPUT(Cycle *c, long long a, long long v) {
    if (a < 0 || a + 2 > 65536) { fail(c, OFF_BAD_ADDRESS); return; }
    put(c->mem, (unsigned)a, v);
}
static inline unsigned MB(Cycle *c, long long a) {
    if (a < 0 || a > 65535) { fail(c, OFF_BAD_ADDRESS); return 0; }
    return c->mem[a];
}
static inline void MSET(Cycle *c, long long a, unsigned v) {
    if (a < 0 || a > 65535) { fail(c, OFF_BAD_ADDRESS); return; }
    c->mem[a] = u8(v);
}
/* the model's data */
static inline unsigned DW(Cycle *c, long long a) {
    if (a < 0 || a + 2 > c->k->ndata) { fail(c, OFF_BAD_ADDRESS); return 0; }
    return W(c->k->data, (unsigned)a);
}
static inline unsigned DB(Cycle *c, long long a) {
    if (a < 0 || a >= c->k->ndata) { fail(c, OFF_BAD_ADDRESS); return 0; }
    return c->k->data[a];
}
/* an object: a view of 7a bytes at `at` */
static inline int ofits(Cycle *c, unsigned at, long long f, int size) {
    long long len = 65536 - (long long)at < OBJECT_SIZE ? 65536 - (long long)at : OBJECT_SIZE;
    if (f < 0 || f + size > len) { fail(c, OFF_BAD_ADDRESS); return 0; }
    return 1;
}
static inline unsigned OW(Cycle *c, unsigned at, long long f) { return ofits(c, at, f, 2) ? W(c->mem, at + f) : 0; }
static inline void OPUT(Cycle *c, unsigned at, long long f, long long v) { if (ofits(c, at, f, 2)) put(c->mem, at + f, v); }
static inline unsigned OB(Cycle *c, unsigned at, long long f) { return ofits(c, at, f, 1) ? c->mem[at + f] : 0; }
static inline void OSET(Cycle *c, unsigned at, long long f, unsigned v) { if (ofits(c, at, f, 1)) c->mem[at + f] = u8(v); }

#define PUSH(list, value) do { \
        if ((list).n == (list).cap) { \
            (list).cap = (list).cap ? 2 * (list).cap : 64; \
            (list).v = realloc((list).v, sizeof *(list).v * (list).cap); \
        } \
        (list).v[(list).n++] = (value); \
    } while (0)

#endif
