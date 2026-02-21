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

// Find or create category by exact name/type/parent. parent_id <= 0 means
// top-level. Returns category id, -1 on error.
int64_t db_get_or_create_category(sqlite3 *db, category_type_t type,
                                  const char *name, int64_t parent_id);

// Update category fields by id. parent_id <= 0 means top-level.
// Returns 0 on success, -2 on uniqueness conflict, -1 on error.
int db_update_category(sqlite3 *db, const category_t *category);

// Count transactions referencing a category. Returns count, -1 on error.
int db_count_transactions_for_category(sqlite3 *db, int64_t category_id);

// Count direct child categories. Returns count, -1 on error.
int db_count_child_categories(sqlite3 *db, int64_t category_id);

// Delete category. Returns 0 success, -4 has child categories, -3 has related
// transactions, -2 not found, -1 error.
int db_delete_category(sqlite3 *db, int64_t category_id);

// Insert a transaction. Returns new row id, -1 on error.
int64_t db_insert_transaction(sqlite3 *db, const transaction_t *txn);

// Insert a transfer pair (from txn.account_id to to_account_id). Returns the
// source transaction id on success, -2 for invalid accounts, -1 on error.
int64_t db_insert_transfer(sqlite3 *db, const transaction_t *txn,
                           int64_t to_account_id);

// Fetch a full transaction by id. Returns 0 success, -2 not found, -1 error.
int db_get_transaction_by_id(sqlite3 *db, int txn_id, transaction_t *out);

// Update a transaction. Returns 0 success, -2 not found, -1 error.
int db_update_transaction(sqlite3 *db, const transaction_t *txn);

// Update or create a transfer pair for an existing source transaction id.
// Returns 0 success, -2 not found, -3 invalid accounts, -1 error.
int db_update_transfer(sqlite3 *db, const transaction_t *txn,
                       int64_t to_account_id);

// Get the paired transfer account id for txn_id. Returns 0 success, -2 if no
// linked pair, -1 on error.
int db_get_transfer_counterparty_account(sqlite3 *db, int64_t txn_id,
                                         int64_t *out_account_id);

// Delete a transaction (and transfer pair if present). Returns 0 success, -2 not found, -1 error.
int db_delete_transaction(sqlite3 *db, int txn_id);

// Count transactions for an account. Returns count, -1 on error.
int db_count_transactions_for_account(sqlite3 *db, int64_t account_id);

// Count uncategorized non-transfer transactions with an exact payee and type.
// Returns 0 on success, -1 on error.
int db_count_uncategorized_by_payee(sqlite3 *db, const char *payee,
                                    transaction_type_t type,
                                    int64_t *out_count);

// Apply category to uncategorized non-transfer transactions with exact payee and
// type. Returns number of updated rows, -1 on error.
int db_apply_category_to_uncategorized_by_payee(sqlite3 *db, const char *payee,
                                                transaction_type_t type,
                                                int64_t category_id);

// Account-level summary helpers. Returns 0 on success, -1 on error.
int db_get_account_balance_cents(sqlite3 *db, int64_t account_id,
                                 int64_t *out_cents);
int db_get_account_month_net_cents(sqlite3 *db, int64_t account_id,
                                   int64_t *out_cents);
int db_get_account_month_income_cents(sqlite3 *db, int64_t account_id,
                                      int64_t *out_cents);
int db_get_account_month_expense_cents(sqlite3 *db, int64_t account_id,
                                       int64_t *out_cents);

// Daily account balance point for charting.
typedef struct {
    char date[11]; // "YYYY-MM-DD"
    int64_t balance_cents;
} balance_point_t;

// Fetch contiguous daily balance points for lookback_days ending today
// (localtime). Caller frees *out. Returns count, -1 on error.
int db_get_account_balance_series(sqlite3 *db, int64_t account_id,
                                  int lookback_days, balance_point_t **out);

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
