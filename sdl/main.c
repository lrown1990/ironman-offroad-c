/* Super Off Road, played: the rebuilt program (src/) on a real machine made
 * of SDL2. The program runs as the model does, with the machine's parts
 * given by this file:
 *   time      a timer tick every 1/70.087 s (the VGA frame of mode 0Dh: the
 *             game's timer waits for the retrace), the BIOS tick count from
 *             the clock of the day (the random seed);
 *   keyboard  the keys as set-1 scan codes through the game's INT 9, at the
 *             ticks, and in the loops that wait for a key; the questions of
 *             the start are answered by the machine (Y joystick, 3 AdLib);
 *   joystick  the first SDL controller as the game port (two axes, two
 *             buttons; on a gamepad R2 also pushes the stick forward);
 *   screen    the page the graphics module shows, every tick, in a window;
 *   sound     the AdLib through Nuked OPL3 (third_party, LGPL 2.1), the PC
 *             speaker as a square wave, one tick of samples at a time;
 *   files     the original files read from their folder (never written); the
 *             records file kept in the user's folder.
 * Usage: offroad [--fix-acceleration] [folder of the original files]
 *   (default folder: ../OFFROAD next to OFFROAD C, then ./OFFROAD)
 *   --fix-acceleration: the one-byte fix at 0000:14c4 (mov [di+71],ax): the
 *   player's acceleration comes from the table of the level bought, as the
 *   computer's does; without it, as the original, it is the start Y >> 5
 *   and the shop's acceleration does nothing (reports/phase-10 in OPUS).
 * For a test without sound or window (SDL_AUDIODRIVER=dummy, SDL_VIDEODRIVER
 * =dummy): OFFROAD_TEST_KEYS the scan codes (hex) the waiting loops get in
 * order, OFFROAD_TEST_TICKKEYS "tick:code ..." keys at given ticks,
 * OFFROAD_TEST_TICKS the tick where the program stops after writing the
 * screen to OFFROAD_TEST_SHOT (a PPM); OFFROAD_TEST_WAV the sound (raw
 * 16-bit stereo at 49716 Hz); OFFROAD_TEST_SECONDS the time of day in seconds
 * (the BIOS count, so the random seed) instead of the clock's.
 * In the browser (Emscripten, web/build.sh) the same file, with ASYNCIFY: the
 * waits of the machine (SDL_Delay) give the page back its turn, so the
 * program keeps its loops. There the original files are /OFFROAD (packed),
 * the records /save (IndexedDB), the acceleration fix is on, and the site
 * drives the window: offroad_riduci (minimised: paused), offroad_chiudi
 * (closed: the machine off, the next opening starts from the beginning),
 * offroad_apri, offroad_muto.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <SDL.h>
#include "offroad.h"
#include "opl3.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define TICK_RATE 70.087
#define AUDIO_RATE 49716
#define PIT_HZ 1193182.0

typedef struct {
    Program *p;
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    SDL_AudioDeviceID audio;
    SDL_Joystick *joystick;
    SDL_GameController *pad;    /* the same device, through SDL's gamepad mapping when it has one */
    opl3_chip opl;
    unsigned opl_reg;
    unsigned speaker_divisor, speaker_gate, speaker_phase;
    int pit_low;            /* the next write to 42h is the low byte */
    uint8_t keys[256]; int nkeys;   /* scan codes waiting for INT 9 */
    double start;           /* the clock of tick 0 (seconds) */
    long long ticks, samples;
    char save[1024];        /* the user's folder */
    int quit;
    int paused;             /* the site's edition: the game waits in its pause (F1) */
    int texts_patched;      /* the site's edition: the quit question without "TO DOS" */
    int r1;                 /* the site's edition: R1 held (the yellow truck's credit) */
    const char *test_keys, *test_tick_keys, *test_shot;   /* the test mode */
    FILE *test_wav;                                       /* OFFROAD_TEST_WAV: the samples, raw */
    long long test_ticks;
} Host;

static double now(void) { return SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency(); }

#ifdef __EMSCRIPTEN__
/* the site's window: minimised = paused, closed = the machine off */
static volatile int web_paused, web_closed, web_muted;
EMSCRIPTEN_KEEPALIVE void offroad_riduci(void) { web_paused = 1; }
EMSCRIPTEN_KEEPALIVE void offroad_chiudi(void) { web_closed = 1; }
EMSCRIPTEN_KEEPALIVE void offroad_apri(void) { web_paused = 0; web_closed = 0; }
EMSCRIPTEN_KEEPALIVE void offroad_muto(int on) { web_muted = on; }
static long long web_ticks;   /* the ticks of this run, for the page's tests */
/* The race asks the program about the pause (F1) at every tick of the race
 * loop: the wrapper notes the tick, and calls the program's own check. */
