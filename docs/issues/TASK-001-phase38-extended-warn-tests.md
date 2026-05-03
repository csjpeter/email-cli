# TASK-001 — Phase 38: Add missing warn tests for niche TB elements

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
