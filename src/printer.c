/* The text printer of game.exe (0434:0f93 and its commands), the glyph width
 * (0434:0f01) and the width of a text (0434:0f2e): a literal translation of
 * tools/text_printer.py.
 * A text is a byte list in the data segment: bytes from 20h up are characters
 * of the current font, bytes below are commands (table at DS:0996, argument
 * sizes for the width routine at DS:09ca):
 *   00 end                      0a save the cursor (098a, 098c)
 *   01 nothing                  0b restore the cursor
 *   02 x = word (big-endian)    0c print text n of the current text table
 *   03 y = byte                 0d repeat the last character n times
 *   04 x = word, y = byte       0e character spacing (098e, signed)
 *   05 x += word                0f line spacing (098f, signed)
 *   06 y += signed byte         10/11 proportional / fixed width (0994)
 *   07 x += word, y += s. byte  12/13 row map 0746 down / up
 *   08 new line                 14/15/16 slant table 0766: none, right, left
 *   09 margin = byte            17 font by name, 18 colours by name, 19 text table by name
 * The cursor is DS:0986 (x) and DS:0988 (y). Every character inside the
 * font's range is drawn by the graphics module's entry 70h.
 * Fonts are the original files, big-endian: name, link to the next font
 * (+4), first and last character (+9, +0b), height (+0c), fixed width (+0e),
 * glyph offsets from +12, glyph width at glyph +4. In memory ROAD (segment
 * in DS:0178) comes first and links to IRON (segment in DS:016a).
 * Colour schemes come from ega.exe: the table at module offset 20b9 lists
 * 17 entries of 18h bytes (name, link, flag at +6, colours from +8), linked
 * at start-up in reverse, so the last one is the head.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "internal.h"

#define MODULE_CODE 9          /* ega.exe code and data: segment [cb4e] + 9 */
#define COLOUR_TABLE 0x20b9

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

Far *far_load(const char *dir) {
    Far *f = calloc(1, sizeof *f);
    int n;
    uint8_t *ega = slurp(dir, "ega.exe", &n);
    f->road = slurp(dir, "roadfont.xx", &f->nroad);
    f->iron = slurp(dir, "ironfont.xx", &f->niron);
    if (!ega || !f->road || !f->iron) { free(ega); far_free(f); return 0; }
    int header = (ega[8] * 16) | (ega[9] << 12);
    f->nega = n - header;
    f->ega = malloc(f->nega);
    memcpy(f->ega, ega + header, f->nega);
    free(ega);
    for (unsigned at = COLOUR_TABLE;; at += 2) {
        unsigned entry = f->ega[MODULE_CODE * 16 + at] | f->ega[MODULE_CODE * 16 + at + 1] << 8;
        if (!entry || f->norder == 64) break;
        f->order[f->norder++] = u16(entry);
    }
    return f;
}
void far_free(Far *f) {
    if (!f) return;
    free(f->road); free(f->iron); free(f->ega); free(f);
}

static void perr(Printer *p, int e) { if (!p->error) p->error = e; }
unsigned far_module_byte(Printer *p, unsigned offset) {
    long long at = MODULE_CODE * 16 + (long long)offset;
    if (at >= p->far->nega) { perr(p, OFF_BAD_ADDRESS); return 0; }
    return p->far->ega[at];
}
/* the font at a segment, as the game holds it (ROAD links to IRON at +4) */
static unsigned font_byte(Printer *p, const uint8_t *m, unsigned segment, long long offset) {
    unsigned road = rw(m, 0x178), iron = rw(m, 0x16a);
    const uint8_t *data;
    int n;
    if (segment == road) { data = p->far->road; n = p->far->nroad; }
    else if (segment == iron) { data = p->far->iron; n = p->far->niron; }
    else { perr(p, OFF_TEXT); return 0; }   /* no font at that segment */
    if (offset < n) {
        if (segment == road && offset == 4) return iron & 255;
        if (segment == road && offset == 5) return iron >> 8;
        return data[offset];
    }
    /* past ROAD the memory holds IRON (the paragraphs that follow) */
    if (segment == road) {
        long long linear = offset - ((long long)iron - (long long)road) * 16;
        if (linear >= 0 && linear < p->far->niron) return p->far->iron[linear];
    }
    perr(p, OFF_TEXT);   /* a font read outside the files */
    return 0;
}
/* big-endian, as the printer reads them (MOV + XCHG) */
static unsigned font_word(Printer *p, const uint8_t *m, unsigned segment, long long offset) {
    unsigned hi = font_byte(p, m, segment, offset);
    return hi << 8 | font_byte(p, m, segment, offset + 1);
}

