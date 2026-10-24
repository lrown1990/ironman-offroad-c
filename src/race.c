/* The race: the 64-slot object pool and its scripts, the trucks' update, the
 * checkpoints and laps, the session, the bonus on the track and the race loop.
 * A literal translation of tools/race_cycle.py (RaceCycle), race_lap_cycle.py
 * (RaceLapCycle), race_session.py (RaceSession), track_bonus.py and
 * race_loop.py; the instructions of the screens (GameCycle) are in screens.c.
 *
 * The model's classes are one structure here; `level` says which one runs:
 * a level has the instructions and the services of the ones below it. What
 * the model calls `data` (the original image of the data segment, or the
 * live segment from GameCycle on) is k->data.
 *
 * Memory access follows the model exactly: words through W/put on the
 * segment do not wrap (the model raises past the end), rw/ww wrap, a byte
 * index past ffff raises; an object is a view of 7a bytes (shorter at the
 * end of the segment) and its fields are checked against it.
 */
#include <stdlib.h>
#include <string.h>

#include "internal.h"

/* ---- lists ----------------------------------------------------------------- */
void cycle_free_lists(Cycle *c) {
    free(c->sounds.v); free(c->script_audio.v); free(c->music.v); free(c->events.v);
    free(c->requests.v); free(c->graphics.v); free(c->timers.v);
    c->sounds.v = 0; c->script_audio.v = 0; c->music.v = 0; c->events.v = 0;
    c->requests.v = 0; c->graphics.v = 0; c->timers.v = 0;
    cycle_clear_lists(c);
    c->sounds.cap = c->script_audio.cap = c->music.cap = c->events.cap = 0;
    c->requests.cap = c->graphics.cap = c->timers.cap = 0;
}
void cycle_clear_lists(Cycle *c) {
    c->sounds.n = c->script_audio.n = c->music.n = c->events.n = 0;
    c->requests.n = c->graphics.n = c->timers.n = 0;
}

/* a sound effect through the cycle's list (GameCycle: each one also goes to
 * the host as it comes); cycle_request: the other requests */
static void sound_list(Cycle *c, unsigned value) {
    PUSH(c->sounds, u16(value));
    if (c->level == LEVEL_GAME && c->host.audio) c->host.audio(c->host.ctx, REQ_EFFECT, value);
}
void cycle_sound(Cycle *c, unsigned value) {
    sound_list(c, value);
    if (!c->live) PUSH(c->requests, (uint32_t)REQ_EFFECT << 16 | u16(value));
}
void cycle_request(Cycle *c, int kind, unsigned value) {
    if (!c->live) PUSH(c->requests, (uint32_t)kind << 16 | u16(value));
    if (c->host.audio && kind != REQ_EFFECT) c->host.audio(c->host.ctx, kind, value);
}
void cycle_call_graphics(Cycle *c, const GfxCall *call) {
    if (!c->live) PUSH(c->graphics, *call);
    if (c->host.graphics) c->host.graphics(c->host.ctx, call);
}

/* ---- set-up --------------------------------------------------------------- */
void cycle_init(Cycle *c, Truck *k, uint8_t *mem, int level, int track, int layout) {
    memset(c, 0, sizeof *c);
    c->k = k; c->mem = mem; c->level = level; c->track = track; c->layout = layout;
    c->segment = level == LEVEL_GAME ? 0x0d98 : 0x3000;
    c->script_limit = level == LEVEL_GAME ? 5000000 : 1000;
    uint8_t *m = mem;
    memset(m, 0, 65536);
    k->data = k->original; k->ndata = k->noriginal;   /* the model's data at the start */
    memcpy(m, k->data, k->ndata < 65536 ? k->ndata : 65536);
    for (unsigned p = 0x31aa; p < 0x3212; p += 4) put(m, p + 2, c->segment);
    put(m, 0xb26, 0xb28);
    for (int n = 0; n < 64; n++) put(m, 0xb28 + n * 0x7a, n < 63 ? 0xb28 + (n + 1) * 0x7a : 0);
    for (unsigned p = 0x29a8; p < 0x29b2; p += 2) put(m, p, 0);
    memset(m + 0x2a9a, 0, 0x4c0); memset(m + 0x2f5a, 0, 64);
    put(m, 0x1a6, 0); m[0x622] = 0;
    /* the terrain in the segment, as 1e7e leaves it */
    memcpy(m + 0x540c, k->terrain[track], TERRAIN_BYTES);
    k->ds = m;
    for (int i = 0; i < 4; i++) c->truck_addresses[i] = TRUCK_ADDRESSES[i];
    if (level >= LEVEL_LAPS) {
        if ((layout & 7) != track) k->error = OFF_NOT_MODELLED;   /* layout and terrain do not match */
        m[0x1a9] = u8(layout); m[0x1aa] = u8(layout & 7);
    }
    if (level == LEVEL_GAME) { k->data = m; k->ndata = 65536; }
}

