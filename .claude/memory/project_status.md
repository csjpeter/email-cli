---
name: Project Status
description: Aktuális coverage számok, kész feladatok, következő lépések (2026-03-26)
type: project
---

# Project Status (2026-03-26)

## CI Status: ALL GREEN ✅
All 3 workflows passing on main:
- CI (build + ASAN + Valgrind)
- Valgrind
- Coverage → GitHub Pages badge

## Coverage (last run)
| Layer | File | Lines |
|-------|------|-------|
| Core | logger.c | 67/68 = 98.5% |
| Core | fs_util.c | 23/25 = 92% |
| Core | raii.h | 18/18 = 100% |
| Infra | config_store.c | 63/69 = 91.3% |
| Infra | curl_adapter.c | 34/34 = 100% |
| Infra | setup_wizard.c | 37/43 = 86% |
| **Core+Infra total** | | **242/257 = 94.2% ✅** |
| Domain | email_service.c | 71/86 = 82.6% |
| App | main.c | 22/56 = 39.3% |
| **Overall** | | **84.7%** |

**Why setup_wizard.c is 86%:** TTY branches (tcgetattr/tcsetattr, hide=1+is_tty)
require a PTY (openpty) to test. 6 uncovered lines are interactive-terminal only.

## Completed Features
- 4-layer CLEAN architecture (Core/Infra/Domain/App)
- RAII memory safety (RAII_STRING, RAII_CURL, RAII_FILE, RAII_SLIST)
- IMAP email fetch: UID SEARCH ALL + per-message FETCH, shows last 10
- TLS 1.2+ enforcement (CURL_SSLVERSION_TLSv1_2), ssl_no_verify for tests
- CURL debug logging forwarding all IMAP traffic to logger at DEBUG level
- Config store (~/.config/email-cli/config.ini, mode 0600)
- Setup wizard (interactive + testable via FILE* stream)
- Log rotation (5 files x 5MB), --clean-logs CLI
- Unit tests: 53 tests, ASAN + Valgrind clean
- Functional tests: mock IMAP server (multi-connection), 5 output assertions
- Integration tests: Dockerized Dovecot with real TLS 1.2+
- GitHub Actions: CI / Valgrind / Coverage → GitHub Pages
- Badges: CI, Valgrind, Coverage (genbadge SVG), License (GPLv3)
- Doxygen comments on all public and internal functions

## Remaining Gaps / Next Logical Steps
1. **setup_wizard.c TTY coverage** — use openpty to cover tcgetattr/tcsetattr lines,
   OR mark with `/* LCOV_EXCL_LINE */` to exclude from stats
2. **email_service.c coverage** — add unit tests for parse_uid_list edge cases
   (empty response, no SEARCH line, multiple UIDs, UID overflow)
3. **main.c coverage** — needs integration-style test running the binary end-to-end
4. **--folder CLI flag** — allow overriding folder without editing config
5. **--count N flag** — make EMAIL_FETCH_RECENT configurable at runtime
