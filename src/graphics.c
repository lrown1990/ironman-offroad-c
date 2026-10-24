/* The graphics module ega.exe: a literal translation of the model
 * tools/graphics.py of OFFROAD CODE OPUS (verified pixel for pixel against
 * the original module and the real game in /vga, /ega16, /ega64);
 * tests/check_graphics.py compares the two.
 *
 * The game calls 33 entries through the far pointers at [cb4e]:0000
 * (table offset = 4 x slot). The module keeps four 8 KB pages in the 64 KB
 * planes of mode 0Dh: cs:[0] the page on screen, cs:[2] the page being
 * drawn, cs:[4] the clean background, cs:[6] a scratch page; cs:[8] the 200
 * row offsets (40 bytes a row). Each update the game restores the
 * rectangles drawn two updates before (64h), draws the object list (60h,
 * recording the new rectangles) and asks for a flip (18h): the timer
 * interrupt then swaps cs:[0] and cs:[2] and moves the start address
 * (gfx_flip). Writes go to the planes as the VGA does them, with the latch
 * always loaded from the address written, as every routine does.
 */
#include <string.h>
#include "offroad.h"

#define WINDOW 0xa0000u

/* ---- memory ------------------------------------------------------------ */
static unsigned mb(Gfx *g, uint32_t a) {
    if (a >= GFX_MEMORY) { if (!g->error) g->error = OFF_BAD_ADDRESS; return 0; }
    return g->mem[a];
}
static void set_mb(Gfx *g, uint32_t a, unsigned v) {
    if (a >= GFX_MEMORY) { if (!g->error) g->error = OFF_BAD_ADDRESS; return; }
    g->mem[a] = (uint8_t)v;
}
static unsigned mw(Gfx *g, uint32_t a) { return mb(g, a) | mb(g, a + 1) << 8; }
static void set_mw(Gfx *g, uint32_t a, unsigned v) { set_mb(g, a, v & 255); set_mb(g, a + 1, (v >> 8) & 255); }

static unsigned cb(Gfx *g, unsigned o) { return mb(g, g->cs + (o & 0xffff)); }
static unsigned cw(Gfx *g, unsigned o) { return mw(g, g->cs + (o & 0xffff)); }
static void set_cb(Gfx *g, unsigned o, unsigned v) { set_mb(g, g->cs + (o & 0xffff), v); }
static void set_cw(Gfx *g, unsigned o, unsigned v) { set_mw(g, g->cs + (o & 0xffff), v); }
static unsigned page(Gfx *g, int n) { return cw(g, 2 * n); }
static unsigned row(Gfx *g, unsigned y) { return cw(g, 8 + 2 * y); }
static unsigned far_byte(Gfx *g, unsigned seg, unsigned off) { return mb(g, seg * 16 + (off & 0xffff)); }
static unsigned obj(Gfx *g, unsigned ds, unsigned si, unsigned o) { return mw(g, ds * 16 + ((si + o) & 0xffff)); }
static unsigned objb(Gfx *g, unsigned ds, unsigned si, unsigned o) { return mb(g, ds * 16 + ((si + o) & 0xffff)); }
static void set_obj(Gfx *g, unsigned ds, unsigned si, unsigned o, unsigned v) { set_mw(g, ds * 16 + ((si + o) & 0xffff), v); }
static int s8(unsigned v) { return v & 0x80 ? (int)v - 256 : (int)v; }

/* ---- planes ---------------------------------------------------------------- */
/* The offset in the planes, or -1 (and the error) outside the window. */
static long long vaddr(Gfx *g, unsigned seg, unsigned off) {
    uint32_t a = seg * 16 + (off & 0xffff);
    if (a < WINDOW || a >= WINDOW + 0x10000) { if (!g->error) g->error = OFF_NOT_MODELLED; return -1; }
    return (long long)(a - WINDOW);
}

/* Write mode 2, replace: the masked pixels take the colour. */
static void put(Gfx *g, unsigned seg, unsigned off, unsigned colour, unsigned mask) {
    if (!mask || g->error) return;
    long long a = vaddr(g, seg, off);
    if (a < 0) return;
    unsigned keep = ~mask & 255;
    for (int p = 0; p < 4; p++)
        g->planes[p][a] = (uint8_t)((g->planes[p][a] & keep) | (colour >> p & 1 ? mask : 0));
}

/* Write mode 0 with the latch from the same address: data rotated right,
 * logical operation (0 replace, 1 AND, 2 OR, 3 XOR) with the latch, bit
 * mask, on the planes of the map mask. */
static void mode0(Gfx *g, unsigned seg, unsigned off, unsigned data, unsigned rotate, int op,
                  unsigned mask, unsigned planes) {
    if (g->error) return;
    long long a = vaddr(g, seg, off);
    if (a < 0) return;
    data &= 255;
    unsigned v = ((data >> rotate) | (data << (8 - rotate))) & 255;
    for (int p = 0; p < 4; p++) {
        if (!(planes >> p & 1)) continue;
        unsigned old = g->planes[p][a], r;
        if (op == 0) r = v; else if (op == 1) r = v & old; else if (op == 2) r = v | old; else r = v ^ old;
        g->planes[p][a] = (uint8_t)((r & mask) | (old & ~mask & 255));
    }
}

/* Write mode 1: the latch of the source to the destination. */
static void copy(Gfx *g, unsigned ss, unsigned so, unsigned ds, unsigned dof) {
    if (g->error) return;
    long long s = vaddr(g, ss, so);
    if (s < 0) return;
    long long d = vaddr(g, ds, dof);
    if (d < 0) return;
    for (int p = 0; p < 4; p++) g->planes[p][d] = g->planes[p][s];
}

static void copy_page(Gfx *g, int src, int dst) {
    long long s = vaddr(g, page(g, src), 0);
    if (s < 0) return;
    long long d = vaddr(g, page(g, dst), 0);
    if (d < 0) return;
    for (int p = 0; p < 4; p++)
        for (int i = 0; i < 8000; i++) g->planes[p][(d + i) & 0xffff] = g->planes[p][(s + i) & 0xffff];
}

/* ---- display ---------------------------------------------------------------- */
/* 09ee, in the timer interrupt: a flip asked for swaps the pages and moves
 * the start address to the new page on screen. */
void gfx_flip(Gfx *g) {
    if (!cb(g, 0x1ff)) return;
    set_cb(g, 0x1ff, 0);
    unsigned shown = page(g, 1);
    set_cw(g, 2, page(g, 0)); set_cw(g, 0, shown);
    g->start = (uint16_t)((shown - 0xa000) * 16);
}

static const uint8_t MODE_0D_ATTRIBUTES[0x15] = {0, 1, 2, 3, 4, 5, 6, 7, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 9, 0, 15, 0, 0};

