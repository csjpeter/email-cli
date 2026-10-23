# Portability: what binds this project to Linux

The target platform is **Linux (Debian)**. A release for, or support of, macOS,
iOS, Android or Windows is **not a goal**; nothing here promises it, and no
build, package or CI job exists for them (`../policies/platform-scope-model.md`).

This file is a list of what is waiting, not a plan. It exists so that the day
another platform becomes a target, the work is a known list.

## Isolation points

Platform-specific code lives in one place, `libemail/src/platform/`: a plain
header per concern and its implementation in `platform/posix/`, selected by
`libemail/CMakeLists.txt`. No platform `#ifdef` exists in the source; there is
no code branch for a platform nobody builds on.

## Dependencies on Linux

| Dependency | Where | Would need |
|---|---|---|
| `termios` raw mode, `ioctl(TIOCGWINSZ)`, `wcwidth(3)` | `libemail/src/platform/posix/terminal.c`, `html_medium.c` | a console-API implementation of `platform/terminal.h` and a bundled `wcwidth` |
| `$HOME`, `getpwuid`, XDG base directories | `libemail/src/platform/posix/path.c` | per-OS application directories behind `platform/path.h` |
| `readlink("/proc/self/exe")` | `libemail/src/platform/posix/path.c` | a per-OS executable-path call |
| `/proc/<pid>/comm`, `kill(pid, 0)` for PID-file liveness | `libemail/src/platform/posix/process.c` | a per-OS process-identity query behind `platform/process.h` |
| `/etc/machine-id`, `~/.ssh/id_*` as credential key source | `libemail/src/platform/posix/credential_key.c` | a per-OS machine identifier behind `platform/credential_key.h` |
| `flock(2)`, `getuid`/`getpwuid` for the append-only operation history (who, serialised writes) | `libemail/src/infrastructure/op_history.c` | a per-OS file lock and user identity behind a platform header |
| `fopencookie(3)` (glibc) for the plain-output filter | `libemail/src/platform/posix/terminal.c` | a per-OS stream wrapper |
| `open_memstream(3)` | `libemail/src/platform/posix/html_medium.c` | a `tmpfile`-based replacement |
| `forkpty`/`<pty.h>`, `-lutil` | `deps/libptytest/`, `tests/pty/` | a ConPTY or per-OS pty layer (test harness only) |
| `crontab` via `popen` (scheduled sync) | `libemail/src/domain/email_service.c` | a per-OS scheduler (launchd, Task Scheduler) |
| static glibc: `getaddrinfo` reads `/etc/hosts` and `/etc/resolv.conf` through the NSS modules `files` and `dns` built into glibc 2.34+; other `nsswitch` modules (mdns, systemd-resolved's) are not available, and `iconv` finds only the built-in character sets without the system's gconv modules (`libemail/src/core/charset.c` carries the common single-byte ones) | `src/`, `libemail-client/` (fully static link, EMAIL-103) | a different libc (musl) would need the same checks |
| CA bundle paths of Debian, RHEL, openSUSE, Alpine | `libemail-client/src/net_tls.c` | a per-OS certificate store |
| `xdg-open` | `libemail-client/src/gmail_auth.c` | a per-OS URL launcher |
| `fork`, `mkdir(path, mode)`, `rename` over an existing file, `strptime`, `timegm`, `strcasecmp`, `asprintf`, `iconv(3)` | `libemail/src/core/`, `infrastructure/` | shims or libraries on a non-POSIX or non-glibc libc |
| `__attribute__((cleanup))` RAII | `libemail/src/core/raii.h` | none on GCC or Clang; MSVC cannot build it |
| `./manage.sh deps` via `apt` or `dnf`, `dpkg`, CPack DEB and RPM, local Dovecot | `manage.sh`, `CMakeLists.txt`, `tests/integration/` | none: the build and test host is Linux regardless |
