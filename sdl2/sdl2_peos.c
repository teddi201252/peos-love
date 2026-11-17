/*
 * SDL2 on PeOS: the part of SDL 2.30's API that LÖVE 11.5 uses (149 calls,
 * counted from its objects), written for PeOS over SDL's own headers (zlib,
 * as SDL). docs/game-ports-plan.md, Balatro phase 3.
 *
 * - The window is the body of a Starlight window whose pixels are the
 *   app's own (SLUI): one window, sized by Starlight (the size asked for is
 *   a wish; the size given comes as SDL_WINDOWEVENT_SIZE_CHANGED). Waiting
 *   for an event is waiting on standard input, where Starlight writes: no
 *   polling while idle.
 * - OpenGL is peos_gl (Mesa: iris on the Intel GPU, virgl in a VM); a swap
 *   copies the frame into the window's pixels; the swap interval paces to
 *   60 frames a second (there is no vertical blank to wait for).
 * - Keys: PeOS's PC set-1 codes to SDL's scan codes, and the US layout for
 *   the key codes; the characters typed come from Starlight (any layout).
 * - Threads, mutexes and condition variables: POSIX's (libpeos-compat).
 * - Not here: joysticks, game controllers and haptics (PeOS has no gamepad
 *   driver yet: none are found), relative mouse mode (reported, the pointer
 *   is Starlight's), several displays, loading shared objects (programs are
 *   static), audio (LÖVE uses OpenAL).
 *
 * Copyright (c) 2026 PeOS. zlib license, as SDL.
 */
#include "SDL.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <peos/peos.h>
#include <peos/power.h>
#include <peos/starlight_ui.h>
#include <peos_gl.h>

/* ---- errors, hints, init, time ---- */

static char g_error[256];
static Uint32 g_inited;
static uint64_t g_start_ns;
static int g_frames_every, g_frames_scale;   /* frames to the log (frames_config) */

static int set_error(const char *what) {
    snprintf(g_error, sizeof(g_error), "%s", what);
    return -1;
}

const char *SDL_GetError(void) { return g_error; }

void SDL_free(void *mem) { free(mem); }

void SDL_GetVersion(SDL_version *ver) {
    if (!ver) return;
    ver->major = SDL_MAJOR_VERSION;
    ver->minor = SDL_MINOR_VERSION;
    ver->patch = SDL_PATCHLEVEL;
}

const char *SDL_GetPlatform(void) { return "PeOS"; }

#define HINTS 32
static struct { char name[64]; char value[128]; } g_hints[HINTS];

SDL_bool SDL_SetHint(const char *name, const char *value) {
    if (!name) return SDL_FALSE;
    int free_at = -1;
    for (int i = 0; i < HINTS; i++) {
        if (!g_hints[i].name[0]) { if (free_at < 0) free_at = i; continue; }
        if (!strcmp(g_hints[i].name, name)) { free_at = i; break; }
    }
    if (free_at < 0) return SDL_FALSE;
    snprintf(g_hints[free_at].name, sizeof(g_hints[free_at].name), "%s", name);
    snprintf(g_hints[free_at].value, sizeof(g_hints[free_at].value), "%s", value ? value : "");
    return SDL_TRUE;
}

const char *SDL_GetHint(const char *name) {
    for (int i = 0; name && i < HINTS; i++)
        if (g_hints[i].name[0] && !strcmp(g_hints[i].name, name)) return g_hints[i].value;
    return NULL;
}

int SDL_InitSubSystem(Uint32 flags) {
    if (!g_start_ns) g_start_ns = peos_time_ns();
    g_inited |= flags;
    return 0;
}

void SDL_QuitSubSystem(Uint32 flags) { g_inited &= ~flags; }

Uint32 SDL_WasInit(Uint32 flags) { return flags ? (g_inited & flags) : g_inited; }

Uint32 SDL_GetTicks(void) {
    if (!g_start_ns) g_start_ns = peos_time_ns();
    return (Uint32)((peos_time_ns() - g_start_ns) / 1000000ull);
}

void SDL_Delay(Uint32 ms) { peos_sleep(ms); }

int SDL_GetCPUCount(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

SDL_PowerState SDL_GetPowerInfo(int *seconds, int *percent) {
    peos_battery_t b;
    memset(&b, 0, sizeof(b));
    if (seconds) *seconds = -1;
    if (percent) *percent = -1;
    if (peos_battery(&b) != 0 || !(b.flags & PEOS_BATTERY_PRESENT)) return SDL_POWERSTATE_NO_BATTERY;
    if (percent) *percent = (int)b.percent;
    if (seconds && b.current_ma < 0 && b.remaining_mah)
        *seconds = (int)((uint64_t)b.remaining_mah * 3600u / (uint64_t)(-b.current_ma));
    if (b.flags & PEOS_BATTERY_FULL) return SDL_POWERSTATE_CHARGED;
    if (b.flags & PEOS_BATTERY_CHARGING) return SDL_POWERSTATE_CHARGING;
    return (b.flags & PEOS_BATTERY_AC) ? SDL_POWERSTATE_CHARGED : SDL_POWERSTATE_ON_BATTERY;
}

/* ---- threads: POSIX's ---- */

struct SDL_Thread {
    pthread_t t;
    SDL_ThreadFunction fn;
    void *data;
    int status;
};

static void *thread_main(void *arg) {
    SDL_Thread *th = (SDL_Thread *)arg;
    th->status = th->fn(th->data);
    return NULL;
}

SDL_Thread *SDL_CreateThread(SDL_ThreadFunction fn, const char *name, void *data) {
    SDL_Thread *th = (SDL_Thread *)calloc(1, sizeof(*th));
    if (!th) return NULL;
    th->fn = fn;
    th->data = data;
    if (pthread_create(&th->t, NULL, thread_main, th) != 0) {
        free(th);
        set_error("no thread");
        return NULL;
    }
    if (name) pthread_setname_np(th->t, name);
    return th;
}

void SDL_WaitThread(SDL_Thread *th, int *status) {
    if (!th) return;
    pthread_join(th->t, NULL);
    if (status) *status = th->status;
    free(th);
}

void SDL_DetachThread(SDL_Thread *th) {
    if (!th) return;
    pthread_detach(th->t);   /* the thread's struct is kept: it reads it to the end */
}

struct SDL_mutex { pthread_mutex_t m; };
struct SDL_cond { pthread_cond_t c; };

SDL_mutex *SDL_CreateMutex(void) {
    SDL_mutex *m = (SDL_mutex *)calloc(1, sizeof(*m));
    if (!m) return NULL;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);   /* SDL's mutexes are recursive */
    pthread_mutex_init(&m->m, &a);
    pthread_mutexattr_destroy(&a);
    return m;
}

void SDL_DestroyMutex(SDL_mutex *m) {
    if (!m) return;
    pthread_mutex_destroy(&m->m);
    free(m);
}

int SDL_LockMutex(SDL_mutex *m) { return m ? pthread_mutex_lock(&m->m) : -1; }
int SDL_UnlockMutex(SDL_mutex *m) { return m ? pthread_mutex_unlock(&m->m) : -1; }

SDL_cond *SDL_CreateCond(void) {
    SDL_cond *c = (SDL_cond *)calloc(1, sizeof(*c));
    if (c) pthread_cond_init(&c->c, NULL);
    return c;
}

void SDL_DestroyCond(SDL_cond *c) {
    if (!c) return;
    pthread_cond_destroy(&c->c);
    free(c);
}

int SDL_CondSignal(SDL_cond *c) { return c ? pthread_cond_signal(&c->c) : -1; }
int SDL_CondBroadcast(SDL_cond *c) { return c ? pthread_cond_broadcast(&c->c) : -1; }
int SDL_CondWait(SDL_cond *c, SDL_mutex *m) { return c && m ? pthread_cond_wait(&c->c, &m->m) : -1; }