static void mode_0d_dac(uint8_t *dac) {
    static const uint8_t dim[8][3] = {{0,0,0},{0,0,42},{0,42,0},{0,42,42},{42,0,0},{42,0,42},{42,21,0},{42,42,42}};
    static const uint8_t bright[8][3] = {{21,21,21},{21,21,63},{21,63,21},{21,63,63},{63,21,21},{63,21,63},{63,63,21},{63,63,63}};
    memset(dac, 0, 768);
    for (int i = 0; i < 64; i++) memcpy(dac + 3 * i, (i & 0x10 ? bright : dim)[i & 7], 3);
}

/* INT 10h AH=0: mode 0Dh clears the planes and the start address and loads
 * the BIOS's DAC; the attribute registers are set by the module right after. */
static void set_mode(Gfx *g, unsigned mode) {
    g->mode = (uint8_t)mode;
    if (mode == 0x0d) {
        memset(g->planes, 0, sizeof g->planes);
        g->start = 0; memcpy(g->attr, MODE_0D_ATTRIBUTES, 0x15); g->page_number = 0; mode_0d_dac(g->dac);
    }
}

/* 0715: INT 10h 1002h with the 17 bytes at 020e, 1013h colour paging. */
static void attribute_defaults(Gfx *g) {
    for (int i = 0; i < 16; i++) g->attr[i] = (uint8_t)cb(g, 0x20e + i);
    g->attr[0x11] = (uint8_t)cb(g, 0x20e + 16);
    g->attr[0x10] &= 0x7f;
    g->attr[0x14] = 0;
}

/* ---- palettes ------------------------------------------------------------ */
static void load_vga(Gfx *g, unsigned si, unsigned cl) {         /* 09a0 */
    for (int k = 0; k < 17; k++)
        for (int i = 0; i < 3; i++) {
            unsigned c = cb(g, si + 3 * k + i);
            if (cl != 0xff) c = (c * cl) >> 8;
            set_cb(g, 0x198 + 3 * k + i, c);
        }
    for (int i = 0; i < 0x33; i++) g->dac[i % 768] = (uint8_t)(cb(g, 0x198 + i) & 0x3f);
}

/* 0949: an EGA colour byte (rgbRGB) scaled by CL. */
static unsigned ega_level(unsigned v, unsigned cl) {             /* 0981 */
    v = ((v & 1) << 1) | (v >> 3);
    unsigned h = (v * cl) >> 8;
    return ((h & 1) << 3) | (h >> 1);
}
static unsigned ega_scaled(unsigned al, unsigned cl) {
    if (cl == 0xff) return al;
    unsigned red = ega_level((al & 0x24) >> 2, cl), green = ega_level((al & 0x12) >> 1, cl), blue = ega_level(al & 0x09, cl);
    return (blue | (green << 1) | (red << 2)) & 255;
}

static void load_ega(Gfx *g, unsigned si, unsigned cl) {         /* 08f3 */
    for (int k = 0; k < 17; k++) set_cb(g, 0x198 + k, ega_scaled(cb(g, si + k), cl));
    for (int i = 0; i < 16; i++) g->attr[i] = (uint8_t)cb(g, 0x198 + i);
    g->attr[0x11] = (uint8_t)cb(g, 0x198 + 16);
}

static void load_palette(Gfx *g, unsigned si, unsigned cl) {
    unsigned loader = cw(g, 0x206);
    if (loader == 0x9a0) load_vga(g, si, cl);
    else if (loader == 0x8f3) load_ega(g, si, cl);
    else if (!g->error) g->error = OFF_NOT_MODELLED;
}

static void palette(Gfx *g, unsigned n) {
    load_palette(g, cw(g, (cw(g, 0x202) + 2 * (n & 255)) & 0xffff), 0xff);
}

/* ---- fills, pictures --------------------------------------------------------- */
static void fill(Gfx *g, int n, unsigned ah) {
    long long a = vaddr(g, page(g, n), 0);
    if (a < 0) return;
    for (int p = 0; p < 4; p++)
        for (int i = 0; i < 8000; i++) g->planes[p][(a + i) & 0xffff] = (uint8_t)(ah >> p & 1 ? 0xff : 0);
}

/* Where decode_plane writes: a page of a plane, or the caller's stack. */
typedef struct { int kind; int plane; unsigned page; uint32_t stack; } Target;
static void target_write(Gfx *g, const Target *t, unsigned off, unsigned v) {
    if (t->kind == 0) {
        long long a = vaddr(g, t->page, off);
        if (a >= 0) g->planes[t->plane][a] = (uint8_t)v;
    } else set_mb(g, t->stack + (off & 0xffff), v);
}

/* 2531: 40 columns of 200 rows. A byte is written and, if it is 00 or ff,
 * the next byte counts further copies; the column ends when its row count
 * reaches 0 after a byte or after a whole run. Returns SI. */
static unsigned decode_plane(Gfx *g, unsigned seg, unsigned si, const Target *t) {
    for (unsigned column = 0; column < 40 && !g->error; column++) {
        unsigned di = column, cx = 200;
        for (;;) {
            unsigned al = far_byte(g, seg, si); si = (si + 1) & 0xffff;
            target_write(g, t, di, al); di = (di + 40) & 0xffff; cx = (cx - 1) & 0xffff;
            if (!cx || g->error) break;
            if (al != 0 && al != 0xff) continue;
            unsigned ah = al;
            al = far_byte(g, seg, si); si = (si + 1) & 0xffff;
            if (!al) continue;
            for (;;) {
                target_write(g, t, di, ah); di = (di + 40) & 0xffff; cx = (cx - 1) & 0xffff; al = (al - 1) & 255;
                if (!al || g->error) break;
            }
            if (!cx) break;
        }
    }
    return si;
}

/* 40 (24bb): four planes into cs:[2], each inverted after decoding. */
static unsigned picture(Gfx *g, unsigned segment, unsigned si) {
    unsigned pg = page(g, 1);
    for (int p = 0; p < 4 && !g->error; p++) {
        Target t = {0, p, pg, 0};
        si = decode_plane(g, segment, si, &t);
        for (int i = 0; i < 8000 && !g->error; i++) {
            long long a = vaddr(g, pg, i);
            if (a >= 0) g->planes[p][a] ^= 0xff;
        }
    }
    return si;
}

/* ---- the restore lists (68, 64) --------------------------------------------- */
static void record(Gfx *g, unsigned di, unsigned es, unsigned width, unsigned rows) {
    unsigned si = cw(g, 0x2ba8);
    set_cw(g, si, di); set_cw(g, si + 2, es); set_cb(g, si + 4, width); set_cw(g, si + 6, rows);
    set_cw(g, 0x2ba8, si + 8);
}

static void e68(Gfx *g) {
    set_cw(g, 0x25a4, 0xffff); set_cw(g, 0x28a6, 0xffff); set_cw(g, 0x2ba8, 0x25a4); set_cb(g, 0x2baa, 0);
}

