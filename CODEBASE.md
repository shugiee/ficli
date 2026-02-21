# Codebase Summary

Quick-reference for the current state of every file, its role, and key implementation details. Read this before exploring individual files to save context.

## File Map

### Entry Point

| File | Purpose |
|------|---------|
| `src/main.c` (29 lines) | Builds DB path (`~/.local/share/ficli/ficli.db`), calls `db_init()`, `ui_init()`, `ui_run()`, `ui_cleanup()`, `db_close()`. No business logic here. |

### CSV Import Layer (`csv/`)

| File | Purpose |
|------|---------|
| `include/csv/csv_import.h` | Types (`csv_type_t`, `csv_row_t`, `csv_parse_result_t`) and API (`csv_parse_file`, `csv_parse_result_free`, `csv_import_credit_card`, `csv_import_checking`) |
| `src/csv/csv_import.c` | Parses CSV files into `csv_parse_result_t`. Detects CC vs checking/savings by presence of a "card" column. Helpers: `csv_parse_line` (quoted-field parser), `normalize_col` (lowercase+trim), `normalize_date` (MM/DD/YYYY or YYYY-MM-DD → YYYY-MM-DD), `parse_csv_amount` (strips $, commas, handles negatives/parens), `extract_last4`. Import functions call `db_insert_transaction()` for each row; CC import matches `card_last4` to CREDIT_CARD accounts. |

### Database Layer (`db/`)

| File | Purpose |
|------|---------|
| `include/db/db.h` | `db_init(path)` returns `sqlite3*`, `db_close(db)` |
| `src/db/db.c` (175 lines) | Creates directory, opens SQLite, creates schema (5 tables + 7 indexes), runs targeted migrations, and seeds defaults on first run. Key helpers: `ensure_dir_exists()`, `exec_sql()`, `is_new_database()`, `create_schema()`, `migrate_schema()`, `seed_defaults()`. |
| `include/db/query.h` | CRUD declarations + list/chart/budget row structs (`txn_row_t`, `balance_point_t`, `budget_row_t`) |
| `src/db/query.c` | Query implementations for accounts/categories/transactions, budget rollups/effective rules, account summaries, and balance-series chart data (`db_get_account_balance_series()`). List-style fetchers use prepare/bind/step/realloc/finalize patterns and return count or -1. |

### Models (`include/models/`)

| File | Struct | Key Fields |
|------|--------|------------|
| `account.h` | `account_t`, `account_type_t` | `id`, `name[64]`, `type` (CASH/CHECKING/SAVINGS/CREDIT_CARD/PHYSICAL_ASSET/INVESTMENT), `card_last4[5]` (non-empty only for CREDIT_CARD) |
| `category.h` | `category_t`, `category_type_t` | `id`, `name[64]`, `type` (EXPENSE/INCOME), `parent_id` |
| `transaction.h` | `transaction_t`, `transaction_type_t` | `id`, `amount_cents`, `type` (EXPENSE/INCOME/TRANSFER), `account_id`, `category_id`, `date[11]` (posted), `reflection_date[11]` (optional override), `payee[128]`, `description[256]`, `transfer_id` |
| `budget.h` | `budget_t` | `id`, `category_id`, `month[8]` ("YYYY-MM"), `limit_cents` |
| `query.h` | `txn_row_t`, `budget_row_t` | `txn_row_t`: `id`, `amount_cents`, `type`, `date[11]` (posted), `reflection_date[11]`, `effective_date[11]`, `category_name[64]`, `payee[128]`, `description[256]`. `budget_row_t`: `category_id`, `parent_category_id`, `category_name[64]`, `child_count`, `net_spent_cents`, `limit_cents`, `has_rule`, `utilization_bps`. |

### UI Layer (`ui/`)

| File | Purpose |
|------|---------|
| `include/ui/ui.h` | `screen_t` enum (DASHBOARD, TRANSACTIONS, CATEGORIES, BUDGETS, REPORTS, COUNT), `ui_init()`, `ui_cleanup()`, `ui_run()` |
| `src/ui/ui.c` (226 lines) | Main UI loop. Static `state` struct holds windows, db handle, screen selection, focus flag, txn_list pointer. Manages layout (header/sidebar/content/status), drawing, and input dispatch. |
| `include/ui/form.h` | `form_add_transaction()` returns `FORM_SAVED` or `FORM_CANCELLED` |
| `src/ui/form.c` (620 lines) | Modal transaction form. Centered overlay on content window. Fields: Type (toggle), Amount (digits+dot), Account (dropdown), Category (dropdown, reloads on type change), Date (posted, YYYY-MM-DD), Reflection Date (optional YYYY-MM-DD), Payee, Description, Submit button. Dropdowns scroll with MAX_DROP=5 visible. Saves via `db_insert_transaction()`/`db_update_transaction()`. |
| `include/ui/txn_list.h` | Opaque `txn_list_state_t`, create/destroy/draw/handle_input/status_hint/mark_dirty/get_current_account_id |
| `src/ui/txn_list.c` | Scrollable transaction list per account with summary header and 90-day balance trend chart (auto-hides on small terminals). Account tabs (1-9 switching), sorting/filtering, colored amounts, bulk selection/edit helpers, and lazy reload via dirty flag. |
| `include/ui/budget_list.h` | Opaque `budget_list_state_t`, create/destroy/draw/handle_input/status_hint/mark_dirty |
| `src/ui/budget_list.c` | Budget view for the selected month: active parent rollups + child spend lines, inline parent budget edits, month navigation, and threshold-colored horizontal progress bars. |
| `include/ui/import_dialog.h` | `import_dialog(parent, db, current_account_id)` — returns imported count or -1 if cancelled |
| `src/ui/import_dialog.c` | Multi-stage modal dialog (56×20). Stages: PATH (text input + parse), CONFIRM_CC (card list with match info and import/skip counts), SELECT_ACCT (j/k scrollable account list), RESULT (final counts), ERROR (error message). |

