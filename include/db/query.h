#ifndef FICLI_QUERY_H
#define FICLI_QUERY_H

#include "models/account.h"
#include "models/category.h"
#include "models/transaction.h"

#include <sqlite3.h>
#include <stdbool.h>

// Fetch all accounts. Caller frees *out. Returns count, -1 on error.
int db_get_accounts(sqlite3 *db, account_t **out);

// Insert an account. card_last4 may be NULL or "" for non-credit-card accounts.
// Returns new row id, -1 on error.
int64_t db_insert_account(sqlite3 *db, const char *name, account_type_t type, const char *card_last4);

// Update an account by id. card_last4 may be NULL or "" for non-credit-card accounts.
// Returns 0 on success, -1 on error.
int db_update_account(sqlite3 *db, const account_t *account);

// Fetch categories by type. Produces "Parent:Child" display names via JOIN.
// Caller frees *out. Returns count, -1 on error.
int db_get_categories(sqlite3 *db, category_type_t type, category_t **out);

// Insert a transaction. Returns new row id, -1 on error.
int64_t db_insert_transaction(sqlite3 *db, const transaction_t *txn);

// Fetch a full transaction by id. Returns 0 success, -2 not found, -1 error.
int db_get_transaction_by_id(sqlite3 *db, int txn_id, transaction_t *out);

// Update a transaction. Returns 0 success, -2 not found, -1 error.
int db_update_transaction(sqlite3 *db, const transaction_t *txn);

// Delete a transaction (and transfer pair if present). Returns 0 success, -2 not found, -1 error.
int db_delete_transaction(sqlite3 *db, int txn_id);

// Count transactions for an account. Returns count, -1 on error.
int db_count_transactions_for_account(sqlite3 *db, int64_t account_id);

// Delete account. If delete_transactions is true, related transactions are
// deleted first. Returns 0 success, -3 has related transactions, -2 not found,
// -1 error.
int db_delete_account(sqlite3 *db, int64_t account_id, bool delete_transactions);

// Lightweight row for transaction list display.
typedef struct {
    int64_t id;
    int64_t amount_cents;
    transaction_type_t type;
    char date[11];
    char category_name[64];  // "Parent:Child" via JOIN, or ""
    char payee[128];
    char description[256];
} txn_row_t;

// Fetch transactions for an account. Caller frees *out. Returns count, -1 on error.
int db_get_transactions(sqlite3 *db, int64_t account_id, txn_row_t **out);

#endif
