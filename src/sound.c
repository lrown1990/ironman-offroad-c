/* The sound of game.exe: the AdLib player with its three voices and the
 * music sequencer, and the PC speaker player (3a52..4342), on the data
 * segment where the game keeps their state. A literal translation of the
 * model tools/sound.py of OFFROAD CODE OPUS, which is verified against the
 * original code and the real game's register writes; tests/check_sound.py
 * compares the two.
 *
 * Requests: 3af8 effect AL, 3b05 music AL, 3b12 music off (everything off),
 * 3aaa AX effect or FFFF everything off, 3ad1 AX music or FFFF music stop.
 * Nothing happens while DS:9cf5 is set (F2). The routines are reached
 * through the pointers at DS:9cf7 (tick), 9cf9 (effect), 9cfb (music), 9cfd
 * (off), 9cff (music stop), set at start-up for the device in DS:9cf4.
 */
#include <string.h>
#include "offroad.h"

#define B(a) ds_b(s->m, (uint16_t)(a))
#define W(a) ds_w(s->m, (uint16_t)(a))
#define SETB(a, v) ds_setb(s->m, (uint16_t)(a), (v))
#define SETW(a, v) ds_setw(s->m, (uint16_t)(a), (v))

static void opl(Sound *s, unsigned reg, unsigned value) {      /* 3c77 */
    if (s->error) return;
    if (s->nopl >= s->opl_cap) { s->error = OFF_OVERFLOW; return; }
    s->opl[2 * s->nopl] = (uint8_t)reg;
    s->opl[2 * s->nopl + 1] = (uint8_t)value;
    s->nopl++;
}

static void speaker(Sound *s, int kind, unsigned divisor) {
    if (s->error) return;
    if (s->nspk >= s->spk_cap) { s->error = OFF_OVERFLOW; return; }
    s->spk[2 * s->nspk] = (uint16_t)kind;
    s->spk[2 * s->nspk + 1] = (uint16_t)divisor;
    s->nspk++;
}

/* ---- AdLib ---------------------------------------------------------------- */
static void adlib_reset(Sound *s) {                              /* 3c91 */
    for (unsigned reg = 0x80; reg < 0x95; reg++) opl(s, reg, 0xff);
    for (unsigned reg = 0xb0; reg < 0xb9; reg++) opl(s, reg, 0);
    opl(s, 0xbd, 0x20);
    SETW(0xb24a, 0); SETW(0xb24c, 0); SETW(0xb24e, 0); SETW(0xb250, 0);
    SETB(0xb252, 0);
    SETW(0x9f0c, 0); SETW(0x9f4c, 0); SETW(0x9f8c, 0);
    SETB(0xb284, 0);
}

/* The silencing part of 3d42 and 3f03: the parts of the voice that sound
 * get release ff and key off. bl = the voice. */
static void key_off(Sound *s, uint16_t di, unsigned bl) {
    if (W(di + 0xd)) { opl(s, bl + 0x80, 0xff); opl(s, bl + 0x83, 0xff); opl(s, bl + 0xb0, 0); }
    if (W(di + 0x1d)) { opl(s, bl + 0x88, 0xff); opl(s, bl + 0x8b, 0xff); opl(s, bl + 0xb3, 0); }
    if (W(di + 0x2d)) {
        unsigned p = B(di + 0x34);
        opl(s, p + 0x8a, 0xff); opl(s, p + 0x8d, 0xff); opl(s, p + 0xb0, 0);
        unsigned bh = ~B(di + 0x33) & B(0xb253) & 255;
        SETB(0xb253, bh); opl(s, 0xbd, bh);
    }
}

static uint16_t operator_(Sound *s, unsigned op, uint16_t si) {  /* 3ed2 */
    static const unsigned regs[5] = {0x20, 0x40, 0x60, 0x80, 0xe0};
    for (int k = 0; k < 5; k++) { opl(s, (op + regs[k]) & 255, B(si)); si++; }
    return si;
}

