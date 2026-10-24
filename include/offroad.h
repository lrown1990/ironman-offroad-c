/* Super Off Road (Graftgold, 1990) rebuilt in C.
 *
 * The game keeps its whole state in one 64 KB data segment with the layout
 * of game.exe; the C code works on the same bytes at the same offsets, so
 * that every step can be compared with the Python models of OFFROAD CODE
 * OPUS (its tools folder), which are verified against the original code and the real
 * game. Offsets in comments are the original's (data segment, or code
 * offset in segment 0000 of game.exe).
 */
#ifndef OFFROAD_H
#define OFFROAD_H
#include <stdint.h>

/* ---- the data segment ---------------------------------------------------- */
static inline uint8_t ds_b(const uint8_t *m, uint16_t a) { return m[a]; }
static inline uint16_t ds_w(const uint8_t *m, uint16_t a) {
    return (uint16_t)(m[a] | m[(uint16_t)(a + 1)] << 8);
}
static inline void ds_setb(uint8_t *m, uint16_t a, unsigned v) { m[a] = (uint8_t)v; }
static inline void ds_setw(uint8_t *m, uint16_t a, unsigned v) {
    m[a] = (uint8_t)v;
    m[(uint16_t)(a + 1)] = (uint8_t)(v >> 8);
}

/* ---- results of a step ---------------------------------------------------- */
enum {
    OFF_OK = 0,
    OFF_NOT_MODELLED = 1, /* a routine pointer the rebuild does not have */
    OFF_HANG = 2,         /* where the original never returns */
    OFF_OVERFLOW = 3,     /* an output buffer is full */
    OFF_BAD_ADDRESS = 4,  /* memory outside the 640 KB, or past the end of a file's data */
    OFF_DIVIDE = 5,       /* a DIV or IDIV the original would fault on */
    OFF_OUTSIDE_GRID = 6, /* static terrain model: a lookup outside the 44x44 cells */
    OFF_POOL = 7,         /* the object pool is full where the original writes through a stale pointer */
    OFF_TEXT = 8          /* the text printer: a command, font or colour scheme the game never uses */
};

/* ---- trucks: physics, interactions, effect scripts (src/truck.c;
 *      tools/truck_physics.py, truck_interactions.py, recover_truck_states.py) */
#define GAME_DATA_OFFSET 0x5b40 /* the data segment's initial image in game.exe */
#define TERRAIN_BYTES (44 * 44 * 3)
enum { TRUCK_SIZE = 0xc2, OBJECT_SIZE = 0x7a };
typedef struct {
    uint8_t *data; int ndata;   /* game.exe from GAME_DATA_OFFSET: the original tables */
    uint8_t *poly; int npoly;   /* poly.bin */
    uint8_t *quad; int nquad;   /* quad.bin */
    uint8_t contour[8][44 * 44 * 2];    /* contour0..7.xx */
    uint8_t terrain[8][TERRAIN_BYTES];  /* 1e7e, per track */
    uint16_t frames[25][32];    /* the pose tables at 5126 */
    int16_t trig[256], support_trig[32];
    uint8_t *ds;                /* the live data segment (terrain at 540c), or 0: static model */
    uint8_t impact_count;       /* EffectVM's counter (the game keeps it at DS:0622) */
    int error;
    uint8_t *original; int noriginal;   /* the image as loaded (data may point at the live segment) */
} Truck;
enum { EV_SURFACE_IMPACT, EV_SURFACE_IMPACT_2, EV_WALL_SOUND };
typedef struct {
    uint16_t rng, x, y;
    uint8_t flags, nev;
    uint16_t ev[8][2];          /* kind, support (impacts) or sound id (walls) */
} TruckStep;
typedef struct {
    uint16_t reference;
    int16_t quadrant;           /* -1: a plain cell */
    uint8_t kind, angle, sx, sy, blocked;
    uint16_t height;
} Surface;
typedef struct {
    uint16_t heights[4], average_height, average_ground;
    uint8_t axis_a, axis_b, direction;
    uint16_t frame, shadow_frame;
    int16_t render_lift;
} Pose;
typedef struct {
    uint16_t template_, x, y, screen_x, screen_y;
    int16_t sound;              /* -1: none */
    uint16_t flags_or;          /* ORed into DS:01a6 */
} Spawn;