/* ---- the pool ---------------------------------------------------------------- */
void cycle_append(Cycle *c, unsigned head, unsigned at, int front) {
    unsigned nxt = front ? MW(c, head) : 0, prev = front ? 0 : MW(c, head + 2);
    MPUT(c, at, nxt); MPUT(c, at + 2, prev);
    MPUT(c, (nxt ? nxt : head) + 2, at); MPUT(c, prev ? prev : head, at);
}
void cycle_unlink(Cycle *c, unsigned head, unsigned at) {
    unsigned nxt = MW(c, at), prev = MW(c, at + 2);
    MPUT(c, (nxt ? nxt : head) + 2, prev); MPUT(c, prev ? prev : head, nxt);
}
void cycle_release(Cycle *c, unsigned at) {
    MPUT(c, at, MW(c, 0xb26)); MPUT(c, 0xb26, at);
}
uint16_t cycle_allocate(Cycle *c, unsigned template_, unsigned parent) {
    unsigned at = MW(c, 0xb26);
    if (!at || ERR) return 0;   /* the allocator returns carry, with no object */
    MPUT(c, 0xb26, MW(c, at)); cycle_append(c, 0x29a8, at, 0);
    OPUT(c, at, 0x74, parent); OPUT(c, at, 4, 0x5858);
    static const unsigned zero[8] = {0xa, 0x4c, 0x5c, 0x60, 0x66, 0x6c, 0x76, 0x78}, top[4] = {0x5e, 0x1a, 0x64, 0x12};
    for (int i = 0; i < 8; i++) OPUT(c, at, zero[i], 0);
    for (int i = 0; i < 4; i++) OPUT(c, at, top[i], 0x8000);
    long long pc = template_;
    while (!ERR) {
        unsigned op = DW(c, pc);
        if (ERR || op == 0x7a) break;
        unsigned field = op & 0x3fff, mode = op >> 14;
        if (mode == 0) { unsigned v = DB(c, pc + 2); OSET(c, at, field, v); pc += 3; }
        else if (mode == 1) { unsigned v = DW(c, pc + 2); OPUT(c, at, field, v); pc += 4; }
        else if (mode == 2) { unsigned v = MB(c, (long long)parent + field); OSET(c, at, field, v); pc += 2; }
        else { unsigned v = MW(c, (long long)parent + field); OPUT(c, at, field, v); pc += 2; }
    }
    OPUT(c, at, 0x3c, pc + 2); OPUT(c, at, 0x3e, c->segment);
    return ERR ? 0 : u16(at);
}
/* the active list; a cycle in it is the model's error (the original would loop) */
int cycle_active(Cycle *c, uint16_t *out, int cap) {
    static uint8_t seen[65536];
    unsigned at = MW(c, 0x29a8);
    int n = 0;
    memset(seen, 0, sizeof seen);
    while (at && !ERR) {
        if (seen[at]) { fail(c, OFF_HANG); break; }
        seen[at] = 1;
        if (n == cap) { fail(c, OFF_OVERFLOW); break; }
        out[n++] = u16(at);
        at = MW(c, at);
    }
    return n;
}
void cycle_spatial(Cycle *c, unsigned at, int insert) {
    unsigned slot = OW(c, at, 0x68);
    /* the original removal shifts by FOUR, insertion by THREE */
    unsigned byte_offset = slot >> (insert ? 3 : 4);
    unsigned mask = DB(c, (insert ? 0x2f9a : 0x2fa2) + (slot & 7));
    unsigned rect = OW(c, at, 0x6a);
    for (int axis = 0; axis < 2; axis++) {
        unsigned field = axis ? 0x26 : 0x22, base = axis ? 0x2d7a : 0x2b1a;
        long long start = (long long)(u16(OW(c, at, field) + DW(c, rect + axis * 4)) & 65532) + base + byte_offset;
        unsigned count = DW(c, rect + axis * 4 + 2);
        if (ERR) return;
        for (unsigned kk = 0; kk < count; kk++) {
            unsigned pos = u16(start + kk * 4);
            c->mem[pos] = u8(insert ? (c->mem[pos] | mask) : (c->mem[pos] & mask));
        }
    }
}
void cycle_register(Cycle *c, unsigned at, unsigned flags, unsigned rect) {
    OPUT(c, at, 0x66, flags); OPUT(c, at, 0x6a, rect);
    if (flags) {
        /* LOOPNE + JCXZ leaves slot 31 unassigned, exactly as 0ec9..0ee5 */
        for (unsigned slot = 0; slot < 31; slot++)
            if (!MW(c, 0x2f5a + slot * 2)) { OPUT(c, at, 0x68, slot); MPUT(c, 0x2f5a + slot * 2, at); break; }
    }
}
void cycle_contacts(Cycle *c) {
    static uint16_t list[65536];
    int n = cycle_active(c, list, 65536);
    for (int i = 0; i < n && !ERR; i++) {
        unsigned at = list[i], flags = 0;
        if (OW(c, at, 0x6c)) {
            uint32_t axes[2];
            unsigned rect = OW(c, at, 0x6a);
            for (int axis = 0; axis < 2; axis++) {
                unsigned field = axis ? 0x26 : 0x22, base = axis ? 0x2d7a : 0x2b1a;
                long long start = (long long)(u16(OW(c, at, field) + DW(c, rect + axis * 4)) & 65532) + base;
                uint32_t bits = 0;
                unsigned count = DW(c, rect + axis * 4 + 2);
                if (ERR) return;
                for (unsigned kk = 0; kk < count; kk++)
                    for (int byte = 0; byte < 4; byte++) bits |= (uint32_t)c->mem[u16(start + kk * 4 + byte)] << (byte * 8);
                axes[axis] = bits;
            }
            uint32_t bits = axes[0] & axes[1];
            /* 0e7d..0eb9: each byte from its bit 7 down, slot = 8 * byte + 7 - bit;
             * the high word is walked from 2f68 + 2 (slot 8), not from slot 16 */
            for (int b = 0; b < 32; b++)
                if (bits >> b & 1) {
                    int slot = 8 * (b >> 3) + 7 - (b & 7) - (b >= 16 ? 8 : 0);
                    flags |= MW(c, (long long)MW(c, 0x2f5a + slot * 2) + 0x66);
                }
        }
        OPUT(c, at, 0x6e, flags);
    }
}
void cycle_spawn(Cycle *c, const Spawn *e) {
    unsigned at = cycle_allocate(c, e->template_, 0);
    if (ERR) return;
    /* the original callers ignore carry and may write through a stale SI:
     * the model stops there instead of dropping the effect */
    if (!at) { fail(c, OFF_POOL); return; }
    if (!ofits(c, at, 0x2e, 2)) return;
    vm_position(c->mem + at, e);
    if (e->sound >= 0) sound_list(c, (unsigned)e->sound);
    MPUT(c, 0x1a6, MW(c, 0x1a6) | e->flags_or);
    SpawnEvent ev = {*e, u16(at)};
    PUSH(c->events, ev);
}

