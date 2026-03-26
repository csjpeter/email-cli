---
name: Workflow Handbook
description: Step-by-step checklist for Claude to follow when completing any task in this project — build, test, verify CI, and document.
type: feedback
---

# Workflow Handbook

This handbook defines the mandatory steps to complete any non-trivial change in this project.
Follow these in order. Never skip CI verification.

---

## 1. Local Verification (before committing)

Run these in order. Fix every failure before moving on.

```bash
./manage.sh build      # Release build must be clean (no warnings)
./manage.sh test       # 52+ unit tests must pass, 0 ASAN errors
./manage.sh valgrind   # 0 Valgrind errors, 0 leaks
./manage.sh coverage   # core+infra coverage must stay >90%
                       # functional test assertions must all PASS
```

**Why:** -Werror makes any warning a build failure. ASAN and Valgrind catch
different classes of bugs. Coverage protects core/infra quality.

---

## 2. Check for Uncommitted Changes

```bash
git diff --name-only HEAD
git status
```

**Why:** Previous sessions showed that file edits often remain unstaged.
Headers (.h files), integration files, and new source files are easily
forgotten. Always stage every changed file explicitly by name — never use
`git add -A` to avoid accidentally including .gcda, coverage_report/, etc.

Files to always check:
- `src/**/*.h` — headers with struct/signature changes
- `tests/integration/**` — Dockerfile, docker-compose, configs
- `tests/functional/**` — mock server, run scripts

---

## 3. Commit

- Stage only relevant files by name
- Write a clear commit message: one subject line + bullet points for each file changed
- Always add `Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>`

```bash
git add <specific files>
git commit -m "$(cat <<'EOF'
Subject line (imperative, <70 chars)

- file1: what changed and why
- file2: what changed and why

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## 4. Push and Verify CI

```bash
git push
sleep 15
gh run list --limit 3
```

If workflows are still `in_progress`, wait longer:
```bash
sleep 30 && gh run list --limit 3
```

All 3 workflows must be `completed success`:
- **CI** — build (Release) + ASAN unit tests + Valgrind
- **Valgrind** — dedicated Valgrind run
- **Coverage** — coverage report + genbadge badge → GitHub Pages

If any workflow fails:
```bash
gh run view <run-id> --log-failed
```
Fix the error, commit the fix, push again. Repeat until green.

**Why:** CI runs on a clean Ubuntu environment without local state.
Past failures: missing committed headers (.h files), stale gcda files
causing checksum errors, functional test lacking assertions.

---

## 5. Determine Next Steps

After CI is green, consult in this order:

1. **`.claude/memory/project_status.md`** — current task status and backlog
2. **`doc/*.md`** — requirements that must be fulfilled:
   - `doc/unit_testing.md` → >90% core+infra coverage, ASAN + Valgrind clean
   - `doc/functional_testing.md` → mock server handles all connections, output assertions
   - `doc/logging.md` → DEBUG level logs IMAP traffic via CURL callback
   - `doc/design.md` → Doxygen on all public functions, CLEAN layer discipline
   - `doc/concepts.md` + `doc/raii.md` → RAII macros used consistently
3. **`README.md`** — badges should reflect actual CI status
4. **`CLAUDE.md`** — project-level guidance from the user

---

## 6. Quality Gates (never regress these)

| Check | Target | How to measure |
|-------|--------|----------------|
| Unit tests | All pass | `./manage.sh test` |
| ASAN | 0 errors | `./manage.sh test` |
| Valgrind | 0 errors, 0 leaks | `./manage.sh valgrind` |
| Core+Infra coverage | >90% lines | `./manage.sh coverage` |
| Functional assertions | All PASS | `./manage.sh coverage` (runs functional) |
| CI workflows | All green | `gh run list --limit 3` |

---

## 7. Common Pitfalls

| Pitfall | Fix |
|---------|-----|
| `Config has no member ssl_no_verify` | Uncommitted `config_store.h` — stage it |
| `libgcov: overwriting profile data` | `find build -name "*.gcda" -delete` (already in manage.sh coverage) |
| Functional test `[FAIL]` in CI | Check mock server handles multiple connections (no break after first client) |
| Valgrind `uninit value` | Check struct init: use `= {0}` for stack-allocated Config |
| `ignoring return value of write` | Capture return: `ssize_t n = write(...); ASSERT(n > 0, ...)` |
| CI fails but local passes | Missing committed file — check `git diff --name-only HEAD` |