static int (*race_pause_original)(void *ctx);
static long long race_tick = -100;
static int race_pause(void *ctx) { race_tick = web_ticks; return race_pause_original(ctx); }
static Host *web_host;
/* F1 pressed by the focus lost: its release waits (1 = not yet delivered, 2 =
 * delivered with the keys of a tick). All the queued keys reach the game
 * before a tick, so a press and a release queued together would be a key
 * already up when the race looks at it. The release goes to the pause's
 * first wait for a key, or with the next tick if no pause came. */
static int web_f1_release;
/* Esc in the pause (the user's request): the game's own pause knows only F1
 * and F10, so the shell asks the game's quit question (the same Esc gives
 * everywhere else): Y quits, N goes back to the pause. */
static Game *web_game;
static int web_esc_in_pause;
/* The page lost the window's focus (another window, the menu): in a race of
 * a game (not the demo) the game's own pause, as if F1 were pressed and let
 * go, and it stays there until F1; anywhere else the machine just stops,
 * and goes on by itself when the focus comes back (offroad_apri). */
EMSCRIPTEN_KEEPALIVE void offroad_fuoco_perso(void) {
    Host *h = web_host;
    int racing = h && h->p && web_ticks - race_tick <= 2 && !h->p->M[DATA_SEGMENT * 16 + 0x1ab];
    if (racing && !h->paused && !web_f1_release && h->nkeys < (int)sizeof h->keys) {
        h->keys[h->nkeys++] = 0x3b;   /* F1 down: the race's check finds it at the next tick */
        web_f1_release = 1;
    } else if (!racing) web_paused = 1;
}
EMSCRIPTEN_KEEPALIVE int offroad_tick(void) { return (int)web_ticks; }
#endif

/* ---- the keyboard: SDL scan codes to set 1 -------------------------------------- */
static unsigned set1(SDL_Scancode s) {
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) {
        static const uint8_t letters[26] = {0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                            0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c};
        return letters[s - SDL_SCANCODE_A];
    }
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) return 0x02 + (s - SDL_SCANCODE_1);
    if (s == SDL_SCANCODE_0) return 0x0b;
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F10) return 0x3b + (s - SDL_SCANCODE_F1);
    switch (s) {
    case SDL_SCANCODE_ESCAPE: return 0x01;
    case SDL_SCANCODE_MINUS: return 0x0c;
    case SDL_SCANCODE_EQUALS: return 0x0d;
    case SDL_SCANCODE_BACKSPACE: return 0x0e;
    case SDL_SCANCODE_TAB: return 0x0f;
    case SDL_SCANCODE_LEFTBRACKET: return 0x1a;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1b;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 0x1c;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x1d;
    case SDL_SCANCODE_SEMICOLON: return 0x27;
    case SDL_SCANCODE_APOSTROPHE: return 0x28;
    case SDL_SCANCODE_GRAVE: return 0x29;
    case SDL_SCANCODE_LSHIFT: return 0x2a;
    case SDL_SCANCODE_BACKSLASH: return 0x2b;
    case SDL_SCANCODE_COMMA: return 0x33;
    case SDL_SCANCODE_PERIOD: return 0x34;
    case SDL_SCANCODE_SLASH: return 0x35;
    case SDL_SCANCODE_RSHIFT: return 0x36;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x37;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 0x38;
    case SDL_SCANCODE_SPACE: return 0x39;
    case SDL_SCANCODE_CAPSLOCK: return 0x3a;
    case SDL_SCANCODE_NUMLOCKCLEAR: return 0x45;
    case SDL_SCANCODE_SCROLLLOCK: return 0x46;
    /* the grey keys send E0 first, which INT 9 ignores: the keypad's codes */
    case SDL_SCANCODE_HOME: case SDL_SCANCODE_KP_7: return 0x47;
    case SDL_SCANCODE_UP: case SDL_SCANCODE_KP_8: return 0x48;
    case SDL_SCANCODE_PAGEUP: case SDL_SCANCODE_KP_9: return 0x49;
    case SDL_SCANCODE_KP_MINUS: return 0x4a;
    case SDL_SCANCODE_LEFT: case SDL_SCANCODE_KP_4: return 0x4b;
    case SDL_SCANCODE_KP_5: return 0x4c;
    case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_KP_6: return 0x4d;
    case SDL_SCANCODE_KP_PLUS: return 0x4e;
    case SDL_SCANCODE_END: case SDL_SCANCODE_KP_1: return 0x4f;
    case SDL_SCANCODE_DOWN: case SDL_SCANCODE_KP_2: return 0x50;
    case SDL_SCANCODE_PAGEDOWN: case SDL_SCANCODE_KP_3: return 0x51;
    case SDL_SCANCODE_INSERT: case SDL_SCANCODE_KP_0: return 0x52;
    case SDL_SCANCODE_DELETE: case SDL_SCANCODE_KP_PERIOD: return 0x53;
    case SDL_SCANCODE_F11: return 0x57;
    case SDL_SCANCODE_F12: return 0x58;
    default: return 0;
    }
}

