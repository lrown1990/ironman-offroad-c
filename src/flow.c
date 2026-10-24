/* The game outside the object scripts: a literal translation of
 * tools/game_flow.py.
 *   0107..013e  the game: new game (01e4), sign-in (2564), then per race 0140,
 *               371e, results 30e4, shop 2bf8, next race 01d5, until 1a6 & 100;
 *               then 36b3 (records) and the Ironman Club table 34c8;
 *   00d9..0105  the title demo: demo mode, new game, then the title screen
 *               2468, a demo race, 371e, results, the table, the demo shop and
 *               the next race of the order; a button in any of them starts a
 *               game at 0107;
 *   24d7        one update of a screen: deadline, keys (0358 Esc, 0342 Alt+B,
 *               3b79 F2), joystick service, scheduler and contacts, drawing
 *               (graphics module 64, 60 that consumes the draw list, 18), wait;
 *   0140        a race: music off, preparation 01ed..02b7, 1339, start
 *               02ba..0309, then the race loop until 1a6 & 4;
 *   06a0        pool reset; 06cf an object without parent.
 * The file buffer is game.exe's stack segment (DS + 0cec in this layout,
 * DS:cec0): the files 1193 and 11c7 load land there.
 * The model's exceptions (a stop asked by observe, a part not modelled, the
 * program's end) end the run with longjmp back to game_run.
 */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "internal.h"

#define FILE_SEGMENT_OFFSET 0xcec0   /* SS:0000 seen from DS (SS = DS + 0cec) */
/* SP where the update loops call 0342 (Alt+B): the screens through 24d7, the race loop in 0140 */
#define SCREEN_SP 0x81fc
#define RACE_SP 0x81fe

const char *const STOP_NAMES[STOP_KINDS] = {
    "screen_before", "screen_after", "screen_after_flip", "pre_1339", "post_1339",
    "before_scheduler", "after_contacts", "after_flip", "pre_finish", "post_finish",
    "pre_shop", "pre_next", "post_next", "game_over", "pre_table", "post_table",
    "demo", "pre_new_game", "pre_signin", "pre_race", "title", "demo_race",
    "demo_finish", "demo_results", "demo_table", "demo_shop", "demo_next"};

void game_raise(Game *g, int outcome) { run_raise(g->run, outcome); }
/* the cycle's errors end the run as the model's exceptions do */
static void check(Game *g) { if (g->c->k->error) game_raise(g, g->c->k->error); }

/* ---- the cycle's host: the drawing (DrawingCalls), the timer clock ---------- */
static void draw_call(void *ctx, const GfxCall *call) {
    Game *g = ctx;
    if (!g->video) return;
    GfxRegs r = {0};
    r.ds = g->c->segment; r.ss = u16(g->c->segment + 0xcec);
    if (call->entry == 0x70) {
        r.ax = call->y; r.bx = call->x; r.cx = 0x746; r.dx = 0x766; r.si = 0x786;
        r.bp = call->font; r.di = call->glyph; r.cf = call->carry;
    } else {
        if (call->has & GA_AX) r.ax = call->ax;
        else if (call->has & GA_AL) r.ax = call->al;
        if (call->has & GA_SI) r.si = call->si;
        if (call->has & GA_BL) r.bx = call->bl;
    }
    if (gfx_call(g->video, call->entry, &r)) game_raise(g, g->video->error ? g->video->error : OFF_NOT_MODELLED);
}
static int settimer(void *ctx, unsigned at, unsigned ax, uint32_t *now) {
    Game *g = ctx;
    return g->io.settimer_clock ? g->io.settimer_clock(g->io.ctx, at, ax, now) : 0;
}
/* with a program: the sound requests to its driver, the waits of a letter
 * to its tasks (on the ideal machine), the port writes of the joystick */