/* ---- the trucks -------------------------------------------------------------- */
/* 1769: CPU trucks (+2 & 0x02) ask the driver model (2189), the others read
 * their controls (1508) for the player in +73 */
unsigned cycle_command(Cycle *c, unsigned ta, int index) {
    if (MB(c, ta + 2) & 2) return ai_command(c->k, c->mem, ta);
    if (c->live_input) {
        int r = ctl_read_controls(c->mem, MB(c, ta + 0x73));
        if (r < 0) { fail(c, OFF_BAD_ADDRESS); return 0; }
        return (unsigned)r;
    }
    return c->controls[index];
}
void cycle_checkpoint_update(Cycle *c, uint8_t *t, const TruckStep *g) {
    if (c->level < LEVEL_LAPS) return;   /* the phase-8 boundary */
    MPUT(c, 0x1a6, rules_update(c->k, c->k->data, c->k->ndata, t, g->x, g->y, c->layout, MW(c, 0x1a6)));
}
void cycle_update_truck(Cycle *c, unsigned at) {
    unsigned ta = OW(c, at, 6);
    int index = -1;
    for (int i = 0; i < 4; i++) if (c->truck_addresses[i] == ta) { index = i; break; }
    if (ERR) return;
    if (index < 0) { fail(c, OFF_NOT_MODELLED); return; }   /* not a truck */
    if (at + OBJECT_SIZE > 65536) { fail(c, OFF_BAD_ADDRESS); return; }
    uint8_t *t = c->mem + c->truck_addresses[index], *o = c->mem + at;
    if (W(o, 0x6e) & W(o, 0x6c)) {
        uint8_t *trucks[4], *objects[4], sounds[8];
        for (int i = 0; i < 4; i++) {
            trucks[i] = c->mem + c->truck_addresses[i];
            if ((long long)c->object_addresses[i] + OBJECT_SIZE > 65536) { fail(c, OFF_BAD_ADDRESS); return; }
            objects[i] = c->mem + c->object_addresses[i];
        }
        int n = truck_collide(c->k, trucks, objects, index, sounds);
        for (int i = 0; i < n; i++) sound_list(c, sounds[i]);
        if (ERR) return;
    }
    put(t, 0x59, W(o, 0x6e));
    unsigned command = cycle_command(c, ta, index);
    if (ERR) return;
    truck_throttle(c->k, t, command);
    Spawn e;
    if (truck_nitro(c->k, t, c->track, &e)) cycle_spawn(c, &e);
    if (ERR) return;
    TruckStep g;
    memset(&g, 0, sizeof g);
    g.rng = u16(MW(c, 0x167));
    truck_integrate(c->k, t, &g);
    /* each support's requests are served before the next support */
    for (int i = 0; i < 4 && !ERR; i++) {
        int old = g.nev;
        Surface s;
        truck_support(c->k, t, &g, c->track, SUPPORTS[i], ANGLES[i], &s);
        for (int j = old; j < g.nev && !ERR; j++) {
            if (g.ev[j][0] == EV_WALL_SOUND) sound_list(c, g.ev[j][1]);
            else if (truck_impact_effect(c->k, t, c->track, SUPPORTS[i], g.ev[j][0] == EV_SURFACE_IMPACT_2 ? 2 : 0,
                                         c->mem[0x622], &e))
                cycle_spawn(c, &e);
        }
        g.nev = 0;   /* served */
    }
    if (ERR) return;
    Pose pose;
    truck_traction(c->k, t); truck_pose(c->k, t, &pose); truck_commit(c->k, t, o, &g, c->track);
    MPUT(c, 0x167, g.rng); MPUT(c, 0x4f7a, g.x); MPUT(c, 0x4f7c, g.y); c->mem[0x4f84] = g.flags;
    cycle_checkpoint_update(c, t, &g);
    OPUT(c, at, 0x6a, 0x5302 + ((u8(t[0x82] + 8) & 240) >> 1));
}