static void start_voice(Sound *s, uint16_t di, uint16_t si, unsigned ah, unsigned bx, unsigned cx,
                        unsigned dl, unsigned dh) {                  /* 3d42 */
    SETW(di, si); SETB(di + 3, dl); SETW(di + 4, bx); SETW(di + 0xf, bx); SETB(di + 0x13, dh);
    SETW(di + 0x14, cx); SETW(di + 0x1f, cx);
    unsigned v = ah;
    key_off(s, di, v);
    si++; unsigned ax = W(si); si += 2;
    SETB(di + 2, ax & 255); SETB(di + 0x33, ax >> 8);
    SETB(0xb24a + v, 0x20);
    si += 3;
    SETW(di + 6, W(si)); si += 2;
    ax = W(si); si += 2; SETW(di + 0x11, ax); SETW(di + 8, (ax >> 1) + 1);
    SETB(di + 0xa, B(si)); si++;
    SETW(di + 0xb, W(si)); si += 2;
    SETW(di + 0xd, W(si)); si += 2;
    opl(s, v + 0xc0, B(si)); si++;
    si = operator_(s, v, si); si = operator_(s, v + 3, si);
    SETB(0xb24d + v, 0x20);
    si += 3;
    SETW(di + 0x16, W(si)); si += 2;
    ax = W(si); si += 2; SETW(di + 0x21, ax); SETW(di + 0x18, (ax >> 1) + 1);
    SETB(di + 0x1a, B(si)); si++;
    SETW(di + 0x1b, W(si)); si += 2;
    SETW(di + 0x1d, W(si)); si += 2;
    opl(s, v + 0xc3, B(si)); si++;
    si = operator_(s, v + 8, si); si = operator_(s, v + 0xb, si);
    unsigned cx2 = 0, bp = 0, ah2 = B(di + 0x33);
    if (ah2) {
        SETB(0xb253, B(0xb253) | ah2);
        unsigned al = 0;
        for (int bit = 0; bit < 4; bit++) { if (ah2 >> bit & 1) break; al++; }
        unsigned p = B(0xb25d + al);
        SETB(di + 0x34, p); SETB(0xb250 + p, 0);
        SETB(di + 0x23, B(si)); si++;
        ax = W(si); si += 2; SETW(di + 0x24, ax); SETW(di + 0x2f, ax);
        SETW(di + 0x26, W(si)); si += 2;
        ax = W(si); si += 2; SETW(di + 0x31, ax); SETW(di + 0x28, (ax >> 1) + 1);
        SETB(di + 0x2a, B(si)); si++;
        unsigned pushed = W(si); si += 2;
        bp = W(si); si += 2;
        opl(s, p + 0xc0, B(si)); si++;
        si = operator_(s, p + 0x0a, si); si = operator_(s, p + 0x0d, si);
        opl(s, 0xbd, B(0xb253));
        cx2 = pushed;
    }
    SETW(di + 0x2b, cx2); SETW(di + 0x2d, bp);
}

/* 3f03: the same instrument again. */
static void restart_voice(Sound *s, uint16_t di, uint16_t si, unsigned ah, unsigned bx, unsigned cx,
                          unsigned dl, unsigned dh) {
    SETW(di, si); SETB(di + 3, dl); SETW(di + 4, bx); SETW(di + 0xf, bx); SETB(di + 0x13, dh);
    SETW(di + 0x14, cx); SETW(di + 0x1f, cx);
    unsigned v = ah;
    key_off(s, di, v);
    SETB(0xb24a + v, 0x20);
    SETW(di + 8, (W(di + 0x11) >> 1) + 1);
    SETW(di + 0xb, W(si + 0xb)); SETW(di + 0xd, W(si + 0xd));
    unsigned bh = B(si + 0x13); opl(s, v + 0x80, bh); opl(s, v + 0x83, bh);
    SETB(0xb24d + v, 0x20);
    SETW(di + 0x18, (W(di + 0x21) >> 1) + 1);
    SETW(di + 0x1b, W(si + 0x22)); SETW(di + 0x1d, W(si + 0x24));
    bh = B(si + 0x2a); opl(s, v + 0x88, bh); opl(s, v + 0x8b, bh);
    unsigned ah2 = B(di + 0x33);
    if (ah2) {
        SETB(0xb253, B(0xb253) | ah2);
        unsigned p = B(di + 0x34); SETB(0xb250 + p, 0);
        SETW(di + 0x24, W(di + 0x2f));
        SETW(di + 0x28, (W(di + 0x31) >> 1) + 1);
        SETW(di + 0x2b, W(si + 0x39)); SETW(di + 0x2d, W(si + 0x3b));
        bh = B(si + 0x41); opl(s, p + 0x90, bh); opl(s, p + 0x93, bh);
        opl(s, 0xbd, B(0xb253));
    }
}