static void audio(void *ctx, int kind, unsigned value) {
    Game *g = ctx;
    if (g->program.sound) g->program.sound(g->program.ctx, kind, value);
}
static int task_wait(void *ctx, unsigned sp) {
    Game *g = ctx;
    if (!g->program.ctx || !g->program.ticks) return 0;
    g->program.task_wait(g->program.ctx, sp);
    return 1;
}
static void port_out(void *ctx, unsigned port, unsigned value) {
    Game *g = ctx;
    if (g->program.port_out) g->program.port_out(g->program.ctx, port, value);
}

void game_init(Game *g, Cycle *c, const char *dir) {
    memset(g, 0, sizeof *g);
    g->c = c;
    snprintf(g->dir, sizeof g->dir, "%s", dir);
    c->live_input = 1;   /* human trucks read the key table (1508) */
    c->host.ctx = g;
    c->host.graphics = draw_call;
    c->host.audio = audio;
    c->host.task_wait = task_wait;
    c->host.port_out = port_out;
    g->run = &g->own;
}

/* ---- helpers --------------------------------------------------------------------- */
static void stop(Game *g, int kind) {
    if (g->io.observe && g->io.observe(g->io.ctx, kind)) game_raise(g, GAME_STOPPED);
}
static void set_counter(Game *g, int kind) {
    uint32_t value;
    if (g->io.counter && g->io.counter(g->io.ctx, kind, &value)) {
        ww(g->c->mem, 0x6c1, value & 65535); ww(g->c->mem, 0x6c3, value >> 16);
    }
}
/* the interrupts of a group of key bytes: F0 goes with the byte after it */
static void apply_keys(Game *g, int kind) {
    uint8_t keys[512];
    int n = g->io.keys ? g->io.keys(g->io.ctx, kind, keys, sizeof keys) : 0;
    if (n < 0) game_raise(g, GAME_STOPPED);   /* the inputs end the run here */
    for (int i = 0; i < n;) {
        int j = i;
        while (j < n && keys[j] == 0xf0) j++;
        int count = (j < n ? j + 1 : j) - i;
        int r = ctl_int9(g->c->mem, keys + i, count);
        if (r) game_raise(g, r);
        i += count;
    }
}
static void music(Game *g, unsigned al) { cycle_request(g->c, REQ_MUSIC, al); }
static void music_off(Game *g) { cycle_request(g->c, REQ_MUSIC_OFF, 0); }
static void call_graphics(Game *g, unsigned entry, unsigned has, unsigned ax, unsigned al, unsigned si) {
    GfxCall call = {0};
    call.entry = u8(entry); call.has = u8(has); call.ax = u16(ax); call.al = u16(al); call.si = u16(si);
    cycle_call_graphics(g->c, &call);
}
static unsigned file_segment(Game *g) { return (g->c->segment + FILE_SEGMENT_OFFSET / 16) & 65535; }
static uint8_t *read_file(Game *g, const char *name, long long *n) {
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", g->dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) game_raise(g, OFF_NOT_MODELLED);
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n ? *n : 1);
    if (fread(b, 1, *n, f) != (size_t)*n) { fclose(f); free(b); game_raise(g, OFF_NOT_MODELLED); }
    fclose(f);
    return b;
}
/* 0434:0d2a into SS:0000; the part inside the data segment. With a program:
 * its loader and the disk request until the file is there with the size
 * 1193 expects (sp: SP at the call of 11f9). expected < 0: none. */
static void load_file(Game *g, const char *name, long long expected, unsigned sp) {
    if (g->program.ctx) { g->program.load_buffer(g->program.ctx, name, expected, sp); return; }
    long long n;
    uint8_t *data = read_file(g, name, &n);
    if (expected >= 0 && n != expected) { free(data); game_raise(g, OFF_NOT_MODELLED); }   /* not the size 1193 expects */
    long long inside = n < 0x10000 - FILE_SEGMENT_OFFSET ? n : 0x10000 - FILE_SEGMENT_OFFSET;
    memcpy(g->c->mem + FILE_SEGMENT_OFFSET, data, inside);
    if (g->memory) {
        long long at = (long long)file_segment(g) * 16, room = GFX_MEMORY - at;
        memcpy(g->memory + at, data, n < room ? n : room);
    }
    free(data);
}
/* entry 5c of the graphics module draws the four colour planes and leaves
 * the fifth (the auxiliary plane, 40 x 200 bytes, offset in the big-endian
 * word +2 of the file, + 4) at the start of the file buffer */