static void e64(Gfx *g) {
    set_cw(g, cw(g, 0x2ba8), 0xffff);
    set_cb(g, 0x2baa, cb(g, 0x2baa) ^ 0xff);
    unsigned bx = cb(g, 0x2baa) == 0 ? 0x25a4 : 0x28a6;
    set_cw(g, 0x2ba8, bx);
    unsigned back = page(g, 2);
    for (;;) {
        unsigned di = cw(g, bx), es = cw(g, bx + 2);
        if (di == 0xffff || g->error) break;
        unsigned rows = cb(g, bx + 6); if (!rows) rows = 0x10000;
        unsigned width = ((cw(g, bx + 4) - 1) & 7) + 1;
        for (unsigned y = 0; y < rows && !g->error; y++) {
            unsigned off = (di + 40 * y) & 0xffff;
            for (unsigned k = 0; k < width; k++) copy(g, back, off + k, es, off + k);
        }
        bx += 8;
    }
}

/* ---- sprites (object types 1, 2, 3; the race panel digits) ------------------- */
enum { NORMAL, LEFT, RIGHT };

/* The routine of each width (8, 16, 32), shift and kind (normal, cut at the
 * left edge, cut at the right edge), with or without the auxiliary plane:
 * the masks of each record (colour byte, bit 7 = new row, then width/8 mask
 * bytes) with write mode 2, shifted right by x & 7 over one byte more; at
 * the edges only the bytes on screen (visible of them). Two routines shift
 * by the wrong amount (width 8 cut at the right with x & 7 = 3 shifts by 2,
 * cut at the left with x & 7 = 4 shifts by 3); with the auxiliary plane
 * three routines of width 16 cut at the left (x & 7 = 5, 6, 7) mask a single
 * byte on screen with the auxiliary byte of the next column. */
static unsigned ui_rows(Gfx *g, unsigned seg, unsigned off, unsigned es, unsigned di, unsigned rows,
                        unsigned width, unsigned shift, int kind, unsigned visible, long long aux) {
    unsigned n = width / 8, s = shift;
    if (width == 8 && kind == RIGHT && shift == 3) s = 2;
    if (width == 8 && kind == LEFT && shift == 4) s = 3;
    unsigned c = far_byte(g, seg, off); off++;
    for (;;) {
        unsigned masks[4], out[5], nout;
        for (unsigned k = 0; k < n; k++) masks[k] = far_byte(g, seg, off + k);
        off += n;
        if (s) {
            out[0] = masks[0] >> s;
            for (unsigned j = 1; j < n; j++) out[j] = ((masks[j - 1] << (8 - s)) | (masks[j] >> s)) & 255;
            out[n] = (masks[n - 1] << (8 - s)) & 255;
            nout = n + 1;
        } else { for (unsigned k = 0; k < n; k++) out[k] = masks[k]; nout = n; }
        unsigned first = 0, count = nout;
        if (kind == LEFT) {                         /* out[len - visible:] as Python slices it */
            if (!visible) count = 0;
            else {
                int start = (int)nout - (int)visible;
                if (start < 0) start += (int)nout;
                if (start < 0) start = 0;
                first = (unsigned)start; count = nout - first;
            }
        }
        else if (kind == RIGHT) { if (visible < nout) count = visible; }
        int shifted = aux >= 0 && visible == 1 && width == 16 && kind == LEFT && shift >= 5;
        for (unsigned k = 0; k < count; k++) {
            unsigned mk = out[first + k], a = (di + k) & 0xffff;
            if (aux >= 0) mk &= mb(g, (uint32_t)aux + ((a + shifted) & 0xffff));
            put(g, es, a, c, mk);
        }
        if (g->error) return off;
        c = far_byte(g, seg, off); off++;
        if (c & 0x80) {
            di = (di + 40) & 0xffff; rows = (rows - 1) & 0xffff;
            if (!rows) return off;
        }
    }
}

/* Type 1 (3326): descriptor [+5a] + [+5c] at x [+2a], y [+2e]. */
static void sprite(Gfx *g, unsigned ds, unsigned si, const GfxRegs *r) {
    unsigned n = (obj(g, ds, si, 0x5a) + obj(g, ds, si, 0x5c)) & 0xffff, d = ((n << 3) + 0xa46) & 0xffff;
    unsigned h = cb(g, d + 4), bp = h, skip = 0, w = cb(g, d + 6);
    unsigned y = (cb(g, d + 5) + obj(g, ds, si, 0x2e)) & 0xffff;
    unsigned seg = cw(g, d + 2), off = cw(g, d);
    if (y & 0x8000) {
        unsigned rows = (-y) & 0xffff;
        if (bp <= rows) return;
        bp -= rows;
        unsigned size = (w >> 3) + 1, p = off;
        for (;;) {                                  /* 3459: count row starts */
            p = (p + size) & 0xffff;
            if (far_byte(g, seg, p) & 0x80) { rows--; if (!rows) break; }
            if (g->error) return;
        }
        skip = (p - off) & 0xffff; y = 0;
    } else if (y + bp > 200) {
        unsigned over = (y + bp - 200) & 0xffff;
        if (bp <= over) return;
        bp -= over;
    }
    unsigned x = (cb(g, d + 7) + obj(g, ds, si, 0x2a)) & 0xffff, cl, ax;
    int kind;
    if (x & 0x8000) {
        if (x + w <= 0xffff || ((x + w) & 0xffff) == 0) return;
        cl = (((((x + w) & 0xffff) - 1) >> 3) + 1); kind = LEFT; ax = x & 7;
    } else if ((int)x > 320 - (int)w) {
        if (x >= 320) return;
        cl = ((320 - x - 1) >> 3) + 1; kind = RIGHT; ax = x;
    } else { cl = (w >> 3) + (x & 7 ? 1 : 0); kind = NORMAL; ax = x; }
    unsigned flags = obj(g, ds, si, 0x0a);
    unsigned di = (row(g, y) + (ax >> 3)) & 0xffff, es = page(g, 1);
    if (!(flags & 4)) record(g, di, es, cl & 255, bp);
    if (w != 8 && w != 16 && w != 32) {
        if (w == 24) return;                        /* 346d: RETF for every case of width 24 */
        if (!g->error) g->error = OFF_NOT_MODELLED;
        return;
    }
    long long aux = flags & 1 ? (long long)r->ss * 16 : -1;
    ui_rows(g, seg, (off + skip) & 0xffff, es, di, bp, w, x & 7, kind, cl, aux);
}