/* ---- the scripts -------------------------------------------------------------- */
void cycle_script(Cycle *c, unsigned at, unsigned pc) {
    long long i;
    for (i = 0; i < c->script_limit && !ERR; i++) {
        unsigned redirect = OW(c, at, 0x4c);
        if (redirect) {
            /* [si+4c+bx], bx = +4c >> 1: anywhere in the data segment */
            unsigned ta = (at + 0x4c + (redirect >> 1)) & 65535, target = c->mem[ta] | c->mem[(ta + 1) & 65535] << 8;
            if (target) pc = target;
            OPUT(c, at, 0x4c, 0);
        }
        unsigned op = DW(c, pc), ax = DW(c, pc + 2), size = (op & 7) + 1, handler = op & 0x7ff8;
        if (ERR) return;
        switch (handler) {
        case 0x1c8: cycle_update_truck(c, at); break;
        case 0x100: OPUT(c, at, 0x5c, op & 7 ? ax : 0); break;
        case 0xc8: cycle_register(c, at, ax, DW(c, pc + 4)); break;
        case 0x170: OPUT(c, at, 0xa, OW(c, at, 0xa) | ax); break;
        case 0x178: OPUT(c, at, 0xa, OW(c, at, 0xa) & u16(~ax)); break;
        case 0xd0:
            OPUT(c, at, 0x6c, op & 7 ? ax : 0);
            if (op & 7) OPUT(c, at, 0x70, pc + DW(c, pc + 4));
            break;
        case 0x10: OPUT(c, at, 0x46, ax); OPUT(c, at, 0x4a, pc + 2); break;
        case 0x18: {
            unsigned old = OW(c, at, 0x46);
            if (old) OPUT(c, at, 0x46, old - 1);
            if (old != 1) pc = OW(c, at, 0x4a);
            break;
        }
        case 0x150: break;
        case 0x158: OPUT(c, at, 4, 0); break;
        case 0x108:
            OPUT(c, at, 0x64, op & 7 ? ax : 0x7fff);
            if (op & 7) OPUT(c, at, 0x62, pc + DW(c, pc + 4));
            break;
        case 0xf8:
            if ((op & 7) == 1) { OPUT(c, at, 0x5e, ax | (OW(c, at, 0x5e) & 0x7fff)); break; }   /* 0d4b: the speed only */
            {
                unsigned seq = u16(pc + ax);
                OPUT(c, at, 0x56, seq);
                unsigned base = DW(c, pc + 4);
                if (!(base & 32768)) OPUT(c, at, 0x5a, base);
                unsigned word = DW(c, seq);
                OPUT(c, at, 0x58, 2); OPUT(c, at, 0x5c, word & 255); OPUT(c, at, 0x5e, word >> 8);
            }
            break;
        case 0x30: pc = u16(pc + ax); break;
        case 0x2d0: c->mem[0x622] = u8(c->mem[0x622] + 1); break;
        case 0x2d8: c->mem[0x622] = u8(c->mem[0x622] - 1); break;
        default: pc = cycle_extra_opcode(c, at, pc, op, ax);
        }
        if (ERR) return;
        if (c->has_step_override) {
            c->has_step_override = 0;
            pc = u16(pc + c->step_override_step);
            if (c->step_override_stop) break;
            continue;
        }
        pc = u16(pc + size * 2);
        if (op & 32768) break;
    }
    if (ERR) return;
    if (i == c->script_limit) { fail(c, OFF_HANG); return; }   /* the script did not yield */
    OPUT(c, at, 0x3c, pc);
}

/* RaceLapCycle's instructions (and, first, GameCycle's in screens.c) */
unsigned cycle_extra_opcode(Cycle *c, unsigned at, unsigned pc, unsigned op, unsigned ax) {
    if (c->level == LEVEL_GAME && screens_opcode(c, at, &pc, op, ax)) return pc;
    if (c->level < LEVEL_LAPS) { fail(c, OFF_NOT_MODELLED); return pc; }
    unsigned handler = op & 0x7ff8;
    switch (handler) {
    case 0x188: { unsigned target = DW(c, pc + 4); MPUT(c, target, MW(c, target) | ax); break; }
    case 0x190: { unsigned target = DW(c, pc + 4); MPUT(c, target, MW(c, target) & u16(~ax)); break; }
    case 0x198: OPUT(c, at, 8, ax & MW(c, DW(c, pc + 4))); break;
    case 0x1a8: OPUT(c, at, DW(c, pc + 4), ax); break;
    case 0x40: if (OW(c, at, 8)) pc = u16(pc + ax); break;
    case 0x48: if (!OW(c, at, 8)) pc = u16(pc + ax); break;
    case 0x1b0: PUSH(c->script_audio, u16(ax)); break;
    /* phase 12: the bonus on the track (object 340e) */
    case 0xe8: bonus_visibility(c->mem, at, ax); break;
    case 0x1d0: bonus_place(c->mem, at); break;
    case 0x2e0: bonus_project(c->mem, at); break;
    case 0x1d8: bonus_pick_up(c, at); break;
    default: fail(c, OFF_NOT_MODELLED);
    }
    return pc;
}