static void auxiliary_plane(Game *g, const char *name) {
    long long n;
    uint8_t *data = read_file(g, name, &n), plane[8000];
    long long r = n >= 4 ? file_decode_plane(data, n, (data[2] << 8 | data[3]) + 4, plane) : -1;
    free(data);
    if (r < 0) game_raise(g, OFF_BAD_ADDRESS);
    memcpy(g->c->mem + FILE_SEGMENT_OFFSET, plane, 8000);
}

/* ---- 06a0, 06cf, 01d5, 01e4 -------------------------------------------------------- */
static void pool_reset(Game *g) {
    uint8_t *m = g->c->mem;
    ww(m, 0xb26, 0xb28);
    for (int n = 0; n < 64; n++) ww(m, 0xb28 + n * 0x7a, n < 63 ? 0xb28 + (n + 1) * 0x7a : 0);
    for (unsigned a = 0x29a8; a <= 0x29b0; a += 2) ww(m, a, 0);
    call_graphics(g, 0x68, 0, 0, 0, 0);
    memset(m + 0x2f5a, 0, 0x40); memset(m + 0x2a9a, 0, 0x4c0);
}
static unsigned new_object(Game *g, unsigned pointer) {
    unsigned at = cycle_alloc_root(g->c, pointer);
    check(g);
    return at;
}
static void next_race(Game *g) {
    uint8_t *m = g->c->mem;
    unsigned al = (m[0x1a8] + 1) & 255;
    if (al >= 0x2b) al = 0x1b;
    m[0x1a8] = u8(al);
}
static void new_game(Game *g) { g->c->mem[0x1a8] = 0; session_new_game(g->c->mem); }

/* ---- keys checked every update ---------------------------------------------------- */
static void escape(Game *g) {   /* 0358 */
    if (!ctl_key_down(g->c->mem, 1)) return;
    if (!g->program.ctx) game_raise(g, OFF_NOT_MODELLED);   /* Esc: the quit question */
    g->program.quit_question(g->program.ctx);
}
static void alt_b(Game *g, unsigned sp) {   /* 0342 */
    uint8_t *m = g->c->mem;
    if (!(m[0x67e] & 8 && ctl_key_down(m, 0x30))) return;
    if (!g->program.ctx) game_raise(g, OFF_NOT_MODELLED);   /* Alt+B: the DOS shell */
    g->program.alt_b(g->program.ctx, sp);
}
/* 24e0..24e6 (Esc, Alt+B, F2) in the screens; the race loop has Alt+B first */
static void key_checks(Game *g, int race) {
    uint8_t *m = g->c->mem;
    if (race) { alt_b(g, RACE_SP); escape(g); }
    else { escape(g); alt_b(g, SCREEN_SP); }
    if (ctl_key_down(m, 0x3c)) {   /* 3b79, F2: music on/off */
        if (!m[0x9cf6]) {
            m[0x9cf6] = 0xff; m[0x9cf5] ^= 0xff;
            if (m[0x9cf5]) music_off(g);
        }
    } else m[0x9cf6] = 0;
}
static void joystick_service(Game *g) {   /* 0434:0888 */
    if (!g->c->mem[0x6b0]) return;
    if (!g->program.ctx) game_raise(g, OFF_NOT_MODELLED);
    g->program.joystick_read(g->program.ctx);
}
/* 253c: any of the four players holds the button (the first that does ends the look) */
static int button_pressed(Game *g) {
    for (unsigned p = 0; p < 4; p++) {
        int r = ctl_read_controls(g->c->mem, p);
        if (r < 0) game_raise(g, OFF_BAD_ADDRESS);
        if (r & 0x10) return 1;
    }
    return 0;
}

/* ---- one update of a screen (24d7) ----------------------------------------------- */
/* 018e/2514: at least one tick, until the counter reaches the deadline. With
 * a program on the ideal machine: the loop as it is, INT 60h and a tick each
 * time (sp: SP at the INT). */
