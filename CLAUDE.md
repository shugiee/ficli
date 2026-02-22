# ficli - Personal Finance CLI

> **Start here:** Read [CODEBASE.md](CODEBASE.md) for a detailed map of every file, struct, color pair, and architectural pattern. It saves significant exploration time.

## Project Overview
A terminal-based personal finance app for tracking expenses, income, and transfers. Built in C with an ncurses UI and SQLite persistence.

## Tech Stack
- **Language:** C (C23 standard)
- **UI:** ncursesw
- **Build:** Make
- **Storage:** SQLite (single local file)
- **Platform:** Linux
- **Version Control:** Git (use `git diff` to reference changes during discussions)

## Architecture Decisions
- Single-binary CLI tool
- Local-only data (no networking)
- SQLite for persistent storage (simple, zero-config, single-file)
- ncurses for interactive terminal UI with keyboard-driven navigation

## Core Features (Current)
- Transaction CRUD for expense, income, and transfer entries
- Per-account transaction list with sort/filter/search and keyboard-first navigation
- Bulk transaction selection/edit plus quick category edit shortcut (`c`)
- Payee-aware auto-categorization prompts for uncategorized matches
- Account management UI (add/edit/delete) with account types and credit-card last4
- Category management UI (add/edit/delete), including parent/child categories (`Parent:Child`)
- Budget tracking UI with parent-category rollups, child spend detail, month navigation, and effective-month budget rules
- CSV/QIF import flows for credit card and checking/savings statements with deduplication
- Account summaries and 90-day balance chart in Transactions view
- Theme toggle (`t`) with persisted preference
- Resize-aware layout handling and in-app keyboard shortcut reference (`?`)

## Project Structure (High-level)
`CODEBASE.md` is the canonical file-by-file map. Keep this section concise for orientation.

```
ficli/
  src/
    main.c              # Entry point + DB path bootstrap
    db/                 # Schema init/migrations + query layer
    ui/                 # Main loop, screen modules, forms, popups
    csv/                # CSV parse + import workflows
  include/
    db/                 # DB APIs (`db.h`, `query.h`)
    ui/                 # UI APIs (`ui.h`, list modules, forms, dialogs, colors)
    csv/                # CSV parse/import API
    models/             # account/category/transaction/budget structs/enums
  build/                # Build artifacts
  ficli_seed.sql        # Optional seed restore file
  CODEBASE.md           # Detailed architecture and file map
  Makefile
  CLAUDE.md
```

## Conventions
- Use snake_case for functions and variables
- Prefix module functions with module name (e.g., `db_init()`, `ui_draw_menu()`)
- Header guards use `FICLI_FILENAME_H`
- Keep functions short and focused
- Free all allocated memory; no leaks
- When completing a Status item, ALWAYS link to its plan document (e.g., `([plan](../.claude/plans/name.md))`)
- This is in early stage development, with fully malleable data in the DB. Feel free to run SQL queries directly as we change the schema.

## Build & Run
```
make          # build
make run      # build + run
make clean    # clean build artifacts
./ficli       # run
```

## Database
- **Path:** `~/.local/share/ficli/ficli.db` (created automatically)
- **Amounts:** Stored as integers in cents to avoid floating-point issues
- **Dates:** `date` is the posted/processed date; optional `reflection_date` controls reporting/budget bucket via `COALESCE(reflection_date, date)`
- **Categories:** Have a type (`EXPENSE`/`INCOME`) and optional `parent_id` for sub-categories (displayed as `Parent:Child`)
- **Accounts:** Each transaction belongs to an account; transfers are two linked transactions sharing a `transfer_id`
- **Defaults seeded on first run:** 1 account (Cash), 9 expense categories, 4 income categories
- **Theme config:** `~/.config/ficli/config.ini` (`theme=dark|light`)
- **Seed backup file:** `ficli_seed.sql` (repo root). To use it: `sqlite3 ~/.local/share/ficli/ficli.db < ficli_seed.sql`

