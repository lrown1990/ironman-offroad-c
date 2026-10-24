/* Keyboard and joystick as the game reads them, on the data segment: a
 * literal translation of tools/controls.py.
 * int9         0434:0698  one byte from port 60 (scan code set 1): key table
 *                         0624..0633, shift/ctrl/alt in 067e, lock keys
 *                         toggled in 067e and held in 067f, presses queued in
 *                         0634..0673 (write 0674, read 0675).
 * key_down     0434:0772  the bit of the key table.
 * keyboard     0434:07b6  map at 5382 (player 1) or 5396 (player 2): entries
 *                         of three bytes (row, mask, command bit) up to a byte
 *                         with bit 7; the map's first byte keeps the result.
 * joystick     0434:0836  680 or 692: axes +0/+2 against centres +4/+6.
 * read_controls 0000:1508 players 0, 1 keyboard; 2 (680), others (692) joystick.
 */
#include "offroad.h"

#define KEYS 0x624
#define MODIFIERS 0x67e
#define LOCKS_HELD 0x67f
#define QUEUE 0x634
#define WRITE 0x674
#define READ 0x675

static unsigned modifier_bit(unsigned code) {
    switch (code) { case 0x36: return 1; case 0x2a: return 2; case 0x1d: return 4; case 0x38: return 8; }
    return 0;
}
static unsigned lock_bit(unsigned code) {
    switch (code) { case 0x46: return 0x10; case 0x45: return 0x20; case 0x3a: return 0x40; case 0x52: return 0x80; }
    return 0;
}

/* E0 and E1..EF/F1..FF are ignored; F0 (a set-2 release prefix, which a set-1
 * keyboard never sends) makes the handler read port 60 again, with bit 7 set */
int ctl_int9(uint8_t *m, const uint8_t *reads, int n) {
    unsigned ah = 0, byte;
    int i = 0;
    for (;;) {
        if (i >= n) return OFF_NOT_MODELLED;   /* F0: the handler reads port 60 again, no byte given */
        byte = reads[i++] | ah;
        if (byte < 0xe0) break;
        if (byte != 0xf0) return OFF_OK;
        ah = 0x80;
    }
    unsigned code = byte & 0x7f, release = byte & 0x80, bit;
    if ((bit = modifier_bit(code))) {
        m[MODIFIERS] = release ? (m[MODIFIERS] & ~bit & 255) : (m[MODIFIERS] | bit);
        return OFF_OK;
    }
    if ((bit = lock_bit(code))) {
        if (release) m[LOCKS_HELD] &= ~bit & 255;
        else { m[MODIFIERS] ^= bit & ~m[LOCKS_HELD] & 255; m[LOCKS_HELD] |= bit; }
        return OFF_OK;
    }
    unsigned at = KEYS + (code >> 3), mask = m[0x676 + (code & 7)];
    if (release) { m[at] &= ~mask & 255; return OFF_OK; }
    m[at] |= mask;
    unsigned write = m[WRITE], read = m[READ];
    if (((read - write) & 31) == 1) return OFF_OK;   /* the queue is full */
    unsigned slot = QUEUE + 2 * (write & 31);
    m[slot] = (uint8_t)byte; m[slot + 1] = m[MODIFIERS]; m[WRITE] = (uint8_t)(write + 1);
    return OFF_OK;
}

unsigned ctl_key_down(const uint8_t *m, unsigned code) {
    return m[0x676 + (code & 7)] & m[KEYS + ((code >> 3) & 31)];
}

int ctl_keyboard(uint8_t *m, unsigned at) {
    unsigned held = 0;
    long long p = (long long)at + 1;
    for (;;) {
        if (p > 65535) return -1;
        if (m[p] & 0x80) break;
        if (p + 1 > 65535) return -1;
        if (m[p + 1] & m[KEYS + m[p]]) {
            if (p + 2 > 65535) return -1;
            held |= m[p + 2];
        }
        p += 3;
    }
    if (at > 65535) return -1;
    unsigned state = held | ((((held ^ m[at]) & held) << 2) & 0xc0);
    m[at] = (uint8_t)state;
    return (int)state;
}

