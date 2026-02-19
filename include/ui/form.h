#ifndef FICLI_FORM_H
#define FICLI_FORM_H

#include <ncurses.h>
#include <sqlite3.h>
#include <stdbool.h>

#include "models/account.h"
#include "models/transaction.h"

typedef enum {
    FORM_SAVED,
    FORM_CANCELLED
} form_result_t;

// Show modal transaction form. Returns FORM_SAVED or FORM_CANCELLED.
form_result_t form_transaction(WINDOW *parent, sqlite3 *db, transaction_t *txn, bool is_edit);

// Show modal account form. Returns FORM_SAVED or FORM_CANCELLED.
form_result_t form_account(WINDOW *parent, sqlite3 *db, account_t *account, bool is_edit);

#endif
