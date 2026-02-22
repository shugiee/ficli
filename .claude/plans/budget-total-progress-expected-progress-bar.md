# Budget Total Progress with Expected-by-Date Marker

## Summary

Add a global total-progress summary to the Budgets screen that aggregates
budgeted parent rows and shows:
- total spent vs total budget for the selected month
- aggregate utilization percentage
- a progress bar with an expected-progress marker based on the current date

## Implementation

1. Extend `budget_list_state_t` with aggregate summary fields:
   - `total_budget_cents`
   - `total_spent_cents`
   - `total_utilization_bps`
   - `expected_bps`
   - `has_total_budget`
2. Add month/time helpers in `src/ui/budget_list.c` to:
   - compare `YYYY-MM` values
   - compute days in month
   - compute expected progress basis points for selected month
     - past month: `100%`
     - current month: linear day-of-month fraction
     - future month: `0%`
3. Compute aggregate totals on reload from already loaded display rows:
   - include parent rows with active budget rules and positive limits
   - clamp per-row negative net spend to zero for total-spent math
   - compute aggregate utilization from total spent/total budget
4. Render a summary row beneath the message row:
   - text: `Total: spent / budget  util%  Expected: expected%`
   - fallback text when no valid parent budget rules are present
   - right-aligned compact total progress bar with expected marker
5. Keep existing per-row progress bars and keybindings unchanged.

## Notes

- No schema migration or query API changes are required.
- Child rows are excluded from total aggregation to avoid double counting.