/* Type 2 (6394): descriptor 1dc + [+5c], 32 then 16 pixels, no cut. */
static void sprite48(Gfx *g, unsigned ds, unsigned si) {
    unsigned n = (obj(g, ds, si, 0x5c) + 0x1dc) & 0xffff, d = ((n << 3) + 0xa46) & 0xffff;
    unsigned bp = cb(g, d + 4), y = (cb(g, d + 5) + obj(g, ds, si, 0x2e)) & 0xffff;
    unsigned x = obj(g, ds, si, 0x2a), di = (cw(g, (2 * y + 8) & 0xffff) + (x >> 3)) & 0xffff, es = page(g, 1);
    if (!(obj(g, ds, si, 0x0a) & 4)) record(g, di, es, 7, bp);
    unsigned seg = cw(g, d + 2), off = cw(g, d);
    off = ui_rows(g, seg, off, es, di, bp, 32, x & 7, NORMAL, 0, -1);
    if (g->error) return;
    ui_rows(g, seg, off, es, (di + 4) & 0xffff, bp, 16, x & 7, NORMAL, 0, -1);
}

/* Type 3 (346e): [+08] sprites of type 1 side by side. */
static void sprite_row(Gfx *g, unsigned ds, unsigned si, const GfxRegs *r) {
    unsigned x0 = obj(g, ds, si, 0x2a), n0 = obj(g, ds, si, 0x5c), count = obj(g, ds, si, 8);
    if (!count) count = 0x10000;
    for (unsigned k = 0; k < count && !g->error; k++) {
        sprite(g, ds, si, r);
        unsigned n = (obj(g, ds, si, 0x5a) + obj(g, ds, si, 0x5c)) & 0xffff, d = ((n << 3) + 0xa46) & 0xffff;
        set_obj(g, ds, si, 0x2a, obj(g, ds, si, 0x2a) + cb(g, d + 6));
        set_obj(g, ds, si, 0x5c, obj(g, ds, si, 0x5c) + 1);
    }
    set_obj(g, ds, si, 0x5c, n0); set_obj(g, ds, si, 0x2a, x0);
}

/* ---- trucks and shadows ------------------------------------------------------ */
/* Passes of each variant (table 5772 for trucks, 6084 for shadows): source
 * column, number of columns, destination +0/+1, mask (0 bh, 1 its
 * complement, 2 the complement when shifted), and whether the pass needs a
 * shift. Variants 0..6: normal, left cut with 1, 2, 3 bytes on screen,
 * right cut with 1, 2, 3 bytes. */
typedef struct { int column, count, dest, mask, needs_shift; } Pass;
static const Pass PASSES[7][2] = {
    {{0, 3, 0, 0, 0}, {0, 3, 1, 1, 1}},
    {{2, 1, 0, 2, 0}, {-1, 0, 0, 0, 0}},
    {{2, 1, 0, 0, 1}, {1, 2, 0, 2, 0}},
    {{1, 2, 0, 0, 0}, {0, 3, 0, 1, 0}},
    {{0, 1, 0, 0, 0}, {-1, 0, 0, 0, 0}},
    {{0, 2, 0, 0, 0}, {0, 1, 1, 1, 1}},
    {{0, 3, 0, 0, 0}, {0, 2, 1, 1, 0}}};

typedef struct { int truck; unsigned livery_planes; int white; long long aux; } Core;

static void core(Gfx *g, const Core *c, unsigned seg, unsigned off, unsigned es, unsigned di, unsigned rows,
                 int count, unsigned mask, unsigned s, unsigned row_bytes) {
    if (!rows) rows = 0x10000;
    for (unsigned y = 0; y < rows && !g->error; y++) {
        if (c->truck) {
            unsigned data[4][3];
            for (int part = 0; part < 4; part++)
                for (int k = 0; k < count; k++) {
                    unsigned v = far_byte(g, seg, off + 3 * part + k);
                    if (c->aux >= 0) v &= mb(g, (uint32_t)c->aux + ((di + k) & 0xffff));
                    data[part][k] = v;
                }
            for (int k = 0; k < count; k++) mode0(g, es, (di + k) & 0xffff, data[0][k], s, 2, mask, 0xf);
            for (int k = 0; k < count; k++) mode0(g, es, (di + k) & 0xffff, data[1][k], s, 3, mask, 1);
            for (int k = 0; k < count; k++) mode0(g, es, (di + k) & 0xffff, data[2][k], s, 3, mask, 2);
            for (int k = 0; k < count; k++) mode0(g, es, (di + k) & 0xffff, data[3][k], s, 3, mask, c->white ? 8 : c->livery_planes);
            if (c->white)
                for (int k = 0; k < count; k++) mode0(g, es, (di + k) & 0xffff, ~data[3][k] & 255, s, 1, mask, 2);
        } else {
            for (int k = 0; k < count; k++) {
                unsigned v = far_byte(g, seg, off + k);
                if (c->aux >= 0) v &= mb(g, (uint32_t)c->aux + ((di + k) & 0xffff));
                mode0(g, es, (di + k) & 0xffff, v, s, 2, mask, 1);
            }
        }
        off = (off + row_bytes) & 0xffff; di = (di + 40) & 0xffff;
    }
}

static void blit(Gfx *g, unsigned seg, unsigned off, unsigned es, unsigned di, unsigned rows, unsigned bh, unsigned s,
                 int variant, unsigned row_bytes, const Core *c) {
    for (int k = 0; k < 2 && !g->error; k++) {
        const Pass *p = &PASSES[variant][k];
        if (p->column < 0) break;
        if (p->needs_shift && !s) continue;
        unsigned m = p->mask == 0 ? bh : (p->mask == 1 ? ~bh & 255 : (s ? ~bh & 255 : bh));
        core(g, c, seg, (off + p->column) & 0xffff, es, (di + p->dest) & 0xffff, rows, p->count, m, s, row_bytes);
    }
}

/* The horizontal cases of 57aa/60a0: variant, bytes, x for the column; 0 = off screen. */
static int placed(unsigned x, int *variant, unsigned *cl, unsigned *ax) {
    const unsigned width = 24;
    if (x & 0x8000) {
        if (x + width <= 0xffff || ((x + width) & 0xffff) == 0) return 0;
        unsigned c = (((x + width) & 0xffff) - 1) >> 3;
        *variant = 1 + c; *cl = c + 1; *ax = x & 7; return 1;
    }
    if (x > 0x128) {
        if (x >= 320) return 0;
        unsigned c = (320 - x - 1) >> 3;
        *variant = 4 + c; *cl = c + 1; *ax = x; return 1;
    }
    *variant = 0; *cl = x & 7 ? 4 : 3; *ax = x; return 1;
}

/* Rows, skip, row: cut at the top and the bottom; 0 = off screen. */
static int vertical(unsigned y, unsigned bp, int shadow, unsigned *rows_out, unsigned *skip, unsigned *yy) {
    if (y & 0x8000) {
        unsigned rows = (-y) & 0xffff;
        if (bp <= rows) return 0;
        *skip = shadow ? (3 * rows) & 0xff : (12 * rows) & 0xffff;
        *rows_out = bp - rows; *yy = 0; return 1;
    }
    if (y + bp > 200) {
        unsigned over = (y + bp - 200) & 0xffff;
        if (bp <= over) return 0;
        *rows_out = bp - over; *skip = 0; *yy = y; return 1;
    }
    *rows_out = bp; *skip = 0; *yy = y; return 1;
}

