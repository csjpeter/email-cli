# US-63 — Import Rules: Binary Available in bin/ and Install

**As a** user who wants to import Thunderbird filters,
**I want to** have `email-import-rules` available in `bin/` after build and installed by `./manage.sh install`,
**so that** I can run it from the same directory as the other tools.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | `./manage.sh build` copies `email-import-rules` to `bin/` alongside the other binaries. |
| 2 | `./manage.sh install` installs `email-import-rules` to `~/.local/bin`. |
| 3 | `./manage.sh uninstall` removes `email-import-rules` from `~/.local/bin`. |
| 4 | `email-import-rules --help` prints a usage page describing all options. |
| 5 | `email-import-rules --version` prints the version string. |

---

## Related

* US-64: warn on unsupported Thunderbird rules
* US-65–US-70: extended import support