static void adlib_effect(Sound *s, unsigned al) {                /* 3cd8 */
    if (!B(0xb275 + al)) return;
    unsigned ah = B(0xb262 + B(0xb284));
    uint16_t di = (uint16_t)(ah * 64 + 0x9f0c), channel = (uint16_t)(ah * 16 + 0x9fcc);
    unsigned cx = W(channel + 6);
    if (cx) SETW(channel + 8, cx);
    uint16_t si = (uint16_t)(((al + 0x20) & 255) * 72 + 0xa04a);
    unsigned bx = W(di);
    if (bx && B(si) < B(bx)) return;
    start_voice(s, di, si, ah, W(si + 4), W(si + 0x1b), B(si + 3), B(si + 0x1a));
}

/* ---- the voices (4037) ----------------------------------------------------- */
static unsigned frequency(Sound *s, unsigned bl, uint16_t di, unsigned ax) {   /* 40f1 */
    unsigned al = ax & 255, ah = ((ax >> 8) | B(0xb24a + bl)) & 255;
    opl(s, bl + 0xa0, al);
    ah |= (B(di) << 2) & 255;
    opl(s, bl + 0xb0, ah);
    return bl;
}

static unsigned sweep(Sound *s, unsigned bl, uint16_t di) {      /* 40e0 */
    unsigned step = (W(di + 5) - 1) & 0xffff, ax;
    SETW(di + 5, step);
    if (step) { ax = (W(di + 1) + W(di + 3)) & 0x3ff; SETW(di + 1, ax); }
    else {
        SETW(di + 5, W(di + 0xe));
        unsigned mode = B(di + 7);
        if (!mode) SETW(di + 1, W(di + 0xc));
        else {
            if (mode != 1) SETW(di + 1, W(di + 0xc));
            SETW(di + 3, (-W(di + 3)) & 0xffff);
        }
        ax = W(di + 1);
    }
    return frequency(s, bl, di, ax);
}

static unsigned release(Sound *s, unsigned bl) {                 /* 415b */
    unsigned p = B(0xb254 + bl);
    opl(s, p + 0x80, 0xff); opl(s, p + 0x83, 0xff);
    opl(s, bl + 0xb0, 0);
    return bl;
}

/* 40d3: returns BL as the routine leaves it. */
static unsigned part(Sound *s, unsigned bl, uint16_t di) {
    unsigned ax = W(di + 8);
    if (ax) {
        ax = (ax - 1) & 0xffff; SETW(di + 8, ax);
        if (!ax) {
            SETB(0xb24a + bl, 0);
            if (W(di + 0xa)) return sweep(s, bl, di);
            return release(s, bl);
        }
        return sweep(s, bl, di);
    }
    ax = W(di + 0xa);
    if (!ax) return bl;
    ax = (ax - 1) & 0xffff; SETW(di + 0xa, ax);
    if (ax) return sweep(s, bl, di);
    return release(s, bl);
}

static void voice(Sound *s, unsigned bl, uint16_t di) {          /* 4062 */
    di += 3;
    bl = part(s, bl, di);
    unsigned al = B(di - 1), cx = W(di + 1), ah = B(di);
    di += 0x10;
    if (al) {
        unsigned dx = (cx + W(di + 3)) & 0xffff;
        al = (al - 1) & 255;
        if (al) { dx = cx; ah = (ah + 1) & 7; }
        SETW(di + 1, dx & 0x3ff); SETB(di, ah);
    }
    bl = (bl + 3) & 255;
    bl = part(s, bl, di);
    di += 0x10;
    bl = B(di + 0x11);
    int last = W(di + 8) == 1;
    bl = part(s, bl, di);
    if (last) { unsigned bh = ~B(di + 0x10) & B(0xb253) & 255; SETB(0xb253, bh); opl(s, 0xbd, bh); }
    if (!(W(di + 8) | W(di - 8) | W(di - 0x18) | W(di + 0xa) | W(di - 6) | W(di - 0x16)))
        SETW(di - 0x23, 0);
}

static void voices_update(Sound *s) {
    static const uint16_t records[3] = {0x9f0c, 0x9f4c, 0x9f8c};
    for (unsigned v = 0; v < 3; v++)
        if (W(records[v])) voice(s, v, records[v]);
}

/* ---- the music (41d8) ------------------------------------------------------ */
/* 431d: 1 = the end of the list (CF). */
static int next_pattern(Sound *s, uint16_t di) {
    uint16_t si = W(di);
    int loops = 0;
    for (;;) {
        unsigned al = B(si); si++; SETW(di, si);
        if (al == 0xfe) {
            /* 433c: back to the start of the list. Only [di] is written in
             * this loop, with the same value from the second time on: the
             * third FE in a row is FE for ever. */
            if (++loops == 3) { s->error = OFF_HANG; return 1; }
            si = W(di + 2);
            continue;
        }
        if (al == 0xff) return 1;
        SETW(di + 4, (W(0xb5e1 + 2 * al) + 0xb5e1) & 0xffff);
        return 0;
    }
}