/* 60a0: the shadow (descriptor 19c + direction / 8), 2 pixels lower and 2
 * to the left, ORed into plane 0. */
static void shadow(Gfx *g, unsigned ds, unsigned si, uint32_t ss) {
    unsigned t = obj(g, ds, si, 6), b = (unsigned)(-(int)objb(g, ds, t, 0x82)) & 0xf8;
    unsigned d = (b + 0x19c * 8 + 0xa46) & 0xffff, bp = cb(g, d + 4), rows, skip, y, cl, ax;
    if (!vertical((cb(g, d + 5) + 2 + obj(g, ds, si, 0x2e)) & 0xffff, bp, 1, &rows, &skip, &y)) return;
    unsigned x = (cb(g, d + 7) + 0xfffe + obj(g, ds, si, 0x2a)) & 0xffff;
    int variant;
    if (!placed(x, &variant, &cl, &ax)) return;
    unsigned s = x & 7;
    Core c = {0, 0, 0, obj(g, ds, si, 0x0a) & 2 ? (long long)ss : -1};
    unsigned di = (row(g, y) + (ax >> 3)) & 0xffff, es = page(g, 1);
    record(g, di, es, cl, rows);
    unsigned seg = cw(g, d + 2), off = cw(g, d);
    blit(g, seg, (off + skip) & 0xffff, es, di, rows, 0xff >> s, s, variant, 3, &c);
}

/* Type 0 (57aa): shadow, then the truck (descriptor in the truck's +87),
 * lifted by +7e - +80, livery +1e (3 = white). */
static void truck(Gfx *g, unsigned ds, unsigned si, const GfxRegs *r) {
    uint32_t ss = (uint32_t)r->ss * 16;
    shadow(g, ds, si, ss);
    if (g->error) return;
    unsigned t = obj(g, ds, si, 6);
    int lift = s8((objb(g, ds, t, 0x7e) - objb(g, ds, t, 0x80)) & 255);
    unsigned n = obj(g, ds, t, 0x87), d = ((n << 3) + 0xa46) & 0xffff, bp = cb(g, d + 4), rows, skip, y, cl, ax;
    if (!vertical((unsigned)((int)cb(g, d + 5) + (int)obj(g, ds, si, 0x2e) - lift) & 0xffff, bp, 0, &rows, &skip, &y)) return;
    unsigned x = (cb(g, d + 7) + obj(g, ds, si, 0x2a)) & 0xffff;
    int variant;
    if (!placed(x, &variant, &cl, &ax)) return;
    unsigned s = x & 7, livery = objb(g, ds, si, 0x1e);
    Core c = {1, 0, livery == 3, obj(g, ds, si, 0x0a) & 1 ? (long long)ss : -1};
    unsigned di = (row(g, y) + (ax >> 3)) & 0xffff, es = page(g, 1);
    record(g, di, es, cl, rows);
    unsigned seg = cw(g, d + 2), off = cw(g, d);
    c.livery_planes = cb(g, 0x576e + livery);
    blit(g, seg, (off + skip) & 0xffff, es, di, rows, 0xff >> s, s, variant, 12, &c);
}

/* ---- bars and rectangles (types 4, 5, 6) --------------------------------------- */
/* +1c points to x0, y0, x1, y1: row offset + x0/8, rows, edge masks, bytes between. */
static void rectangle(Gfx *g, unsigned ds, unsigned si, unsigned *di, unsigned *rows, unsigned *left, unsigned *right, unsigned *middle) {
    unsigned b = obj(g, ds, si, 0x1c);
    unsigned x0 = mw(g, ds * 16 + (b & 0xffff)), y0 = mw(g, ds * 16 + ((b + 2) & 0xffff));
    unsigned x1 = mw(g, ds * 16 + ((b + 4) & 0xffff)), y1 = mw(g, ds * 16 + ((b + 6) & 0xffff));
    *rows = (y1 - y0) & 0xffff;
    *di = (cw(g, (2 * y0 + 8) & 0xffff) + (x0 >> 3)) & 0xffff;
    *left = cb(g, 0x6428 + (x0 & 7)); *right = cb(g, 0x6430 + (x1 & 7));
    *middle = ((((x1 - x0 + 7) & 0xffff) >> 3) - 2) & 0xffff;
}

/* Type 4 (643d): a bar of colour 6438[+1e] on the drawing page. */
static void bar(Gfx *g, unsigned ds, unsigned si) {
    unsigned di, rows, left, right, middle;
    rectangle(g, ds, si, &di, &rows, &left, &right, &middle);
    unsigned colour = cb(g, 0x6438 + objb(g, ds, si, 0x1e)), es = page(g, 1);
    if (!rows) rows = 0x10000;
    for (unsigned y = 0; y < rows && !g->error; y++) {
        unsigned a = di;
        put(g, es, a, colour, left); a = (a + 1) & 0xffff;
        for (unsigned k = 0; k < middle && !g->error; k++) { put(g, es, a, colour, 0xff); a = (a + 1) & 0xffff; }
        put(g, es, a, colour, right);
        di = (di + 40) & 0xffff;
    }
}

/* Type 5 (64c6): the rectangle from the page on screen to the drawing page
 * and to the background, plane by plane (the edge masks have no effect: the
 * latch comes from the same address of the page read). */
static void freeze(Gfx *g, unsigned ds, unsigned si) {
    unsigned di, rows, left, right, middle;
    rectangle(g, ds, si, &di, &rows, &left, &right, &middle);
    unsigned src = page(g, 0), dsts[2] = {page(g, 1), page(g, 2)};
    if (!rows) rows = 0x10000;
    for (int k2 = 0; k2 < 2; k2++)
        for (int p = 0; p < 4; p++) {
            unsigned a = di;
            for (unsigned y = 0; y < rows && !g->error; y++) {
                for (unsigned k = 0; k < middle + 2 && !g->error; k++) {
                    long long s = vaddr(g, src, (a + k) & 0xffff);
                    if (s < 0) return;
                    long long d = vaddr(g, dsts[k2], (a + k) & 0xffff);
                    if (d < 0) return;
                    g->planes[p][d] = g->planes[p][s];
                }
                a = (a + 40) & 0xffff;
            }
        }
}

/* Type 6 (65ab): [+0c] groups of [+0e] rows in colour 12, then [+0d] groups
 * of [+0f] rows in colour 5, 7 pixels wide from x [+2a], y [+2e], [+10]
 * rows between the groups. */