/* The keys of this edition: the game keeps its own maps (DS:5382 player 1,
 * DS:5396 player 2: Q gas, A nitro, Z/X left, C/V right; ] gas, ' nitro, M/,
 * left, ./ right); here the keyboard sends the codes of those keys from other
 * places, so the game itself is unchanged. 0: the key sends its own code. */
#define IGNORED 0x100
static unsigned remap(SDL_Scancode s) {
    switch (s) {
    /* red truck, player 1 */
    case SDL_SCANCODE_W: return 0x10;        /* Q: gas */
    case SDL_SCANCODE_A: return 0x2c;        /* Z: left */
    case SDL_SCANCODE_D: return 0x2f;        /* V: right */
    case SDL_SCANCODE_C: return 0x1e;        /* A: nitro */
    /* blue truck, player 2 */
    case SDL_SCANCODE_I: return 0x1b;        /* ]: gas */
    case SDL_SCANCODE_J: return 0x32;        /* M: left */
    case SDL_SCANCODE_L: return 0x35;        /* /: right */
    case SDL_SCANCODE_RSHIFT: return 0x28;   /* ': nitro */
    /* the original places, now off (IGNORED: the key sends nothing) */
    case SDL_SCANCODE_Q: case SDL_SCANCODE_Z: case SDL_SCANCODE_X: case SDL_SCANCODE_V:
    case SDL_SCANCODE_RIGHTBRACKET: case SDL_SCANCODE_APOSTROPHE: case SDL_SCANCODE_M:
    case SDL_SCANCODE_COMMA: case SDL_SCANCODE_PERIOD: case SDL_SCANCODE_SLASH:
        return IGNORED;
#ifdef __EMSCRIPTEN__
    case SDL_SCANCODE_F10:   /* the site's edition: off (in the browser it did not reach the game) */
        return IGNORED;
    /* The site's edition: no function keys, the browser has them (help,
     * reload, ...). P is the pause (F1 in a race); the credits are 3 red, 8
     * blue (their own codes, written in the game's table by patch_keys) and
     * R1 yellow (F5, from the controller); F2 (music on/off) off. */
    case SDL_SCANCODE_P: return 0x3b;        /* F1: the pause */
    case SDL_SCANCODE_F1: case SDL_SCANCODE_F2: case SDL_SCANCODE_F5: case SDL_SCANCODE_F9:
        return IGNORED;
#endif
    default: return 0;
    }
}

/* a controller that comes: the game's own calibration (07e2) with the
 * stick taken at its centre; one that goes: the port reads 1 again */
static void open_controller(Host *h, int index) {
    if (h->joystick) return;
    if (SDL_IsGameController(index)) { h->pad = SDL_GameControllerOpen(index); h->joystick = SDL_GameControllerGetJoystick(h->pad); }
    else h->joystick = SDL_JoystickOpen(index);
    if (!h->joystick) return;
    static const int centre[4] = {100, 100, -1, -1};
    joy_init(&h->p->mc->joystick, centre, 0xf0);
    if (h->p->video) program_joystick_calibrate(h->p);   /* (before the start: the joystick question does it) */
}
static void close_controller(Host *h) {
    if (h->pad) SDL_GameControllerClose(h->pad); else SDL_JoystickClose(h->joystick);
    h->pad = 0; h->joystick = 0;
    static const int none[4] = {-1, -1, -1, -1};
    joy_init(&h->p->mc->joystick, none, 0xf0);
    /* the port no longer answers, so the game keeps its last readings: the
     * stick back at its centre, the buttons let go */
    uint8_t *m = h->p->M + DATA_SEGMENT * 16;
    static const unsigned ports[2] = {0x680, 0x692};
    for (int i = 0; i < 2; i++) {
        unsigned at = ports[i];
        m[at] = m[at + 4]; m[at + 1] = m[at + 5]; m[at + 2] = m[at + 6]; m[at + 3] = m[at + 7];
    }
    m[0x690] = m[0x691] = m[0x6a2] = m[0x6a3] = 0;
#ifdef __EMSCRIPTEN__
    if (h->r1 && h->nkeys < (int)sizeof h->keys) h->keys[h->nkeys++] = 0xbf;   /* R1 (F5) let go */
    h->r1 = 0;
#endif
}

