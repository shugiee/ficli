# Codebase Summary

Quick-reference for the current state of every file, its role, and key implementation details. Read this before exploring individual files to save context.

## File Map

### Entry Point

| File | Purpose |
|------|---------|
| `src/main.c` (29 lines) | Builds DB path (`~/.local/share/ficli/ficli.db`), calls `db_init()`, `ui_init()`, `ui_run()`, `ui_cleanup()`, `db_close()`. No business logic here. |

### Database Layer (`db/`)

| File | Purpose |
|------|---------|
| `include/db/db.h` | `db_init(path)` returns `sqlite3*`, `db_close(db)` |
| `src/db/db.c` (175 lines) | Creates directory, opens SQLite, creates schema (5 tables + 6 indexes), seeds defaults on first run. Key helpers: `ensure_dir_exists()`, `exec_sql()`, `is_new_database()`, `create_schema()`, `seed_defaults()`. |
| `include/db/query.h` | CRUD declarations + `txn_row_t` struct for list display |
| `src/db/query.c` (208 lines) | `db_get_accounts()`, `db_get_categories()`, `db_get_transactions()`, `db_insert_transaction()`. All follow same pattern: prepare, bind, loop with doubling-array realloc, finalize. Return count or -1. Caller frees `*out`. |

### Models (`include/models/`)

| File | Struct | Key Fields |
|------|--------|------------|
| `account.h` | `account_t`, `account_type_t` | `id`, `name[64]`, `type` (CASH/CHECKING/SAVINGS/CREDIT_CARD/PHYSICAL_ASSET/INVESTMENT) |
| `category.h` | `category_t`, `category_type_t` | `id`, `name[64]`, `type` (EXPENSE/INCOME), `parent_id` |
| `transaction.h` | `transaction_t`, `transaction_type_t` | `id`, `amount_cents`, `type` (EXPENSE/INCOME/TRANSFER), `account_id`, `category_id`, `date[11]`, `description[256]`, `transfer_id` |
| `budget.h` | `budget_t` | `id`, `category_id`, `month[8]` ("YYYY-MM"), `limit_cents` |
| `query.h` | `txn_row_t` | `id`, `amount_cents`, `type`, `date[11]`, `category_name[64]`, `description[256]` — flat row for list display |

### UI Layer (`ui/`)

| File | Purpose |
|------|---------|
| `include/ui/ui.h` | `screen_t` enum (DASHBOARD, TRANSACTIONS, CATEGORIES, BUDGETS, REPORTS, COUNT), `ui_init()`, `ui_cleanup()`, `ui_run()` |
| `src/ui/ui.c` (226 lines) | Main UI loop. Static `state` struct holds windows, db handle, screen selection, focus flag, txn_list pointer. Manages layout (header/sidebar/content/status), drawing, and input dispatch. |
| `include/ui/form.h` | `form_add_transaction()` returns `FORM_SAVED` or `FORM_CANCELLED` |
| `src/ui/form.c` (620 lines) | Modal transaction form. Centered overlay on content window. Fields: Type (toggle), Amount (digits+dot), Account (dropdown), Category (dropdown, reloads on type change), Date (YYYY-MM-DD, defaults today), Description (free text), Submit button. Dropdowns scroll with MAX_DROP=5 visible. Saves via `db_insert_transaction()`. |
| `include/ui/txn_list.h` | Opaque `txn_list_state_t`, create/destroy/draw/handle_input/status_hint/mark_dirty |
| `src/ui/txn_list.c` (224 lines) | Scrollable transaction list per account. Account tabs (1-9 switching), column headers, colored amounts (red=expense, green=income), cursor with A_REVERSE, auto-scroll. Lazy-loads data on dirty flag. |

### Build

| File | Details |
|------|---------|
| `Makefile` | C23 (`-std=c2x`), `-Wall -Wextra -Wpedantic -g`, `-Iinclude`, pkg-config for ncursesw and sqlite3. Source discovery via `$(wildcard src/*.c) $(wildcard src/**/*.c)` — new `.c` files under `src/` are auto-discovered. Targets: `all`, `clean`, `run`. |

## Color Pair IDs

| ID | Name | Foreground | Background | Used In |
|----|------|------------|------------|---------|
| 1 | `COLOR_HEADER` | BLACK | CYAN | Header bar |
| 2 | `COLOR_SELECTED` | BLACK | WHITE | Sidebar highlight, account tabs |
| 3 | `COLOR_STATUS` | BLACK | CYAN | Status bar |
| 10 | `COLOR_FORM` | WHITE | BLUE | Form background |
| 11 | `COLOR_FORM_ACTIVE` | BLACK | CYAN | Active form field |
| 12 | `COLOR_EXPENSE` | RED | default | Expense amounts in list |
| 13 | `COLOR_INCOME` | GREEN | default | Income amounts in list |

IDs 1-3 are in the `enum` in `ui.c`; 10-11 are defined with literal ints (shared between `ui.c` init and `form.c` `enum`); 12-13 are `COLOR_EXPENSE`/`COLOR_INCOME` in both `ui.c` enum and `txn_list.c` `#define`.

## UI Architecture

**Window layout** (4 windows, recreated on resize):
- `header`: 1 row, full width, row 0
- `sidebar`: rows-2 tall, 18 cols wide (SIDEBAR_WIDTH), row 1
- `content`: rows-2 tall, remaining width, row 1
- `status`: 1 row, full width, last row

**Focus model** (`content_focused` flag in `state`):
- Sidebar focused (default): UP/DOWN navigate sidebar items, Enter/Right selects and focuses content
- Content focused: LEFT/Escape returns to sidebar. Keys delegate to the active screen's handler (currently only `txn_list_handle_input`). Unhandled keys (q, a, KEY_RESIZE) fall through to the main switch.
- Sidebar highlight dims (A_DIM) when content is focused.
- Status bar shows screen-specific hints when content is focused.

**Screen content**: Only SCREEN_TRANSACTIONS has a dedicated renderer (`txn_list_draw`). All other screens show a centered placeholder label.

## DB Query Patterns

All query functions in `query.c` follow this pattern:
1. `sqlite3_prepare_v2()` with SQL string
2. `sqlite3_bind_*()` parameters
3. Loop `sqlite3_step() == SQLITE_ROW`, doubling-array realloc
4. `sqlite3_finalize()`, set `*out`, return count (-1 on error)

Types are stored as TEXT in SQLite (`"EXPENSE"`, `"INCOME"`, `"TRANSFER"`) and converted to/from C enums on read/write.

## Schema (v2)

**Tables:** `schema_version`, `accounts`, `categories`, `transactions`, `budgets`

**Default seed data:** 1 account ("Cash", type CASH), 9 expense categories, 4 income categories.

**Indexes:** `idx_transactions_date`, `idx_transactions_category`, `idx_transactions_account`, `idx_transactions_transfer`, `idx_budgets_month`, `idx_categories_parent`.

Amounts are stored as `INTEGER` cents throughout. Dates are `TEXT` in `YYYY-MM-DD` format.

**Migrations:** `db_init()` checks `schema_version` and runs migrations for existing databases. v1→v2 adds `type TEXT NOT NULL DEFAULT 'CASH'` column to `accounts`.
