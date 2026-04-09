#include "../process.h"
#include <windows.h>
#include <string.h>
#include <tlhelp32.h>

pid_t platform_getpid(void) {
    return (pid_t)GetCurrentProcessId();
}

int platform_pid_is_program(pid_t pid, const char *progname) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 1;

    PROCESSENTRY32 pe = { .dwSize = sizeof(pe) };
    int found = 0;
    if (Process32First(snap, &pe)) {
        do {
            if ((pid_t)pe.th32ProcessID == pid) {
                /* Compare basename (exe name without path) */
                const char *base = strrchr(pe.szExeFile, '\\');
                base = base ? base + 1 : pe.szExeFile;
                /* Strip .exe suffix for comparison */
                char name[MAX_PATH];
                strncpy(name, base, sizeof(name) - 1);
                name[sizeof(name) - 1] = '\0';
                size_t len = strlen(name);
                if (len > 4 && _stricmp(name + len - 4, ".exe") == 0)
                    name[len - 4] = '\0';
                found = (_stricmp(name, progname) == 0);
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return found ? found : 1; /* not found → assume stale */
}
