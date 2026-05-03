# US-64 — Import Rules: Warn on Unsupported Thunderbird Elements

**As a** user importing Thunderbird filters,
**I want to** be told which parts of a rule could not be converted,
**so that** I can manually handle the rules that the importer cannot translate.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | For each unsupported condition field (e.g. `body`, `age`, `date`) a `[warn]` line is printed on stderr. |
| 2 | For each unsupported match type (e.g. `doesn't contain`, `begins with`) a `[warn]` line is printed on stderr. |
| 3 | For each unsupported action (e.g. `Label`, `Mark as read`, `Reply`) a `[warn]` line is printed on stderr. |
| 4 | A rule where no condition and no action could be converted produces an additional `[warn]` summarising that it "will be empty". |
| 5 | Successfully converted rules are NOT warned about. |
| 6 | Warnings do not prevent the successfully converted rules from being saved. |
| 7 | `--dry-run` shows the warnings without writing any file. |

---

## Warning format

```
  [warn] Rule "Body filter": condition field "body" is not supported (only from/subject/to), skipping term
  [warn] Rule "Body filter": action "Label" is not supported, skipping
  [warn] Rule "Body filter": no conditions or actions could be converted — rule will be empty
```

---

## Related

* US-63: binary availability
* US-65–US-70: implementing support for currently-warned-about features
