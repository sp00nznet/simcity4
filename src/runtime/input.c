/*
 * Scripted input, kotor's way: clicks and keys are posted to the game's own
 * window as messages, so the player's real mouse and keyboard are never
 * touched and a run works headless. The clock starts when the game's main
 * loop does (framework phase 7, input_main_loop()).
 *
 *   --click X,Y@S      left click at client X,Y, S seconds into the main loop
 *   --rclick X,Y@S     the same with the right button
 *   --key NAME@S       a key: a letter or digit, or ESC, ENTER, SPACE, TAB, F1..F12;
 *                      CTRL+NAME holds Control with it (CTRL+S saves the city)
 *
 * @Nf instead of @S waits for frame N: frames stop while the game loads and
 * slow down with it on a busy machine, so a frame-timed script stays in step.
 * Events run in the order given.
 *
 * SC4 also asks GetCursorPos where the mouse is, so while a scripted pointer
 * is set, the GetCursorPos shim answers with it (in screen coordinates).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input.h"

#define MAX_EVENTS 64
typedef struct { DWORD at_ms; LONG at_frame; int kind; int x, y; UINT vk, ctrl; } ev_t;   /* at_frame 0: timed by at_ms */
extern volatile LONG g_frames;   /* kind: 1 click, 2 rclick, 3 key */
static ev_t g_ev[MAX_EVENTS];
static int g_nev;
static volatile HWND g_hwnd;
static volatile LONG g_loop_tick;     /* GetTickCount when the main loop began; 0 before */
static volatile LONG g_have_ptr;
static volatile LONG g_ptr_x, g_ptr_y;
static volatile LONG g_down_vk;      /* the scripted button or key held down now, 0 if none */
static volatile LONG g_ctrl;         /* scripted Control held */

static UINT key_vk(const char* n) {
    static const struct { const char* name; UINT vk; } named[] = {
        { "ESC", VK_ESCAPE }, { "ENTER", VK_RETURN }, { "SPACE", VK_SPACE }, { "TAB", VK_TAB },
        { "LEFT", VK_LEFT }, { "RIGHT", VK_RIGHT }, { "UP", VK_UP }, { "DOWN", VK_DOWN },
    };
    for (size_t i = 0; i < sizeof named / sizeof named[0]; i++)
        if (!_stricmp(n, named[i].name)) return named[i].vk;
    if ((n[0] == 'F' || n[0] == 'f') && n[1] >= '1' && n[1] <= '9') {
        int f = atoi(n + 1);
        if (f >= 1 && f <= 12) return VK_F1 + f - 1;
    }
    if (n[0] && !n[1] && ((n[0] >= 'A' && n[0] <= 'Z') || (n[0] >= '0' && n[0] <= '9'))) return (UINT)n[0];
    if (n[0] && !n[1] && n[0] >= 'a' && n[0] <= 'z') return (UINT)(n[0] - 32);
    return 0;
}

/* "X,Y@S" or "NAME@S" -> event; 0 when malformed. */
static int parse(const char* opt, const char* v, ev_t* e) {
    const char* at = strrchr(v, '@');
    if (!at) return 0;
    char* end;
    double s = strtod(at + 1, &end);
    if (s < 0) return 0;
    if (*end == 'f') e->at_frame = (LONG)s;
    else e->at_ms = (DWORD)(s * 1000.0);
    if (!strcmp(opt, "--key")) {
        char name[16];
        size_t n = (size_t)(at - v);
        if (!n || n >= sizeof name) return 0;
        memcpy(name, v, n);
        name[n] = 0;
        e->kind = 3;
        e->ctrl = !_strnicmp(name, "CTRL+", 5);
        e->vk = key_vk(name + (e->ctrl ? 5 : 0));
        return e->vk != 0;
    }
    e->kind = !strcmp(opt, "--rclick") ? 2 : 1;
    return sscanf(v, "%d,%d", &e->x, &e->y) == 2;
}

int input_arg(int argc, char** argv, int i) {
    const char* o = argv[i];
    if (strcmp(o, "--click") && strcmp(o, "--rclick") && strcmp(o, "--key")) return 0;
    if (i + 1 >= argc || g_nev == MAX_EVENTS || !parse(o, argv[i + 1], &g_ev[g_nev])) {
        fprintf(stderr, "bad %s %s (X,Y@S or NAME@S)\n", o, i + 1 < argc ? argv[i + 1] : "");
        exit(1);
    }
    g_nev++;
    return 2;
}

void input_help(void) {
    printf("           [--click X,Y@S] [--rclick X,Y@S] [--key NAME@S]   scripted input, S seconds into the main loop\n");
}

