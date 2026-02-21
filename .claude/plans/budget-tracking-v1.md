# Budget Tracking V1

## Summary

Implement the `Budgets` screen with monthly category budgeting that is safe for historical data:
- Parent-category rollup rows with child spend detail rows.
- Inline budget editing on parent rows only.
- Effective-month budget rules (`YYYY-MM`) so updates apply to that month and all future months.
- Active-only row visibility and month navigation.
- Horizontal progress bars with thresholds: green (`<=100%`), yellow (`<=125%`), red (`>125%`).

## Implementation

1. Add budget query APIs in `db/query` for:
   - active top-level rows for a month
   - active child rows for a parent and month
   - effective budget upsert
   - effective budget lookup
2. Implement recursive category rollups for monthly net spend using `COALESCE(reflection_date, date)` and `EXPENSE - INCOME` net math.
3. Add `ui/budget_list` module for rendering, keyboard handling, inline edit mode, and month controls.
4. Wire `SCREEN_BUDGETS` into `ui.c` draw/input/status/help and mark budget view dirty when transaction/category/account changes can affect budget math.
5. Add warning color pair support for the 100-125% utilization range.

## Notes

- Existing `budgets` schema is reused; no migration is required.
- Negative net spend clamps utilization to `0%` while preserving signed net display.