int SDL_CondWaitTimeout(SDL_cond *c, SDL_mutex *m, Uint32 ms) {
    if (!c || !m) return -1;
    if (ms == SDL_MUTEX_MAXWAIT) return SDL_CondWait(c, m);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000u;
    ts.tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    int r = pthread_cond_timedwait(&c->c, &m->m, &ts);
    return r == 0 ? 0 : r == ETIMEDOUT ? SDL_MUTEX_TIMEDOUT : -1;
}

/* ---- shared objects: none (PeOS's programs are static) ---- */

void *SDL_LoadObject(const char *sofile) {
    (void)sofile;
    set_error("PeOS programs are static: nothing to load");
    return NULL;
}
void *SDL_LoadFunction(void *handle, const char *name) {
    (void)handle;
    (void)name;
    return NULL;
}
void SDL_UnloadObject(void *handle) { (void)handle; }

/* ---- surfaces (an icon, a cursor's image): the pixels given, kept as they are ---- */

SDL_Surface *SDL_CreateRGBSurfaceFrom(void *pixels, int width, int height, int depth, int pitch, Uint32 Rmask,
                                      Uint32 Gmask, Uint32 Bmask, Uint32 Amask) {
    (void)depth;
    (void)Rmask;
    (void)Gmask;
    (void)Bmask;
    (void)Amask;
    SDL_Surface *s = (SDL_Surface *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->flags = SDL_PREALLOC;
    s->w = width;
    s->h = height;
    s->pitch = pitch;
    s->pixels = pixels;
    s->refcount = 1;
    return s;
}

void SDL_FreeSurface(SDL_Surface *s) { free(s); }

/* ---- the event queue ---- */

#define QUEUE 512
static SDL_Event g_queue[QUEUE];
static int g_qhead, g_qcount;
static pthread_mutex_t g_qlock = PTHREAD_MUTEX_INITIALIZER;

#define WATCHES 8
static struct { SDL_EventFilter fn; void *data; } g_watch[WATCHES];

static void push(const SDL_Event *e) {
    for (int i = 0; i < WATCHES; i++)
        if (g_watch[i].fn) (void)g_watch[i].fn(g_watch[i].data, (SDL_Event *)e);
    pthread_mutex_lock(&g_qlock);
    if (g_qcount < QUEUE) {
        g_queue[(g_qhead + g_qcount) % QUEUE] = *e;
        g_qcount++;
    }
    pthread_mutex_unlock(&g_qlock);
}

static int pop(SDL_Event *out) {
    pthread_mutex_lock(&g_qlock);
    int got = g_qcount > 0;
    if (got) {
        if (out) *out = g_queue[g_qhead];
        g_qhead = (g_qhead + 1) % QUEUE;
        g_qcount--;
    }
    pthread_mutex_unlock(&g_qlock);
    return got;
}

void SDL_AddEventWatch(SDL_EventFilter filter, void *userdata) {
    for (int i = 0; i < WATCHES; i++)
        if (!g_watch[i].fn) {
            g_watch[i].fn = filter;
            g_watch[i].data = userdata;
            return;
        }
}

void SDL_DelEventWatch(SDL_EventFilter filter, void *userdata) {
    for (int i = 0; i < WATCHES; i++)
        if (g_watch[i].fn == filter && g_watch[i].data == userdata) g_watch[i].fn = NULL;
}

void SDL_FlushEvent(Uint32 type) {
    pthread_mutex_lock(&g_qlock);
    int kept = 0;
    for (int i = 0; i < g_qcount; i++) {
        SDL_Event e = g_queue[(g_qhead + i) % QUEUE];
        if (e.type != type) g_queue[(g_qhead + kept++) % QUEUE] = e;
    }
    g_qcount = kept;
    pthread_mutex_unlock(&g_qlock);
}

/* ---- the window ---- */

struct SDL_Window {
    int w, h;            /* the body Starlight gave */
    Uint32 flags;
    int min_w, min_h;
    char title[128];
};

static struct {
    SDL_Window *window;
    SDL_Window one;
    uint32_t surf_w, surf_h;   /* the pixels asked for (a little larger) */
    int pixels_ready;
    unsigned buttons;          /* SLUI's bits: 0 left, 1 right, 2 middle */
    int mouse_x, mouse_y, entered;
    int clipboard_here;
    char *clipboard;
    int cursor_shown;
    SDL_Cursor *cursor;
    int text_input;
    Uint8 keys[SDL_NUM_SCANCODES];
    Uint16 mods;
    int screensaver;
    int relative;
    SDL_bool grab;
} g = { .cursor_shown = 1, .text_input = 1, .screensaver = 1 };

static void send_frame(void) {
    uint32_t w = g.window ? (uint32_t)g.window->w : 0, h = g.window ? (uint32_t)g.window->h : 0;
    slui_begin_frame();
    if (g.pixels_ready && w && h) slui_pixels(0, 0, w, h, 0, 0);
    else slui_rect(0, 0, w ? w : 1, h ? h : 1, 18, 20, 30);
    slui_end_frame();
}

static void window_event(Uint8 what, int a, int b) {
    if (!g.window) return;
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = SDL_WINDOWEVENT;
    e.window.timestamp = SDL_GetTicks();
    e.window.windowID = 1;
    e.window.event = what;
    e.window.data1 = a;
    e.window.data2 = b;
    push(&e);
}

struct peos_gl_context;
static peos_gl_context *g_current_gl;
static int g_gl_w, g_gl_h;

/* Starlight said the body's size: the window, the pixels, the GL frame. */
static void layout(void) {
    uint32_t w = 0, h = 0;
    slui_viewport(&w, &h);
    if (!g.window || !w || !h) return;
    if (w > g.surf_w || h > g.surf_h || !g.pixels_ready) {
        if (w > g.surf_w) g.surf_w = w + 64;
        if (h > g.surf_h) g.surf_h = h + 64;
        g.pixels_ready = 0;
        slui_pixels_open(g.surf_w, g.surf_h);
    }
    if ((int)w != g.window->w || (int)h != g.window->h) {
        g.window->w = (int)w;
        g.window->h = (int)h;
        if (g_current_gl) {
            peos_gl_resize(g_current_gl, (int)w, (int)h);
            g_gl_w = (int)w;
            g_gl_h = (int)h;
        }
        window_event(SDL_WINDOWEVENT_SIZE_CHANGED, (int)w, (int)h);
        window_event(SDL_WINDOWEVENT_RESIZED, (int)w, (int)h);
    }
    send_frame();
}

/* ---- keys: PC set-1 codes (PeOS's) to SDL's scan codes ---- */

static SDL_Scancode scancode_of(uint16_t code) {
    static const unsigned char plain[0x59] = {
        [0x01] = SDL_SCANCODE_ESCAPE, [0x02] = SDL_SCANCODE_1, [0x03] = SDL_SCANCODE_2, [0x04] = SDL_SCANCODE_3,
        [0x05] = SDL_SCANCODE_4, [0x06] = SDL_SCANCODE_5, [0x07] = SDL_SCANCODE_6, [0x08] = SDL_SCANCODE_7,
        [0x09] = SDL_SCANCODE_8, [0x0A] = SDL_SCANCODE_9, [0x0B] = SDL_SCANCODE_0, [0x0C] = SDL_SCANCODE_MINUS,
        [0x0D] = SDL_SCANCODE_EQUALS, [0x0E] = SDL_SCANCODE_BACKSPACE, [0x0F] = SDL_SCANCODE_TAB,
        [0x10] = SDL_SCANCODE_Q, [0x11] = SDL_SCANCODE_W, [0x12] = SDL_SCANCODE_E, [0x13] = SDL_SCANCODE_R,
        [0x14] = SDL_SCANCODE_T, [0x15] = SDL_SCANCODE_Y, [0x16] = SDL_SCANCODE_U, [0x17] = SDL_SCANCODE_I,
        [0x18] = SDL_SCANCODE_O, [0x19] = SDL_SCANCODE_P, [0x1A] = SDL_SCANCODE_LEFTBRACKET,
        [0x1B] = SDL_SCANCODE_RIGHTBRACKET, [0x1C] = SDL_SCANCODE_RETURN, [0x1D] = SDL_SCANCODE_LCTRL,
        [0x1E] = SDL_SCANCODE_A, [0x1F] = SDL_SCANCODE_S, [0x20] = SDL_SCANCODE_D, [0x21] = SDL_SCANCODE_F,
        [0x22] = SDL_SCANCODE_G, [0x23] = SDL_SCANCODE_H, [0x24] = SDL_SCANCODE_J, [0x25] = SDL_SCANCODE_K,
        [0x26] = SDL_SCANCODE_L, [0x27] = SDL_SCANCODE_SEMICOLON, [0x28] = SDL_SCANCODE_APOSTROPHE,
        [0x29] = SDL_SCANCODE_GRAVE, [0x2A] = SDL_SCANCODE_LSHIFT, [0x2B] = SDL_SCANCODE_BACKSLASH,
        [0x2C] = SDL_SCANCODE_Z, [0x2D] = SDL_SCANCODE_X, [0x2E] = SDL_SCANCODE_C, [0x2F] = SDL_SCANCODE_V,
        [0x30] = SDL_SCANCODE_B, [0x31] = SDL_SCANCODE_N, [0x32] = SDL_SCANCODE_M, [0x33] = SDL_SCANCODE_COMMA,
        [0x34] = SDL_SCANCODE_PERIOD, [0x35] = SDL_SCANCODE_SLASH, [0x36] = SDL_SCANCODE_RSHIFT,
        [0x37] = SDL_SCANCODE_KP_MULTIPLY, [0x38] = SDL_SCANCODE_LALT, [0x39] = SDL_SCANCODE_SPACE,
        [0x3A] = SDL_SCANCODE_CAPSLOCK, [0x3B] = SDL_SCANCODE_F1, [0x3C] = SDL_SCANCODE_F2,
        [0x3D] = SDL_SCANCODE_F3, [0x3E] = SDL_SCANCODE_F4, [0x3F] = SDL_SCANCODE_F5, [0x40] = SDL_SCANCODE_F6,
        [0x41] = SDL_SCANCODE_F7, [0x42] = SDL_SCANCODE_F8, [0x43] = SDL_SCANCODE_F9, [0x44] = SDL_SCANCODE_F10,
        [0x45] = SDL_SCANCODE_NUMLOCKCLEAR, [0x46] = SDL_SCANCODE_SCROLLLOCK,
        /* PeOS gives the navigation keys also without the 0xE0 */
        [0x47] = SDL_SCANCODE_HOME, [0x48] = SDL_SCANCODE_UP, [0x49] = SDL_SCANCODE_PAGEUP,
        [0x4A] = SDL_SCANCODE_KP_MINUS, [0x4B] = SDL_SCANCODE_LEFT, [0x4C] = SDL_SCANCODE_KP_5,
        [0x4D] = SDL_SCANCODE_RIGHT, [0x4E] = SDL_SCANCODE_KP_PLUS, [0x4F] = SDL_SCANCODE_END,
        [0x50] = SDL_SCANCODE_DOWN, [0x51] = SDL_SCANCODE_PAGEDOWN, [0x52] = SDL_SCANCODE_INSERT,
        [0x53] = SDL_SCANCODE_DELETE, [0x56] = SDL_SCANCODE_NONUSBACKSLASH, [0x57] = SDL_SCANCODE_F11,
        [0x58] = SDL_SCANCODE_F12,
    };
    if (code < 0x59u) return (SDL_Scancode)plain[code];
    switch (code) {
        case 0xE01C: return SDL_SCANCODE_KP_ENTER;
        case 0xE01D: return SDL_SCANCODE_RCTRL;
        case 0xE035: return SDL_SCANCODE_KP_DIVIDE;
        case 0xE038: return SDL_SCANCODE_RALT;
        case 0xE047: return SDL_SCANCODE_HOME;
        case 0xE048: return SDL_SCANCODE_UP;
        case 0xE049: return SDL_SCANCODE_PAGEUP;
        case 0xE04B: return SDL_SCANCODE_LEFT;
        case 0xE04D: return SDL_SCANCODE_RIGHT;
        case 0xE04F: return SDL_SCANCODE_END;
        case 0xE050: return SDL_SCANCODE_DOWN;
        case 0xE051: return SDL_SCANCODE_PAGEDOWN;
        case 0xE052: return SDL_SCANCODE_INSERT;
        case 0xE053: return SDL_SCANCODE_DELETE;
        case 0xE05B: return SDL_SCANCODE_LGUI;
        case 0xE05C: return SDL_SCANCODE_RGUI;
        case 0xE05D: return SDL_SCANCODE_APPLICATION;
        default: return SDL_SCANCODE_UNKNOWN;
    }
}

/* The US layout: the key a scan code makes. */
SDL_Keycode SDL_GetKeyFromScancode(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return SDLK_a + (sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return SDLK_1 + (sc - SDL_SCANCODE_1);
    switch (sc) {
        case SDL_SCANCODE_0: return SDLK_0;
        case SDL_SCANCODE_RETURN: return SDLK_RETURN;
        case SDL_SCANCODE_ESCAPE: return SDLK_ESCAPE;
        case SDL_SCANCODE_BACKSPACE: return SDLK_BACKSPACE;
        case SDL_SCANCODE_TAB: return SDLK_TAB;
        case SDL_SCANCODE_SPACE: return SDLK_SPACE;
        case SDL_SCANCODE_MINUS: return SDLK_MINUS;
        case SDL_SCANCODE_EQUALS: return SDLK_EQUALS;
        case SDL_SCANCODE_LEFTBRACKET: return SDLK_LEFTBRACKET;
        case SDL_SCANCODE_RIGHTBRACKET: return SDLK_RIGHTBRACKET;
        case SDL_SCANCODE_BACKSLASH: return SDLK_BACKSLASH;
        case SDL_SCANCODE_NONUSBACKSLASH: return SDLK_LESS;
        case SDL_SCANCODE_SEMICOLON: return SDLK_SEMICOLON;
        case SDL_SCANCODE_APOSTROPHE: return SDLK_QUOTE;
        case SDL_SCANCODE_GRAVE: return SDLK_BACKQUOTE;
        case SDL_SCANCODE_COMMA: return SDLK_COMMA;
        case SDL_SCANCODE_PERIOD: return SDLK_PERIOD;
        case SDL_SCANCODE_SLASH: return SDLK_SLASH;
        case SDL_SCANCODE_DELETE: return SDLK_DELETE;
        default: return (SDL_Keycode)SDL_SCANCODE_TO_KEYCODE(sc);
    }
}

SDL_Scancode SDL_GetScancodeFromKey(SDL_Keycode key) {
    for (int sc = 0; sc < SDL_NUM_SCANCODES; sc++)
        if (SDL_GetKeyFromScancode((SDL_Scancode)sc) == key) return (SDL_Scancode)sc;
    return SDL_SCANCODE_UNKNOWN;
}

const Uint8 *SDL_GetKeyboardState(int *numkeys) {
    if (numkeys) *numkeys = SDL_NUM_SCANCODES;
    return g.keys;
}

static Uint16 mods_of(uint16_t m) {
    return ((m & SLUI_MOD_SHIFT) ? KMOD_LSHIFT : 0) | ((m & SLUI_MOD_CTRL) ? KMOD_LCTRL : 0) |
           ((m & SLUI_MOD_ALT) ? KMOD_LALT : 0) | ((m & SLUI_MOD_SUPER) ? KMOD_LGUI : 0);
}

void SDL_StartTextInput(void) { g.text_input = 1; }
void SDL_StopTextInput(void) { g.text_input = 0; }
SDL_bool SDL_IsTextInputActive(void) { return g.text_input ? SDL_TRUE : SDL_FALSE; }
void SDL_SetTextInputRect(const SDL_Rect *rect) { (void)rect; }
SDL_bool SDL_HasScreenKeyboardSupport(void) { return SDL_FALSE; }
SDL_Window *SDL_GetKeyboardFocus(void) { return g.window; }

/* ---- the pointer ---- */

static Uint32 sdl_buttons(unsigned slui) {
    return ((slui & 1u) ? SDL_BUTTON_LMASK : 0) | ((slui & 2u) ? SDL_BUTTON_RMASK : 0) |
           ((slui & 4u) ? SDL_BUTTON_MMASK : 0);
}

Uint32 SDL_GetMouseState(int *x, int *y) {
    if (x) *x = g.mouse_x;
    if (y) *y = g.mouse_y;
    return sdl_buttons(g.buttons);
}

SDL_Window *SDL_GetMouseFocus(void) { return g.entered ? g.window : NULL; }
void SDL_WarpMouseInWindow(SDL_Window *window, int x, int y) {
    (void)window;   /* Starlight moves the pointer, not apps: where the game thinks it is */
    g.mouse_x = x;
    g.mouse_y = y;
}

int SDL_SetRelativeMouseMode(SDL_bool enabled) {
    g.relative = enabled;   /* motions carry xrel/yrel anyway; the pointer stays Starlight's */
    return 0;
}
SDL_bool SDL_GetRelativeMouseMode(void) { return g.relative ? SDL_TRUE : SDL_FALSE; }

/* Cursors: Starlight's arrow, hand or text beam, or none. An image cursor
 * shows Starlight's arrow (not drawn into the frame yet). */
struct SDL_Cursor { int kind; };
static SDL_Cursor g_arrow = { SLUI_CURSOR_ARROW };

static void update_pointer(void) {
    slui_pixels_cursor(!g.cursor_shown ? SLUI_CURSOR_NONE : (slui_cursor_t)(g.cursor ? g.cursor->kind : SLUI_CURSOR_ARROW));
}

SDL_Cursor *SDL_CreateSystemCursor(SDL_SystemCursor id) {
    SDL_Cursor *c = (SDL_Cursor *)malloc(sizeof(*c));
    if (!c) return NULL;
    c->kind = id == SDL_SYSTEM_CURSOR_HAND ? SLUI_CURSOR_HAND : id == SDL_SYSTEM_CURSOR_IBEAM ? SLUI_CURSOR_TEXT
                                                                                           : SLUI_CURSOR_ARROW;
    return c;
}

SDL_Cursor *SDL_CreateColorCursor(SDL_Surface *surface, int hot_x, int hot_y) {
    (void)surface;
    (void)hot_x;
    (void)hot_y;
    return SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
}

void SDL_SetCursor(SDL_Cursor *cursor) {
    g.cursor = cursor;
    update_pointer();
}

SDL_Cursor *SDL_GetDefaultCursor(void) { return &g_arrow; }

void SDL_FreeCursor(SDL_Cursor *cursor) {
    if (!cursor || cursor == &g_arrow) return;
    if (g.cursor == cursor) {
        g.cursor = NULL;
        update_pointer();
    }
    free(cursor);
}

int SDL_ShowCursor(int toggle) {
    if (toggle == SDL_ENABLE || toggle == SDL_DISABLE) {
        g.cursor_shown = toggle == SDL_ENABLE;
        update_pointer();
    }
    return g.cursor_shown ? SDL_ENABLE : SDL_DISABLE;
}

/* ---- events from Starlight ---- */

static void utf8(uint32_t cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; out[1] = 0; }
    else if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); out[2] = 0; }
    else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F)); out[3] = 0;
    } else {
        out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F)); out[4] = 0;
    }
}