static void wait_until(Game *g, unsigned deadline, unsigned sp) {
    uint8_t *m = g->c->mem;
    if (g->program.ctx && g->program.ticks) {
        for (;;) {
            g->program.task_wait(g->program.ctx, sp);
            if ((deadline - rw(m, 0x6c1) - 1) & 0x8000) return;
        }
    }
    uint32_t now = rw(m, 0x6c1) | (uint32_t)rw(m, 0x6c3) << 16;
    int late = ((deadline - (now & 65535) - 1) & 65535) >= 0x8000;
    uint32_t target = late ? now + 1 : now + ((deadline - now) & 65535);
    ww(m, 0x6c1, target & 65535); ww(m, 0x6c3, (target >> 16) & 65535);
    unsigned task = rw(m, 0x70e);   /* the last wake-up time the loop set: the counter */
    ww(m, task + 0x20, target & 65535); ww(m, task + 0x22, (target >> 16) & 65535);
    if (g->video) gfx_flip(g->video);   /* 09ee at the first tick */
}
static void frame(Game *g) {
    static const uint8_t none[4] = {0, 0, 0, 0};
    cycle_frame(g->c, none);
    check(g);
}
static void screen_frame(Game *g) {
    Cycle *c = g->c;
    uint8_t *m = c->mem;
    set_counter(g, STOP_SCREEN_BEFORE);
    ww(m, 0x1a0, rw(m, 0x6c1) + 3);
    key_checks(g, 0); joystick_service(g);
    stop(g, STOP_SCREEN_BEFORE);
    c->host.settimer_clock = g->io.settimer_clock ? settimer : 0;
    frame(g);
    stop(g, STOP_SCREEN_AFTER);
    call_graphics(g, 0x64, 0, 0, 0, 0); call_graphics(g, 0x60, GA_SI, 0, 0, 0x29b0); ww(m, 0x29b0, 0);
    call_graphics(g, 0x18, GA_AL, 0, 0, 0);
    apply_keys(g, STOP_SCREEN_AFTER);
    stop(g, STOP_SCREEN_AFTER_FLIP);
    wait_until(g, rw(m, 0x1a0), SCREEN_SP);
}
/* the common shape of 2564, 30e4 and 2bf8: 1a6 = 0, pool reset, root object */
static void screen(Game *g, unsigned pointer) {
    ww(g->c->mem, 0x1a6, 0); pool_reset(g); new_object(g, pointer);
}
static int run_screen(Game *g, int check_button) {
    for (;;) {
        screen_frame(g);
        if (check_button && button_pressed(g)) return 1;
        if (rw(g->c->mem, 0x1a6) & 0x10) return 0;
    }
}

/* ---- the screens -------------------------------------------------------------------- */
static void sign_in(Game *g) { screen(g, 0x31fa); run_screen(g, 0); }   /* 2564 */
static int results(Game *g) {   /* 30e4 */
    uint8_t *m = g->c->mem;
    ww(m, 0x1a6, 0); pool_reset(g); new_object(g, 0x31f6);
    call_graphics(g, 0x2c, 0, 0, 0, 0);
    long long at = (long long)rw(m, 0x9cec) + 0xe;
    if (at > 65535) game_raise(g, OFF_BAD_ADDRESS);
    music(g, (m[at] + 2) & 255);
    return run_screen(g, m[0x1ab] != 0);
}
static int shop(Game *g) {   /* 2bf8 */
    uint8_t *m = g->c->mem;
    ww(m, 0x1a6, 0); pool_reset(g); new_object(g, m[0x1ab] ? 0x3202 : 0x31fe);
    int pressed = run_screen(g, m[0x1ab] != 0);
    music_off(g);
    return pressed;
}
static int table(Game *g) {   /* 34c8 */
    uint8_t *m = g->c->mem;
    call_graphics(g, 0x2c, 0, 0, 0, 0);
    for (;;) {
        ww(m, 0x1a6, 0); pool_reset(g); new_object(g, 0x31f2);
        for (;;) {
            screen_frame(g);
            if (button_pressed(g)) return 1;
            if (g->program.ctx) {
                if (g->program.save_question(g->program.ctx)) break;   /* 34eb: the table again */
            } else if (m[0x67e] & 4 && ctl_key_down(m, 0x1f)) game_raise(g, OFF_NOT_MODELLED);   /* Ctrl+S */
            if (rw(m, 0x1a6) & 0x10) return 0;
        }
    }
}