void cycle_sweep_retired(Cycle *c) {
    unsigned at = MW(c, 0x29ac);
    while (at && !ERR) {
        unsigned nxt = MW(c, at);
        if (!MW(c, at + 0x78)) { cycle_unlink(c, 0x29ac, at); cycle_release(c, at); }
        at = nxt;
    }
}

void cycle_scheduler(Cycle *c) {
    unsigned at = MW(c, 0x29a8);
    while (at && !ERR) {
        if (!(MW(c, 0x1a6) & 8) || OW(c, at, 0xa) & 8) {
            if (OW(c, at, 0x66)) cycle_spatial(c, at, 0);
            unsigned pc = OW(c, at, 0x3c);
            if (OW(c, at, 0x6e) & OW(c, at, 0x6c)) { OPUT(c, at, 0x72, pc); pc = OW(c, at, 0x70); }
            else {
                int expired = 0;
                if (!(OW(c, at, 0x12) & 32768)) {
                    OPUT(c, at, 0x12, OW(c, at, 0x12) - 1);
                    if (OW(c, at, 0x12) & 32768 && OW(c, at, 0x14)) { pc = OW(c, at, 0x14); expired = 1; }
                }
                if (!expired && OW(c, at, 0x60) >= OW(c, at, 0x64)) { pc = OW(c, at, 0x62); OPUT(c, at, 0x60, 0); }
            }
            if (ERR) return;
            cycle_script(c, at, pc);
            if (ERR) return;
            if (!OW(c, at, 4)) {
                if (OW(c, at, 0x66)) MPUT(c, 0x2f5a + OW(c, at, 4) * 2, 0);
                unsigned parent = OW(c, at, 0x74);
                if (parent) {
                    MPUT(c, parent + 0x78, MW(c, parent + 0x78) - 1);
                    if (OW(c, at, 0xa) & 0x2000) MPUT(c, parent + 0x76, 0);
                }
                unsigned nxt = OW(c, at, 0);
                cycle_unlink(c, 0x29a8, at);
                if (OW(c, at, 0x78)) cycle_append(c, 0x29ac, at, 1);
                else cycle_release(c, at);
                if (!nxt) return;   /* the original deletion of the last item bypasses the sweep */
                at = nxt;
                continue;
            }
            if (OW(c, at, 0x66)) cycle_spatial(c, at, 1);
            if (!(OW(c, at, 0x5e) & 32768)) {
                if (at + OBJECT_SIZE > 65536) { fail(c, OFF_BAD_ADDRESS); return; }
                vm_animation(c->k, c->mem + at);
            }
        }
        if (!(OW(c, at, 0x1a) & 32768)) {
            unsigned prev = 0x299a, nxt = MW(c, prev + 0x16);
            while (nxt && OW(c, at, 0x18) >= MW(c, nxt + 0x18) && !ERR) { prev = nxt; nxt = MW(c, prev + 0x16); }
            MPUT(c, prev + 0x16, at); OPUT(c, at, 0x16, nxt);
        }
        at = OW(c, at, 0);
    }
    if (!ERR) cycle_sweep_retired(c);
}

int cycle_frame(Cycle *c, const uint8_t controls[4]) {
    for (int i = 0; i < 4; i++) c->controls[i] = controls[i];
    c->sounds.n = 0; c->events.n = 0;
    if (c->level >= LEVEL_LAPS) c->script_audio.n = 0;
    MPUT(c, 0x29b0, 0);   /* consumed and cleared by the drawing, outside this slice */
    cycle_scheduler(c);
    if (!ERR) cycle_contacts(c);
    static uint16_t list[65536];
    if (!ERR) cycle_active(c, list, 65536);   /* the model's result lists them */
    return c->level >= LEVEL_LAPS && (MW(c, 0x1a6) & 4) != 0;
}

/* 371e once race_exit is set: preparation, ranking, awards and records */
void cycle_finish(Cycle *c) { awards_finish_race(c->k, c->mem); }

/* ---- the session ---------------------------------------------------------------
 * 12ae drivers: +0 driver 0..3, +73 control type 0/2/1/3, identity 'XXX' +
 *      '112768' + 8 (MMDDYY in truck order +08..+0d).
 * 12f9 new_game: grid places 3,2,1,0, ranking pointers 9cec, all trucks CPU,
 *      two credits (+1), level and rating 6.
 * 1339 race_start: CPU levels (20c6), the four trucks created by 1383, the
 *      object pointers 4c6a.., 4f82. */
