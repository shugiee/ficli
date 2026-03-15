# Show Account Totals Per Row on Accounts Page

## Goal
Show each account's current balance directly in its row on the Accounts view.

## Plan
1. Load account balances during Accounts view reload using existing
   `db_get_account_balance_cents` logic.
2. Store balances alongside loaded account rows for rendering.
3. Render each row with name, right-aligned signed balance, and type tag (when
   width allows).
4. Use responsive fallbacks so balances remain visible on narrow terminals.

## Notes
- No schema migration is required.
- Row totals use the same transaction sign handling as existing account balance
  calculations.
