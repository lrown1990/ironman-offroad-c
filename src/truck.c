/* The trucks: movement, suspension and walls (game 1778..1e7d), the player's
 * parameters, nitro, the contact effects, the truck-to-truck response and the
 * effect script subset. A literal translation of tools/truck_physics.py,
 * tools/truck_interactions.py and select_pose of tools/recover_truck_states.py.
 *
 * A truck is the 0c2-byte record at DS:4c72 + n*0c2, its object the 07a-byte
 * record of the object pool; both are plain byte buffers here (pointers into
 * the data segment when the race runs). The tables are read from the
 * original data segment image (game.exe from 5b40), as the model does.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "offroad.h"

#define u8(n) ((uint8_t)(n))
#define u16(n) ((uint16_t)(n))
static inline int s8(unsigned n) { return (int8_t)(uint8_t)n; }
static inline int s16(unsigned n) { return (int16_t)(uint16_t)n; }
/* Python's >> on negative numbers: a floor */
static inline int64_t sar(int64_t v, int n) { return v >= 0 ? v >> n : ~((~v) >> n); }

static inline unsigned W(const uint8_t *b, unsigned o) { return b[o] | b[o + 1] << 8; }
static inline void put(uint8_t *b, unsigned o, long long v) { b[o] = (uint8_t)v; b[o + 1] = (uint8_t)(v >> 8); }

static const unsigned SUPPORTS[4] = {0x8a, 0x98, 0xa6, 0xb4};
static const unsigned ANGLES[4] = {0xa000, 0x6000, 0xe000, 0x2000};

static void fail(Truck *k, int e) { if (!k->error) k->error = e; }
/* the original tables: past the end of the image the model raises */
static unsigned dw(Truck *k, long long o) {
    if (o < 0 || o + 2 > k->ndata) { fail(k, OFF_BAD_ADDRESS); return 0; }
    return W(k->data, (unsigned)o);
}
static unsigned db(Truck *k, long long o) {
    if (o < 0 || o >= k->ndata) { fail(k, OFF_BAD_ADDRESS); return 0; }
    return k->data[o];
}

/* ---- loading ------------------------------------------------------------- */
static uint8_t *slurp(const char *dir, const char *name, int *n) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(size ? size : 1);
    if (b && fread(b, 1, size, f) != (size_t)size) { free(b); b = 0; }
    fclose(f);
    *n = (int)size;
    return b;
}

Truck *truck_load(const char *dir) {
    Truck *k = calloc(1, sizeof *k);
    int n;
    uint8_t *exe = slurp(dir, "game.exe", &n);
    if (!k || !exe || n <= GAME_DATA_OFFSET) goto bad;
    k->ndata = n - GAME_DATA_OFFSET;
    k->data = malloc(k->ndata);
    memcpy(k->data, exe + GAME_DATA_OFFSET, k->ndata);
    free(exe);
    exe = 0;
    if (!(k->poly = slurp(dir, "poly.bin", &k->npoly))) goto bad;
    if (!(k->quad = slurp(dir, "quad.bin", &k->nquad))) goto bad;
    for (int t = 0; t < 8; t++) {
        char name[16];
        snprintf(name, sizeof name, "contour%d.xx", t);
        uint8_t *c = slurp(dir, name, &n);
        if (!c || n != 44 * 44 * 2) { free(c); goto bad; }
        memcpy(k->contour[t], c, n);
        free(c);
    }
    k->original = k->data; k->noriginal = k->ndata;
    for (int i = 0; i < 256; i++) k->trig[i] = (int16_t)W(k->data, 0x2faa + 2 * i);
    for (int i = 0; i < 32; i++) k->support_trig[i] = (int16_t)W(k->data, 0x50e6 + 2 * i);
    for (int p = 0; p < 25; p++) {
        unsigned base = W(k->data, 0x5126 + p * 4), pointer = W(k->data, 0x5128 + p * 4);
        for (int d = 0; d < 32; d++) k->frames[p][d] = u16(base + k->data[pointer + d]);
    }
    for (int t = 0; t < 8; t++) truck_build_terrain(k, t, k->terrain[t]);
    if (k->error) goto bad;
    return k;
bad:
    free(exe);
    truck_free(k);
    return 0;
}

void truck_free(Truck *k) {
    if (!k) return;
    free(k->original); free(k->poly); free(k->quad); free(k);
}

/* ---- the terrain ------------------------------------------------------- */
static int slope(Truck *k, unsigned ref, unsigned quadrant, unsigned axis) {
    if (ref & 1) {
        unsigned at = (ref & 65534) + quadrant;
        if (at + 2 > (unsigned)k->nquad) { fail(k, OFF_BAD_ADDRESS); return 0; }
        ref = W(k->quad, at);
    }
    if (ref + axis >= (unsigned)k->npoly) { fail(k, OFF_BAD_ADDRESS); return 0; }
    return s8(k->poly[ref + axis]);
}