static void pointer(const slui_event_t *ev) {
    int x = (int)ev->value, y = (int)ev->value2;
    SDL_Event e;
    if (!g.entered) {
        g.entered = 1;
        window_event(SDL_WINDOWEVENT_ENTER, 0, 0);
    }
    if (ev->id == SLUI_POINTER_WHEEL) {
        memset(&e, 0, sizeof(e));
        e.type = SDL_MOUSEWHEEL;
        e.wheel.timestamp = SDL_GetTicks();
        e.wheel.windowID = 1;
        e.wheel.y = ev->wheel;
        e.wheel.preciseY = (float)ev->wheel;
        e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
        e.wheel.mouseX = g.mouse_x;
        e.wheel.mouseY = g.mouse_y;
        push(&e);
        return;
    }
    unsigned held = ev->keycode & 7u;
    if (x != g.mouse_x || y != g.mouse_y) {
        memset(&e, 0, sizeof(e));
        e.type = SDL_MOUSEMOTION;
        e.motion.timestamp = SDL_GetTicks();
        e.motion.windowID = 1;
        e.motion.state = sdl_buttons(held);
        e.motion.x = x;
        e.motion.y = y;
        e.motion.xrel = x - g.mouse_x;
        e.motion.yrel = y - g.mouse_y;
        g.mouse_x = x;
        g.mouse_y = y;
        push(&e);
    }
    unsigned changed = held ^ g.buttons;
    static const Uint8 sdl_button[3] = { SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT, SDL_BUTTON_MIDDLE };
    for (int b = 0; b < 3; b++) {
        if (!(changed & (1u << b))) continue;
        memset(&e, 0, sizeof(e));
        int down = (held & (1u << b)) != 0;
        e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
        e.button.timestamp = SDL_GetTicks();
        e.button.windowID = 1;
        e.button.button = sdl_button[b];
        e.button.state = down ? SDL_PRESSED : SDL_RELEASED;
        e.button.clicks = 1;
        e.button.x = x;
        e.button.y = y;
        push(&e);
        if (g_frames_every && down) fprintf(stderr, "love: click %d at %d,%d of the window\n", b, x, y);
    }
    g.buttons = held;
}

