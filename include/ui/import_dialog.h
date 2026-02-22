#ifndef FICLI_IMPORT_DIALOG_H
#define FICLI_IMPORT_DIALOG_H

#include <ncurses.h>
#include <sqlite3.h>
#include <stdint.h>

// Show the file import dialog (CSV/QIF) over parent.
// Returns number of transactions imported (>= 0), or -1 if cancelled.
int import_dialog(WINDOW *parent, sqlite3 *db, int64_t current_account_id);

#endif
