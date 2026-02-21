# Auto-Categorization On Import

## Goal
Automatically assign categories during CSV import by reusing the category from
the most recent transaction in the same account with the same payee and
transaction type.

## Plan
1. Add a DB helper that fetches the most recent matching transaction category
   by `(account_id, payee, type)`.
2. During CSV import (credit-card and checking/savings paths), resolve
   `txn.category_id` with that helper before inserting each row.
3. Keep fallback behavior uncategorized when there is no prior match or when
   the prior match is uncategorized.

## Notes
- Matching is exact payee string and same transaction type (`EXPENSE`/`INCOME`).
- Transfer transactions are excluded from this lookup.