/* the window's events: the keys into the queue, the joystick into the port */
static void events(Host *h) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) h->quit = 1;
        else if (e.type == SDL_CONTROLLERDEVICEADDED || (e.type == SDL_JOYDEVICEADDED && !SDL_IsGameController(e.jdevice.which))) {
            if (!h->joystick) open_controller(h, e.cdevice.which);
        } else if (e.type == SDL_CONTROLLERDEVICEREMOVED || e.type == SDL_JOYDEVICEREMOVED) {
            if (h->joystick && e.jdevice.which == SDL_JoystickInstanceID(h->joystick)) close_controller(h);
        } else if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && !e.key.repeat) {
            unsigned code = remap(e.key.keysym.scancode);
            if (code == IGNORED) continue;
            if (!code) code = set1(e.key.keysym.scancode);
            if (code && h->nkeys < (int)sizeof h->keys) h->keys[h->nkeys++] = (uint8_t)(code | (e.type == SDL_KEYUP ? 0x80 : 0));
        }
    }
    if (h->joystick) {
        /* the game port: an axis bit stays at 1 for a count of reads that
         * grows with the stick's position (calibrated by the game at the
         * joystick question); buttons 0 when pressed. A gamepad: the left
         * stick or the D-pad is the stick, R2 pushes it forward (the
         * accelerator of the joystick), the bottom face button (Cross, A)
         * is button 1 (nitro), the right one (Circle, B) button 2. */
        Joystick *j = &h->p->mc->joystick;
        int x, y, b1, b2, r1;
        if (h->pad) {
            SDL_GameController *c = h->pad;
            x = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
            y = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
            if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) x = -32768;
            if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) x = 32767;
            if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP)) y = -32768;
            if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) y = 32767;
            if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000) y = -32768;
            b1 = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A);
            b2 = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B);
            r1 = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        } else {
            x = SDL_JoystickGetAxis(h->joystick, 0); y = SDL_JoystickGetAxis(h->joystick, 1);
            b1 = SDL_JoystickGetButton(h->joystick, 0); b2 = SDL_JoystickGetButton(h->joystick, 1);
            r1 = SDL_JoystickGetButton(h->joystick, 5);   /* R1 in the browser's standard layout */
        }
#ifdef __EMSCRIPTEN__
        /* the site's edition: R1 is F5, the yellow truck's credit in the shop */
        if (r1 != h->r1 && h->nkeys < (int)sizeof h->keys) { h->keys[h->nkeys++] = r1 ? 0x3f : 0xbf; h->r1 = r1; }
#else
        (void)r1;
#endif
        j->axes[0] = 20 + (x + 32768) * 160 / 65535;
        j->axes[1] = 20 + (y + 32768) * 160 / 65535;
        unsigned buttons = 0xf0;
        if (b1) buttons &= ~0x10;
        if (b2) buttons &= ~0x20;
        j->buttons = buttons;
    }
#ifdef __EMSCRIPTEN__
    if (web_closed) h->quit = 1;   /* the site's window closed: the program ends, main starts it again */
#endif
    if (h->quit) run_raise(h->p->run, GAME_EXIT);   /* the window closed: the program ends there */
}

/* The browser's minimised window: the machine stops where it is. The events
 * still come in (a key let go meanwhile must reach the game; a close ends
 * the program from events), and after it the clock starts again from now,
 * with no ticks to catch up, and the sound with its margin of silence. */
static void queue_margin(Host *h) {   /* 80 ms of silence first: room for the ticks' jitter */
    static int16_t silence[2 * AUDIO_RATE * 80 / 1000];
    SDL_QueueAudio(h->audio, silence, sizeof silence);
}
static void hold(Host *h) {
#ifdef __EMSCRIPTEN__
    if (!web_paused) return;
    if (h->audio) SDL_PauseAudioDevice(h->audio, 1);
    while (web_paused) { events(h); SDL_Delay(50); }
    if (h->audio) { SDL_ClearQueuedAudio(h->audio); queue_margin(h); SDL_PauseAudioDevice(h->audio, 0); }
    h->start = now() - h->ticks / TICK_RATE;
#else
    (void)h;
#endif
}
static void deliver(Host *h) {
    for (int i = 0; i < h->nkeys; i++) ctl_int9(h->p->M + DATA_SEGMENT * 16, &h->keys[i], 1);
    h->nkeys = 0;
}

/* ---- the site's edition: the pause and the quit question ------------------------------ */
#ifdef __EMSCRIPTEN__
/* In the pause (F1) the picture goes dark and PAUSE is written in the
 * middle, in a 5x7 pixel font at four times its size, with a shadow. */
static void pause_overlay(uint8_t *rgb) {
    static const char *const glyphs[5][7] = {
        {"1111.", "1...1", "1...1", "1111.", "1....", "1....", "1...."},   /* P */
        {".111.", "1...1", "1...1", "11111", "1...1", "1...1", "1...1"},   /* A */
        {"1...1", "1...1", "1...1", "1...1", "1...1", "1...1", ".111."},   /* U */
        {".1111", "1....", "1....", ".111.", "....1", "....1", "1111."},   /* S */
        {"11111", "1....", "1....", "1111.", "1....", "1....", "11111"},   /* E */
    };
    for (int i = 0; i < 320 * 200 * 3; i++) rgb[i] = (uint8_t)(rgb[i] * 35 / 100);
    const int scale = 4, w = 5 * (5 * scale) + 4 * scale, x0 = (320 - w) / 2, y0 = (200 - 7 * scale) / 2;
    for (int pass = 0; pass < 2; pass++)   /* the shadow, then the letters */
        for (int g = 0; g < 5; g++)
            for (int r = 0; r < 7; r++)
                for (int c = 0; c < 5; c++) {
                    if (glyphs[g][r][c] != '1') continue;
                    int x = x0 + g * 6 * scale + c * scale + (pass ? 0 : 2), y = y0 + r * scale + (pass ? 0 : 2);
                    uint8_t v = pass ? 255 : 0;
                    for (int dy = 0; dy < scale; dy++)
                        for (int dx = 0; dx < scale; dx++) {
                            uint8_t *q = rgb + ((y + dy) * 320 + x + dx) * 3;
                            q[0] = q[1] = q[2] = v;
                        }
                }
}
/* The quit question says 'ARE YOU SURE YOU WANT / TO EXIT TO DOS (Y/N)': in
 * the browser there is no DOS, so the program's copy of the text (in its
 * data segment, once loaded; the original files stay as they are) loses
 * "TO DOS". */
