# Reports Screen: Category and Payee Tabs

## Summary

Implement the `Reports` screen with two in-screen tabs (`Category` and `Payee`)
and period presets, so reporting dimensions stay grouped under one sidebar entry.

## Implementation

1. Add report query APIs in `db/query` that aggregate expense, income, net, and
   transaction count by:
   - category (with `Parent:Child` names and `Uncategorized` bucket)
   - payee (with `(No payee)` bucket for empty values)
2. Use effective date (`COALESCE(reflection_date, date)`) and exclude transfers.
3. Add a new `ui/report_list` module with:
   - tab switching (`[`, `]`) for category/payee
   - period cycling (`p`): This Month, Last 30 Days, YTD, Last 12 Months
   - sortable columns (`s`, `S`): Name, Expense, Income, Net, Txns
   - scrollable, selectable rows with status hints
4. Wire `SCREEN_REPORTS` in `ui.c` draw/input/status paths and make the screen
   content-focusable.
5. Mark reports dirty when transaction/category-changing actions occur so report
   results refresh without restarting.

## Notes

- No schema migration is required.
- Aggregation and filters are read-only and rely on existing transaction data.
