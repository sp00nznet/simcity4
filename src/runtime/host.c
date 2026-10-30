/*
 * SimCity 4 Deluxe - static recompilation host.
 *
 * A 32-bit host on pcrecomp's runtime/native32 (the native bridge, callbacks
 * and machine lock; see its header). Every import goes to the real DLL --
 * OpenGL, DirectDraw/Direct3D 7 (loaded by the game at run time), DirectSound,
 * WinINet -- so what is here is only what is specific to this game: where the
 * image goes, the command line, the calls that would reach outside the game's
 * own window, muting, and the fault report. Adapted from kotor's host.
 *
 * Linked at /BASE:0x60000000 (CMakeLists.txt) so the guest's
 * 0x00400000..0x00BC8000 is free when main() maps the image.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native32.h"
#include "recomp_trace.h"
#include "oracle.h"
#include "steamreg.h"
#include "input.h"

extern const uint32_t sc4_entry_va;  /* recomp_dispatch.c */

#define SC4_IMAGE_BASE 0x00400000u
#define ARG(n) MEM32(g_esp + 4 + 4 * (n))

static DWORD g_watchdog_s;
static int   g_modeswitch;   /* --modeswitch: let ChangeDisplaySettingsA through */
static int   g_gamma;        /* --gamma: let SetDeviceGammaRamp through */
static int   g_headless;
static int   g_original;   /* --original: the shipping machine code (oracle.c) */

/* ---- calls that reach outside the window -----------------------------------
 * ChangeDisplaySettingsA resizes the user's real screen and SetDeviceGammaRamp
 * outlives a crash; both are answered "done" unless asked for. */
static void shim_ChangeDisplaySettingsA(void) {
    DEVMODEA* dm = (DEVMODEA*)(uintptr_t)ARG(0);
    if (g_modeswitch) {
        g_eax = (uint32_t)ChangeDisplaySettingsA(dm, ARG(1));
    } else {
        if (dm)
            fprintf(stderr, "[host] ChangeDisplaySettingsA %lux%lu %lubpp: skipped (--modeswitch)\n",
                    dm->dmPelsWidth, dm->dmPelsHeight, dm->dmBitsPerPel);
        g_eax = DISP_CHANGE_SUCCESSFUL;
    }
    g_esp += 4 + 2 * 4;
}

static void shim_SetDeviceGammaRamp(void) {
    g_eax = g_gamma ? SetDeviceGammaRamp((HDC)(uintptr_t)ARG(0), (LPVOID)(uintptr_t)ARG(1)) : TRUE;
    g_esp += 4 + 2 * 4;
}

/* ---- the process is the host, the module is the guest ----------------------
 * GetModuleHandleA(NULL) and GetModuleFileNameA(NULL) would name the host; SC4
 * finds its data one folder up from its own exe (Apps\..). */
static char g_guest_exe[MAX_PATH], g_guest_cmdline[1024];

static void shim_GetModuleHandleA(void) {
    g_eax = ARG(0) ? (uint32_t)(uintptr_t)GetModuleHandleA((LPCSTR)(uintptr_t)ARG(0))
                   : SC4_IMAGE_BASE;
    g_esp += 4 + 1 * 4;
}

static void shim_GetModuleFileNameA(void) {
    uint32_t h = ARG(0), size = ARG(2);
    char* out = (char*)(uintptr_t)ARG(1);
    if (h == 0 || h == SC4_IMAGE_BASE) {
        uint32_t n = (uint32_t)strlen(g_guest_exe);
        if (size) {
            uint32_t k = n < size ? n : size - 1;
            memcpy(out, g_guest_exe, k);
            out[k] = 0;
            n = k;
        }
        g_eax = n;
    } else {
        g_eax = GetModuleFileNameA((HMODULE)(uintptr_t)h, out, size);
    }
    g_esp += 4 + 3 * 4;
}

/* SC4 also fetches GetModuleFileNameW through GetProcAddress (0x00912467) and
 * builds its data paths from it: unanswered, it looked for Graphics Rules.sgr
 * beside the host and threw nSCRes::cParseError when it was not there. So
 * GetProcAddress hands out this stand-in for that one name. */
static DWORD WINAPI guest_GetModuleFileNameW(HMODULE h, LPWSTR out, DWORD size) {
    if (h && (uint32_t)(uintptr_t)h != SC4_IMAGE_BASE) return GetModuleFileNameW(h, out, size);
    if (!size) return 0;
    int n = MultiByteToWideChar(CP_ACP, 0, g_guest_exe, -1, out, (int)size);
    if (!n) { out[size - 1] = 0; SetLastError(ERROR_INSUFFICIENT_BUFFER); return size; }
    return (DWORD)n - 1;
}

/* Documents: the game keeps sc4.cfg, its Regions and Plugins in Documents\
 * SimCity 4. Every run gets work\docs instead (--user-docs for the real one),
 * so a test never writes into the player's cities: the first visible run, before
 * this, copied the stock regions into the real Documents. It asks two ways: the
 * SHGetFolderPathW it fetches through GetProcAddress, and
 * SHGetSpecialFolderLocation + SHGetPathFromIDListA (imported). */
#define CSIDL_PERSONAL_ 0x0005
static char g_private_docs[MAX_PATH];   /* empty: the real Documents */
static char g_real_docs[MAX_PATH];

static HRESULT WINAPI guest_SHGetFolderPathW(HWND w, int csidl, HANDLE tok, DWORD flags, LPWSTR out) {
    typedef HRESULT (WINAPI *fn_t)(HWND, int, HANDLE, DWORD, LPWSTR);
    static fn_t real;
    if (!real) real = (fn_t)GetProcAddress(LoadLibraryA("shell32.dll"), "SHGetFolderPathW");
    if (g_private_docs[0] && (csidl & 0xFF) == CSIDL_PERSONAL_) {
        MultiByteToWideChar(CP_ACP, 0, g_private_docs, -1, out, MAX_PATH);
        return S_OK;
    }
    return real(w, csidl, tok, flags, out);
}

static void shim_SHGetPathFromIDListA(void) {
    typedef BOOL (WINAPI *fn_t)(const void*, LPSTR);
    static fn_t real;
    if (!real) real = (fn_t)GetProcAddress(LoadLibraryA("shell32.dll"), "SHGetPathFromIDListA");
    char* out = (char*)(uintptr_t)ARG(1);
    g_eax = real((const void*)(uintptr_t)ARG(0), out);
    if (g_eax && g_private_docs[0] && !_stricmp(out, g_real_docs)) strcpy(out, g_private_docs);
    g_esp += 4 + 2 * 4;
}

static void shim_GetProcAddress(void) {
    HMODULE m = (HMODULE)(uintptr_t)ARG(0);
    const char* name = (const char*)(uintptr_t)ARG(1);
    if (HIWORD((uintptr_t)name) && !strcmp(name, "GetModuleFileNameW"))
        g_eax = (uint32_t)(uintptr_t)guest_GetModuleFileNameW;
    else if (HIWORD((uintptr_t)name) && !strcmp(name, "SHGetFolderPathW"))
        g_eax = (uint32_t)(uintptr_t)guest_SHGetFolderPathW;
    else
        g_eax = (uint32_t)(uintptr_t)GetProcAddress(m, name);
    g_esp += 4 + 2 * 4;
}

static void shim_GetCommandLineA(void) {
    g_eax = (uint32_t)(uintptr_t)g_guest_cmdline;
    g_esp += 4;
}

/* ---- windows ------------------------------------------------------------------
 * A shim runs holding the machine, and a window call can wait on another
 * thread; the call runs with the machine let go, as native32's bridge does for
 * every unshimmed import (kotor's startup deadlock). */
#define UNLOCKED(call) do { uint32_t esp_ = g_esp; \
    if (g_original) oracle_release(); else mach_leave(); \
    call; DWORD err_ = GetLastError(); \
    if (g_original) oracle_take(); else mach_enter(); \
    g_esp = esp_; SetLastError(err_); } while (0)

