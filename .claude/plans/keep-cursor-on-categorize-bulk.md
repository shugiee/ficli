# Keep Cursor On Categorized Transaction

## Goal
Keep the transaction list cursor on the edited transaction after category edits,
including bulk category apply flows.

## Plan
1. Add a reload focus anchor in `txn_list` state using transaction id, not row index.
2. Set the focus anchor when category edits are saved so reload can restore the
   edited/template transaction row.
3. Resolve the anchor after list rebuild (post sort/filter) and clamp safely when
   the row is no longer visible.

## Notes
- Preserve existing delete behavior that keeps cursor by row index.
- Bulk selection remains cleared after apply; only cursor focus is preserved.
