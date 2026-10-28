/*
 * IL2CPP compatibility changes by David Salas Kalendar, 2026-10-28.
 * Based on UnityDoorstop; distributed under the GNU LGPL 2.1 (see LICENSE).
 * See COMPATIBILITY.md and CHANGES.md for the changes and upstream revision.
 */
#include "il2cpp_resolver.h"

static BOOL parse_hex(const char *text, DWORD *value) {
    DWORD result = 0;
    unsigned int count = 0;
    for (; *text; ++text) {
        unsigned int digit;
        if (*text >= '0' && *text <= '9') digit = *text - '0';
        else if (*text >= 'a' && *text <= 'f') digit = *text - 'a' + 10;
        else if (*text >= 'A' && *text <= 'F') digit = *text - 'A' + 10;
        else return FALSE;
        if (++count > 8) return FALSE;
        result = (result << 4) | digit;
    }
    if (!count) return FALSE;
    *value = result;
    return TRUE;
}

BOOL doorstop_export_alias(HMODULE module, const char *canonical,
                           char *alias, DWORD capacity) {
    char value[256], stamp_text[32], size_text[32], discovery[64];
    DWORD stamp, size, expected_rva, length;
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    unsigned int separator = 0;
    if (!module || !canonical || !HIWORD(canonical) || !alias || capacity < 2) return FALSE;
    /* Scope aliases to the game runtime; unrelated DLL lookups stay intact. */
    if (module != GetModuleHandleW(L"GameAssembly.dll")) return FALSE;
    /* Retire maps produced by the previous discovery implementation, even if
       this game build is unchanged, so upgrading performs fresh discovery. */
    if (GetPrivateProfileIntA("Target", "Format", 0, ".\\doorstop_api.ini") != 2) return FALSE;
    GetPrivateProfileStringA("Target", "Discovery", "", discovery, sizeof(discovery), ".\\doorstop_api.ini");
    if (lstrcmpA(discovery, "binary-loader-schema")) return FALSE;
    if (!GetPrivateProfileStringA("Target", "TimeDateStamp", "", stamp_text,
                                  sizeof(stamp_text), ".\\doorstop_api.ini") ||
        !GetPrivateProfileStringA("Target", "SizeOfImage", "", size_text,
                                  sizeof(size_text), ".\\doorstop_api.ini") ||
        !parse_hex(stamp_text, &stamp) || !parse_hex(size_text, &size)) return FALSE;
    dos = (IMAGE_DOS_HEADER *)module;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) return FALSE;
    nt = (IMAGE_NT_HEADERS *)((BYTE *)module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.TimeDateStamp != stamp || nt->OptionalHeader.SizeOfImage != size) return FALSE;
    length = GetPrivateProfileStringA("Exports", canonical, "", value,
                                      sizeof(value), ".\\doorstop_api.ini");
    if (!length || length >= sizeof(value) - 1) return FALSE;
    while (separator < length && value[separator] != ',') ++separator;
    if (!separator || separator >= capacity || separator == length) return FALSE;
    value[separator] = 0;
    if (!parse_hex(value + separator + 1, &expected_rva) || expected_rva >= size) return FALSE;
    if (GetProcAddress(module, value) != (FARPROC)((BYTE *)module + expected_rva)) return FALSE;
    for (unsigned int i = 0; i <= separator; ++i) alias[i] = value[i];
    return TRUE;
}

FARPROC doorstop_resolve_export(HMODULE module, const char *canonical) {
    char alias[256];
    if (!canonical) return NULL;
    FARPROC address = GetProcAddress(module, canonical);
    if (address || !HIWORD(canonical)) return address;
    if (doorstop_export_alias(module, canonical, alias, sizeof(alias)))
        return GetProcAddress(module, alias);
    return NULL;
}

FARPROC WINAPI doorstop_get_proc_address(HMODULE module, LPCSTR name) {
    return doorstop_resolve_export(module, name);
}
