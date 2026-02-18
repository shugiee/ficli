# Allow Editing/Deleting Transactions (Strict Transfer Symmetry)

## Goal

Implement editing and deleting transactions from the transaction list view, with strict transfer symmetry rules enforced at the application layer.

This completes the Status item:

```
[ ] Allow editing/deleting transactions
```

---

## Transfer Invariants (Application-Enforced)

For any transaction where `transfer_id != NULL`:

- `type = TRANSFER`
- `category_id = NULL`
- Exactly 2 rows share the same `transfer_id`
- `amount_cents`, `date`, and `description` match across both rows
- The two rows must have distinct `account_id` values
- Each row’s `account_id` mirrors the other side’s previous account when edited

No DB-level constraints are added; all integrity is enforced in `db_update_transaction()`.

---

## Database Layer Changes

### New Public APIs (`query.h`)

```
int db_get_transaction_by_id(sqlite3 *db, int txn_id, transaction_t *out);
int db_update_transaction(sqlite3 *db, const transaction_t *txn);
int db_delete_transaction(sqlite3 *db, int txn_id);
```

### db_get_transaction_by_id

- SELECT full transaction row by `id`
- Convert TEXT ↔ enum
- Return:
  - `0` success
  - `-2` not found
  - `-1` error

---

### db_delete_transaction

1. SELECT `transfer_id` for `id`
2. If NULL → `DELETE FROM transactions WHERE id = ?`
3. If NOT NULL → `DELETE FROM transactions WHERE transfer_id = ?`

Guarantees no orphaned transfer halves.

---

### db_update_transaction

Algorithm:

1. Load original `transfer_id` and `account_id`
2. Normalize input:
   - If `transfer_id != NULL`:
     - Force `type = TRANSFER`
     - Force `category_id = NULL`
   - If `type != TRANSFER`:
     - Force `transfer_id = NULL`
3. UPDATE current row
4. If resulting `transfer_id != NULL`:
   - COUNT rows sharing that `transfer_id`
   - If count == 1 → clear `transfer_id` (self-heal)
   - Else:
     - Mirror to all partner rows:
       - `amount_cents`
       - `date`
       - `description`
       - `type = TRANSFER`
       - `category_id = NULL`
       - `account_id = old_account_id` (swap semantics)
5. If transfer removed (old != NULL and new == NULL):
   - Clear `transfer_id` on partner rows

Corrupt case (COUNT > 2):
- Naively mirror to all rows with same `transfer_id`
- No special handling yet

---

## Form Refactor

Replace `form_add_transaction()` with:

```
form_result_t form_transaction(
    WINDOW *parent,
    sqlite3 *db,
    transaction_t *txn,
    bool is_edit
);
```

- Add mode → insert
- Edit mode → update
- If user changes type away from TRANSFER → clear `transfer_id`

---

## Transaction List Integration

Add keybindings (content-focused mode):

- `e` → edit selected transaction
- `d` → delete selected transaction (with confirmation modal)

After edit/delete:
- Mark list dirty
- Clamp cursor to valid range

---

## Status Hint Update

When list non-empty:

```
↑↓ move  1-9 account  e edit  d delete  a add  ← back
```

---

## Future Improvements

- Add error log UI for resolving transfer integrity issues
- Improve transfer integrity controls (selective cascade delete, unlink tools)

---

## Notes

- No DB schema changes
- No foreign key constraints added
- No automatic creation of inverse transfer rows
- All symmetry logic lives in `query.c`
