/* The program and DOS: a literal translation of tools/program.py, on a 640 KB
 * image of conventional memory and the graphics module (graphics.c), without
 * running any x86 code.
 *   DOS        EXEC: the environment (07ca), the PSP (0814), the program's
 *              block from 0813, game.exe loaded at 0824 with its relocations;
 *              INT 21h for memory (first fit, MCBs named after the program),
 *              vectors, files (find first fills the DTA), overlays (4B03h).
 *   0434:0004  the library: DS, the PSP, DOS version, the program's block cut
 *              to its image and stack, a stack of 8200h bytes, an arena of
 *              4800h paragraphs or less for the library's own memory manager
 *              (blocks with a header of 16 bytes: +0a the block before, +0c
 *              the state of the block before and +0d its own ('M' used, 'F'
 *              free), +0e the size, +10/+12 the free list), the random seed,
 *              the CPU calibration, the BIOS timer vector, the vectors of INT
 *              00..03, then the devices in the order of their dependencies.
 *   devices    records of 12h bytes from DS:cc0e: a state word (1, 8000h once
 *              started), four far routines (+2 start, +6 end, +0a out to DOS,
 *              +0e back from DOS) and the records they depend on; the started
 *              ones are listed from DS:cc8e. Keyboard cc0e, critical error
 *              cc4c, sound cc60, graphics driver cc74, timer cc22, tasks cc38.
 *   0000:0000  the game: the module's file list, black, fonts, colour
 *              schemes, the text table, the loading picture, the sprites
 *              converted (1038), poly.bin and quad.bin, the records (367f),
 *              the drivers (12ae), the joystick question (124a), the sound
 *              question (3b1a) and the sound driver (3a5b).
 *   0434:00a6  the way out; 0434:00c8 the shell.
 * Port writes come out in ports as (port, value): the PIT (40h, 43h), the
 * game port (201h), the AdLib (388h, 389h) and the speaker (42h, 61h).
 * The data segment is part of the one memory here (the model keeps it as a
 * separate array once a game is attached; the bytes are the same).
 */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include "internal.h"

#define CODE CODE_SEGMENT
#define DATA DATA_SEGMENT
#define LIB LIB_SEGMENT
#define DRV DRV_SEGMENT
#define PSP PSP_SEGMENT
#define ENV ENV_SEGMENT
#define MAIN_SP 0x8200                        /* SP in 0000:0000.. (0434:0086 jumps there) */
#define DIVIDE_ERROR (DATA & 255)             /* 0434:0272: AX = the data segment, its low byte */
#define MAIN_TASK 0x6c6
#define IDLE_TASK 0x6ea

const char *const POINT_NAMES[POINTS] = {"joystick question", "sound question", "disk request", "quit question",
                                         "pause", "save question", "key wait", "ticks"};

void run_raise(Run *r, int outcome) {
    r->outcome = outcome;
    longjmp(*(jmp_buf *)r->jump, 1);
}
static void raise_(Program *p, int outcome) { run_raise(p->run, outcome); }
static void not_modelled(Program *p) { raise_(p, OFF_NOT_MODELLED); }

void machine_init(Machine *mc, const uint8_t *environment, int n, const char *disk) {
    memset(mc, 0, sizeof *mc);
    if (n > (int)sizeof mc->environment) n = sizeof mc->environment;
    memcpy(mc->environment, environment, n); mc->nenvironment = n;
    memcpy(mc->tail, " /vga", 5); mc->ntail = 5;
    mc->tail_end[0] = '\r'; mc->ntail_end = 1;
    snprintf(mc->disks[0], sizeof mc->disks[0], "%s", disk); mc->ndisks = 1;
    mc->top = 0x9fff; mc->dos_version = 5;
    static const int none[4] = {-1, -1, -1, -1};
    joy_init(&mc->joystick, none, 0xf0);
    mc->time_ticks = 1;
}

/* ---- memory ------------------------------------------------------------------- */
static unsigned b(Program *p, unsigned seg, unsigned off) {
    uint32_t a = (seg * 16u + off) & 0xfffff;
    if (a >= GFX_MEMORY) not_modelled(p);   /* a read past conventional memory (the model's index error) */
    return p->M[a];
}
static void sb(Program *p, unsigned seg, unsigned off, unsigned v) {
    uint32_t a = (seg * 16u + off) & 0xfffff;
    if (a >= GFX_MEMORY) not_modelled(p);   /* a write past conventional memory */
    p->M[a] = u8(v);
}
static unsigned w(Program *p, unsigned seg, unsigned off) { return b(p, seg, off) | b(p, seg, off + 1) << 8; }
static void sw(Program *p, unsigned seg, unsigned off, unsigned v) { sb(p, seg, off, v); sb(p, seg, off + 1, v >> 8); }
#define DS (p->M + DATA * 16)
static unsigned d(Program *p, unsigned off) { return rw(DS, off); }
static void sd(Program *p, unsigned off, unsigned v) { ww(DS, off, v); }
static void write_(Program *p, uint32_t linear, const uint8_t *data, long long n) {
    if (linear + n > GFX_MEMORY) not_modelled(p);
    memcpy(p->M + linear, data, n);
}
static void out(Program *p, unsigned port, unsigned value) {
    if (p->host.port) p->host.port(p->host.ctx, port, value & 255);
    if (!p->host.live) PUSH(p->ports, (uint32_t)port << 8 | (value & 255));
}
static void port_out(void *ctx, unsigned port, unsigned value) { out(ctx, port, value); }
uint8_t *program_snapshot(Program *p) { return p->M; }

/* ---- the sound driver on the data segment ---------------------------------------- */
static void opl(Program *p, const uint8_t *writes, int n) {
    for (int i = 0; i < n; i++) { out(p, 0x388, writes[2 * i]); out(p, 0x389, writes[2 * i + 1]); }
}
/* the speaker player's writes: PIT channel 2 (43h B6h, 42h low, high) and the
 * gate on port 61h (03h on, 00h off; the bits read from 61h are 0) */
static void speaker_out(Program *p, const uint16_t *events, int n) {
    for (int i = 0; i < n; i++) {
        unsigned kind = events[2 * i], divisor = events[2 * i + 1];
        if (kind == SPK_PIT) { out(p, 0x43, 0xb6); out(p, 0x42, divisor & 255); out(p, 0x42, divisor >> 8); }
        else if (kind == SPK_ON) out(p, 0x61, 3);
        else out(p, 0x61, 0);
    }
}
static uint8_t sound_writes[2 * 65536];
static uint16_t sound_speaker[2 * 4096];
static void sound_new(Program *p, Sound *s) {
    memset(s, 0, sizeof *s);
    s->m = DS; s->opl = sound_writes; s->opl_cap = 65536; s->spk = sound_speaker; s->spk_cap = 4096;
}
static void sound_done(Program *p, Sound *s) {
    opl(p, s->opl, s->nopl); speaker_out(p, s->spk, s->nspk);
    if (s->error) raise_(p, s->error);
}
static void sound_request(Program *p, int kind, unsigned value) {
    Sound s;
    sound_new(p, &s);
    snd_request(&s, kind, value);
    sound_done(p, &s);
}
/* a sound request of the game (3af8 effect, 3aaa/3ad1 script audio, 3b05 music, 3b12 off) */
static void program_sound(void *ctx, int kind, unsigned value) {
    static const int kinds[5] = {SND_EFFECT, SND_SCRIPT_AUDIO, SND_SCRIPT_AUDIO_B, SND_MUSIC, SND_MUSIC_OFF};
    sound_request(ctx, kinds[kind], value);
}