### Build

| File | Details |
|------|---------|
| `Makefile` | C23 (`-std=c2x`), `-Wall -Wextra -Wpedantic -g`, `-Iinclude`, pkg-config for ncursesw and sqlite3. Source discovery via `$(wildcard src/*.c) $(wildcard src/**/*.c)` — new `.c` files under `src/` are auto-discovered. Targets: `all`, `clean`, `run`. |

## Color Pair IDs

| ID | Name | Foreground | Background | Used In |
|----|------|------------|------------|---------|
| 1 | `COLOR_NORMAL` | CUST_FG | CUST_BG | Default text/background |
| 2 | `COLOR_HEADER` | CUST_BG | CUST_BLUE | Header bar |
| 3 | `COLOR_SELECTED` | CUST_BG | CUST_FG | Sidebar/list selected row |
| 4 | `COLOR_STATUS` | CUST_BG | CUST_BLUE | Status bar |
| 5 | `COLOR_FORM` | CUST_BG | CUST_FG | Modal/dialog base |
| 6 | `COLOR_FORM_ACTIVE` | CUST_BG | CUST_AQUA | Active form field |
| 7 | `COLOR_EXPENSE` | CUST_RED | CUST_BG | Expense / overrun visuals |
| 8 | `COLOR_INCOME` | CUST_GREEN | CUST_BG | Income / under-limit visuals |
| 9 | `COLOR_INFO` | CUST_AQUA | CUST_BG | Informational text |
| 10 | `COLOR_FORM_DROPDOWN` | CUST_FG | CUST_SURFACE | Dropdown layer |
| 11 | `COLOR_ERROR` | CUST_RED | CUST_BG | Error text/borders |
| 12 | `COLOR_WARNING` | CUST_YELLOW | CUST_BG | Budget warning threshold (100-125%) |

Color pair IDs are centralized in `include/ui/colors.h`.

## UI Architecture

**Window layout** (4 windows, recreated on resize):
- `header`: 1 row, full width, row 0
- `sidebar`: rows-2 tall, 18 cols wide (SIDEBAR_WIDTH), row 1
- `content`: rows-2 tall, remaining width, row 1
- `status`: 1 row, full width, last row

**Focus model** (`content_focused` flag in `state`):
- Sidebar focused (default): UP/DOWN navigate sidebar items, Enter/Right selects and focuses content
- Content focused: Escape returns to sidebar. Keys delegate to the active screen's handler (`txn_list`, `account_list`, `category_list`, `budget_list`). Unhandled keys (q, a, KEY_RESIZE) fall through to the main switch.
- Sidebar highlight dims (A_DIM) when content is focused.
- Status bar shows screen-specific hints when content is focused.

**Screen content**: Dedicated renderers exist for Transactions, Accounts, Categories, and Budgets. Dashboard/Reports still show placeholders.

## DB Query Patterns

All query functions in `query.c` follow this pattern:
1. `sqlite3_prepare_v2()` with SQL string
2. `sqlite3_bind_*()` parameters
3. Loop `sqlite3_step() == SQLITE_ROW`, doubling-array realloc
4. `sqlite3_finalize()`, set `*out`, return count (-1 on error)

Types are stored as TEXT in SQLite (`"EXPENSE"`, `"INCOME"`, `"TRANSFER"`) and converted to/from C enums on read/write.

## Schema (v2)

**Tables:** `accounts`, `categories`, `transactions`, `budgets`

**Default seed data:** 1 account ("Cash", type CASH), 9 expense categories, 4 income categories.

**Indexes:** `idx_transactions_date`, `idx_transactions_effective_date`, `idx_transactions_category`, `idx_transactions_account`, `idx_transactions_transfer`, `idx_budgets_month`, `idx_categories_parent`.

Amounts are stored as `INTEGER` cents throughout. Dates are `TEXT` in `YYYY-MM-DD` format. Reporting/budgeting date uses `COALESCE(reflection_date, date)` while account balance charting still uses posted `date`.