/* 1e7e: 44x44 entries (surface reference, height byte) */
void truck_build_terrain(Truck *k, int track, uint8_t *out) {
    const uint8_t *c = k->contour[track];
    unsigned words[44 * 44 + 1];   /* the final look-ahead is discarded */
    for (int i = 0; i < 44 * 44; i++) words[i] = W(c, 2 * i);
    words[44 * 44] = 0;
    unsigned height = 0x8000;
    for (int y = 0; y < 44; y++) {
        unsigned row_height = height;
        for (int x = 0; x < 44; x++) {
            int i = y * 44 + x;
            out[i * 3] = u8(words[i]); out[i * 3 + 1] = u8(words[i] >> 8); out[i * 3 + 2] = u8(height >> 8);
            height = u16(height + slope(k, words[i], 2, 2) * 128 + slope(k, words[i + 1], 4, 2) * 128);
        }
        if (y < 43)
            height = u16(row_height - slope(k, words[y * 44], 0, 3) * 128 - slope(k, words[(y + 1) * 44], 6, 3) * 128);
    }
}

/* 190a..1923: record = row pointer (table 6abc) + column*3, no bounds check */
void truck_cell(Truck *k, int track, unsigned x, unsigned y, uint16_t *ref, uint8_t *height) {
    unsigned column = (x >> 8) & 255, row = (y >> 8) & 255;
    if (!k->ds) {
        if (!(column < 44 && row < 44)) { fail(k, OFF_OUTSIDE_GRID); *ref = 0; *height = 0; return; }
        const uint8_t *t = k->terrain[track];
        unsigned i = (row * 44 + column) * 3;
        *ref = u16(W(t, i)); *height = t[i + 2];
        return;
    }
    const uint8_t *ds = k->ds;
    unsigned at = (ds[0x6abc + row * 2] | ds[0x6abd + row * 2] << 8) + column * 3;
    *ref = u16(ds[at & 65535] | ds[(at + 1) & 65535] << 8);
    *height = ds[(at + 2) & 65535];
}

void truck_surface(Truck *k, int track, unsigned x, unsigned y, Surface *s) {
    uint16_t ref; uint8_t h;
    truck_cell(k, track, x, y, &ref, &h);
    unsigned base = (unsigned)h << 8;
    s->quadrant = -1;
    if (ref & 1) {
        s->quadrant = (int16_t)(((x & 255) + (y & 255) >= 256 ? 0 : 4) + ((x & 255) >= (y & 255) ? 2 : 0));
        /* past the end of quad.bin / poly.bin the model reads zeros */
        unsigned at = (ref & 65534) + s->quadrant;
        ref = u16((at < (unsigned)k->nquad ? k->quad[at] : 0) | (at + 1 < (unsigned)k->nquad ? k->quad[at + 1] : 0) << 8);
    }
    uint8_t v[4];
    for (int i = 0; i < 4; i++) v[i] = (unsigned)ref + i < (unsigned)k->npoly ? k->poly[ref + i] : 0;
    s->reference = ref; s->kind = v[0]; s->angle = v[1]; s->sx = v[2]; s->sy = v[3];
    unsigned limit = dw(k, 0x6b14 + track * 2);
    s->blocked = (s->kind & 128) || u8((limit & 255) + s->sx) >= (limit >> 8) || u8((limit & 255) + s->sy) >= (limit >> 8);
    s->height = u16(base + s8(s->sx) * (int)(2 * ((x & 255) >> 1) - 128) + s8(s->sy) * (int)(2 * ((y & 255) >> 1) - 128));
}

/* ---- movement ----------------------------------------------------------- */
/* 1778..179b, after the keyboard or the driver model made the control byte */
void truck_throttle(Truck *k, uint8_t *t, unsigned controls) {
    t[0x74] = u8(controls);
    unsigned v = u16(W(t, 0x75) + (controls & 8 ? 32 : -16));
    put(t, 0x75, v & 32768 ? 0 : (v < 256 ? v : 256));
}

/* 1871..18de: candidate horizontal displacement and four vertical heights */
void truck_integrate(Truck *k, uint8_t *t, TruckStep *g) {
    unsigned angle = u8(t[0x84] + 7) & 248;
    int64_t speed = s16(W(t, 0x85));
    unsigned vy = u16(sar(speed * k->trig[angle] * 4, 16));
    unsigned vx = u16(sar(speed * k->trig[(angle + 64) & 255] * 4, 16));
    put(t, 0x55, vx); put(t, 0x57, vy);
    g->x = u16(W(t, 0x79) + vx); g->y = u16(W(t, 0x7b) + vy);
    for (int i = 0; i < 4; i++) put(t, SUPPORTS[i] + 4, W(t, SUPPORTS[i] + 4) + W(t, SUPPORTS[i] + 6));
}