/* ---- the race ------------------------------------------------------------------------- */
static void terrain(Game *g) {   /* 1e7e */
    uint8_t *m = g->c->mem;
    unsigned kk = m[0x1aa] * 2;
    ww(m, 0x4f7e, rw(m, 0x53ec + kk)); ww(m, 0x4f80, rw(m, 0x53fc + kk)); ww(m, 0x53ea, rw(m, 0x6b14 + kk));
    memcpy(m + 0x540c, g->c->k->terrain[m[0x1aa]], TERRAIN_BYTES);
}
static void race_preparation(Game *g) {   /* 01ed..02b4 */
    Cycle *c = g->c;
    uint8_t *m = c->mem;
    m[0x1a9] = m[0x1ad + m[0x1a8]]; m[0x1aa] = m[0x1a9] & 7; m[0x622] = 0;
    c->track = m[0x1aa]; c->layout = m[0x1a9];
    pool_reset(g);
    unsigned si = new_object(g, 0x31e2), bx = 0x1d8 + m[0x1aa] * 4;
    ww(m, si + 0x2a, rw(m, bx)); ww(m, si + 0x2e, rw(m, bx + 2)); ww(m, si + 0x18, 0x7fff);
    new_object(g, 0x31e6); m[0x494] = m[0x495] = 0;
    bx = rw(m, 0x1f8 + m[0x1a9] * 2);
    while (rw(m, bx)) {
        unsigned ax = rw(m, bx);
        unsigned pointer = (ax & 0xc000) == 0xc000 ? 0x31de : ax & 0x8000 ? 0x31da : ax & 0x4000 ? 0x31d6 : 0x31d2;
        si = new_object(g, pointer);
        ww(m, si + 0x2a, rw(m, bx) & 0x3fff); ww(m, si + 0x2e, rw(m, bx + 2));
        ww(m, si + 0x22, rw(m, bx + 4) >> 5);
        unsigned y = rw(m, bx + 6) >> 5;
        ww(m, si + 0x26, y); ww(m, si + 0x18, y);
        ww(m, si + 0x28, 0); ww(m, si + 0x2c, 0); ww(m, si + 0x20, 0); ww(m, si + 0x24, 0);
        bx = (bx + 8) & 65535;
    }
    char name[32];
    snprintf(name, sizeof name, "contour%u.xx", m[0x1aa]);
    load_file(g, name, -1, 0x81f6); ww(m, 0x4b3f, file_segment(g));   /* 11c7 from 01ed */
    terrain(g);
    ww(m, 0x1a6, 0x80);
}
/* 0140: 1 when the button of a player ends a demo race (01c2) */
static int race(Game *g) {
    Cycle *c = g->c;
    uint8_t *m = c->mem;
    int draw = g->video != 0;
    music_off(g); race_preparation(g);
    stop(g, STOP_PRE_1339);
    session_race_start(c); check(g);
    stop(g, STOP_POST_1339);
    /* 02ba..0309: one scheduler pass, palette 0, the track picture into the
     * file buffer and onto the pages (its fifth plane into the buffer), the
     * draw list consumed, the restore lists emptied, the panel (03dc), 1a6 =
     * 8 (paused for the start lights), palette 2, the music of the track */
    memset(c->controls, 0, 4);
    c->sounds.n = 0; c->events.n = 0; c->script_audio.n = 0;
    cycle_scheduler(c); check(g);
    call_graphics(g, 0x2c, 0, 0, 0, 0);
    char name[32];
    snprintf(name, sizeof name, "track%u.xx", m[0x1aa]);
    load_file(g, name, rw(m, 0x4b29 + 2 * m[0x1aa]), 0x81f4); ww(m, 0x4aa5, file_segment(g));   /* 1193 from 01ed */
    call_graphics(g, 0x5c, GA_AX, rw(m, 0x4aa5), 0, 0); auxiliary_plane(g, name);
    call_graphics(g, 0x60, GA_SI, 0, 0, 0x29b0); ww(m, 0x29b0, 0); call_graphics(g, 0x68, 0, 0, 0, 0);
    loop_hud_init(c, draw); check(g);
    ww(m, 0x1a6, 8);
    call_graphics(g, 0x28, GA_AL, 0, 2, 0); music(g, (m[0x1aa] + 11) & 255);
    ww(m, 0x1a0, rw(m, 0x6c1) + 3);
    for (;;) {
        set_counter(g, STOP_BEFORE_SCHEDULER);
        joystick_service(g);   /* 014f */
        stop(g, STOP_BEFORE_SCHEDULER);
        frame(g);
        stop(g, STOP_AFTER_CONTACTS);
        if (draw) {
            call_graphics(g, 0x64, 0, 0, 0, 0); loop_race_clock(m); loop_hud(c, 1);
            call_graphics(g, 0x60, GA_SI, 0, 0, 0x29b0); ww(m, 0x29b0, 0);
        } else loop_tail(c);
        key_checks(g, 1);
        call_graphics(g, 0x18, GA_AL, 0, 1, 0);
        if (g->program.ctx) {
            if (g->program.pause(g->program.ctx)) { music_off(g); return 1; }   /* 030a: F10 in the pause */
        } else if (!m[0x1ab] && ctl_key_down(m, 0x3b)) game_raise(g, OFF_NOT_MODELLED);   /* F1: the pause */
        apply_keys(g, STOP_AFTER_CONTACTS);
        stop(g, STOP_AFTER_FLIP);
        wait_until(g, rw(m, 0x1a0), RACE_SP); ww(m, 0x1a0, rw(m, 0x6c1) + 3);
        if (m[0x1ab] && button_pressed(g)) { music_off(g); return 1; }   /* 01d0 */
        if (rw(m, 0x1a6) & 4) return 0;
    }
}
static int title(Game *g) {   /* 2468 */
    uint8_t *m = g->c->mem;
    load_file(g, "titles.xx", rw(m, 0x4b29 + 16), 0x81f6); ww(m, 0x4aa5, file_segment(g));   /* 1193 from 2468 */
    music(g, 1); ww(m, 0x1a6, 0); pool_reset(g);
    call_graphics(g, 0x20, GA_AX, 0xf03, 0, 0); call_graphics(g, 0x30, GA_AL, 0, 3, 0);
    new_object(g, 0x31ee);
    for (;;) {
        screen_frame(g);
        if (button_pressed(g)) return 1;
        if (rw(m, 0x1a6) & 0x10) return 0;
    }
}
static int finish(Game *g) { stop(g, STOP_PRE_FINISH); cycle_finish(g->c); check(g); return 0; }
static int next_race_step(Game *g) { next_race(g); return 0; }
/* 00d9..0105: the title demo until a button starts a game; resume: the stop
 * of the loop to start at (-1: 00d9) */