static void handle(const slui_event_t *ev) {
    SDL_Event e;
    switch (ev->type) {
        case SLUI_EVENT_RESIZE:
            layout();
            break;
        case SLUI_EVENT_PIXELS:
            g.pixels_ready = ev->value > 0 && ev->value2 > 0;
            send_frame();
            window_event(SDL_WINDOWEVENT_EXPOSED, 0, 0);
            break;
        case SLUI_EVENT_KEY_DOWN:
        case SLUI_EVENT_KEY_REPEAT:
        case SLUI_EVENT_KEY_UP: {
            SDL_Scancode sc = scancode_of(ev->keycode);
            int down = ev->type != SLUI_EVENT_KEY_UP;
            g.mods = mods_of(ev->mods);
            if (sc != SDL_SCANCODE_UNKNOWN) g.keys[sc] = (Uint8)down;
            memset(&e, 0, sizeof(e));
            e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
            e.key.timestamp = SDL_GetTicks();
            e.key.windowID = 1;
            e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
            e.key.repeat = ev->type == SLUI_EVENT_KEY_REPEAT;
            e.key.keysym.scancode = sc;
            e.key.keysym.sym = SDL_GetKeyFromScancode(sc);
            e.key.keysym.mod = g.mods;
            push(&e);
            break;
        }
        case SLUI_EVENT_CHAR:
            if (g.text_input && ev->value >= 32 && ev->value != 127) {
                memset(&e, 0, sizeof(e));
                e.type = SDL_TEXTINPUT;
                e.text.timestamp = SDL_GetTicks();
                e.text.windowID = 1;
                utf8(ev->value, e.text.text);
                push(&e);
            }
            break;
        case SLUI_EVENT_POINTER:
            pointer(ev);
            break;
        case SLUI_EVENT_CLIPBOARD:
            free(g.clipboard);
            g.clipboard = strdup(ev->text ? ev->text : "");
            g.clipboard_here = 1;
            break;
        default:
            break;
    }
}

