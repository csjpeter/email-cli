# US-42 — Gmail Virtual Label Display Names

**As a** Gmail user navigating label views,
**I want to** see human-readable names ("Archive", "Trash", "Spam") in headers and status bars,
**so that** the interface never exposes internal virtual-label identifiers like `_nolabel`.

---

## Background

The local store uses synthetic virtual labels for special Gmail views:

| Internal ID  | Meaning                       |
|-------------|-------------------------------|
| `_nolabel`  | Archive (messages with no user label) |
| `_trash`    | Trash                         |
| `_spam`     | Spam / Junk                   |

These IDs must never appear in user-facing text.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | When viewing Archive (`_nolabel`), the header reads `Archive` — never `_nolabel`. |
| 2 | When viewing Trash (`_trash`), the header reads `Trash` — never `_trash`. |
| 3 | When viewing Spam (`_spam`), the header reads `Spam` — never `_spam`. |
| 4 | The status bar message count (e.g. `[1/5]`) and folder title both use the human name. |
| 5 | An empty Trash or Archive folder still shows the correct human name in the header. |
| 6 | The label browser list entry for each virtual label shows the human name ("Archive", "Trash", "Spam"), not the internal ID. |

---

## Example

Viewing an archived message:

```
  Labels — user@gmail.com > Archive  (1 message)

  Date              Labels         Subject              From
  ════════════════  ═════════════  ═══════════════════  ════════════
▶ 2026-04-16                      Quarterly review     Boss
```

**Not:**

```
  Labels — user@gmail.com > _nolabel  (1 message)   ← wrong
```

---

## Implementation notes

* `email_service.c` performs the mapping after loading `folder_display` from the
  local store: if the display name is empty or equals the internal ID, substitute
  the canonical human name.
* The canonical mapping lives in `email_service.c` near the `folder_display`
  assignment and applies whenever `cfg->gmail_mode` is true.

---

## Related

* US-28: Gmail Label Navigation
* US-29: Gmail Message Operations
* Spec: `docs/spec/gmail-api.md` — virtual label IDs