/* --monitor N: the game's top-level windows go on display N (\.\DISPLAYN)
 * instead of the primary one. Positions the game gives relative to the
 * primary screen are shifted by that monitor's origin; ones already on it,
 * CW_USEDEFAULT and the minimized -32000 are left alone. ponytail: the window
 * keeps the game's size; if it is bigger than the monitor, it hangs over. */
static int g_mon_on;
static RECT g_mon;     /* that monitor's work area */

static BOOL CALLBACK find_monitor(HMONITOR m, HDC dc, LPRECT r, LPARAM want) {
    (void)dc; (void)r;
    MONITORINFOEXA mi;
    mi.cbSize = sizeof mi;
    char name[32];
    _snprintf(name, sizeof name - 1, "\\\\.\\DISPLAY%d", (int)want);
    name[sizeof name - 1] = 0;
    if (GetMonitorInfoA(m, (MONITORINFO*)&mi) && !_stricmp(mi.szDevice, name)) {
        g_mon = mi.rcWork;
        g_mon_on = 1;
        return FALSE;
    }
    return TRUE;
}

static void place(uint32_t style, uint32_t* x, uint32_t* y) {
    if (!g_mon_on || (style & WS_CHILD) || *x == (uint32_t)CW_USEDEFAULT) return;
    int ix = (int)*x, iy = (int)*y;
    if (ix <= -10000 || (ix >= g_mon.left && ix < g_mon.right && iy >= g_mon.top && iy < g_mon.bottom)) return;
    *x = (uint32_t)(ix + g_mon.left);
    *y = (uint32_t)(iy + g_mon.top);
}

static int is_top_level(HWND w) {
    return w && !(GetWindowLongA(w, GWL_STYLE) & WS_CHILD);
}

static void shim_MoveWindow(void) {
    uint32_t a[6];
    for (int i = 0; i < 6; i++) a[i] = ARG(i);
    if (is_top_level((HWND)(uintptr_t)a[0])) place(0, &a[1], &a[2]);
    BOOL r;
    UNLOCKED(r = MoveWindow((HWND)(uintptr_t)a[0], (int)a[1], (int)a[2], (int)a[3], (int)a[4], (BOOL)a[5]));
    g_eax = r;
    g_esp += 4 + 6 * 4;
}

static void shim_CreateWindowExA(void) {
    uint32_t a[12];
    for (int i = 0; i < 12; i++) a[i] = ARG(i);
    if (g_headless) a[3] &= ~WS_VISIBLE;
    place(a[3], &a[4], &a[5]);
    HWND h;
    UNLOCKED(h = CreateWindowExA(a[0], (LPCSTR)(uintptr_t)a[1], (LPCSTR)(uintptr_t)a[2], a[3],
                                 a[4], a[5], a[6], a[7], (HWND)(uintptr_t)a[8],
                                 (HMENU)(uintptr_t)a[9], (HINSTANCE)(uintptr_t)a[10], (LPVOID)(uintptr_t)a[11]));
    DWORD err = h ? 0 : GetLastError();
    printf("[host] CreateWindowExA(\"%s\", %dx%d) -> %p (error %lu)\n",
           a[2] && HIWORD(a[2]) ? (const char*)(uintptr_t)a[2] : "", (int)a[6], (int)a[7], (void*)h, err);
    if (h && a[2] && HIWORD(a[2]) && !strncmp((const char*)(uintptr_t)a[2], "GDriverWindow", 13)) {
        input_on_window(h);                   /* where scripted input goes */
    }
    SetLastError(err);
    g_eax = (uint32_t)(uintptr_t)h;
    g_esp += 4 + 12 * 4;
}

static void shim_GetCursorPos(void) {
    POINT* p = (POINT*)(uintptr_t)ARG(0);
    long x, y;
    if (p && input_cursor(&x, &y)) { p->x = x; p->y = y; g_eax = TRUE; }
    else g_eax = GetCursorPos(p);
    g_esp += 4 + 1 * 4;
}

static void shim_GetAsyncKeyState(void) {
    int vk = (int)ARG(0), st = input_key_state(vk);
    g_eax = st >= 0 ? (uint32_t)st : (uint32_t)(uint16_t)GetAsyncKeyState(vk);
    g_esp += 4 + 1 * 4;
}

static void shim_GetKeyState(void) {
    int vk = (int)ARG(0), st = input_key_state(vk);
    g_eax = st >= 0 ? (uint32_t)(st ? 0xFF80 : 0) : (uint32_t)(uint16_t)GetKeyState(vk);
    g_esp += 4 + 1 * 4;
}

static void shim_ShowWindow(void) {
    HWND w = (HWND)(uintptr_t)ARG(0);
    int cmd = (int)ARG(1);
    g_eax = 0;
    if (!g_headless) UNLOCKED(g_eax = ShowWindow(w, cmd));
    g_esp += 4 + 2 * 4;
}

/* Headless, GetDC(NULL) must not hand out the real screen (kotor painted the
 * desktop black through it), but SC4 reads the screen's depth from it
 * (0x0098ADE0): 0 there means no video mode and a null view at 0x0098E9A6.
 * A memory DC compatible with the screen answers GetDeviceCaps the same and
 * draws nowhere. ponytail: never deleted; it is made a few times at startup. */
static void shim_GetDC(void) {
    HWND w = (HWND)(uintptr_t)ARG(0);
    g_eax = (g_headless && !w) ? (uint32_t)(uintptr_t)CreateCompatibleDC(NULL)
                               : (uint32_t)(uintptr_t)GetDC(w);
    g_esp += 4 + 1 * 4;
}

static void shim_SetWindowPos(void) {
    uint32_t a[7];
    for (int i = 0; i < 7; i++) a[i] = ARG(i);
    if (g_headless) a[6] = (a[6] | SWP_NOACTIVATE | SWP_NOZORDER) & ~SWP_SHOWWINDOW;
    if (!(a[6] & SWP_NOMOVE) && is_top_level((HWND)(uintptr_t)a[0])) place(0, &a[2], &a[3]);
    BOOL r;
    UNLOCKED(r = SetWindowPos((HWND)(uintptr_t)a[0], (HWND)(uintptr_t)a[1], (int)a[2], (int)a[3],
                              (int)a[4], (int)a[5], a[6]));
    g_eax = r;
    g_esp += 4 + 7 * 4;
}

static void shim_MessageBoxA(void) {
    const char* text = ARG(1) ? (const char*)(uintptr_t)ARG(1) : "";
    const char* cap = ARG(2) ? (const char*)(uintptr_t)ARG(2) : "";
    printf("[host] MessageBoxA \"%s\": %s\n", cap, text);
    if (g_headless) {
        uint32_t b = ARG(3) & MB_TYPEMASK;
        g_eax = (b == MB_YESNO || b == MB_YESNOCANCEL) ? IDNO : IDOK;
    } else {
        HWND w = (HWND)(uintptr_t)ARG(0);
        UINT type = ARG(3);
        int r;
        UNLOCKED(r = MessageBoxA(w, text, cap, type));
        g_eax = r;
    }
    g_esp += 4 + 4 * 4;
}

/* ---- milestones ------------------------------------------------------------- */
volatile LONG g_frames;   /* frames presented; input.c times @Nf events by it */

static const char* g_snap_dir;   /* --snap: defined with the software driver's below */
static LONG g_snap_every;

/* --snap under OpenGL: the back buffer just before the swap, read with
 * glReadPixels (kotor's recorder does the same), written bottom-up as BMP. */