static int pump_once(void) {
    slui_event_t ev;
    int n = 0;
    while (slui_poll_event(&ev)) {
        handle(&ev);
        n++;
    }
    return n;
}

/* Waits up to timeout_ms (-1: for ever) for Starlight to write. */
static void pump_wait(int timeout_ms) {
    if (pump_once() > 0 || timeout_ms == 0) return;
    peos_wait_item_t item = { 0, PEOS_WAIT_READ, 0 };
    uint64_t ns = timeout_ms < 0 ? PEOS_WAIT_FOREVER : (uint64_t)timeout_ms * 1000000ull;
    if (peos_wait(&item, 1, ns) > 0) pump_once();
}

void SDL_PumpEvents(void) { pump_once(); }

int SDL_PollEvent(SDL_Event *event) {
    if (!g_qcount) pump_once();
    return pop(event);
}

int SDL_WaitEvent(SDL_Event *event) {
    while (!g_qcount) pump_wait(-1);
    return pop(event);
}

/* ---- displays: one, the screen ---- */

static void screen_size(int *w, int *h) {
    peos_fb_info_t fb;
    memset(&fb, 0, sizeof(fb));
    if (peos_fbinfo(&fb) == 0 && fb.width && fb.height) {
        *w = (int)fb.width;
        *h = (int)fb.height;
    } else {
        *w = g.window && g.window->w ? g.window->w : 1280;
        *h = g.window && g.window->h ? g.window->h : 800;
    }
}

static void the_mode(SDL_DisplayMode *mode) {
    memset(mode, 0, sizeof(*mode));
    mode->format = SDL_PIXELFORMAT_RGB888;
    screen_size(&mode->w, &mode->h);
    mode->refresh_rate = 60;
}

int SDL_GetNumVideoDisplays(void) { return 1; }
const char *SDL_GetDisplayName(int index) { return index == 0 ? "PeOS screen" : NULL; }
const char *SDL_GetCurrentVideoDriver(void) { return "peos"; }
int SDL_GetNumDisplayModes(int index) { return index == 0 ? 1 : -1; }

int SDL_GetDisplayBounds(int index, SDL_Rect *rect) {
    if (index != 0 || !rect) return set_error("no such display");
    rect->x = rect->y = 0;
    screen_size(&rect->w, &rect->h);
    return 0;
}

int SDL_GetDisplayMode(int index, int mode_index, SDL_DisplayMode *mode) {
    if (index != 0 || mode_index != 0 || !mode) return set_error("no such mode");
    the_mode(mode);
    return 0;
}

int SDL_GetDesktopDisplayMode(int index, SDL_DisplayMode *mode) { return SDL_GetDisplayMode(index, 0, mode); }
int SDL_GetCurrentDisplayMode(int index, SDL_DisplayMode *mode) { return SDL_GetDisplayMode(index, 0, mode); }

SDL_DisplayMode *SDL_GetClosestDisplayMode(int index, const SDL_DisplayMode *mode, SDL_DisplayMode *closest) {
    (void)mode;
    if (index != 0 || !closest) return NULL;
    the_mode(closest);
    return closest;
}

int SDL_GetWindowDisplayIndex(SDL_Window *window) { return window ? 0 : -1; }
int SDL_SetWindowDisplayMode(SDL_Window *window, const SDL_DisplayMode *mode) {
    (void)window;
    (void)mode;
    return 0;
}

void SDL_DisableScreenSaver(void) { g.screensaver = 0; }
void SDL_EnableScreenSaver(void) { g.screensaver = 1; }
SDL_bool SDL_IsScreenSaverEnabled(void) { return g.screensaver ? SDL_TRUE : SDL_FALSE; }

/* ---- the window's calls ---- */

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags) {
    (void)x;
    (void)y;
    if (g.window) {
        set_error("PeOS gives an app one window");
        return NULL;
    }
    g.window = &g.one;
    memset(g.window, 0, sizeof(*g.window));
    g.window->flags = flags | SDL_WINDOW_SHOWN | SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS;
    snprintf(g.window->title, sizeof(g.window->title), "%s", title ? title : "");
    if (!g_start_ns) g_start_ns = peos_time_ns();
    /* Lua's print shares standard output with Starlight's commands (which
     * go out whole, a line each): a whole line at a time, so the two never
     * mix inside one line (Starlight logs the game's lines as they are) */
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* Hello to Starlight: it answers with the body's size, then the pixels.
     * A window made again (LÖVE's restart, a new mode) takes the size
     * Starlight already said: it says it only when it changes. */
    layout();
    send_frame();
    for (int i = 0; i < 300 && (!g.window->w || !g.pixels_ready); i++) pump_wait(10);
    if (!g.window->w) {   /* no Starlight (started from a shell): the size asked for */
        int sw, sh;
        screen_size(&sw, &sh);
        g.window->w = w > 0 ? w : sw;
        g.window->h = h > 0 ? h : sh;
    }
    update_pointer();
    /* The language the user chose (Starlight said it with the size), for
     * the game's Lua: os.getenv("PEOS_LANG") ("it", "en", ...) */
    setenv("PEOS_LANG", slui_language(), 1);
    return g.window;
}

void SDL_DestroyWindow(SDL_Window *window) {
    if (window == g.window) g.window = NULL;
}

void SDL_SetWindowTitle(SDL_Window *window, const char *title) {
    if (window && title) snprintf(window->title, sizeof(window->title), "%s", title);   /* Starlight shows app.ini's */
}

void SDL_GetWindowSize(SDL_Window *window, int *w, int *h) {
    if (w) *w = window ? window->w : 0;
    if (h) *h = window ? window->h : 0;
}

void SDL_GetWindowPosition(SDL_Window *window, int *x, int *y) {
    (void)window;
    if (x) *x = 0;
    if (y) *y = 0;
}

void SDL_SetWindowPosition(SDL_Window *window, int x, int y) {
    (void)window;
    (void)x;
    (void)y;
}

Uint32 SDL_GetWindowFlags(SDL_Window *window) { return window ? window->flags : 0; }

int SDL_SetWindowFullscreen(SDL_Window *window, Uint32 flags) {
    if (!window) return -1;
    window->flags = (window->flags & ~(Uint32)SDL_WINDOW_FULLSCREEN_DESKTOP) | flags;   /* Starlight sizes it */
    return 0;
}

void SDL_SetWindowMinimumSize(SDL_Window *window, int min_w, int min_h) {
    if (!window) return;
    window->min_w = min_w;
    window->min_h = min_h;
}

void SDL_MinimizeWindow(SDL_Window *window) { (void)window; }
void SDL_MaximizeWindow(SDL_Window *window) { (void)window; }
void SDL_RestoreWindow(SDL_Window *window) { (void)window; }
void SDL_RaiseWindow(SDL_Window *window) { (void)window; }
void SDL_SetWindowIcon(SDL_Window *window, SDL_Surface *icon) {
    (void)window;   /* the icon is the bundle's */
    (void)icon;
}
void SDL_SetWindowGrab(SDL_Window *window, SDL_bool grabbed) {
    (void)window;
    g.grab = grabbed;
}
SDL_bool SDL_GetWindowGrab(SDL_Window *window) {
    (void)window;
    return g.grab;
}

/* ---- the clipboard: Starlight's ---- */

int SDL_SetClipboardText(const char *text) {
    slui_clipboard_copy(text ? text : "", text ? strlen(text) : 0);
    return 0;
}

char *SDL_GetClipboardText(void) {
    g.clipboard_here = 0;
    slui_clipboard_request();
    uint64_t until = peos_time_ns() + 200000000ull;
    while (!g.clipboard_here && peos_time_ns() < until) pump_wait(20);
    return strdup(g.clipboard_here && g.clipboard ? g.clipboard : "");
}

/* ---- message boxes: on stderr (the log), the first button answered ---- */

int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, SDL_Window *window) {
    (void)flags;
    (void)window;
    fprintf(stderr, "[%s] %s\n", title ? title : "", message ? message : "");
    return 0;
}

