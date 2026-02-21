# ficli - Personal Finance CLI

> **Start here:** Read [CODEBASE.md](CODEBASE.md) for a detailed map of every file, struct, color pair, and architectural pattern. It saves significant exploration time.

## Project Overview
A terminal-based personal finance application for tracking expenses and income. Built as a learning project in C with an ncurses UI.

## Tech Stack
- **Language:** C (C23 standard)
- **UI:** ncurses
- **Build:** Make
- **Storage:** SQLite (single local file)
- **Platform:** Linux
- **Version Control:** Git (use `git diff` to reference changes during discussions)

## Architecture Decisions
- Single-binary CLI tool
- Local-only data (no networking)
- SQLite for persistent storage (simple, zero-config, single-file)
- ncurses for interactive terminal UI with keyboard-driven navigation

## Core Features (Planned)
- Add/edit/delete transactions (expenses and income)
- Categorize transactions
- View transaction history with filtering (by date, category, type)
- Monthly/yearly summaries
- Budget tracking per category

## Project Structure
```
ficli/
  src/
    main.c          # Entry point, ncurses init
    ui/             # UI components (windows, forms, menus)
    db/             # SQLite database layer (schema, seeding)
  include/
    db/db.h         # Database public API
    ui/ui.h         # UI public API
    models/         # Data structures
      account.h     # account_t
      budget.h      # budget_t
      category.h    # category_t, category_type_t
      transaction.h # transaction_t, transaction_type_t
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

## Build & Run
```
make          # build
make clean    # clean build artifacts
./ficli       # run
```

## Database
- **Path:** `~/.local/share/ficli/ficli.db` (created automatically)
- **Amounts:** Stored as integers in cents to avoid floating-point issues
- **Categories:** Have a type (`EXPENSE`/`INCOME`) and optional `parent_id` for sub-categories (displayed as `Parent:Child`)
- **Accounts:** Each transaction belongs to an account; transfers are two linked transactions sharing a `transfer_id`
- **Defaults seeded on first run:** 1 account (Cash), 9 expense categories, 4 income categories
- **Seed backup file** flicli_seed.sql. To use it: `sqlite3 ~/.local/share/ficli/ficli.db < ficli_seed.sql`

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
- [ ] Summary/report views
- [ ] Allow user to send set of selected transactions to LLM for auto-categorization
- [ ] Allow user to type to filter dropdowns
- [ ] Prevent keyboard events from hitting UI behind the keyboard shortcut popout
- [ ] Allow "reflection date" field for transactions to let user control where they're bucketed for reports and budgets without editing actual transaction date
- [ ] Budget tracking
- [ ] Allow archiving accounts
- [ ] Support CSV imports for investment accounts
- [ ] Data export (CSV)
- [ ] Support split transactions
- [ ] Add row indices to transaction list
- [ ] Add undo logic
- [ ] Automatically enter inverse transaction for transfers
- [ ] Add investment purchases/sales with cost basis tracking
- [ ] Handle window resizing better: debounce resize events, and make sure all UI components adjust correctly without user interaction with UI
- [ ] When deleting a category, offer to reassign transactions to another category
- [ ] Add reconciliation
- [ ] Add password protection and encryption
- [ ] Support auto-categorization
- [ ] Allow user to choose whether to delete linked transfer transactions when deleting a transaction
- [ ] Add error UI for handling logic mismatches, e.g. transfers with more than 2 transactions
- [ ] Add popout for keyboard shortcuts reference
- [ ] Add CLI arguments for quick actions (e.g., `ficli add -a`)