unsigned ctl_joystick(uint8_t *m, unsigned at) {
    if (!m[0x6b0]) { m[at + 0x10] = 0; return 0; }
    unsigned state = m[at + 0x10];
    static const unsigned axes[2][4] = {{0, 4, 2, 1}, {2, 6, 8, 4}};
    for (int i = 0; i < 2; i++) {
        unsigned reading = at + axes[i][0], centre = at + axes[i][1];
        unsigned c = m[centre] | m[(centre + 1) & 65535] << 8, value = m[reading] | m[(reading + 1) & 65535] << 8;
        unsigned half = c >> 1;
        if (half >= value) state |= axes[i][2];
        if (((half + c) & 0xffff) < value) state |= axes[i][3];
    }
    state &= 255;
    state = state | ((((state ^ m[at + 0x11]) & state) << 2) & 0xc0);
    m[at + 0x10] = (uint8_t)state;
    return state;
}

int ctl_read_controls(uint8_t *m, unsigned player) {
    player &= 255;
    if (player == 0) return ctl_keyboard(m, 0x5382);
    if (player == 1) return ctl_keyboard(m, 0x5396);
    return (int)ctl_joystick(m, player == 2 ? 0x680 : 0x692);
}

/* ---- the game port ------------------------------------------------------- */
void joy_init(Joystick *j, const int axes[4], unsigned buttons) {
    for (int i = 0; i < 4; i++) j->axes[i] = axes[i];
    j->buttons = buttons & 0xf0; j->since = -1;
}
unsigned joy_read(Joystick *j) {
    unsigned v = j->buttons;
    for (int i = 0; i < 4; i++)
        if (j->axes[i] < 0 || (j->since >= 0 && j->since < j->axes[i])) v |= 1u << i;
    if (j->since >= 0) j->since++;
    return v;
}
/* 0434:0888: the axes that answer a trigger are timed together: each round
 * reads the port until one of them drops (or CX runs out), writes (the bits
 * that dropped, CX) at 06a4 + 3n and takes 2 from CX; then every record with
 * bits gives -CX-1 to its axes (0680, 0682, 0692, 0694) until a record
 * without bits (the scan goes past 06ad when four records have bits: 06b0 is
 * the joystick flag). Then the buttons (0690/0691, 06a2/06a3: now and
 * before). No axis answering: nothing more (not even the buttons). */
void joystick_read(uint8_t *m, Joystick *j, void (*out)(void *ctx, unsigned port, unsigned value), void *ctx) {
    if (!m[0x6b0]) return;
    j->since = -1;
    unsigned al = joy_read(j), ah = al;
    if (out) out(ctx, 0x201, al);
    j->since = 0;
    al = joy_read(j);
    al = (al ^ ah) & 0x0f;
    if (!al) return;
    ah = al;
    unsigned si = 0x6a4, cx = 0;
    m[0x6a4] = m[0x6a7] = m[0x6aa] = m[0x6ad] = 0;
    if (out) out(ctx, 0x201, al);
    j->since = 0;
    for (;;) {
        for (;;) {   /* 08b8: IN, NOT, AND, LOOPE */
            al = ~joy_read(j) & ah & 255; cx = (cx - 1) & 0xffff;
            if (al || !cx) break;
        }
        m[si] = (uint8_t)al; m[si + 1] = (uint8_t)cx; m[si + 2] = (uint8_t)(cx >> 8);
        si += 3; cx = (cx - 2) & 0xffff;
        ah ^= al;
        if (!ah) break;
    }
    static const unsigned at[4] = {0x680, 0x682, 0x692, 0x694};
    si = 0x6a4;
    for (;;) {
        al = m[si]; si += 1;
        if (!al) break;
        unsigned ax = (-(m[si] | m[si + 1] << 8) - 1) & 0xffff;
        si += 2;
        for (int bit = 0; bit < 4; bit++) if (al >> bit & 1) { m[at[bit]] = (uint8_t)ax; m[at[bit] + 1] = (uint8_t)(ax >> 8); }
    }
    al = ~joy_read(j) & 255; ah = al;
    unsigned now = al & 0x30;
    m[0x691] = m[0x690]; m[0x690] = (uint8_t)now;
    now = (ah >> 2) & 0x30;
    m[0x6a3] = m[0x6a2]; m[0x6a2] = (uint8_t)now;
}