int SDL_ShowMessageBox(const SDL_MessageBoxData *data, int *buttonid) {
    if (!data) return -1;
    fprintf(stderr, "[%s] %s\n", data->title ? data->title : "", data->message ? data->message : "");
    if (buttonid) *buttonid = data->numbuttons > 0 ? data->buttons[0].buttonid : -1;
    return 0;
}

/* ---- OpenGL: peos_gl ---- */

static int g_attr[SDL_GL_FLOATBUFFERS + 1] = {
    [SDL_GL_RED_SIZE] = 8, [SDL_GL_GREEN_SIZE] = 8, [SDL_GL_BLUE_SIZE] = 8, [SDL_GL_ALPHA_SIZE] = 8,
    [SDL_GL_DOUBLEBUFFER] = 1, [SDL_GL_DEPTH_SIZE] = 16, [SDL_GL_CONTEXT_MAJOR_VERSION] = 2,
    [SDL_GL_CONTEXT_MINOR_VERSION] = 1, [SDL_GL_ACCELERATED_VISUAL] = 1,
};
static int g_interval = 1;
static uint64_t g_last_swap;

int SDL_GL_SetAttribute(SDL_GLattr attr, int value) {
    if ((int)attr < 0 || (int)attr > SDL_GL_FLOATBUFFERS) return set_error("unknown attribute");
    g_attr[attr] = value;
    return 0;
}

int SDL_GL_GetAttribute(SDL_GLattr attr, int *value) {
    if ((int)attr < 0 || (int)attr > SDL_GL_FLOATBUFFERS || !value) return set_error("unknown attribute");
    *value = g_attr[attr];
    return 0;
}

SDL_GLContext SDL_GL_CreateContext(SDL_Window *window) {
    if (!window) {
        set_error("no window");
        return NULL;
    }
    unsigned flags = 0;
    int major = g_attr[SDL_GL_CONTEXT_MAJOR_VERSION], minor = g_attr[SDL_GL_CONTEXT_MINOR_VERSION];
    if (g_attr[SDL_GL_CONTEXT_PROFILE_MASK] & SDL_GL_CONTEXT_PROFILE_ES) {
        set_error("PeOS: OpenGL only (no OpenGL ES)");
        return NULL;
    }
    if ((g_attr[SDL_GL_CONTEXT_PROFILE_MASK] & SDL_GL_CONTEXT_PROFILE_CORE) || major > 3 || (major == 3 && minor >= 2))
        flags |= PEOS_GL_CORE;
    if (g_attr[SDL_GL_DEPTH_SIZE] > 0 || g_attr[SDL_GL_STENCIL_SIZE] > 0) flags |= PEOS_GL_DEPTH;
    if (g_attr[SDL_GL_CONTEXT_FLAGS] & SDL_GL_CONTEXT_DEBUG_FLAG) flags |= PEOS_GL_DEBUG;
    peos_gl_context *c = peos_gl_create(window->w, window->h, major, minor, flags);
    if (!c) {
        set_error(peos_gl_error());
        return NULL;
    }
    g_current_gl = c;
    g_gl_w = window->w;
    g_gl_h = window->h;
    if (peos_gl_make_current(c) != 0) {
        peos_gl_destroy(c);
        g_current_gl = NULL;
        set_error(peos_gl_error());
        return NULL;
    }
    return (SDL_GLContext)c;
}

int SDL_GL_MakeCurrent(SDL_Window *window, SDL_GLContext context) {
    (void)window;
    g_current_gl = (peos_gl_context *)context;
    return peos_gl_make_current((peos_gl_context *)context) == 0 ? 0 : set_error(peos_gl_error());
}

void SDL_GL_DeleteContext(SDL_GLContext context) {
    if (!context) return;
    if ((peos_gl_context *)context == g_current_gl) {
        peos_gl_make_current(NULL);
        g_current_gl = NULL;
    }
    peos_gl_destroy((peos_gl_context *)context);
}

/* With frames.cfg (the debug knob above), the game's OpenGL calls that
 * cost are timed (the TSC: a syscall each would weigh more than they do):
 * draws, vertex uploads, and program, uniform and texture changes. The 10 s
 * line then splits the game's time into those and the rest (its Lua). */
static inline uint64_t tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}
enum { GLP_DRAW, GLP_BUFFER, GLP_STATE, GLP_N };
static uint64_t g_glp_ticks[GLP_N], g_glp_calls[GLP_N];

#define GLP_WRAP(kind, name, params, args)                                  \
    static void(*real_##name) params;                                       \
    static void wrap_##name params {                                        \
        uint64_t t_0 = tsc();                                               \
        real_##name args;                                                   \
        g_glp_ticks[kind] += tsc() - t_0;                                   \
        g_glp_calls[kind]++;                                                \
    }
GLP_WRAP(GLP_DRAW, glDrawArrays, (unsigned m, int f, int c), (m, f, c))
GLP_WRAP(GLP_DRAW, glDrawElements, (unsigned m, int c, unsigned t, const void *i), (m, c, t, i))
GLP_WRAP(GLP_DRAW, glDrawArraysInstanced, (unsigned m, int f, int c, int n), (m, f, c, n))
GLP_WRAP(GLP_DRAW, glDrawElementsInstanced, (unsigned m, int c, unsigned t, const void *i, int n), (m, c, t, i, n))
GLP_WRAP(GLP_DRAW, glDrawElementsBaseVertex, (unsigned m, int c, unsigned t, const void *i, int b), (m, c, t, i, b))
GLP_WRAP(GLP_BUFFER, glBufferData, (unsigned t, intptr_t n, const void *d, unsigned u), (t, n, d, u))
GLP_WRAP(GLP_BUFFER, glBufferSubData, (unsigned t, intptr_t o, intptr_t n, const void *d), (t, o, n, d))
GLP_WRAP(GLP_BUFFER, glFlushMappedBufferRange, (unsigned t, intptr_t o, intptr_t n), (t, o, n))
GLP_WRAP(GLP_STATE, glUseProgram, (unsigned p), (p))
GLP_WRAP(GLP_STATE, glBindTexture, (unsigned t, unsigned x), (t, x))
GLP_WRAP(GLP_STATE, glUniform4fv, (int l, int c, const float *v), (l, c, v))
GLP_WRAP(GLP_STATE, glUniform1iv, (int l, int c, const int *v), (l, c, v))
GLP_WRAP(GLP_STATE, glUniformMatrix4fv, (int l, int c, unsigned char tr, const float *v), (l, c, tr, v))
GLP_WRAP(GLP_STATE, glUniformMatrix3fv, (int l, int c, unsigned char tr, const float *v), (l, c, tr, v))
GLP_WRAP(GLP_STATE, glUniform1fv, (int l, int c, const float *v), (l, c, v))
GLP_WRAP(GLP_STATE, glUniform2fv, (int l, int c, const float *v), (l, c, v))
#undef GLP_WRAP

