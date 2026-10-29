/* The registry keys Steam's install script would have written (steamreg.c). */
#pragma once

/* Read <appid>_install.vdf; %INSTALLDIR% becomes installdir. Keys loaded, 0 if none. */
int steamreg_load(const char* vdf, const char* installdir);

void shim_RegOpenKeyExA(void);
void shim_RegCreateKeyExA(void);
void shim_RegQueryValueExA(void);
void shim_RegSetValueExA(void);
void shim_RegCloseKey(void);