static void event(Truck *k, TruckStep *g, unsigned kind, unsigned value) {
    if (g->nev >= 8) { fail(k, OFF_OVERFLOW); return; }
    g->ev[g->nev][0] = (uint16_t)kind; g->ev[g->nev][1] = (uint16_t)value; g->nev++;
}

/* 18df..1b95: footprint lookup, contact, gravity, slopes and walls */
void truck_support(Truck *k, uint8_t *t, TruckStep *g, int track, unsigned p, unsigned angular_offset, Surface *s) {
    unsigned angle = (u16(W(t, 0x81) + angular_offset) >> 11) & 31;
    unsigned x = u16(g->x + k->support_trig[(angle + 8) & 31]);
    unsigned y = u16(g->y + k->support_trig[angle]);
    put(t, p, x); put(t, p + 2, y);
    truck_surface(k, track, x, y, s);
    if (s->blocked) { truck_wall(k, t, g, p, s); return; }
    unsigned ground = s->height;
    put(t, p + 8, ground);
    unsigned delta = u16(ground - W(t, p + 4));
    if (delta == 0 || delta & 32768) {
        if (delta) put(t, p + 6, W(t, p + 6) - 256);
        put(t, p + 10, -(long long)delta);
        return;
    }
    unsigned velocity = W(t, p + 6), impulse, out;
    if (!(velocity & 32768)) {
        impulse = delta;
        out = u16(((delta * W(t, 0x63)) >> 16) + velocity);
    } else {
        unsigned threshold = s->kind == 2 ? 0xffa0 : 0xfd80;
        if (velocity < threshold) event(k, g, s->kind == 2 ? EV_SURFACE_IMPACT_2 : EV_SURFACE_IMPACT, p);
        unsigned up = u16(-velocity);
        impulse = delta > up ? delta : up;
        out = (impulse * W(t, 0x63)) >> 16;
    }
    put(t, p + 12, impulse);
    put(t, p + 6, out < 512 ? 0 : (out < 2048 ? out : 2048));
    put(t, p + 10, 0); put(t, p + 4, ground);
    if (s->kind == 1 && W(t, 0x85) > 48) { unsigned v = W(t, 0x85) - 2; put(t, 0x85, v > 48 ? v : 48); }
    if (s->sx || s->sy) {
        unsigned a = u8(t[0x84] - s->angle + 64);
        if (a & 128) {
            a = u8(-a);
            if (a >= 40) a = u8(-a) & 0x70;
            unsigned speed = u16(W(t, 0x85) - (a >> 4) * t[p + 13]);
            put(t, 0x85, speed & 32768 ? 0 : speed);
        }
    }
}

/* 1a68..1b95, with the wall's random numbers; the sound becomes an event */
void truck_wall(Truck *k, uint8_t *t, TruckStep *g, unsigned p, const Surface *s) {
    put(t, p + 4, W(t, 0x7d)); put(t, p + 8, W(t, 0x7f)); put(t, p + 10, W(t, 0x7d) - W(t, 0x7f));
    unsigned angle = s->angle;
    if (g->flags) {
        unsigned a = u8(t[0x84] - angle);
        if (u8(a + 64) & 128) { t[0x84] = u8(t[0x84] + (a >> 1)); g->flags = 0x83; }
        return;
    }
    unsigned a = u8((t[0x82] ^ t[0x89]) - angle - 64);
    if (a & 128) { put(t, 0x83, W(t, 0x81) ^ (t[0x89] << 8)); return; }
    a = u8(a - 56);
    if (a & 128) {
        put(t, 0x81, W(t, 0x81) - 4096);
        put(t, 0x83, (u8(angle + 64) ^ t[0x89]) << 8);
    } else if (a < 16) {
        put(t, 0x85, (160 * W(t, 0x6f)) >> 8); put(t, 0x75, 0); t[0x77] = 0;
        g->rng = u16((u16(g->rng + 1) * 171u) % 0x7673);
        unsigned random_value = u16(g->rng - 1);
        t[0x82] = u8(t[0x82] + (random_value & 32 ? 32 : -32));
        t[0x89] ^= 128; t[0x84] = t[0x82] ^ t[0x89];
        g->flags = 3; event(k, g, EV_WALL_SOUND, 8);
        return;
    } else {
        put(t, 0x81, W(t, 0x81) + 4096);
        put(t, 0x83, (u8(angle + 192) ^ t[0x89]) << 8);
    }
    g->flags = u8(db(k, 0x5252 + (angle >> 4)));
    unsigned throttle = W(t, 0x75);
    if (throttle > 64) { unsigned v = throttle - (throttle >> 2); put(t, 0x75, v > 64 ? v : 64); }
    unsigned top = W(t, 0x6f);
    if (t[0x77]) { unsigned v = u16(top + (top >> 1)); top = v < 240 ? v : 240; }
    put(t, 0x85, (top * W(t, 0x75)) >> 8);
    event(k, g, EV_WALL_SOUND, 8);
}