static void *glp_hook(const char *proc, void *real) {
#define GLP_HOOK(name)                                        \
    if (!strcmp(proc, #name)) {                               \
        real_##name = (__typeof__(real_##name))real;          \
        return (void *)wrap_##name;                           \
    }
    GLP_HOOK(glDrawArrays) GLP_HOOK(glDrawElements) GLP_HOOK(glDrawArraysInstanced)
    GLP_HOOK(glDrawElementsInstanced) GLP_HOOK(glDrawElementsBaseVertex) GLP_HOOK(glBufferData)
    GLP_HOOK(glBufferSubData) GLP_HOOK(glFlushMappedBufferRange) GLP_HOOK(glUseProgram) GLP_HOOK(glBindTexture)
    GLP_HOOK(glUniform4fv) GLP_HOOK(glUniform1iv) GLP_HOOK(glUniformMatrix4fv) GLP_HOOK(glUniformMatrix3fv)
    GLP_HOOK(glUniform1fv) GLP_HOOK(glUniform2fv)
#undef GLP_HOOK
    return real;
}

static void frames_config(void);

void *SDL_GL_GetProcAddress(const char *proc) {
    void *real = (void *)peos_gl_get_proc_address(proc);
    frames_config();
    return real && g_frames_every ? glp_hook(proc, real) : real;
}

void SDL_GL_GetDrawableSize(SDL_Window *window, int *w, int *h) { SDL_GetWindowSize(window, w, h); }

int SDL_GL_SetSwapInterval(int interval) {
    g_interval = interval < 0 ? 1 : interval;   /* adaptive (-1): as 1 */
    return 0;
}

int SDL_GL_GetSwapInterval(void) { return g_interval; }

/* Frames to the log, for looking at a game where the screen cannot be seen
 * (QEMU with virgl: its screendump is black). Only when the file
 * /starlight/config/love/frames.cfg is there (a test puts it): "every=S
 * scale=N", a frame every S seconds, 1/N of its size, as a PPM in base64
 * lines "love-frame <n>: ..." (userland/ports/love/tests/frames_from_log.py
 * makes PNGs of them). Clicks are logged too, in the window's pixels. */
static uint64_t g_frames_next;
static unsigned g_frames_n;

static void frames_config(void) {
    static int read_it;
    if (read_it) return;
    read_it = 1;
    FILE *f = fopen("/starlight/config/love/frames.cfg", "r");
    if (!f) return;
    int every = 10, scale = 4;
    if (fscanf(f, "every=%d scale=%d", &every, &scale) < 1) every = 10;
    fclose(f);
    g_frames_every = every > 0 ? every : 10;
    g_frames_scale = scale > 0 ? scale : 4;
    fprintf(stderr, "love: frames to the log every %d s, 1/%d of their size\n", g_frames_every, g_frames_scale);
}

static void frame_to_log(const uint32_t *px, uint32_t pitch, int w, int h) {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int s = g_frames_scale, ow = w / s, oh = h / s;
    if (ow <= 0 || oh <= 0) return;
    char head[64];
    int hn = snprintf(head, sizeof(head), "P6\n%d %d\n255\n", ow, oh);
    size_t n = (size_t)hn + (size_t)ow * (size_t)oh * 3u;
    unsigned char *ppm = (unsigned char *)malloc(n);
    if (!ppm) return;
    memcpy(ppm, head, (size_t)hn);
    unsigned char *o = ppm + hn;
    for (int y = 0; y < oh; y++) {
        const uint32_t *row = (const uint32_t *)((const unsigned char *)px + (size_t)(y * s) * pitch);
        for (int x = 0; x < ow; x++) {
            uint32_t v = row[x * s];
            *o++ = (unsigned char)(v >> 16);
            *o++ = (unsigned char)(v >> 8);
            *o++ = (unsigned char)v;
        }
    }
    unsigned id = g_frames_n++;
    char line[160];
    size_t i = 0;
    while (i < n) {
        int k = snprintf(line, sizeof(line), "love-frame %u: ", id);
        for (int q = 0; q < 19 && i < n; q++, i += 3) {   /* 57 bytes: 76 characters */
            uint32_t v = (uint32_t)ppm[i] << 16 | (i + 1 < n ? (uint32_t)ppm[i + 1] << 8 : 0) | (i + 2 < n ? ppm[i + 2] : 0);
            line[k++] = b64[(v >> 18) & 63];
            line[k++] = b64[(v >> 12) & 63];
            line[k++] = i + 1 < n ? b64[(v >> 6) & 63] : '=';
            line[k++] = i + 2 < n ? b64[v & 63] : '=';
        }
        line[k++] = '\n';
        fwrite(line, 1, (size_t)k, stderr);
    }
    fprintf(stderr, "love-frame %u end %dx%d at %llu ms\n", id, ow, oh, (unsigned long long)SDL_GetTicks());
    free(ppm);
}

void SDL_GL_SwapWindow(SDL_Window *window) {
    if (!window || !g_current_gl) return;
    static uint64_t frame_end;   /* when the last swap returned: the game's time starts there */
    uint64_t t0 = peos_time_ns();
    uint32_t sw = 0, sh = 0, pitch = 0;
    uint32_t *px = g.pixels_ready ? slui_pixels_buffer(&sw, &sh, &pitch) : NULL;
    uint64_t t1, t2;
    if (px && sw >= (uint32_t)g_gl_w && sh >= (uint32_t)g_gl_h) {
        /* the plain swap: peos_gl_swap_pipelined (a frame later on screen)
         * gave nothing here (2026-11-17: in QEMU the wait is the host's,
         * not the GPU's; docs/game-ports-plan.md, question 8) */
        peos_gl_swap(g_current_gl, px, (int)pitch);
        t1 = peos_time_ns();
        slui_pixels_damage(0, 0, (uint32_t)g_gl_w, (uint32_t)g_gl_h);
        frames_config();
        if (g_frames_every && t1 >= g_frames_next) {
            g_frames_next = t1 + (uint64_t)g_frames_every * 1000000000ull;
            frame_to_log(px, pitch, g_gl_w, g_gl_h);
        }
    } else {
        peos_gl_swap(g_current_gl, NULL, 0);   /* the frame still ends */
        t1 = peos_time_ns();
    }
    t2 = peos_time_ns();
    uint64_t fence = 0, map = 0, copy = 0;
    peos_gl_swap_times(g_current_gl, &fence, &map, &copy);

    /* the swap interval: frames of 1/60 s, so a game asking for 1 does not
     * draw (and burn) faster than that */
    if (g_interval > 0) {
        uint64_t period = 16666667ull * (uint64_t)g_interval, now = peos_time_ns();
        if (g_last_swap && now - g_last_swap + 1000000ull < period)
            peos_sleep((period - (now - g_last_swap)) / 1000000ull);
        g_last_swap = peos_time_ns();
    }
    uint64_t t3 = peos_time_ns();

    /* every 10 s a line in the log (stderr): the frames, the slowest, and
     * where a frame's time went on average: the game (its Lua and its GL
     * calls, from one swap to the next), the swap (waiting for the GPU,
     * mapping the frame, copying it into the window), telling Starlight,
     * and the pacing's sleep */
    static uint64_t st_start, st_frames, st_worst, st_game, st_fence, st_map, st_copy, st_swap, st_damage, st_pace;
    if (!st_start) st_start = t0;
    uint64_t whole = frame_end ? t3 - frame_end : 0;
    if (whole > st_worst) st_worst = whole;
    st_frames++;
    st_game += frame_end ? t0 - frame_end : 0;
    st_swap += t1 - t0;
    st_fence += fence;
    st_map += map;
    st_copy += copy;
    st_damage += t2 - t1;
    st_pace += t3 - t2;
    frame_end = t3;
    if (t3 - st_start >= 10000000000ull) {
        uint64_t f = st_frames ? st_frames : 1;
#define MS(x) ((double)(x) / (double)f / 1e6)
        fprintf(stderr,
                "love: 10 s: %llu frames, slowest %llu ms, window %dx%d; a frame: game %.1f ms, swap %.1f (GPU %.1f, "
                "map %.1f, copy %.1f), damage %.1f, pacing %.1f\n",
                (unsigned long long)st_frames, (unsigned long long)(st_worst / 1000000ull), g_gl_w, g_gl_h, MS(st_game),
                MS(st_swap), MS(st_fence), MS(st_map), MS(st_copy), MS(st_damage), MS(st_pace));
        if (g_frames_every) {   /* the game's OpenGL calls, TSC turned into time over these 10 s */
            static uint64_t tsc_mark, ns_mark;
            uint64_t tn = tsc();
            if (tsc_mark && tn > tsc_mark) {
                double ns_per_tick = (double)(t3 - ns_mark) / (double)(tn - tsc_mark);
#define GMS(k) ((double)g_glp_ticks[k] * ns_per_tick / (double)f / 1e6)
                fprintf(stderr,
                        "love: a frame's OpenGL: draws %.1f ms (%llu), uploads %.1f ms (%llu), state %.1f ms (%llu); "
                        "the rest of the game (Lua) %.1f ms\n",
                        GMS(GLP_DRAW), (unsigned long long)(g_glp_calls[GLP_DRAW] / f), GMS(GLP_BUFFER),
                        (unsigned long long)(g_glp_calls[GLP_BUFFER] / f), GMS(GLP_STATE),
                        (unsigned long long)(g_glp_calls[GLP_STATE] / f),
                        MS(st_game) - GMS(GLP_DRAW) - GMS(GLP_BUFFER) - GMS(GLP_STATE));
#undef GMS
            }
            tsc_mark = tn;
            ns_mark = t3;
            memset(g_glp_ticks, 0, sizeof(g_glp_ticks));
            memset(g_glp_calls, 0, sizeof(g_glp_calls));
        }
#undef MS
        st_start = t3;
        st_frames = st_worst = st_game = st_fence = st_map = st_copy = st_swap = st_damage = st_pace = 0;
    }
}

/* ---- joysticks, game controllers, haptics: none yet on PeOS ---- */

int SDL_NumJoysticks(void) { return 0; }
SDL_Joystick *SDL_JoystickOpen(int index) {
    (void)index;
    set_error("no joysticks on PeOS yet");
    return NULL;
}
void SDL_JoystickClose(SDL_Joystick *j) { (void)j; }
int SDL_JoystickEventState(int state) { return state == SDL_QUERY ? SDL_IGNORE : state; }
SDL_bool SDL_JoystickGetAttached(SDL_Joystick *j) { (void)j; return SDL_FALSE; }
Sint16 SDL_JoystickGetAxis(SDL_Joystick *j, int axis) { (void)j; (void)axis; return 0; }
Uint8 SDL_JoystickGetButton(SDL_Joystick *j, int button) { (void)j; (void)button; return 0; }
Uint8 SDL_JoystickGetHat(SDL_Joystick *j, int hat) { (void)j; (void)hat; return SDL_HAT_CENTERED; }
SDL_JoystickGUID SDL_JoystickGetDeviceGUID(int index) { SDL_JoystickGUID g0; (void)index; memset(&g0, 0, sizeof(g0)); return g0; }
SDL_JoystickGUID SDL_JoystickGetGUID(SDL_Joystick *j) { (void)j; return SDL_JoystickGetDeviceGUID(0); }
SDL_JoystickGUID SDL_JoystickGetGUIDFromString(const char *s) { (void)s; return SDL_JoystickGetDeviceGUID(0); }
void SDL_JoystickGetGUIDString(SDL_JoystickGUID guid, char *out, int size) {
    (void)guid;
    if (out && size > 0) snprintf(out, (size_t)size, "00000000000000000000000000000000");
}
Uint16 SDL_JoystickGetProduct(SDL_Joystick *j) { (void)j; return 0; }
Uint16 SDL_JoystickGetProductVersion(SDL_Joystick *j) { (void)j; return 0; }
Uint16 SDL_JoystickGetVendor(SDL_Joystick *j) { (void)j; return 0; }
SDL_bool SDL_JoystickHasRumble(SDL_Joystick *j) { (void)j; return SDL_FALSE; }
SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick *j) { (void)j; return -1; }
const char *SDL_JoystickName(SDL_Joystick *j) { (void)j; return NULL; }
int SDL_JoystickNumAxes(SDL_Joystick *j) { (void)j; return 0; }
int SDL_JoystickNumButtons(SDL_Joystick *j) { (void)j; return 0; }
int SDL_JoystickNumHats(SDL_Joystick *j) { (void)j; return 0; }
int SDL_JoystickRumble(SDL_Joystick *j, Uint16 lo, Uint16 hi, Uint32 ms) { (void)j; (void)lo; (void)hi; (void)ms; return -1; }

