/* The screens between the races: sign-in (2564), results (30e4), shop (2bf8)
 * and the Ironman Club table (34c8) are object scripts like the race; this is
 * GameCycle of tools/screens.py, literally: the instructions those scripts
 * use, as their handlers do them.
 *   generic   000/008 loop A, 020/028 timer wait, 038 switch, 050 redirections,
 *             058 expiry target, 098 move, 110/118/120 children, 128/130
 *             parent flags, 138 position from the parent, 148 follow the
 *             parent, 160 template again, 168 random, 180 flag test, 1a0 byte,
 *             1b8 audio (and 060..0f0, 140, 1c0, which no script uses)
 *   screens   1e0 text, 1e8/1f0 button and joining, 1f8/200/208 counts and
 *             grid, 210..228 graphics module, 230..250 initials, month, day,
 *             year, nation, 258/260 shop menu, 268/270 shop displays and
 *             money, 278 the Ironman Club table, 280/2b8 results, 288 the
 *             continue countdown, 290..2a0 computer ahead and credits, 2a8/2b0
 *             driver out and login, 2c0/2c8 title, 2e8 a credit bought (F1,
 *             F5, F9 for drivers 0, 1, 2)
 */
#include "internal.h"

/* ---- BCD -------------------------------------------------------------------- */
/* SUB/SBB AL,value then DAS as the 386 does */
static unsigned sub_das(unsigned a, unsigned value, unsigned borrow, unsigned *cf_out) {
    unsigned al = (a - value - borrow) & 255;
    int af = (a & 15) < (value & 15) + borrow, cf = a < value + borrow;
    unsigned old_al = al;
    int old_cf = cf;
    cf = 0;
    if ((al & 15) > 9 || af) { cf = old_cf || al < 6; al = (al - 6) & 255; }
    if (old_al > 0x99 || old_cf) { al = (al - 0x60) & 255; cf = 1; }
    if (cf_out) *cf_out = (unsigned)cf;
    return al;
}
static unsigned das_word(unsigned word, unsigned value) {
    unsigned cf, lo = sub_das(word & 255, value & 255, 0, &cf), hi = sub_das(word >> 8, value >> 8, cf, 0);
    return lo | hi << 8;
}
static unsigned daa_word_add(unsigned word, unsigned value) {
    unsigned cf, lo = add_daa(word & 255, value & 255, 0, &cf), hi = add_daa(word >> 8, value >> 8, cf, 0);
    return lo | hi << 8;
}

/* the BCD words on their own, for the tests */
unsigned screens_das_word(unsigned word, unsigned value) { return das_word(word, value); }
unsigned screens_daa_word_add(unsigned word, unsigned value) { return daa_word_add(word, value); }

/* ---- helpers ------------------------------------------------------------------- */
static void graphics(Cycle *c, unsigned entry, unsigned has, unsigned ax, unsigned si, unsigned bl) {
    GfxCall call = {0};
    call.entry = u8(entry); call.has = u8(has); call.ax = u16(ax); call.si = u16(si); call.bl = u16(bl);
    cycle_call_graphics(c, &call);
}
/* task wake-up = counter + 1, INT 60: the counter reaches it. sp: SP at the
 * INT (81EAh in the entry fields and 2e88, 81ECh in 301a) */
static void wait_tick(Cycle *c, unsigned sp) {
    if (c->host.task_wait && c->host.task_wait(c->host.ctx, sp)) return;
    uint8_t *m = c->mem;
    unsigned task = rw(m, 0x70e);
    uint32_t now = rw(m, 0x6c1) | (uint32_t)rw(m, 0x6c3) << 16, wake = now + 1;
    ww(m, task + 0x20, wake & 65535); ww(m, task + 0x22, wake >> 16);
    if (now < wake) { ww(m, 0x6c1, wake & 65535); ww(m, 0x6c3, wake >> 16); }
}
static Printer *printer(Cycle *c) { Printer *p = c->printer; p->c = c; return p; }
static void print_text(Cycle *c, unsigned si) {
    Printer *p = printer(c);
    text_print(p, c->mem, si);
    if (p->error) fail(c, p->error);
}
static unsigned text_width_of(Cycle *c, long long si) {
    if (si > 65535) { fail(c, OFF_BAD_ADDRESS); return 0; }   /* the first byte is read unmasked */
    Printer *p = printer(c);
    unsigned w = text_width(p, c->mem, (unsigned)si);
    if (p->error) fail(c, p->error);
    return w;
}
/* 0732 on the existing object at: field writes go to the data segment (a
 * field near the end of an object reaches the next one). Returns BP when a
 * field came from the parent (parent + field of the last one), or -1. */
static long long apply_template(Cycle *c, unsigned at, unsigned template_) {
    uint8_t *m = c->mem;
    long long pc = template_, bp = -1;
    for (;;) {
        unsigned op = DW(c, pc);
        if (ERR || op == 0x7a) break;
        unsigned field = op & 0x3fff, mode = op >> 14, to = (at + field) & 65535;
        if (mode == 0) { m[to] = u8(DB(c, pc + 2)); pc += 3; }
        else if (mode == 1) { unsigned v = DW(c, pc + 2); m[to] = u8(v); m[(to + 1) & 65535] = u8(v >> 8); pc += 4; }
        else {
            unsigned parent = m[(at + 0x74) & 65535] | m[(at + 0x75) & 65535] << 8;   /* read again: a field may have changed it */
            bp = (parent + field) & 65535;
            if (mode == 2) m[to] = m[bp];
            else { unsigned v = m[bp] | m[(bp + 1) & 65535] << 8; m[to] = u8(v); m[(to + 1) & 65535] = u8(v >> 8); }
            pc += 2;
        }
    }
    return bp;
}
/* 06cf with the far pointer at pointer: an object without parent, or 0 when
 * the pool is empty (the caller then writes from DS:0000) */
unsigned cycle_alloc_root(Cycle *c, unsigned pointer) {
    unsigned template_ = MW(c, pointer);
    if (ERR) return 0;
    return cycle_allocate(c, template_, 0);
}
/* 0af2: own +20/22, +28/2a = parent's + (+34, +36); +24/26, +2c/2e = parent's
 * + (+38, +3a), 32-bit */