static void snap_gl(HWND w, LONG n) {
    typedef void (WINAPI *read_pixels_t)(int, int, int, int, unsigned, unsigned, void*);
    typedef void (WINAPI *read_buffer_t)(unsigned);
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    read_pixels_t rp = gl ? (read_pixels_t)GetProcAddress(gl, "glReadPixels") : NULL;
    read_buffer_t rb = gl ? (read_buffer_t)GetProcAddress(gl, "glReadBuffer") : NULL;
    RECT rc;
    if (!rp || !GetClientRect(w, &rc) || rc.right <= 0 || rc.bottom <= 0) return;
    int cw = rc.right, ch = rc.bottom;
    DWORD size = (DWORD)cw * ch * 4;
    uint8_t* px = (uint8_t*)malloc(size);
    if (!px) return;
    if (rb) rb(0x0405 /* GL_BACK */);
    rp(0, 0, cw, ch, 0x80E1 /* GL_BGRA */, 0x1401 /* GL_UNSIGNED_BYTE */, px);
    char path[MAX_PATH];
    _snprintf(path, sizeof path - 1, "%s\\frame_%05ld.bmp", g_snap_dir, n);
    path[sizeof path - 1] = 0;
    FILE* f = fopen(path, "wb");
    if (f) {
        BITMAPINFOHEADER bi = { sizeof bi, cw, ch, 1, 32, BI_RGB };
        BITMAPFILEHEADER fh = { 0x4D42, sizeof fh + sizeof bi + size, 0, 0, sizeof fh + sizeof bi };
        fwrite(&fh, sizeof fh, 1, f);
        fwrite(&bi, sizeof bi, 1, f);
        fwrite(px, 1, size, f);
        fclose(f);
        printf("[host] frame %ld -> %s (%dx%d, OpenGL)\n", n, path, cw, ch);
    }
    free(px);
}

static void gl_frame_end(void);

static void shim_SwapBuffers(void) {
    gl_frame_end();
    LONG n = InterlockedIncrement(&g_frames);
    if (n == 1 || n == 60 || n % 1000 == 0) printf("[host] frame %ld presented (OpenGL)\n", n);
    HDC dc = (HDC)(uintptr_t)ARG(0);
    HWND w = WindowFromDC(dc);
    if (g_headless && w && IsWindowVisible(w)) ShowWindow(w, SW_HIDE);
    if (g_snap_dir && w && (n == 1 || n % g_snap_every == 0)) snap_gl(w, n);
    g_eax = SwapBuffers(dc);
    g_esp += 4 + 1 * 4;
}

/* The software driver presents with BitBlt from its DIB section to the
 * window, in 16-row strips (0x008F3640): a strip at y = 0 starts a frame.
 * --snap DIR writes the whole source surface as DIR\frame_NNNNN.bmp every
 * --snap-every frames (default 100), so a headless run has real screenshots.
 * ponytail: BMP, converted outside (ffmpeg); a --record pipe like kotor's when
 * video is wanted. */
static const char* g_snap_dir;
static LONG g_snap_every = 100;

static void snap_surface(HDC src, LONG n) {
    HBITMAP bm = (HBITMAP)GetCurrentObject(src, OBJ_BITMAP);
    DIBSECTION ds;
    if (!bm || GetObjectA(bm, sizeof ds, &ds) != sizeof ds || !ds.dsBm.bmBits) return;
    int w = ds.dsBm.bmWidth, h = ds.dsBm.bmHeight, bpp = ds.dsBm.bmBitsPixel;
    if (bpp != 32 && bpp != 24 && bpp != 16) return;
    char path[MAX_PATH];
    _snprintf(path, sizeof path - 1, "%s\\frame_%05ld.bmp", g_snap_dir, n);
    path[sizeof path - 1] = 0;
    FILE* f = fopen(path, "wb");
    if (!f) return;
    /* Written as 32-bit top-down whatever the source is: GetDIBits converts. */
    BITMAPINFOHEADER bi = { sizeof bi, w, -abs(h), 1, 32, BI_RGB };
    DWORD size = (DWORD)w * abs(h) * 4;
    uint8_t* px = (uint8_t*)malloc(size);
    HDC dc = CreateCompatibleDC(NULL);
    if (px && GetDIBits(dc, bm, 0, abs(h), px, (BITMAPINFO*)&bi, DIB_RGB_COLORS)) {
        BITMAPFILEHEADER fh = { 0x4D42, sizeof fh + sizeof bi + size, 0, 0, sizeof fh + sizeof bi };
        fwrite(&fh, sizeof fh, 1, f);
        fwrite(&bi, sizeof bi, 1, f);
        fwrite(px, 1, size, f);
        printf("[host] frame %ld -> %s (%dx%d, %d bpp source)\n", n, path, w, abs(h), bpp);
    }
    DeleteDC(dc);
    free(px);
    fclose(f);
}

static void shim_BitBlt(void) {
    uint32_t a[9];
    for (int i = 0; i < 9; i++) a[i] = ARG(i);
    if (a[2] == 0) {                           /* y = 0: a new frame */
        LONG n = InterlockedIncrement(&g_frames);
        if (n == 1 || n == 60 || n % 1000 == 0) printf("[host] frame %ld presented (software)\n", n);
        if (g_snap_dir && (n == 1 || n % g_snap_every == 0)) snap_surface((HDC)(uintptr_t)a[5], n);
    }
    g_eax = BitBlt((HDC)(uintptr_t)a[0], (int)a[1], (int)a[2], (int)a[3], (int)a[4],
                   (HDC)(uintptr_t)a[5], (int)a[6], (int)a[7], a[8]);
    g_esp += 4 + 9 * 4;
}

/* --gl-hide A,B: glGetString(GL_EXTENSIONS) without those names, to steer the
 * OpenGL driver off a path a modern driver renders wrong. */
static char g_gl_hide[512];
static char g_gl_only[512];   /* --gl-only A,B: just these, the ones the driver has */

static void shim_glGetString(void) {
    typedef const char* (WINAPI *gs_t)(unsigned);
    static gs_t real;
    static char filtered[1 << 16];
    if (!real) real = (gs_t)GetProcAddress(GetModuleHandleA("opengl32.dll"), "glGetString");
    unsigned name = ARG(0);
    const char* s = real(name);
    if (name == 0x1F03 /* GL_EXTENSIONS */ && s && (g_gl_hide[0] || g_gl_only[0])) {
        size_t o = 0;
        const char* p = s;
        while (*p) {
            while (*p == ' ') p++;
            const char* e = p;
            while (*e && *e != ' ') e++;
            size_t n = (size_t)(e - p);
            char tok[128];
            int hide = 0;
            if (n && n < sizeof tok) {
                memcpy(tok, p, n);
                tok[n] = 0;
                const char* h = strstr(g_gl_hide, tok);
                hide = h && (h == g_gl_hide || h[-1] == ',') && (h[n] == ',' || !h[n]);
                if (g_gl_only[0]) {
                    const char* k = strstr(g_gl_only, tok);
                    hide |= !(k && (k == g_gl_only || k[-1] == ',') && (k[n] == ',' || !k[n]));
                }
            }
            if (n && !hide && o + n + 2 < sizeof filtered) { memcpy(filtered + o, p, n); o += n; filtered[o++] = ' '; }
            p = e;
        }
        filtered[o] = 0;
        static LONG once;
        if (!InterlockedExchange(&once, 1))
            printf("[host] GL_EXTENSIONS: %zu bytes, %zu after hiding %s\n", strlen(s), o, g_gl_hide);
        s = filtered;
    }
    g_eax = (uint32_t)(uintptr_t)s;
    g_esp += 4 + 1 * 4;
}

/* --gltrace: each distinct texture/state GL call once, with the frame it first
 * happened in, to see how the city view sets up terrain texturing.
 * ponytail: linear dedup over 4096 lines; enough for one city load. */
static int g_gltrace;
static long g_gltrace_frame = -1;   /* --gltrace-frame N: every call in frame N, in order */
static long g_gltrace_busy;         /* --gltrace-busy N: every call in the first frame with N+ draws */
static char g_busy_buf[8 << 20];
static size_t g_busy_len;
static long g_busy_draws;
static long g_busy_after;          /* ...@F: not before frame F */

static void gl_frame_end(void) {
    static int dumped;
    if (!g_gltrace_busy || dumped) return;
    if (g_busy_draws >= g_gltrace_busy && g_frames >= g_busy_after) {
        printf("[host] frame %ld: %ld draws, its GL calls follow\n", g_frames + 1, g_busy_draws);
        fwrite(g_busy_buf, 1, g_busy_len, stdout);
        fflush(stdout);
        dumped = 1;
    }
    g_busy_len = 0;
    g_busy_draws = 0;
}