static void bars(Gfx *g, unsigned ds, unsigned si) {
    unsigned di = (cw(g, (2 * obj(g, ds, si, 0x2e) + 8) & 0xffff) + (obj(g, ds, si, 0x2a) >> 3)) & 0xffff;
    unsigned es = page(g, 1), m = cw(g, 0x659b + 2 * (obj(g, ds, si, 0x2a) & 7)), bl = m & 255, bh = m >> 8;
    unsigned gap = objb(g, ds, si, 0x10) * 40;
    unsigned groups[2] = {objb(g, ds, si, 0x0c), objb(g, ds, si, 0x0d)}, rowsn[2] = {objb(g, ds, si, 0x0e), objb(g, ds, si, 0x0f)};
    const unsigned colours[2] = {0x0c, 5};
    for (int k = 0; k < 2; k++) {
        if (!groups[k] || !rowsn[k]) continue;
        for (unsigned gr = 0; gr < groups[k]; gr++) {
            for (unsigned y = 0; y < rowsn[k]; y++) {
                put(g, es, di, colours[k], bl); put(g, es, (di + 1) & 0xffff, colours[k], bh); di = (di + 40) & 0xffff;
            }
            di = (di + gap) & 0xffff;
        }
    }
}

/* 60: draw the object list whose head is at DS:SI (then head = 0). */
static void e60(Gfx *g, const GfxRegs *r) {
    uint32_t head = (uint32_t)r->ds * 16 + r->si;
    unsigned si = mw(g, head), ds = r->ds;
    while (si && !g->error) {
        unsigned kind = mw(g, ds * 16 + si + 0x1a);
        uint32_t far = cw(g, 0xa2a + kind) | (uint32_t)cw(g, 0xa2c + kind) << 16;
        if (far >> 16 != (uint32_t)(g->base + 9)) { if (!g->error) g->error = OFF_NOT_MODELLED; return; }
        switch (far & 0xffff) {
        case 0x57aa: truck(g, ds, si, r); break;
        case 0x3326: sprite(g, ds, si, r); break;
        case 0x6394: sprite48(g, ds, si); break;
        case 0x346e: sprite_row(g, ds, si, r); break;
        case 0x643d: bar(g, ds, si); break;
        case 0x64c6: freeze(g, ds, si); break;
        case 0x65ab: bars(g, ds, si); break;
        default: if (!g->error) g->error = OFF_NOT_MODELLED; return;
        }
        si = mw(g, ds * 16 + si + 0x16);
    }
    if (!g->error) set_mw(g, head, 0);
}

/* ---- 70: a letter -------------------------------------------------------------- */
/* 2fca: the glyph at BP:DI (big-endian: strips of 16 pixels, each with its
 * rows and 4 plane words), x BX, y AX, row map at DS:CX, slant at DS:DX,
 * colours DS:SI (16 a row); page cs:[0] (CF clear) or cs:[2]. Every row:
 * the colours go through the table, the shape (OR of the planes) is
 * cleared to 0 with AND, then each plane is ORed in. */
static unsigned fw(Gfx *g, uint32_t font, unsigned o) {
    return mb(g, font + (o & 0xffff)) << 8 | mb(g, font + ((o + 1) & 0xffff));
}
static void three(unsigned word, unsigned cl, unsigned out[3]) {
    unsigned rot = cl ? ((word >> cl) | (word << (16 - cl))) & 0xffff : word;
    unsigned ch = (0xff >> cl) & 255, hi = rot >> 8, lo = rot & 255;
    out[0] = hi & ch; out[1] = lo; out[2] = hi & ~ch & 255;
}
static void e70(Gfx *g, const GfxRegs *r) {
    unsigned ds = r->ds;
    set_cw(g, 0x2fbc, ds); set_cw(g, 0x2fbe, r->cx); set_cw(g, 0x2fc0, r->dx); set_cw(g, 0x2fc2, r->si); set_cw(g, 0x2fc8, r->ax);
    unsigned pg = r->cf ? page(g, 1) : page(g, 0);
    set_cw(g, 0x2fc4, pg);
    uint32_t font = (uint32_t)r->bp * 16;
    unsigned si = r->di;
    unsigned strips = (fw(g, font, si) + 1) & 0xffff;
    set_cw(g, 0x2fc6, (fw(g, font, si + 2) + r->bx) & 0xffff);
    si = (si + 6) & 0xffff;
    if (!strips) strips = 0x10000;
    for (unsigned st = 0; st < strips && !g->error; st++) {
        unsigned rows = (fw(g, font, si) + 1) & 0xffff, bx = fw(g, font, si + 2);
        si = (si + 4) & 0xffff;
        if (!rows) rows = 0x10000;
        for (unsigned y = 0; y < rows && !g->error; y++) {
            int rowmap = s8(mb(g, ds * 16 + ((bx + cw(g, 0x2fbe)) & 0xffff)));
            unsigned bp = cw(g, ((((unsigned)(rowmap + (int)cw(g, 0x2fc8)) & 0xffff) * 2 + 8) & 0xffff));
            int slant = s8(mb(g, ds * 16 + ((bx + cw(g, 0x2fc0)) & 0xffff)));
            unsigned ax = (unsigned)(slant + (int)cw(g, 0x2fc6)) & 0xffff, cl = ax & 7, di = (bp + (ax >> 3)) & 0xffff;
            unsigned w[4];
            for (int k = 0; k < 4; k++) w[k] = fw(g, font, si + 2 * k);
            si = (si + 8) & 0xffff;
            for (int k = 0; k < 4; k++) set_cw(g, 0x2fb4 + 2 * k, w[k]);
            unsigned shape = w[0] | w[1] | w[2] | w[3];
            uint32_t table = ds * 16;
            unsigned base = ((bx << 4) + cw(g, 0x2fc2)) & 0xffff;
            /* 308d: 17 rotations through the colour table, in place. */
            unsigned al = 0, cf = 0;
            for (int i = 0; i < 17; i++) {
                for (int k = 3; k >= 0; k--) {
                    unsigned out = w[k] >> 15; w[k] = ((w[k] << 1) | cf) & 0xffff; cf = out;
                    out = al >> 7; al = ((al << 1) | cf) & 255; cf = out;
                }
                al = mb(g, table + ((base + al) & 0xffff));
                for (int j = 0; j < 5; j++) { cf = al >> 7; al = (al << 1) & 255; }
            }
            for (int k = 0; k < 4; k++) set_cw(g, 0x2fb4 + 2 * k, w[k]);
            unsigned b3[3];
            three(shape, cl, b3);
            for (int k = 0; k < 3; k++) mode0(g, pg, (di + k) & 0xffff, ~b3[k] & 255, 0, 1, 0xff, 0xf);
            for (int p = 0; p < 4; p++) {
                three(w[p], cl, b3);
                for (int k = 0; k < 3; k++) mode0(g, pg, (di + k) & 0xffff, b3[k], 0, 2, 0xff, 1u << p);
            }
            bx = (bx + 1) & 0xffff;
        }
        set_cw(g, 0x2fc6, (cw(g, 0x2fc6) + 16) & 0xffff);
    }
}

