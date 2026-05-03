# US-67 — Import Rules: Thunderbird Label Action → Custom Label

**As a** user importing Thunderbird filters that use coloured labels,
**I want** the `Label` action to be converted to an email-cli `then-add-label` with the label's name,
**so that** message categorisation rules survive the import.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | `action=Label` with `actionValue=$label1` is converted to `then-add-label = Important`. |
| 2 | `action=Label` with `actionValue=$label2` is converted to `then-add-label = Work`. |
| 3 | `action=Label` with `actionValue=$label3` is converted to `then-add-label = Personal`. |
| 4 | `action=Label` with `actionValue=$label4` is converted to `then-add-label = TODO`. |
| 5 | `action=Label` with `actionValue=$label5` is converted to `then-add-label = Later`. |
| 6 | An unknown `$labelN` value (N > 5) is imported as `then-add-label = Label<N>`. |
| 7 | No `[warn]` is emitted for the `Label` action once this story is implemented. |

---

## Thunderbird default label mapping

| Tag | Default name |
|-----|-------------|
| `$label1` | Important |
| `$label2` | Work |
| `$label3` | Personal |
| `$label4` | TODO |
| `$label5` | Later |

Note: Thunderbird allows renaming these labels; the importer uses the defaults.
Future enhancement: read the actual names from `prefs.js`.

---

## Implementation notes

Only `src/main_import_rules.c` changes. In the `action=Label` / `actionValue` branch,
map `$labelN` to the appropriate name string via a lookup table.

---

## Related

* US-64: warn on unsupported elements
* US-66: flag actions
