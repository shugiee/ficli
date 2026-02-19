# Quick Category Edit Shortcut

## Goal
Allow users to update a transaction's category directly from the transactions
list by pressing `c`, without opening the full edit form.

## Plan
1. Add a category-only modal form that loads the selected transaction's current
   category and category options.
2. Reuse the existing category creation flow (`<Add category>` / `n`) inside
   the category-only modal.
3. Save only category changes, while reusing existing propagation logic for
   uncategorized matching-payee transactions.
4. Bind `c` in the transactions list and update status/help hints.

## Notes
- Transfers are excluded because they do not use categories.