/* 1c8b..1d72: contact-dependent steering, slip, speed target */
void truck_traction(Truck *k, uint8_t *t) {
    unsigned mask = 0;
    for (int i = 0; i < 4; i++) mask = ((mask << 1) | (W(t, 0x65) < W(t, SUPPORTS[i] + 10))) & 255;
    mask ^= 255;
    unsigned difference = u16(W(t, 0x81) - W(t, 0x83)), high = difference >> 8;
    unsigned reverse = u8(high + 64) & 128;
    t[0x89] = u8(reverse);
    unsigned correction_byte = u8(reverse + high);
    if (mask & 3) {
        unsigned steer = W(t, 0x67);
        if (t[0x74] & 1 || t[0x74] & 2) {
            if (!(t[0x74] & 1)) steer = u16(-steer);
            if ((mask & 3) != 3) steer = u16(s16(steer) >> 1);
            put(t, 0x81, W(t, 0x81) + steer);
        }
        if (mask & 12) {
            unsigned correction = u16(s8(correction_byte) * s16(W(t, 0x6b)));
            if ((mask & 12) == 12) correction = u16(correction << 1);
            put(t, 0x83, W(t, 0x83) + correction);
        }
    }
    mask &= 12;
    if (!mask) return;
    unsigned current = W(t, 0x85), top = W(t, 0x6f);
    if (t[0x77]) { unsigned v = u16(top + (top >> 1)); top = v < 240 ? v : 240; }
    unsigned target = ((top * W(t, 0x75)) >> 8) & 65535;
    if (reverse == 128) target = 0;
    else if (current <= target) {
        if (current < target) {
            unsigned accel = W(t, 0x71);
            if (mask != 12) accel = u16(s16(accel) >> 1);
            unsigned v = u16(current + accel);
            current = target < v ? target : v;
        }
        put(t, 0x85, current);
        return;
    }
    unsigned decel = W(t, 0x6d);
    if (mask != 12) decel = u16(s16(decel) >> 1);
    if (current <= decel) { put(t, 0x83, W(t, 0x81)); put(t, 0x85, 0); put(t, 0x75, 0); t[0x89] = 0; }
    else { unsigned v = u16(current - decel); put(t, 0x85, target > v ? target : v); }
}

/* 1d73..1e7d: ADD AH,10 sets SF from the wrapped byte, not the word */
static unsigned quantize(unsigned negative_difference) {
    unsigned high = (((negative_difference & 0xffff) >> 8) + 10) & 255;
    if (high & 128) high = 0;
    return (high < 16 ? high : 16) >> 2;
}

void truck_select_pose(Truck *k, const uint16_t heights[4], const uint16_t ground[4], unsigned heading, Pose *r) {
    unsigned h[4], g[4];
    for (int i = 0; i < 4; i++) { h[i] = heights[i]; g[i] = ground[i]; }
    unsigned diagonal_ad = (h[0] >> 1) + (h[3] >> 1), diagonal_bc = (h[1] >> 1) + (h[2] >> 1);
    unsigned delta = diagonal_ad > diagonal_bc ? diagonal_ad - diagonal_bc : diagonal_bc - diagonal_ad;
    if (delta >= 0x100) {
        int i0 = diagonal_ad >= diagonal_bc ? 1 : 0, i1 = diagonal_ad >= diagonal_bc ? 2 : 3;
        h[i0] = (h[i0] + (delta >> 1)) & 0xffff;
        h[i1] = (h[i1] + (delta >> 1)) & 0xffff;
    }
    r->average_height = u16((diagonal_ad >> 1) + (diagonal_bc >> 1));
    r->average_ground = u16((((g[0] >> 1) + (g[1] >> 1)) >> 1) + (((g[2] >> 1) + (g[3] >> 1)) >> 1));
    int a = h[0] >> 1, b = h[1] >> 1, c = h[2] >> 1, d = h[3] >> 1;
    r->axis_a = u8(quantize(u16(-(int)u16(a + b - c - d))));
    r->axis_b = u8(quantize(u16(-(int)u16(a + c - b - d))));
    r->direction = u8(u8(-(int)(heading & 255)) >> 3);
    r->frame = k->frames[r->axis_a * 5 + r->axis_b][r->direction];
    r->shadow_frame = u16(412 + r->direction);
    /* EGA 57c7: SUB AL, [BX+80]; CBW. The shadow has its own direction. */
    r->render_lift = (int16_t)s8(((r->average_height >> 8) - (r->average_ground >> 8)) & 255);
    for (int i = 0; i < 4; i++) r->heights[i] = u16(h[i]);
}

