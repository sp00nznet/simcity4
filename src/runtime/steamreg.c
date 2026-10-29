/*
 * The registry Steam would have written: the keys in the game folder's
 * <appid>_install.vdf ("InstallScript" > "Registry"), served to the game from
 * memory. Steam runs that script on the first launch through Steam; a copy
 * that never was launched (or a test VM) has none of the keys, and SimCity 4's
 * app init (0x0044C320) then fails on "Install Dir". Nothing is written to the
 * real registry. Keys the script does not name pass through to the real one,
 * except under the script's own top-level roots, which stay virtual so the
 * game's writes there never reach the machine.
 *
 * Language blocks ("english" { "Language" "English US" }) are flattened: the
 * english one's values are taken, the others ignored.
 * ponytail: writes are kept for the run, not saved; save them beside the game
 * if a setting stored there ever needs to survive a restart.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

#ifndef STEAMREG_SELFTEST
#include "native32.h"
#endif
#include "steamreg.h"

#define ARG(n) MEM32(g_esp + 4 + 4 * (n))
#define MAX_KEYS 64
#define MAX_VALS 32
#define HANDLE_BASE 0x5EE00000u     /* fake HKEYs: HANDLE_BASE + key index */

typedef struct { char name[64]; DWORD type; DWORD size; BYTE data[512]; } reg_val_t;
typedef struct { char path[256]; int nvals; reg_val_t vals[MAX_VALS]; } reg_key_t;

static reg_key_t g_keys[MAX_KEYS];
static int g_nkeys;
static char g_roots[4][256];        /* script top-level roots, kept virtual */
static int g_nroots;

/* "HKEY_LOCAL_MACHINE\\SOFTWARE\\Maxis" or (0x80000002, "Software\\Maxis") ->
 * "hklm\software\maxis": one spelling to compare. */
static void norm(char* out, size_t n, uint32_t hive, const char* sub) {
    const char* h = hive == (uint32_t)(uintptr_t)HKEY_LOCAL_MACHINE ? "hklm"
                  : hive == (uint32_t)(uintptr_t)HKEY_CURRENT_USER ? "hkcu" : "";
    if (!hive && sub) {
        if (!_strnicmp(sub, "HKEY_LOCAL_MACHINE\\", 19)) { h = "hklm"; sub += 19; }
        else if (!_strnicmp(sub, "HKEY_CURRENT_USER\\", 18)) { h = "hkcu"; sub += 18; }
    }
    _snprintf(out, n - 1, "%s\\%s", h, sub ? sub : "");
    out[n - 1] = 0;
    for (char* p = out; *p; p++) *p = (char)tolower((unsigned char)*p);
    size_t l = strlen(out);
    while (l && out[l - 1] == '\\') out[--l] = 0;
}

static reg_key_t* find_key(const char* path) {
    for (int i = 0; i < g_nkeys; i++) if (!strcmp(g_keys[i].path, path)) return &g_keys[i];
    return NULL;
}

static reg_key_t* add_key(const char* path) {
    reg_key_t* k = find_key(path);
    if (k || g_nkeys == MAX_KEYS) return k;
    k = &g_keys[g_nkeys++];
    strncpy(k->path, path, sizeof k->path - 1);
    return k;
}

static void set_val(reg_key_t* k, const char* name, DWORD type, const void* data, DWORD size) {
    if (!k) return;
    reg_val_t* v = NULL;
    for (int i = 0; i < k->nvals; i++) if (!_stricmp(k->vals[i].name, name)) v = &k->vals[i];
    if (!v) { if (k->nvals == MAX_VALS) return; v = &k->vals[k->nvals++]; }
    strncpy(v->name, name, sizeof v->name - 1);
    v->type = type;
    v->size = size < sizeof v->data ? size : sizeof v->data;
    memcpy(v->data, data, v->size);
}

static int is_virtual(const char* path) {
    if (find_key(path)) return 1;
    for (int i = 0; i < g_nroots; i++) {
        size_t l = strlen(g_roots[i]);
        if (!strncmp(path, g_roots[i], l) && (path[l] == 0 || path[l] == '\\')) return 1;
    }
    return 0;
}

/* ---- the .vdf: quoted tokens and braces ------------------------------------ */
static const char* g_p;

static int token(char* out, size_t n) {          /* 1 string, '{', '}', 0 end */
    while (*g_p && (isspace((unsigned char)*g_p))) g_p++;
    if (*g_p == '{' || *g_p == '}') return *g_p++;
    if (*g_p != '"') return 0;
    size_t i = 0;
    for (g_p++; *g_p && *g_p != '"'; g_p++) {
        char c = *g_p;
        if (c == '\\' && g_p[1]) c = *++g_p;      /* \\ -> \ */
        if (i + 1 < n) out[i++] = c;
    }
    if (*g_p) g_p++;
    out[i] = 0;
    return 1;
}