/* 0434:116f */
void text_row_map(Printer *p, uint8_t *m) {
    unsigned font = rw(m, 0x73a), dh = 0, dl = 1;
    if (m[0x991]) { dh = (font_byte(p, m, font, 0xd) - 1) & 255; dl = 0xff; }
    for (int i = 0; i < 0x20; i++) { m[0x746 + i] = u8(dh); dh = (dh + dl) & 255; }
}
/* 0434:1194 */
void text_slant(Printer *p, uint8_t *m) {
    unsigned font = rw(m, 0x73a), mode = m[0x992];
    unsigned long long count = font_word(p, m, font, 0xc);
    if (!count) count = 0x10000;   /* LOOP with CX = 0 */
    unsigned half = (count & 255) >> 1, value, step;
    if (mode == 0) { value = 0; step = 0; }
    else if (mode & 0x80) { value = (-half) & 255; step = 1; }
    else { value = half; step = 0xff; }
    if (p->error) return;
    for (unsigned long long i = 0; i < count; i++) { m[(0x766 + i) & 65535] = u8(value); value = (value + step) & 255; }
}
static void glyph_event(Printer *p, const GfxCall *call) {
    if (p->c) cycle_call_graphics(p->c, call);
    else PUSH(p->events, *call);
}
/* 0434:0fab */
void text_glyph(Printer *p, uint8_t *m, unsigned ch) {
    unsigned font = rw(m, 0x73a);
    if (ch < font_byte(p, m, font, 9) || ch > font_byte(p, m, font, 0xb) || p->error) return;
    m[0x993] = u8(ch);
    unsigned index = (ch - font_byte(p, m, font, 9)) & 255;
    unsigned di = font_word(p, m, font, 0x12 + 2 * index);
    unsigned width = font_word(p, m, font, di + 4);
    if (p->error) return;
    GfxCall call = {0};
    call.entry = 0x70; call.has = GA_GLYPH;
    call.x = u16(rw(m, 0x986)); call.y = u16(rw(m, 0x988)); call.font = u16(font); call.glyph = u16(di);
    call.ch = u8(ch); call.carry = m[0x995] & 1;
    glyph_event(p, &call);
    if (m[0x994]) width = font_word(p, m, font, 0xe);
    ww(m, 0x986, rw(m, 0x986) + width + s8(m[0x98e]));
}
/* 0434:0f01 */
unsigned text_glyph_width(Printer *p, uint8_t *m, unsigned ch) {
    unsigned font = rw(m, 0x73a);
    if (m[0x994]) return font_word(p, m, font, 0xe);
    unsigned index = (ch - font_byte(p, m, font, 9)) & 255;
    unsigned di = font_word(p, m, font, 0x12 + 2 * index);
    return font_word(p, m, font, di + 4);
}
/* 0434:0f2e */
unsigned text_width(Printer *p, uint8_t *m, unsigned si) {
    unsigned cx = 0;
    uint8_t saved994 = m[0x994], saved98e = m[0x98e];
    for (;;) {
        unsigned al = m[si];
        si = (si + 1) & 65535;
        if (al >= 0x20) {
            cx = (cx + text_glyph_width(p, m, al) + s8(m[0x98e])) & 65535;
            if (p->error) return 0;
            continue;
        }
        if (al == 0) { m[0x994] = saved994; m[0x98e] = saved98e; return cx; }
        if (al == 0x11) m[0x994] = 1;
        else if (al == 0x10) m[0x994] = 0;
        else if (al == 5) { cx = (cx + (m[si] << 8 | m[(si + 1) & 65535])) & 65535; si = (si + 2) & 65535; }
        else if (al == 0xe) { m[0x98e] = m[si]; si = (si + 1) & 65535; }
        else si = (si + s8(m[0x9ca + al])) & 65535;   /* CBW of the argument size */
    }
}

static unsigned command(Printer *p, uint8_t *m, unsigned al, unsigned si);
static unsigned print_loop(Printer *p, uint8_t *m, unsigned si);
/* 0434:0f93 */
unsigned text_print(Printer *p, uint8_t *m, unsigned si) {
    /* command 0c prints another text from inside this one; a table that
     * leads back makes the model run out of Python's stack (a hang here) */
    static int depth;
    if (depth > 400) { perr(p, OFF_HANG); return si; }
    depth++;
    si = print_loop(p, m, si);
    depth--;
    return si;
}
static unsigned print_loop(Printer *p, uint8_t *m, unsigned si) {
    for (;;) {
        unsigned al = m[si];
        si = (si + 1) & 65535;
        if (al >= 0x20) { text_glyph(p, m, al); if (p->error) return si; continue; }
        if (al == 0) return si;
        si = command(p, m, al, si);
        if (p->error) return si;
    }
}

/* 0434:11cd */
void text_colours(Printer *p, uint8_t *m, unsigned entry) {
    int repeat = far_module_byte(p, entry + 6) == 0;
    unsigned at = entry + 8;
    for (int row = 0; row < 0x20; row++) {
        for (int i = 0; i < 16; i++) m[0x786 + row * 16 + i] = u8(far_module_byte(p, at + i));
        if (!repeat) at += 16;
    }
}