void truck_pose(Truck *k, uint8_t *t, Pose *r) {
    uint16_t h[4], g[4];
    for (int i = 0; i < 4; i++) { h[i] = u16(W(t, SUPPORTS[i] + 4)); g[i] = u16(W(t, SUPPORTS[i] + 8)); }
    truck_select_pose(k, h, g, t[0x82], r);
    for (int i = 0; i < 4; i++) put(t, SUPPORTS[i] + 4, r->heights[i]);
    put(t, 0x7d, r->average_height); put(t, 0x7f, r->average_ground); put(t, 0x87, r->frame);
}

void truck_project(Truck *k, int track, unsigned x, unsigned y, unsigned z, uint16_t *sx, uint16_t *sy) {
    int elevation = -s8(u8((z >> 8) - 128));
    *sy = u16(elevation + sar((int64_t)s16(y) * 0x471, 16) + dw(k, 0x53fc + track * 2));
    unsigned q = (u16(-(long long)(y - 0x2c00)) * 0x5555u) >> 16;
    *sx = u16(sar(s16(x), 5) + sar(s16(q), 5) + dw(k, 0x53ec + track * 2));
}

void truck_commit(Truck *k, uint8_t *t, uint8_t *obj, const TruckStep *g, int track) {
    unsigned flags = W(obj, 0x66) & 0xffef;
    if (g->flags & 1) flags |= 16; else { put(t, 0x79, g->x); put(obj, 0x22, g->x >> 5); }
    if (g->flags & 2) flags |= 16; else { put(t, 0x7b, g->y); put(obj, 0x26, g->y >> 5); }
    put(obj, 0x66, flags);
    uint16_t sx, sy;
    truck_project(k, track, W(t, 0x79), W(t, 0x7b), W(t, 0x7f), &sx, &sy);
    put(obj, 0x2a, sx - 15); put(obj, 0x2e, sy - 14);
    flags = W(obj, 0xa) | 3;
    unsigned delta = u16(W(t, 0x7d) - W(t, 0x7f));
    if (!(delta & 32768) && delta >= (db(k, 0x524a + track) << 8)) flags &= 65534;
    put(obj, 0xa, flags); put(obj, 0x18, W(obj, 0x26));
}

void truck_step(Truck *k, uint8_t *t, uint8_t *obj, int track, unsigned rng, int controls,
                TruckStep *g, Pose *pose, Surface surfaces[4]) {
    if (controls >= 0) truck_throttle(k, t, (unsigned)controls);
    memset(g, 0, sizeof *g);
    g->rng = u16(rng);
    truck_integrate(k, t, g);
    Surface local[4];
    if (!surfaces) surfaces = local;
    for (int i = 0; i < 4; i++) truck_support(k, t, g, track, SUPPORTS[i], ANGLES[i], &surfaces[i]);
    truck_traction(k, t);
    truck_pose(k, t, pose);
    truck_commit(k, t, obj, g, track);
}

/* 14c7: the computer's parameter table, independent of its input */
void truck_demo_parameters(Truck *k, uint8_t *t, int level) {
    static const unsigned table[8][2] = {{0x63, 0x4fea}, {0x6d, 0x500e}, {0x67, 0x5032}, {0x69, 0x5032},
                                         {0x6b, 0x5056}, {0x65, 0x507a}, {0x71, 0x509e}, {0x6f, 0x50c2}};
    if (level < 0 || level >= 18) { fail(k, OFF_NOT_MODELLED); return; }
    t[0x5d] = 10;
    for (int i = 0; i < 8; i++) put(t, table[i][0], dw(k, table[i][1] + 2 * level));
}

void truck_initial_state(Truck *k, int track, int level, uint8_t *t, uint8_t *obj) {
    static const unsigned table[6][2] = {{0x79, 0x5262}, {0x7b, 0x5282}, {0x7d, 0x52a2},
                                         {0x7f, 0x52a2}, {0x81, 0x52c2}, {0x83, 0x52c2}};
    memset(t, 0, TRUCK_SIZE); memset(obj, 0, OBJECT_SIZE);
    t[0x5b] = u8(level);
    truck_demo_parameters(k, t, level);
    for (int i = 0; i < 6; i++) put(t, table[i][0], dw(k, table[i][1] + 2 * track));
    for (int i = 0; i < 4; i++) { put(t, SUPPORTS[i] + 4, W(t, 0x7d)); put(t, SUPPORTS[i] + 8, W(t, 0x7f)); }
    put(obj, 0x22, W(t, 0x79) >> 5); put(obj, 0x26, W(t, 0x7b) >> 5);
}

/* ---- interactions -------------------------------------------------------- */
/* 147c..14c6 stores the incoming DX at +71, not the table's AX, unless the
 * one-byte fix at 14c4 is asked for (tools/verify_acceleration_fix.py). */