static void skip_block(void) {
    char t[512];
    for (int depth = 1, k; depth && (k = token(t, sizeof t)); )
        depth += k == '{' ? 1 : k == '}' ? -1 : 0;
}

/* One "dword" or "string" block of a key; nested language blocks recurse. */
static void read_values(reg_key_t* k, int dword, const char* installdir) {
    char name[512], val[512];
    for (int t; (t = token(name, sizeof name)) == 1; ) {
        if (token(val, sizeof val) == '{') {
            if (!_stricmp(name, "english")) read_values(k, dword, installdir);
            else skip_block();
            continue;
        }
        const char* def = !_stricmp(name, "(Default)") ? "" : name;
        if (dword) {
            DWORD d = strtoul(val, NULL, 0);
            set_val(k, def, REG_DWORD, &d, 4);
        } else {
            char full[512];
            const char* at = strstr(val, "%INSTALLDIR%");
            if (at) _snprintf(full, sizeof full - 1, "%.*s%s%s", (int)(at - val), val, installdir, at + 12);
            else strcpy(full, val);
            full[sizeof full - 1] = 0;
            set_val(k, def, REG_SZ, full, (DWORD)strlen(full) + 1);
        }
    }
}

int steamreg_load(const char* vdf, const char* installdir) {
    FILE* f = fopen(vdf, "rb");
    if (!f) return 0;
    static char buf[1 << 16];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    g_p = strstr(buf, "\"Registry\"");
    if (!g_p) return 0;
    char t[512], path[256];
    g_p += 10;
    if (token(t, sizeof t) != '{') return 0;
    for (int k; (k = token(t, sizeof t)) == 1; ) {          /* "HKEY_...\\path" { */
        norm(path, sizeof path, 0, t);
        reg_key_t* key = add_key(path);
        if (token(t, sizeof t) != '{') return 0;
        for (int b; (b = token(t, sizeof t)) == 1; ) {      /* "dword"/"string" { */
            if (token(path, sizeof path) != '{') return 0;
            read_values(key, !_stricmp(t, "dword"), installdir);
        }
    }
    /* Roots: each key's first three levels (hklm\software\maxis). */
    for (int i = 0; i < g_nkeys; i++) {
        char r[256];
        strcpy(r, g_keys[i].path);
        char* p = r;
        for (int lv = 0; lv < 3 && p; lv++) p = strchr(p + 1, '\\');
        if (p) *p = 0;
        int seen = 0;
        for (int j = 0; j < g_nroots; j++) seen |= !strcmp(g_roots[j], r);
        if (!seen && g_nroots < 4) strcpy(g_roots[g_nroots++], r);
    }
    return g_nkeys;
}

#ifndef STEAMREG_SELFTEST
/* ---- the shims ------------------------------------------------------------- */
static int handle_key(uint32_t h) {
    return h >= HANDLE_BASE && h < HANDLE_BASE + MAX_KEYS ? (int)(h - HANDLE_BASE) : -1;
}

/* The full path of (parent handle, subkey), or 0 if the parent is a real key. */
static int resolve(uint32_t parent, const char* sub, char* out, size_t n) {
    int i = handle_key(parent);
    if (i >= 0) {
        _snprintf(out, n - 1, "%s%s%s", g_keys[i].path, sub && *sub ? "\\" : "", sub ? sub : "");
        out[n - 1] = 0;
        for (char* p = out; *p; p++) *p = (char)tolower((unsigned char)*p);
        return 1;
    }
    if (parent != (uint32_t)(uintptr_t)HKEY_LOCAL_MACHINE && parent != (uint32_t)(uintptr_t)HKEY_CURRENT_USER) return 0;
    norm(out, n, parent, sub);
    return 1;
}

void shim_RegOpenKeyExA(void) {
    uint32_t parent = ARG(0), out = ARG(4);
    const char* sub = (const char*)(uintptr_t)ARG(1);
    char path[256];
    if (resolve(parent, sub, path, sizeof path) && is_virtual(path)) {
        reg_key_t* k = find_key(path);
        g_eax = k ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
        if (k && out) MEM32(out) = HANDLE_BASE + (uint32_t)(k - g_keys);
    } else {
        g_eax = RegOpenKeyExA((HKEY)(uintptr_t)parent, sub, ARG(2), ARG(3), (PHKEY)(uintptr_t)out);
    }
    g_esp += 4 + 5 * 4;
}

