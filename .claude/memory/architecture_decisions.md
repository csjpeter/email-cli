---
name: Architecture Decisions
description: Meghozott tervezési döntések indokaik és alkalmazási területük
type: project
---

## Makefile eltávolítva → manage.sh

**Döntés:** A Makefile törlésre került; `manage.sh` közvetlenül hívja a CMake-et.
**Why:** A manage.sh rugalmasabb bővítésre; a Makefile csak wrapper volt, duplikáció.
**How to apply:** Minden build/test parancs `./manage.sh <cmd>` formában fut.

## Dovecot a Mail-in-a-Box helyett

**Döntés:** Az integrációs teszt Dovecot IMAP szervert használ, nem Mail-in-a-Box-ot.
**Why:** MIAB systemd PID 1-et igényel, ~20 perces telepítés, nem Docker-natív.
Dovecot = MIAB IMAP komponense, de pár másodperc alatt indul.
**How to apply:** `./manage.sh integration` — Docker szükséges.

## genbadge a Codecov helyett

**Döntés:** Coverage badge önállóan generálódik lcov→Cobertura XML→SVG pipeline-on.
**Why:** Codecov a main ágra tokent kér (regisztráció), genbadge-hez semmi sem kell.
**How to apply:** Badge URL: `https://csjpeter.github.io/email-cli/coverage-badge.svg`

## Cross-platform portabilitás

**Döntés:** A projekt Linux mellett macOS, Windows és Android platformokra is céloz.
**Why:** A felhasználó explicit igénye: jövőbeli multi-platform support.
**How to apply:**
- Új platform-specifikus hívás előtt ellenőrizd a hordozhatósági táblázatot a CLAUDE.md-ben.
- Terminal I/O (raw mode, ablakméret, fd 0/1 olvasás) kerüljön egy `platform/` absztrakcióba — ne szóródjon a domain/core kódban.
- `__attribute__((cleanup(...)))`: GCC/Clang OK; MSVC-re MinGW vagy RAII-redesign kell.
- Android batch (nem-interaktív) módnak mindig működnie kell, TUI csak terminálemulátorban.
- Ismert portabilitási rések: `termios.h`, `ioctl TIOCGWINSZ`, `wcwidth`, `asprintf`, `iconv`, home dir útvonalak — részletek a CLAUDE.md Portability szekciójában.

## Minimális függőségek

**Döntés:** Csak C stdlib, POSIX, libcurl és libssl engedélyezett runtime függőségként.
**Why:** A felhasználó explicit elvként rögzítette: a függőségeket minimálisan kell tartani.
**How to apply:** Új könyvtár hozzáadása előtt mindig ki kell meríteni a stdlib/POSIX lehetőségeket. Új runtime függőség csak indoklással és felhasználói jóváhagyással kerülhet be.

## Repo-beli memória symlink-kel

**Döntés:** `.claude/memory/` a repóban van; `manage.sh memory-setup` symlinkeli
a Claude rendszer-memória helyére (`~/.claude/projects/.../memory/`).
**Why:** Más hostról is folytatható a munka — `git pull` + `manage.sh memory-setup`.
**How to apply:** Új gépen: `git clone ... && cd email-cli && ./manage.sh memory-setup`