unsigned truck_human_parameters(Truck *k, uint8_t *t, unsigned dx, int acceleration_fix) {
    static const unsigned table[7][3] = {{0x63, 0x4f86, 0x5e}, {0x6d, 0x4f9e, 0x5f}, {0x67, 0x4f92, 0x5f},
                                         {0x69, 0x4f92, 0x5f}, {0x6b, 0x4faa, 0x5f}, {0x65, 0x4fb6, 0x5f},
                                         {0x6f, 0x4fce, 0x61}};
    for (int i = 0; i < 7; i++) put(t, table[i][0], dw(k, table[i][1] + u8(t[table[i][2]] * 2)));
    unsigned ax = dw(k, 0x4fc2 + u8(t[0x60] * 2));
    put(t, 0x71, acceleration_fix ? ax : dx);
    return ax;
}

void truck_start_coordinates(Truck *k, int track, int slot, uint16_t *x, uint16_t *y) {
    unsigned heading = dw(k, 0x52c2 + track * 2);
    long long base = 0x52e2 + (heading & 32768 ? 16 : 0) + slot * 4;
    *x = u16(dw(k, 0x5262 + track * 2) + dw(k, base));
    *y = u16(dw(k, 0x5282 + track * 2) + dw(k, base + 2));
}

/* 17a1..1870: the effect and its sound are given back, the allocation is the caller's */
int truck_nitro(Truck *k, uint8_t *t, int track, Spawn *e) {
    if (t[0x77]) {
        t[0x77] = u8(t[0x77] - 1);
        if (t[0x77] >= 12) return 0;
    }
    if (!(t[0x74] & 64) || !t[0x5d]) return 0;
    t[0x5d] -= 1;
    unsigned offset = 0x53aa + (((u8(t[0x82] + 32) >> 5) & 7) * 4);
    e->x = u16(W(t, 0x79) + dw(k, offset)); e->y = u16(W(t, 0x7b) + dw(k, offset + 2));
    truck_project(k, track, e->x, e->y, W(t, 0x7d), &e->screen_x, &e->screen_y);
    t[0x77] = 20;
    if (t[0x89]) { put(t, 0x75, 0); put(t, 0x85, 0); t[0x89] = 0; put(t, 0x83, W(t, 0x81)); }
    else {
        unsigned speed = W(t, 0x85);
        put(t, 0x85, speed + (speed >> 1));
        put(t, 0x75, W(t, 0x75) > 64 ? W(t, 0x75) : 64);
    }
    e->template_ = 0x3362; e->sound = 10; e->flags_or = 64;
    return 1;
}

/* 1b96 / 1bee: the cap of 16 effects and the direction of a splash */
int truck_impact_effect(Truck *k, uint8_t *t, int track, unsigned support, int kind, int count, Spawn *e) {
    if (count >= 16) return 0;
    long long dx, dy;
    if (kind == 2) {
        unsigned variant = (support == 0xa6 || support == 0x8a) ? 0 : 1;
        unsigned angle = u8(t[0x82] + 32) & 192;
        if (!(angle & 64)) variant |= 2;
        if (angle & 128) variant ^= 1;
        unsigned base = 0x53ca + variant * 8;
        e->template_ = u16(dw(k, dw(k, base))); dx = dw(k, base + 2); dy = dw(k, base + 4);
        e->sound = 7;
    } else { e->template_ = 0x332a; dx = -256; dy = -128; e->sound = -1; }
    e->x = u16(W(t, support) + dx); e->y = u16(W(t, support + 2) + dy);
    truck_project(k, track, e->x, e->y, W(t, support + 8), &e->screen_x, &e->screen_y);
    e->flags_or = 0;
    return 1;
}

/* 0f3e: four integer Newton steps and a binary search of the sine table */
void truck_vector_polar(Truck *k, unsigned x, unsigned y, uint16_t *angle_out, uint16_t *speed_out) {
    x = u16(x); y = u16(y);
    int64_t a = s16(x) < 0 ? -s16(x) : s16(x), b = s16(y) < 0 ? -s16(y) : s16(y);
    int64_t r = u16(a + b);
    if (!r) { *angle_out = u16(x); *speed_out = u16(y); return; }
    r = (r >> 1) + 16;
    int64_t n = a * a + b * b;
    for (int i = 0; i < 4; i++) {
        int64_t q = n / r;
        if (q > 65535) { fail(k, OFF_DIVIDE); return; }
        r = (r + q) >> 1;
        if (r == q) break;
    }
    int64_t numerator = (int64_t)s16(y) * 16384;
    int64_t q = ((numerator < 0 ? -numerator : numerator) / r) * (numerator < 0 ? -1 : 1);
    if (q < -32768 || q > 32767) { fail(k, OFF_DIVIDE); return; }
    long long angle = s16(x) >= 0 ? 64 : 192, delta = s16(x) >= 0 ? 32 : -32;
    if (q < 0) angle ^= 384;
    for (int i = 0; i < 5; i++) {
        unsigned value = dw(k, 0x2faa + angle);
        if (u16(q) == value) break;
        angle = u16(angle + (u16(q) > value ? delta : -delta));
        delta = sar(delta, 1);
    }
    *angle_out = u16(s16(angle) >> 1);
    *speed_out = u16(r);
}