Truck *truck_load(const char *dir);   /* the folder of the original files; 0 on failure */
void truck_free(Truck *k);
void truck_build_terrain(Truck *k, int track, uint8_t *out);
void truck_cell(Truck *k, int track, unsigned x, unsigned y, uint16_t *ref, uint8_t *height);
void truck_surface(Truck *k, int track, unsigned x, unsigned y, Surface *s);
void truck_throttle(Truck *k, uint8_t *t, unsigned controls);
void truck_integrate(Truck *k, uint8_t *t, TruckStep *g);
void truck_support(Truck *k, uint8_t *t, TruckStep *g, int track, unsigned p, unsigned angular_offset, Surface *s);
void truck_wall(Truck *k, uint8_t *t, TruckStep *g, unsigned p, const Surface *s);
void truck_traction(Truck *k, uint8_t *t);
void truck_select_pose(Truck *k, const uint16_t heights[4], const uint16_t ground[4], unsigned heading, Pose *r);
void truck_pose(Truck *k, uint8_t *t, Pose *r);
void truck_project(Truck *k, int track, unsigned x, unsigned y, unsigned z, uint16_t *sx, uint16_t *sy);
void truck_commit(Truck *k, uint8_t *t, uint8_t *obj, const TruckStep *g, int track);
/* controls < 0: none; surfaces may be 0 */
void truck_step(Truck *k, uint8_t *t, uint8_t *obj, int track, unsigned rng, int controls,
                TruckStep *g, Pose *pose, Surface surfaces[4]);
void truck_demo_parameters(Truck *k, uint8_t *t, int level);
void truck_initial_state(Truck *k, int track, int level, uint8_t *t, uint8_t *obj);
unsigned truck_human_parameters(Truck *k, uint8_t *t, unsigned dx, int acceleration_fix);
void truck_start_coordinates(Truck *k, int track, int slot, uint16_t *x, uint16_t *y);
int truck_nitro(Truck *k, uint8_t *t, int track, Spawn *e);           /* 1: an effect */
int truck_impact_effect(Truck *k, uint8_t *t, int track, unsigned support, int kind, int count, Spawn *e);
void truck_vector_polar(Truck *k, unsigned x, unsigned y, uint16_t *angle, uint16_t *speed);
int truck_collide(Truck *k, uint8_t *trucks[4], uint8_t *objects[4], int index, uint8_t *sounds); /* sounds made */
void truck_collision_masks(Truck *k, uint8_t **objects, int n, uint16_t *flags);
/* EffectVM */
void vm_create(Truck *k, unsigned template_, uint8_t *o);
void vm_position(uint8_t *o, const Spawn *e);
void vm_animation(Truck *k, uint8_t *o);
int vm_tick(Truck *k, uint8_t *o, unsigned contacts);   /* 1: still alive */

/* ---- race rules: checkpoints, laps, results (src/rules.c; tools/race_rules.py)
 * The tables come from `data` (the original image, or the live segment);
 * k only carries the error. A truck record is TRUCK_SIZE bytes. */
uint8_t direction32(unsigned ax, unsigned bx);
unsigned packed_decimal(long long value);
int rules_records(Truck *k, const uint8_t *data, int ndata, int layout, uint16_t *out);   /* count */
void rules_reset_route(Truck *k, const uint8_t *data, int ndata, uint8_t *t, int layout);
void rules_initialize(Truck *k, const uint8_t *data, int ndata, uint8_t *t, int layout);
int rules_region(Truck *k, const uint8_t *data, int ndata, unsigned at, unsigned x, unsigned y); /* -1: outside */
unsigned rules_update(Truck *k, const uint8_t *data, int ndata, uint8_t *t, unsigned x, unsigned y, int layout, unsigned flags);
void rules_normalize_progress(Truck *k, const uint8_t *data, int ndata, uint8_t *t, int layout);
void rules_result_time(uint8_t *t);
void rules_rating(Truck *k, const uint8_t *data, int ndata, uint8_t *t, int layout);
void rules_prepare_result(Truck *k, const uint8_t *data, int ndata, uint8_t *t, int layout);
/* 38c2 on records that may be cut short (len[i] bytes): order in/out */
void rules_rank(Truck *k, uint8_t *trucks[4], const int len[4], int order[4]);

