#ifndef PLATFORM_PROCESS_H
#define PLATFORM_PROCESS_H

#include <sys/types.h>

/**
 * @file process.h
 * @brief Platform abstraction for process identity helpers.
 */

/**
 * @brief Returns the calling process's PID.
 */
pid_t platform_getpid(void);

/**
 * @brief Checks whether a process with the given PID is currently running
 *        AND its executable name matches @p progname.
 *
 * Uses kill(pid, 0) to test liveness, then reads the process name from the
 * OS (e.g. /proc/<pid>/comm on Linux).  If the name cannot be determined,
 * the function conservatively returns 1 (assume it matches).
 *
 * @param pid      PID to check.
 * @param progname Expected program name (basename, not full path).
 * @return 1 if the process is alive and matches @p progname, 0 otherwise.
 */
int platform_pid_is_program(pid_t pid, const char *progname);

#endif /* PLATFORM_PROCESS_H */