/* 169d..1768: the contact mask already made; a contact that goes on is
 * suppressed. Both trucks change, the other object's flags are cleared. */
int truck_collide(Truck *k, uint8_t *trucks[4], uint8_t *objects[4], int index, uint8_t *sounds) {
    uint8_t *t = trucks[index], *obj = objects[index];
    unsigned pending = W(obj, 0x6e) & W(obj, 0x6c);
    int n = 0;
    while (pending) {
        int other = 3;
        for (int i = 0; i < 3; i++) if (pending & (1u << i)) { other = i; break; }
        unsigned bit = other < 3 ? 1u << other : pending;
        put(objects[other], 0x6e, W(objects[other], 0x6e) & u16(~W(obj, 0x66)));
        pending &= u16(~bit);
        if (bit & W(t, 0x59)) continue;
        sounds[n++] = 0;
        uint8_t *b = trucks[other];
        unsigned dx = u16(W(b, 0x55) - W(t, 0x55)); dx = u16(dx - (s16(dx) >> 2));
        unsigned dy = u16(W(b, 0x57) - W(t, 0x57)); dy = u16(dy - (s16(dy) >> 2));
        uint8_t *who[2] = {t, b};
        unsigned vx[2] = {W(t, 0x55) + dx, W(b, 0x55) - dx}, vy[2] = {W(t, 0x57) + dy, W(b, 0x57) - dy};
        for (int j = 0; j < 2; j++) {
            uint16_t angle, speed;
            truck_vector_polar(k, vx[j], vy[j], &angle, &speed);
            if (k->error) return n;
            who[j][0x84] = u8(angle); put(who[j], 0x85, speed);
            put(who[j], 0x75, W(who[j], 0x75) > 32 ? W(who[j], 0x75) : 32);
        }
    }
    return n;
}

/* d97 + e25: the spatial bit grids, as sets of cells: two objects touch when
 * their X cells and their Y cells meet. A range is a run of multiples of 4,
 * so on the circle of 16384 cells two runs meet when one starts inside the other. */
static int runs_meet(unsigned s1, unsigned c1, unsigned s2, unsigned c2) {
    if (!c1 || !c2) return 0;
    unsigned d12 = ((s2 >> 2) - (s1 >> 2)) & 16383, d21 = ((s1 >> 2) - (s2 >> 2)) & 16383;
    return d12 < c1 || d21 < c2;
}
void truck_collision_masks(Truck *k, uint8_t **objects, int n, uint16_t *flags) {
    unsigned (*start)[2] = malloc(sizeof(unsigned[2]) * (n ? n : 1)), (*count)[2] = malloc(sizeof(unsigned[2]) * (n ? n : 1));
    for (int i = 0; i < n; i++) {
        unsigned at = W(objects[i], 0x6a);
        for (int axis = 0; axis < 2; axis++) {
            start[i][axis] = u16(W(objects[i], axis ? 0x26 : 0x22) + dw(k, at + axis * 4)) & 65532;
            count[i][axis] = dw(k, at + axis * 4 + 2);
        }
    }
    for (int i = 0; i < n; i++) {
        unsigned f = 0;
        if (W(objects[i], 0x6c))
            for (int j = 0; j < n; j++)
                if (W(objects[j], 0x66) && runs_meet(start[i][0], count[i][0], start[j][0], count[j][0]) &&
                    runs_meet(start[i][1], count[i][1], start[j][1], count[j][1]))
                    f |= W(objects[j], 0x66);
        flags[i] = u16(f);
    }
    free(start); free(count);
}

/* ---- the effect scripts (EffectVM) -------------------------------------- */
/* One tick: the scheduler's script and event handling, then the animation
 * countdown. A frame's duration byte means countdown+1 scheduler passes. */