static void demo(Game *g, int resume) {
    uint8_t *m = g->c->mem;
    static const int kinds[7] = {STOP_TITLE, STOP_DEMO_RACE, STOP_DEMO_FINISH, STOP_DEMO_RESULTS, STOP_DEMO_TABLE, STOP_DEMO_SHOP, STOP_DEMO_NEXT};
    static int (*const steps[7])(Game *) = {title, race, finish, results, table, shop, next_race_step};
    if (resume < 0) { m[0x1ac] = 0; m[0x1ab] = 1; new_game(g); }
    int i = 0;
    for (int j = 0; j < 7; j++) if (kinds[j] == resume) i = j;
    for (;;) {
        stop(g, kinds[i]);
        if (steps[i](g)) return;
        i = (i + 1) % 7;
    }
}
/* the game: 0107..013e; 1 = 'demo', 0 = 'new game' */
static int game(Game *g) {
    Cycle *c = g->c;
    uint8_t *m = c->mem;
    stop(g, STOP_PRE_NEW_GAME);
    m[0x1ab] = 0; new_game(g);
    stop(g, STOP_PRE_SIGNIN);
    sign_in(g);
    for (;;) {
        stop(g, STOP_PRE_RACE);
        if (race(g)) return 1;   /* 0115: F10 in the pause, back to the title */
        stop(g, STOP_PRE_FINISH);
        cycle_finish(c); check(g);
        stop(g, STOP_POST_FINISH);
        results(g);
        stop(g, STOP_PRE_SHOP);
        shop(g);
        stop(g, STOP_PRE_NEXT);
        next_race(g);
        stop(g, STOP_POST_NEXT);
        if (rw(m, 0x1a6) & 0x100) break;
    }
    stop(g, STOP_GAME_OVER);
    call_graphics(g, 0x2c, 0, 0, 0, 0);
    if (g->program.ctx) g->program.records_save(g->program.ctx, 0);
    else if (g->io.records_file && g->io.records_file(g->io.ctx)) game_raise(g, OFF_NOT_MODELLED);   /* 36b3 writing ironhi.bin */
    stop(g, STOP_PRE_TABLE);
    int pressed = table(g);
    stop(g, STOP_POST_TABLE);
    if (pressed) return 0;
    stop(g, STOP_DEMO);
    return 1;
}
/* games one after the other, through the title screen */
static void session(Game *g) {
    for (;;) if (game(g)) demo(g, -1);
}

