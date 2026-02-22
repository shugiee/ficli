#ifndef FICLI_CSV_IMPORT_H
#define FICLI_CSV_IMPORT_H

#include "models/transaction.h"

#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CSV_TYPE_UNKNOWN,
    CSV_TYPE_CREDIT_CARD,
    CSV_TYPE_CHECKING_SAVINGS,
    CSV_TYPE_QIF
} csv_type_t;

typedef struct {
    char date[11];
    int64_t amount_cents;
    transaction_type_t type;
    char payee[128];
    char description[256];
    char category[64];
    bool has_category;
    int64_t category_id;
    char card_last4[5];
} csv_row_t;

typedef struct {
    csv_type_t type;
    csv_row_t *rows;
    int row_count;
    char source_account[64];
    char error[256];
} csv_parse_result_t;

// Parse a CSV or QIF file. Returns result with type set; result.error non-empty
// on failure. For QIF imports, source_account is set when one account is found.
// Caller must call csv_parse_result_free() when done.
csv_parse_result_t csv_parse_file(const char *path);

// Free resources held by a parse result.
void csv_parse_result_free(csv_parse_result_t *r);

// Import CC transactions: matches each row's card_last4 to a CREDIT_CARD account.
// Returns 0 on success, -1 on DB error. *imported and *skipped are set.
int csv_import_credit_card(sqlite3 *db, const csv_parse_result_t *r,
                           int *imported, int *skipped);

// Import checking/savings transactions into the given account.
// Returns 0 on success, -1 on DB error. *imported and *skipped are set.
int csv_import_checking(sqlite3 *db, const csv_parse_result_t *r,
                        int64_t account_id, int *imported, int *skipped);

#endif