/* ---- the computer's drivers (src/ai.c; tools/truck_ai.py) ----------------
 * On the whole data segment m (64 KB); di is the truck's address. */
extern const uint16_t TRUCK_ADDRESSES[4];   /* 4c72, 4d34, 4df6, 4eb8 */
unsigned ai_random_word(uint8_t *m);
int ai_in_range(unsigned value, unsigned lo, unsigned hi);
unsigned ai_distance(uint8_t *m, unsigned di, unsigned point);
void ai_cpu_parameters(Truck *k, uint8_t *m, unsigned di, int nitro);
void ai_cpu_levels(Truck *k, uint8_t *m);
unsigned ai_nearest_point(Truck *k, uint8_t *m, unsigned di);
void ai_rubber_band(Truck *k, uint8_t *m, unsigned di);
unsigned ai_next_point(Truck *k, uint8_t *m, unsigned di);
void ai_start(Truck *k, uint8_t *m, unsigned di);
unsigned ai_command(Truck *k, uint8_t *m, unsigned di);

/* ---- keyboard and joystick as the game reads them (src/controls.c; tools/controls.py) */
int ctl_int9(uint8_t *m, const uint8_t *reads, int n);    /* one interrupt; OFF_NOT_MODELLED: F0 with no byte after */
unsigned ctl_key_down(const uint8_t *m, unsigned code);
int ctl_keyboard(uint8_t *m, unsigned at);                /* < 0: past the end of the segment */
unsigned ctl_joystick(uint8_t *m, unsigned at);
int ctl_read_controls(uint8_t *m, unsigned player);

/* the game port (201h) as the machine gives it: for each axis bit, how many
 * reads after a trigger it stays at 1 (-1: not connected, it reads 1 always),
 * the four buttons (bits 4..7, 0 = pressed); since: reads since the trigger
 * (-1: the one-shots have run out) */
typedef struct { int axes[4]; unsigned buttons; long long since; } Joystick;
void joy_init(Joystick *j, const int axes[4], unsigned buttons);
unsigned joy_read(Joystick *j);
/* 0434:0888 on the data segment with the game port j; out gets the port writes */
void joystick_read(uint8_t *m, Joystick *j, void (*out)(void *ctx, unsigned port, unsigned value), void *ctx);

/* ---- the end of a race, awards, the Ironman Club (src/awards.c; tools/race_awards.py) */
unsigned add_daa(unsigned al, unsigned value, unsigned carry, unsigned *cf);
void awards_sort_times(uint8_t *m, unsigned at);
int awards_find_time(uint8_t *m, unsigned at, unsigned value);   /* -1: none */
void awards_award(Truck *k, uint8_t *m, unsigned di, unsigned track);
void awards_finish_race(Truck *k, uint8_t *m);
void awards_sort_drivers(uint8_t *m);
void awards_driver_out(Truck *k, uint8_t *m, unsigned si);
int awards_driver_login(uint8_t *m, unsigned si);
#define RECORDS_BLOCK 0x98b9
#define RECORDS_SIZE 0x404
void awards_records_file(const uint8_t *m, uint8_t out[2 + RECORDS_SIZE]);
int awards_load_records(uint8_t *m, const uint8_t *raw, long long n);

/* ---- the race: the object pool and its scripts, the trucks' update, laps,
 *      the session, the bonus on the track, the race loop (src/race.c;
 *      tools/race_cycle.py, race_lap_cycle.py, race_session.py, track_bonus.py,
 *      race_loop.py). One structure for the chain of classes of the model:
 *      `level` says which one it is. */