static void relpos(Cycle *c, unsigned at) {
    uint8_t *m = c->mem;
    unsigned p = rw(m, at + 0x74);
    static const unsigned groups[2][2][2] = {{{0x20, 0x22}, {0x28, 0x2a}}, {{0x24, 0x26}, {0x2c, 0x2e}}};
    static const unsigned offsets[2][2] = {{0x34, 0x36}, {0x38, 0x3a}};
    for (int g = 0; g < 2; g++) {
        uint32_t add = rw(m, at + offsets[g][0]) | (uint32_t)rw(m, at + offsets[g][1]) << 16;
        for (int i = 0; i < 2; i++) {
            unsigned lo = groups[g][i][0], hi = groups[g][i][1];
            uint32_t v = (rw(m, p + lo) | (uint32_t)rw(m, p + hi) << 16) + add;
            ww(m, at + lo, v & 65535); ww(m, at + hi, v >> 16);
        }
    }
}
/* 1508 for the player of truck di, the result in +74 */
static unsigned controls_of(Cycle *c, unsigned di) {
    unsigned player = MB(c, di + 0x73);
    if (ERR) return 0;
    int state = ctl_read_controls(c->mem, player);
    if (state < 0) { fail(c, OFF_BAD_ADDRESS); return 0; }
    MSET(c, di + 0x74, (unsigned)state);
    return (unsigned)state;
}
static unsigned step(unsigned state) { return state & 1 ? 1 : (state & 2 ? 0xff : 0); }
static void sound_move(Cycle *c, unsigned al) { if (al & 3) cycle_sound(c, 0xb); }   /* 2bc0 */

/* 0ff8: from speed +30 and direction +33 (the table at 2faa, cosine 64
 * entries on): +34/+36 = speed x cos, +38/+3a = speed x sin, each the signed
 * product shifted right by 8 (the top byte sign-extended) */
static void velocity(Cycle *c, unsigned at) {
    uint8_t *m = c->mem;
    unsigned bx = 2 * (OW(c, at, 0x32) >> 8);
    unsigned sn = rw(m, (0x2faa + bx) & 65535), cs = rw(m, (0x2faa + ((bx + 0x80) & 0x1ff)) & 65535);
    long long speed = s16(OW(c, at, 0x30));
    unsigned values[2] = {sn, cs}, los[2] = {0x38, 0x34}, his[2] = {0x3a, 0x36};
    for (int i = 0; i < 2; i++) {
        uint32_t p = (uint32_t)(speed * s16(values[i]));
        OPUT(c, at, los[i], (p >> 8) & 65535);
        unsigned top = (p >> 24) & 255;
        OPUT(c, at, his[i], top & 0x80 ? (long long)top - 256 : (long long)top);
    }
}
/* 0b54: away from the parent (its count of children -1, its +76 cleared if
 * this one held it), then the parent [29b2 + AX] (AX >= 0, count +1) or none */
static void reparent(Cycle *c, unsigned at, unsigned ax) {
    uint8_t *m = c->mem;
    unsigned bx = OW(c, at, 0x74);
    if (bx) {
        if (OW(c, at, 0xa) & 0x2000) { OPUT(c, at, 0xa, OW(c, at, 0xa) & 0xdfff); ww(m, bx + 0x76, 0); }
        ww(m, bx + 0x78, rw(m, bx + 0x78) - 1);
    }
    if (ax & 0x8000) { OPUT(c, at, 0x74, 0); return; }
    bx = rw(m, (0x29b2 + ax) & 65535);
    OPUT(c, at, 0x74, bx); ww(m, bx + 0x78, rw(m, bx + 0x78) + 1);
}
/* 0c64: the joystick read (0434:0888) and its direction (0836 for 0680): the
 * table at 2a74 gives the velocity (+36 from the low byte, +3a from the high
 * one, fractions 0) */
static void joystick_velocity(Cycle *c, unsigned at) {
    uint8_t *m = c->mem;
    Joystick bench, *j = c->host.joystick;
    if (!j) { static const int zero[4] = {0, 0, 0, 0}; joy_init(&bench, zero, 0); j = &bench; }   /* a port that reads 0 */
    joystick_read(m, j, c->host.port_out, c->host.ctx);
    ctl_joystick(m, 0x680);
    unsigned bx = rw(m, 0x2a74 + 2 * (m[0x690] & 0x0f)), lo = bx & 255, hi = bx >> 8;
    OPUT(c, at, 0x34, 0); OPUT(c, at, 0x36, lo & 0x80 ? (long long)lo - 256 : (long long)lo);
    OPUT(c, at, 0x38, 0); OPUT(c, at, 0x3a, hi & 0x80 ? (long long)hi - 256 : (long long)hi);
}
/* 0b8d: follow the parent's flags */
static unsigned follow(Cycle *c, unsigned at, unsigned pc) {
    uint8_t *m = c->mem;
    unsigned p = OW(c, at, 0x74), cx = rw(m, p + 0xa), ax = OW(c, at, 0xa);
    if (ax & 0x20 && cx & 0x10) return u16(OW(c, at, 0x14) - 2);
    if (ax & 0x80 && cx & 0x40) { OPUT(c, at, 0x4c, 8); return pc; }
    if (ax & 0x100) relpos(c, at);
    if (ax & 0x400) OPUT(c, at, 0x5c, rw(m, p + 0x5c));
    if (ax & 0x200) OPUT(c, at, 0x1a, (OW(c, at, 0x1a) & 0x7fff) | (rw(m, p + 0x1a) & 0x8000));
    if (ax & 0x800) OPUT(c, at, 0x18, rw(m, p + 0x18) - 1);
    if (ax & 0x1000) OPUT(c, at, 0x18, rw(m, p + 0x18) + 1);
    return pc;
}

