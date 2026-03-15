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
// Returns new row id, -2 on uniqueness conflict, -1 on error.
int64_t db_insert_account(sqlite3 *db, const char *name, account_type_t type, const char *card_last4);

// Update an account by id. card_last4 may be NULL or "" for non-credit-card accounts.
// Returns 0 on success, -2 on uniqueness conflict, -1 on error.
int db_update_account(sqlite3 *db, const account_t *account);

typedef enum {
    LOAN_KIND_CAR = 0,
    LOAN_KIND_MORTGAGE = 1,
} loan_kind_t;

typedef struct {
    int64_t id;
    int64_t account_id;
    char account_name[64];
    loan_kind_t loan_kind;
    char start_date[11];
    int interest_rate_bps;
    int64_t initial_principal_cents;
    int64_t scheduled_payment_cents;
    int payment_day;
    int64_t split_principal_cents;
    int64_t split_interest_cents;
    int64_t split_escrow_cents;
    int64_t split_principal_category_id;
    int64_t split_interest_category_id;
    int64_t split_escrow_category_id;
} loan_profile_t;

typedef struct {
    int64_t id;
    int64_t transaction_id;
    int64_t category_id;
    int64_t amount_cents;
    char category_name[64];
} txn_split_t;

// Fetch loan profiles with account names. Caller frees *out.
// Returns count, -1 on error.
int db_get_loan_profiles(sqlite3 *db, loan_profile_t **out);

// Fetch one loan profile by account id. Returns 0 success, -2 not found, -1
// on error.
int db_get_loan_profile_by_account(sqlite3 *db, int64_t account_id,
                                   loan_profile_t *out);

// Insert or update loan profile by account id. Returns 0 on success, -1 on
// error.
int db_upsert_loan_profile(sqlite3 *db, const loan_profile_t *profile);

// Delete loan profile by account id. Returns 0 success, -2 not found, -1 on
// error.
int db_delete_loan_profile(sqlite3 *db, int64_t account_id);

// Compute next payment date for a loan account as "YYYY-MM-DD". Returns 0
// success, -2 not found, -1 on error.
int db_get_next_loan_payment_date(sqlite3 *db, int64_t account_id,
                                  char out_date[11]);

// Enact the next scheduled loan payment as an EXPENSE transaction. Returns
// inserted transaction id, -2 not found, -1 on error.
int64_t db_enact_loan_payment(sqlite3 *db, int64_t account_id);

// Compute the amortized split breakdown for the next scheduled payment.
// Outputs principal, interest, and escrow components in cents.
// Returns 0 success, -2 not found, -1 error.
int db_get_next_loan_payment_breakdown(sqlite3 *db, int64_t account_id,
                                       int64_t *out_principal_cents,
                                       int64_t *out_interest_cents,
                                       int64_t *out_escrow_cents);

// Compute remaining principal after all enacted loan payments for account.
// Returns 0 success, -2 not found, -1 error.
int db_get_loan_remaining_principal_cents(sqlite3 *db, int64_t account_id,
                                          int64_t *out_remaining_cents);

// Ensure standard loan split categories exist for a loan kind.
// For both loan kinds: principal + interest + escrow categories are populated
// under either "Auto Loan" or "Mortgage".
// Returns 0 on success, -1 on error.
int db_ensure_loan_split_categories(sqlite3 *db, loan_kind_t loan_kind,
                                    int64_t *out_principal_category_id,
                                    int64_t *out_interest_category_id,
                                    int64_t *out_escrow_category_id);

// Fetch split rows for one transaction. Caller frees *out.
// Returns count, -1 on error.
int db_get_transaction_splits(sqlite3 *db, int64_t transaction_id,
                              txn_split_t **out);

// Replace split rows for one transaction atomically.
// Validation rules:
// - transaction must exist and be EXPENSE/INCOME
// - split_count must be > 0
// - each split amount must be > 0
// - sum(split amounts) must equal transaction amount
// Returns 0 on success, -2 not found, -3 invalid transaction type,
// -4 invalid splits, -1 on error.
int db_replace_transaction_splits(sqlite3 *db, int64_t transaction_id,
                                  const txn_split_t *splits, int split_count);

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

// Delete category and optionally reassign related transactions in one
// transaction. replacement_category_id <= 0 leaves them uncategorized.
// Returns 0 success, -5 invalid replacement category, -4 has child categories,
// -2 not found, -1 error.
int db_delete_category_with_reassignment(sqlite3 *db, int64_t category_id,
                                         int64_t replacement_category_id);

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
                       int64_t to_account_id, bool allow_existing_match);

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

// Get category from the most recent transaction in an account with exact payee
// and type. Sets *out_category_id to 0 when no matching row or when the
// matching row is uncategorized. Returns 0 on success, -1 on error.
int db_get_most_recent_category_for_payee(sqlite3 *db, int64_t account_id,
                                          const char *payee,
                                          transaction_type_t type,
                                          int64_t *out_category_id);

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
    char date[11];              // posted date
    char reflection_date[11];   // optional reporting override
    char effective_date[11];    // COALESCE(reflection_date, date)
    char category_name[64];  // "Parent:Child" via JOIN, or ""
    char payee[128];
    char description[256];
} txn_row_t;

// Fetch transactions for an account. Caller frees *out. Returns count, -1 on error.
int db_get_transactions(sqlite3 *db, int64_t account_id, txn_row_t **out);

typedef enum {
    REPORT_GROUP_CATEGORY = 0,
    REPORT_GROUP_PAYEE = 1,
} report_group_t;