enum { LEVEL_CYCLE, LEVEL_LAPS, LEVEL_SESSION, LEVEL_GAME };
enum { REQ_EFFECT, REQ_SCRIPT_AUDIO, REQ_SCRIPT_AUDIO_B, REQ_MUSIC, REQ_MUSIC_OFF };
typedef struct { Spawn e; uint16_t object; } SpawnEvent;
/* a call to the graphics module as the model records it: which arguments it has */
enum { GA_AX = 1, GA_AL = 2, GA_SI = 4, GA_BL = 8, GA_GLYPH = 16 };
typedef struct {
    uint8_t entry, has;
    uint16_t ax, al, si, bl;
    uint16_t x, y, font, glyph; uint8_t ch, carry;   /* entry 70h: a letter */
} GfxCall;
#define LIST(T) struct { T *v; int n, cap; }
typedef struct Cycle Cycle;
typedef struct {
    void *ctx;
    void (*audio)(void *ctx, int kind, unsigned value);              /* every request as it comes */
    int (*settimer_clock)(void *ctx, unsigned at, unsigned ax, uint32_t *now);   /* 1: a counter */
    int (*task_wait)(void *ctx, unsigned sp);                       /* 1: done by the host */
    void (*graphics)(void *ctx, const GfxCall *call);               /* every call as it comes */
    Joystick *joystick;                                             /* 0: a port that reads 0 (the benches') */
    void (*port_out)(void *ctx, unsigned port, unsigned value);
} CycleHost;
struct Cycle {
    Truck *k;                   /* the tables; k->data is the model's `data` */
    uint8_t *mem;               /* the data segment */
    int level, track, layout;
    uint16_t segment;           /* stored with each script pointer (+3e) */
    long long script_limit;
    int live_input, acceleration_fix;
    uint8_t controls[4];
    uint16_t truck_addresses[4], object_addresses[4];
    int has_step_override, step_override_stop;
    uint16_t step_override_step;
    LIST(uint16_t) sounds, script_audio, music;
    LIST(SpawnEvent) events;
    LIST(uint32_t) requests;    /* kind << 16 | value */
    LIST(GfxCall) graphics;
    LIST(uint32_t) timers;      /* object << 16 | AX */
    CycleHost host;
    void *printer;              /* the text printer (LEVEL_GAME) */
    int live;                   /* the game played: the requests and the drawing go out, not into lists */
};
void cycle_init(Cycle *c, Truck *k, uint8_t *mem, int level, int track, int layout);
void cycle_free_lists(Cycle *c);
void cycle_clear_lists(Cycle *c);
uint16_t cycle_allocate(Cycle *c, unsigned template_, unsigned parent);   /* 0: the pool is empty */
void cycle_append(Cycle *c, unsigned head, unsigned at, int front);
void cycle_unlink(Cycle *c, unsigned head, unsigned at);
void cycle_release(Cycle *c, unsigned at);
int cycle_active(Cycle *c, uint16_t *out, int cap);
void cycle_spatial(Cycle *c, unsigned at, int insert);
void cycle_register(Cycle *c, unsigned at, unsigned flags, unsigned rect);
void cycle_contacts(Cycle *c);
void cycle_spawn(Cycle *c, const Spawn *e);
void cycle_update_truck(Cycle *c, unsigned at);
unsigned cycle_command(Cycle *c, unsigned ta, int index);
void cycle_checkpoint_update(Cycle *c, uint8_t *t, const TruckStep *g);
void cycle_script(Cycle *c, unsigned at, unsigned pc);
unsigned cycle_extra_opcode(Cycle *c, unsigned at, unsigned pc, unsigned op, unsigned ax);
void cycle_sweep_retired(Cycle *c);
void cycle_scheduler(Cycle *c);
int cycle_frame(Cycle *c, const uint8_t controls[4]);   /* LEVEL_LAPS and up: 1 = race_exit */
void cycle_finish(Cycle *c);
void cycle_sound(Cycle *c, unsigned value);              /* a sound effect (3af8) */
void cycle_request(Cycle *c, int kind, unsigned value);  /* script audio, music */
void cycle_call_graphics(Cycle *c, const GfxCall *call);
/* the session */
void session_drivers(uint8_t *m);
void session_new_game(uint8_t *m);
void session_truck_start(Cycle *c, unsigned ta, unsigned at);
void session_race_start(Cycle *c);
/* the bonus on the track (object 340e) */
void bonus_visibility(uint8_t *m, unsigned o, unsigned ax);
void bonus_place(uint8_t *m, unsigned o);
void bonus_screen(uint8_t *m, unsigned x, unsigned y, unsigned height, uint16_t *sx, uint16_t *sy);
void bonus_project(uint8_t *m, unsigned o);
void bonus_pick_up(Cycle *c, unsigned o);
/* the race loop: draw may be 0 (the model without drawing) */
void loop_race_clock(uint8_t *m);
void loop_clock_digits(Cycle *c, int draw);
void loop_nitro_digits(Cycle *c, int draw);
void loop_lap_digits(Cycle *c, int draw);
void loop_hud(Cycle *c, int draw);
int loop_screen_offset(unsigned x, unsigned y, uint16_t *out);   /* 0: outside the table */
void loop_hud_init(Cycle *c, int draw);
void loop_start(Cycle *c);
void loop_tail(Cycle *c);
/* ---- the text printer (src/printer.c; tools/text_printer.py) --------------
 * The segments outside the data segment it reads: the two fonts and the
 * graphics module with its colour schemes. */