/* ---- the sign-in --------------------------------------------------------------- */
/* 25f8: the truck changes place with the computer trucks behind it */
static void grid_forward(Cycle *c, unsigned di) {
    uint8_t *m = c->mem;
    int place = s8(MB(c, di + 0x3b));
    if (ERR) return;
    unsigned cx = (3 - place) & 65535;
    if (!cx) return;
    unsigned si = (0x9cec + 2 * place) & 65535;
    for (;;) {
        unsigned bx = rw(m, si + 2);
        if (m[(bx + 2) & 65535] & 9) return;
        m[(bx + 0x3b) & 65535] = u8(m[(bx + 0x3b) & 65535] - 1);
        unsigned old = rw(m, si);
        ww(m, si, bx); bx = old;
        m[(bx + 0x3b) & 65535] = u8(m[(bx + 0x3b) & 65535] + 1);
        si = (si + 2) & 65535; ww(m, si, bx);
        cx = (cx - 1) & 65535;
        if (!cx) return;
    }
}
static void h1e8(Cycle *c, unsigned at) {   /* 257f */
    uint8_t *m = c->mem;
    unsigned di = OW(c, at, 6);
    if (MB(c, di) == 2) {
        if (m[0x5396] & 0x10) MSET(c, di + 0x73, 1);
        else if (m[0x6a2] & 0x10) MSET(c, di + 0x73, 3);
    }
    unsigned player = MB(c, di + 0x73);
    if (ERR) return;
    if (player == 0 || player == 1) {
        unsigned map = player == 0 ? 0x5382 : 0x5396;
        OSET(c, at, 8, m[map] & 0x10);
        if (ctl_keyboard(m, map) < 0) fail(c, OFF_BAD_ADDRESS);
    } else {
        unsigned joy = player == 2 ? 0x680 : 0x692;
        OSET(c, at, 8, m[joy + 0x10] & 0x10);
        ctl_joystick(m, joy);
    }
}
static void letter(Cycle *c, unsigned at, unsigned ah) {   /* 26b2's letter */
    uint8_t *m = c->mem;
    m[0x9240] = m[0x9242 + ah];
    unsigned x = (s8(OB(c, at, 0xe)) * 8 + 0x3c + rw(m, OW(c, at, 0x1c))) & 65535;
    ww(m, 0x923a, ((x & 255) << 8) | (x >> 8));
    wait_tick(c, 0x81ea); m[0x995] = 1; print_text(c, 0x9238); m[0x995] = 0;
}
static void h230(Cycle *c, unsigned at) {   /* 26b2 initials */
    uint8_t *m = c->mem;
    print_text(c, OW(c, at, 0x10));
    unsigned di = OW(c, at, 6), state = controls_of(c, di);
    if (ERR) return;
    OSET(c, at, 0xf, OB(c, at, 0xf) - 1);
    if (!(OB(c, at, 0xf) & 1)) {
        unsigned ah = (step(state) + OB(c, at, 0xc)) & 255;
        if (ah & 0x80) ah = 0x1c;
        if (ah > 0x1c) ah = 0;
        OSET(c, at, 0xc, ah); sound_move(c, state);
        OPUT(c, at, 0x5c, ah == 0x1c ? 2 : 5);
        OPUT(c, at, 0x3a, (ah / 5) * 0x11 + 0x11); OPUT(c, at, 0x36, (ah % 5) * 0x14 + 3);
    }
    state = MB(c, di + 0x74);
    if (ERR) return;
    if (state & 0x40) {
        cycle_sound(c, 0xc);
        unsigned ah = OB(c, at, 0xc), al = 0x58;
        if (ah < 0x1b) al = m[0x9242 + ah];
        else if (ah > 0x1b) { OPUT(c, at, 0x4c, 8); return; }
        m[(di + 5 + OB(c, at, 0xe)) & 65535] = u8(al);
        OSET(c, at, 0xd, ah | 0x80); letter(c, at, ah);
        return;
    }
    unsigned ah = OB(c, at, 0xd);
    if (ah & 0x80) {
        OSET(c, at, 0xd, 0); ah &= 0x7f;
        if (ah == 0x1b) {
            letter(c, at, ah);
            unsigned al = (OB(c, at, 0xe) - 1) & 255;
            if (!(al & 0x80)) OSET(c, at, 0xe, al);
            return;
        }
        letter(c, at, ah);
        unsigned al = (OB(c, at, 0xe) + 1) & 255;
        if (al == 3) OPUT(c, at, 0x4c, 4);
        else OSET(c, at, 0xe, al);
        return;
    }
    letter(c, at, OB(c, at, 0xf) & 1 ? 0x1b : 0x1a);
}
typedef struct { unsigned limit, box, columns, dy, y0, dx, x0, table, slot, text, store, back; } Field;
static void field_draw(Cycle *c, const Field *f, unsigned ah) {
    uint8_t *m = c->mem;
    ww(m, f->slot, rw(m, (f->table + 2 * ah) & 65535));
    wait_tick(c, 0x81ea); m[0x995] = 1; print_text(c, f->text); m[0x995] = 0;
}
/* 2805 (month) and 2925 (day): the same code with different numbers */
static void field(Cycle *c, unsigned at, const Field *f) {
    uint8_t *m = c->mem;
    print_text(c, OW(c, at, 0x10));
    unsigned di = OW(c, at, 6), state = controls_of(c, di);
    if (ERR) return;
    OSET(c, at, 0xf, OB(c, at, 0xf) - 1);
    if (!(OB(c, at, 0xf) & 1)) {
        unsigned ah = (step(state) + OB(c, at, 0xc)) & 255;
        if (ah & 0x80) ah = f->limit;
        if (ah > f->limit) ah = 0;
        OSET(c, at, 0xc, ah); sound_move(c, state);
        OPUT(c, at, 0x5c, ah == f->limit ? 2 : f->box);
        OPUT(c, at, 0x3a, (ah / f->columns) * f->dy + f->y0); OPUT(c, at, 0x36, (ah % f->columns) * f->dx + f->x0);
    }
    state = MB(c, di + 0x74);
    if (ERR) return;
    if (state & 0x40) {
        cycle_sound(c, 0xc);
        unsigned ah = OB(c, at, 0xc);
        if (ah > f->back) { OPUT(c, at, 0x4c, 8); return; }
        if (ah == f->back) { OPUT(c, at, 0x4c, 0xc); return; }
        ww(m, di + f->store, rw(m, f->table + 2 * ah)); OSET(c, at, 0xd, ah | 0x80); field_draw(c, f, ah);
        return;
    }
    unsigned ah = OB(c, at, 0xd);
    if (ah & 0x80) { OSET(c, at, 0xd, 0); field_draw(c, f, ah & 0x7f); OPUT(c, at, 0x4c, 4); return; }
    field_draw(c, f, OB(c, at, 0xf) & 1 ? f->back + 1 : f->back);
}
static void year_draw(Cycle *c, unsigned ah) {
    uint8_t *m = c->mem;
    ww(m, 0x93fb, rw(m, (0x93fe + 2 * ah) & 65535));
    wait_tick(c, 0x81ea); m[0x995] = 1; print_text(c, 0x93f3); m[0x995] = 0;
}
static void h248(Cycle *c, unsigned at) {   /* 2a45 year */
    uint8_t *m = c->mem;
    print_text(c, OW(c, at, 0x10));
    unsigned di = OW(c, at, 6), state = controls_of(c, di);
    if (ERR) return;
    unsigned ah = (step(state) + OB(c, at, 0xc)) & 255;
    if (!(ah & 0x80) && ah <= 0x45) {
        OSET(c, at, 0xc, ah); sound_move(c, state);
        OPUT(c, at, 0x3a, (ah / 5) * 9 + 0x11); OPUT(c, at, 0x36, (ah % 5) * 0x12 + 3);
        if (MB(c, di + 0x74) & 0x40) {
            cycle_sound(c, 0xc);
            ah = (OB(c, at, 0xc) + OB(c, at, 0xe)) & 255;
            ww(m, di + 0xc, rw(m, (0x93fe + 2 * ah) & 65535)); OSET(c, at, 0xd, ah | 0x80); year_draw(c, ah);
            return;
        }
    } else {
        ah = (ah + OB(c, at, 0xe)) & 255;
        if (!(ah & 0x80) && ah <= 0x63) { OPUT(c, at, 0x4c, 8); return; }
    }
    if (ERR) return;
    ah = OB(c, at, 0xd);
    if (ah & 0x80) { OSET(c, at, 0xd, 0); year_draw(c, ah & 0x7f); OPUT(c, at, 0x4c, 4); return; }
    OSET(c, at, 0xf, OB(c, at, 0xf) - 1);
    year_draw(c, OB(c, at, 0xf) & 1 ? 0x65 : 0x64);
}
static void h250(Cycle *c, unsigned at) {   /* 2b4d nation */
    uint8_t *m = c->mem;
    unsigned di = OW(c, at, 6);
    controls_of(c, di);
    if (ERR) return;
    OSET(c, at, 0xf, OB(c, at, 0xf) - 1);
    if (!(OB(c, at, 0xf) & 3)) {
        unsigned al = MB(c, di + 0x74), ah = al & 5 ? 1 : (al & 0xa ? 0xff : 0);
        al |= ah; ah = (ah + OB(c, at, 0xc)) & 255;
        if (ah & 0x80) ah = 7;
        if (ah > 7) ah = 0;
        OSET(c, at, 0xc, ah); sound_move(c, al);
        OPUT(c, at, 0x3a, rw(m, 0x94f4 + 2 * ah)); OPUT(c, at, 0x36, 2);
    }
    if (MB(c, di + 0x74) & 0x40) { cycle_sound(c, 0xc); MSET(c, di + 0xe, OB(c, at, 0xc)); OPUT(c, at, 0x4c, 4); }
}