typedef enum {
    REPORT_PERIOD_THIS_MONTH = 0,
    REPORT_PERIOD_LAST_30_DAYS = 1,
    REPORT_PERIOD_YTD = 2,
    REPORT_PERIOD_LAST_12_MONTHS = 3,
} report_period_t;

typedef struct {
    char label[128];
    int64_t expense_cents;
    int64_t income_cents;
    int64_t net_cents;
    int txn_count;
} report_row_t;

// Fetch grouped report rows for the selected dimension and period.
// Caller frees *out. Returns count, -1 on error.
int db_get_report_rows(sqlite3 *db, report_group_t group, report_period_t period,
                       report_row_t **out);

// Budget row for the Budgets screen. utilization_bps is basis points where
// 10000 = 100%; -1 means utilization is not defined (no active budget limit).
typedef struct {
    int64_t category_id;
    int64_t parent_category_id; // 0 for top-level categories
    char category_name[64];
    int child_count; // direct children
    int txn_count;   // matching EXPENSE/INCOME txns in view month subtree
    int64_t net_spent_cents;
    int64_t direct_limit_cents;
    int64_t limit_cents; // roll-up limit (direct + descendants)
    bool has_rule;       // direct budget rule exists on this category
    bool has_rollup_rule; // any budget rule exists in this category subtree
    int utilization_bps;
} budget_row_t;

// Lightweight transaction row for matching transactions in Budgets view.
typedef struct {
    int64_t id;
    int64_t amount_cents;
    transaction_type_t type;
    char effective_date[11];
    char account_name[64];
    char category_name[64];
    char payee[128];
    char description[256];
} budget_txn_row_t;

// Fetch report-detail transactions for one grouped row label.
// Caller frees *out. Returns count, -1 on error.
int db_get_report_transactions(sqlite3 *db, report_group_t group,
                               report_period_t period, const char *label,
                               budget_txn_row_t **out);

typedef enum {
    BUDGET_CATEGORY_FILTER_EXCLUDE_SELECTED = 0,
    BUDGET_CATEGORY_FILTER_INCLUDE_SELECTED = 1
} budget_category_filter_mode_t;

typedef struct {
    int64_t id;
    int64_t parent_id; // 0 for top-level categories
    category_type_t type;
    char name[64];
} budget_filter_category_t;

// Fetch active top-level budget rows for month "YYYY-MM". Caller frees *out.
// Returns count, -1 on error.
int db_get_budget_rows_for_month(sqlite3 *db, const char *month_ym,
                                 budget_row_t **out);

// Fetch active direct child rows for one top-level category and month
// "YYYY-MM". Caller frees *out. Returns count, -1 on error.
int db_get_budget_child_rows_for_month(sqlite3 *db, int64_t parent_category_id,
                                       const char *month_ym, budget_row_t **out);

// Fetch running budget progress for the selected month context. Sums are for
// previous months only within that month's calendar year (Jan..month-1) for
// the category subtree. actual_cents uses EXPENSE-INCOME net over allowed
// categories based on current budget filter settings. expected_cents is the
// sum of monthly effective limits over the same range.
int db_get_budget_running_progress_for_year_before_month(
    sqlite3 *db, int64_t category_id, const char *month_ym,
    int64_t *out_actual_cents, int64_t *out_expected_cents);

// Set a category budget rule effective from month "YYYY-MM". If the exact
// effective month exists, updates it. Returns 0 success, -1 on error.
int db_set_budget_effective(sqlite3 *db, int64_t category_id,
                            const char *effective_month_ym,
                            int64_t limit_cents);

// Set a one-month budget override for month "YYYY-MM". If the exact month
// override exists, updates it. Returns 0 success, -1 on error.
int db_set_budget_month_override(sqlite3 *db, int64_t category_id,
                                 const char *month_ym, int64_t limit_cents);

// Clear a one-month budget override for month "YYYY-MM". Returns 0 success,
// -1 on error.
int db_clear_budget_month_override(sqlite3 *db, int64_t category_id,
                                   const char *month_ym);

// Fetch effective budget limit for a category in month "YYYY-MM". Returns 0
// success, -2 when no matching rule exists, -1 on error.
int db_get_budget_limit_for_month(sqlite3 *db, int64_t category_id,
                                  const char *month_ym, int64_t *out_limit_cents);

// Fetch transactions matching the category subtree for a month ("YYYY-MM"),
// using effective date (COALESCE(reflection_date, date)).
// Caller frees *out. Returns count, -1 on error.
int db_get_budget_transactions_for_month(sqlite3 *db, int64_t category_id,
                                         const char *month_ym,
                                         budget_txn_row_t **out);

// Get/set persistent category filter mode for Budgets screen.
// Default mode is BUDGET_CATEGORY_FILTER_EXCLUDE_SELECTED.
int db_get_budget_category_filter_mode(sqlite3 *db,
                                       budget_category_filter_mode_t *out_mode);
int db_set_budget_category_filter_mode(sqlite3 *db,
                                       budget_category_filter_mode_t mode);

// Fetch selected budget filter category ids. Caller frees *out.
int db_get_budget_category_filter_selected(sqlite3 *db, int64_t **out);

// Toggle category selection in budget filters. selected=true inserts, false
// removes. Returns 0 success, -2 not found, -1 error.
int db_set_budget_category_filter_selected(sqlite3 *db, int64_t category_id,
                                           bool selected);

// Fetch all categories for budget filter UI. Caller frees *out. Returns count,
// -1 on error.
int db_get_budget_filter_categories(sqlite3 *db, budget_filter_category_t **out);

#endif