int game_run(Game *g, int entry, long long a, long long b, const char *name, long long *value) {
    jmp_buf here;
    void *outer = g->run->jump;
    volatile long long v = 0;
    g->run->jump = &here;
    g->run->outcome = GAME_RUNNING;
    if (!setjmp(here)) {
        switch (entry) {
        case G_SESSION: session(g); break;
        case G_GAME: v = game(g); break;
        case G_DEMO: demo(g, (int)a); break;
        case G_RACE: v = race(g); break;
        case G_TITLE: v = title(g); break;
        case G_TABLE: v = table(g); break;
        case G_SHOP: v = shop(g); break;
        case G_RESULTS: v = results(g); break;
        case G_SIGN_IN: sign_in(g); break;
        case G_SCREEN_FRAME: screen_frame(g); break;
        case G_RACE_PREPARATION: race_preparation(g); break;
        case G_TERRAIN: terrain(g); break;
        case G_POOL_RESET: pool_reset(g); break;
        case G_NEXT_RACE: next_race(g); break;
        case G_NEW_GAME: new_game(g); break;
        case G_KEY_CHECKS: key_checks(g, (int)a); break;
        case G_WAIT: wait_until(g, (unsigned)a, (unsigned)b); break;
        case G_BUTTON_PRESSED: v = button_pressed(g); break;
        case G_RUN_SCREEN: v = run_screen(g, (int)a); break;
        case G_SCREEN: screen(g, (unsigned)a); break;
        case G_LOAD_FILE: load_file(g, name, a, (unsigned)b); break;
        case G_AUXILIARY_PLANE: auxiliary_plane(g, name); break;
        case G_JOYSTICK_SERVICE: joystick_service(g); break;
        case G_ESCAPE: escape(g); break;
        case G_ALT_B: alt_b(g, (unsigned)a); break;
        case G_SET_COUNTER: set_counter(g, (int)a); break;
        case G_APPLY_KEYS: apply_keys(g, (int)a); break;
        case G_MUSIC: music(g, (unsigned)a); break;
        case G_MUSIC_OFF: music_off(g); break;
        case G_NEW_OBJECT: v = new_object(g, (unsigned)a); break;
        default: g->run->outcome = OFF_NOT_MODELLED;
        }
    }
    g->run->jump = outer;
    if (value) *value = v;
    return g->run->outcome;
}