## Status
- [x] Project scaffolding and build system
- [x] Database schema and initialization
- [x] Basic ncurses UI framework ([plan](../.claude/plans/gentle-strolling-lighthouse.md))
- [x] Transaction input form ([plan](../.claude/plans/b3d42bcf-a861-4f8f-aec5-fe893c511847.md))
- [x] Transaction list view per account ([plan](../.claude/plans/buzzing-growing-fog.md))
- [x] Add UI to add new accounts ([plan](../.claude/plans/stateless-wibbling-plum.md))
- [x] Add a "Type" property to accounts (Cash, Checking, Savings, Credit Card, Physical Asset, Investment) ([plan](../.claude/plans/sequential-percolating-starlight.md))
- [x] Allow editing/deleting transactions
- [x] Support sort and filter in transaction list ([plan](../.claude/plans/fluttering-shimmying-river.md))
- [x] Allow hjkl navigation, matching arrow keys
- [x] Show keyboard commands when user hits ?
- [x] Align table header row
- [x] Improve color theme
- [x] Add card number to CC accounts
- [x] Data import (CSV) ([plan](../.claude/plans/csv-import.md))
- [x] Make `s` for sort select columns in the correct order
- [x] Add a Payee field to transactions
- [x] Prevent duplicate transactions when importing CSVs
- [x] Add "Submit" button when adding an account
- [x] Allow toggling light/dark mode via `t` shortcut
- [x] Show new account in Transactions view
- [x] Allow editing/deleting accounts
- [x] Show summary information in header
- [x] When adding category to a transaction, offer to apply same change to all transactions with the same payee and no category
- [x] Allow user to add new category on the fly when categorizing a transaction, including sub-category support (e.g., "Food:Dining Out") ([plan](../.claude/plans/on-the-fly-category-creation.md))
- [x] Allow user to edit only category by hitting `c` on a transaction, without opening the full edit form ([plan](../.claude/plans/quick-category-edit-shortcut.md))
- [x] Allow bulk editing transactions (e.g., for categorization), maybe by selecting multiple transactions with spacebar and then hitting `e` to edit all selected transactions
- [x] Allow adding, editing, and deleting Categories in categories UI
- [x] Use ctrl-d and ctrl-u for half-page down/up in transaction list
- [x] Show chart of account balance over time ([plan](../.claude/plans/account-balance-chart.md))
- [x] Add generic error popup that's centered, near the top of the UI. Use it to show error when account is added with naming conflict. ([plan](../.claude/plans/shared-error-popup-account-name-conflict.md))
- [x] Add popout for keyboard shortcuts reference
- [x] Support auto-categorization ([plan](../.claude/plans/auto-categorization-import-payee.md))
- [x] Allow user to type to filter dropdowns ([plan](../.claude/plans/dropdown-type-filter.md))
- [x] Handle window resizing better: debounce resize events, and make sure all UI components adjust correctly without user interaction with UI ([plan](../.claude/plans/window-resize-debounce.md))
- [x] Allow "reflection date" field for transactions to let user control where they're bucketed for reports and budgets without editing actual transaction date
- [x] Automatically enter inverse transaction for transfers
- [x] Budget tracking ([plan](../.claude/plans/budget-tracking-v1.md))
- [x] Show related transactions in Budget view on "Enter"; only edit on `e` ([plan](../.claude/plans/budget-related-transactions-on-enter.md))
- [x] Enforce a minimum window width and height. Show a full-screen message if window is too small, giving dimensions.
- [x] Show total progress towards budget in Budget view, with a progress bar that indicates expected progress for the current date ([plan](../.claude/plans/budget-total-progress-expected-progress-bar.md))
- [x] Support importing from QIF ([plan](../.claude/plans/qif-import.md))
- [ ] Keep cursor on edited transaction when categorizing, including when applying category changes to multiple transactions at once
- [ ] Allow filtering transactions list using regex
- [ ] Allow mass-renaming payee after filtering transactions list
- [x] Allow uncategorizing transactions in `e` or `c` mode ([plan](../.claude/plans/uncategorize-transaction-edit.md))
- [ ] Allow scrolling through transactions list when filtered
- [ ] Summary/report views
- [ ] Add investment purchases/sales with cost basis tracking
- [ ] Allow user to send set of selected transactions to LLM for auto-categorization
- [ ] Offer to auto-create accounts when importing transactions with an account that doesn't exist yet
- [ ] Prevent keyboard events from hitting UI behind the keyboard shortcut popout
- [ ] Allow archiving accounts
- [ ] Support CSV imports for investment accounts
- [ ] Data export (CSV)
- [ ] Support split transactions
- [ ] Add row indices to transaction list
- [ ] Add undo logic
- [ ] When deleting a category, offer to reassign transactions to another category
- [ ] Add reconciliation
- [ ] Add password protection and encryption
- [ ] Allow user to choose whether to delete linked transfer transactions when deleting a transaction
- [ ] Add error UI for handling logic mismatches, e.g. transfers with more than 2 transactions
- [ ] Add CLI arguments for quick actions (e.g., `ficli add -a`)