void session_drivers(uint8_t *m) {
    static const uint8_t control[4] = {0, 2, 1, 3};
    for (int i = 0; i < 4; i++) {
        unsigned t = TRUCK_ADDRESSES[i];
        m[t] = u8(i); m[t + 0x73] = control[i]; ww(m, t + 5, 0x5858); m[t + 7] = 0x58;
        ww(m, t + 0xa, 0x3732); ww(m, t + 8, 0x3131); ww(m, t + 0xc, 0x3836); m[t + 0xe] = 8;
    }
}
void session_new_game(uint8_t *m) {
    for (int i = 0; i < 4; i++) {
        unsigned t = TRUCK_ADDRESSES[i], place = 3 - i;
        m[t + 0x3b] = u8(place); ww(m, 0x9cec + 2 * place, t);
        m[t + 2] = 2; m[t + 1] = 2; m[t + 0x5b] = 6; m[t + 0x5c] = 6;
    }
}
/* 1383 after the allocation, then 13b0 */
void session_truck_start(Cycle *c, unsigned ta, unsigned at) {
    uint8_t *m = c->mem;
    if (m[ta + 2] & 1) {
        m[ta + 0x5f] = m[ta + 0x60] = m[ta + 0x5e] = m[ta + 0x61] = 0;
        ww(m, ta + 0xf, 0); ww(m, ta + 0x11, 0); m[ta + 0x5d] = 0x19; m[ta + 0x5b] = 6; m[ta + 0x5c] = 6;
    }
    int layout = m[0x1a9];
    unsigned kk = layout * 2;
    unsigned heading = rw(m, 0x52c2 + kk);
    ww(m, ta + 0x81, heading); ww(m, ta + 0x83, heading);
    unsigned grid = u16(0x52e2 + (heading & 0x8000 ? 0x10 : 0) + m[ta + 0x3b] * 4);
    unsigned height = rw(m, 0x52a2 + kk);
    unsigned x = u16(rw(m, 0x5262 + kk) + rw(m, grid)), y = u16(rw(m, 0x5282 + kk) + rw(m, grid + 2));
    ww(m, ta + 0x79, x); ww(m, at + 0x22, x >> 5); ww(m, ta + 0x7b, y); ww(m, at + 0x26, y >> 5);
    static const unsigned heights[9] = {0x7d, 0x8e, 0x9c, 0xaa, 0xb8, 0x92, 0xa0, 0xae, 0xbc};
    for (int i = 0; i < 9; i++) ww(m, ta + heights[i], height);
    for (unsigned f = 0x20; f <= 0x2c; f += 4) ww(m, at + f, 0);
    static const unsigned zero[9] = {0x90, 0x9e, 0xac, 0xba, 0x75, 0x85, 0x55, 0x57, 0x59};
    for (int i = 0; i < 9; i++) ww(m, ta + zero[i], 0);
    m[ta + 0x62] = 0;
    uint8_t t[TRUCK_SIZE];
    memcpy(t, m + ta, TRUCK_SIZE);
    rules_initialize(c->k, c->k->data, c->k->ndata, t, layout);
    memcpy(m + ta, t, TRUCK_SIZE);
    ai_start(c->k, m, ta);
    if (m[ta + 2] & 2) ai_cpu_parameters(c->k, m, ta, 1);
    else {
        /* DX still holds start Y >> 5 here: 2172 only calls 21f3 for the CPU */
        memcpy(t, m + ta, TRUCK_SIZE);
        truck_human_parameters(c->k, t, y >> 5, c->acceleration_fix);
        memcpy(m + ta, t, TRUCK_SIZE);
    }
}
void session_race_start(Cycle *c) {
    static const unsigned pointers[4] = {0x31aa, 0x31ae, 0x31b2, 0x31b6};
    uint8_t *m = c->mem;
    ai_cpu_levels(c->k, m);
    for (int i = 0; i < 4 && !ERR; i++) {
        unsigned at = cycle_allocate(c, rw(m, pointers[i]), 0);
        if (ERR) return;
        if (!at) { fail(c, OFF_POOL); return; }   /* 1383: the allocation failed */
        session_truck_start(c, TRUCK_ADDRESSES[i], at);
        ww(m, 0x4c6a + 2 * i, at); c->object_addresses[i] = u16(at);
    }
    if (!ERR) ww(m, 0x4f82, m[0x524a + m[0x1aa]] << 8);
}

/* ---- the bonus on the track (phase 12): object 340e ---------------------------
 * op 0e8 -> 0cfd  +1a; a negative argument sets only bit 15 from bit 0.
 * op 1d0 -> 0508  place: one of 16 per track (49a, 51a), kind in 0496.
 * op 2e0 -> 161c  screen position from the terrain height under it (165f).
 * op 1d8 -> 056e  pick-up: sound 4/5/6 by driver (none for Ivan), then nitro
 *                 (+1..+9, 99 at most) or money (+1..+9 BCD on +62), 0494/0495
 *                 count the pick-ups of this race; two popups from 31ea. */
