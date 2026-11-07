/*
 * Native IL2CPP discovery by David Salas Kalendar, 2026-11-07.
 * Distributed under the GNU LGPL 2.1; see LICENSE.
 */
#ifndef DOORSTOP_NATIVE_API_H
#define DOORSTOP_NATIVE_API_H

#include <windows.h>

#define NATIVE_API_CAPACITY 256

typedef struct {
    const char *canonical;
    const char *actual;
    DWORD rva;
    FARPROC address;
} NativeApiBinding;

typedef struct {
    DWORD timestamp;
    DWORD image_size;
    size_t count;
    NativeApiBinding entries[NATIVE_API_CAPACITY];
} NativeApiMap;

const NativeApiBinding *doorstop_find_native_api(HMODULE module, const char *canonical);

#ifdef DOORSTOP_NATIVE_API
BOOL doorstop_discover_native_apis(HMODULE unity, HMODULE game, LPCWSTR decoder_path,
                                  NativeApiMap *result, const char **failure);
#endif

#endif