/* ---- the shop ------------------------------------------------------------------- */
static void h258(Cycle *c, unsigned at) {   /* 2d68 menu */
    uint8_t *m = c->mem;
    unsigned di = OW(c, at, 6), state = controls_of(c, di);
    if (ERR) return;
    OSET(c, at, 0xf, OB(c, at, 0xf) - 1);
    if (!(OB(c, at, 0xf) & 1)) {
        unsigned ah = (step(state) + OB(c, at, 0xc)) & 255;
        if (ah & 0x80) ah = 5;
        if (ah > 5) ah = 0;
        if (ah != OB(c, at, 0xc)) { OPUT(c, at, 0x5c, 0); OSET(c, at, 0xc, ah); }
        sound_move(c, state);
        OPUT(c, at, 0x36, rw(m, 0x951a + 2 * ah)); OPUT(c, at, 0x3a, rw(m, 0x9526 + 2 * ah));
    }
    if (!(MB(c, di + 0x74) & 0x40) || ERR) return;
    cycle_sound(c, 0xd);
    unsigned ah = OB(c, at, 0xc);
    if (ah == 3) { OPUT(c, at, 0x4c, 4); return; }
    unsigned limit = m[0x9514 + ah], cost = m[0x9508 + ah], fld = m[0x950e + ah];
    if (limit == m[(di + fld) & 65535]) { cycle_sound(c, 0xe); return; }
    unsigned money = rw(m, di + 0xf);
    if (cost > money) {
        if (!MB(c, di + 1)) { OPUT(c, at, 0xa, OW(c, at, 0xa) | 0x40); return; }
        MSET(c, di + 1, MB(c, di + 1) - 1); money = daa_word_add(money, 0x20);
    }
    m[(di + fld) & 65535] = u8(m[(di + fld) & 65535] + 1);
    ww(m, di + 0xf, das_word(money, cost)); OPUT(c, at, 0x4c, 8);
}
static void h260(Cycle *c, unsigned at) {   /* 2e40 demo menu */
    uint8_t *m = c->mem;
    OSET(c, at, 0xd, (OB(c, at, 0xd) - 1) & 15);
    if (OB(c, at, 0xd)) return;
    unsigned r = ai_random_word(m), ah, al;
    if (r & 1) { ah = 1; al = 1; } else { ah = 0xff; al = 2; }
    ah = (ah + OB(c, at, 0xc)) & 255;
    if (ah & 0x80) ah = 5;
    if (ah > 5) ah = 0;
    OSET(c, at, 0xc, ah); sound_move(c, al);
    OPUT(c, at, 0x36, rw(m, 0x951a + 2 * ah)); OPUT(c, at, 0x3a, rw(m, 0x9526 + 2 * ah));
}
static void place(uint8_t *m, unsigned si, unsigned x, unsigned y) {
    unsigned table = rw(m, rw(m, 0x9504) + 0x1c);
    ww(m, si + 0x2a, x + rw(m, table)); ww(m, si + 0x2e, y + rw(m, table + 2));
}
static void h268(Cycle *c, unsigned at) {   /* 2c2f displays */
    uint8_t *m = c->mem;
    ww(m, 0x9504, at);
    unsigned truck = OW(c, at, 6);
    ww(m, 0x9506, truck);
    unsigned si = cycle_alloc_root(c, 0x3206);
    if (ERR) return;
    place(m, si, 0x32, 0x8b);
    unsigned al = m[0x1a8] + 1;
    if (al >= 0x2b) al = 0x1b;
    ww(m, si + 0x5c, (m[0x1ad + al] & 7) * 2);
    si = cycle_alloc_root(c, 0x320a);
    if (ERR) return;
    place(m, si, 0x26, 0x1f);
    ww(m, si + 0xc, 0x101);
    unsigned ah = MB(c, truck + 0x5d);
    if (ah > 0x28) ah = 0x28;
    ww(m, si + 0xe, (0x28 - ah) | ah << 8); ww(m, si + 0x10, 0);
    static const unsigned bars[4][3] = {{0x55, 0x2a, 0x60}, {0x26, 0x58, 0x5f}, {0x55, 0x58, 0x61}, {0x26, 0x86, 0x5e}};
    for (int i = 0; i < 4 && !ERR; i++) {
        si = cycle_alloc_root(c, 0x320a);
        if (ERR) return;
        place(m, si, bars[i][0], bars[i][1]);
        ah = MB(c, truck + bars[i][2]);
        ww(m, si + 0xc, ((5 - ah) & 255) | ah << 8); ww(m, si + 0xe, 0x505); ww(m, si + 0x10, 1);
    }
}
static void h270(Cycle *c, unsigned at) {   /* 2d53 money text */
    uint8_t *m = c->mem;
    unsigned bx = OW(c, at, 0x10), di = OW(c, at, 6), si = OW(c, at, 0x1c);
    MSET(c, (long long)bx + 0x21, MB(c, di + 1) + 0x30);
    for (unsigned kk = 0x25; kk <= 0x29; kk += 2) ww(m, bx + kk, 0x2020);
    long long p = (long long)bx + 0x2a;
    unsigned dx = rw(m, di + 0xf);
    for (unsigned cx = 4; cx >= 1; cx--) {
        if (dx == 0) break;
        if (cx == 2) { MSET(c, p, 0x2c); p--; }
        MSET(c, p, (dx & 15) + 0x30); p--; dx >>= 4;
    }
    MSET(c, p, 0x24);
    unsigned x = rw(m, si) + 0x11;
    MSET(c, (long long)bx + 0xc, (x >> 8) & 255); MSET(c, (long long)bx + 0xd, x & 255);
    MSET(c, (long long)bx + 0x1f, (x >> 8) & 255); MSET(c, (long long)bx + 0x20, x & 255);
    MSET(c, (long long)bx + 0xe, rw(m, si + 2) + 0xd);
    if (ERR) return;
    m[0x98e] = 0xff; m[0x994] = 0;
    unsigned saved = rw(m, 0x73a);
    ww(m, 0x73a, rw(m, 0x16a));
    unsigned width = text_width_of(c, (long long)bx + 0x25);
    if (ERR) return;
    ww(m, 0x73a, saved);
    x = (0x5c - width + rw(m, si)) & 65535;
    MSET(c, (long long)bx + 0x23, x >> 8); MSET(c, (long long)bx + 0x24, x & 255);
    if (ERR) return;
    wait_tick(c, 0x81ea); print_text(c, bx);
}
static void h288(Cycle *c, unsigned at, unsigned ax) {   /* 301a countdown */
    uint8_t *m = c->mem;
    unsigned bx = ax;
    OSET(c, at, 0xc, OB(c, at, 0xc) - 1);
    if (OB(c, at, 0xc) & 0x80) {
        unsigned ah = (OB(c, at, 0xd) - 1) & 255;
        if (ah & 0x80) { OPUT(c, at, 0x4c, 8); return; }
        OSET(c, at, 0xc, 0x1e); OSET(c, at, 0xd, ah); m[(bx + 0x14) & 65535] = u8(ah + 0x30);
        wait_tick(c, 0x81ec); print_text(c, bx); OPUT(c, at, 0x4c, 0xc);
    }
    if (ERR) return;
    unsigned di = OW(c, at, 6);
    if (controls_of(c, di) & 0x40) OPUT(c, at, 0x4c, 4);
}
static void h290(Cycle *c, unsigned at) {   /* 2f3a computer ahead */
    uint8_t *m = c->mem;
    unsigned cx = 0, bx = (MB(c, OW(c, at, 6) + 0x3b) * 2) & 65535;
    if (ERR) return;
    while (bx) {
        bx = (bx - 2) & 65535;
        if (m[(rw(m, 0x9cec + bx) + 2) & 65535] & 2) cx = 0xffff;
    }
    OPUT(c, at, 8, cx);
}
static void h2e8(Cycle *c, unsigned at) {   /* 3098 credit */
    uint8_t *m = c->mem;
    unsigned di = OW(c, at, 6), key = m[0x9661 + MB(c, di)];
    if (ERR || !key || !ctl_key_down(m, key)) return;
    unsigned money = rw(m, di + 0xf);
    if (money < 0x25 || MB(c, di + 1) >= 9) return;
    MSET(c, di + 1, MB(c, di + 1) + 1); ww(m, di + 0xf, das_word(money, 0x25)); OPUT(c, at, 0x4c, 8);
}