static void gl_note(const char* fmt, ...) {
    static char seen[4096][160];
    static int nseen;
    static CRITICAL_SECTION cs;
    static LONG init;
    if (!InterlockedExchange(&init, 1)) InitializeCriticalSection(&cs);
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (g_gltrace_busy) {
        int n = snprintf(g_busy_buf + g_busy_len, sizeof g_busy_buf - g_busy_len, "[gl.] %s\n", line);
        if (n > 0 && g_busy_len + n < sizeof g_busy_buf) g_busy_len += n;
        if (!strncmp(line, "glDraw", 6)) g_busy_draws++;
        return;
    }
    if (g_gltrace_frame >= 0) { if (g_frames == g_gltrace_frame) printf("[gl.] %s\n", line); return; }
    EnterCriticalSection(&cs);
    int i = 0;
    while (i < nseen && strcmp(seen[i], line)) i++;
    if (i == nseen && nseen < 4096) {
        strcpy(seen[nseen++], line);
        printf("[gl] f%ld %s\n", g_frames, line);
    }
    LeaveCriticalSection(&cs);
}

static FARPROC gl_real(const char* name) { return GetProcAddress(GetModuleHandleA("opengl32.dll"), name); }

#define GL_CALL0 ((void (WINAPI*)(void))real)()
#define GL_CALL1 ((void (WINAPI*)(uint32_t))real)(a0)
#define GL_CALL2 ((void (WINAPI*)(uint32_t,uint32_t))real)(a0,a1)
#define GL_CALL3 ((void (WINAPI*)(uint32_t,uint32_t,uint32_t))real)(a0,a1,a2)
#define GL_CALL4 ((void (WINAPI*)(uint32_t,uint32_t,uint32_t,uint32_t))real)(a0,a1,a2,a3)
#define GL_CALL9 ((void (WINAPI*)(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t))real)(a0,a1,a2,a3,a4,a5,a6,a7,a8)
#define GL_SHIM(fn, n, ...)                                                          \
    static void shim_##fn(void) {                                                    \
        static FARPROC real;                                                         \
        if (!real) real = gl_real(#fn);                                              \
        uint32_t a[9] = {0};                                                         \
        for (int i = 0; i < n; i++) a[i] = ARG(i);                                   \
        uint32_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3], a4 = a[4], a5 = a[5], a6 = a[6], a7 = a[7], a8 = a[8]; \
        (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8;                  \
        if (g_gltrace) gl_note(__VA_ARGS__);                                         \
        GL_CALL##n;                                                                  \
        g_esp += 4 + n * 4;                                                          \
    }
GL_SHIM(glEnable, 1, "glEnable %04X", a0)
GL_SHIM(glDisable, 1, "glDisable %04X", a0)
GL_SHIM(glTexEnvi, 3, "glTexEnvi %04X %04X %04X", a0, a1, a2)
GL_SHIM(glTexParameteri, 3, "glTexParameteri %04X %04X %04X", a0, a1, a2)
GL_SHIM(glTexGeni, 3, "glTexGeni %04X %04X %04X", a0, a1, a2)
GL_SHIM(glBindTexture, 2, "glBindTexture %04X %u", a0, a1)
static uint32_t g_cp, g_cs;   /* the colour array: pointer, stride (traced draws show their first vertex colour) */
static uint32_t vcol(uint32_t a1, uint32_t a2, uint32_t a3) {
    if (!g_cp || !a1 || !a3) return 0;
    uint32_t i = a2 == 0x1403 ? *(const uint16_t*)(uintptr_t)a3 : a2 == 0x1405 ? *(const uint32_t*)(uintptr_t)a3 : *(const uint8_t*)(uintptr_t)a3;
    return *(const uint32_t*)(uintptr_t)(g_cp + i * (g_cs ? g_cs : 4));
}
static void shim_glDrawElements(void) {
    static FARPROC real;
    if (!real) real = gl_real("glDrawElements");
    uint32_t a0 = ARG(0), a1 = ARG(1), a2 = ARG(2), a3 = ARG(3);
    if (g_gltrace) gl_note("glDrawElements %04X n%u %04X vc%08X", a0, a1, a2, vcol(a1, a2, a3));
    if (g_gltrace == 2) {   /* --gltex: what the driver holds for the bound texture */
        static void (WINAPI *get_iv)(uint32_t, int*);
        static void (WINAPI *get_tl)(uint32_t, int, uint32_t, int*);
        static void (WINAPI *get_tp)(uint32_t, uint32_t, int*);
        if (!get_iv) get_iv = (void*)gl_real("glGetIntegerv"), get_tl = (void*)gl_real("glGetTexLevelParameteriv"),
                     get_tp = (void*)gl_real("glGetTexParameteriv");
        int id = 0, w0 = 0, h0 = 0, f0 = 0, w1 = 0, minf = 0, en = 0, src = 0;
        get_iv(0x8069, &id);         /* GL_TEXTURE_BINDING_2D */
        get_iv(0x0DE1, &en);
        get_iv(0x0BE1, &src);
        get_tl(0x0DE1, 0, 0x1000, &w0); get_tl(0x0DE1, 0, 0x1001, &h0); get_tl(0x0DE1, 0, 0x1003, &f0);
        get_tl(0x0DE1, 1, 0x1000, &w1);
        get_tp(0x0DE1, 0x2801, &minf);
        int maxl = 0;
        get_tp(0x0DE1, 0x813D, &maxl);
        gl_note("  tex %d en%d blendsrc %04X: %dx%d if%04X lv1w%d min%04X max%d", id, en, src, w0, h0, f0, w1, minf, maxl);
    }
    ((void (WINAPI*)(uint32_t, uint32_t, uint32_t, uint32_t))real)(a0, a1, a2, a3);
    g_esp += 4 + 4 * 4;
}
GL_SHIM(glDrawArrays, 3, "glDrawArrays %04X %u n%u", a0, a1, a2)
GL_SHIM(glEnableClientState, 1, "glEnableClientState %04X", a0)
GL_SHIM(glDisableClientState, 1, "glDisableClientState %04X", a0)
GL_SHIM(glBlendFunc, 2, "glBlendFunc %04X %04X", a0, a1)
GL_SHIM(glTexCoordPointer, 4, "glTexCoordPointer %u %04X s%u", a0, a1, a2)
static double u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static double pf(uint32_t p, int i) { return p ? ((const float*)(uintptr_t)p)[i] : -1; }
GL_SHIM(glColorMask, 4, "glColorMask %u %u %u %u", a0, a1, a2, a3)
GL_SHIM(glDepthMask, 1, "glDepthMask %u", a0)
GL_SHIM(glDepthFunc, 1, "glDepthFunc %04X", a0)
GL_SHIM(glVertexPointer, 4, "glVertexPointer %u %04X s%u", a0, a1, a2)
GL_SHIM(glMatrixMode, 1, "glMatrixMode %04X", a0)
GL_SHIM(glLoadIdentity, 0, "glLoadIdentity")
GL_SHIM(glLoadMatrixf, 1, "glLoadMatrixf rot %.3g %.3g %.3g, at %.4g %.4g %.4g", pf(a0, 0), pf(a0, 1), pf(a0, 2), pf(a0, 12), pf(a0, 13), pf(a0, 14))
GL_SHIM(glColorPointer, 4, "glColorPointer %u %04X s%u", (g_cp = a3, g_cs = a2, a0), a1, a2)
GL_SHIM(glColor4f, 4, "glColor4f %.2f %.2f %.2f %.2f", u2f(a0), u2f(a1), u2f(a2), u2f(a3))
GL_SHIM(glColorMaterial, 2, "glColorMaterial %04X %04X", a0, a1)
GL_SHIM(glMaterialfv, 3, "glMaterialfv %04X %04X %.2f %.2f %.2f %.2f", a0, a1, pf(a2, 0), pf(a2, 1), pf(a2, 2), pf(a2, 3))
GL_SHIM(glLightfv, 3, "glLightfv %04X %04X %.2f %.2f %.2f %.2f", a0, a1, pf(a2, 0), pf(a2, 1), pf(a2, 2), pf(a2, 3))
/* SC4 lights its terrain and water on the CPU, into vertex colours, and draws them
 * with LIGHT0 enabled, COLOR_MATERIAL and a 1.0 global ambient: ambient alone
 * gives back the vertex colour. It never calls glLightfv, so LIGHT0 keeps GL's
 * white diffuse and specular and adds them on top: grey-washed terrain and pale
 * water (--original too). Zero them.
 * ponytail: would undo the game's own LIGHT0 if it ever set one before a
 * glLightModelfv; none seen. */
