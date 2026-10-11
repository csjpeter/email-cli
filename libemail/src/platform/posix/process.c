/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-04-09 */
/* POSIX process identity helpers. */
#include "../process.h"
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

pid_t platform_getpid(void) {
    return getpid();
}

int platform_pid_is_program(pid_t pid, const char *progname) {
    /* Check liveness with signal 0 (POSIX) */
    if (kill(pid, 0) != 0)
        return 0;

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
}
