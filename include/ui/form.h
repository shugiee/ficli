#ifndef FICLI_FORM_H
#define FICLI_FORM_H

#include <ncurses.h>
#include <sqlite3.h>
#include <stdbool.h>

#include "models/account.h"
#include "models/category.h"
#include "models/transaction.h"

typedef enum {
    FORM_SAVED,
    FORM_CANCELLED
} form_result_t;

// Show modal transaction form. Returns FORM_SAVED or FORM_CANCELLED.
form_result_t form_transaction(WINDOW *parent, sqlite3 *db, transaction_t *txn, bool is_edit);

// Show modal category-only editor for an existing non-transfer transaction.
// Returns FORM_SAVED when category was updated.
form_result_t form_transaction_category(WINDOW *parent, sqlite3 *db,
                                        transaction_t *txn);

// Show modal account form. Returns FORM_SAVED or FORM_CANCELLED.
form_result_t form_account(WINDOW *parent, sqlite3 *db, account_t *account, bool is_edit);

// Show modal category form. Name accepts "Parent:Child" path format.
// Returns FORM_SAVED or FORM_CANCELLED.
form_result_t form_category(WINDOW *parent, sqlite3 *db, category_t *category,
                            bool is_edit);

#endif
