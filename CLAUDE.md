# ficli - Personal Finance CLI

## Project Overview
A terminal-based personal finance application for tracking expenses and income. Built as a learning project in C with an ncurses UI.

## Tech Stack
- **Language:** C (C23 standard)
- **UI:** ncurses
- **Build:** Make
- **Storage:** SQLite (single local file)
- **Platform:** Linux

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
- **Schema versioning:** `schema_version` table for future migrations (currently version 1)
- **Defaults seeded on first run:** 1 account (Cash), 9 expense categories, 4 income categories

## Status
- [x] Project scaffolding and build system
- [x] Database schema and initialization
- [x] Basic ncurses UI framework ([plan](../.claude/plans/gentle-strolling-lighthouse.md))
- [x] Transaction input form ([plan](../.claude/plans/b3d42bcf-a861-4f8f-aec5-fe893c511847.md))
- [ ] Transaction list view
- [ ] Category management
- [ ] Summary/report views
- [ ] Budget tracking