void vm_create(Truck *k, unsigned template_, uint8_t *o) {
    memset(o, 0, OBJECT_SIZE);
    put(o, 0x5e, 0x8000); put(o, 0x1a, 0x8000); put(o, 0x64, 0x8000); put(o, 0x12, 0x8000);
    put(o, 4, 0x5858);
    long long pc = template_;
    for (;;) {
        unsigned op = dw(k, pc);
        if (k->error || op == 0x7a) break;
        unsigned at = op & 0x3fff;
        if ((op & 0xc000) == 0x4000) {
            unsigned v = dw(k, pc + 2);
            if (at + 2 > OBJECT_SIZE) { fail(k, OFF_BAD_ADDRESS); return; }
            put(o, at, v); pc += 4;
        } else if (!(op & 0xc000)) {
            unsigned v = db(k, pc + 2);
            if (at >= OBJECT_SIZE) { fail(k, OFF_BAD_ADDRESS); return; }
            o[at] = u8(v); pc += 3;
        } else { fail(k, OFF_NOT_MODELLED); return; }
    }
    put(o, 0x3c, pc + 2); put(o, 0x3e, 0x3000);
}

void vm_position(uint8_t *o, const Spawn *e) {
    put(o, 0x22, e->x >> 5); put(o, 0x26, e->y >> 5); put(o, 0x18, e->y >> 5);
    put(o, 0x2a, e->screen_x); put(o, 0x2e, e->screen_y);
}

void vm_animation(Truck *k, uint8_t *o) {
    unsigned timer = u16(W(o, 0x5e) - 1);
    if (!(timer & 32768)) { put(o, 0x5e, timer); return; }
    unsigned pc = u16(W(o, 0x56) + W(o, 0x58)), word = dw(k, pc), offset = u16(W(o, 0x58) + 2), ev = 4;
    if (word & 32768) {
        if (word == 0xffff) { pc = W(o, 0x56); word = dw(k, pc); offset = 2; ev = 8; }
        else if (word == 0xfffe) { pc = u16(pc + dw(k, pc + 2)); put(o, 0x56, pc); word = dw(k, pc); offset = 2; ev = 12; }
        else if (word == 0xfffd) { put(o, 0x60, 16); put(o, 0x5e, 0x8000); return; }
        else { fail(k, OFF_NOT_MODELLED); return; }
    }
    put(o, 0x58, offset); put(o, 0x5c, word & 255); put(o, 0x5e, word >> 8); put(o, 0x60, ev);
}

int vm_tick(Truck *k, uint8_t *o, unsigned contacts) {
    if (!W(o, 4)) return 0;
    put(o, 0x6e, contacts);
    unsigned pc = W(o, 0x3c);
    if (contacts & W(o, 0x6c)) { put(o, 0x72, pc); pc = W(o, 0x70); }
    else if (W(o, 0x60) >= W(o, 0x64)) { pc = W(o, 0x62); put(o, 0x60, 0); }
    int i;
    for (i = 0; i < 100; i++) {
        unsigned op = dw(k, pc), ax = dw(k, pc + 2), size = (op & 7) + 1, handler = op & 0x7ff8;
        if (k->error) return 0;
        switch (handler) {
        case 0x100: put(o, 0x5c, op & 7 ? ax : 0); break;
        case 0xc8: put(o, 0x66, ax); put(o, 0x6a, dw(k, pc + 4)); break;
        case 0x170: put(o, 0xa, W(o, 0xa) | ax); break;
        case 0x178: put(o, 0xa, W(o, 0xa) & u16(~ax)); break;
        case 0xd0:
            put(o, 0x6c, op & 7 ? ax : 0);
            if (op & 7) put(o, 0x70, pc + dw(k, pc + 4));
            break;
        case 0x10: put(o, 0x46, ax); put(o, 0x4a, pc + 2); break;
        case 0x18: {
            unsigned old = W(o, 0x46);
            if (old) put(o, 0x46, old - 1);
            if (old != 1) pc = W(o, 0x4a);
            break;
        }
        case 0x150: break;
        case 0x158: put(o, 4, 0); break;
        case 0x108:
            put(o, 0x64, op & 7 ? ax : 0x8000);
            if (op & 7) put(o, 0x62, pc + dw(k, pc + 4));
            break;
        case 0xf8: {
            unsigned seq = u16(pc + ax);
            put(o, 0x56, seq);
            unsigned base = dw(k, pc + 4);
            if (!(base & 32768)) put(o, 0x5a, base);
            unsigned word = dw(k, seq);
            put(o, 0x58, 2); put(o, 0x5c, word & 255); put(o, 0x5e, word >> 8);
            break;
        }
        case 0x30: pc = u16(pc + ax); break;
        case 0x2d0: k->impact_count = u8(k->impact_count + 1); break;
        case 0x2d8: k->impact_count = u8(k->impact_count - 1); break;
        default: fail(k, OFF_NOT_MODELLED); return 0;
        }
        if (k->error) return 0;
        pc = u16(pc + size * 2);
        if (op & 32768) break;
    }
    if (i == 100) { fail(k, OFF_HANG); return 0; }
    put(o, 0x3c, pc);
    if (W(o, 4) && !(W(o, 0x5e) & 32768)) vm_animation(k, o);
    return W(o, 4) != 0;
}
