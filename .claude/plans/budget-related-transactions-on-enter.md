# Budget Related Transactions on Enter

## Summary

Update the Budgets screen interaction model so `Enter` shows matching transactions
for the selected budget row, while `e` remains the only budget-edit shortcut.
Render matching transactions in a dedicated lower section below the budget rows.

## Implementation

1. Add a new `db/query` API that fetches transactions for a category subtree and
   month (`YYYY-MM`) using effective date (`COALESCE(reflection_date, date)`).
2. Extend `ui/budget_list` state to store the currently shown related-transaction
   list and the selected category context.
3. Add a new "Matching Transactions" subsection at the bottom of the Budgets
   page with its own headers and rows (date, amount, account, details).
4. Change key handling in Budgets:
   - `Enter` loads/displays matching transactions for the selected row.
   - `e` starts inline budget editing (parent rows only).
5. Update Budgets help/status hints to reflect the new shortcut behavior.

## Notes

- Related transactions are refreshed when budget rows reload (for example after
  month navigation or external data changes) if the related section is active.
- The matching query includes both `EXPENSE` and `INCOME` transactions and
  includes descendant categories of the selected budget category.