typedef struct {
    uint8_t *road, *iron, *ega; int nroad, niron, nega;   /* ega: the module, past its header */
    uint16_t order[64]; int norder;                        /* colour schemes in registration order */
} Far;
Far *far_load(const char *dir);
void far_free(Far *f);
typedef struct {
    Far *far;
    Cycle *c;                   /* the glyphs go to its graphics list, or to `events` */
    LIST(GfxCall) events;
    int error;                  /* OFF_TEXT, OFF_BAD_ADDRESS */
    int far_return;             /* command 0d: the CPU goes on at far_cs:10a5 */
    uint16_t far_cs;
} Printer;
unsigned text_print(Printer *p, uint8_t *m, unsigned si);   /* the address after the text */
unsigned text_width(Printer *p, uint8_t *m, unsigned si);
unsigned text_glyph_width(Printer *p, uint8_t *m, unsigned ch);
void text_glyph(Printer *p, uint8_t *m, unsigned ch);
void text_row_map(Printer *p, uint8_t *m);
void text_slant(Printer *p, uint8_t *m);
void text_colours(Printer *p, uint8_t *m, unsigned entry);
unsigned far_module_byte(Printer *p, unsigned offset);

/* ---- the game outside the scripts (src/flow.c; tools/game_flow.py) -------
 * The machine-dependent inputs come from outside (GameIO): the timer counter
 * at the start of an update, the key bytes that reach INT 9 after a stop,
 * whether ironhi.bin exists; observe is called at the stops of the captures
 * and may end the run there. Without a program, what needs one (Esc, Alt+B,
 * the joystick service, F1, Ctrl+S, writing the records) is OFF_NOT_MODELLED. */
enum {
    STOP_SCREEN_BEFORE, STOP_SCREEN_AFTER, STOP_SCREEN_AFTER_FLIP, STOP_PRE_1339, STOP_POST_1339,
    STOP_BEFORE_SCHEDULER, STOP_AFTER_CONTACTS, STOP_AFTER_FLIP, STOP_PRE_FINISH, STOP_POST_FINISH,
    STOP_PRE_SHOP, STOP_PRE_NEXT, STOP_POST_NEXT, STOP_GAME_OVER, STOP_PRE_TABLE, STOP_POST_TABLE,
    STOP_DEMO, STOP_PRE_NEW_GAME, STOP_PRE_SIGNIN, STOP_PRE_RACE, STOP_TITLE, STOP_DEMO_RACE,
    STOP_DEMO_FINISH, STOP_DEMO_RESULTS, STOP_DEMO_TABLE, STOP_DEMO_SHOP, STOP_DEMO_NEXT, STOP_KINDS
};
extern const char *const STOP_NAMES[STOP_KINDS];
typedef struct {
    void *ctx;
    int (*counter)(void *ctx, int kind, uint32_t *value);       /* 1: the counter at that stop */
    int (*keys)(void *ctx, int kind, uint8_t *out, int cap);    /* the key bytes after that stop */
    int (*records_file)(void *ctx);                             /* ironhi.bin exists */
    int (*observe)(void *ctx, int kind);                        /* not 0: the run ends here */
    int (*settimer_clock)(void *ctx, unsigned at, unsigned ax, uint32_t *now);
} GameIO;
/* how a run ends early: the model's exceptions (a stop asked by observe, a
 * part not modelled, the program's exit, a loop that waits for a key the
 * script does not give) */