static void patch_texts(Host *h) {
    if (h->texts_patched || !h->p) return;
    static const char from[] = "TO EXIT TO DOS (Y/N)", to[] = "TO EXIT? (Y/N)";
    uint8_t *ds = h->p->M + DATA_SEGMENT * 16;
    for (long long i = 0; i + (long long)sizeof from <= 65536; i++)
        if (!memcmp(ds + i, from, sizeof from)) {   /* with its terminating 0 */
            memset(ds + i, 0, sizeof from);
            memcpy(ds + i, to, sizeof to);
            h->texts_patched = 1;
            return;
        }
}
/* The keys that buy a credit (DS:9661, by driver: F1 red, F5 yellow, F9
 * blue) in the program's copy: 3 for the red and 8 for the blue, so F1 is
 * only the pause (P). Every time: cheap, and right once the data is loaded. */
static void patch_keys(Host *h) {
    if (!h->p) return;
    uint8_t *k = h->p->M + DATA_SEGMENT * 16 + 0x9661;
    for (int i = 0; i < 4; i++) {
        if (k[i] == 0x3b) k[i] = 0x04;        /* F1 -> 3 */
        else if (k[i] == 0x43) k[i] = 0x09;   /* F9 -> 8 */
    }
}
#endif

/* ---- the screen and the sound ------------------------------------------------------- */
static void present(Host *h) {
    static uint8_t rgb[320 * 200 * 3];
    static double last;
    if (!h->p->video) return;
    /* At most a picture per display frame (with the ticks at 70 Hz this
     * shows about one tick in two). In the browser every tick instead: the
     * page shows the last picture at each of the screen's frames, so a limit
     * there would only drop ticks; but not in a test, which runs as fast as
     * it can and would draw all its ticks. */
    int limit = 1;
#ifdef __EMSCRIPTEN__
    limit = h->test_ticks != 0;
#endif
    double t = now();
    if (limit && t - last < 1 / 60.0) return;
    last = t;
    gfx_screen(h->p->video, rgb);
#ifdef __EMSCRIPTEN__
    if (h->paused) pause_overlay(rgb);
#endif
    SDL_UpdateTexture(h->texture, 0, rgb, 320 * 3);
    SDL_RenderClear(h->renderer);
    SDL_RenderCopy(h->renderer, h->texture, 0, 0);
    SDL_RenderPresent(h->renderer);
}
static void port(void *ctx, unsigned port_, unsigned value) {
    Host *h = ctx;
    switch (port_) {
    case 0x388: h->opl_reg = value; break;
    case 0x389: OPL3_WriteReg(&h->opl, (uint16_t)h->opl_reg, (uint8_t)value);   /* as the model renders it */ break;
    case 0x43: if (value == 0xb6) h->pit_low = 1; break;
    case 0x42:
        if (h->pit_low) { h->speaker_divisor = (h->speaker_divisor & 0xff00) | value; h->pit_low = 0; }
        else { h->speaker_divisor = (h->speaker_divisor & 0xff) | value << 8; h->pit_low = 1; }
        break;
    case 0x61: h->speaker_gate = (value & 3) == 3; break;
    }
}
/* one tick of samples: the chip, and the speaker's square wave over it */
static void sound(Host *h) {
    long long target = (long long)((h->ticks + 1) * AUDIO_RATE / TICK_RATE);
    int n = (int)(target - h->samples);
    static int16_t buf[2 * 4096];
    if (n > 4096) n = 4096;
    for (int i = 0; i < n; i++) {
        OPL3_Generate(&h->opl, buf + 2 * i);
        if (h->speaker_gate && h->speaker_divisor) {
            unsigned period = (unsigned)(AUDIO_RATE * h->speaker_divisor / PIT_HZ);
            if (period < 2) period = 2;
            int v = (h->speaker_phase++ % period) < period / 2 ? 6000 : -6000;
            buf[2 * i] = (int16_t)SDL_clamp(buf[2 * i] + v, -32768, 32767);
            buf[2 * i + 1] = (int16_t)SDL_clamp(buf[2 * i + 1] + v, -32768, 32767);
        }
    }
    h->samples = target;
    if (h->test_wav) fwrite(buf, 4, n, h->test_wav);
#ifdef __EMSCRIPTEN__
    if (web_muted) memset(buf, 0, (size_t)n * 4);   /* the chip goes on: only the page is silent */
#endif
    if (h->audio && SDL_GetQueuedAudioSize(h->audio) < AUDIO_RATE * 4 / 2) SDL_QueueAudio(h->audio, buf, n * 4);
}

