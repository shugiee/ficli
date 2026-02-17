#ifndef FICLI_QUERY_H
#define FICLI_QUERY_H

#include "models/account.h"
#include "models/category.h"
#include "models/transaction.h"

#include <sqlite3.h>

// Fetch all accounts. Caller frees *out. Returns count, -1 on error.
int db_get_accounts(sqlite3 *db, account_t **out);

// Insert an account. Returns new row id, -1 on error.
int64_t db_insert_account(sqlite3 *db, const char *name);

// Fetch categories by type. Produces "Parent:Child" display names via JOIN.
// Caller frees *out. Returns count, -1 on error.
int db_get_categories(sqlite3 *db, category_type_t type, category_t **out);

// Insert a transaction. Returns new row id, -1 on error.
int64_t db_insert_transaction(sqlite3 *db, const transaction_t *txn);

// Lightweight row for transaction list display.
typedef struct {
    int64_t id;
    int64_t amount_cents;
    transaction_type_t type;
    char date[11];
    char category_name[64];  // "Parent:Child" via JOIN, or ""
    char description[256];
} txn_row_t;

// Fetch transactions for an account. Caller frees *out. Returns count, -1 on error.
int db_get_transactions(sqlite3 *db, int64_t account_id, txn_row_t **out);

#endif