typedef struct { void *jump; int outcome, exit_code, point; } Run;
void run_raise(Run *r, int outcome);
/* what a program gives the game (program.c); all 0 without one */
typedef struct {
    void *ctx;
    void (*quit_question)(void *ctx);
    void (*alt_b)(void *ctx, unsigned sp);
    void (*joystick_read)(void *ctx);
    int (*pause)(void *ctx);                    /* 1: F10, the race ends */
    int (*save_question)(void *ctx);            /* 1: the table again */
    void (*records_save)(void *ctx, int al);
    long long (*load_buffer)(void *ctx, const char *name, long long expected, unsigned sp);
    int ticks;                                  /* the ideal machine's time: the waits tick */
    void (*task_wait)(void *ctx, unsigned sp);
    void (*sound)(void *ctx, int kind, unsigned value);   /* every sound request (the sound driver) */
    void (*port_out)(void *ctx, unsigned port, unsigned value);
    Joystick *joystick;
} ProgramHooks;
enum { GAME_RUNNING, GAME_STOPPED = 100, GAME_EXIT = 101, GAME_WAITING = 102 };
typedef struct {
    Cycle *c;
    GameIO io;
    ProgramHooks program;       /* program.ctx == 0: none */
    struct Gfx *video;          /* 0: no drawing */
    uint8_t *memory;            /* the whole machine (GFX_MEMORY bytes) when there is one: files land there whole */
    char dir[1024];             /* the original files */
    Run own, *run;              /* the run (a program's, when there is one) */
} Game;
void game_init(Game *g, Cycle *c, const char *dir);
void game_raise(Game *g, int outcome);   /* ends the run (OFF_*, GAME_*) */
/* one entry of the model run to its end or to a stop: returns the outcome
 * (GAME_RUNNING when it returned), its value in *value */
enum {
    G_SESSION, G_GAME, G_DEMO, G_RACE, G_TITLE, G_TABLE, G_SHOP, G_RESULTS, G_SIGN_IN, G_SCREEN_FRAME,
    G_RACE_PREPARATION, G_TERRAIN, G_POOL_RESET, G_NEXT_RACE, G_NEW_GAME, G_KEY_CHECKS, G_WAIT,
    G_BUTTON_PRESSED, G_RUN_SCREEN, G_SCREEN, G_LOAD_FILE, G_AUXILIARY_PLANE, G_JOYSTICK_SERVICE,
    G_ESCAPE, G_ALT_B, G_SET_COUNTER, G_APPLY_KEYS, G_MUSIC, G_MUSIC_OFF, G_NEW_OBJECT
};
int game_run(Game *g, int entry, long long a, long long b, const char *name, long long *value);
unsigned cycle_alloc_root(Cycle *c, unsigned pointer);

/* ---- the program and DOS (src/program.c; tools/program.py) ----------------
 * The model on a 640 KB image of conventional memory: the start-up from
 * DOS's EXEC of game.exe to the title demo (0000:00d9), the way back to DOS,
 * the shell (Alt+B), the questions of the start, the disk request, the pause
 * (F1), the joystick, the records file, the timer interrupt and the tasks.
 * The machine (Machine) gives what DOS and the hardware would. */
