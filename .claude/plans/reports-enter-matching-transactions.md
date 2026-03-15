# Reports: Matching Transactions on Enter

## Summary

Extend the `Reports` screen so pressing `Enter` on a selected summary row shows
matching transactions in a lower section, mirroring the Budgets interaction
pattern, with `e` opening edit for the selected matching transaction.

## Implementation

1. Add `db/query` API for report detail rows by selected report grouping label:
   - respects current reports group (`Category` or `Payee`)
   - respects period preset windows using effective date
   - includes both `EXPENSE` and `INCOME`, excludes transfers
2. Extend `ui/report_list` state with a related-transactions section:
   - visibility/focus, cursor/scroll
   - selected anchor context (group, period, label)
3. Wire reports key handling:
   - `Enter` on summary row loads matching transactions and focuses details
   - `e` in details focus opens transaction edit form
   - `Enter`/`Tab`/`Esc` exits details focus back to summary list
4. Render a lower "Matching Transactions" table with date, amount, account,
   and details (category + payee/description context).
5. Refresh/clear related rows when report context changes (tab/period) and on
   dirty reload after edits/imports.

## Notes

- Category detail matching uses the same display label semantics as summary rows,
  including `Uncategorized` handling.
- Payee detail matching supports the `(No payee)` bucket.
