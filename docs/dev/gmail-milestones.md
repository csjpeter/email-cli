# Gmail Implementation Milestones (GML-01 – GML-24)

This document tracks the Gmail native API implementation milestones.
Each milestone maps to one or more git commits and corresponds to sections
of the `docs/spec/gmail-api.md` specification.

---

## Phase 1 — Foundation (GML-01 – GML-04)

These milestones were part of the initial planning and infrastructure work.
They predate the GML tagging convention in commit messages.

| ID | Description | Commit | Spec section |
|----|------------|--------|-------------|
| GML-01 | Gmail API specification (initial draft) | `51a344b` | All |
| GML-02 | Gmail API specification (complete with TUI/UX sections) | `4388cc2` | §4–§16 |
| GML-03 | Config fields (`GMAIL_MODE`), libcurl in libemail | `a6f504e` | §1 |
| GML-04 | Custom JSON parser (`json_util.c`) | `a6f504e` | (infrastructure) |

---

## Phase 2 — Core API (GML-05 – GML-08)

| ID | Description | Commit | Spec section |
|----|------------|--------|-------------|
| GML-05 | OAuth2 device-flow and token refresh | `e5af6a4` | §2 |
| GML-06 | Gmail REST API client (messages, labels, modify, trash, send) | `f69b904` | §9 |
| GML-07 | Label index files (`.idx`) with binary search | `f16ce68` | §7 |
| GML-08 | Full and incremental sync via History API | `fc8abff` | §8 |

---

## Phase 3 — Dispatch & Integration (GML-09 – GML-12)

| ID | Description | Commit | Spec section |
|----|------------|--------|-------------|
| GML-09 | Unified `mail_client` dispatch layer | `e06d198` | §10 |
| GML-10 | `email_service.c` refactored to use `mail_client` | `1faeea2` | §10 |
| GML-11 | Send dispatch — Gmail API vs SMTP | `f08765f` | §16 |
| GML-12 | Setup wizard with account type selection | `7732435` | §11 |

---

## Phase 4 — Documentation (GML-13 – GML-15)

| ID | Description | Commit | Spec section |
|----|------------|--------|-------------|
| GML-13 | CLAUDE.md update for Gmail support | `25260a8` | — |
| GML-14 | Project memory update | `25260a8` | — |
| GML-15 | Architecture documentation | `25260a8` | — |

---

## Phase 5 — TUI & UX (GML-16 – GML-24)

| ID | Description | Commit | Spec section |
|----|------------|--------|-------------|
| GML-16 | Account list with Type column (IMAP/Gmail) | `fc818bb` | §12 |
| GML-17 | Label list view (3-section layout) | `fc818bb` | §4 |
| GML-18 | Message list with Labels column | `fc818bb` | §5 |
| GML-19 | Message reader with Labels row | `fc818bb` | §15 |
| GML-20 | Gmail-specific key bindings (r/d/a/f/n) | `fc818bb`, `d2bcf01` | §5 |
| GML-21 | Interactive label picker (t key) | `fc818bb` | §13 |
| GML-22 | base64url encoding tests | `d2bcf01` | §9 |
| GML-23 | Status bar adaptation for Gmail views | `fc818bb` | §14 |
| GML-24 | Usage guidelines and documentation | `d2bcf01` | — |

---

## Post-milestone fixes

| Commit | Description |
|--------|------------|
| `14bb56f` | Server-side archive and label modify via Gmail API |
| `2e9519b` | Gmail UX fixes, untrash restore, 35 new tests |

---

## User stories

| Milestone range | User story |
|----------------|-----------|
| GML-05, GML-12 | [US-27: Gmail Account Setup](../userstories/27-gmail-account-setup.md) |
| GML-16, GML-17 | [US-28: Gmail Label Navigation](../userstories/28-gmail-label-navigation.md) |
| GML-18–21 | [US-29: Gmail Message Operations](../userstories/29-gmail-message-operations.md) |
| GML-21, GML-23 | [US-30: Gmail Label Picker](../userstories/30-gmail-label-picker.md) |
| GML-07, GML-08 | [US-31: Gmail Synchronization](../userstories/31-gmail-sync.md) |
| GML-11 | [US-32: Gmail Compose and Send](../userstories/32-gmail-compose-send.md) |
