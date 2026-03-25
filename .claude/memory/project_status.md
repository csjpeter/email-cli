---
name: Project Status
description: Aktuális munka állapota, elvégzett feladatok, következő lépések
type: project
---

## Elvégzett feladatok

- Makefile eltávolítva; `manage.sh` mostantól közvetlenül CMake-et hív
- GitHub Actions CI pipeline: build + ASAN + Valgrind (`ci.yml`, `valgrind.yml`)
- Coverage pipeline: lcov → genbadge SVG badge → GitHub Pages deploy (`coverage.yml`)
- README badge-ek: CI, Valgrind, Coverage (%), License
- Unit tesztek bővítve 22 → 42 tesztre (81.5% lefedettség, korábban 73.4%)
- Két valódi memóriaszivárgás javítva (`logger_init`, `setup_wizard get_input`)
- Dovecot-alapú integrációs teszt környezet Docker Compose-zal (`tests/integration/`)
  - Perzisztens named volume, automatikus email seed, `--down`/`--clean` subcommand-ok

## Aktuális lefedettség részletezve

| Fájl | Lefedettség |
|---|---|
| `src/core/logger.c` | 99% |
| `src/infrastructure/curl_adapter.c` | 100% |
| `src/infrastructure/config_store.c` | 91% |
| `src/core/fs_util.c` | 92% |
| `src/infrastructure/setup_wizard.c` | 70% (stdin ágak nehezen tesztelhetők) |
| `src/domain/email_service.c` | 76% |
| `src/main.c` | 39% (alkalmazásréteg, integrációs teszt kell) |

**Why:** A `setup_wizard.c` stdin ágai (`if (stream == stdin)`) csak interaktív futáskor aktívak.
**How to apply:** A coverage növelése `main.c`-re integrációs teszttel lehetséges (Docker Dovecot env).

## Következő lehetséges lépések

- Integrációs teszt futtatása Dockerrel (szükséges: `docker` és `docker compose`)
- Coverage növelése `main.c`-re (integrációs teszt + email-cli stdout ellenőrzés)
- `email_service_fetch_recent` bővítése (jelenleg csak UID=1-et kér le)