static void shim_glLightModelfv(void) {
    static FARPROC real, lfv;
    if (!real) real = gl_real("glLightModelfv"), lfv = gl_real("glLightfv");
    uint32_t a0 = ARG(0), a1 = ARG(1);
    if (g_gltrace) gl_note("glLightModelfv %04X %.2f %.2f %.2f %.2f", a0, pf(a1, 0), pf(a1, 1), pf(a1, 2), pf(a1, 3));
    ((void (WINAPI*)(uint32_t, uint32_t))real)(a0, a1);
    static const float black[4] = {0, 0, 0, 1};
    ((void (WINAPI*)(uint32_t, uint32_t, const float*))lfv)(0x4000, 0x1201 /* DIFFUSE */, black);
    ((void (WINAPI*)(uint32_t, uint32_t, const float*))lfv)(0x4000, 0x1202 /* SPECULAR */, black);
    g_esp += 4 + 2 * 4;
}
GL_SHIM(glAlphaFunc, 2, "glAlphaFunc %04X %.2f", a0, u2f(a1))
GL_SHIM(glTexSubImage2D, 9, "glTexSubImage2D %04X lv%u %u,%u %ux%u %04X %04X", a0, a1, a2, a3, a4, a5, a6, a7)
/* SC4 uploads only level 0 of its terrain textures and never sets their min
 * filter, so they keep GL's default GL_NEAREST_MIPMAP_LINEAR: incomplete
 * without mipmaps, and today's drivers sample them white (white city terrain,
 * --original too). Capping GL_TEXTURE_MAX_LEVEL at the highest level uploaded
 * makes them complete under any filter. */
static void shim_glTexImage2D(void) {
    static FARPROC real;
    static void (WINAPI *tex_pi)(uint32_t, uint32_t, int);
    if (!real) real = gl_real("glTexImage2D"), tex_pi = (void*)gl_real("glTexParameteri");
    uint32_t a[9];
    for (int i = 0; i < 9; i++) a[i] = ARG(i);
    if (g_gltrace) gl_note("glTexImage2D %04X lv%u if%04X %ux%u b%u %04X %04X", a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
    ((void (WINAPI*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))real)
        (a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]);
    if (a[0] == 0x0DE1) tex_pi(0x0DE1, 0x813D /* GL_TEXTURE_MAX_LEVEL */, (int)a[1]);
    g_esp += 4 + 9 * 4;
}

static void shim_glTexEnvfv(void) {
    static FARPROC real;
    if (!real) real = gl_real("glTexEnvfv");
    uint32_t t = ARG(0), p = ARG(1);
    const float* v = (const float*)(uintptr_t)ARG(2);
    if (g_gltrace) gl_note("glTexEnvfv %04X %04X %.2f %.2f %.2f %.2f", t, p, v[0], v[1], v[2], v[3]);
    ((void (WINAPI*)(uint32_t, uint32_t, const float*))real)(t, p, v);
    g_esp += 4 + 3 * 4;
}

/* Extension entry points the guest calls straight through the pointer
 * wglGetProcAddress gave it; under --gltrace it gets these wrappers instead
 * (and always the compressed upload, for its GL_TEXTURE_MAX_LEVEL cap). */