/* ---- the results -------------------------------------------------------------------- */
static unsigned string_at(uint8_t *m, unsigned n, unsigned extra) {
    return (rw(m, (0x83ee + 2 * s8(n)) & 65535) + 0x83e8 + extra) & 65535;
}
static long long zeros(Cycle *c, long long si) {
    MSET(c, si, 0x30); MSET(c, si + 1, 0x2c); MSET(c, si + 2, 0x30); MSET(c, si + 3, 0x30); MSET(c, si + 4, 0x30);
    return si + 5;
}
/* 397a: four BCD digits as ASCII (AH, AL, CH, CL) */
static void digits(unsigned ax, unsigned d[4]) {
    for (int i = 0; i < 4; i++) d[i] = ((ax >> (12 - 4 * i)) & 15) + 0x30;
}
/* 3270: '$' + prize by place + '0,000' (+ ' + $' bonus '0,000'), the width */
static unsigned money_text(Cycle *c, unsigned n, unsigned di) {
    uint8_t *m = c->mem;
    unsigned start = string_at(m, n, 0);
    long long si = start;
    MSET(c, si, 0x24); si++;
    ww(m, si, rw(m, 0x97d2 + 2 * MB(c, di + 0x3b))); si += 2;
    si = zeros(c, si);
    unsigned al = MB(c, di + 0x62);
    if (al) {
        MSET(c, si, 0x20); MSET(c, si + 1, 0x2b); MSET(c, si + 2, 0x20); MSET(c, si + 3, 0x24); si += 4;
        if (al >> 4) { MSET(c, si, (al >> 4) + 0x30); si++; }
        MSET(c, si, (al & 15) + 0x30); si++;
        si = zeros(c, si);
    }
    MSET(c, si, 0);
    if (ERR) return 0;
    m[0x98e] = 0xff; m[0x994] = 0;
    return text_width_of(c, start);
}
/* the ' (1st)' tail of 32fc/3390 */
static long long place_text(Cycle *c, long long si, unsigned index) {
    uint8_t *m = c->mem;
    unsigned p = 0x966f + index * 4;
    ww(m, si + 2, rw(m, p));
    unsigned cx = rw(m, p + 2);
    MSET(c, si + 4, cx & 255); ww(m, si, 0x2820); si += 5;
    if (cx >> 8) { MSET(c, si, cx >> 8); si++; }
    MSET(c, si, 0x29);
    return si + 1;
}
static void mean_text(Cycle *c, unsigned n, unsigned di) {   /* 32fc */
    uint8_t *m = c->mem;
    long long si = string_at(m, n, 6);
    unsigned track = m[0x1aa];
    int kk = s8((track * 3) & 255);
    unsigned mean = rw(m, (di + kk + 0x24) & 65535), bx = (mean * 5) & 65535;
    unsigned ax = bx >= 0x270f ? 0x9999 : packed_decimal(bx), d[4];
    digits(ax, d);
    MSET(c, si, d[0]); MSET(c, si + 1, d[1]); MSET(c, si + 2, 0x2e); MSET(c, si + 3, d[2]); MSET(c, si + 4, d[3]);
    si += 5;
    int found = awards_find_time(m, (track * 0x18 + 0x9bfd) & 65535, ax);
    if (found >= 0 && found < 10) si = place_text(c, si, (unsigned)found);
    MSET(c, si, 0);
}
static void best_text(Cycle *c, unsigned n, unsigned di) {   /* 3390 */
    uint8_t *m = c->mem;
    long long si = string_at(m, n, 7);
    unsigned track = m[0x1aa], ax = rw(m, (di + track * 2 + 0x13) & 65535);
    if (ax == 0x9999) {
        MSET(c, si, 0x20); MSET(c, si + 1, 0x2d); MSET(c, si + 2, 0x2d); MSET(c, si + 3, 0x2d); si += 4;
    } else {
        unsigned d[4];
        digits(ax, d);
        MSET(c, si, d[0]); MSET(c, si + 1, d[1]); MSET(c, si + 2, 0x2e); MSET(c, si + 3, d[2]); MSET(c, si + 4, d[3]);
        si += 5;
        int found = awards_find_time(m, (track * 0x18 + 0x9b3d) & 65535, ax);
        if (found >= 0 && found < 10) si = place_text(c, si, (unsigned)found);
    }
    MSET(c, si, 0);
}
/* 315b: three pictures and the texts of one panel; returns BX as left */
static unsigned result_panel(Cycle *c, unsigned di, unsigned cx, unsigned bx) {
    uint8_t *m = c->mem;
    unsigned panel = bx;
    if (MB(c, di + 2) & 2) {
        bx = (MB(c, di + 0x3b) * 2) & 65535;
        for (;;) {
            bx = (bx + 2) & 65535;
            if (bx == 8) { di = 0x4eb8; break; }
            unsigned t = rw(m, (0x9cec + bx) & 65535);
            if (m[t] == 3) continue;
            if (!(m[(t + 2) & 65535] & 2)) continue;
            break;
        }
    }
    if (ERR) return 0;
    unsigned si = cycle_alloc_root(c, 0x320e);
    if (ERR) return 0;
    ww(m, si + 0x2a, cx);
    unsigned ax = (MB(c, di) * 6) & 255;
    long long sax = ax & 0x80 ? (long long)ax - 256 : (long long)ax;
    ww(m, si + 0x5c, (unsigned)sax);
    si = cycle_alloc_root(c, 0x3212);
    if (ERR) return 0;
    cx = (cx + 6) & 65535; ww(m, si + 0x2a, cx); ww(m, si + 0x5c, (unsigned)sax);
    si = cycle_alloc_root(c, 0x3216);
    if (ERR) return 0;
    cx = (cx + 0x12) & 65535; ww(m, si + 0x2a, cx);
    ww(m, si + 0x5c, u16(s8(m[0x9666 + MB(c, di + 0xe)])));
    if (ERR) return 0;
    if (MB(c, di + 2) & 2) {
        bx = rw(m, 0x969d + panel);
        unsigned p = (s8(MB(c, di + 0x3b)) * 4 + 0x966f) & 65535;
        ww(m, bx + 0x16, rw(m, p)); MSET(c, (long long)bx + 0x18, MB(c, (long long)p + 2));
        unsigned al = (MB(c, di) + 0xd) & 255;
        MSET(c, (long long)bx + 0x27, al); MSET(c, (long long)bx + 0x29, al + 4);
        if (ERR) return 0;
        unsigned w = money_text(c, MB(c, (long long)bx + 0x24), di), x = (0x5c - w) & 65535;
        x = (x >> 1) | (x & 0x8000);   /* SAR */
        MSET(c, (long long)bx + 0x21, x >> 8); MSET(c, (long long)bx + 0x22, x & 255);
        MSET(c, (long long)bx + 0xb, MB(c, di) + 0x31);
    } else {
        bx = rw(m, 0x96a3 + panel);
        unsigned p = (s8(MB(c, di + 0x3b)) * 4 + 0x966f) & 65535;
        ww(m, bx + 0x16, rw(m, p)); MSET(c, (long long)bx + 0x18, MB(c, (long long)p + 2));
        ww(m, bx + 0x22, rw(m, di + 5)); MSET(c, (long long)bx + 0x24, MB(c, di + 7));
        if (ERR) return 0;
        unsigned w = money_text(c, MB(c, (long long)bx + 0x2a), di), x = (0x5c - w) & 65535;
        x = (x >> 1) | (x & 0x8000);
        MSET(c, (long long)bx + 0x27, x >> 8); MSET(c, (long long)bx + 0x28, x & 255);
        if (ERR) return 0;
        mean_text(c, MB(c, (long long)bx + 0x30), di);
        if (ERR) return 0;
        best_text(c, MB(c, (long long)bx + 0x36), di);
        MSET(c, (long long)bx + 0xb, 0x35);
    }
    return bx;
}
static void result_record(Cycle *c) {   /* 3421 */
    uint8_t *m = c->mem;
    unsigned bx = m[0x1aa] * 2, di = rw(m, 0x9cec), d[4];
    digits(rw(m, 0x9b0d + bx), d);
    m[0x981a] = u8(d[0]); m[0x981b] = u8(d[1]); m[0x981c] = 0x2e; m[0x981d] = u8(d[2]); m[0x981e] = u8(d[3]);
    bx *= 2;
    ww(m, 0x980e, rw(m, 0x9b1d + bx)); ww(m, 0x9810, rw(m, 0x9b1f + bx));
    digits(rw(m, di + 0x44), d);
    m[0x97fd] = u8(d[0]); m[0x97fe] = u8(d[1]); m[0x97ff] = 0x2e; m[0x9800] = u8(d[2]); m[0x9801] = u8(d[3]);
    unsigned ax;
    if (MB(c, di + 2) & 2) { unsigned kk = MB(c, di) * 4; ax = rw(m, 0x9820 + kk); bx = rw(m, 0x9822 + kk); }
    else { ax = 0x20 | MB(c, di + 5) << 8; bx = rw(m, di + 6); }
    if (ERR) return;
    ww(m, 0x97ea, ax); ww(m, 0x97ec, bx);
}
static void h280(Cycle *c) {   /* 3120 */
    static const unsigned panels[3][4] = {{0x4c72, 7, 0, 0x9697}, {0x4d34, 0x73, 2, 0x969b}, {0x4df6, 0xdf, 4, 0x9699}};
    for (int i = 0; i < 3; i++) {
        unsigned v = result_panel(c, panels[i][0], panels[i][1], panels[i][2]);
        if (ERR) return;
        ww(c->mem, panels[i][3], v);
    }
    result_record(c);
}
static void h2b8(Cycle *c) {   /* 349b */
    uint8_t *m = c->mem;
    unsigned texts[4] = {0x97da, rw(m, 0x9697), rw(m, 0x969b), rw(m, 0x9699)};
    for (int i = 0; i < 4 && !ERR; i++) print_text(c, texts[i]);
}
static void h278(Cycle *c) {   /* 3529 the Ironman Club table */
    uint8_t *m = c->mem;
    print_text(c, 0x985a);
    unsigned di = 0x98b9;
    for (unsigned cx = 10; cx > 0 && !ERR; cx--) {
        unsigned e = (rw(m, di) + 0x98b9) & 65535, dx = 10 - cx;
        ww(m, 0x988, dx * 13 + 0x26); ww(m, 0x986, 0x4d);
        ww(m, 0x9882, rw(m, 0x98a5 + dx * 2));
        ww(m, 0x9886, rw(m, e)); m[0x9888] = u8(MB(c, (long long)e + 2));
        if (ERR) return;
        print_text(c, 0x9881);
        if (ERR) return;
        unsigned d[4];
        digits(rw(m, e + 0xa), d);
        unsigned dh = d[0], dl = d[1], ch = d[2], cl = d[3];
        if (dh == 0x30) {
            dh = 0x20;
            if (dl == 0x30) {
                dl = 0x20;
                if (ch == 0x30) { ch = 0x20; if (cl == 0x30) cl = 0x20; }
            }
        }
        unsigned bx = 0x9899;
        if (cl != 0x20) {
            m[bx] = u8(cl); bx--;
            if (ch != 0x20) {
                m[bx] = u8(ch); bx--;
                if (dl != 0x20) {
                    bx -= 7;
                    ww(m, bx + 6, rw(m, 0x989f)); ww(m, bx + 4, rw(m, 0x989d)); ww(m, bx + 2, rw(m, 0x989b)); m[bx + 1] = u8(dl);
                    if (dh != 0x20) { m[bx] = u8(dh); bx--; }
                }
            }
        }
        m[bx] = 0x24; bx--; m[bx] = 0x11;
        unsigned width = text_width_of(c, bx), x = (0xdc - width) & 65535;
        if (ERR) return;
        bx -= 3; m[bx + 1] = u8(x >> 8); m[bx + 2] = u8(x); m[bx] = 2;
        print_text(c, bx);
        di += 2;
    }
}