void input_on_window(void* hwnd) { g_hwnd = (HWND)hwnd; }

void input_main_loop(void) { InterlockedCompareExchange(&g_loop_tick, (LONG)GetTickCount() | 1, 0); }

int input_cursor(long* x, long* y) {
    if (!g_have_ptr || !g_hwnd) return 0;
    POINT p = { g_ptr_x, g_ptr_y };
    ClientToScreen(g_hwnd, &p);
    *x = p.x, *y = p.y;
    return 1;
}

/* GetAsyncKeyState/GetKeyState: the scripted button or key reads as down while
 * it is held; anything else is the real state (-1: ask Windows). */
int input_key_state(int vk) {
    LONG d = g_down_vk;
    if (g_ctrl && (vk == VK_CONTROL || vk == VK_LCONTROL)) return 0x8000;
    if (!d && !g_have_ptr) return -1;
    if (vk == (int)d) return 0x8000;
    if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) return 0;   /* the real mouse is not ours */
    return -1;
}

static void click(int x, int y, int right) {
    HWND h = g_hwnd;
    LPARAM lp = MAKELPARAM(x, y);
    InterlockedExchange(&g_ptr_x, x);
    InterlockedExchange(&g_ptr_y, y);
    InterlockedExchange(&g_have_ptr, 1);
    /* A hover first, a second of it: the region view picks the city under the
     * pointer in its per-frame update, not in the click handler. */
    for (int i = 0; i < 10; i++) { PostMessageA(h, WM_MOUSEMOVE, 0, lp); Sleep(100); }
    InterlockedExchange(&g_down_vk, right ? VK_RBUTTON : VK_LBUTTON);
    PostMessageA(h, right ? WM_RBUTTONDOWN : WM_LBUTTONDOWN, right ? MK_RBUTTON : MK_LBUTTON, lp);
    Sleep(150);
    InterlockedExchange(&g_down_vk, 0);
    PostMessageA(h, right ? WM_RBUTTONUP : WM_LBUTTONUP, 0, lp);
}

static void key(UINT vk, int ctrl) {
    HWND h = g_hwnd;
    UINT sc = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC), csc = MapVirtualKeyA(VK_CONTROL, MAPVK_VK_TO_VSC);
    if (ctrl) { InterlockedExchange(&g_ctrl, 1); PostMessageA(h, WM_KEYDOWN, VK_CONTROL, 1 | (csc << 16)); Sleep(50); }
    InterlockedExchange(&g_down_vk, (LONG)vk);
    PostMessageA(h, WM_KEYDOWN, vk, 1 | (sc << 16));
    if (ctrl && vk >= 'A' && vk <= 'Z') PostMessageA(h, WM_CHAR, vk - 'A' + 1, 1 | (sc << 16));
    else if (vk >= '0' && vk <= 'Z') PostMessageA(h, WM_CHAR, vk >= 'A' ? vk + 32 : vk, 1 | (sc << 16));
    Sleep(150);
    InterlockedExchange(&g_down_vk, 0);
    PostMessageA(h, WM_KEYUP, vk, 1 | (sc << 16) | (1u << 30) | (1u << 31));
    if (ctrl) {
        Sleep(50);
        PostMessageA(h, WM_KEYUP, VK_CONTROL, 1 | (csc << 16) | (1u << 30) | (1u << 31));
        InterlockedExchange(&g_ctrl, 0);
    }
}

static DWORD WINAPI script(LPVOID unused) {
    (void)unused;
    while (!g_loop_tick || !g_hwnd) Sleep(20);
    DWORD t0 = (DWORD)g_loop_tick;
    for (int k = 0; k < g_nev; k++) {
        ev_t e = g_ev[k];
        if (e.at_frame) while (g_frames < e.at_frame) Sleep(10);
        else for (DWORD now; (int)((now = GetTickCount()) - (t0 + e.at_ms)) < 0; ) Sleep(10);
        double s = (GetTickCount() - t0) / 1000.0;
        if (e.kind == 3) printf("[input] key %s0x%02X at %.1f s, frame %ld\n", e.ctrl ? "CTRL+" : "", e.vk, s, g_frames);
        else printf("[input] %s %d,%d at %.1f s, frame %ld\n", e.kind == 2 ? "rclick" : "click", e.x, e.y, s, g_frames);
        fflush(stdout);
        if (e.kind == 3) key(e.vk, e.ctrl);
        else click(e.x, e.y, e.kind == 2);
    }
    return 0;
}

void input_start(void) {
    if (g_nev) CloseHandle(CreateThread(NULL, 0, script, NULL, 0, NULL));
}
