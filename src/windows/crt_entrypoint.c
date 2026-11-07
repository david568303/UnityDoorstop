/*
 * Standard MSVC static CRT startup bridge, 2026-11-07. LGPL 2.1; see LICENSE.
 * The normal CRT entry point initializes security cookies before loader code.
 */
#if defined(_MSC_VER) && defined(DOORSTOP_STANDARD_CRT)
#include <windows.h>
BOOL WINAPI DllEntry(HINSTANCE instance, DWORD reason, LPVOID reserved);
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    return DllEntry(instance, reason, reserved);
}
#endif