/* ---- the instructions ------------------------------------------------------------------ */
int screens_opcode(Cycle *c, unsigned at, unsigned *pcp, unsigned op, unsigned ax) {
    uint8_t *m = c->mem;
    unsigned pc = *pcp, h = op & 0x7ff8, size = op & 7;
#define ARG(n) DW(c, (long long)pc + 2 * (n))
    switch (h) {
    case 0x000: OPUT(c, at, 0x40, ax); OPUT(c, at, 0x44, pc + 2); break;
    case 0x008: {
        unsigned old = OW(c, at, 0x40);
        if (old) OPUT(c, at, 0x40, old - 1);
        if (old != 1) pc = OW(c, at, 0x44);
        break;
    }
    case 0x020: {
        if (!c->live) PUSH(c->timers, (uint32_t)u16(at) << 16 | u16(ax));
        uint32_t now;
        if (c->host.settimer_clock && c->host.settimer_clock(c->host.ctx, at, ax, &now)) {
            ww(m, 0x6c1, now & 65535); ww(m, 0x6c3, now >> 16);
        }
        uint32_t v = (rw(m, 0x6c1) | (uint32_t)rw(m, 0x6c3) << 16) + ax;
        OPUT(c, at, 0x46, v & 65535); OPUT(c, at, 0x48, v >> 16); OPUT(c, at, 0x4a, pc + 2);
        break;
    }
    case 0x028: {
        uint32_t target = OW(c, at, 0x46) | (uint32_t)OW(c, at, 0x48) << 16, now = rw(m, 0x6c1) | (uint32_t)rw(m, 0x6c3) << 16;
        if (target >= now) pc = OW(c, at, 0x4a);
        break;
    }
    case 0x038: {
        unsigned bx = (OW(c, at, 8) << 1) & 65535;
        if (bx) pc = u16(pc + DW(c, u16(pc + bx)));
        break;
    }
    case 0x050:
        if (ax == 0) { OPUT(c, at, 0x4e, 0); OPUT(c, at, 0x50, 0); OPUT(c, at, 0x52, 0); OPUT(c, at, 0x54, 0); }
        else {
            unsigned target = ARG(2);
            MPUT(c, (at + 0x4c + (ax >> 1)) & 65535, target == 0xffff ? 0 : u16(target + pc));
        }
        break;
    case 0x058: OPUT(c, at, 0x14, size ? u16(pc + ax) : 0); break;
    /* ---- instructions no script of the game uses (phase 16) ---- */
    case 0x060: OPUT(c, at, 0x30, ax); velocity(c, at); break;   /* 0fe6..0ff5: speed +30, direction +32 */
    case 0x068: OPUT(c, at, 0x30, OW(c, at, 0x30) + ax); velocity(c, at); break;
    case 0x070: OPUT(c, at, 0x32, ax); velocity(c, at); break;
    case 0x078: OPUT(c, at, 0x32, OW(c, at, 0x32) + ax); velocity(c, at); break;
    case 0x080: OPUT(c, at, 0x36, ax); OPUT(c, at, 0x3a, ARG(2)); OPUT(c, at, 0x34, 0); OPUT(c, at, 0x38, 0); break;   /* 09ca */
    case 0x088: OPUT(c, at, 0x36, OW(c, at, 0x36) + ax); OPUT(c, at, 0x3a, OW(c, at, 0x3a) + ARG(2)); break;   /* 09dd */
    case 0x090: {   /* 09e8: position (both pairs) */
        unsigned dy = ARG(2);
        OPUT(c, at, 0x22, ax); OPUT(c, at, 0x2a, ax); OPUT(c, at, 0x26, dy); OPUT(c, at, 0x2e, dy);
        OPUT(c, at, 0x20, 0); OPUT(c, at, 0x28, 0); OPUT(c, at, 0x24, 0); OPUT(c, at, 0x2c, 0);
        break;
    }
    case 0x0a0: case 0x0a8: {   /* 0a18, 0a31: position + velocity, 32 bits */
        unsigned base = h == 0x0a0 ? 0x20 : 0x28, pairs[2][2] = {{base, 0x34}, {base + 4, 0x38}};
        for (int i = 0; i < 2; i++) {
            unsigned pos = pairs[i][0], vel = pairs[i][1];
            uint64_t v = (uint64_t)(OW(c, at, pos) | (uint32_t)OW(c, at, pos + 2) << 16) + (OW(c, at, vel) | (uint32_t)OW(c, at, vel + 2) << 16);
            OPUT(c, at, pos, v & 65535); OPUT(c, at, pos + 2, (v >> 16) & 65535);
        }
        break;
    }
    case 0x0b0:   /* 0a4a: +28/+2c from +20/+24 less the scroll */
        OPUT(c, at, 0x28, OW(c, at, 0x20)); OPUT(c, at, 0x2a, OW(c, at, 0x22) - rw(m, 0x29b4));
        OPUT(c, at, 0x2c, OW(c, at, 0x24)); OPUT(c, at, 0x2e, OW(c, at, 0x26) - rw(m, 0x29b6));
        break;
    case 0x0b8:   /* 0a6b: back */
        OPUT(c, at, 0x20, OW(c, at, 0x28)); OPUT(c, at, 0x22, OW(c, at, 0x2a) + rw(m, 0x29b4));
        OPUT(c, at, 0x24, OW(c, at, 0x2c)); OPUT(c, at, 0x26, OW(c, at, 0x2e) + rw(m, 0x29b6));
        break;
    case 0x0c0: {   /* 0a8c: positions masked, fractions cleared */
        unsigned dy = ARG(2);
        OPUT(c, at, 0x22, OW(c, at, 0x22) & ax); OPUT(c, at, 0x2a, OW(c, at, 0x2a) & ax);
        OPUT(c, at, 0x26, OW(c, at, 0x26) & dy); OPUT(c, at, 0x2e, OW(c, at, 0x2e) & dy);
        OPUT(c, at, 0x20, 0); OPUT(c, at, 0x24, 0); OPUT(c, at, 0x28, 0); OPUT(c, at, 0x2c, 0);
        break;
    }
    case 0x0d8: pc = u16(OW(c, at, 0x72) - 2); break;   /* 0efe: back to where a contact took it */
    case 0x0e0: {   /* 0f05: the first argument with a contact bit */
        unsigned cx = size, bx = pc + 2;
        const uint8_t *d = c->k->data;
        for (;;) {
            unsigned v = (d[bx & 65535] | d[(bx + 1) & 65535] << 8) & OW(c, at, 0x6e);
            bx = (bx + 2) & 65535; cx = (cx - 1) & 65535;
            if (v || !cx) break;
        }
        OPUT(c, at, 8, !cx ? 0 : (size - cx + 1) & 65535);
        break;
    }
    case 0x0f0: OPUT(c, at, 0x18, ax); break;   /* 0d17: the drawing order */
    case 0x140: reparent(c, at, ax); break;     /* 0b54 */
    case 0x1c0: joystick_velocity(c, at); break;   /* 0c64 */
    case 0x098: {
        unsigned dy = ARG(2);
        OPUT(c, at, 0x22, OW(c, at, 0x22) + ax); OPUT(c, at, 0x2a, OW(c, at, 0x2a) + ax);
        OPUT(c, at, 0x26, OW(c, at, 0x26) + dy); OPUT(c, at, 0x2e, OW(c, at, 0x2e) + dy);
        break;
    }
    case 0x110: case 0x118: case 0x120: {
        unsigned child = cycle_allocate(c, ax, at);
        if (child) {
            OPUT(c, at, 0x78, OW(c, at, 0x78) + 1);
            if (h == 0x118) relpos(c, child);
            if (h == 0x120) { OPUT(c, child, 0xa, OW(c, child, 0xa) | 0x2000); OPUT(c, at, 0x76, child); }
        }
        break;
    }
    case 0x128: { unsigned p = OW(c, at, 0x74); MPUT(c, (long long)p + 0xa, MW(c, (long long)p + 0xa) | ax); break; }
    case 0x130: { unsigned p = OW(c, at, 0x74); MPUT(c, (long long)p + 0xa, MW(c, (long long)p + 0xa) & u16(~ax)); break; }
    case 0x138: relpos(c, at); break;
    case 0x148: pc = follow(c, at, pc); break;
    case 0x160: {
        /* 0c09 does not save BP, and 0732 leaves in it parent + field for the
         * fields copied from the parent: then the dispatcher steps by
         * 2 * (BP + 1) and yields if that sum has bit 15 */
        long long bp = apply_template(c, at, ax);
        if (bp >= 0) {
            bp = (bp + 1) & 65535;
            c->has_step_override = 1; c->step_override_step = u16(bp << 1); c->step_override_stop = (bp & 0x8000) != 0;
        }
        break;
    }
    case 0x168: OPUT(c, at, 8, ai_random_word(m) & ARG(1)); break;
    case 0x180: OPUT(c, at, 8, ax & OW(c, at, 0xa)); break;
    case 0x1a0: m[(at + ARG(2)) & 65535] = u8(ax); break;
    case 0x1a8: MPUT(c, (at + ARG(2)) & 65535, ax); break;   /* any offset: [si+n] in the data segment */
    case 0x1b8: PUSH(c->script_audio, u16(ax)); cycle_request(c, REQ_SCRIPT_AUDIO_B, ax); break;
    case 0x1b0: PUSH(c->script_audio, u16(ax)); cycle_request(c, REQ_SCRIPT_AUDIO, ax); break;
    /* the screens */
    case 0x1e0: print_text(c, ax); break;   /* 2651 */
    case 0x1e8: h1e8(c, at); break;
    case 0x1f0: {   /* 25e3 */
        unsigned di = OW(c, at, 6);
        MSET(c, di + 2, (MB(c, di + 2) & 0xfd) | 1); grid_forward(c, di);
        break;
    }
    case 0x1f8: {   /* 3088 */
        unsigned di = OW(c, at, 6);
        MSET(c, di + 2, MB(c, di + 2) | 8); grid_forward(c, di);
        break;
    }
    case 0x200: OPUT(c, at, 8, OW(c, at, 0x78)); break;   /* 2627 */
    case 0x208: {   /* 262e */
        unsigned cx = 0;
        for (int i = 0; i < 3; i++) if (m[TRUCK_ADDRESSES[i] + 2] & 2) cx++;
        OPUT(c, at, 8, OW(c, at, 0x78) - cx);
        break;
    }
    case 0x210: graphics(c, 0x44, 0, 0, 0, 0); graphics(c, 0x48, 0, 0, 0, 0); break;   /* 2661 */
    case 0x218: graphics(c, 0x28, GA_AX, ax, 0, 0); break;   /* 267c */
    case 0x220: graphics(c, 0x2c, 0, 0, 0, 0); break;        /* 268e */
    case 0x228: graphics(c, 0x30, GA_AX, ax, 0, 0); break;   /* 26a0 */
    case 0x230: h230(c, at); break;
    case 0x238: {   /* 2805 month */
        static const Field f = {0xd, 3, 3, 0x12, 0x1a, 0x1e, 7, 0x92b2, 0x92af, 0x92a7, 8, 0xc};
        field(c, at, &f);
        break;
    }
    case 0x240: {   /* 2925 day */
        static const Field f = {0x20, 4, 5, 0x11, 0x11, 0x12, 3, 0x9330, 0x932d, 0x9325, 0xa, 0x1f};
        field(c, at, &f);
        break;
    }
    case 0x248: h248(c, at); break;
    case 0x250: h250(c, at); break;
    case 0x258: h258(c, at); break;
    case 0x260: h260(c, at); break;
    case 0x268: h268(c, at); break;
    case 0x270: h270(c, at); break;
    case 0x278: h278(c); break;
    case 0x280: h280(c); break;
    case 0x288: h288(c, at, ax); break;
    case 0x290: h290(c, at); break;
    case 0x298: OPUT(c, at, 8, MB(c, (long long)OW(c, at, 6) + 1)); break;   /* 2f63 credits */
    case 0x2a0: { unsigned di = OW(c, at, 6); MSET(c, di + 1, MB(c, di + 1) - 1); break; }   /* 2f6f */
    case 0x2a8: awards_driver_out(c->k, m, at); break;   /* 2f76 */
    case 0x2b0: awards_driver_login(m, at); break;       /* 3613 */
    case 0x2b8: h2b8(c); break;
    case 0x2c0: graphics(c, 0x40, GA_AX | GA_SI | GA_BL, rw(m, 0x4aa5), 4, 0); break;   /* 24a9 */
    case 0x2c8: {   /* 24c3 */
        unsigned al = m[0x1ac];
        OPUT(c, at, 8, al); al++;
        m[0x1ac] = u8(al >= 3 ? 0 : al);
        break;
    }
    case 0x2e8: h2e8(c, at); break;
    default: return 0;
    }
#undef ARG
    *pcp = pc;
    return 1;
}