#define CODE_SEGMENT 0x0824
#define DATA_SEGMENT 0x0d98
#define LIB_SEGMENT (CODE_SEGMENT + 0x434)
#define DRV_SEGMENT (CODE_SEGMENT + 0x56a)
#define PSP_SEGMENT 0x0814
#define ENV_SEGMENT 0x07ca
/* the loops that wait for a key */
enum { POINT_JOYSTICK, POINT_SOUND, POINT_DISK, POINT_QUIT, POINT_PAUSE, POINT_SAVE, POINT_KEY_WAIT, POINT_TICKS, POINTS };
extern const char *const POINT_NAMES[POINTS];
typedef struct { uint8_t b[16]; int n; } KeyGroup;
typedef struct { char name[16]; uint8_t *data; long long n; int owned; } MachineFile;   /* owned: data is the program's (malloc) */
enum { CALL_FIND = 1, CALL_OPEN = 2, CALL_CREATE = 4, CALL_EXEC = 8 };
typedef struct {
    uint8_t environment[0x800]; int nenvironment;
    uint8_t tail[0x100]; int ntail;
    uint8_t tail_end[8]; int ntail_end;          /* what follows the tail in the PSP (DOS: CR) */
    uint32_t ticks;                              /* the BIOS tick count: the random seed */
    uint8_t ivt[0x400]; int nivt;                /* the interrupt vectors before the game */
    uint16_t measure_at[16]; uint8_t measure_value[16]; int nmeasure;   /* DS:0010, 06b8..06bc */
    KeyGroup *keys[POINTS]; int nkeys[POINTS], next_key[POINTS];
    char disks[4][1024]; int ndisks;             /* a folder per disk */
    unsigned top, dos_version;
    int command_com_returns;                     /* EXEC of COMMAND.COM: 1 it runs and returns, 0 missing */
    Joystick joystick;
    unsigned shift_flags;
    MachineFile written[16]; int nwritten;       /* files the program creates (never written to the disk) */
    int time_ticks;                              /* 1: a tick when the CPU idles; 0: INT 60h jumps */
    char critical[8][16]; int ncritical;         /* reading them ends in a critical error */
    char critical_name[8][16]; unsigned critical_calls[8]; int ncritical_calls;
    unsigned bios_keys;                          /* keys waiting in the BIOS buffer when the shell asks */
} Machine;
void machine_init(Machine *mc, const uint8_t *environment, int n, const char *disk);   /* the model's defaults */
/* the machine when it is a real one (the game played, sdl/main.c): all 0 in
 * the tests, where the model's script and its ideal time are used */
typedef struct {
    void *ctx;
    int (*wait_keys)(void *ctx, int point, uint8_t *bytes, int cap);   /* a loop waits for a key: the bytes of INT 9 */
    void (*before_tick)(void *ctx);      /* the time of the next timer tick */
    void (*after_tick)(void *ctx);       /* after it: the screen, the sound */
    void (*port)(void *ctx, unsigned port, unsigned value);   /* every port write */
    void (*saved)(void *ctx, const char *name, const uint8_t *data, long long n);   /* a file the program wrote */
    int live;                            /* 1: the writes are not kept in lists */
} ProgramHost;
typedef struct Program {
    Machine *mc;
    uint8_t *M;                 /* GFX_MEMORY bytes; the data segment at DATA_SEGMENT */
    int disk;
    uint16_t dta_seg, dta_off;
    LIST(uint8_t) output;       /* what the program wrote to the standard output */
    LIST(uint32_t) ports;       /* port << 8 | value */
    LIST(uint16_t) dos;         /* the DOS calls that leave no other trace */
    struct Gfx *video;          /* from the graphics driver's start on */
    Printer printer;
    Far *far;
    uint16_t ss, sp, di, callback_sp, reg_dx, reg_bp;
    long long ticks, tick_limit;     /* tick_limit < 0: none */
    Run own, *run;
    char dir[1024];             /* the original files (fonts, ega.exe for the printer) */
    ProgramHost host;
} Program;
Program *program_new(Machine *mc, const char *dir);
void program_free(Program *p);
int program_start(Program *p);                  /* GAME_RUNNING: the title (0000:00d9) */
void program_attach(Program *p, Game *g);       /* the game goes on on the program's memory */
int program_run(Program *p, int entry, long long a, long long b, long long *value);   /* one routine of the model, for the tests */
uint8_t *program_snapshot(Program *p);          /* conventional memory (the data segment is in it) */
void program_make_video(Program *p);            /* the graphics module's state, for the tests */
void program_joystick_calibrate(Program *p);    /* 07e2 for 0680 and 0692: the stick's centre now */

/* ---- the game's routines it never calls (src/unused.c; tools/unused.py) --- */
typedef struct { uint16_t ax, bx, cx, dx, bp; } Regs5;
void unused_table_pair(const uint8_t *m, unsigned al, Regs5 *r);
void unused_binary_to_bcd(unsigned ax, Regs5 *r);
void unused_bcd_to_binary(unsigned ax, Regs5 *r);
void unused_bcd_to_ascii(unsigned ax, Regs5 *r);
void unused_ascii_to_bcd(unsigned ax, unsigned cx, Regs5 *r);
void unused_hex_to_ascii(unsigned ax, Regs5 *r);
void unused_ascii_to_hex(unsigned ax, unsigned cx, Regs5 *r);
void unused_binary_to_ascii(unsigned ax, Regs5 *r);
void unused_ascii_to_binary(unsigned ax, unsigned cx, Regs5 *r);