typedef void (WINAPI *cti2d_t)(uint32_t, int, uint32_t, int, int, int, int, const void*);
static cti2d_t real_cti2d;
static void WINAPI trace_cti2d(uint32_t t, int lv, uint32_t f, int w, int h, int b, int n, const void* d) {
    static void (WINAPI *tex_pi)(uint32_t, uint32_t, int);
    if (!tex_pi) tex_pi = (void*)gl_real("glTexParameteri");
    if (g_gltrace) gl_note("glCompressedTexImage2D %04X lv%d if%04X %dx%d %dB", t, lv, f, w, h, n);
    /* DXT1 as RGB (83F0) turns its cut-out texels black (black squares
     * around trees and buildings, --original too); Direct3D's DXT1 always
     * keeps them transparent, so ask for RGBA DXT1 (83F1). Opaque texels
     * decode the same either way. */
    if (f == 0x83F0) f = 0x83F1;
    real_cti2d(t, lv, f, w, h, b, n, d);
    if (t == 0x0DE1) tex_pi(0x0DE1, 0x813D, lv);   /* the same cap as shim_glTexImage2D */
}
typedef void (WINAPI *at_t)(uint32_t);
static at_t real_at;
static void WINAPI trace_at(uint32_t u) { gl_note("glActiveTexture %04X", u); real_at(u); }
static at_t real_cat;
typedef uint32_t (WINAPI *g7_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static g7_t real_rbr, real_dbr, real_nbr, real_la, real_ua;
static uint32_t WINAPI trace_nbr(uint32_t t) { uint32_t r = ((uint32_t (WINAPI*)(uint32_t))real_nbr)(t); gl_note("glNewBufferRegion %04X -> %u", t, r); return r; }
static void WINAPI trace_rbr(uint32_t r, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    gl_note("glReadBufferRegion %u %u,%u %ux%u", r, x, y, w, h);
    ((void (WINAPI*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))real_rbr)(r, x, y, w, h);
}
static void WINAPI trace_dbr(uint32_t r, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t dx, uint32_t dy) {
    gl_note("glDrawBufferRegion %u %u,%u %ux%u -> %u,%u", r, x, y, w, h, dx, dy);
    real_dbr(r, x, y, w, h, dx, dy);
}
static void WINAPI trace_la(uint32_t f, uint32_t n) { gl_note("glLockArrays %u n%u", f, n); ((void (WINAPI*)(uint32_t, uint32_t))real_la)(f, n); }
static void WINAPI trace_ua(void) { gl_note("glUnlockArrays"); ((void (WINAPI*)(void))real_ua)(); }
static void WINAPI trace_cat(uint32_t u) { gl_note("glClientActiveTexture %04X", u); real_cat(u); }

static void shim_wglGetProcAddress(void) {
    const char* name = (const char*)(uintptr_t)ARG(0);
    PROC p = wglGetProcAddress(name);
    if (p && !strcmp(name, "glCompressedTexImage2DARB")) real_cti2d = (cti2d_t)p, p = (PROC)trace_cti2d;
    else if (g_gltrace && p) {
        gl_note("wglGetProcAddress %s", name);
        if (!strcmp(name, "glActiveTextureARB")) real_at = (at_t)p, p = (PROC)trace_at;
        else if (!strcmp(name, "glNewBufferRegion")) real_nbr = (g7_t)p, p = (PROC)trace_nbr;
        else if (!strcmp(name, "glReadBufferRegion")) real_rbr = (g7_t)p, p = (PROC)trace_rbr;
        else if (!strcmp(name, "glDrawBufferRegion")) real_dbr = (g7_t)p, p = (PROC)trace_dbr;
        else if (!strcmp(name, "glLockArraysEXT")) real_la = (g7_t)p, p = (PROC)trace_la;
        else if (!strcmp(name, "glUnlockArraysEXT")) real_ua = (g7_t)p, p = (PROC)trace_ua;
        else if (!strcmp(name, "glClientActiveTextureARB")) real_cat = (at_t)p, p = (PROC)trace_cat;
    }
    g_eax = (uint32_t)(uintptr_t)p;
    g_esp += 4 + 1 * 4;
}

static void shim_TerminateProcess(void) {
    HANDLE h = (HANDLE)(uintptr_t)ARG(0);
    UINT code = ARG(1);
    if (h == GetCurrentProcess() || GetProcessId(h) == GetCurrentProcessId()) {
        printf("[host] TerminateProcess(self, %u) from sub_%08X\n", code, g_cur_func);
        fflush(stdout);
        native32_dump_icalls(8);
    }
    g_eax = TerminateProcess(h, code);
    g_esp += 4 + 2 * 4;
}

static void shim_ExitProcess(void) {
    UINT code = ARG(0);
    printf("[host] ExitProcess(%u) from sub_%08X\n", code, g_cur_func);
    fflush(stdout);
    ExitProcess(code);
}

static native32_shim_t g_shims[] = {
    { "CreateWindowExA", shim_CreateWindowExA },
    { "ShowWindow", shim_ShowWindow },
    { "GetDC", shim_GetDC },
    { "MessageBoxA", shim_MessageBoxA },
    { "SetWindowPos", shim_SetWindowPos },
    { "MoveWindow", shim_MoveWindow },
    { "GetCursorPos", shim_GetCursorPos },
    { "GetAsyncKeyState", shim_GetAsyncKeyState },
    { "GetKeyState", shim_GetKeyState },
    { "SwapBuffers", shim_SwapBuffers },
    { "BitBlt", shim_BitBlt },
    { "glGetString", shim_glGetString },
    { "glEnable", shim_glEnable },
    { "glDisable", shim_glDisable },
    { "glTexEnvi", shim_glTexEnvi },
    { "glTexEnvfv", shim_glTexEnvfv },
    { "glTexParameteri", shim_glTexParameteri },
    { "glTexGeni", shim_glTexGeni },
    { "glTexImage2D", shim_glTexImage2D },
    { "glBindTexture", shim_glBindTexture },
    { "glDrawElements", shim_glDrawElements },
    { "glDrawArrays", shim_glDrawArrays },
    { "glEnableClientState", shim_glEnableClientState },
    { "glDisableClientState", shim_glDisableClientState },
    { "glBlendFunc", shim_glBlendFunc },
    { "glTexCoordPointer", shim_glTexCoordPointer },
    { "glTexSubImage2D", shim_glTexSubImage2D },
    { "glColorMask", shim_glColorMask },
    { "glDepthMask", shim_glDepthMask },
    { "glDepthFunc", shim_glDepthFunc },
    { "glColorPointer", shim_glColorPointer },
    { "glVertexPointer", shim_glVertexPointer },
    { "glMatrixMode", shim_glMatrixMode },
    { "glLoadIdentity", shim_glLoadIdentity },
    { "glLoadMatrixf", shim_glLoadMatrixf },
    { "glColor4f", shim_glColor4f },
    { "glColorMaterial", shim_glColorMaterial },
    { "glMaterialfv", shim_glMaterialfv },
    { "glLightfv", shim_glLightfv },
    { "glLightModelfv", shim_glLightModelfv },
    { "glAlphaFunc", shim_glAlphaFunc },
    { "wglGetProcAddress", shim_wglGetProcAddress },
    { "ExitProcess", shim_ExitProcess },
    { "TerminateProcess", shim_TerminateProcess },
    { "ChangeDisplaySettingsA", shim_ChangeDisplaySettingsA },
    { "SetDeviceGammaRamp", shim_SetDeviceGammaRamp },
    { "GetModuleHandleA", shim_GetModuleHandleA },
    { "GetModuleFileNameA", shim_GetModuleFileNameA },
    { "GetCommandLineA", shim_GetCommandLineA },
    { "GetProcAddress", shim_GetProcAddress },
    { "SHGetPathFromIDListA", shim_SHGetPathFromIDListA },
    { "RegOpenKeyExA", shim_RegOpenKeyExA },        /* steamreg.c */
    { "RegCreateKeyExA", shim_RegCreateKeyExA },
    { "RegQueryValueExA", shim_RegQueryValueExA },
    { "RegSetValueExA", shim_RegSetValueExA },
    { "RegCloseKey", shim_RegCloseKey },
};

/* --noshim A,B: those imports go straight to Windows (to find a shim that
 * breaks a run). Dropped entries move to the end, past the count. */
static char g_noshim[512];
static int shim_count(void) {
    int n = (int)(sizeof g_shims / sizeof g_shims[0]);
    for (int i = 0; i < n; ) {
        const char* h = strstr(g_noshim, g_shims[i].name);
        size_t k = strlen(g_shims[i].name);
        if (h && (h == g_noshim || h[-1] == ',') && (h[k] == ',' || !h[k])) {
            native32_shim_t t = g_shims[i];
            g_shims[i] = g_shims[--n];
            g_shims[n] = t;
        } else i++;
    }
    return n;
}

/* ---- muted by default (kotor's: the process's own audio session) ------------ */
static const GUID kCLSID_MMDeviceEnumerator =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID kIID_IMMDeviceEnumerator =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID kIID_IAudioSessionManager =
    { 0xBFA971F1, 0x4D5E, 0x40BB, { 0x93, 0x5E, 0x96, 0x70, 0x39, 0xBF, 0xBE, 0xE4 } };

/* 1: muted; 2: there is no audio device; 0: failed. */
static int mute_process(void) {
    IMMDeviceEnumerator* en = NULL;
    IMMDevice* dev = NULL;
    IAudioSessionManager* mgr = NULL;
    ISimpleAudioVolume* vol = NULL;
    HRESULT hr = CoCreateInstance(&kCLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                  &kIID_IMMDeviceEnumerator, (void**)&en);
    if (SUCCEEDED(hr)) hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) { IMMDeviceEnumerator_Release(en); return 2; }
    if (SUCCEEDED(hr)) hr = IMMDevice_Activate(dev, &kIID_IAudioSessionManager, CLSCTX_ALL, NULL, (void**)&mgr);
    if (SUCCEEDED(hr)) hr = IAudioSessionManager_GetSimpleAudioVolume(mgr, &GUID_NULL, FALSE, &vol);
    if (SUCCEEDED(hr)) hr = ISimpleAudioVolume_SetMute(vol, TRUE, NULL);
    if (vol) ISimpleAudioVolume_Release(vol);
    if (mgr) IAudioSessionManager_Release(mgr);
    if (dev) IMMDevice_Release(dev);
    if (en) IMMDeviceEnumerator_Release(en);
    return SUCCEEDED(hr);
}

/* ---- reports ---------------------------------------------------------------- */
/* --spy VA: every indirect call lifted function VA makes, with `this`, its
 * vtable, the first two arguments and the answer. --trace-init is --spy
 * 0x00879FBC, the framework's component init loop. Each lifted indirect call
 * asks recomp_lookup_manual first, so the spy sits in between without
 * touching the lift; the globals are current inside it (RECOMP_REGS_OUT ran).
 * ponytail: lifted targets only, one function at a time; widen when needed. */
static uint32_t g_spy_func;
static uint32_t g_spy_va;
static recomp_func_t g_spy_fn;

static void spy_init_call(void) {
    uint32_t va = g_spy_va, self = g_ecx;
    recomp_func_t fn = g_spy_fn;
    uint32_t a0 = MEM32(g_esp + 4), a1 = MEM32(g_esp + 8);
    fn();
    printf("[spy] 0x%08X this=0x%08X vtable=0x%08X (0x%X, 0x%X) -> 0x%X\n",
           va, self, self ? MEM32(self) : 0, a0, a1, g_eax);
    fflush(stdout);
}

recomp_func_t recomp_lookup_manual(uint32_t va) {
    if (g_spy_func && g_cur_func == g_spy_func) {
        recomp_func_t fn = recomp_lookup(va);
        if (fn) { g_spy_va = va; g_spy_fn = fn; return spy_init_call; }
    }
    return NULL;
}