/* ---- the program's machine --------------------------------------------------------------- */
static void before_tick(void *ctx) {
    Host *h = ctx;
#ifdef __EMSCRIPTEN__
    h->paused = 0;   /* the game runs: out of its pause */
    patch_texts(h);
    patch_keys(h);
    if (web_f1_release == 2 && h->nkeys < (int)sizeof h->keys) { h->keys[h->nkeys++] = 0xbb; web_f1_release = 0; }   /* no pause came */
    else if (web_f1_release == 1) web_f1_release = 2;   /* F1 goes with this tick's keys */
#endif
    double due = h->start + (h->ticks + 1) / TICK_RATE;
    for (;;) {
        hold(h);
        due = h->start + (h->ticks + 1) / TICK_RATE;
        events(h);
        double t = now();
        if (h->test_ticks && !getenv("OFFROAD_TEST_REALTIME")) break;   /* a test runs as fast as it can */
        if (t >= due) {
            if (t > due + 0.25) h->start = t - (h->ticks + 1) / TICK_RATE;   /* far behind (a wait for a key): no catching up */
            break;
        }
        SDL_Delay((Uint32)((due - t) * 1000 > 1 ? (due - t) * 1000 - 1 : 0));
    }
    deliver(h);   /* IRQ 1 before IRQ 0 */
}
static void shot(Host *h) {
    static uint8_t rgb[320 * 200 * 3];
    FILE *f = fopen(h->test_shot, "wb");
    if (!f || !h->p->video) { if (f) fclose(f); return; }
    gfx_screen(h->p->video, rgb);
    fprintf(f, "P6\n320 200\n255\n");
    fwrite(rgb, 1, sizeof rgb, f);
    fclose(f);
}
static void after_tick(void *ctx) {
    Host *h = ctx;
    present(h);
    sound(h);
    h->ticks++;
#ifdef __EMSCRIPTEN__
    web_ticks = h->ticks;
#endif
    if (h->test_tick_keys) {   /* "tick:code" pairs */
        const char *q = h->test_tick_keys;
        while (*q) {
            char *end;
            long long t = strtol(q, &end, 10);
            if (*end != ':') break;
            unsigned code = (unsigned)strtoul(end + 1, &end, 16);
            if (t == h->ticks && h->nkeys < (int)sizeof h->keys) h->keys[h->nkeys++] = (uint8_t)code;
            q = end;
            while (*q == ' ') q++;
        }
    }
    if (h->test_ticks && h->ticks >= h->test_ticks) {
        if (h->test_shot) shot(h);
        h->quit = 1;
    }
}
/* a loop that waits for a key: the bytes of the next key that comes */
static int wait_keys(void *ctx, int point, uint8_t *bytes, int cap) {
    Host *h = ctx;
#ifdef __EMSCRIPTEN__
    if (point == POINT_PAUSE) h->paused = 1;   /* until the next tick */
    patch_texts(h);
    patch_keys(h);
    if (point == POINT_PAUSE && web_f1_release) {   /* the pause came: F1 let go */
        web_f1_release = 0;
        bytes[0] = 0xbb;
        return 1;
    }
    if (point == POINT_PAUSE && web_esc_in_pause && web_game && ctl_key_down(h->p->M + DATA_SEGMENT * 16, 0x01)) {
        web_esc_in_pause = 0;
        h->paused = 0;   /* the question without the dark veil */
        web_game->program.quit_question(web_game->program.ctx);   /* Y does not come back */
        h->paused = 1;   /* N: the pause again */
    }
    web_esc_in_pause = 0;
#endif
    /* the questions of the start, answered by the machine: Y to the joystick
     * (it stays on: a controller can come and go), 3 to the sound (AdLib);
     * each key let go at the next question or tick */
    if (point == POINT_JOYSTICK && !h->test_keys) { bytes[0] = 0x15; return 1; }
    if (point == POINT_SOUND && !h->test_keys) {
        bytes[0] = 0x95; bytes[1] = 0x04;
        h->keys[h->nkeys++] = 0x84;   /* 3 let go at the first tick */
        return 2;
    }
    if (h->test_keys) {   /* the next scan code of the test */
        while (*h->test_keys == ' ') h->test_keys++;
        if (!*h->test_keys) { h->quit = 1; events(h); }
        char *end;
        bytes[0] = (uint8_t)strtoul(h->test_keys, &end, 16);
        h->test_keys = end;
        return 1;
    }
    for (;;) {
        hold(h);
        events(h);
        if (h->nkeys) {
            int n = 1;   /* one interrupt per pass, as the script gives them */
            bytes[0] = h->keys[0];
#ifdef __EMSCRIPTEN__
            if (point == POINT_PAUSE && bytes[0] == 0x01) web_esc_in_pause = 1;   /* Esc down: the question at the next wait */
#endif
            memmove(h->keys, h->keys + 1, --h->nkeys);
            (void)cap;
            return n;
        }
        present(h);
        SDL_Delay(10);
    }
}
static void saved(void *ctx, const char *name, const uint8_t *data, long long n) {
    Host *h = ctx;
    char path[1200];
    snprintf(path, sizeof path, "%s%s", h->save, name);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(data, 1, n, f); fclose(f); }