SDL_bool SDL_IsGameController(int index) { (void)index; return SDL_FALSE; }
SDL_GameController *SDL_GameControllerOpen(int index) { (void)index; return NULL; }
void SDL_GameControllerClose(SDL_GameController *c) { (void)c; }
int SDL_GameControllerAddMapping(const char *mapping) { (void)mapping; return 0; }
int SDL_GameControllerEventState(int state) { return state == SDL_QUERY ? SDL_IGNORE : state; }
Sint16 SDL_GameControllerGetAxis(SDL_GameController *c, SDL_GameControllerAxis a) { (void)c; (void)a; return 0; }
Uint8 SDL_GameControllerGetButton(SDL_GameController *c, SDL_GameControllerButton b) { (void)c; (void)b; return 0; }
SDL_GameControllerButtonBind SDL_GameControllerGetBindForAxis(SDL_GameController *c, SDL_GameControllerAxis a) {
    SDL_GameControllerButtonBind b;
    (void)c;
    (void)a;
    memset(&b, 0, sizeof(b));
    return b;
}
SDL_GameControllerButtonBind SDL_GameControllerGetBindForButton(SDL_GameController *c, SDL_GameControllerButton bt) {
    SDL_GameControllerButtonBind b;
    (void)c;
    (void)bt;
    memset(&b, 0, sizeof(b));
    return b;
}
SDL_Joystick *SDL_GameControllerGetJoystick(SDL_GameController *c) { (void)c; return NULL; }
const char *SDL_GameControllerGetStringForAxis(SDL_GameControllerAxis a) { (void)a; return NULL; }
const char *SDL_GameControllerGetStringForButton(SDL_GameControllerButton b) { (void)b; return NULL; }
char *SDL_GameControllerMapping(SDL_GameController *c) { (void)c; return NULL; }
char *SDL_GameControllerMappingForGUID(SDL_JoystickGUID guid) { (void)guid; return NULL; }
const char *SDL_GameControllerName(SDL_GameController *c) { (void)c; return NULL; }

int SDL_HapticIndex(SDL_Haptic *h) { (void)h; return -1; }
SDL_Haptic *SDL_HapticOpenFromJoystick(SDL_Joystick *j) { (void)j; return NULL; }
void SDL_HapticClose(SDL_Haptic *h) { (void)h; }
unsigned int SDL_HapticQuery(SDL_Haptic *h) { (void)h; return 0; }
int SDL_HapticNumAxes(SDL_Haptic *h) { (void)h; return -1; }
int SDL_HapticNewEffect(SDL_Haptic *h, SDL_HapticEffect *e) { (void)h; (void)e; return -1; }
int SDL_HapticUpdateEffect(SDL_Haptic *h, int effect, SDL_HapticEffect *data) { (void)h; (void)effect; (void)data; return -1; }
int SDL_HapticRunEffect(SDL_Haptic *h, int effect, Uint32 iterations) { (void)h; (void)effect; (void)iterations; return -1; }
int SDL_HapticStopEffect(SDL_Haptic *h, int effect) { (void)h; (void)effect; return -1; }
void SDL_HapticDestroyEffect(SDL_Haptic *h, int effect) { (void)h; (void)effect; }
int SDL_HapticGetEffectStatus(SDL_Haptic *h, int effect) { (void)h; (void)effect; return -1; }
