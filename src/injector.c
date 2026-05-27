/*
 * omp-wine-injector — a small Win32 DLL injector for running the open.mp / SA-MP
 * client inside a Wine/CrossOver bottle on macOS.
 *
 * The official open.mp launcher (Windows) injects the client DLLs at runtime
 * with CreateRemoteThread + LoadLibrary (via the `dll_syringe` crate). That is
 * impossible to do from a *native macOS* process against a *Windows* process
 * living inside Wine — the Win32 process APIs only work Windows-to-Windows.
 *
 * So this tool is the same logic, compiled as a tiny Windows .exe that the macOS
 * launcher runs *inside the bottle* (via cxstart). It is a 1:1 port of the
 * launcher's inject_dll: spawn the game, then inject each DLL with the same
 * vorbis-readiness gate and retry/back-off fallbacks.
 *
 * Usage:
 *   injector.exe --spawn "<game.exe>" "<args>" -- <dll1> [dll2 ...]
 *   injector.exe --pid <pid> -- <dll1> [dll2 ...]
 *
 * Exit codes: 0 all injected · 1 usage · 2 spawn failed · 3 injection failed.
 */
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>

/* Mirror the launcher's constants (launcher constants.rs). */
#define INJECTION_MAX_RETRIES        60
#define INJECTION_RETRY_DELAY_MS     250
#define PROCESS_MODULE_BUFFER_SIZE   1024
#define MODULE_NAME_BUFFER           1024

static void logf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* Is any module whose path contains `needle` loaded in the target yet?
 * Mirrors the launcher's "wait until vorbisFile.dll appears" readiness gate. */
static BOOL module_loaded(HANDLE proc, const char *needle) {
    HMODULE mods[PROCESS_MODULE_BUFFER_SIZE];
    DWORD needed = 0;
    if (!EnumProcessModulesEx(proc, mods, sizeof(mods), &needed, LIST_MODULES_ALL))
        return FALSE;
    DWORD count = needed / sizeof(HMODULE);
    if (count > PROCESS_MODULE_BUFFER_SIZE) count = PROCESS_MODULE_BUFFER_SIZE;
    char name[MODULE_NAME_BUFFER];
    for (DWORD i = 0; i < count; i++) {
        if (GetModuleFileNameExA(proc, mods[i], name, sizeof(name))) {
            /* case-insensitive contains */
            char low[MODULE_NAME_BUFFER];
            size_t n = 0;
            for (; name[n] && n < sizeof(low) - 1; n++)
                low[n] = (char)tolower((unsigned char)name[n]);
            low[n] = 0;
            if (strstr(low, needle)) return TRUE;
        }
    }
    return FALSE;
}

/* Classic CreateRemoteThread + LoadLibraryA injection of one DLL. */
static BOOL inject_once(HANDLE proc, const char *dll_path) {
    SIZE_T len = strlen(dll_path) + 1;
    LPVOID remote = VirtualAllocEx(proc, NULL, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { logf("[inject] VirtualAllocEx failed: %lu", GetLastError()); return FALSE; }

    if (!WriteProcessMemory(proc, remote, dll_path, len, NULL)) {
        logf("[inject] WriteProcessMemory failed: %lu", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return FALSE;
    }

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    FARPROC load = GetProcAddress(k32, "LoadLibraryA");
    HANDLE th = CreateRemoteThread(proc, NULL, 0, (LPTHREAD_START_ROUTINE)load, remote, 0, NULL);
    if (!th) {
        logf("[inject] CreateRemoteThread failed: %lu", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return FALSE;
    }
    WaitForSingleObject(th, 15000);
    DWORD ret = 0;
    GetExitCodeThread(th, &ret);            /* low word of the loaded HMODULE */
    CloseHandle(th);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    if (ret == 0) { logf("[inject] LoadLibraryA returned 0 (load failed) for %s", dll_path); return FALSE; }
    return TRUE;
}

/* Port of inject_dll(child, dll, times, waiting_for_vorbis):
 *  - first try a direct inject;
 *  - on failure, switch to the vorbis-readiness path: wait until the game's
 *    vorbisFile module is loaded (i.e. it is far enough into init to accept a
 *    LoadLibrary), then inject;
 *  - back off INJECTION_RETRY_DELAY_MS between attempts, up to MAX_RETRIES. */
static BOOL inject_dll(DWORD pid, const char *dll_path) {
    HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!proc) { logf("[inject] OpenProcess(%lu) failed: %lu", pid, GetLastError()); return FALSE; }

    BOOL waiting_for_vorbis = FALSE;
    for (unsigned times = 0; times < INJECTION_MAX_RETRIES; times++) {
        if (waiting_for_vorbis && !module_loaded(proc, "vorbis")) {
            Sleep(INJECTION_RETRY_DELAY_MS);
            continue;                       /* not ready; keep waiting */
        }
        if (inject_once(proc, dll_path)) {
            logf("[inject] OK: %s (attempt %u)", dll_path, times + 1);
            CloseHandle(proc);
            return TRUE;
        }
        /* First failure flips us into the vorbis-readiness wait, exactly like
         * the launcher's `return inject_dll(child, dll, 0, true)`. */
        waiting_for_vorbis = TRUE;
        Sleep(INJECTION_RETRY_DELAY_MS);
    }
    logf("[inject] FAILED after %d attempts: %s", INJECTION_MAX_RETRIES, dll_path);
    CloseHandle(proc);
    return FALSE;
}

int main(int argc, char **argv) {
    DWORD pid = 0;
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    int dll_start = -1;

    /* Parse: --spawn <exe> <args> -- <dlls...>  |  --pid <pid> -- <dlls...> */
    if (argc >= 4 && strcmp(argv[1], "--spawn") == 0) {
        const char *exe = argv[2];
        const char *args = argv[3];
        char cmdline[2048];
        snprintf(cmdline, sizeof(cmdline), "\"%s\" %s", exe, args);
        char workdir[1024]; strncpy(workdir, exe, sizeof(workdir) - 1); workdir[sizeof(workdir)-1]=0;
        char *slash = strrchr(workdir, '\\'); if (slash) *slash = 0;

        STARTUPINFOA si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
        if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, workdir, &si, &pi)) {
            logf("[injector] CreateProcess failed: %lu", GetLastError());
            return 2;
        }
        pid = pi.dwProcessId;
        logf("[injector] spawned pid=%lu: %s", pid, cmdline);
        /* find the "--" separator */
        for (int i = 4; i < argc; i++) if (strcmp(argv[i], "--") == 0) { dll_start = i + 1; break; }
    } else if (argc >= 4 && strcmp(argv[1], "--pid") == 0) {
        pid = (DWORD)strtoul(argv[2], NULL, 10);
        for (int i = 3; i < argc; i++) if (strcmp(argv[i], "--") == 0) { dll_start = i + 1; break; }
    } else {
        logf("usage: injector.exe --spawn \"<exe>\" \"<args>\" -- <dll1> [dll2 ...]");
        logf("       injector.exe --pid <pid> -- <dll1> [dll2 ...]");
        return 1;
    }

    if (dll_start < 0 || dll_start >= argc) {
        logf("[injector] no DLLs given after '--'");
        if (pi.hProcess) { ResumeThread(pi.hThread); CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
        return 1;
    }

    int rc = 0;
    for (int i = dll_start; i < argc; i++) {
        if (!inject_dll(pid, argv[i])) rc = 3;   /* keep going; report failure */
    }

    if (pi.hProcess) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    return rc;
}