#ifdef __EMSCRIPTEN__
    EM_ASM(FS.syncfs(false, function () {}););   /* to IndexedDB */
#endif
}

static int folder_has_game(const char *dir) {
    char path[1200];
    snprintf(path, sizeof path, "%s/game.exe", dir);
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != 0;
}

/* One run of the program, from the start to its end: the machine, the
 * records kept, the program and the game (on the desktop once; in the
 * browser again after every close of the window). */
static int play(Host *h, const char *dir, int fix_acceleration) {
    h->ticks = h->samples = 0; h->nkeys = 0; h->quit = 0; h->paused = 0; h->texts_patched = 0;
    h->speaker_divisor = h->speaker_gate = h->speaker_phase = 0; h->pit_low = 0; h->opl_reg = 0;
    OPL3_Reset(&h->opl, AUDIO_RATE);

    /* the machine: the environment and the measurements of the capture's DOSBox-X */
    static Machine mc;
    static const char env[] = "PATH=Z:\\\0COMSPEC=Z:\\COMMAND.COM\0PROMPT=$P$G\0BLASTER=A220 I7 D1 H5 P330 T6";
    machine_init(&mc, (const uint8_t *)env, sizeof env - 1, dir);
    static const uint16_t at[5] = {0x10, 0x6b8, 0x6ba, 0x6bb, 0x6bc};
    static const uint8_t value[5] = {0x0f, 0x0a, 0x15, 0x41, 0x46};
    for (int i = 0; i < 5; i++) { mc.measure_at[i] = at[i]; mc.measure_value[i] = value[i]; }
    mc.nmeasure = 5;
    time_t t = time(0);
    struct tm day;
    localtime_r(&t, &day);
    long long seconds = day.tm_hour * 3600 + day.tm_min * 60 + day.tm_sec;
    if (getenv("OFFROAD_TEST_SECONDS")) seconds = atol(getenv("OFFROAD_TEST_SECONDS"));   /* a test: the same seed everywhere */
    mc.ticks = (uint32_t)(seconds * 1193182.0 / 65536);   /* the BIOS count */

    {   /* the records kept from earlier games */
        char path[1200];
        snprintf(path, sizeof path, "%sironhi.bin", h->save);
        FILE *f = fopen(path, "rb");
        if (f) {
            static uint8_t data[0x406];
            long long n = (long long)fread(data, 1, sizeof data, f);
            fclose(f);
            snprintf(mc.written[0].name, sizeof mc.written[0].name, "ironhi.bin");
            mc.written[0].data = data; mc.written[0].n = n; mc.written[0].owned = 0; mc.nwritten = 1;
        }
    }

    Program *p = program_new(&mc, dir);
    h->p = p;
    if (SDL_NumJoysticks() > 0) open_controller(h, 0);
    ProgramHost host = {h, wait_keys, before_tick, after_tick, port, saved, 1};
    p->host = host;
    h->start = now();
    int out = program_start(p);
    Truck *k = 0;
    if (out == GAME_RUNNING) {
        k = truck_load(dir);
        static uint8_t scratch[65536];
        static Cycle c;
        static Printer printer;
        static Game g;
        cycle_init(&c, k, scratch, LEVEL_GAME, 3, 3);
        c.live = 1;
        c.acceleration_fix = fix_acceleration;
        printer.far = p->far;
        c.printer = &printer;
        game_init(&g, &c, dir);
        program_attach(p, &g);
#ifdef __EMSCRIPTEN__
        race_pause_original = g.program.pause;
        g.program.pause = race_pause;
        race_tick = -100;
        web_f1_release = 0;
        web_esc_in_pause = 0;
        web_game = &g;
        web_host = h;
        h->r1 = 0;
#endif
        long long v = 0;
        out = game_run(&g, G_DEMO, -1, 0, 0, &v);
        while (out == GAME_RUNNING) {
            out = game_run(&g, G_GAME, 0, 0, 0, &v);
            if (out == GAME_RUNNING && v == 1) out = game_run(&g, G_DEMO, -1, 0, 0, &v);
        }
    }
    if (out == GAME_EXIT && p->output.n) fwrite(p->output.v, 1, p->output.n, stdout);
    else if (out != GAME_EXIT) fprintf(stderr, "offroad: the program stopped (%d)\n", out);
    int code = out == GAME_EXIT ? p->run->exit_code : 1;
#ifdef __EMSCRIPTEN__
    if (h->joystick) close_controller(h);   /* the next run opens it again on its own program */
    truck_free(k);
    program_free(p);
    h->p = 0;
#endif
    return code;
}