/* ---- 74..80: the race panel ---------------------------------------------------- */
/* 2cab: the backgrounds of the three counters ([+08], [+14], [+24] of DS:SI,
 * + 1), 3 x 6 and 7 x 6 bytes, from the background page into the scratch page. */
static void e74(Gfx *g, const GfxRegs *r) {
    unsigned ds = r->ds, si = r->si;
    set_cw(g, 0x2c8f, (obj(g, ds, si, 8) + 1) & 0xffff);
    set_cw(g, 0x2c91, (obj(g, ds, si, 0x14) + 1) & 0xffff);
    set_cw(g, 0x2c93, (obj(g, ds, si, 0x24) + 1) & 0xffff);
    unsigned src = page(g, 2), dst = page(g, 3), di = 0;
    set_cw(g, 0x2c95, 0);
    static const unsigned starts[3] = {0x2c8f, 0x2c91, 0x2c93}, widths[3] = {3, 7, 7}, marks[3] = {0x2c97, 0x2c99, 0};
    for (int i = 0; i < 3; i++) {
        unsigned s = cw(g, starts[i]);
        for (int y = 0; y < 6; y++) {
            for (unsigned k = 0; k < widths[i]; k++) copy(g, src, (s + k) & 0xffff, dst, (di + k) & 0xffff);
            di = (di + widths[i]) & 0xffff; s = (s + 40) & 0xffff;
        }
        if (marks[i]) set_cw(g, marks[i], di);
    }
}

/* 2f7b: digit sprite 209h + BL (+ AH * 256) at offset DI, x AX. */
static void panel_digit(Gfx *g, unsigned pg, unsigned di, unsigned x, unsigned digit) {
    unsigned bx = (((x >> 8) << 8) | digit) + 0x209, d = ((bx << 3) + 0xa46) & 0xffff;
    unsigned al = (x + cb(g, d + 7)) & 255, ax = (x & 0xff00) | al;
    di = (di + (ax >> 3)) & 0xffff;
    unsigned rows = ((ax >> 3) & 0xff00) | cb(g, d + 4);
    ui_rows(g, cw(g, d + 2), cw(g, d), pg, di, rows, 8, ax & 7, NORMAL, 0, -1);
}

static void panel(Gfx *g, unsigned mark, unsigned saved, unsigned width, const unsigned (*digits)[2], int n,
                  unsigned ds, unsigned si) {
    unsigned back = page(g, 2), dst = cw(g, mark), src = cw(g, saved);
    for (unsigned y = 0; y < 6; y++)
        for (unsigned k = 0; k < width; k++) copy(g, page(g, 3), (src + width * y + k) & 0xffff, back, (dst + 40 * y + k) & 0xffff);
    for (int i = 0; i < n && !g->error; i++)
        panel_digit(g, back, obj(g, ds, si, digits[i][1]), obj(g, ds, si, digits[i][1] + 2), digits[i][0]);
    unsigned pages[2] = {page(g, 1), page(g, 0)};
    for (int p = 0; p < 2; p++)
        for (unsigned y = 0; y < 6; y++)
            for (unsigned k = 0; k < width; k++) copy(g, back, (dst + 40 * y + k) & 0xffff, pages[p], (dst + 40 * y + k) & 0xffff);
}

/* 2d28: three digits [+0], [+1], [+2] (leading zeros not drawn). */
static void e78(Gfx *g, const GfxRegs *r) {
    unsigned ds = r->ds, si = r->si, digits[3][2];
    int n = 0;
    if (objb(g, ds, si, 0) || objb(g, ds, si, 1)) {
        if (objb(g, ds, si, 0)) { digits[n][0] = objb(g, ds, si, 0); digits[n++][1] = 8; }
        digits[n][0] = objb(g, ds, si, 1); digits[n++][1] = 0xc;
    }
    digits[n][0] = objb(g, ds, si, 2); digits[n++][1] = 0x10;
    panel(g, 0x2c8f, 0x2c95, 3, (const unsigned (*)[2])digits, n, ds, si);
}

/* 2dd2: four digits [+0..+3]. */
static void e7c(Gfx *g, const GfxRegs *r) {
    unsigned ds = r->ds, si = r->si, digits[4][2];
    for (int k = 0; k < 4; k++) { digits[k][0] = objb(g, ds, si, k); digits[k][1] = 0x14 + 4 * k; }
    panel(g, 0x2c91, 0x2c97, 7, (const unsigned (*)[2])digits, 4, ds, si);
}

/* 2e89: four pairs; a pair whose first digit is 0 shows the second one
 * alone, in the middle. */
static void e80(Gfx *g, const GfxRegs *r) {
    unsigned ds = r->ds, si = r->si, digits[8][2];
    static const unsigned fields[4][2] = {{0x24, 0x28}, {0x30, 0x34}, {0x3c, 0x40}, {0x48, 0x4c}};
    int n = 0;
    for (int pair = 0; pair < 4; pair++) {
        unsigned first = objb(g, ds, si, 2 * pair), second = objb(g, ds, si, 2 * pair + 1);
        if (first) {
            digits[n][0] = first; digits[n++][1] = fields[pair][0];
            digits[n][0] = second; digits[n++][1] = fields[pair][0] + 8;
        } else { digits[n][0] = second; digits[n++][1] = fields[pair][1]; }
    }
    panel(g, 0x2c93, 0x2c99, 7, (const unsigned (*)[2])digits, n, ds, si);
}

/* ---- 00, 04, 08, 0c: the start, the end and the shell to DOS (Alt+B) -------------- */
static void e00(Gfx *g, GfxRegs *r) {
    set_cw(g, 0x20c, r->ax); set_cb(g, 0x1fe, r->bx & 255);
    for (int i = 0; i < 200; i++) set_cw(g, 8 + 2 * i, 40 * i);
    set_cb(g, 0x200, g->mode); set_cb(g, 0x201, g->page_number);
    set_mode(g, 0x0d);
    unsigned kind = r->bx & 255, table, size, loader;
    if (kind == 1) { table = 0x3b7; size = 0x11; loader = 0x8f3; }
    else if (kind == 2) { table = 0x418; size = 0x11; loader = 0x8f3; }
    else { attribute_defaults(g); table = 0x479; size = 0x33; loader = 0x9a0; }
    set_cw(g, 0x202, table); set_cw(g, 0x204, size); set_cw(g, 0x206, loader);
    palette(g, 0);
    uint32_t hook = (uint32_t)r->bp * 16 + r->di;
    set_cw(g, 0x208, mw(g, hook)); set_cw(g, 0x20a, mw(g, hook + 2));
    set_mw(g, hook, 0x9ee); set_mw(g, hook + 2, g->base + 9);
    r->es = r->bp;
}

