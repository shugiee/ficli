# On-The-Fly Category Creation

## Goal
Allow users to create categories directly from the transaction form while
categorizing, with sub-category support via `Parent:Child` syntax.

## Plan
1. Add a DB helper to find-or-create categories by `(type, name, parent_id)`.
2. Add a transaction-form action on the Category field (`n`) to open a small
   prompt and accept a category path.
3. Parse the entered path, create parent and child categories as needed, reload
   category options, and select the resulting category.
4. Keep existing category dropdown and save flow unchanged.

## Notes
- Input supports either `Category` (top-level) or `Parent:Child` (sub-category).
- Invalid input (empty segments or multiple `:` separators) shows a form error.