/* ---- the files' formats (src/files.c; tools/extract_sprites.py,
 *      extract_backgrounds.py) ---------------------------------------------- */
/* the load-time sprite compiler (0000:1038 through the module's converter):
 * flags 87h/01h (planes, 87h with a mask row first) or 0fh (colour masks);
 * returns the bytes written to out, -1 for other flags (the model's assert) */
long long sprite_converted(const uint8_t *raw, long long nraw, unsigned row_bytes, unsigned height, unsigned planes,
                      unsigned flags, uint8_t *out, long long cap);
/* a plane of a picture file: 40 columns of 200 rows, run-length coded, each
 * byte inverted; returns the position after it, -1 when the file ends early
 * or a run passes the column */
long long file_decode_plane(const uint8_t *data, long long n, long long position, uint8_t out[8000]);

/* the screens' instructions (src/screens.c): 1 = handled */
int screens_opcode(Cycle *c, unsigned at, unsigned *pc, unsigned op, unsigned ax);
unsigned screens_das_word(unsigned word, unsigned value);       /* SUB/SBB with DAS on a BCD word */
unsigned screens_daa_word_add(unsigned word, unsigned value);   /* ADD/ADC with DAA */

/* ---- graphics: the module ega.exe (src/graphics.c; tools/graphics.py) ---- */
#define GFX_MEMORY 0xa0000u /* conventional memory, as DOSBox-X lays it out */
typedef struct {
    uint16_t ax, bx, cx, dx, si, di, bp, ds, es, ss, cf;
} GfxRegs;
typedef struct Gfx {
    uint8_t *mem;               /* GFX_MEMORY bytes: the module, the game's data, the sprite banks */
    uint8_t planes[4][0x10000]; /* the four planes of mode 0Dh */
    uint8_t dac[768];
    uint8_t attr[0x15];         /* the attribute registers */
    uint16_t start;             /* the CRTC start address */
    uint8_t mode, page_number;  /* as INT 10h AH=0Fh reports them */
    uint16_t ds, base;          /* the game's data segment, the module's load segment */
    uint32_t cs;                /* linear address of the module's segment 9 */
    int error;
} Gfx;
void gfx_init(Gfx *g, uint8_t *mem, uint16_t data_segment);
int gfx_call(Gfx *g, unsigned entry, GfxRegs *regs);   /* entry = table offset 00..80 */
void gfx_flip(Gfx *g);                                  /* 09ee, at the timer tick */
void gfx_screen(const Gfx *g, uint8_t rgb[320 * 200 * 3]);   /* the picture on screen (src/screen.c) */

/* One call for the tests: entry, or -1 for the flip. The state goes in and
 * comes out in plain buffers; regs holds AX..CF in the order of GfxRegs. */
int offroad_graphics(uint8_t *mem, uint8_t *planes, uint8_t *dac, uint8_t *attr, uint16_t *start,
                     uint8_t *mode_page, int entry, uint16_t *regs);

/* ---- sound (src/sound.c; tools/sound.py) ---------------------------------- */
enum { SPK_PIT = 0, SPK_ON = 1, SPK_OFF = 2 };
typedef struct {
    uint8_t *m;                 /* the data segment */
    uint8_t *opl;               /* AdLib writes: register, value, ... */
    int nopl, opl_cap;          /* in writes */
    uint16_t *spk;              /* PC speaker events: kind, divisor, ... */
    int nspk, spk_cap;          /* in events */
    int error;
} Sound;

/* The requests (the game's entries) and the timer tick. */
enum { SND_TICK, SND_EFFECT, SND_MUSIC, SND_MUSIC_OFF, SND_SCRIPT_AUDIO, SND_SCRIPT_AUDIO_B };
int snd_request(Sound *s, int kind, unsigned arg);
void snd_opl(Sound *s, unsigned reg, unsigned value);
void snd_adlib_reset(Sound *s);

/* One call for a host that holds plain buffers (the tests load it with
 * ctypes): returns the result, the counts of writes and events. */
int offroad_sound(uint8_t *ds, int kind, unsigned arg, uint8_t *opl, int opl_cap, int *nopl,
                  uint16_t *spk, int spk_cap, int *nspk);

#endif
