/*
 * IL2CPP compatibility changes by David Salas Kalendar, 2026-10-28.
 * Based on UnityDoorstop; distributed under the GNU LGPL 2.1 (see LICENSE).
 * See COMPATIBILITY.md and CHANGES.md for the changes and upstream revision.
 */
#ifndef DOORSTOP_IL2CPP_RESOLVER_H
#define DOORSTOP_IL2CPP_RESOLVER_H
#include <windows.h>

/* Map discovered from the paired native binaries, never from fixed offsets. */
BOOL doorstop_export_alias(HMODULE module, const char *canonical,
                           char *alias, DWORD capacity);
FARPROC doorstop_resolve_export(HMODULE module, const char *canonical);
FARPROC WINAPI doorstop_get_proc_address(HMODULE module, LPCSTR name);
void doorstop_prepare_api_map(HMODULE module);
#endif