/* ---- DOS ------------------------------------------------------------------------- */
static void mcb(Program *p, unsigned seg, char kind, unsigned owner, unsigned size, const char *name) {
    uint8_t *a = p->M + seg * 16;
    a[0] = (uint8_t)kind; a[1] = u8(owner); a[2] = u8(owner >> 8); a[3] = u8(size); a[4] = u8(size >> 8);
    a[5] = a[6] = a[7] = 0;
    memset(a + 8, 0, 8);
    memcpy(a + 8, name, strlen(name) < 8 ? strlen(name) : 8);
}
/* a free block keeps the name bytes that were there (DOSBox-X) */
static void free_mcb(Program *p, unsigned seg, char kind, unsigned size) {
    uint8_t *a = p->M + seg * 16;
    a[0] = (uint8_t)kind; a[1] = a[2] = 0; a[3] = u8(size); a[4] = u8(size >> 8); a[5] = a[6] = a[7] = 0;
}
typedef struct { unsigned seg; char kind; unsigned owner, size; } Block;
static int chain(Program *p, Block *out_, int cap) {
    int n = 0;
    unsigned a = PSP - 1;
    for (;;) {
        if (a * 16 + 5 > GFX_MEMORY || n == cap) not_modelled(p);
        Block k = {a, (char)p->M[a * 16], W(p->M, a * 16 + 1), W(p->M, a * 16 + 3)};
        out_[n++] = k;
        if (k.kind == 'Z' || a + k.size + 1 >= p->mc->top) return n;
        a += k.size + 1;
    }
}
static long long file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode)) return -1;
    return st.st_size;
}
static uint8_t *read_path(const char *path, long long *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(*n ? *n : 1);
    if (fread(data, 1, *n, f) != (size_t)*n) { free(data); data = 0; }
    fclose(f);
    return data;
}
/* the name DOS looks up: lower case, after the last backslash */
static void base_name(const char *name, char *out_, int cap) {
    const char *slash = strrchr(name, '\\');
    if (slash) name = slash + 1;
    int i;
    for (i = 0; name[i] && i < cap - 1; i++) out_[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
    out_[i] = 0;
}
/* DOS's EXEC: the environment block, the PSP, the program's block (all
 * memory), game.exe at 0824 with its relocations; SS, SP */
static void dos_exec(Program *p, unsigned *ss, unsigned *sp) {
    Machine *mc = p->mc;
    uint8_t env[0x900];
    int n = mc->nenvironment;
    memcpy(env, mc->environment, n);
    memcpy(env + n, "\0\0\x01\0C:\\GAME.EXE\0", 16); n += 16;
    unsigned blocks = (n + 15) / 16, top = mc->top;
    mcb(p, ENV - 1, 'M', PSP, blocks, "GAME"); write_(p, ENV * 16, env, n);
    mcb(p, PSP - 1, 'Z', PSP, top - PSP, "GAME");
    uint8_t *q = p->M + PSP * 16;
    q[0] = 0xcd; q[1] = 0x20; put(q, 2, top);
    put(q, 0x2c, ENV);
    q[0x80] = u8(mc->ntail);
    write_(p, PSP * 16 + 0x81, mc->tail, mc->ntail); write_(p, PSP * 16 + 0x81 + mc->ntail, mc->tail_end, mc->ntail_end);
    char path[1200];
    snprintf(path, sizeof path, "%s/game.exe", mc->disks[0]);
    long long size;
    uint8_t *raw = read_path(path, &size);
    if (!raw) not_modelled(p);
    unsigned header = W(raw, 8) * 16, count = W(raw, 6), table = W(raw, 0x18);
    long long body = size - header;
    uint8_t *image = malloc(body);
    memcpy(image, raw + header, body);
    for (unsigned i = 0; i < count; i++) {
        unsigned off = W(raw, table + 4 * i), seg = W(raw, table + 4 * i + 2);
        unsigned long long at = seg * 16ul + off;
        put(image, at, W(image, at) + CODE);
    }
    write_(p, CODE * 16, image, body);
    free(image);
    *ss = (CODE + W(raw, 2 + 2 * 6)) & 0xffff; *sp = W(raw, 2 + 2 * 7);
    free(raw);
}
/* INT 21h 4Ah: 0 (and no change) when the block cannot grow */
static int dos_resize(Program *p, unsigned seg, unsigned want) {
    Block list[64];
    int n = chain(p, list, 64), i, j;
    for (i = 0; i < n && list[i].seg != seg - 1; i++);
    if (i == n) not_modelled(p);
    char kind = list[i].kind;
    unsigned owner = list[i].owner, size = list[i].size, avail = size, nxt = seg - 1 + size + 1;
    for (j = 0; j < n && list[j].seg != nxt; j++);
    if (j < n && list[j].owner == 0) { avail += list[j].size + 1; kind = list[j].kind; }
    if (want > avail) return 0;
    if (want < avail) { mcb(p, seg - 1, 'M', owner, want, "GAME"); free_mcb(p, seg + want, kind, avail - want - 1); }
    else mcb(p, seg - 1, kind, owner, want, "GAME");
    return 1;
}
/* INT 21h 48h, first fit: the segment, or 0 and the largest */
static unsigned dos_alloc(Program *p, unsigned want, unsigned *largest) {
    Block list[64];
    int n = chain(p, list, 64);
    unsigned best = 0;
    for (int i = 0; i < n; i++)
        if (list[i].owner == 0) {
            if (list[i].size >= want) {
                if (list[i].size > want) { mcb(p, list[i].seg, 'M', PSP, want, "GAME"); free_mcb(p, list[i].seg + want + 1, list[i].kind, list[i].size - want - 1); }
                else mcb(p, list[i].seg, list[i].kind, PSP, want, "GAME");
                return list[i].seg + 1;
            }
            if (list[i].size > best) best = list[i].size;
        }
    if (largest) *largest = best;
    return 0;
}
static void get_vector(Program *p, unsigned n, unsigned *seg, unsigned *off) { *seg = w(p, 0, 4 * n + 2); *off = w(p, 0, 4 * n); }
static void set_vector(Program *p, unsigned n, unsigned seg, unsigned off) { sw(p, 0, 4 * n, off); sw(p, 0, 4 * n + 2, seg); }
static int critical(Program *p, const char *name, unsigned call) {
    char base[64];
    base_name(name, base, sizeof base);
    for (int i = 0; i < p->mc->ncritical; i++) if (!strcmp(base, p->mc->critical[i])) return 1;
    for (int i = 0; i < p->mc->ncritical_calls; i++) if (!strcmp(base, p->mc->critical_name[i]) && p->mc->critical_calls[i] & call) return 1;
    return 0;
}
static MachineFile *written(Program *p, const char *base) {
    for (int i = 0; i < p->mc->nwritten; i++) if (!strcmp(base, p->mc->written[i].name)) return &p->mc->written[i];
    return 0;
}
/* the bytes of a file (malloc'd), or 0: missing, or a critical error (INT 24h: 0cd7 fails the call) */
static uint8_t *file(Program *p, const char *name, unsigned call, long long *n) {
    if (critical(p, name, call)) return 0;
    char base[64], path[1200];
    base_name(name, base, sizeof base);
    MachineFile *f = written(p, base);
    if (f) { uint8_t *copy = malloc(f->n ? f->n : 1); memcpy(copy, f->data, f->n); *n = f->n; return copy; }
    snprintf(path, sizeof path, "%s/%s", p->mc->disks[p->disk], base);
    if (file_size(path) < 0) return 0;
    return read_path(path, n);
}
/* the local date and time of the file, as DOS gives them */
static void file_time(Program *p, const char *name, unsigned *dtime, unsigned *ddate) {
    char base[64], path[1200];
    base_name(name, base, sizeof base);
    if (written(p, base)) { *dtime = 0; *ddate = (1990 - 1980) << 9 | 7 << 5 | 29; return; }   /* any fixed value */
    snprintf(path, sizeof path, "%s/%s", p->mc->disks[p->disk], base);
    struct stat st;
    stat(path, &st);
    struct tm t;
    localtime_r(&st.st_mtime, &t);
    *dtime = (unsigned)(t.tm_hour << 11 | t.tm_min << 5 | t.tm_sec / 2);
    *ddate = (unsigned)((t.tm_year + 1900 - 1980) << 9 | (t.tm_mon + 1) << 5 | t.tm_mday);
}
/* INT 21h 4Eh: the DTA gets the attribute, the date and time of the file,
 * its size and name (DOS's search state: zeros); the size or -1 */
static long long find_first(Program *p, const char *name) {
    long long n;
    uint8_t *data = file(p, name, CALL_FIND, &n);
    if (!data) return -1;
    free(data);
    unsigned dtime, ddate;
    file_time(p, name, &dtime, &ddate);
    uint8_t entry[21 + 1 + 8 + 13] = {0};
    entry[21] = 0x20;
    put(entry, 22, dtime); put(entry, 24, ddate);
    entry[26] = u8(n); entry[27] = u8(n >> 8); entry[28] = u8(n >> 16); entry[29] = u8(n >> 24);
    const char *base = strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;
    for (int i = 0; base[i] && i < 13; i++) entry[30 + i] = (uint8_t)(base[i] >= 'a' && base[i] <= 'z' ? base[i] - 32 : base[i]);
    write_(p, p->dta_seg * 16u + p->dta_off, entry, sizeof entry);
    return n;
}
static void dos_write(Program *p, const uint8_t *data, int n) { for (int i = 0; i < n; i++) PUSH(p->output, data[i]); }

/* ---- the library's memory manager (0434:031a, 033e, 0383, 038e, 0435) -------------- */
static void arena_add(Program *p, unsigned ax, unsigned bx) {   /* 033e */
    bx = (bx - 1) & 0xffff;
    sd(p, 0x140, (d(p, 0x140) + bx) & 0xffff); sd(p, 0x144, (d(p, 0x144) + 1) & 0xffff);
    unsigned es = d(p, 0x13c), dx = w(p, es, 0x10);
    sw(p, ax, 0x0c, 0x464d); sw(p, ax, 0x0e, bx);
    unsigned end = (bx + ax) & 0xffff;
    sw(p, end, 0x0c, 0x4d46); sw(p, end, 0x0a, ax);
    sw(p, ax, 0x10, dx); sw(p, ax, 0x12, es); sw(p, es, 0x10, ax); sw(p, dx, 0x12, ax);
}
static void arena_init(Program *p, unsigned ax, unsigned bx) {
    sd(p, 0x140, 0); sd(p, 0x144, 0); sd(p, 0x142, 0); sd(p, 0x13c, ax); sd(p, 0x13e, ax);
    sw(p, ax, 0x10, ax); sw(p, ax, 0x12, ax);
    arena_add(p, ax, bx);
}
static void exit_(Program *p, unsigned al, unsigned dx);
/* 038e: next fit from the rover, the block taken from the top of a free
 * block. DX = the paragraphs + 1, BP = 4D46h when it splits a block (they
 * matter once: task_create). No block: "Out of memory" (0434:042a). */
static unsigned alloc(Program *p, unsigned paragraphs) {
    unsigned dx = (paragraphs + 1) & 0xffff, ds = d(p, 0x13e), cx = w(p, ds, 0x0e);
    p->reg_dx = u16(dx);
    if (cx < dx) {
        unsigned ax = w(p, ds, 0x0e), rover = ds;
        sw(p, ds, 0x0e, 0xffff);
        for (;;) {
            ds = w(p, ds, 0x10);
            if (w(p, ds, 0x0e) >= dx) break;
        }
        cx = w(p, ds, 0x0e); sw(p, rover, 0x0e, ax);
        if (cx == 0xffff) exit_(p, 1, 0x146);
    }
    unsigned es = (ds + cx) & 0xffff;
    sb(p, es, 0x0c, 0x4d);
    unsigned rest = (cx - dx) & 0xffff;
    if (rest == 0) {
        sb(p, ds, 0x0d, 0x4d);
        unsigned bx = w(p, ds, 0x10), ax = w(p, ds, 0x12);
        sw(p, bx, 0x12, ax); sw(p, ax, 0x10, bx);
        sd(p, 0x13e, bx); sd(p, 0x140, (d(p, 0x140) - dx) & 0xffff);
        sd(p, 0x144, (d(p, 0x144) - 1) & 0xffff); sd(p, 0x142, (d(p, 0x142) + 1) & 0xffff);
        return (ds + 1) & 0xffff;
    }
    sw(p, ds, 0x0e, rest);
    unsigned blk = (ds + rest) & 0xffff;
    p->reg_bp = 0x4d46;
    sw(p, blk, 0x0a, ds); sw(p, blk, 0x0c, 0x4d46); sw(p, blk, 0x0e, dx);
    sd(p, 0x13e, ds); sd(p, 0x140, (d(p, 0x140) - dx) & 0xffff); sd(p, 0x142, (d(p, 0x142) + 1) & 0xffff);
    return (blk + 1) & 0xffff;
}
static unsigned alloc_bytes(Program *p, unsigned n) { return alloc(p, ((n + 3) & 0xffff) >> 4); }   /* 0383 */
static void free_(Program *p, unsigned seg) {   /* 0435 */
    unsigned ds = (seg - 1) & 0xffff, dx = w(p, ds, 0x0e);
    if (b(p, ds, 0x0c) != 0x4d) {
        unsigned bx = w(p, ds, 0x0a), es = (ds + dx) & 0xffff;
        if (b(p, es, 0x0d) != 0x4d) {
            unsigned ax = (w(p, es, 0x0e) + dx + w(p, bx, 0x0e)) & 0xffff;
            sw(p, bx, 0x0e, ax);
            unsigned after = (ax + bx) & 0xffff;
            sw(p, after, 0x0a, bx);
            unsigned nxt = w(p, es, 0x10), prv = w(p, es, 0x12);
            sw(p, nxt, 0x12, prv); sw(p, prv, 0x10, nxt);
            sd(p, 0x13e, nxt); sd(p, 0x140, (d(p, 0x140) + dx) & 0xffff);
            sd(p, 0x144, (d(p, 0x144) - 1) & 0xffff); sd(p, 0x142, (d(p, 0x142) - 1) & 0xffff);
            return;
        }
        unsigned ax = (w(p, bx, 0x0e) + dx) & 0xffff;
        sw(p, bx, 0x0e, ax);
        unsigned after = (ax + bx) & 0xffff;
        sw(p, after, 0x0a, bx); sb(p, after, 0x0c, 0x46);
        sd(p, 0x13e, bx); sd(p, 0x140, (d(p, 0x140) + dx) & 0xffff); sd(p, 0x142, (d(p, 0x142) - 1) & 0xffff);
        return;
    }
    sb(p, ds, 0x0d, 0x46);
    unsigned es = (ds + dx) & 0xffff;
    if (b(p, es, 0x0d) == 0x46) {
        unsigned nxt = w(p, es, 0x10), prv = w(p, es, 0x12);
        sw(p, ds, 0x10, nxt); sw(p, ds, 0x12, prv);
        unsigned cx = (w(p, es, 0x0e) + w(p, ds, 0x0e)) & 0xffff;
        sw(p, ds, 0x0e, cx);
        sw(p, nxt, 0x12, ds); sw(p, prv, 0x10, ds); sw(p, (cx + ds) & 0xffff, 0x0a, ds);
        sd(p, 0x13e, ds); sd(p, 0x140, (d(p, 0x140) + dx) & 0xffff); sd(p, 0x142, (d(p, 0x142) - 1) & 0xffff);
        return;
    }
    sb(p, es, 0x0c, 0x46); sw(p, es, 0x0a, ds);
    unsigned base = d(p, 0x13c), bx = w(p, base, 0x10);
    sw(p, ds, 0x10, bx); sw(p, ds, 0x12, base); sw(p, base, 0x10, ds); sw(p, bx, 0x12, ds);
    sd(p, 0x13e, ds); sd(p, 0x140, (d(p, 0x140) + dx) & 0xffff);
    sd(p, 0x144, (d(p, 0x144) + 1) & 0xffff); sd(p, 0x142, (d(p, 0x142) - 1) & 0xffff);
}

/* ---- the measurements, the PIT, the vectors ---------------------------------------- */
static void measure(Program *p, const unsigned *offsets, int n) {
    for (int i = 0; i < n; i++)
        for (int j = 0; j < p->mc->nmeasure; j++)
            if (p->mc->measure_at[j] == offsets[i]) DS[offsets[i]] = p->mc->measure_value[j];
}
static void pit(Program *p, unsigned divisor) {   /* 02ec / 09b5: mode 3 on channel 0 */
    out(p, 0x43, 0x36); out(p, 0x40, divisor & 255); out(p, 0x40, (divisor >> 8) & 255);
}
static void calibrate_cpu(Program *p) {   /* 021b */
    static const unsigned at[1] = {0x10};
    pit(p, 0xffff);
    out(p, 0x43, 0xff);   /* 0303 with AL = FFh (from the delay loop): read-back */
    pit(p, 0);
    measure(p, at, 1);
}
static void measure_frame(Program *p) {   /* 093c */
    static const unsigned at[4] = {0x6b8, 0x6ba, 0x6bb, 0x6bc};
    pit(p, 0xffff); out(p, 0x43, 0x36);   /* 09b5 and 09c8 around a frame */
    measure(p, at, 4);
}
static void swap_vector(Program *p, unsigned entry) {   /* 0291 */
    unsigned n = DS[entry], seg, off;
    get_vector(p, n, &seg, &off);
    set_vector(p, n, d(p, entry + 3), d(p, entry + 1));
    sd(p, entry + 1, off); sd(p, entry + 3, seg);
}
static void swap_vectors(Program *p) {   /* 0251: INT 00..03 */
    static const unsigned entries[4] = {0x107, 0x10c, 0x111, 0x116};
    for (int i = 0; i < 4; i++) swap_vector(p, entries[i]);
}
static void set_timer(Program *p, unsigned record) {   /* 02d4 (136h) and 02d9 (130h): INT 08 and the PIT */
    set_vector(p, 8, d(p, record + 2), d(p, record)); pit(p, d(p, record + 4));
}
static void bios_tick(Program *p) { p->M[0x440] = 0; }   /* 0434:0cef */

/* ---- the devices (019f, 01a4, 01c2, 01f0, 01f5) -------------------------------------- */
static void routine(Program *p, unsigned record, unsigned offset);
/* 019f (start, +2) and 01a4 (back from DOS, +0e): every record, its
 * dependencies first; SP is followed because two routines of the start store
 * it (the overlay load, the creation of a task) */
static void device(Program *p, unsigned si, unsigned sp, unsigned offset) {
    if (d(p, si) & 0x8000) return;
    sp -= 2;
    unsigned at = si + 0x12;
    while (d(p, at) != 0xffff) { device(p, d(p, at), sp - 2 - 2, offset); at += 2; }
    p->callback_sp = u16(sp + 2 - 6 - 4);   /* 01dd pops SI, pushes DI, SI, BX, calls far */
    routine(p, si, offset);
    sd(p, p->di, si); p->di += 2; sd(p, si, 0x8000);
}
static void devices_start(Program *p, unsigned offset, unsigned sp) {
    unsigned si = 0xcc0e;
    p->di = 0xcc8e;
    while (d(p, si)) {
        device(p, si, sp - 2, offset);
        si += 0x12;
        while (d(p, si) != 0xffff) si += 2;
        si += 2;
    }
}
/* 01f0 (end, +6) and 01f5 (out to DOS, +0a): the started records, the last first, each with the state 1 */
static void devices_stop(Program *p, unsigned offset) {
    unsigned started[64];
    int n = 0;
    for (unsigned si = 0xcc8e; d(p, si) && n < 64; si += 2) started[n++] = d(p, si);
    for (int i = n - 1; i >= 0; i--) { sd(p, started[i], 1); routine(p, started[i], offset); }
}
static void keyboard_start(Program *p) {   /* 0658 */
    swap_vector(p, 0x6b1);
    sd(p, 0x67e, w(p, 0x40, 0x17)); out(p, 0x201, b(p, 0x40, 0x17));   /* (OUT 201h with AL: a trigger) */
    memset(DS + 0x624, 0, 16); sd(p, 0x674, 0);
}
static void keyboard_stop(Program *p) { swap_vector(p, 0x6b1); sw(p, 0x40, 0x17, d(p, 0x67e)); }   /* 0683 */
static void critical_vector(Program *p) { swap_vector(p, 0x732); }   /* 0cce */
static void critical_back(Program *p) { p->dta_seg = u16(d(p, 0x730)); p->dta_off = 0; critical_vector(p); }   /* 0cc1 */
static void critical_start(Program *p) { sd(p, 0x730, alloc_bytes(p, 0x80)); critical_back(p); }   /* 0cb6 */
static void sound_start(Program *p) {   /* 3a20 */
    sd(p, 0x9d01, d(p, 0x6bd)); sd(p, 0x9d03, d(p, 0x6bf));
    sd(p, 0x6bd, 0x3a52); sd(p, 0x6bf, CODE);
}
static void sound_out(Program *p) { sound_request(p, SND_MUSIC_OFF, 0); }   /* 3a4e: 3b12, everything off */
static void sound_end(Program *p) {   /* 3a3e */
    sd(p, 0x6bd, d(p, 0x9d01)); sd(p, 0x6bf, d(p, 0x9d03));
    sound_out(p);
}
static void timer_start(Program *p) {   /* 0918 */
    measure_frame(p);
    sd(p, 0x136, 0x0a44); sd(p, 0x138, LIB); sd(p, 0x13a, d(p, 0x6ba));
    set_timer(p, 0x136);
}
static void timer_out(Program *p) { set_timer(p, 0x130); }   /* 0936 */
static uint32_t clock_(Program *p);
static void timer_end(Program *p) { set_timer(p, 0x130); clock_(p); }   /* 092d */
/* 09db: the BIOS clock gets the time the game kept: counter x (1234DCh /
 * frame rate) / 10000h ticks, days carried as DOS learns them (the midnight
 * flag, INT 21h 2Ch), the counter back to 0 */
static uint32_t clock_(Program *p) {
    if (!DS[0x6bc]) not_modelled(p);   /* a division by zero */
    unsigned long long bx = 0x1234dc / DS[0x6bc];
    uint32_t counter = d(p, 0x6c1) | (uint32_t)d(p, 0x6c3) << 16;
    unsigned long long low = (d(p, 0x6c1) * bx) >> 16, high = d(p, 0x6c3) * bx;
    uint32_t ticks = (uint32_t)(low + high);
    sd(p, 0x6c1, 0); sd(p, 0x6c3, 0);
    ticks += w(p, 0x40, 0x6c) | (uint32_t)w(p, 0x40, 0x6e) << 16;
    int days = 0;
    for (;;) {
        days++;
        if (ticks < 0x1800b0) break;
        ticks -= 0x1800b0;
    }
    sw(p, 0x40, 0x6c, ticks & 0xffff); sw(p, 0x40, 0x6e, ticks >> 16);
    for (int i = 0; i < days - 1; i++) { sb(p, 0x40, 0x70, 1); PUSH(p->dos, 0x2c); sb(p, 0x40, 0x70, 0); }   /* DOS reads the flag */
    return counter;
}
/* the registers 0434:0b3c pushes for the new task */
static unsigned flags_after_sub(unsigned a, unsigned bb) {
    unsigned r = (a - bb) & 0xffff, f = 0x7202;   /* DOSBox-X: IOPL 3 and NT set; IF; bit 1 */
    if (a < bb) f |= 1;
    if (__builtin_popcount(r & 255) % 2 == 0) f |= 4;
    if ((a & 15) < (bb & 15)) f |= 0x10;
    if (r == 0) f |= 0x40;
    if (r & 0x8000) f |= 0x80;
    if (((a ^ bb) & (a ^ r)) & 0x8000) f |= 0x800;
    return f;
}
/* 0af1: a task: its stack in the arena with the frame of its first switch,
 * and its place in the list by priority */
static void task_create(Program *p, unsigned si) {
    unsigned paragraphs = d(p, si + 0x12), es = alloc(p, paragraphs), top = (paragraphs << 4) & 0xffff;
    /* 0a96 calls 0af1 (4); 0af1 pushes SI (2) around the allocation, then ES, BX (4) */
    sd(p, 0x722, (p->callback_sp - 4 - 4) & 0xffff); sd(p, 0x724, p->ss);
    sw(p, es, top - 2, LIB); sw(p, es, top - 4, 0x0b83);
    sw(p, es, top - 8, d(p, si + 0xc)); sw(p, es, top - 0xa, d(p, si + 0xa));
    unsigned bx = (top - 0xa) & 0xffff;
    unsigned regs[9] = {DATA, bx, d(p, si + 0xa), p->reg_dx, si, p->di, p->reg_bp, DATA, es};   /* AX BX CX DX SI DI BP DS ES */
    unsigned sp = bx;
    for (int i = 0; i < 9; i++) { sp = (sp - 2) & 0xffff; sw(p, es, sp, regs[i]); }
    sd(p, si + 0x14, sp); sd(p, si + 0x16, es);
    sw(p, es, top - 6, flags_after_sub((d(p, si + 0x12) << 4) & 0xffff, 0xa));
    sd(p, 0x720, si);
    unsigned ax = d(p, si + 4), di = 0x710;
    for (;;) {
        di = d(p, di);
        if (!(ax <= d(p, di + 4))) break;
    }
    unsigned bx2 = d(p, di + 2);
    sd(p, si, di); sd(p, si + 2, bx2); sd(p, di + 2, si); sd(p, bx2, si);
}
static void tasks_vector(Program *p) { swap_vector(p, 0x72a); }   /* 0ae0 */
static void tasks_start(Program *p) {   /* 0a96 */
    sd(p, 0x6c6, 0x716); sd(p, 0x6c8, 0x710);
    task_create(p, IDLE_TASK);
    swap_vector(p, 0x72a);
    sd(p, 0x71c, d(p, 0x6bd)); sd(p, 0x71e, d(p, 0x6bf));
    sd(p, 0x6bd, 0x0c48); sd(p, 0x6bf, LIB);
}
static void tasks_end(Program *p) {   /* 0ad0 */
    sd(p, 0x6bd, d(p, 0x71c)); sd(p, 0x6bf, d(p, 0x71e));
    tasks_vector(p);
}

/* ---- the graphics driver (056a:000a, 007a, 0089, 0098) --------------------------------- */
static GfxRegs regs_of(void) { GfxRegs r = {0}; r.ds = DATA; r.ss = DATA + 0xcec; return r; }
static GfxRegs call(Program *p, unsigned entry, GfxRegs r) {
    if (!p->video) not_modelled(p);
    if (gfx_call(p->video, entry, &r)) raise_(p, p->video->error ? p->video->error : OFF_NOT_MODELLED);
    return r;
}
static void module(Program *p, unsigned entry) {
    if (!p->video) return;
    GfxRegs r = regs_of();
    r.bp = DATA; r.di = 0x6bd;
    call(p, entry, r);
}
/* 0434:1200: the command tail copied to DS:0a26 up to CR (or 1Ah), split into
 * words at space, comma and brackets, their offsets at DS:09e4 (at most 32),
 * a zero after the last; the number of words */
static int kind_of(unsigned c) { return c == 0x0d || c == 0x1a ? 2 : (c == 0x20 || c == 0x2c || c == 0x28 || c == 0x29); }
static int command_line(Program *p, unsigned *first) {
    uint32_t src = PSP * 16 + 0x81;
    unsigned dst = 0xa26, cx = 0x100, al;
    for (;;) {
        al = p->M[src]; DS[dst] = u8(al); src++; dst++; cx--;
        if (al == 0x0d || al == 0x1a || cx == 0) break;
    }
    if (cx == 0 && al != 0x0d && al != 0x1a) return 0;
    unsigned si = 0xa26, bx = 0x9e4;
    int words = 0;
    for (;;) {
        while (kind_of(DS[si]) == 1) si++;
        if (kind_of(DS[si]) == 2) break;
        sd(p, bx, si); bx += 2;
        if (!words) *first = si;
        words++;
        si++;
        while (kind_of(DS[si]) == 0) si++;
        if (kind_of(DS[si]) == 2) break;
        DS[si] = 0; si++;
        if (words == 0x20) { si--; break; }
    }
    DS[si] = 0; sd(p, bx, 0);
    return words;
}
/* 0434:127a: the word against the table at DS:DI (cb75 for the video modes:
 * names, a zero, the code), capitals folded to lower case; the code or -1 */
static int option(Program *p, unsigned si, unsigned di) {
    for (;;) {
        if (DS[di] & 0x80) return -1;
        unsigned kk = 0;
        for (;;) {
            unsigned a = DS[(si + kk) & 0xffff], t = DS[(di + kk) & 0xffff];
            if ((a | t) == 0) return DS[(di + kk + 1) & 0xffff];
            if (t == 0) break;
            if (a >= 0x41 && a <= 0x5a) a |= 0x20;
            if (a != t) break;
            kk++;
        }
        while (DS[di]) di++;
        di += 2;
    }
}
static void text_at(Program *p, unsigned off, char *out_, int cap) {
    int n = 0;
    while (DS[(off + n) & 0xffff] && n < cap - 1) { out_[n] = (char)DS[(off + n) & 0xffff]; n++; }
    out_[n] = 0;
}
static void string_at(Program *p, unsigned seg, unsigned off, char *out_, int cap) {
    int n = 0;
    while (b(p, seg, off + n) && n < cap - 1) { out_[n] = (char)b(p, seg, off + n); n++; }
    out_[n] = 0;
}
static unsigned load_overlay(Program *p, const char *name);
static void divide_error(Program *p);
static void graphics_start(Program *p) {
    unsigned first = 0;
    if (!command_line(p, &first)) exit_(p, 1, 0xcba4);   /* 0434:1200 */
    int code = option(p, first, 0xcb75);                  /* 0434:127a */
    if (code < 0) exit_(p, 1, 0xcbfa);
    char name[64];
    text_at(p, d(p, 0xcb6b + 2 * code), name, sizeof name);
    unsigned seg = load_overlay(p, name);                 /* 0434:0e37 */
    sd(p, 0xcb4e, seg); DS[0xcb50] = u8(code);
    unsigned work = alloc(p, d(p, 0xcb9a + 2 * code));
    if (seg == 0) {
        /* the module is not there: the far call goes through the pointer at
         * 0000:0000, the vector of INT 00, the game's own handler since 0251
         * (0434:0272): "Divide by zero error" */
        divide_error(p);
    }
    p->video = calloc(1, sizeof *p->video);
    gfx_init(p->video, p->M, DATA);
    p->video->mode = 3; p->video->page_number = 0;
    GfxRegs r = regs_of();
    r.ax = u16(work); r.bx = u16(code); r.bp = DATA; r.di = 0x6bd; r.dx = u16(d(p, 0x0e));
    call(p, 0x00, r);
    measure_frame(p);
}
static void graphics_end(Program *p) { module(p, 0x04); }   /* 007a */
static void graphics_out(Program *p) { module(p, 0x08); }   /* 0089 */
static void graphics_back(Program *p) { module(p, 0x0c); measure_frame(p); }   /* 0098 */

static void routine(Program *p, unsigned record, unsigned offset) {
    unsigned seg = d(p, record + offset + 2), off = d(p, record + offset);
#define AT(s, o) (seg == (s) && off == (o))
    if (AT(LIB, 0x0658)) keyboard_start(p);
    else if (AT(LIB, 0x0683)) keyboard_stop(p);
    else if (AT(LIB, 0x0918)) timer_start(p);
    else if (AT(LIB, 0x092d)) timer_end(p);
    else if (AT(LIB, 0x0936)) timer_out(p);
    else if (AT(LIB, 0x0a96)) tasks_start(p);
    else if (AT(LIB, 0x0ad0)) tasks_end(p);
    else if (AT(LIB, 0x0ae0)) tasks_vector(p);
    else if (AT(LIB, 0x0cb6)) critical_start(p);
    else if (AT(LIB, 0x0cc1)) critical_back(p);
    else if (AT(LIB, 0x0cce)) critical_vector(p);
    else if (AT(CODE, 0x3a20)) sound_start(p);
    else if (AT(CODE, 0x3a3e)) sound_end(p);
    else if (AT(CODE, 0x3a4e)) sound_out(p);
    else if (AT(CODE, 0x3a51)) {}
    else if (AT(DRV, 0x000a)) graphics_start(p);
    else if (AT(DRV, 0x007a)) graphics_end(p);
    else if (AT(DRV, 0x0089)) graphics_out(p);
    else if (AT(DRV, 0x0098)) graphics_back(p);
    else not_modelled(p);   /* the model's key error */
#undef AT
}

/* 0434:0e37: find the file (its size), a block of size/16 + 1 paragraphs,
 * INT 21h 4B03h with the block as load segment and relocation factor. Not
 * found: BX as 02d4 leaves it, [013a]: 0 before the timer starts. */
static unsigned load_overlay(Program *p, const char *name) {
    set_timer(p, 0x130);
    long long size = find_first(p, name);
    if (size < 0) { bios_tick(p); set_timer(p, 0x136); return d(p, 0x13a); }
    unsigned seg = alloc(p, ((size >> 4) + 1) & 0xffff);
    sw(p, LIB, 0xe33, seg); sw(p, LIB, 0xe35, seg);
    /* SP at 0e76: the driver pushes the option (2) and calls 0e37 (4), which
     * pushes DS, ES, DI (6), pops ES, DI (4), pushes CX, DX, ES, DI (8), pops
     * DX, DS (4) and pushes AX (2) */
    sw(p, LIB, 0xe2f, (p->callback_sp - 2 - 4 - 6 + 4 - 8 + 4 - 2) & 0xffff); sw(p, LIB, 0xe31, p->ss);
    long long n;
    uint8_t *raw = file(p, name, CALL_EXEC, &n);
    if (!raw) {   /* 0ea9: the block freed; BX as 02d4 leaves it */
        free_(p, seg); bios_tick(p); set_timer(p, 0x136);
        return d(p, 0x13a);
    }
    if (n >= 2 && raw[0] == 'M' && raw[1] == 'Z') {
        unsigned hdr = W(raw, 8) * 16, last = W(raw, 2), pages = W(raw, 4);
        long long end = (long long)pages * 512 - (last ? 512 - last : 0);
        if (end > n) end = n;
        long long body = end > (long long)hdr ? end - hdr : 0;
        uint8_t *image = malloc(body ? body : 1);
        memcpy(image, raw + hdr, body);
        unsigned count = W(raw, 6), table = W(raw, 0x18);
        for (unsigned i = 0; i < count; i++) {
            unsigned off = W(raw, table + 4 * i), s = W(raw, table + 4 * i + 2);
            unsigned long long at = s * 16ul + off;
            if ((long long)at + 2 > body) { free(image); free(raw); not_modelled(p); }
            put(image, at, W(image, at) + seg);
        }
        write_(p, seg * 16u, image, body);
        free(image);
    } else write_(p, seg * 16u, raw, n);
    free(raw);
    bios_tick(p); set_timer(p, 0x136);
    return seg;
}

/* ---- files (0434:0d03, 0d2a, 0d98, 0dde) ------------------------------------------- */
/* 0d2a: the file read in 32 KB pieces from segment:0 on; the bytes read kept
 * in 0434:0d26 (32 bits). -1 if it is not there. */
static long long load_into(Program *p, const char *name, unsigned segment) {
    set_timer(p, 0x130);
    long long n;
    uint8_t *data = file(p, name, CALL_OPEN, &n);
    if (!data) { bios_tick(p); set_timer(p, 0x136); return -1; }
    write_(p, segment * 16u, data, n);
    free(data);
    sw(p, LIB, 0xd26, n & 0xffff); sw(p, LIB, 0xd28, (n >> 16) & 0xffff);
    bios_tick(p); set_timer(p, 0x136);
    return n;
}
static void disk_request(Program *p, unsigned sp);
/* 1193 (expected: the size of DS:4b29) and 11c7 (-1): the file into the
 * file buffer (the stack segment, SS:0000) until it is there */
static long long load_buffer(void *ctx, const char *name, long long expected, unsigned sp) {
    Program *p = ctx;
    for (;;) {
        long long got = load_into(p, name, p->ss);
        if (got >= 0 && (expected < 0 || (got & 0xffff) == expected)) return got;
        disk_request(p, sp);
    }
}
/* 0d98: find the file, a block of size/16 + 1 paragraphs, the file into it;
 * the segment (and the size), or 0. Not found: the BIOS timer stays on INT 08
 * (0d98 put it there and only 0d2a puts the game's back). */
static unsigned load_new(Program *p, const char *name, long long *size_out) {
    set_timer(p, 0x130);
    long long size = find_first(p, name);
    if (size < 0) return 0;
    unsigned seg = alloc(p, ((size >> 4) + 1) & 0xffff);
    if (load_into(p, name, seg) < 0) { free_(p, seg); return 0; }
    if (size_out) *size_out = size;
    return seg;
}
static void write_file(Program *p, const char *name, unsigned segment, unsigned count) {   /* 0dde: create, write, close */
    set_timer(p, 0x130);
    if (!critical(p, name, CALL_CREATE)) {   /* (0e20: a failure only ends it) */
        char base[64];
        base_name(name, base, sizeof base);
        MachineFile *f = written(p, base);
        if (!f) {
            if (p->mc->nwritten == 16) not_modelled(p);
            f = &p->mc->written[p->mc->nwritten++];
            snprintf(f->name, sizeof f->name, "%s", base);
            f->data = 0; f->owned = 0;
        }
        if (f->owned) free(f->data);
        f->data = malloc(count ? count : 1); f->n = count; f->owned = 1;
        memcpy(f->data, p->M + segment * 16u, count);
        if (p->host.saved) p->host.saved(p->host.ctx, base, f->data, f->n);
    }
    if (!p->host.live) PUSH(p->dos, 0x3c);
    bios_tick(p); set_timer(p, 0x136);
}
/* 0000:115a: a list of (word, name) entries until a word that is not zero:
 * each file into a new block, its segment into the word. A file missing:
 * with AL = 0 the end (11ef), else the disk request and again. */
static void load_list(Program *p, unsigned es, unsigned di, unsigned al, unsigned sp) {
    DS[0x4aa4] = u8(al);
    while (!w(p, es, di)) {
        unsigned got;
        for (;;) {
            char name[64];
            string_at(p, es, di + 2, name, sizeof name);
            got = load_new(p, name, 0);
            if (got) break;
            if (!DS[0x4aa4]) exit_(p, 1, 0x4bb1);
            disk_request(p, sp - 4);   /* 117b, after PUSH ES, DI */
        }
        sw(p, es, di, got); di += 2;
        while (b(p, es, di)) di++;
        di++;
    }
}
/* 0000:1193: file n of the list at DS:4aa7 into the file buffer, its size
 * checked against DS:4b29; the disk request and again until both hold */
static void load_picture(Program *p, unsigned n, unsigned sp) {
    char name[64];
    text_at(p, d(p, 0x4aa7 + 2 * n), name, sizeof name);
    unsigned expected = d(p, 0x4b29 + 2 * n);
    for (;;) {
        long long got = load_into(p, name, p->ss);
        if (got >= 0 && (got & 0xffff) == expected) break;
        disk_request(p, sp - 6);   /* 11bf, after PUSH ES, DI, AX */
    }
    sd(p, 0x4aa5, p->ss);
}
/* ---- sprites (0000:1038) ------------------------------------------------------------ */
/* every file of the module's list: a block for the converted sprites, the
 * file in a block of its own, each sprite through the module's converter to
 * the next place, its descriptor pointed at it, the file's block freed; the
 * converter's far pointer stays in 0000:1034 */
static void sprites(Program *p, unsigned al, unsigned es, unsigned di, unsigned table, unsigned sp) {
    static uint8_t converted[0x40000];
    DS[0x4aa4] = u8(al); sd(p, 0x4a9a, table); sd(p, 0x4aa2, 0);
    for (;;) {
        sd(p, 0x4a96, di); sd(p, 0x4a98, es);
        char name[64];
        string_at(p, es, di, name, sizeof name);
        di += (unsigned)strlen(name) + 1;
        unsigned dest = alloc(p, w(p, es, di));
        sd(p, 0x4a9c, 0); sd(p, 0x4a9e, dest);
        unsigned got;
        for (;;) {
            got = load_new(p, name, 0);
            if (got) break;
            if (!DS[0x4aa4]) exit_(p, 1, 0x4bb1);
            disk_request(p, sp);   /* 1076 */
        }
        sd(p, 0x4aa0, got); di += 2;
        for (;;) {
            unsigned off = w(p, es, di + 3), seg = w(p, es, di + 5);
            sw(p, CODE, 0x1034, off); sw(p, CODE, 0x1036, seg);
            unsigned count = w(p, es, di), flags = b(p, es, di + 2);
            unsigned planes = (unsigned)__builtin_popcount(flags & 0x3f) + ((flags >> 6) & 1);
            for (unsigned i = 0; i < count; i++) {
                unsigned bx = (d(p, 0x4aa2) * 8 + d(p, 0x4a9a)) & 0xffff;
                unsigned height = b(p, es, bx + 4), width = b(p, es, bx + 6), row = ((width + 7) & 255) >> 3;
                unsigned src_off = w(p, es, bx), src_seg = w(p, es, bx + 2);
                sw(p, es, bx, d(p, 0x4a9c)); sw(p, es, bx + 2, d(p, 0x4a9e));
                uint32_t src = (src_seg + d(p, 0x4aa0)) * 16u + src_off;
                long long nraw = (long long)row * height * planes;
                if (src > GFX_MEMORY) nraw = 0;
                else if (src + nraw > GFX_MEMORY) nraw = GFX_MEMORY - src;   /* the model's slice */
                long long n = sprite_converted(p->M + src, nraw, row, height, planes, flags, converted, sizeof converted);
                if (n < 0) not_modelled(p);
                uint32_t dst = d(p, 0x4a9e) * 16u + d(p, 0x4a9c);
                write_(p, dst, converted, n);
                unsigned long long end = d(p, 0x4a9c) + n;
                sd(p, 0x4a9c, end & 0xf); sd(p, 0x4a9e, (d(p, 0x4a9e) + (end >> 4)) & 0xffff);
                sd(p, 0x4aa2, d(p, 0x4aa2) + 1);
            }
            di += 7;
            unsigned ax = w(p, es, di);
            if (ax & 0x8000) { free_(p, d(p, 0x4aa0)); return; }
            if (ax == 0) { free_(p, d(p, 0x4aa0)); di += 2; break; }
        }
    }
}

/* ---- the text printer on the program's memory (its glyphs go to the module) --------- */
static void print_text(Program *p, unsigned si) {
    Printer *pr = &p->printer;
    pr->far = p->far; pr->c = 0; pr->error = 0; pr->events.n = 0;
    text_print(pr, DS, si);
    for (int i = 0; i < pr->events.n; i++) {
        GfxCall *e = &pr->events.v[i];
        GfxRegs r = regs_of();
        r.ax = e->y; r.bx = e->x; r.cx = 0x746; r.dx = 0x766; r.si = 0x786; r.bp = e->font; r.di = e->glyph; r.cf = e->carry;
        call(p, 0x70, r);
    }
    pr->events.n = 0;
    if (pr->error) raise_(p, pr->error);
}

/* ---- keys ------------------------------------------------------------------------- */
/* one pass of a loop that waits for a key: the next group of key bytes of
 * the script for it, through the game's INT 9 */
static void keys_at(Program *p, int point) {
    Machine *mc = p->mc;
    KeyGroup live;
    KeyGroup *g = &live;
    if (p->host.wait_keys) live.n = p->host.wait_keys(p->host.ctx, point, live.b, sizeof live.b);   /* the keyboard, as it comes */
    else {
        if (mc->next_key[point] >= mc->nkeys[point]) { p->run->point = point; raise_(p, GAME_WAITING); }
        g = &mc->keys[point][mc->next_key[point]++];
    }
    for (int i = 0; i < g->n;) {
        int j = i;
        while (j < g->n && g->b[j] == 0xf0) j++;
        int count = (j < g->n ? j + 1 : j) - i;
        int r = ctl_int9(DS, g->b + i, count);
        if (r) raise_(p, r);
        i += count;
    }
}
static unsigned key(Program *p, unsigned scan) { return ctl_key_down(DS, scan); }

/* ---- the way out (0434:00a6), the shell (0434:00c8), the faults --------------------- */
static void message(Program *p, unsigned dx) {   /* 018b: up to the zero after the first byte */
    int n = 1;
    while (DS[(dx + n) & 0xffff]) n++;
    for (int i = 0; i < n; i++) PUSH(p->output, DS[(dx + i) & 0xffff]);
}
static void exit_(Program *p, unsigned al, unsigned dx) {
    devices_stop(p, 6);   /* 01f0 */
    swap_vectors(p);      /* 0251 */
    if (al) message(p, dx);
    p->run->exit_code = (int)al;
    raise_(p, GAME_EXIT);
}
static void divide_error(Program *p) { exit_(p, DIVIDE_ERROR, 0x11b); }   /* INT 00: 0434:0272 */
/* 0434:00c8 with SP = sp at its first instruction (EXEC saves it in CS:00c4
 * after pushing DS, ES, BX) */
static void shell(Program *p, unsigned sp) {
    devices_stop(p, 0x0a);   /* 01f5 */
    swap_vectors(p);
    unsigned es = PSP, bx = 0x7a;
    sd(p, bx, w(p, es, 0x2c)); sd(p, bx + 2, 0x78); sd(p, bx + 4, DATA);
    static const unsigned fcb[4][2] = {{6, 0x5c}, {8, 0x5e}, {0xa, 0x6c}, {0xc, 0x6e}};   /* the words of the FCBs */
    for (int i = 0; i < 4; i++) sd(p, bx + fcb[i][0], w(p, es, fcb[i][1]));
    sw(p, LIB, 0xc4, (sp - 6) & 0xffff); sw(p, LIB, 0xc6, p->ss); PUSH(p->dos, 0x4b00);
    if (!p->mc->command_com_returns) {
        sw(p, LIB, 0xc4, (sp - 2) & 0xffff); PUSH(p->dos, 0x4b00);   /* c:\command.com, after PUSH DS */
        message(p, 0xa9); message(p, 0xea);   /* "Can't find COMMAND.COM", "Press any key..." */
        p->mc->bios_keys = 0;                  /* INT 16h: the keys waiting taken, then one more */
    }
    swap_vectors(p);
    devices_start(p, 0x0e, sp - 2);   /* 01a4 (called from 0187) */
}

/* ---- the questions (124a, 3b1a), the sound driver (3a5b), the joystick ------------- */
static void call_ax(Program *p, unsigned entry, unsigned ax) { GfxRegs r = regs_of(); r.ax = u16(ax); call(p, entry, r); }
static void joystick_read_(Program *p) { joystick_read(DS, &p->mc->joystick, port_out, p); }   /* 0888 */
static void calibrate(Program *p, unsigned di) {   /* 07e2 */
    uint8_t old = DS[0x6b0];
    DS[0x6b0] = 0xff;
    joystick_read_(p);
    sd(p, di + 4, d(p, di)); sd(p, di + 6, d(p, di + 2));
    DS[0x6b0] = old;
}
/* 07e2 on both game ports, for a machine whose controller comes later */
void program_joystick_calibrate(Program *p) { calibrate(p, 0x680); calibrate(p, 0x692); }
static void quit_question(Program *p);
static void alt_b_(Program *p, unsigned sp) {   /* 0342; sp: SP at its call */
    if (DS[0x67e] & 8 && key(p, 0x30)) shell(p, sp - 2 - 4);
}
static void joystick_question(Program *p, unsigned sp) {   /* 124a */
    call_ax(p, 0x20, 0xf03); call_ax(p, 0x30, 3); print_text(p, 0x4c3c); call_ax(p, 0x48, 0);
    unsigned al;
    for (;;) {   /* 1272 */
        keys_at(p, POINT_JOYSTICK);
        quit_question(p);
        alt_b_(p, sp);   /* 1275 */
        if (key(p, 0x31)) { al = 0; break; }   /* N */
        if (key(p, 0x15)) { calibrate(p, 0x680); calibrate(p, 0x692); al = 0xff; break; }   /* Y */
    }
    DS[0x6b0] = u8(al);
    call_ax(p, 0x20, 0xf03);
}
static void sound_question(Program *p, unsigned sp) {   /* 3b1a */
    call_ax(p, 0x20, 0xf03); call_ax(p, 0x30, 3); print_text(p, 0x9d05); call_ax(p, 0x48, 0);
    int card;
    for (;;) {   /* 3b42 */
        keys_at(p, POINT_SOUND);
        quit_question(p);
        alt_b_(p, sp);   /* 3b45 */
        if (key(p, 0x02)) { card = 0; break; }
        if (key(p, 0x03)) { card = 1; break; }
        if (key(p, 0x04)) { card = 2; break; }
    }
    DS[0x9cf4] = u8(card);
    call_ax(p, 0x20, 0xf03);
}
static void sound_driver(Program *p, unsigned sp) {   /* 3a5b */
    unsigned card = DS[0x9cf4];
    if (card == 0) return;
    static const unsigned speaker_[5] = {0x3ba7, 0x3bc9, 0x3b9e, 0x3bc9, 0x3bca};   /* 3ba6 is a RET */
    static const unsigned adlib[5] = {0x3cd8, 0x417e, 0x3c91, 0x41d2, 0x3cd1};
    const unsigned *pointers = speaker_;
    if (card != 1) {
        unsigned kk = 4 * DS[0x10];
        sd(p, 0x9ffc, d(p, 0xa000 + kk)); sd(p, 0x9ffe, d(p, 0xa002 + kk));   /* 3c1a */
        Sound s;
        sound_new(p, &s);
        snd_opl(&s, 0x01, 0x20); snd_opl(&s, 0x04, 0xe0); snd_adlib_reset(&s);
        sound_done(p, &s);
        unsigned got;
        for (;;) {   /* 3c45 */
            char name[64];
            text_at(p, 0xa040, name, sizeof name);
            got = load_new(p, name, 0);
            if (got) break;
            disk_request(p, sp - 4);   /* 3c53 (3c1a and 3c45 called) */
        }
        for (unsigned i = 0; i < 0x900; i++) sd(p, 0xa04a + 2 * i, w(p, got, 2 * i));
        free_(p, got);
        pointers = adlib;
    }
    static const unsigned at[5] = {0x9cf9, 0x9cfb, 0x9cfd, 0x9cff, 0x9cf7};
    for (int i = 0; i < 5; i++) sd(p, at[i], pointers[i]);
}
/* Esc held: the palette saved, the page shown filled, palette 3, 'ARE YOU
 * SURE YOU WANT TO EXIT TO DOS (Y/N)', then N: the page and the palette
 * back; Y: the way out with AL = 0 */
static void quit_question(Program *p) {   /* 0358 */
    if (!key(p, 0x01)) return;
    call_ax(p, 0x38, 0); call_ax(p, 0x20, 0xf03); call_ax(p, 0x30, 3); print_text(p, 0x3b6);
    for (;;) {   /* 0389 */
        keys_at(p, POINT_QUIT);
        if (key(p, 0x31)) break;
        if (key(p, 0x15)) exit_(p, 0, 0);
    }
    call_ax(p, 0x58, 0); call_ax(p, 0x3c, 0); call_ax(p, 0x54, 0);
}
static int pause_(Program *p) {   /* 030a: 1 = carry (F10) */
    if (DS[0x1ab] || !key(p, 0x3b)) return 0;
    for (;;) {   /* 031a: F1 let go */
        keys_at(p, POINT_PAUSE);
        if (!key(p, 0x3b)) break;
    }
    for (;;) {   /* 0323 */
        keys_at(p, POINT_PAUSE);
        if (key(p, 0x3b))
            for (;;) {   /* 0337 */
                keys_at(p, POINT_PAUSE);
                if (!key(p, 0x3b)) return 0;
            }
        if (key(p, 0x44)) return 1;
    }
}
static void records_save(Program *p, unsigned al);
static int save_question(Program *p) {   /* 34f6: 1 = carry */
    if (!(DS[0x67e] & 4) || !key(p, 0x1f)) return 0;
    print_text(p, 0x9830);
    for (;;) {   /* 350e */
        keys_at(p, POINT_SAVE);
        if (key(p, 0x31)) return 1;
        if (key(p, 0x15)) { records_save(p, 1); return 1; }
    }
}
/* 11f9: 'PLEASE INSERT DISK 2 AND PRESS SPACE' until space (Esc and Alt+B
 * work meanwhile), then 'LOADING...' and a disk reset (INT 21h 0Dh); the
 * machine's next disk goes in when space is pressed */
static void disk_request(Program *p, unsigned sp) {
    call_ax(p, 0x20, 0xf03); call_ax(p, 0x30, 3); print_text(p, 0x4bcb); call_ax(p, 0x48, 0);
    for (;;) {   /* 1221 */
        keys_at(p, POINT_DISK);
        quit_question(p);
        alt_b_(p, sp - 2);   /* 1224 */
        if (key(p, 0x39)) break;
    }
    if (p->disk + 1 < p->mc->ndisks) p->disk++;
    call_ax(p, 0x20, 0xf03); print_text(p, 0x4c1a); PUSH(p->dos, 0x0d);
}

/* ---- the records (367f, 36b3) --------------------------------------------------------- */
static void records_load(Program *p) {   /* 367f */
    char name[64];
    text_at(p, 0x9cbd, name, sizeof name);
    unsigned seg = load_new(p, name, 0);
    if (!seg) return;
    if (w(p, seg, 0) == 0x4952)
        for (unsigned kk = 0; kk < 0x404; kk++) DS[0x98b9 + kk] = u8(b(p, seg, 2 + kk));
    free_(p, seg);
}
/* 36b3: AL = 0 (the end of a game): only if ironhi.bin is there; AL = 1
 * (Ctrl+S): always. 'RI' and the 404h bytes of DS:98b9 into a block, then to
 * the file (created anew). */
static void records_save(Program *p, unsigned al) {
    char name[64];
    text_at(p, 0x9cbd, name, sizeof name);
    if (!al && find_first(p, name) < 0) return;
    unsigned seg = alloc_bytes(p, 0x404);
    uint8_t data[0x406];
    awards_records_file(DS, data);
    write_(p, seg * 16u, data, sizeof data);
    write_file(p, name, seg, 0x406);
    free_(p, seg);
}

/* ---- the library's start (0434:0004..0086) and the game's (0000:0000..00d9) ------------ */
static void library_start(Program *p) {
    unsigned ss, sp;
    dos_exec(p, &ss, &sp);
    sd(p, 0x0c, PSP);   /* the PSP (ES at the entry) */
    if (p->mc->dos_version < 2) {   /* 0434:008b */
        int n = 0;
        while (DS[0x11 + n] != '$') n++;
        dos_write(p, DS + 0x11, n);
        sw(p, ss, sp - 2, PSP); sw(p, ss, sp - 4, 0);   /* PUSH PSP, PUSH 0, RETF: INT 20h at PSP:0000 */
        p->run->exit_code = 0;
        raise_(p, GAME_EXIT);
    }
    dos_resize(p, PSP, (ss + (sp >> 4) + 1 - PSP) & 0xffff);   /* its failure is not looked at */
    sd(p, 0x0e, w(p, PSP, 2));
    unsigned stack = dos_alloc(p, (0x8200 + 0xf) >> 4, 0);
    if (!stack) {   /* 0434:009a */
        int n = 0;
        while (DS[0x36 + n] != '$') n++;
        dos_write(p, DS + 0x36, n);
        p->run->exit_code = 1;
        raise_(p, GAME_EXIT);
    }
    p->ss = u16(stack); p->sp = 0x8200;
    unsigned largest = 0, arena = dos_alloc(p, 0x4800, &largest), size = 0x4800;
    if (!arena) {
        /* 0434:0064: the largest block; if even that fails, AX holds the error
         * (8) and the arena goes there */
        size = largest; arena = dos_alloc(p, largest, 0);
        if (!arena) arena = 8;
    }
    arena_init(p, arena, size);
    /* no memory after the stack: the arena is the interrupt table or a block
     * of 0 paragraphs; not modelled past this point */
    if (arena == 8 || size == 0) not_modelled(p);
    sd(p, 0x167, (w(p, 0x40, 0x6c) ^ 0x0d0c) & 0xffff);
    calibrate_cpu(p);   /* 021b */
    unsigned seg, off;
    get_vector(p, 8, &seg, &off);   /* 02ab: the BIOS timer */
    sd(p, 0x130, off); sd(p, 0x132, seg); sd(p, 0x134, 0); sd(p, 0x136, off); sd(p, 0x138, seg); sd(p, 0x13a, 0);
    swap_vectors(p);   /* 0251 */
    devices_start(p, 2, 0x8200 - 2);   /* 019f */
}
static void game_start(Program *p) {
    unsigned sp = MAIN_SP - 2;   /* SP in the routines main calls */
    GfxRegs r = call(p, 0x10, regs_of());
    sd(p, 0x1a2, r.di); sd(p, 0x1a4, r.es);
    call_ax(p, 0x30, 0); call_ax(p, 0x1c, 0); call_ax(p, 0x20, 0);
    load_list(p, DATA, 0x16a, 0, sp);
    static const unsigned fonts[2] = {0x16a, 0x178};
    for (int i = 0; i < 2; i++) {   /* 0434:0ec4: fonts, the last first */
        unsigned seg = d(p, fonts[i]), old = d(p, 0x738);
        sd(p, 0x738, seg); sw(p, seg, 4, old); sd(p, 0x73a, seg);
    }
    unsigned es = d(p, 0x1a4), di = d(p, 0x1a2);
    unsigned cseg = w(p, es, di + 8), coff = w(p, es, di + 6);   /* 0434:0ed3: colour schemes */
    sd(p, 0x73e, cseg); sd(p, 0x73c, 0);
    while (w(p, cseg, coff)) {
        unsigned bx = w(p, cseg, coff), old = d(p, 0x73c);
        coff += 2;
        sd(p, 0x73c, bx); sw(p, cseg, bx + 2, old);
    }
    unsigned old = d(p, 0x742);
    sd(p, 0x742, 0x83e8); sd(p, 0x83e8 + 4, old); sd(p, 0x744, 0x83e8);   /* 0434:0ef3 */
    load_picture(p, 10, sp);
    GfxRegs pic = regs_of();
    pic.ax = u16(d(p, 0x4aa5)); pic.si = 4; pic.bx = 0;
    call(p, 0x40, pic);
    call_ax(p, 0x50, 0); call_ax(p, 0x30, 3); call_ax(p, 0x4c, 0);
    es = d(p, 0x1a4); di = d(p, 0x1a2);
    sprites(p, 1, es, w(p, es, di), w(p, es, di + 4), sp);
    load_list(p, es, w(p, es, di + 2), 1, sp);
    load_list(p, DATA, 0x188, 1, sp);
    records_load(p);
    session_drivers(DS);   /* 12ae */
    call_ax(p, 0x30, 0);
    joystick_question(p, sp);
    sound_question(p, sp);
    sound_driver(p, sp);
}

/* ---- time: the timer interrupt and the tasks (0434:0a44, 0bce, 0c48) ----------------------
 * The ideal machine: the program's work takes no time and a tick (IRQ 0)
 * comes only when the CPU has nothing to do: when the idle task runs. The
 * main task gives the CPU away with INT 60h in its waits, the idle task gets
 * it, the tick makes the main task ready again and switches back. */
/* 0bec..0c3b (and 0c5f..0cae): the first task of the list whose wake-up time
 * has come is the current one; if the next has the same priority it goes
 * after all the tasks of that priority */
static unsigned task_next(Program *p) {
    unsigned si = 0x710;
    uint32_t now = d(p, 0x6c1) | (uint32_t)d(p, 0x6c3) << 16;
    for (;;) {
        si = d(p, si);
        if (now >= (d(p, si + 0x20) | (uint32_t)d(p, si + 0x22) << 16)) break;
    }
    sd(p, 0x70e, si); sd(p, 0x726, d(p, si + 0x18));
    unsigned ax = d(p, si + 4), di = d(p, si);
    if (ax > d(p, di + 4)) return si;
    for (;;) {
        di = d(p, di);
        if (ax != d(p, di + 4)) break;
    }
    unsigned bx = d(p, si + 2), nxt = d(p, si);
    sd(p, bx, nxt); sd(p, nxt + 2, bx);
    bx = d(p, di + 2);
    sd(p, si, di); sd(p, si + 2, bx); sd(p, bx, si); sd(p, di + 2, si);
    return si;
}
static void tick(Program *p);
static void task_yield(Program *p, unsigned sp) {   /* INT 60h: 0434:0bce */
    unsigned si = d(p, 0x70e);
    sd(p, si + 0x14, (sp - 6 - 18) & 0xffff); sd(p, si + 0x16, p->ss); sd(p, si + 0x18, d(p, 0x726));
    task_next(p);
    while (d(p, 0x70e) != MAIN_TASK) {   /* another task runs (its loop at 0aef) until a tick */
        if (p->tick_limit >= 0 && p->ticks >= p->tick_limit) { p->run->point = POINT_TICKS; raise_(p, GAME_WAITING); }
        if (p->host.before_tick) p->host.before_tick(p->host.ctx);
        tick(p);
        if (p->host.after_tick) p->host.after_tick(p->host.ctx);
    }
}
/* the waits of the game: the wake-up time = counter + 1 into the current task, then INT 60h */
static void task_wait(void *ctx, unsigned sp) {
    Program *p = ctx;
    unsigned si = d(p, 0x70e);
    uint32_t now = (d(p, 0x6c1) | (uint32_t)d(p, 0x6c3) << 16) + 1;
    sd(p, si + 0x20, now & 0xffff); sd(p, si + 0x22, (now >> 16) & 0xffff);
    task_yield(p, sp);
}
/* IRQ 0 in the idle task (0434:0a44): the PIT started again with the divisor
 * (06ba), the counter + 1, then the chain from [06bd]: the task switch (0c48),
 * the module's flip (09ee), the audio tick (3a52), the end (0a88) */
static void tick(Program *p) {
    p->ticks++;
    out(p, 0x43, 0x36);
    unsigned div = d(p, 0x6ba);
    out(p, 0x40, div & 255); out(p, 0x40, div >> 8);
    uint32_t n = (d(p, 0x6c1) | (uint32_t)d(p, 0x6c3) << 16) + 1;
    sd(p, 0x6c1, n & 0xffff); sd(p, 0x6c3, n >> 16);
    unsigned seg = d(p, 0x6bf), off = d(p, 0x6bd), module_seg = (d(p, 0xcb4e) + 9) & 0xffff;
    for (;;) {
        if (seg == LIB && off == 0x0c48) {
            unsigned ax = d(p, 0x726);
            sd(p, 0x728, ax);
            if (!ax) {
                unsigned si = d(p, 0x70e);   /* a task in its loop at 0aef: its frame is where it was */
                sd(p, si + 0x18, ax);
                task_next(p);
            }
            seg = d(p, 0x71e); off = d(p, 0x71c);
        } else if (seg == module_seg && off == 0x09ee) {
            if (p->video) gfx_flip(p->video);
            seg = w(p, module_seg, 0x20a); off = w(p, module_seg, 0x208);
        } else if (seg == CODE && off == 0x3a52) {
            sound_request(p, SND_TICK, 0);
            seg = d(p, 0x9d03); off = d(p, 0x9d01);
        } else if (seg == LIB && off == 0x0a88) break;   /* pops, EOI, IRET */
        else not_modelled(p);   /* the timer chain somewhere else */
    }
}

/* ---- the program, the game attached ------------------------------------------------- */
Program *program_new(Machine *mc, const char *dir) {
    Program *p = calloc(1, sizeof *p);
    p->mc = mc;
    p->M = calloc(1, GFX_MEMORY);
    memcpy(p->M, mc->ivt, mc->nivt);
    p->M[0x46c] = u8(mc->ticks); p->M[0x46d] = u8(mc->ticks >> 8); p->M[0x46e] = u8(mc->ticks >> 16); p->M[0x46f] = u8(mc->ticks >> 24);
    p->M[0x449] = 3;
    put(p->M, 0x417, mc->shift_flags);
    p->dta_seg = PSP; p->dta_off = 0x80;
    p->run = &p->own;
    p->tick_limit = -1;
    snprintf(p->dir, sizeof p->dir, "%s", dir);
    p->far = far_load(dir);
    return p;
}
void program_free(Program *p) {
    if (!p) return;
    free(p->M); free(p->video); far_free(p->far);
    free(p->output.v); free(p->ports.v); free(p->dos.v); free(p->printer.events.v);
    free(p);
}
int program_start(Program *p) {
    jmp_buf here;
    void *outer = p->run->jump;
    p->run->jump = &here;
    p->run->outcome = GAME_RUNNING;
    if (!setjmp(here)) { library_start(p); game_start(p); }
    p->run->jump = outer;
    return p->run->outcome;
}
static void hook_quit(void *ctx) { quit_question(ctx); }
static void hook_alt_b(void *ctx, unsigned sp) { alt_b_(ctx, sp); }
static void hook_joystick(void *ctx) { joystick_read_(ctx); }
static int hook_pause(void *ctx) { return pause_(ctx); }
static int hook_save(void *ctx) { return save_question(ctx); }
static void hook_records(void *ctx, int al) { records_save(ctx, (unsigned)al); }
/* game_flow.Game with a program: the game's data segment is the program's
 * (the cycle works on it from now on), its sound requests go to the sound
 * driver, its waits to the tasks */
void program_attach(Program *p, Game *g) {
    Cycle *c = g->c;
    c->mem = DS; c->k->ds = DS;
    if (c->level == LEVEL_GAME) { c->k->data = DS; c->k->ndata = 65536; }
    g->run = p->run;
    g->video = p->video;
    g->memory = p->M;
    ProgramHooks h = {0};
    h.ctx = p;
    h.quit_question = hook_quit; h.alt_b = hook_alt_b; h.joystick_read = hook_joystick;
    h.pause = hook_pause; h.save_question = hook_save; h.records_save = hook_records;
    h.load_buffer = load_buffer; h.ticks = p->mc->time_ticks; h.task_wait = task_wait;
    h.sound = program_sound; h.port_out = port_out; h.joystick = &p->mc->joystick;
    g->program = h;
    c->host.joystick = &p->mc->joystick;
}

/* ---- the library's routines the game never calls (map_game.py, DEAD_CODE) ------------- */
static unsigned alloc_clear(Program *p, unsigned n) {   /* 054a: n bytes, cleared */
    unsigned seg = alloc_bytes(p, n);
    static const uint8_t zero[0x10000];
    write_(p, seg * 16u, zero, (n & 0xfffe) | (n & 1));
    return seg;
}
static unsigned alloc_zero(Program *p, unsigned paragraphs) {   /* 0563: paragraphs, cleared */
    static const uint8_t zero[16];
    unsigned seg = alloc(p, paragraphs), cx = paragraphs, bx = seg;
    for (;;) {
        write_(p, bx * 16u, zero, 16); bx = (bx + 1) & 0xffff; cx = (cx - 1) & 0xffff;
        if (!cx) break;
    }
    return seg;
}
static void arena_check(Program *p) {   /* 0580: the free list, or "Memory corrupt" */
    unsigned base = d(p, 0x13c), ds = base;
    for (;;) {
        unsigned ax = w(p, ds, 0x10);
        if (w(p, ax, 0x12) != ds) exit_(p, 1, 0x156);
        ds = ax;
        if (ax == base) return;
        if (b(p, ds, 0x0d) != 0x46) exit_(p, 1, 0x156);
        if (b(p, (ax + w(p, ds, 0x0e)) & 0xffff, 0x0c) != 0x46) exit_(p, 1, 0x156);
    }
}
/* 060b: node bx with key ax in the list at si, before the first one whose key (+4) is below ax */
static void list_insert_sorted(Program *p, unsigned si, unsigned bx, unsigned ax) {
    sd(p, bx + 4, ax);
    unsigned di = si, dx, cx;
    for (;;) {
        dx = di; di = d(p, di); cx = di;
        if (!cx || ax > d(p, di + 4)) break;
    }
    sd(p, bx, cx); sd(p, bx + 2, dx);
    sd(p, (cx ? cx : si) + 2, bx);
    sd(p, dx ? dx : si, bx);
}
static int key_waiting(Program *p, unsigned *bx_out) {   /* 078b: 1 = no key in the queue */
    unsigned bx = d(p, 0x674) & 0x1f1f;
    if (bx_out) *bx_out = bx;
    return (bx >> 8) == (bx & 255);
}
static unsigned key_wait(Program *p) {   /* 0796: the next key of the queue (byte, shift flags) */
    unsigned bx;
    for (;;) {   /* the loop is the whole routine: a pass each time */
        keys_at(p, POINT_KEY_WAIT);
        if (!key_waiting(p, &bx)) break;
    }
    unsigned slot = 0x634 + 2 * ((bx >> 8) & 0x1f), ax = d(p, slot);
    DS[0x675] = u8(DS[0x675] + 1);
    return ax;
}
static void calibrate_low(Program *p, unsigned di) {   /* 07fe */
    uint8_t old = DS[0x6b0];
    DS[0x6b0] = 0xff; joystick_read_(p);
    sd(p, di + 8, d(p, di)); sd(p, di + 0xc, d(p, di + 2)); DS[0x6b0] = old;
}
static void calibrate_high(Program *p, unsigned di) {   /* 081a */
    uint8_t old = DS[0x6b0];
    DS[0x6b0] = 0xff; joystick_read_(p);
    sd(p, di + 0xa, d(p, di)); sd(p, di + 0xe, d(p, di + 2)); DS[0x6b0] = old;
}
/* 0b84: the bits of AX for the current task: those already there (+1c) are
 * taken, the others waited for (+1e): wake-up time FFFFFFFFh and INT 60h */
static unsigned events_wait(Program *p, unsigned ax, unsigned sp) {
    unsigned si = d(p, 0x70e), have = ax & d(p, si + 0x1c);
    sd(p, si + 0x1c, d(p, si + 0x1c) ^ have);
    unsigned missing = have ^ ax;
    sd(p, si + 0x1e, missing);
    if (!missing) return 0;
    sd(p, si + 0x20, 0xffff); sd(p, si + 0x22, 0xffff);
    task_yield(p, sp);
    return 0xffff;
}
/* 0ba3: the bits of AX to the task at DI: the ones it waits for clear its
 * wait, the others are kept for it; when it waits for nothing more it is
 * ready and, with a priority not below the current task's, gets the CPU */
static void events_signal(Program *p, unsigned di, unsigned ax, unsigned sp) {
    unsigned hit = ax & d(p, di + 0x1e), rest = ax ^ hit;
    sd(p, di + 0x1c, d(p, di + 0x1c) | rest);
    unsigned left = d(p, di + 0x1e) ^ hit;
    sd(p, di + 0x1e, left);
    if (left) return;
    sd(p, di + 0x20, d(p, 0x6c1)); sd(p, di + 0x22, d(p, 0x6c3));
    if (d(p, di + 4) < d(p, d(p, 0x70e) + 4)) return;
    task_yield(p, sp);
}
static int extension(Program *p, unsigned si, unsigned di) {   /* 12b8: the word after '.'; -1: none */
    for (;;) {
        unsigned al = DS[si & 0xffff];
        si++;
        if (!al) return -1;
        if (al == 0x2e) return option(p, si, di);
    }
}
/* 12ca: a number from text at DS:SI: leading zeros skipped, '$' before or
 * 'h' after for hexadecimal; the digits from the last one back, each times
 * 10 (or 16) to the power of its place; past 16 bits is an error (carry).
 * Returns the carry; the value in *value. */
static int number(Program *p, long long si, unsigned *value) {
#define AT(i) DS[(i) & 0xffff]
    int dh = 0;
    unsigned al;
    *value = 0;
    if (AT(si) == 0x24) { dh = 1; si++; }
    for (;;) { al = AT(si); si++; if (al != 0x30) break; }
    if (!al) return 0;
    long long cx = 0;
    for (;;) { al = AT(si); si++; cx++; if (!al) break; }
    si -= 2;
    long long bp = cx;
    if ((AT(si) | 0x20) == 0x68) {
        dh = 1; si--; bp--;
        if (!bp) return 0;
    }
    unsigned base = dh ? 16 : 10, bx = 1;
    unsigned long long sum = 0;
    for (;;) {
        al = AT(si); si--;
        if (base == 10) {
            al = (al - 0x30) & 0x1ff;
            if (al > 0xff || al >= 10) return 1;
        } else {
            al = ((al | 0x20) - 0x30) & 0x1ff;
            if (al > 0xff) return 1;
            if (al >= 10) {
                al = (al - 0x31) & 0x1ff;
                if (al > 0xff || al >= 6) return 1;
                al += 10;
            }
        }
        unsigned long long product = (unsigned long long)al * bx;
        if (product > 0xffff) return 1;
        sum += product;
        if (sum > 0xffff) return 1;
        bx = (bx * base) & 0xffff;
        bp--;
        if (!bp) { *value = (unsigned)sum; return 0; }
    }
#undef AT
}

/* ---- one routine of the model, for the tests ---------------------------------------- */
enum {
    P_START, P_ALLOC, P_ALLOC_BYTES, P_FREE, P_ALLOC_CLEAR, P_ALLOC_ZERO, P_ARENA_CHECK, P_LIST_INSERT,
    P_KEY_WAITING, P_KEY_WAIT, P_CALIBRATE_LOW, P_CALIBRATE_HIGH, P_EVENTS_WAIT, P_EVENTS_SIGNAL,
    P_EXTENSION, P_NUMBER, P_DEBUG_INTERRUPT, P_TASK_END, P_RECORDS_SAVE, P_TICK, P_TASK_WAIT,
    P_QUIT_QUESTION, P_PAUSE, P_SAVE_QUESTION, P_DISK_REQUEST, P_SHELL, P_CLOCK, P_SOUND, P_JOYSTICK_READ,
    P_TASK_CREATE, P_SPRITES
};
/* the graphics module's state, for a test that sets it */
void program_make_video(Program *p) {
    if (p->video) return;
    p->video = calloc(1, sizeof *p->video);
    gfx_init(p->video, p->M, DATA);
}
int program_run(Program *p, int entry, long long a, long long bb, long long *value) {
    jmp_buf here;
    void *outer = p->run->jump;
    volatile long long v = 0;
    p->run->jump = &here;
    p->run->outcome = GAME_RUNNING;
    if (!setjmp(here)) {
        unsigned x;
        switch (entry) {
        case P_START: library_start(p); game_start(p); break;
        case P_ALLOC: v = alloc(p, (unsigned)a); break;
        case P_ALLOC_BYTES: v = alloc_bytes(p, (unsigned)a); break;
        case P_FREE: free_(p, (unsigned)a); break;
        case P_ALLOC_CLEAR: v = alloc_clear(p, (unsigned)a); break;
        case P_ALLOC_ZERO: v = alloc_zero(p, (unsigned)a); break;
        case P_ARENA_CHECK: arena_check(p); break;
        case P_LIST_INSERT: list_insert_sorted(p, (unsigned)a, (unsigned)(bb >> 16), (unsigned)(bb & 0xffff)); break;
        case P_KEY_WAITING: v = key_waiting(p, 0); break;
        case P_KEY_WAIT: v = key_wait(p); break;
        case P_CALIBRATE_LOW: calibrate_low(p, (unsigned)a); break;
        case P_CALIBRATE_HIGH: calibrate_high(p, (unsigned)a); break;
        case P_EVENTS_WAIT: v = events_wait(p, (unsigned)a, (unsigned)bb); break;
        case P_EVENTS_SIGNAL: events_signal(p, (unsigned)(a >> 16), (unsigned)(a & 0xffff), (unsigned)bb); break;
        case P_EXTENSION: v = extension(p, (unsigned)a, (unsigned)bb); break;
        case P_NUMBER: { int carry = number(p, a, &x); v = carry ? -1 : (long long)x; break; }
        case P_DEBUG_INTERRUPT: v = (long long)w(p, 0, 0x66) << 16 | w(p, 0, 0x64); break;   /* INT 01..03: 0434:0281, a RETF through INT 19h's vector */
        case P_TASK_END: {   /* 0434:0b83: a RETF through the two words above the task's stack */
            unsigned seg = d(p, (unsigned)a + 0x16), top = (d(p, (unsigned)a + 0x12) << 4) & 0xffff;
            v = (long long)w(p, seg, top + 2) << 16 | w(p, seg, top);
            break;
        }
        case P_RECORDS_SAVE: records_save(p, (unsigned)a); break;
        case P_TICK: tick(p); break;
        case P_TASK_WAIT: task_wait(p, (unsigned)a); break;
        case P_QUIT_QUESTION: quit_question(p); break;
        case P_PAUSE: v = pause_(p); break;
        case P_SAVE_QUESTION: v = save_question(p); break;
        case P_DISK_REQUEST: disk_request(p, (unsigned)a); break;
        case P_SHELL: shell(p, (unsigned)a); break;
        case P_CLOCK: v = clock_(p); break;
        case P_SOUND: program_sound(p, (int)a, (unsigned)bb); break;
        case P_JOYSTICK_READ: joystick_read_(p); break;
        case P_TASK_CREATE: task_create(p, (unsigned)a); break;
        case P_SPRITES:   /* a = AL << 32 | ES << 16 | DI, bb = table << 16 | SP */
            sprites(p, (unsigned)(a >> 32) & 255, (unsigned)(a >> 16) & 0xffff, (unsigned)a & 0xffff,
                    (unsigned)(bb >> 16) & 0xffff, (unsigned)bb & 0xffff);
            break;
        default: p->run->outcome = OFF_NOT_MODELLED;
        }
    }
    p->run->jump = outer;
    if (value) *value = v;
    return p->run->outcome;
}