static void channel_start(Sound *s, uint16_t di, unsigned ax) {  /* 41b4 */
    ax = (ax + 0xb407) & 0xffff;
    SETW(di, ax); SETW(di + 2, ax);
    SETW(di + 6, 0); SETW(di + 8, 0); SETB(di + 0xa, 1); SETB(di + 0xb, 1);
    next_pattern(s, di);
}

static void music_start(Sound *s, unsigned al) {                 /* 417e */
    uint16_t si = (uint16_t)((((al - 1) & 255) * 12) + 0xb407);
    SETB(0xb285, W(si) >> 8); si += 2;
    SETB(0xb286, W(si) & 255); si += 2;
    static const uint16_t channels[3] = {0x9fcc, 0x9fdc, 0x9fec};
    for (int k = 0; k < 3 && !s->error; k++) { unsigned ax = W(si); si += 2; channel_start(s, channels[k], ax); }
    if (s->error) return;
    SETB(0xb284, al);
}

static void music_stop(Sound *s) { SETB(0xb284, 0); }            /* 41d2 */

/* 422d: one channel of the music: DH its voice, BP the voice record, DI the
 * channel (+0 pattern list, +2 its start, +4 note pointer, +6 the
 * instrument, +8 an instrument for the next note, +a tempo count, +b length
 * count, +c length). */
#define SEEN_MAX 4096
static void channel(Sound *s, unsigned dh, uint16_t bp, uint16_t di) {
    SETB(di + 0xa, B(di + 0xa) - 1);
    if (B(di + 0xa)) return;
    SETB(di + 0xa, B(0xb285));
    SETB(di + 0xb, B(di + 0xb) - 1);
    if (B(di + 0xb)) return;
    SETB(di + 0xb, B(di + 0xc));
    uint16_t si = W(di + 4);
    static uint8_t seen[SEEN_MAX][13];
    int nseen = 0;
    for (;;) {
        unsigned al = B(si); si++; SETW(di + 4, si);
        if (al < 0x80) {                    /* a note: 4-byte entry of the table at b287 */
            unsigned cx = W(0xb287 + 4 * al), dl = B(0xb289 + 4 * al);
            unsigned ins = W(di + 8);
            if (ins) {                      /* 425b: an instrument chosen for this note */
                unsigned bx = W(bp);
                if (bx && B(ins) < B(bx)) return;
                SETW(di + 8, 0); SETW(di + 6, ins);
                start_voice(s, bp, (uint16_t)ins, dh, cx, cx, dl, dl);
                return;
            }
            ins = W(di + 6);                /* 4284: the same instrument again */
            unsigned bx = W(bp);
            if (bx && B(ins) < B(bx)) return;
            restart_voice(s, bp, (uint16_t)ins, dh, cx, cx, dl, dl);
            return;
        }
        if (al < 0xb0) {                    /* 80..af: length */
            unsigned n = (al - 0x7f) & 255; SETB(di + 0xc, n); SETB(di + 0xb, n); continue;
        }
        if (al < 0xd0) {                    /* b0..cf: a record of the bank, now */
            uint16_t rec = (uint16_t)((al - 0xb0) * 72 + 0xa04a);
            SETW(di + 8, W(di + 6));
            start_voice(s, bp, rec, dh, W(rec + 4), W(rec + 0x1b), B(rec + 3), B(rec + 0x1a));
            return;
        }
        if (al < 0xfd) { SETW(di + 8, ((al - 0xd0) * 72 + 0xa04a) & 0xffff); continue; }   /* d0..fc */
        if (al == 0xfd) continue;
        if (al == 0xfe) return;
        if (next_pattern(s, di)) { if (!s->error) SETW(di + 4, 0); return; }
        si = W(di + 4);
        /* Only the channel record (+0..+c) is written in this loop: the same
         * record at the start of a pattern again is the same for ever. */
        uint8_t state[13];
        for (int k = 0; k < 13; k++) state[k] = B(di + k);
        for (int k = 0; k < nseen; k++)
            if (!memcmp(seen[k], state, 13)) { s->error = OFF_HANG; return; }
        if (nseen == SEEN_MAX) { s->error = OFF_OVERFLOW; return; }
        memcpy(seen[nseen++], state, 13);
    }
}

