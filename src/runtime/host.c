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

extern const uint32_t sc4_entry_va;  /* recomp_dispatch.c */

#define SC4_IMAGE_BASE 0x00400000u
#define ARG(n) MEM32(g_esp + 4 + 4 * (n))

static DWORD g_watchdog_s;
static int   g_modeswitch;   /* --modeswitch: let ChangeDisplaySettingsA through */
static int   g_gamma;        /* --gamma: let SetDeviceGammaRamp through */
static int   g_headless;

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

static void shim_GetCommandLineA(void) {
    g_eax = (uint32_t)(uintptr_t)g_guest_cmdline;
    g_esp += 4;
}

/* ---- windows ------------------------------------------------------------------
 * A shim runs holding the machine, and a window call can wait on another
 * thread; the call runs with the machine let go, as native32's bridge does for
 * every unshimmed import (kotor's startup deadlock). */
#define UNLOCKED(call) do { uint32_t esp_ = g_esp; mach_leave(); \
    call; DWORD err_ = GetLastError(); mach_enter(); \
    g_esp = esp_; SetLastError(err_); } while (0)

static void shim_CreateWindowExA(void) {
    uint32_t a[12];
    for (int i = 0; i < 12; i++) a[i] = ARG(i);
    if (g_headless) a[3] &= ~WS_VISIBLE;
    HWND h;
    UNLOCKED(h = CreateWindowExA(a[0], (LPCSTR)(uintptr_t)a[1], (LPCSTR)(uintptr_t)a[2], a[3],
                                 a[4], a[5], a[6], a[7], (HWND)(uintptr_t)a[8],
                                 (HMENU)(uintptr_t)a[9], (HINSTANCE)(uintptr_t)a[10], (LPVOID)(uintptr_t)a[11]));
    DWORD err = h ? 0 : GetLastError();
    printf("[host] CreateWindowExA(\"%s\", %dx%d) -> %p (error %lu)\n",
           a[2] && HIWORD(a[2]) ? (const char*)(uintptr_t)a[2] : "", (int)a[6], (int)a[7], (void*)h, err);
    SetLastError(err);
    g_eax = (uint32_t)(uintptr_t)h;
    g_esp += 4 + 12 * 4;
}

static void shim_ShowWindow(void) {
    HWND w = (HWND)(uintptr_t)ARG(0);
    int cmd = (int)ARG(1);
    g_eax = 0;
    if (!g_headless) UNLOCKED(g_eax = ShowWindow(w, cmd));
    g_esp += 4 + 2 * 4;
}

static void shim_GetDC(void) {
    HWND w = (HWND)(uintptr_t)ARG(0);
    g_eax = (g_headless && !w) ? 0 : (uint32_t)(uintptr_t)GetDC(w);
    g_esp += 4 + 1 * 4;
}

static void shim_SetWindowPos(void) {
    uint32_t a[7];
    for (int i = 0; i < 7; i++) a[i] = ARG(i);
    if (g_headless) a[6] = (a[6] | SWP_NOACTIVATE | SWP_NOZORDER) & ~SWP_SHOWWINDOW;
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
static volatile LONG g_frames;

static void shim_SwapBuffers(void) {
    LONG n = InterlockedIncrement(&g_frames);
    if (n == 1 || n == 60 || n % 1000 == 0) printf("[host] frame %ld presented (OpenGL)\n", n);
    HDC dc = (HDC)(uintptr_t)ARG(0);
    HWND w = WindowFromDC(dc);
    if (g_headless && w && IsWindowVisible(w)) ShowWindow(w, SW_HIDE);
    g_eax = SwapBuffers(dc);
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
    { "SwapBuffers", shim_SwapBuffers },
    { "ExitProcess", shim_ExitProcess },
    { "TerminateProcess", shim_TerminateProcess },
    { "ChangeDisplaySettingsA", shim_ChangeDisplaySettingsA },
    { "SetDeviceGammaRamp", shim_SetDeviceGammaRamp },
    { "GetModuleHandleA", shim_GetModuleHandleA },
    { "GetModuleFileNameA", shim_GetModuleFileNameA },
    { "GetCommandLineA", shim_GetCommandLineA },
};

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
recomp_func_t recomp_lookup_manual(uint32_t va) {
    (void)va;
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
    int run = 0, sound = 0;
    for (int i = 1; i < argc; i++) {
        int n = recomp_trace_arg(argc, argv, i);
        if (n) { i += n - 1; continue; }
        if (!strcmp(argv[i], "--run")) run = 1;
        else if (!strcmp(argv[i], "--sound")) sound = 1;
        else if (!strcmp(argv[i], "--headless")) g_headless = 1;
        else if (!strcmp(argv[i], "--modeswitch")) g_modeswitch = 1;
        else if (!strcmp(argv[i], "--gamma")) g_gamma = 1;
        else if (!strcmp(argv[i], "--exe") && i + 1 < argc) exe = argv[++i];
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--args") && i + 1 < argc) game_args = argv[++i];
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) g_watchdog_s = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--native-trace")) native32_trace_native = 1;
        else if (!strcmp(argv[i], "--callbacks")) native32_trace_callbacks = 1;
        else {
            printf("usage: sc4 [--run] [--headless] [--sound] [--modeswitch] [--gamma] [--exe work\\sc4.exe] [--game game]\n"
                   "           [--args \"-intro:off -w\"] [--watchdog S] [--native-trace] [--callbacks]\n");
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

    native32_init();
    AddVectoredExceptionHandler(0, crash);
    printf("SimCity 4 recomp host\n  lifted functions in dispatch: %u\n", recomp_dispatch_count);

    uint32_t span = native32_map(exe_full, SC4_IMAGE_BASE);
    if (!span) { fprintf(stderr, "cannot map %s at 0x%08X\n", exe_full, SC4_IMAGE_BASE); return 1; }
    printf("  mapped %s: 0x%08X-0x%08X\n", exe, SC4_IMAGE_BASE, SC4_IMAGE_BASE + span);
    if (native32_bind(SC4_IMAGE_BASE, g_shims, (int)(sizeof g_shims / sizeof g_shims[0]))) return 1;
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
    if (g_watchdog_s) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    printf("  entering 0x%08X\n\n", sc4_entry_va);
    fflush(stdout);
    native32_call_guest(sc4_entry_va, 0, NULL);
    printf("\nentry returned eax=%08X\n", g_eax);
    return (int)g_eax;
}
