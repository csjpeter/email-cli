#include "../process.h"
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

pid_t platform_getpid(void) {
    return getpid();
}

int platform_pid_is_program(pid_t pid, const char *progname) {
    /* Check liveness with signal 0 (POSIX — Linux, macOS, Android) */
    if (kill(pid, 0) != 0)
        return 0;

#ifdef __linux__
    /* Verify executable name via /proc/<pid>/comm */
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return 1; /* can't verify — conservatively assume it matches */
    char comm[256] = {0};
    if (fgets(comm, sizeof(comm), f)) {
        fclose(f);
        /* strip trailing newline */
        size_t len = strlen(comm);
        while (len > 0 && (comm[len - 1] == '\n' || comm[len - 1] == '\r'))
            comm[--len] = '\0';
        return strcmp(comm, progname) == 0;
    }
    fclose(f);
    return 1; /* can't verify — conservatively assume it matches */
#else
    /* macOS / Android: process exists, assume it's our program.
     * A stale PID file from a crashed previous run is handled by the
     * caller checking the file's age or by the mismatch being rare. */
    (void)progname;
    return 1;
#endif
}