static void music_update(Sound *s) {
    if (!B(0xb284)) return;
    static const uint16_t bps[3] = {0x9f0c, 0x9f4c, 0x9f8c}, dis[3] = {0x9fcc, 0x9fdc, 0x9fec};
    for (unsigned k = 0; k < 3 && !s->error; k++)
        if (W(dis[k] + 4)) channel(s, k, bps[k], dis[k]);
    if (s->error) return;
    /* 4215: the end of a sequence (the third channel is tested as 9fe0 again) */
    if (!(W(0x9fd0) | W(0x9fe0)) && B(0xb286)) music_start(s, B(0xb286));
}

static void adlib_tick(Sound *s) {                               /* 3cd1 */
    music_update(s);
    if (!s->error) voices_update(s);
}

/* ---- PC speaker ---------------------------------------------------------- */
static void speaker_reset(Sound *s) { SETW(0x9f07, 0); SETW(0x9f09, 0); }   /* 3b9e */

static void speaker_effect(Sound *s, unsigned al) {              /* 3ba7 */
    uint16_t si = (uint16_t)(0x9d58 + 4 * al);
    unsigned ax = W(si + 2);
    if (W(0x9f07) && ax < W(0x9f09)) return;
    SETW(0x9f09, ax); SETW(0x9f07, W(si));
}

static void speaker_tick(Sound *s) {                             /* 3bca */
    uint16_t si = W(0x9f07);
    if (!si) return;
    for (;;) {
        unsigned al = B(si); si++;
        if (al < 0x80) { SETW(0x9f07, si); speaker(s, SPK_PIT, W(0x9e47 + 2 * al)); return; }
        if (al == 0xff) { speaker(s, SPK_ON, 0); continue; }     /* speaker on, go on */
        if (al == 0xfe) { speaker(s, SPK_OFF, 0); SETW(0x9f07, si); return; }
        if (al == 0xfd) { SETW(0x9f07, si); return; }
        speaker(s, SPK_OFF, 0); SETW(0x9f07, 0); return;
    }
}

/* ---- requests -------------------------------------------------------------- */
static void dispatch(Sound *s, uint16_t pointer, unsigned arg) {
    switch (W(pointer)) {
    case 0x3cd1: adlib_tick(s); break;
    case 0x3cd8: adlib_effect(s, arg); break;
    case 0x417e: music_start(s, arg); break;
    case 0x3c91: adlib_reset(s); break;
    case 0x41d2: music_stop(s); break;
    case 0x3bca: speaker_tick(s); break;
    case 0x3ba7: speaker_effect(s, arg); break;
    case 0x3b9e: speaker_reset(s); break;
    case 0x3bc9: case 0x3a5a: break;        /* RET */
    default: s->error = OFF_NOT_MODELLED;
    }
}

int snd_request(Sound *s, int kind, unsigned arg) {
    s->error = OFF_OK;
    switch (kind) {
    case SND_TICK: dispatch(s, 0x9cf7, 0); break;                              /* 3a52 */
    case SND_EFFECT: if (!B(0x9cf5)) dispatch(s, 0x9cf9, arg & 255); break;   /* 3af8 */
    case SND_MUSIC: if (!B(0x9cf5)) dispatch(s, 0x9cfb, arg & 255); break;    /* 3b05 */
    case SND_MUSIC_OFF: dispatch(s, 0x9cfd, 0); break;                         /* 3b12 */
    case SND_SCRIPT_AUDIO:                                                     /* 3aaa */
        if ((arg & 0xffff) == 0xffff) dispatch(s, 0x9cfd, 0);
        else if (!B(0x9cf5)) dispatch(s, 0x9cf9, arg & 255);
        break;
    case SND_SCRIPT_AUDIO_B:                                                   /* 3ad1 */
        if ((arg & 0xffff) == 0xffff) dispatch(s, 0x9cff, 0);
        else if (!B(0x9cf5)) dispatch(s, 0x9cfb, arg & 255);
        break;
    default: s->error = OFF_NOT_MODELLED;
    }
    return s->error;
}

/* what the program's start (3a5b) calls directly */
void snd_opl(Sound *s, unsigned reg, unsigned value) { opl(s, reg, value); }
void snd_adlib_reset(Sound *s) { adlib_reset(s); }

int offroad_sound(uint8_t *ds, int kind, unsigned arg, uint8_t *opl_out, int opl_cap, int *nopl,
                  uint16_t *spk_out, int spk_cap, int *nspk) {
    Sound s = {ds, opl_out, 0, opl_cap, spk_out, 0, spk_cap, 0};
    int r = snd_request(&s, kind, arg);
    *nopl = s.nopl; *nspk = s.nspk;
    return r;
}