void recomp_not_lifted(uint32_t va) {
    fprintf(stderr,
        "\n[not-lifted] sub_%08X  (called from 0x%08X)\n"
        "  Widen the closure:  py -3 run_lift.py --roots 0x%08X  (or --max N, or --all)\n",
        va, g_cur_func, va);
    recomp_dump_trace("not-lifted");
    native32_dump_icalls(8);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 2);
}

/* Written through WriteFile from a static buffer: a fault while another thread
 * holds the CRT's stderr lock otherwise ends with no report (The Movies). */
static char g_crash_buf[4096];
static int g_crash_len;
static void crash_emit(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf(g_crash_buf + g_crash_len, sizeof g_crash_buf - 1 - g_crash_len, fmt, ap);
    va_end(ap);
    if (n > 0) g_crash_len += n;
}

static LONG CALLBACK crash(EXCEPTION_POINTERS* ep) {
    static volatile LONG once;
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if ((r->ExceptionCode & 0xF0000000u) != 0xC0000000u) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedExchange(&once, 1)) TerminateProcess(GetCurrentProcess(), 3);
    crash_emit("\n=== fault 0x%08lX at 0x%p, thread %lu ===\n", r->ExceptionCode,
               r->ExceptionAddress, GetCurrentThreadId());
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        ULONG_PTR op = r->ExceptionInformation[0];
        uint32_t at = (uint32_t)r->ExceptionInformation[1];
        crash_emit("  %s of 0x%08X%s\n", op == 0 ? "read" : op == 1 ? "write" : "execute", at,
                   native32_in_guest(at) ? " (inside the guest image)" : at < 0x10000 ? " (null/low)" : "");
    }
    crash_emit("  in lifted sub_%08X, last native call %s\n", g_cur_func, g_cur_import);
    crash_emit("  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n",
               g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
    crash_emit("last indirect calls (newest first):\n");
    for (int i = 1; i <= 12 && i <= (int)g_icall_trace_idx; i++) {
        uint32_t k = (g_icall_trace_idx - i) & (ICALL_TRACE_SIZE - 1);
        const char* nm = native32_name(g_icall_trace[k]);
        crash_emit("  0x%08X  from 0x%08X  %s\n", g_icall_trace[k], g_icall_from[k], nm ? nm : "");
    }
    DWORD w;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), g_crash_buf, (DWORD)g_crash_len, &w, NULL);
    recomp_trace_flush();
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* cGZFramework (pointer at 0x00B540AC) stamps how far its startup got at +0xCC
 * (0x0087AF13): 1 COM and plugins, 3-4 the app's own init, 5-6 running. Each
 * change is a milestone line. ponytail: polled every 5 ms, so a phase that
 * lasts less than that can be missed; hook 0x0087AF13 if that matters. */
static int readable(uint32_t a, uint32_t n);

static DWORD WINAPI phase_watch(LPVOID unused) {
    (void)unused;
    int last = -1;
    for (;;) {
        if (!readable(0x00B540ACu, 4)) { Sleep(5); continue; }   /* --original maps the image later */
        uint32_t fw = MEM32(0x00B540ACu);
        int ph = fw ? (int)MEM32(fw + 0xCC) : -1;
        if (ph != last) {
            if (ph == 7) input_main_loop();
            printf("[host] framework phase %d", ph);
            if (fw) printf(" (framework 0x%08X, vtable 0x%08X)", fw, MEM32(fw));
            printf("\n");
            fflush(stdout);
            last = ph;
        }
        Sleep(5);
    }
}

/* Every MSVC C++ throw (0xE06D7363), first chance: the type, from the x86
 * ThrowInfo (+0x0C catchable types -> [0] -> type descriptor, name at +8),
 * the object's first words, and where it came from. Passed on untouched. */
/* Committed and readable for n bytes, asked without touching it: IsBadReadPtr
 * faults to find out, and crash() takes that fault for the game's. */
static int readable(uint32_t a, uint32_t n) {
    MEMORY_BASIC_INFORMATION mi;
    if (a < 0x10000 || !VirtualQuery((void*)(uintptr_t)a, &mi, sizeof mi) || mi.State != MEM_COMMIT) return 0;
    if (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return a + n <= (uint32_t)(uintptr_t)mi.BaseAddress + (uint32_t)mi.RegionSize;
}

static LONG CALLBACK cxx_throw(EXCEPTION_POINTERS* ep) {
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if (r->ExceptionCode != 0xE06D7363u || r->NumberParameters < 3) return EXCEPTION_CONTINUE_SEARCH;
    uint32_t obj = (uint32_t)r->ExceptionInformation[1], ti = (uint32_t)r->ExceptionInformation[2];
    const char* name = "?";
    if (ti && MEM32(ti + 0x0C) && MEM32(MEM32(ti + 0x0C) + 4))
        name = (const char*)(uintptr_t)(MEM32(MEM32(MEM32(ti + 0x0C) + 4) + 4) + 8);
    printf("[host] C++ throw %s, object 0x%08X [%08X %08X %08X], in sub_%08X, thread %lu\n",
           name, obj, obj ? MEM32(obj) : 0, obj ? MEM32(obj + 4) : 0, obj ? MEM32(obj + 8) : 0,
           g_cur_func, GetCurrentThreadId());
    /* Any word of the object that points at text (an error message, a file
     * name): STLport strings keep a plain char* to it. */
    if (obj && readable(obj, 0x20))
        printf("        words %08X %08X %08X %08X %08X %08X %08X %08X\n", MEM32(obj), MEM32(obj + 4),
               MEM32(obj + 8), MEM32(obj + 12), MEM32(obj + 16), MEM32(obj + 20), MEM32(obj + 24), MEM32(obj + 28));
    /* nSCRes::cParseError's builder (0x0067A250) formats into this buffer. */
    /* nSCRes::cParseError (0x0067A250): {vtable, code, ?, message start, finish, ...}. */
    if (strstr(name, "cParseError") && MEM32(obj + 16) > MEM32(obj + 12) && MEM32(obj + 16) - MEM32(obj + 12) < 1024)
        printf("        message: \"%.*s\"\n", (int)(MEM32(obj + 16) - MEM32(obj + 12)), (const char*)(uintptr_t)MEM32(obj + 12));
    for (int i = 0; obj && i < 16; i++) {
        uint32_t w = MEM32(obj + 4 * i);
        if (!readable(w, 128)) continue;
        const char* s = (const char*)(uintptr_t)w;
        int n = 0;
        while (n < 120 && s[n] >= 32 && s[n] < 127) n++;
        if (n >= 3 && !s[n]) printf("        +0x%02X -> \"%s\"\n", 4 * i, s);
    }
    fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    Sleep(g_watchdog_s * 1000);
    fprintf(stderr, "\n[watchdog] %lu s: in sub_%08X, last native call %s, %u indirect calls, %ld frames\n",
            g_watchdog_s, g_cur_func, g_cur_import, g_icall_count, g_frames);
    native32_dump_icalls(8);
    recomp_trace_flush();
    fflush(stdout);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 4);
    return 0;
}