static unsigned command(Printer *p, uint8_t *m, unsigned al, unsigned si) {
#define BYTE() (si = (si + 1) & 65535, m[(si - 1) & 65535])
#define WORD() (si = (si + 2) & 65535, (unsigned)(m[(si - 2) & 65535] << 8 | m[(si - 1) & 65535]))
    switch (al) {
    case 1: break;
    case 2: ww(m, 0x986, WORD()); break;
    case 3: ww(m, 0x988, BYTE()); break;
    case 4: { unsigned x = WORD(); ww(m, 0x986, x); ww(m, 0x988, BYTE()); break; }
    case 5: ww(m, 0x986, rw(m, 0x986) + WORD()); break;
    case 6: ww(m, 0x988, rw(m, 0x988) + s8(BYTE())); break;
    case 7: { unsigned x = WORD(); ww(m, 0x986, rw(m, 0x986) + x); ww(m, 0x988, rw(m, 0x988) + s8(BYTE())); break; }
    case 8:
        ww(m, 0x986, m[0x990]);
        ww(m, 0x988, rw(m, 0x988) + s8(m[0x98f]) + font_word(p, m, rw(m, 0x73a), 0xc));
        break;
    case 9: m[0x990] = BYTE(); break;
    case 0xa: ww(m, 0x98a, rw(m, 0x986)); ww(m, 0x98c, rw(m, 0x988)); break;
    case 0xb: ww(m, 0x986, rw(m, 0x98a)); ww(m, 0x988, rw(m, 0x98c)); break;
    case 0xc: {
        unsigned n = BYTE(), table = rw(m, 0x744);
        text_print(p, m, (table + rw(m, (table + 6 + 2 * n) & 65535)) & 65535);
        break;
    }
    case 0xd: {
        /* 10a2 reaches the glyph routine with a NEAR call, but it ends with
         * RETF: the original goes on at CX:10a5. No text of the game has it. */
        unsigned n = BYTE();
        text_glyph(p, m, m[0x993]);
        if (p->error) break;
        p->far_return = 1; p->far_cs = u16(n);
        perr(p, OFF_TEXT);
        break;
    }
    case 0xe: m[0x98e] = BYTE(); break;
    case 0xf: m[0x98f] = BYTE(); break;
    case 0x10: m[0x994] = 0; break;
    case 0x11: m[0x994] = 1; break;
    case 0x12: case 0x13: m[0x991] = u8(al - 0x12); text_row_map(p, m); break;
    case 0x14: case 0x15: case 0x16: {
        static const uint8_t modes[3] = {0, 1, 0xff};
        m[0x992] = modes[al - 0x14]; text_slant(p, m);
        break;
    }
    case 0x17: {
        unsigned ax = rw(m, si), bx = rw(m, si + 2), font = rw(m, 0x738);
        int i;
        for (i = 0; i < 64; i++) {
            unsigned a = font_byte(p, m, font, 0) | font_byte(p, m, font, 1) << 8;
            unsigned b = font_byte(p, m, font, 2) | font_byte(p, m, font, 3) << 8;
            if (p->error) return si;
            if (a == ax && b == bx) break;
            font = font_byte(p, m, font, 4) | font_byte(p, m, font, 5) << 8;
            if (p->error) return si;
        }
        if (i == 64) { perr(p, OFF_TEXT); return si; }   /* font not found */
        ww(m, 0x73a, font); si = (si + 4) & 65535;
        m[0x990] = m[0x991] = m[0x992] = m[0x994] = 0;
        text_row_map(p, m); text_slant(p, m);
        break;
    }
    case 0x18: {
        unsigned name = rw(m, si), entry = rw(m, 0x73c);
        si = (si + 2) & 65535;
        if (rw(m, 0x73e) != (unsigned long long)rw(m, 0xcb4e) + MODULE_CODE) { perr(p, OFF_TEXT); return si; }   /* the colour list outside the module */
        int i;
        for (i = 0; i < 64; i++) {
            unsigned v = far_module_byte(p, entry) | far_module_byte(p, entry + 1) << 8;
            if (p->error) return si;
            if (v == name) break;
            int j, found = -1;
            for (j = 0; j < p->far->norder; j++) if (p->far->order[j] == entry) { found = j; break; }
            if (found < 0) { perr(p, OFF_TEXT); return si; }   /* not a registered scheme */
            entry = found ? p->far->order[found - 1] : 0;
        }
        if (i == 64) { perr(p, OFF_TEXT); return si; }   /* colour scheme not found */
        ww(m, 0x740, entry); text_colours(p, m, entry);
        break;
    }
    case 0x19: {
        unsigned ax = rw(m, si), bx = rw(m, si + 2), table = rw(m, 0x742);
        int i;
        for (i = 0; i < 64; i++) {
            if (rw(m, table) == ax && rw(m, table + 2) == bx) break;
            table = rw(m, table + 4);
        }
        if (i == 64) { perr(p, OFF_TEXT); return si; }   /* text table not found */
        ww(m, 0x744, table); si = (si + 4) & 65535;
        break;
    }
    default: perr(p, OFF_TEXT);   /* no such command */
    }
    return si;
#undef BYTE
#undef WORD
}
