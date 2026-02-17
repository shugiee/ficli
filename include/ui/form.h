#ifndef FICLI_FORM_H
#define FICLI_FORM_H

#include <ncurses.h>
#include <sqlite3.h>

typedef enum {
    FORM_SAVED,
    FORM_CANCELLED
} form_result_t;

// Show modal transaction form. Returns FORM_SAVED or FORM_CANCELLED.
form_result_t form_add_transaction(sqlite3 *db, WINDOW *parent);

#endif