int main(int argc, char** argv) {
    const char* exe = "work\\sc4.exe";
    const char* game = "game";
    const char* game_args = "-intro:off -w";   /* the game's own switches; --args replaces them */
    char exe_full[MAX_PATH], game_full[MAX_PATH], apps[MAX_PATH];
    int run = 0, sound = 0, user_docs = 0;
    for (int i = 1; i < argc; i++) {
        int n = recomp_trace_arg(argc, argv, i);
        if (!n) n = input_arg(argc, argv, i);
        if (n) { i += n - 1; continue; }
        if (!strcmp(argv[i], "--run")) run = 1;
        else if (!strcmp(argv[i], "--sound")) sound = 1;
        else if (!strcmp(argv[i], "--headless")) g_headless = 1;
        else if (!strcmp(argv[i], "--original")) g_original = 1;
        else if (!strcmp(argv[i], "--user-docs")) user_docs = 1;
        else if (!strcmp(argv[i], "--trace-init")) g_spy_func = 0x00879FBCu;
        else if (!strcmp(argv[i], "--snap") && i + 1 < argc) {
            static char snap_full[MAX_PATH];     /* the run chdirs into game\Apps */
            CreateDirectoryA(argv[++i], NULL);
            GetFullPathNameA(argv[i], MAX_PATH, snap_full, NULL);
            g_snap_dir = snap_full;
        }
        else if (!strcmp(argv[i], "--snap-every") && i + 1 < argc) g_snap_every = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--monitor") && i + 1 < argc) {
            int want = atoi(argv[++i]);
            EnumDisplayMonitors(NULL, NULL, find_monitor, want);
            if (!g_mon_on) { fprintf(stderr, "no display %d\n", want); return 1; }
            printf("  windows on display %d: %ld,%ld %ldx%ld\n", want, g_mon.left, g_mon.top,
                   g_mon.right - g_mon.left, g_mon.bottom - g_mon.top);
        }
        else if (!strcmp(argv[i], "--gl-only") && i + 1 < argc) strncpy(g_gl_only, argv[++i], sizeof g_gl_only - 1);
        else if (!strcmp(argv[i], "--gltrace")) g_gltrace = 1;
        else if (!strcmp(argv[i], "--gltex")) g_gltrace = 2;   /* before --gltrace-busy */
        else if (!strcmp(argv[i], "--gltrace-busy") && i + 1 < argc) { char* e; if (!g_gltrace) g_gltrace = 1; g_gltrace_busy = strtol(argv[++i], &e, 0); if (*e == '@') g_busy_after = strtol(e + 1, NULL, 0); }
        else if (!strcmp(argv[i], "--gltrace-frame") && i + 1 < argc) g_gltrace = 1, g_gltrace_frame = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--noshim") && i + 1 < argc) strncpy(g_noshim, argv[++i], sizeof g_noshim - 1);
        else if (!strcmp(argv[i], "--gl-hide") && i + 1 < argc) strncpy(g_gl_hide, argv[++i], sizeof g_gl_hide - 1);
        else if (!strcmp(argv[i], "--spy") && i + 1 < argc) g_spy_func = strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--modeswitch")) g_modeswitch = 1;
        else if (!strcmp(argv[i], "--gamma")) g_gamma = 1;
        else if (!strcmp(argv[i], "--exe") && i + 1 < argc) exe = argv[++i];
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--args") && i + 1 < argc) game_args = argv[++i];
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) g_watchdog_s = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--native-trace")) native32_trace_native = 1;
        else if (!strcmp(argv[i], "--callbacks")) native32_trace_callbacks = 1;
        else {
            printf("usage: sc4 [--run] [--headless] [--sound] [--modeswitch] [--gamma] [--original] [--user-docs] [--trace-init] [--spy VA] [--monitor N] [--gl-hide EXT,...] [--gl-only EXT,...] [--gltrace] [--gltex] [--gltrace-frame N] [--gltrace-busy DRAWS[@FRAME]] [--noshim NAME,...] [--snap DIR] [--snap-every N] [--exe work\\sc4.exe] [--game game]\n"
                   "           [--args \"-intro:off -w\"] [--watchdog S] [--native-trace] [--callbacks]\n");
            input_help();
            recomp_trace_help();
            return argv[i][1] == 'h' || argv[i][2] == 'h' ? 0 : 1;
        }
    }
    GetFullPathNameA(exe, MAX_PATH, exe_full, NULL);
    GetFullPathNameA(game, MAX_PATH, game_full, NULL);
    _snprintf(apps, sizeof apps - 1, "%s\\Apps", game_full);
    _snprintf(g_guest_exe, sizeof g_guest_exe - 1, "%s\\SimCity 4.exe", apps);
    _snprintf(g_guest_cmdline, sizeof g_guest_cmdline - 1, "\"%s\" %s", g_guest_exe, game_args);
    SetDllDirectoryA(apps);
    if (!user_docs) {                           /* before the run chdirs into game\Apps */
        typedef HRESULT (WINAPI *gfp_t)(HWND, int, HANDLE, DWORD, LPSTR);
        gfp_t gfp = (gfp_t)GetProcAddress(LoadLibraryA("shell32.dll"), "SHGetFolderPathA");
        if (gfp) gfp(NULL, CSIDL_PERSONAL_, NULL, 0, g_real_docs);
        CreateDirectoryA("work", NULL);
        GetFullPathNameA("work\\docs", MAX_PATH, g_private_docs, NULL);
        CreateDirectoryA(g_private_docs, NULL);
        printf("  documents: %s (not %s)\n", g_private_docs, g_real_docs);
    }
    {
        char vdf[MAX_PATH], dir[MAX_PATH];
        _snprintf(vdf, sizeof vdf - 1, "%s\\24780_install.vdf", game_full);
        _snprintf(dir, sizeof dir - 1, "%s", game_full);   /* %INSTALLDIR% */
        printf("  registry: %d keys from Steam's install script%s\n", steamreg_load(vdf, dir),
               GetFileAttributesA(vdf) == INVALID_FILE_ATTRIBUTES ? " (none found: real registry only)" : "");
    }

    AddVectoredExceptionHandler(1, cxx_throw);
    if (g_original) {
        /* The same shims, folder, mute and watchdog around the original code. */
        if (!SetCurrentDirectoryA(apps)) { fprintf(stderr, "cannot enter %s\n", apps); return 1; }
        CoInitialize(NULL);
        if (!sound) mute_process();
        if (g_watchdog_s) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
        CloseHandle(CreateThread(NULL, 0, phase_watch, NULL, 0, NULL));   /* starts the input clock */
        input_start();
        return oracle_run(exe_full, SC4_IMAGE_BASE, g_shims, shim_count(), NULL, 0);
    }

    native32_init();
    AddVectoredExceptionHandler(0, crash);
    printf("SimCity 4 recomp host\n  lifted functions in dispatch: %u\n", recomp_dispatch_count);

    uint32_t span = native32_map(exe_full, SC4_IMAGE_BASE);
    if (!span) { fprintf(stderr, "cannot map %s at 0x%08X\n", exe_full, SC4_IMAGE_BASE); return 1; }
    printf("  mapped %s: 0x%08X-0x%08X\n", exe, SC4_IMAGE_BASE, SC4_IMAGE_BASE + span);
    if (native32_bind(SC4_IMAGE_BASE, g_shims, shim_count())) return 1;
    printf("  guest command line %s\n", g_guest_cmdline);

    if (!run) {
        printf("\n(dry run: image mapped and bound; --run enters 0x%08X)\n", sc4_entry_va);
        return 0;
    }
    if (!SetCurrentDirectoryA(apps)) { fprintf(stderr, "cannot enter %s\n", apps); return 1; }
    CoInitialize(NULL);
    int m = sound ? 1 : mute_process();
    if (!m) { fprintf(stderr, "cannot mute the process audio session; pass --sound to run with sound\n"); return 1; }
    printf("  audio: %s\n", sound ? "on (--sound)" : m == 2 ? "muted (no audio device)" : "muted (--sound to hear it)");
    /* With no output device DirectSound fails, the sound manager's init never
     * runs, and its per-frame update writes through the buffer object init
     * would have made (0x00876C03): SC4 needs -audio:off on such a machine,
     * the original as much as the lift. */
    if (m == 2 && !strstr(g_guest_cmdline, "-audio:")) {
        strncat(g_guest_cmdline, " -audio:off", sizeof g_guest_cmdline - strlen(g_guest_cmdline) - 1);
        printf("  guest command line %s (no audio device)\n", g_guest_cmdline);
    }
    if (g_watchdog_s) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, phase_watch, NULL, 0, NULL));
    input_start();
    printf("  entering 0x%08X\n\n", sc4_entry_va);
    fflush(stdout);
    native32_call_guest(sc4_entry_va, 0, NULL);
    printf("\nentry returned eax=%08X\n", g_eax);
    return (int)g_eax;
}
