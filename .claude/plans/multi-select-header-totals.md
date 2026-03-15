# Multi-Select Header Totals In Transactions View

## Goal
Show an extra header row in Transactions view while multi-select is active with
the selected count plus selected Sum, Income, Expense, and Net totals.

## Plan
1. Compute aggregate totals from selected transaction IDs in `txn_list`.
2. Exclude transfers from selected financial totals to match header MTD rules.
3. Render a second header line with width-aware fallback variants.
4. Keep account balance and chart math transfer-inclusive for accuracy.

## Notes
- Selected count includes all selected rows; financial totals ignore transfers.
- Balance and chart calculations treat `transfer_id` rows as transfers first,
  even if legacy rows have non-transfer `type` values.
