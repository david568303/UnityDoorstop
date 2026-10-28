#include "../crt.h"
#include "../util/logging.h"
#include "il2cpp_resolver.h"

static volatile LONG preparation_state;

static BOOL current_map_is_valid(HMODULE module) {
    char alias[256];
    return doorstop_export_alias(module, "il2cpp_init", alias, sizeof(alias)) &&
           doorstop_export_alias(module, "il2cpp_runtime_invoke", alias, sizeof(alias)) &&
           doorstop_export_alias(module, "il2cpp_method_get_name", alias, sizeof(alias));
}

static BOOL safe_argument(const WCHAR *value) {
    if (!*value) return FALSE;
    for (; *value; ++value)
        if (*value == L'"' || *value == L'\r' || *value == L'\n') return FALSE;
    return TRUE;
}

/* Called from UnityPlayer's API binder, after LoadLibrary(GameAssembly) returns.
   Never called from DllEntry: starting/waiting for a helper under loader lock
   would be unsafe. Existing valid maps take this path without spawning anything. */
void doorstop_prepare_api_map(HMODULE module) {
    LONG previous;
    WCHAR *storage, *python, *helper, *resolved_python, *command;
    DWORD length, exit_code = 1, wait_result;
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    SECURITY_ATTRIBUTES security = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};
    HANDLE output = INVALID_HANDLE_VALUE, input = INVALID_HANDLE_VALUE;
    BOOL started;
    WCHAR refresh_option[8];
    if (!module || module != GetModuleHandleW(L"GameAssembly.dll")) return;
    if (GetProcAddress(module, "il2cpp_init")) return;
    previous = InterlockedCompareExchange(&preparation_state, 1, 0);
    if (previous) {
        while (InterlockedCompareExchange(&preparation_state, 0, 0) == 1) Sleep(10);
        return;
    }
    if (current_map_is_valid(module)) goto finished;
    GetPrivateProfileStringW(L"Compatibility", L"auto_refresh", L"false", refresh_option,
                             8, L".\\doorstop_config.ini");
    if (lstrcmpiW(refresh_option, L"true") && lstrcmpW(refresh_option, L"1")) goto finished;
    LOG("API map missing or stale; discovering bindings from native binaries");
    storage = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 6144 * sizeof(WCHAR));
    if (!storage) goto finished;
    python = storage;
    helper = storage + 1024;
    resolved_python = storage + 2048;
    command = storage + 3072;
    length = GetPrivateProfileStringW(L"Compatibility", L"python_executable", L"python.exe",
                                      python, 1024, L".\\doorstop_config.ini");
    if (!length || length >= 1023 || !safe_argument(python)) goto cleanup;
    length = GetPrivateProfileStringW(L"Compatibility", L"helper_script",
                                      L"BepInEx\\compatibility\\tools\\prepare_api_map.py",
                                      helper, 1024, L".\\doorstop_config.ini");
    if (!length || length >= 1023 || !safe_argument(helper)) goto cleanup;
    if (GetFileAttributesW(helper) == INVALID_FILE_ATTRIBUTES) {
        LOG("API refresh helper was not found");
        goto cleanup;
    }
    length = SearchPathW(NULL, python, NULL, 1024, resolved_python, NULL);
    if (!length || length >= 1024 || !safe_argument(resolved_python)) {
        LOG("Existing Python executable was not found; API map was not refreshed");
        goto cleanup;
    }
    /* No command shell. Both executable and script are separately quoted. */
    lstrcpyW(command, L"\"");
    lstrcatW(command, resolved_python);
    lstrcatW(command, L"\" -B -X utf8 \"");
    lstrcatW(command, helper);
    lstrcatW(command, L"\" --unity UnityPlayer.dll --game GameAssembly.dll --output doorstop_api.ini --json BepInEx\\cache\\compatibility\\api-map.json");
    CreateDirectoryW(L"BepInEx", NULL);
    CreateDirectoryW(L"BepInEx\\cache", NULL);
    CreateDirectoryW(L"BepInEx\\cache\\compatibility", NULL);
    output = CreateFileW(L"BepInEx\\cache\\compatibility\\api-refresh.log", GENERIC_WRITE,
                         FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (output == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) goto cleanup;
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = output;
    startup.hStdError = output;
    startup.hStdInput = input;
    started = CreateProcessW(resolved_python, command, NULL, NULL, TRUE,
                              CREATE_NO_WINDOW, NULL, NULL, &startup, &process);
    if (!started) {
        LOG("Could not start API refresh helper (Windows error %lu)", GetLastError());
        goto cleanup;
    }
    CloseHandle(process.hThread);
    wait_result = WaitForSingleObject(process.hProcess, 90000);
    if (wait_result == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &exit_code);
    else {
        TerminateProcess(process.hProcess, 1);
        LOG("API refresh helper timed out or could not be waited for");
    }
    CloseHandle(process.hProcess);
    WritePrivateProfileStringW(NULL, NULL, NULL, L".\\doorstop_api.ini");
    if (!exit_code && current_map_is_valid(module)) {
        LOG("API bindings refreshed and validated successfully");
    } else {
        LOG("API refresh failed; original game API calls are preserved. See BepInEx/cache/compatibility/api-refresh.log");
    }
cleanup:
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    HeapFree(GetProcessHeap(), 0, storage);
finished:
    InterlockedExchange(&preparation_state, 2);
}