void bonus_visibility(uint8_t *m, unsigned o, unsigned ax) {
    if (ax & 0x8000) ww(m, o + 0x1a, ((ax & 1) << 15) | (rw(m, o + 0x1a) & 0x7fff));
    else ww(m, o + 0x1a, ax);
}
void bonus_place(uint8_t *m, unsigned o) {
    unsigned track = m[0x1aa];
    unsigned index = (ai_random_word(m) & 15) | u8(track << 4);
    unsigned at = u16(rw(m, 0x51a + track * 2) + s8(m[(0x49a + index) & 65535]) * 4);
    ww(m, o + 0x22, rw(m, at) >> 5);
    unsigned y = rw(m, at + 2) >> 5;
    ww(m, o + 0x26, y); ww(m, o + 0x18, y);
    m[0x496] = u8(ai_random_word(m) & 1);
    ww(m, o + 0x5a, m[0x496] ? 0x1bd : 0x1bc);
}
static long long sar(long long v, int n) { return v >= 0 ? v >> n : ~((~v) >> n); }
void bonus_screen(uint8_t *m, unsigned x, unsigned y, unsigned height, uint16_t *sx, uint16_t *sy) {
    unsigned elevation = u16(-s8(u8(height - 0x80)));
    *sy = u16(elevation + sar((long long)s16(y) * 0x471, 16) + rw(m, 0x4f80));
    unsigned q = (u16(0x2c00 - y) * 0x5555u) >> 16;
    *sx = u16(sar(s16(x), 5) + sar(s16(q), 5) + rw(m, 0x4f7e));
}
void bonus_project(uint8_t *m, unsigned o) {
    unsigned x = u16(rw(m, o + 0x22) << 5), y = u16(rw(m, o + 0x26) << 5);
    unsigned cell = u16(rw(m, 0x6abc + (y >> 8) * 2) + (x >> 8) * 3);
    uint16_t sx, sy;
    bonus_screen(m, x, y, m[(cell + 2) & 65535], &sx, &sy);
    ww(m, o + 0x2a, sx); ww(m, o + 0x2e, sy);
}
/* 06cf clears SI before saving it: a full pool leaves SI = 0, so the fields
 * land at DS:0000+offset, exactly as in the original */
static unsigned popup(Cycle *c, unsigned template_) { return cycle_allocate(c, template_, 0); }
void bonus_pick_up(Cycle *c, unsigned o) {
    uint8_t *m = c->mem;
    unsigned contacts = rw(m, o + 0x6e), truck = TRUCK_ADDRESSES[3];
    for (int i = 0; i < 3; i++) if (contacts & (1u << i)) { truck = TRUCK_ADDRESSES[i]; break; }
    unsigned driver = m[truck];
    if (driver != 3) sound_list(c, m[(0x497 + driver) & 65535]);
    unsigned sx = rw(m, o + 0x2a), sy = rw(m, o + 0x2e), template_ = rw(m, 0x31ea);
    if (m[0x496]) {
        unsigned count = u8(m[0x495] + 1);
        count = count < 9 ? count : 9; m[0x495] = u8(count);
        unsigned total = u8(count + m[truck + 0x5d]);
        m[truck + 0x5d] = u8(total < 0x63 ? total : 0x63);
        ww(m, 0x1a6, rw(m, 0x1a6) | 0x40);
        unsigned p = popup(c, template_);
        if (ERR) return;
        ww(m, p + 0x2e, sy); ww(m, p + 0x2a, sx - 0x10); ww(m, p + 0x18, 0); ww(m, p + 0x5a, 0x209); ww(m, p + 0x5c, u16(s8(count)));
        p = popup(c, template_);
        if (ERR) return;
        ww(m, p + 0x2e, sy); ww(m, p + 0x2a, sx); ww(m, p + 0x18, 0); ww(m, p + 0x5c, 0);
        ww(m, p + 0x5a, count == 1 ? 0x213 : 0x214);
    } else {
        unsigned count = u8(m[0x494] + 1);
        count = count < 9 ? count : 9; m[0x494] = u8(count);
        m[truck + 0x62] = u8(add_daa(count, m[truck + 0x62], 0, 0));
        unsigned p = popup(c, template_);
        if (ERR) return;
        ww(m, p + 0x2e, sy); ww(m, p + 0x2a, sx - 0x10); ww(m, p + 0x18, 0); ww(m, p + 0x5a, 0x209); ww(m, p + 0x5c, u16(s8(count)));
        p = popup(c, template_);
        if (ERR) return;
        ww(m, p + 0x2e, sy + 1); ww(m, p + 0x2a, sx - 1); ww(m, p + 0x18, 0); ww(m, p + 0x5c, 0); ww(m, p + 0x5a, 0x215);
    }
}

