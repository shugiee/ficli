# Plan: Filtered Bulk Edit via Transaction Form

## Goal
Allow users to edit all currently filtered transactions through the existing `e` edit form (when no explicit selection), so payee and other fields can be changed in one pass.

## Implementation
- Reuse the existing `e` edit flow as a template-based bulk operation when a filter is active and no rows are explicitly selected.
- Apply only fields that changed in the template edit to all currently displayed filtered rows (excluding the template row itself, which is already saved by the form).
- Keep focus on the previously selected transaction after reload and clear bulk selection state.
- Update transaction status hint text and keyboard-shortcuts help entries to document filtered bulk edit.

## Validation
- Build succeeds with `make`.
- With an active filter (for example `AMAZ`) and no explicit selection, pressing `e`, changing payee to `Amazon`, and saving updates all filtered rows while untouched fields (for example date) stay unchanged.
- After apply/reload, the focused row remains on the same transaction id.
