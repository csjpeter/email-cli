# TASK-001 — Phase 38: Add missing warn tests for niche TB elements

**Status:** DONE  
Phase 38 now covers every still-unsupported element: condition fields
(`to or cc`, `size`, `has attachment`, `date`), an unsupported match type on a
supported field (`is in ab`), and the `Stop filter evaluation` action (checks
38.23–38.30).  The elements this ticket originally expected to warn about
(`isn't`, `starred`, `junk`, `Delete`) are supported since US-66/US-68 and are
asserted NOT to warn.

**Type:** Test  
**Related US:** US-74, BUG-001  

## Work items

Add the following checks to Phase 38 in `tests/functional/run_functional.sh`.

The synthetic `msgFilterRules.dat` must be extended with rules that exercise:

### Condition fields
- `cc,contains,@example.com` → `[warn].*cc`
- `to or cc,contains,@example.com` → `[warn].*to or cc`
- `size,greater than,100` → `[warn].*size`
- `has attachment,is,true` → `[warn].*attachment`

### Match types
- `from,isn't,spam` → `[warn].*isn` (BUG-001 fix validation)
- `date,is before,2020-01-01` → `[warn].*is before`

### Actions
- `Copy to folder` → `[warn].*Copy`
- `Mark as starred` → `[warn].*starred`
- `Mark as junk` → `[warn].*junk`
- `Delete` → `[warn].*[Dd]elete`
- `Stop filter evaluation` → `[warn].*[Ss]top`

## Definition of done

All new checks pass (PASS).  Total Phase 38 count increases accordingly.