int main(int argc, char **argv) {
    char dir[1024] = "";
    int fix_acceleration = 0;
#ifdef __EMSCRIPTEN__
    /* the site's edition: the original files packed at /OFFROAD, and the
     * acceleration as it should be (the user's choice) */
    snprintf(dir, sizeof dir, "/OFFROAD");
    fix_acceleration = 1;
    (void)argc; (void)argv;
#else
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fix-acceleration")) fix_acceleration = 1;
        else snprintf(dir, sizeof dir, "%s", argv[i]);
    }
    if (!dir[0]) {
        char *base = SDL_GetBasePath();
        const char *tries[3] = {"../../OFFROAD", "../OFFROAD", "OFFROAD"};
        for (int i = 0; i < 3 && !dir[0]; i++) {
            char path[1024];
            snprintf(path, sizeof path, "%s%s", base ? base : "", tries[i]);
            if (folder_has_game(path)) snprintf(dir, sizeof dir, "%s", path);
        }
        SDL_free(base);
    }
#endif
    if (!dir[0] || !folder_has_game(dir)) { fprintf(stderr, "offroad: the folder of the original files was not found\n"); return 1; }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) < 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    static Host h;
    h.test_keys = getenv("OFFROAD_TEST_KEYS"); h.test_tick_keys = getenv("OFFROAD_TEST_TICKKEYS");
    h.test_shot = getenv("OFFROAD_TEST_SHOT");
    if (getenv("OFFROAD_TEST_WAV")) h.test_wav = fopen(getenv("OFFROAD_TEST_WAV"), "wb");
    h.test_ticks = getenv("OFFROAD_TEST_TICKS") ? atol(getenv("OFFROAD_TEST_TICKS")) : 0;
#ifdef __EMSCRIPTEN__
    /* the page decides the canvas's size: 640 x 480, 320 x 200 on a 4:3 screen */
    h.window = SDL_CreateWindow("Super Off Road", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480, 0);
    h.renderer = SDL_CreateRenderer(h.window, -1, 0);
    SDL_RenderSetLogicalSize(h.renderer, 640, 480);
#else
    h.window = SDL_CreateWindow("Super Off Road", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 720, SDL_WINDOW_RESIZABLE);
    h.renderer = SDL_CreateRenderer(h.window, -1, 0);   /* no vsync: the ticks keep their own time */
    SDL_RenderSetLogicalSize(h.renderer, 960, 720);   /* 320 x 200 on a 4:3 screen */
#endif
    /* the game reads keys, not text: no text input, so macOS does not open
     * its accent picker when a letter is held */
    SDL_StopTextInput();
    h.texture = SDL_CreateTexture(h.renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, 320, 200);
    SDL_AudioSpec want = {0}, got;
    want.freq = AUDIO_RATE; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
    /* one device for the whole program, in the browser too: closing and
     * opening it again there is a use of freed memory (Forget-Me-Not) */
    h.audio = SDL_OpenAudioDevice(0, 0, &want, &got, 0);
    if (h.audio) {
        queue_margin(&h);
        SDL_PauseAudioDevice(h.audio, 0);
    }
#ifdef __EMSCRIPTEN__
    /* the records live in the browser (IndexedDB), read before the start */
    EM_ASM({
        FS.mkdir('/save');
        FS.mount(IDBFS, {}, '/save');
        Module.offroadLetti = 0;
        FS.syncfs(true, function () { Module.offroadLetti = 1; });
    });
    while (!EM_ASM_INT({ return Module.offroadLetti; })) SDL_Delay(10);
    snprintf(h.save, sizeof h.save, "/save/");

    /* a run, then the window closed (or the game quit by itself: the site
     * closes the window): the machine waits off until it opens again */
    for (;;) {
        play(&h, dir, fix_acceleration);
        if (!web_closed) {
            web_closed = 1;
            EM_ASM({ if (window.CVOS_offroadUscita) window.CVOS_offroadUscita(); });
        }
        if (h.audio) { SDL_PauseAudioDevice(h.audio, 1); SDL_ClearQueuedAudio(h.audio); }
        while (web_closed) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {}
            SDL_Delay(50);
        }
        if (h.audio) { queue_margin(&h); SDL_PauseAudioDevice(h.audio, 0); }
    }
#else
    {   /* the first controller (macOS lists them a moment after the start) */
        Uint32 t0 = SDL_GetTicks();
        while (SDL_NumJoysticks() == 0 && SDL_GetTicks() - t0 < 1500) { SDL_PumpEvents(); SDL_Delay(20); }
    }
    char *pref = SDL_GetPrefPath("carlosinatra", "Super Off Road");
    snprintf(h.save, sizeof h.save, "%s", pref ? pref : "./");
    SDL_free(pref);
    int code = play(&h, dir, fix_acceleration);
    if (h.test_wav) fclose(h.test_wav);
    SDL_Quit();
    return code;
#endif
}
