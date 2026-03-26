---
name: Commit Discipline — Stage Every Changed File
description: Always verify uncommitted files before pushing; headers and integration files are silently left out, breaking CI on a clean checkout.
type: feedback
---

Always run `git diff --name-only HEAD` before pushing. Stage every changed file by name.

**Why:** In the 2026-03-26 session, 7 files from the previous session had never been
committed: `config_store.h` (ssl_no_verify field), `curl_adapter.h` (verify_peer param),
`email_service.c` (full UID SEARCH rewrite), and all 4 integration test files. Local builds
passed because the files existed on disk. CI checked out the repo fresh and failed with
`'Config' has no member named 'ssl_no_verify'` — a clean-room failure invisible locally.

**How to apply:** After any multi-file change, run `git diff --name-only HEAD` and
`git status` before pushing. Be especially suspicious of: `.h` headers (struct/signature
changes), `tests/integration/**` files, and any file touched in a previous session that
"seemed done." Never use `git add -A` — stage files by name to stay aware of what's going in.
