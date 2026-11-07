/*
 * Native IL2CPP resolution by David Salas Kalendar, 2026-11-07.
 * Distributed under the GNU LGPL 2.1; see LICENSE.
 */
#include "il2cpp_resolver.h"
#include "native_api.h"

BOOL doorstop_export_alias(HMODULE module, const char *canonical,
                           char *alias, DWORD capacity) {
    if (!alias || capacity < 2) return FALSE;
    const NativeApiBinding *binding = doorstop_find_native_api(module, canonical);
    if (!binding) return FALSE;
    size_t length = (size_t)lstrlenA(binding->actual);
    if (length >= capacity) return FALSE;
    for (size_t i = 0; i <= length; ++i) alias[i] = binding->actual[i];
    return TRUE;
}

FARPROC doorstop_resolve_export(HMODULE module, const char *canonical) {
    if (!module || !canonical) return NULL;
    FARPROC address = GetProcAddress(module, canonical);
    if (address || !HIWORD(canonical)) return address;
    const NativeApiBinding *binding = doorstop_find_native_api(module, canonical);
    return binding ? binding->address : NULL;
}

FARPROC WINAPI doorstop_get_proc_address(HMODULE module, LPCSTR name) {
    return doorstop_resolve_export(module, name);
}
