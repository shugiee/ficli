# Uncategorize Transactions In Edit Flows

## Goal
Allow users to clear an existing category from expense/income transactions in
both full edit (`e`) and quick category edit (`c`) flows.

## Plan
1. Add an explicit `Uncategorized` dropdown option in transaction category
   selectors while preserving `Add category` behavior.
2. Update form save logic to map the `Uncategorized` selection to
   `category_id = NULL` in SQLite (`0` in model memory).
3. Keep auto-categorization propagation prompts limited to transitions from
   uncategorized to categorized.

## Notes
- Transfer transactions remain unchanged and cannot be categorized.
- New transaction defaults still prefer the first category when available.