void shim_RegCreateKeyExA(void) {
    uint32_t parent = ARG(0), out = ARG(7), disp = ARG(8);
    const char* sub = (const char*)(uintptr_t)ARG(1);
    char path[256];
    if (resolve(parent, sub, path, sizeof path) && is_virtual(path)) {
        int existed = find_key(path) != NULL;
        reg_key_t* k = add_key(path);
        g_eax = k ? ERROR_SUCCESS : ERROR_OUTOFMEMORY;
        if (k && out) MEM32(out) = HANDLE_BASE + (uint32_t)(k - g_keys);
        if (k && disp) MEM32(disp) = existed ? REG_OPENED_EXISTING_KEY : REG_CREATED_NEW_KEY;
    } else {
        g_eax = RegCreateKeyExA((HKEY)(uintptr_t)parent, sub, ARG(2), (LPSTR)(uintptr_t)ARG(3), ARG(4), ARG(5),
                                (LPSECURITY_ATTRIBUTES)(uintptr_t)ARG(6), (PHKEY)(uintptr_t)out, (LPDWORD)(uintptr_t)disp);
    }
    g_esp += 4 + 9 * 4;
}

void shim_RegQueryValueExA(void) {
    int i = handle_key(ARG(0));
    if (i < 0) {
        g_eax = RegQueryValueExA((HKEY)(uintptr_t)ARG(0), (LPCSTR)(uintptr_t)ARG(1), (LPDWORD)(uintptr_t)ARG(2),
                                 (LPDWORD)(uintptr_t)ARG(3), (LPBYTE)(uintptr_t)ARG(4), (LPDWORD)(uintptr_t)ARG(5));
    } else {
        const char* name = ARG(1) ? (const char*)(uintptr_t)ARG(1) : "";
        uint32_t type = ARG(3), data = ARG(4), size = ARG(5);
        reg_val_t* v = NULL;
        for (int j = 0; j < g_keys[i].nvals; j++) if (!_stricmp(g_keys[i].vals[j].name, name)) v = &g_keys[i].vals[j];
        if (!v) g_eax = ERROR_FILE_NOT_FOUND;
        else {
            if (type) MEM32(type) = v->type;
            g_eax = ERROR_SUCCESS;
            if (data && size && MEM32(size) < v->size) g_eax = ERROR_MORE_DATA;
            else if (data) memcpy((void*)(uintptr_t)data, v->data, v->size);
            if (size) MEM32(size) = v->size;
        }
    }
    g_esp += 4 + 6 * 4;
}

void shim_RegSetValueExA(void) {
    int i = handle_key(ARG(0));
    if (i < 0) {
        g_eax = RegSetValueExA((HKEY)(uintptr_t)ARG(0), (LPCSTR)(uintptr_t)ARG(1), ARG(2), ARG(3),
                               (const BYTE*)(uintptr_t)ARG(4), ARG(5));
    } else {
        set_val(&g_keys[i], ARG(1) ? (const char*)(uintptr_t)ARG(1) : "", ARG(3), (const void*)(uintptr_t)ARG(4), ARG(5));
        g_eax = ERROR_SUCCESS;
    }
    g_esp += 4 + 6 * 4;
}

void shim_RegCloseKey(void) {
    g_eax = handle_key(ARG(0)) >= 0 ? ERROR_SUCCESS : RegCloseKey((HKEY)(uintptr_t)ARG(0));
    g_esp += 4 + 1 * 4;
}

#else
/* cl /DSTEAMREG_SELFTEST: the parser on SimCity 4's script shape. */
int main(void) {
    const char* vdf = "\"InstallScript\"{\"Registry\"{\"HKEY_LOCAL_MACHINE\\\\SOFTWARE\\\\Maxis\\\\SimCity 4\""
        "{\"dword\"{\"IsDeluxe\"\"1\"}\"string\"{\"Install Dir\"\"%INSTALLDIR%\\\\\"\"english\"{\"Language\"\"English US\"}"
        "\"French\"{\"Language\"\"French\"}}}\"HKEY_LOCAL_MACHINE\\\\SOFTWARE\\\\Maxis\\\\SimCity 4\\\\EP1\""
        "{\"string\"{\"(Default)\"\"KEY\"}}}}";
    FILE* f = fopen("steamreg_test.vdf", "wb"); fputs(vdf, f); fclose(f);
    int n = steamreg_load("steamreg_test.vdf", "C:\\Games\\SC4");
    reg_key_t* k = find_key("hklm\\software\\maxis\\simcity 4");
    int ok = n == 2 && k && k->nvals == 3
        && !strcmp((char*)k->vals[1].data, "C:\\Games\\SC4\\")
        && !strcmp((char*)k->vals[2].data, "English US")
        && *(DWORD*)k->vals[0].data == 1
        && is_virtual("hklm\\software\\maxis\\simcity 4\\1.0")
        && !is_virtual("hklm\\software\\microsoft")
        && !strcmp((char*)find_key("hklm\\software\\maxis\\simcity 4\\ep1")->vals[0].data, "KEY");
    printf("steamreg selftest: %s\n", ok ? "ok" : "FAIL");
    remove("steamreg_test.vdf");
    return !ok;
}
#endif