static void e38(Gfx *g) { for (unsigned i = 0; i < cw(g, 0x204); i++) set_cb(g, 0x1cb + i, cb(g, 0x198 + i)); }
static void e3c(Gfx *g) {
    for (unsigned i = 0; i < cw(g, 0x204); i++) set_cb(g, 0x198 + i, cb(g, 0x1cb + i));
    load_palette(g, 0x1cb, 0xff);
}

/* 0645: page cs:[0] filled with AH, palette 1, the old mode and page. */
static void back_to_text(Gfx *g, unsigned ah) {
    fill(g, 0, ah); palette(g, 1);
    set_mode(g, cb(g, 0x200)); g->page_number = (uint8_t)cb(g, 0x201);
}

static void e04(Gfx *g, const GfxRegs *r) {
    uint32_t hook = (uint32_t)r->bp * 16 + r->di;
    set_mw(g, hook, cw(g, 0x208)); set_mw(g, hook + 2, cw(g, 0x20a));
    back_to_text(g, cw(g, 0x208) >> 8);
}

static void e08(Gfx *g) {
    uint32_t save = (uint32_t)cw(g, 0x20c) * 16;
    unsigned back = page(g, 2), scratch = page(g, 3);
    for (int p = 0; p < 4; p++)
        for (int i = 0; i < 8000; i++) { long long a = vaddr(g, back, i); if (a < 0) return; set_mb(g, save + p * 8000 + i, g->planes[p][a]); }
    for (int p = 0; p < 4; p++)
        for (int i = 0; i < 0x66; i++) { long long a = vaddr(g, scratch, i); if (a < 0) return; set_cb(g, 0x21f + p * 0x66 + i, g->planes[p][a]); }
    e38(g); back_to_text(g, (g->base + 9) >> 8);
}

static void e0c(Gfx *g) {
    set_mode(g, 0x0d); attribute_defaults(g);
    uint32_t save = (uint32_t)cw(g, 0x20c) * 16;
    unsigned back = page(g, 2);
    for (int p = 0; p < 4; p++)
        for (int i = 0; i < 8000; i++) { long long a = vaddr(g, back, i); if (a < 0) return; g->planes[p][a] = (uint8_t)mb(g, save + p * 8000 + i); }
    copy_page(g, 2, 1); e3c(g); copy_page(g, 2, 0);
    unsigned scratch = page(g, 3);
    for (int p = 0; p < 4; p++)
        for (int i = 0; i < 0x66; i++) { long long a = vaddr(g, scratch, i); if (a < 0) return; g->planes[p][a] = (uint8_t)cb(g, 0x21f + p * 0x66 + i); }
}

static void e5c(Gfx *g, const GfxRegs *r) {
    unsigned ax = r->ax;
    picture(g, ax, 4); copy_page(g, 1, 2);
    unsigned fifth = ((far_byte(g, ax, 2) << 8 | far_byte(g, ax, 3)) + 4) & 0xffff;
    uint32_t ss = (uint32_t)r->ss * 16;
    Target t = {1, 0, 0, ss};
    decode_plane(g, ax, fifth, &t);
    for (int i = 0; i < 8000; i++) set_mb(g, ss + i, mb(g, ss + i) ^ 0xff);
}

/* ---- the entries ----------------------------------------------------------------- */
void gfx_init(Gfx *g, uint8_t *mem, uint16_t data_segment) {
    memset(g, 0, sizeof *g);
    g->mem = mem; g->ds = data_segment;
    g->base = (uint16_t)(mem[data_segment * 16 + 0xcb4e] | mem[data_segment * 16 + 0xcb4f] << 8);
    g->cs = ((uint32_t)g->base + 9) * 16;
    g->mode = 0x0d;
}

int gfx_call(Gfx *g, unsigned entry, GfxRegs *r) {
    g->error = OFF_OK;
    switch (entry) {
    case 0x00: e00(g, r); break;
    case 0x04: e04(g, r); break;
    case 0x08: e08(g); break;
    case 0x0c: e0c(g); break;
    case 0x10: r->es = g->base + 9; r->di = 0x2275; break;
    case 0x14: { unsigned bx = cw(g, (2 * r->bx + 8) & 0xffff); r->ax = (bx + (r->ax >> 3)) & 0xffff; r->bx = bx; break; }
    case 0x18: set_cb(g, 0x1ff, 1); break;
    case 0x1c: fill(g, 1, r->ax >> 8); break;
    case 0x20: fill(g, 0, r->ax >> 8); break;
    case 0x24: fill(g, 2, r->ax >> 8); break;
    case 0x28: copy_page(g, 1, 0); palette(g, r->ax & 255); break;
    case 0x2c: palette(g, 0); break;
    case 0x30: palette(g, r->ax & 255); break;
    case 0x34: g->attr[0x11] = (uint8_t)r->ax; break;
    case 0x38: e38(g); break;
    case 0x3c: e3c(g); break;
    case 0x40: picture(g, r->ax, r->si); break;
    case 0x44: copy_page(g, 0, 1); break;
    case 0x48: copy_page(g, 0, 2); break;
    case 0x4c: copy_page(g, 1, 0); break;
    case 0x50: copy_page(g, 1, 2); break;
    case 0x54: copy_page(g, 2, 0); break;
    case 0x58: copy_page(g, 2, 1); break;
    case 0x5c: e5c(g, r); break;
    case 0x60: e60(g, r); break;
    case 0x64: e64(g); break;
    case 0x68: e68(g); break;
    case 0x70: e70(g, r); break;
    case 0x74: e74(g, r); break;
    case 0x78: e78(g, r); break;
    case 0x7c: e7c(g, r); break;
    case 0x80: e80(g, r); break;
    /* 6c (a pixel with the controller as left by the previous routine): the
     * module and the game never call it. */
    default: g->error = OFF_NOT_MODELLED;
    }
    return g->error;
}

int offroad_graphics(uint8_t *mem, uint8_t *planes, uint8_t *dac, uint8_t *attr, uint16_t *start,
                     uint8_t *mode_page, int entry, uint16_t *regs) {
    static Gfx g;
    gfx_init(&g, mem, 0x0d98);
    memcpy(g.planes, planes, sizeof g.planes); memcpy(g.dac, dac, 768); memcpy(g.attr, attr, 0x15);
    g.start = *start; g.mode = mode_page[0]; g.page_number = mode_page[1];
    GfxRegs r;
    memcpy(&r, regs, sizeof r);
    int out;
    if (entry < 0) { gfx_flip(&g); out = g.error; }
    else out = gfx_call(&g, (unsigned)entry, &r);
    memcpy(planes, g.planes, sizeof g.planes); memcpy(dac, g.dac, 768); memcpy(attr, g.attr, 0x15);
    *start = g.start; mode_page[0] = g.mode; mode_page[1] = g.page_number;
    memcpy(regs, &r, sizeof r);
    return out;
}
