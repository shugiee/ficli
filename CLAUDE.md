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

## Project Structure (Planned)
```
ficli/
  src/
    main.c          # Entry point, ncurses init
    ui/             # UI components (windows, forms, menus)
    db/             # SQLite database layer
    models/         # Data structures (transaction, category, budget)
  include/          # Header files
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

## Status
- [ ] Project scaffolding and build system
- [ ] Database schema and initialization
- [ ] Basic ncurses UI framework
- [ ] Transaction input form
- [ ] Transaction list view
- [ ] Category management
- [ ] Summary/report views
- [ ] Budget tracking