/* ---- the race loop around the scheduler (014f..01cd) --------------------------- */
/* 03be: four BCD digits +5 per update unless paused; 9999 on overflow */
void loop_race_clock(uint8_t *m) {
    if (rw(m, 0x1a6) & 8) return;
    unsigned clock = rw(m, 0x3f3), cf;
    unsigned lo = add_daa(clock & 255, 5, 0, &cf), hi = add_daa(clock >> 8, 0, cf, &cf);
    ww(m, 0x3f3, cf ? 0x9999 : lo | hi << 8);
}
static void draw_si(Cycle *c, unsigned entry, unsigned si) {
    GfxCall call = {0};
    call.entry = u8(entry); call.has = GA_SI; call.si = u16(si);
    cycle_call_graphics(c, &call);
}
/* 045c: three digits of the clock to 0440, drawn by es:78 */
void loop_clock_digits(Cycle *c, int draw) {
    uint8_t *m = c->mem;
    unsigned clock = rw(m, 0x3f3);
    m[0x442] = u8((clock & 255) >> 4); m[0x441] = u8((clock >> 8) & 15); m[0x440] = u8(clock >> 12);
    if (draw) draw_si(c, 0x78, 0x440);
}
/* 04c2: when 1a6 & 40, two digits per truck (AAM 10) to 0440, drawn by es:80 */
void loop_nitro_digits(Cycle *c, int draw) {
    uint8_t *m = c->mem;
    unsigned flags = rw(m, 0x1a6);
    if (!(flags & 0x40)) return;
    ww(m, 0x1a6, flags & 0xffbf);
    for (int i = 0; i < 4; i++) {
        unsigned nitro = m[TRUCK_ADDRESSES[i] + 0x5d];
        m[0x440 + 2 * i] = u8(nitro / 10); m[0x441 + 2 * i] = u8(nitro % 10);
    }
    if (draw) draw_si(c, 0x80, 0x440);
}
/* 0488: when 1a6 & 20, lap + 1 per truck to 0440, drawn by es:7c; otherwise 04c2 */
void loop_lap_digits(Cycle *c, int draw) {
    uint8_t *m = c->mem;
    unsigned flags = rw(m, 0x1a6);
    if (!(flags & 0x20)) { loop_nitro_digits(c, draw); return; }
    ww(m, 0x1a6, flags & 0xffdf);
    for (int i = 0; i < 4; i++) m[0x440 + i] = u8(m[TRUCK_ADDRESSES[i] + 0x3c] + 1);
    if (draw) draw_si(c, 0x7c, 0x440);
}
/* 0452: alternate updates show the clock, or else the laps or the nitro */
void loop_hud(Cycle *c, int draw) {
    c->mem[0x3f9] ^= 0xff;
    if (!c->mem[0x3f9]) loop_clock_digits(c, draw);
    else loop_lap_digits(c, draw);
}
/* es:14 of the graphics module: row address + x/8 (the captures show y*40) */
int loop_screen_offset(unsigned x, unsigned y, uint16_t *out) {
    if ((int)y < 0 || y >= 200) return 0;
    *out = u16(y * 40 + (x >> 3));
    return 1;
}
/* 03dc: the panel's origin by track (03fa), the 19 digit places (041a) as
 * screen offsets and bit shifts (0448..), es:74 saves the backgrounds, clock 5,
 * then the three displays */
void loop_hud_init(Cycle *c, int draw) {
    uint8_t *m = c->mem;
    unsigned track = m[0x1aa], x0 = rw(m, 0x3fa + track * 4), y0 = rw(m, 0x3fa + track * 4 + 2);
    ww(m, 0x3f5, x0); ww(m, 0x3f7, y0);
    for (int i = 0; i < 19; i++) {
        unsigned x = u16(m[0x41a + 2 * i] + x0), y = u16(m[0x41b + 2 * i] + y0);
        uint16_t off;
        ww(m, 0x44a + 4 * i, x & 7);
        if (!loop_screen_offset(x, y, &off)) { fail(c, OFF_NOT_MODELLED); return; }   /* a row outside the observed table */
        ww(m, 0x448 + 4 * i, off);
    }
    if (draw) draw_si(c, 0x74, 0x440);
    m[0x3f9] = 0; ww(m, 0x1a6, 0x60); ww(m, 0x3f3, 5);
    loop_clock_digits(c, draw); loop_lap_digits(c, draw); loop_nitro_digits(c, draw);
}
/* 02ba..0146 after 1339: one scheduler pass while 1a6 = 80, the draw list
 * consumed, the panel set up, 1a6 = 8 (paused for the start lights), the
 * music of the track (3b05 with AL = track + 11) */
void loop_start(Cycle *c) {
    memset(c->controls, 0, 4);
    c->sounds.n = 0; c->events.n = 0; c->script_audio.n = 0;
    cycle_scheduler(c);
    if (ERR) return;
    ww(c->mem, 0x29b0, 0); loop_hud_init(c, 0); ww(c->mem, 0x1a6, 8);
    c->music.n = 0;
    PUSH(c->music, u16((c->mem[0x1aa] + 11) & 255));
}
void loop_tail(Cycle *c) {
    loop_race_clock(c->mem); loop_hud(c, 0); ww(c->mem, 0x29b0, 0);
}
