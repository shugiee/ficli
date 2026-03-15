#include "db/query.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>

static const char *account_type_db_strings[] = {
    "CASH", "CHECKING", "SAVINGS", "CREDIT_CARD", "PHYSICAL_ASSET", "INVESTMENT"
};

static const char *transaction_type_db_strings[] = {
    "EXPENSE", "INCOME", "TRANSFER"
};
static const int transfer_match_date_window_days = 3;

static account_type_t account_type_from_str(const char *s) {
    if (s) {
        for (int i = 0; i < ACCOUNT_TYPE_COUNT; i++) {
            if (strcmp(s, account_type_db_strings[i]) == 0)
                return (account_type_t)i;
        }
    }
    return ACCOUNT_CASH;
}

static transaction_type_t transaction_type_from_str(const char *s) {
    if (s) {
        for (int i = 0; i < 3; i++) {
            if (strcmp(s, transaction_type_db_strings[i]) == 0)
                return (transaction_type_t)i;
        }
    }
    return TRANSACTION_EXPENSE;
}

static const char *transaction_type_to_str(transaction_type_t type) {
    if (type < 0 || type > TRANSACTION_TRANSFER)
        return "EXPENSE";
    return transaction_type_db_strings[type];
}

static const char *category_type_to_str(category_type_t type) {
    return (type == CATEGORY_INCOME) ? "INCOME" : "EXPENSE";
}

static budget_category_filter_mode_t
budget_filter_mode_from_str(const char *s) {
    if (s && strcmp(s, "INCLUDE_SELECTED") == 0)
        return BUDGET_CATEGORY_FILTER_INCLUDE_SELECTED;
    return BUDGET_CATEGORY_FILTER_EXCLUDE_SELECTED;
}

static const char *
budget_filter_mode_to_str(budget_category_filter_mode_t mode) {
    return (mode == BUDGET_CATEGORY_FILTER_INCLUDE_SELECTED)
               ? "INCLUDE_SELECTED"
               : "EXCLUDE_SELECTED";
}

static const char *report_period_start_expr(report_period_t period) {
    switch (period) {
    case REPORT_PERIOD_THIS_MONTH:
        return "date('now', 'localtime', 'start of month')";
    case REPORT_PERIOD_LAST_30_DAYS:
        return "date('now', 'localtime', '-29 days')";
    case REPORT_PERIOD_YTD:
        return "date('now', 'localtime', 'start of year')";
    case REPORT_PERIOD_LAST_12_MONTHS:
        return "date('now', 'localtime', 'start of month', '-11 months')";
    default:
        return NULL;
    }
}

static int bind_text_or_null(sqlite3_stmt *stmt, int idx, const char *value) {
    if (value && value[0] != '\0')
        return sqlite3_bind_text(stmt, idx, value, -1, SQLITE_STATIC);
    return sqlite3_bind_null(stmt, idx);
}

static int normalize_txn_date(const char *src, char out[11]) {
    if (!src || !out)
        return -1;

    int y = 0, m = 0, d = 0;
    int len = (int)strlen(src);
    if (len == 10 && src[4] == '-' && src[7] == '-') {
        if (sscanf(src, "%4d-%2d-%2d", &y, &m, &d) != 3)
            return -1;
    } else if (len == 10 && src[2] == '/' && src[5] == '/') {
        if (sscanf(src, "%2d/%2d/%4d", &m, &d, &y) != 3)
            return -1;
    } else if (len == 8 && src[2] == '/' && src[5] == '/') {
        int yy = 0;
        if (sscanf(src, "%2d/%2d/%2d", &m, &d, &yy) != 3)
            return -1;
        y = 2000 + yy;
    } else {
        return -1;
    }

    if (m < 1 || m > 12 || d < 1 || d > 31 || y < 1900)
        return -1;

    struct tm tmv = {0};
    tmv.tm_year = y - 1900;
    tmv.tm_mon = m - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = 12;
    tmv.tm_isdst = -1;
    if (mktime(&tmv) == (time_t)-1)
        return -1;
    if (tmv.tm_year != y - 1900 || tmv.tm_mon != m - 1 || tmv.tm_mday != d)
        return -1;

    if (strftime(out, 11, "%Y-%m-%d", &tmv) != 10)
        return -1;
    return 0;
}

static int normalize_optional_txn_date(const char *src, char out[11]) {
    if (!out)
        return -1;
    if (!src || src[0] == '\0') {
        out[0] = '\0';
        return 0;
    }
    return normalize_txn_date(src, out);
}

static int insert_transfer_row(sqlite3 *db, const transaction_t *txn,
                               int64_t account_id, int64_t transfer_id,
                               int64_t *out_id) {
    char norm_date[11];
    if (normalize_txn_date(txn->date, norm_date) < 0)
        return -1;
    char norm_reflection_date[11];
    if (normalize_optional_txn_date(txn->reflection_date, norm_reflection_date) <
        0)
        return -1;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "INSERT INTO transactions (amount_cents, type, account_id, category_id, date, reflection_date, payee, description, transfer_id)"
        " VALUES (?, 'TRANSFER', ?, NULL, ?, ?, ?, ?, ?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "insert_transfer_row prepare: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, txn->amount_cents);
    sqlite3_bind_int64(stmt, 2, account_id);
    sqlite3_bind_text(stmt, 3, norm_date, -1, SQLITE_TRANSIENT);
    if (norm_reflection_date[0] != '\0')
        sqlite3_bind_text(stmt, 4, norm_reflection_date, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(stmt, 4);
    bind_text_or_null(stmt, 5, txn->payee);
    bind_text_or_null(stmt, 6, txn->description);
    if (transfer_id > 0)
        sqlite3_bind_int64(stmt, 7, transfer_id);
    else
        sqlite3_bind_null(stmt, 7);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "insert_transfer_row step: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    if (out_id)
        *out_id = sqlite3_last_insert_rowid(db);
    return 0;
}

static int parse_ymd(const char *date, int *out_y, int *out_m, int *out_d) {
    if (!date || !out_y || !out_m || !out_d)
        return -1;
    if (strlen(date) != 10)
        return -1;
    if (sscanf(date, "%4d-%2d-%2d", out_y, out_m, out_d) != 3)
        return -1;
    return 0;
}

static int date_add_one_day(char date[11]) {
    int y = 0, m = 0, d = 0;
    if (parse_ymd(date, &y, &m, &d) < 0)
        return -1;

    struct tm tmv = {0};
    tmv.tm_year = y - 1900;
    tmv.tm_mon = m - 1;
    tmv.tm_mday = d + 1;
    tmv.tm_hour = 12;
    tmv.tm_isdst = -1;
    if (mktime(&tmv) == (time_t)-1)
        return -1;

    if (strftime(date, 11, "%Y-%m-%d", &tmv) != 10)
        return -1;
    return 0;
}

static int normalize_budget_month(const char *src, char out[8]) {
    if (!src || !out)
        return -1;
    if (strlen(src) != 7 || src[4] != '-')
        return -1;

    int y = 0;
    int m = 0;
    if (sscanf(src, "%4d-%2d", &y, &m) != 2)
        return -1;
    if (y < 1900 || m < 1 || m > 12)
        return -1;

    snprintf(out, 8, "%04d-%02d", y, m);
    return 0;
}

static void compute_budget_utilization(budget_row_t *row) {
    if (!row)
        return;

    if (!row->has_rollup_rule || row->limit_cents <= 0) {
        row->utilization_bps = -1;
        return;
    }

    int64_t spent = row->net_spent_cents;
    if (spent < 0)
        spent = 0;

    if (spent > INT64_MAX / 10000) {
        row->utilization_bps = INT_MAX;
        return;
    }
    int64_t util = (spent * 10000) / row->limit_cents;
    if (util > INT_MAX)
        util = INT_MAX;
    row->utilization_bps = (int)util;
}

int db_get_accounts(sqlite3 *db, account_t **out) {
    *out = NULL;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT id, name, type, card_last4 FROM accounts ORDER BY name", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_get_accounts prepare: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    int capacity = 8;
    int count = 0;
    account_t *list = malloc(capacity * sizeof(account_t));
    if (!list) {
        sqlite3_finalize(stmt);
        return -1;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= capacity) {
            capacity *= 2;
            account_t *tmp = realloc(list, capacity * sizeof(account_t));
            if (!tmp) {
                free(list);
                sqlite3_finalize(stmt);
                return -1;
            }
            list = tmp;
        }
        list[count].id = sqlite3_column_int64(stmt, 0);
        const char *name = (const char *)sqlite3_column_text(stmt, 1);
        snprintf(list[count].name, sizeof(list[count].name), "%s", name ? name : "");
        const char *atype = (const char *)sqlite3_column_text(stmt, 2);
        list[count].type = account_type_from_str(atype);
        const char *cl4 = (const char *)sqlite3_column_text(stmt, 3);
        snprintf(list[count].card_last4, sizeof(list[count].card_last4), "%s", cl4 ? cl4 : "");
        count++;
    }

    sqlite3_finalize(stmt);
    *out = list;
    return count;
}

int64_t db_insert_account(sqlite3 *db, const char *name, account_type_t type, const char *card_last4) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO accounts (name, type, card_last4) VALUES (?, ?, ?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_insert_account prepare: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, account_type_db_strings[type], -1, SQLITE_STATIC);
    if (card_last4 && card_last4[0] != '\0')
        sqlite3_bind_text(stmt, 3, card_last4, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 3);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_DONE)
        return sqlite3_last_insert_rowid(db);
    if (rc == SQLITE_CONSTRAINT)
        return -2;
    fprintf(stderr, "db_insert_account step: %s\n", sqlite3_errmsg(db));
    return -1;
}

int db_update_account(sqlite3 *db, const account_t *account) {
    if (!account)
        return -1;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "UPDATE accounts SET name = ?, type = ?, card_last4 = ? WHERE id = ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_update_account prepare: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, account->name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, account_type_db_strings[account->type], -1,
                      SQLITE_STATIC);
    if (account->type == ACCOUNT_CREDIT_CARD && account->card_last4[0] != '\0')
        sqlite3_bind_text(stmt, 3, account->card_last4, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 3);
    sqlite3_bind_int64(stmt, 4, account->id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE)
        return 0;
    if (rc == SQLITE_CONSTRAINT)
        return -2;

    fprintf(stderr, "db_update_account step: %s\n", sqlite3_errmsg(db));
    return -1;
}

int db_count_transactions_for_account(sqlite3 *db, int64_t account_id) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db, "SELECT COUNT(*) FROM transactions WHERE account_id = ?", -1, &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_count_transactions_for_account prepare: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, account_id);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        fprintf(stderr, "db_count_transactions_for_account step: %s\n",
                sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }

    int count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

int db_count_uncategorized_by_payee(sqlite3 *db, const char *payee,
                                    transaction_type_t type,
                                    int64_t *out_count) {
    if (!payee || payee[0] == '\0' || !out_count || type == TRANSACTION_TRANSFER)
        return -1;

    *out_count = 0;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "SELECT COUNT(*) FROM transactions"
        " WHERE payee = ?"
        "   AND type = ?"
        "   AND category_id IS NULL",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_count_uncategorized_by_payee prepare: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, payee, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, transaction_type_to_str(type), -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        fprintf(stderr, "db_count_uncategorized_by_payee step: %s\n",
                sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }

    *out_count = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return 0;
}

int db_apply_category_to_uncategorized_by_payee(sqlite3 *db, const char *payee,
                                                transaction_type_t type,
                                                int64_t category_id) {
    if (!payee || payee[0] == '\0' || category_id <= 0 ||
        type == TRANSACTION_TRANSFER)
        return -1;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "UPDATE transactions"
        " SET category_id = ?"
        " WHERE payee = ?"
        "   AND type = ?"
        "   AND category_id IS NULL",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_apply_category_to_uncategorized_by_payee prepare: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, category_id);
    sqlite3_bind_text(stmt, 2, payee, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, transaction_type_to_str(type), -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_apply_category_to_uncategorized_by_payee step: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    return sqlite3_changes(db);
}

int db_get_most_recent_category_for_payee(sqlite3 *db, int64_t account_id,
                                          const char *payee,
                                          transaction_type_t type,
                                          int64_t *out_category_id) {
    if (!out_category_id)
        return -1;
    *out_category_id = 0;

    if (account_id <= 0 || !payee || payee[0] == '\0' ||
        type == TRANSACTION_TRANSFER)
        return 0;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "SELECT category_id FROM transactions"
        " WHERE account_id = ?"
        "   AND payee = ?"
        "   AND type = ?"
        " ORDER BY COALESCE(reflection_date, date) DESC, id DESC"
        " LIMIT 1",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_get_most_recent_category_for_payee prepare: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, account_id);
    sqlite3_bind_text(stmt, 2, payee, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, transaction_type_to_str(type), -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        if (sqlite3_column_type(stmt, 0) != SQLITE_NULL)
            *out_category_id = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return 0;
    }
    if (rc == SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return 0;
    }

    fprintf(stderr, "db_get_most_recent_category_for_payee step: %s\n",
            sqlite3_errmsg(db));
    sqlite3_finalize(stmt);
    return -1;
}

static int db_find_category_id(sqlite3 *db, category_type_t type,
                               const char *name, int64_t parent_id,
                               int64_t *out_id) {
    if (!db || !name || name[0] == '\0' || !out_id)
        return -1;

    const char *type_str = category_type_to_str(type);
    sqlite3_stmt *stmt = NULL;
    int rc = SQLITE_OK;

    if (parent_id > 0) {
        rc = sqlite3_prepare_v2(
            db,
            "SELECT id FROM categories"
            " WHERE type = ? AND name = ? AND parent_id = ?"
            " LIMIT 1",
            -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "db_find_category_id prepare child: %s\n",
                    sqlite3_errmsg(db));
            return -1;
        }
        sqlite3_bind_text(stmt, 1, type_str, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 3, parent_id);
    } else {
        rc = sqlite3_prepare_v2(
            db,
            "SELECT id FROM categories"
            " WHERE type = ? AND name = ? AND parent_id IS NULL"
            " LIMIT 1",
            -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "db_find_category_id prepare top-level: %s\n",
                    sqlite3_errmsg(db));
            return -1;
        }
        sqlite3_bind_text(stmt, 1, type_str, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *out_id = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return 1;
    }
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_find_category_id step: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }

    sqlite3_finalize(stmt);
    *out_id = 0;
    return 0;
}

int64_t db_get_or_create_category(sqlite3 *db, category_type_t type,
                                  const char *name, int64_t parent_id) {
    if (!db || !name || name[0] == '\0')
        return -1;

    int64_t existing_id = 0;
    int found = db_find_category_id(db, type, name, parent_id, &existing_id);
    if (found < 0)
        return -1;
    if (found == 1)
        return existing_id;

    const char *type_str = category_type_to_str(type);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "INSERT INTO categories (name, type, parent_id)"
        " VALUES (?, ?, ?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_get_or_create_category prepare: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, type_str, -1, SQLITE_STATIC);
    if (parent_id > 0)
        sqlite3_bind_int64(stmt, 3, parent_id);
    else
        sqlite3_bind_null(stmt, 3);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE)
        return sqlite3_last_insert_rowid(db);
    if (rc != SQLITE_CONSTRAINT) {
        fprintf(stderr, "db_get_or_create_category step: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    // Another caller may have inserted the same row before this insert.
    found = db_find_category_id(db, type, name, parent_id, &existing_id);
    if (found == 1)
        return existing_id;
    return -1;
}

int db_update_category(sqlite3 *db, const category_t *category) {
    if (!category || category->id <= 0 || category->name[0] == '\0')
        return -1;
    if (category->parent_id == category->id)
        return -1;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "UPDATE categories"
        " SET name = ?, type = ?, parent_id = ?"
        " WHERE id = ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_update_category prepare: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, category->name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, category_type_to_str(category->type), -1,
                      SQLITE_STATIC);
    if (category->parent_id > 0)
        sqlite3_bind_int64(stmt, 3, category->parent_id);
    else
        sqlite3_bind_null(stmt, 3);
    sqlite3_bind_int64(stmt, 4, category->id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE)
        return 0;
    if (rc == SQLITE_CONSTRAINT)
        return -2;

    fprintf(stderr, "db_update_category step: %s\n", sqlite3_errmsg(db));
    return -1;
}

int db_count_transactions_for_category(sqlite3 *db, int64_t category_id) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db, "SELECT COUNT(*) FROM transactions WHERE category_id = ?", -1, &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_count_transactions_for_category prepare: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, category_id);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        fprintf(stderr, "db_count_transactions_for_category step: %s\n",
                sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }

    int count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

int db_count_child_categories(sqlite3 *db, int64_t category_id) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db, "SELECT COUNT(*) FROM categories WHERE parent_id = ?", -1, &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_count_child_categories prepare: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, category_id);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        fprintf(stderr, "db_count_child_categories step: %s\n",
                sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }

    int count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

static int db_get_category_type_name(sqlite3 *db, int64_t category_id,
                                     char out[8]) {
    if (!out || category_id <= 0)
        return -1;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT type FROM categories WHERE id = ?",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_get_category_type_name prepare: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, category_id);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const char *type_name = (const char *)sqlite3_column_text(stmt, 0);
        if (!type_name || type_name[0] == '\0') {
            sqlite3_finalize(stmt);
            return -1;
        }
        snprintf(out, 8, "%s", type_name);
        sqlite3_finalize(stmt);
        return 0;
    }

    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE)
        return -2;

    fprintf(stderr, "db_get_category_type_name step: %s\n", sqlite3_errmsg(db));
    return -1;
}

int db_delete_category_with_reassignment(sqlite3 *db, int64_t category_id,
                                         int64_t replacement_category_id) {
    if (category_id <= 0)
        return -1;
    if (replacement_category_id == category_id)
        return -5;

    char source_type[8];
    int type_rc = db_get_category_type_name(db, category_id, source_type);
    if (type_rc == -2)
        return -2;
    if (type_rc < 0)
        return -1;

    int child_count = db_count_child_categories(db, category_id);
    if (child_count < 0)
        return -1;
    if (child_count > 0)
        return -4;

    if (replacement_category_id > 0) {
        char replacement_type[8];
        type_rc = db_get_category_type_name(db, replacement_category_id,
                                            replacement_type);
        if (type_rc == -2)
            return -5;
        if (type_rc < 0)
            return -1;
        if (strcmp(source_type, replacement_type) != 0)
            return -5;
    }

    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_delete_category_with_reassignment begin: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_stmt *stmt = NULL;
    if (replacement_category_id > 0) {
        rc = sqlite3_prepare_v2(
            db, "UPDATE transactions SET category_id = ? WHERE category_id = ?",
            -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            fprintf(stderr,
                    "db_delete_category_with_reassignment prepare reassign: %s\n",
                    sqlite3_errmsg(db));
            goto rollback;
        }
        sqlite3_bind_int64(stmt, 1, replacement_category_id);
        sqlite3_bind_int64(stmt, 2, category_id);
    } else {
        rc = sqlite3_prepare_v2(
            db, "UPDATE transactions SET category_id = NULL WHERE category_id = ?",
            -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            fprintf(stderr,
                    "db_delete_category_with_reassignment prepare clear: %s\n",
                    sqlite3_errmsg(db));
            goto rollback;
        }
        sqlite3_bind_int64(stmt, 1, category_id);
    }
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_delete_category_with_reassignment step update: %s\n",
                sqlite3_errmsg(db));
        goto rollback;
    }

    rc = sqlite3_prepare_v2(db, "DELETE FROM categories WHERE id = ?", -1, &stmt,
                            NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_delete_category_with_reassignment prepare delete: %s\n",
                sqlite3_errmsg(db));
        goto rollback;
    }
    sqlite3_bind_int64(stmt, 1, category_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_delete_category_with_reassignment step delete: %s\n",
                sqlite3_errmsg(db));
        goto rollback;
    }

    if (sqlite3_changes(db) == 0) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -2;
    }

    rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_delete_category_with_reassignment commit: %s\n",
                sqlite3_errmsg(db));
        goto rollback;
    }

    return 0;

rollback:
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    return -1;
}

int db_delete_category(sqlite3 *db, int64_t category_id) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT 1 FROM categories WHERE id = ?", -1,
                                &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_delete_category prepare exists: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, category_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE)
        return -2;
    if (rc != SQLITE_ROW) {
        fprintf(stderr, "db_delete_category step exists: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    int child_count = db_count_child_categories(db, category_id);
    if (child_count < 0)
        return -1;
    if (child_count > 0)
        return -4;

    int txn_count = db_count_transactions_for_category(db, category_id);
    if (txn_count < 0)
        return -1;
    if (txn_count > 0)
        return -3;

    rc = sqlite3_prepare_v2(db, "DELETE FROM categories WHERE id = ?", -1, &stmt,
                            NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_delete_category prepare delete: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, category_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_delete_category step delete: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }
    if (sqlite3_changes(db) == 0)
        return -2;

    return 0;
}

int db_get_account_balance_cents(sqlite3 *db, int64_t account_id,
                                 int64_t *out_cents) {
    if (!out_cents)
        return -1;
    *out_cents = 0;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "SELECT COALESCE(SUM(CASE"
        "  WHEN type = 'INCOME' THEN amount_cents"
        "  WHEN type = 'EXPENSE' THEN -amount_cents"
        "  WHEN type = 'TRANSFER' THEN CASE"
        "    WHEN transfer_id IS NOT NULL AND id = transfer_id THEN -amount_cents"
        "    ELSE amount_cents"
        "  END"
        "  ELSE 0"
        " END), 0)"
        " FROM transactions"
        " WHERE account_id = ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_get_account_balance_cents prepare: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, account_id);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        fprintf(stderr, "db_get_account_balance_cents step: %s\n",
                sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }

    *out_cents = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return 0;
}

int db_get_account_month_net_cents(sqlite3 *db, int64_t account_id,
                                   int64_t *out_cents) {
    if (!out_cents)
        return -1;
    *out_cents = 0;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "SELECT COALESCE(SUM(CASE"
        "  WHEN type = 'INCOME' THEN amount_cents"
        "  WHEN type = 'EXPENSE' THEN -amount_cents"
        "  ELSE 0"
        " END), 0)"
        " FROM transactions"
        " WHERE account_id = ?"
        "   AND transfer_id IS NULL"
        "   AND COALESCE(reflection_date, date) >= date('now', 'localtime', 'start of month')"
        "   AND COALESCE(reflection_date, date) <= date('now', 'localtime')",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_get_account_month_net_cents prepare: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, account_id);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        fprintf(stderr, "db_get_account_month_net_cents step: %s\n",
                sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }

    *out_cents = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return 0;
}

int db_get_account_month_income_cents(sqlite3 *db, int64_t account_id,
                                      int64_t *out_cents) {
    if (!out_cents)
        return -1;
    *out_cents = 0;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "SELECT COALESCE(SUM(amount_cents), 0)"
        " FROM transactions"
        " WHERE account_id = ?"
        "   AND type = 'INCOME'"
        "   AND transfer_id IS NULL"
        "   AND COALESCE(reflection_date, date) >= date('now', 'localtime', 'start of month')"
        "   AND COALESCE(reflection_date, date) <= date('now', 'localtime')",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_get_account_month_income_cents prepare: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, account_id);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        fprintf(stderr, "db_get_account_month_income_cents step: %s\n",
                sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }

    *out_cents = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return 0;
}

int db_get_account_month_expense_cents(sqlite3 *db, int64_t account_id,
                                       int64_t *out_cents) {
    if (!out_cents)
        return -1;
    *out_cents = 0;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "SELECT COALESCE(SUM(amount_cents), 0)"
        " FROM transactions"
        " WHERE account_id = ?"
        "   AND type = 'EXPENSE'"
        "   AND transfer_id IS NULL"
        "   AND COALESCE(reflection_date, date) >= date('now', 'localtime', 'start of month')"
        "   AND COALESCE(reflection_date, date) <= date('now', 'localtime')",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_get_account_month_expense_cents prepare: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, account_id);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        fprintf(stderr, "db_get_account_month_expense_cents step: %s\n",
                sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }

    *out_cents = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return 0;
}

int db_get_account_balance_series(sqlite3 *db, int64_t account_id,
                                  int lookback_days, balance_point_t **out) {
    if (!out || lookback_days <= 0)
        return -1;
    *out = NULL;

    balance_point_t *list =
        calloc((size_t)lookback_days, sizeof(balance_point_t));
    if (!list)
        return -1;

    char offset[32];
    snprintf(offset, sizeof(offset), "-%d days", lookback_days - 1);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db, "SELECT date('now', 'localtime', ?)", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_get_account_balance_series prepare: %s\n",
                sqlite3_errmsg(db));
        free(list);
        return -1;
    }

    sqlite3_bind_text(stmt, 1, offset, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        fprintf(stderr, "db_get_account_balance_series start step: %s\n",
                sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        free(list);
        return -1;
    }
    const char *start_date = (const char *)sqlite3_column_text(stmt, 0);
    char cur_date[11];
    snprintf(cur_date, sizeof(cur_date), "%s", start_date ? start_date : "");
    sqlite3_finalize(stmt);
    stmt = NULL;

    for (int i = 0; i < lookback_days; i++) {
        snprintf(list[i].date, sizeof(list[i].date), "%s", cur_date);
        if (i < lookback_days - 1 && date_add_one_day(cur_date) < 0) {
            free(list);
            return -1;
        }
    }

    rc = sqlite3_prepare_v2(
        db,
        "SELECT COALESCE(SUM(CASE"
        "  WHEN type = 'INCOME' THEN amount_cents"
        "  WHEN type = 'EXPENSE' THEN -amount_cents"
        "  WHEN type = 'TRANSFER' THEN CASE"
        "    WHEN transfer_id IS NOT NULL AND id = transfer_id THEN -amount_cents"
        "    ELSE amount_cents"
        "  END"
        "  ELSE 0"
        " END), 0)"
        " FROM transactions"
        " WHERE account_id = ?"
        "   AND COALESCE(reflection_date, date) < date('now', 'localtime', ?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_get_account_balance_series opening prepare: %s\n",
                sqlite3_errmsg(db));
        free(list);
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, account_id);
    sqlite3_bind_text(stmt, 2, offset, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        fprintf(stderr, "db_get_account_balance_series opening step: %s\n",
                sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        free(list);
        return -1;
    }
    int64_t opening_balance = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    stmt = NULL;

    rc = sqlite3_prepare_v2(
        db,
        "SELECT COALESCE(reflection_date, date),"
        "       COALESCE(SUM(CASE"
        "         WHEN type = 'INCOME' THEN amount_cents"
        "         WHEN type = 'EXPENSE' THEN -amount_cents"
        "         WHEN type = 'TRANSFER' THEN CASE"
        "           WHEN transfer_id IS NOT NULL AND id = transfer_id THEN -amount_cents"
        "           ELSE amount_cents"
        "         END"
        "         ELSE 0"
        "       END), 0)"
        " FROM transactions"
        " WHERE account_id = ?"
        "   AND COALESCE(reflection_date, date) >= date('now', 'localtime', ?)"
        "   AND COALESCE(reflection_date, date) <= date('now', 'localtime')"
        " GROUP BY COALESCE(reflection_date, date)"
        " ORDER BY COALESCE(reflection_date, date)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_get_account_balance_series deltas prepare: %s\n",
                sqlite3_errmsg(db));
        free(list);
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, account_id);
    sqlite3_bind_text(stmt, 2, offset, -1, SQLITE_TRANSIENT);

    int idx = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *date = (const char *)sqlite3_column_text(stmt, 0);
        if (!date)
            continue;
        int64_t net_cents = sqlite3_column_int64(stmt, 1);
        while (idx < lookback_days && strcmp(list[idx].date, date) < 0)
            idx++;
        if (idx < lookback_days && strcmp(list[idx].date, date) == 0)
            list[idx].balance_cents += net_cents;
    }
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_get_account_balance_series deltas step: %s\n",
                sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        free(list);
        return -1;
    }
    sqlite3_finalize(stmt);

    int64_t running = opening_balance;
    for (int i = 0; i < lookback_days; i++) {
        running += list[i].balance_cents;
        list[i].balance_cents = running;
    }

    *out = list;
    return lookback_days;
}

int db_delete_account(sqlite3 *db, int64_t account_id, bool delete_transactions) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT 1 FROM accounts WHERE id = ?", -1,
                                &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_delete_account prepare exists: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, account_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE)
        return -2;
    if (rc != SQLITE_ROW) {
        fprintf(stderr, "db_delete_account step exists: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    int txn_count = db_count_transactions_for_account(db, account_id);
    if (txn_count < 0)
        return -1;
    if (txn_count > 0 && !delete_transactions)
        return -3;

    rc = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_delete_account begin: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    if (delete_transactions && txn_count > 0) {
        rc = sqlite3_prepare_v2(db, "DELETE FROM transactions WHERE account_id = ?",
                                -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "db_delete_account prepare txns: %s\n",
                    sqlite3_errmsg(db));
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            return -1;
        }
        sqlite3_bind_int64(stmt, 1, account_id);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        stmt = NULL;
        if (rc != SQLITE_DONE) {
            fprintf(stderr, "db_delete_account step txns: %s\n", sqlite3_errmsg(db));
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            return -1;
        }
    }

    rc = sqlite3_prepare_v2(db, "DELETE FROM accounts WHERE id = ?", -1, &stmt,
                            NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_delete_account prepare account: %s\n",
                sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, account_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_delete_account step account: %s\n", sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }

    if (sqlite3_changes(db) == 0) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -2;
    }

    rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_delete_account commit: %s\n", sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }

    return 0;
}

int db_get_categories(sqlite3 *db, category_type_t type, category_t **out) {
    *out = NULL;

    const char *type_str = category_type_to_str(type);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT c.id,"
        "  CASE WHEN p.name IS NOT NULL THEN p.name || ':' || c.name ELSE c.name END,"
        "  c.type, c.parent_id"
        " FROM categories c"
        " LEFT JOIN categories p ON c.parent_id = p.id"
        " WHERE c.type = ?"
        " ORDER BY 2",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_get_categories prepare: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, type_str, -1, SQLITE_STATIC);

    int capacity = 16;
    int count = 0;
    category_t *list = malloc(capacity * sizeof(category_t));
    if (!list) {
        sqlite3_finalize(stmt);
        return -1;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= capacity) {
            capacity *= 2;
            category_t *tmp = realloc(list, capacity * sizeof(category_t));
            if (!tmp) {
                free(list);
                sqlite3_finalize(stmt);
                return -1;
            }
            list = tmp;
        }
        list[count].id = sqlite3_column_int64(stmt, 0);
        const char *name = (const char *)sqlite3_column_text(stmt, 1);
        snprintf(list[count].name, sizeof(list[count].name), "%s", name ? name : "");
        const char *ctype = (const char *)sqlite3_column_text(stmt, 2);
        list[count].type = (ctype && strcmp(ctype, "INCOME") == 0)
            ? CATEGORY_INCOME : CATEGORY_EXPENSE;
        list[count].parent_id = sqlite3_column_int64(stmt, 3);
        count++;
    }

    sqlite3_finalize(stmt);
    *out = list;
    return count;
}

int db_get_transactions(sqlite3 *db, int64_t account_id, txn_row_t **out) {
    *out = NULL;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT t.id, t.amount_cents, t.type, t.date,"
        "  COALESCE(t.reflection_date, ''),"
        "  COALESCE(t.reflection_date, t.date),"
        "  CASE"
        "    WHEN t.type = 'TRANSFER' THEN COALESCE(ta.name, '(transfer)')"
        "    WHEN p.name IS NOT NULL THEN p.name || ':' || c.name"
        "    ELSE COALESCE(c.name, '')"
        "  END,"
        "  COALESCE(t.payee, ''),"
        "  COALESCE(t.description, '')"
        " FROM transactions t"
        " LEFT JOIN categories c ON t.category_id = c.id"
        " LEFT JOIN categories p ON c.parent_id = p.id"
        " LEFT JOIN transactions tt ON tt.id = ("
        "   SELECT t2.id FROM transactions t2"
        "   WHERE t2.transfer_id = t.transfer_id AND t2.id != t.id"
        "   LIMIT 1)"
        " LEFT JOIN accounts ta ON ta.id = tt.account_id"
        " WHERE t.account_id = ?"
        " ORDER BY COALESCE(t.reflection_date, t.date) DESC, t.id DESC",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_get_transactions prepare: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, account_id);

    int capacity = 32;
    int count = 0;
    txn_row_t *list = malloc(capacity * sizeof(txn_row_t));
    if (!list) {
        sqlite3_finalize(stmt);
        return -1;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= capacity) {
            capacity *= 2;
            txn_row_t *tmp = realloc(list, capacity * sizeof(txn_row_t));
            if (!tmp) {
                free(list);
                sqlite3_finalize(stmt);
                return -1;
            }
            list = tmp;
        }
        list[count].id = sqlite3_column_int64(stmt, 0);
        list[count].amount_cents = sqlite3_column_int64(stmt, 1);
        const char *ttype = (const char *)sqlite3_column_text(stmt, 2);
        list[count].type = transaction_type_from_str(ttype);
        const char *date = (const char *)sqlite3_column_text(stmt, 3);
        snprintf(list[count].date, sizeof(list[count].date), "%s", date ? date : "");
        const char *reflection_date = (const char *)sqlite3_column_text(stmt, 4);
        snprintf(list[count].reflection_date,
                 sizeof(list[count].reflection_date), "%s",
                 reflection_date ? reflection_date : "");
        const char *effective_date = (const char *)sqlite3_column_text(stmt, 5);
        snprintf(list[count].effective_date, sizeof(list[count].effective_date),
                 "%s", effective_date ? effective_date : "");
        const char *cat = (const char *)sqlite3_column_text(stmt, 6);
        snprintf(list[count].category_name, sizeof(list[count].category_name), "%s", cat ? cat : "");
        const char *payee = (const char *)sqlite3_column_text(stmt, 7);
        snprintf(list[count].payee, sizeof(list[count].payee), "%s", payee ? payee : "");
        const char *desc = (const char *)sqlite3_column_text(stmt, 8);
        snprintf(list[count].description, sizeof(list[count].description), "%s", desc ? desc : "");
        count++;
    }

    sqlite3_finalize(stmt);
    *out = list;
    return count;
}

int db_get_report_rows(sqlite3 *db, report_group_t group, report_period_t period,
                       report_row_t **out) {
    if (!out)
        return -1;
    *out = NULL;

    const char *start_expr = report_period_start_expr(period);
    if (!start_expr)
        return -1;

    const char *label_expr = NULL;
    const char *join_clause = "";
    if (group == REPORT_GROUP_CATEGORY) {
        label_expr =
            "CASE"
            "  WHEN c.id IS NULL THEN 'Uncategorized'"
            "  WHEN p.name IS NOT NULL THEN p.name || ':' || c.name"
            "  ELSE c.name"
            " END";
        join_clause =
            " LEFT JOIN categories c ON c.id = t.category_id"
            " LEFT JOIN categories p ON p.id = c.parent_id";
    } else if (group == REPORT_GROUP_PAYEE) {
        label_expr =
            "CASE"
            "  WHEN t.payee IS NULL OR trim(t.payee) = '' THEN '(No payee)'"
            "  ELSE t.payee"
            " END";
    } else {
        return -1;
    }

    char sql[4096];
    snprintf(
        sql, sizeof(sql),
        "SELECT %s AS label,"
        "       COALESCE(SUM(CASE WHEN t.type = 'EXPENSE' THEN t.amount_cents ELSE 0 END), 0),"
        "       COALESCE(SUM(CASE WHEN t.type = 'INCOME' THEN t.amount_cents ELSE 0 END), 0),"
        "       COALESCE(SUM(CASE WHEN t.type = 'INCOME' THEN t.amount_cents ELSE -t.amount_cents END), 0),"
        "       COUNT(*)"
        " FROM transactions t"
        "%s"
        " WHERE t.type IN ('EXPENSE', 'INCOME')"
        "   AND COALESCE(t.reflection_date, t.date) >= %s"
        "   AND COALESCE(t.reflection_date, t.date) <= date('now', 'localtime')"
        " GROUP BY label"
        " ORDER BY label COLLATE NOCASE",
        label_expr, join_clause, start_expr);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_get_report_rows prepare: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    int capacity = 32;
    int count = 0;
    report_row_t *list = malloc((size_t)capacity * sizeof(report_row_t));
    if (!list) {
        sqlite3_finalize(stmt);
        return -1;
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (count >= capacity) {
            capacity *= 2;
            report_row_t *tmp =
                realloc(list, (size_t)capacity * sizeof(report_row_t));
            if (!tmp) {
                free(list);
                sqlite3_finalize(stmt);
                return -1;
            }
            list = tmp;
        }

        memset(&list[count], 0, sizeof(report_row_t));
        const char *label = (const char *)sqlite3_column_text(stmt, 0);
        snprintf(list[count].label, sizeof(list[count].label), "%s",
                 label ? label : "");
        list[count].expense_cents = sqlite3_column_int64(stmt, 1);
        list[count].income_cents = sqlite3_column_int64(stmt, 2);
        list[count].net_cents = sqlite3_column_int64(stmt, 3);
        list[count].txn_count = sqlite3_column_int(stmt, 4);
        count++;
    }

    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_get_report_rows step: %s\n", sqlite3_errmsg(db));
        free(list);
        return -1;
    }

    *out = list;
    return count;
}

int db_get_budget_transactions_for_month(sqlite3 *db, int64_t category_id,
                                         const char *month_ym,
                                         budget_txn_row_t **out) {
    if (!out || category_id <= 0)
        return -1;
    *out = NULL;

    char norm_month[8];
    if (normalize_budget_month(month_ym, norm_month) < 0)
        return -1;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "WITH RECURSIVE"
        " descendants(category_id) AS ("
        "   SELECT id FROM categories WHERE id = ?"
        "   UNION ALL"
        "   SELECT c.id FROM categories c"
        "   JOIN descendants d ON c.parent_id = d.category_id"
        " ),"
        " selected_descendants(category_id) AS ("
        "   SELECT category_id FROM budget_category_filters"
        "   UNION"
        "   SELECT c.id FROM categories c"
        "   JOIN selected_descendants sd ON c.parent_id = sd.category_id"
        " ),"
        " filter_mode(include_selected) AS ("
        "   SELECT CASE"
        "     WHEN EXISTS("
        "       SELECT 1 FROM budget_filter_settings bfs"
        "       WHERE bfs.id = 1 AND bfs.mode = 'INCLUDE_SELECTED'"
        "     ) THEN 1 ELSE 0 END"
        " ),"
        " allowed_categories(category_id) AS ("
        "   SELECT c.id"
        "   FROM categories c"
        "   CROSS JOIN filter_mode fm"
        "   WHERE (fm.include_selected = 0"
        "          AND c.id NOT IN (SELECT category_id FROM selected_descendants))"
        "      OR (fm.include_selected = 1"
        "          AND c.id IN (SELECT category_id FROM selected_descendants))"
        " )"
        " SELECT t.id, t.amount_cents, t.type,"
        "        COALESCE(t.reflection_date, t.date),"
        "        COALESCE(a.name, ''),"
        "        CASE"
        "          WHEN p.name IS NOT NULL THEN p.name || ':' || c.name"
        "          ELSE COALESCE(c.name, '')"
        "        END,"
        "        COALESCE(t.payee, ''),"
        "        COALESCE(t.description, '')"
        " FROM transactions t"
        " JOIN descendants d ON d.category_id = t.category_id"
        " JOIN allowed_categories ac ON ac.category_id = t.category_id"
        " LEFT JOIN accounts a ON a.id = t.account_id"
        " LEFT JOIN categories c ON c.id = t.category_id"
        " LEFT JOIN categories p ON p.id = c.parent_id"
        " WHERE substr(COALESCE(t.reflection_date, t.date), 1, 7) = ?"
        "   AND t.type IN ('EXPENSE', 'INCOME')"
        " ORDER BY COALESCE(t.reflection_date, t.date) DESC, t.id DESC",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_get_budget_transactions_for_month prepare: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, category_id);
    sqlite3_bind_text(stmt, 2, norm_month, -1, SQLITE_TRANSIENT);

    int capacity = 32;
    int count = 0;
    budget_txn_row_t *list =
        malloc((size_t)capacity * sizeof(budget_txn_row_t));
    if (!list) {
        sqlite3_finalize(stmt);
        return -1;
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (count >= capacity) {
            capacity *= 2;
            budget_txn_row_t *tmp = realloc(
                list, (size_t)capacity * sizeof(budget_txn_row_t));
            if (!tmp) {
                free(list);
                sqlite3_finalize(stmt);
                return -1;
            }
            list = tmp;
        }

        memset(&list[count], 0, sizeof(budget_txn_row_t));
        list[count].id = sqlite3_column_int64(stmt, 0);
        list[count].amount_cents = sqlite3_column_int64(stmt, 1);
        const char *ttype = (const char *)sqlite3_column_text(stmt, 2);
        list[count].type = transaction_type_from_str(ttype);
        const char *effective_date = (const char *)sqlite3_column_text(stmt, 3);
        snprintf(list[count].effective_date, sizeof(list[count].effective_date),
                 "%s", effective_date ? effective_date : "");
        const char *account_name = (const char *)sqlite3_column_text(stmt, 4);
        snprintf(list[count].account_name, sizeof(list[count].account_name),
                 "%s", account_name ? account_name : "");
        const char *category_name = (const char *)sqlite3_column_text(stmt, 5);
        snprintf(list[count].category_name, sizeof(list[count].category_name),
                 "%s", category_name ? category_name : "");
        const char *payee = (const char *)sqlite3_column_text(stmt, 6);
        snprintf(list[count].payee, sizeof(list[count].payee), "%s",
                 payee ? payee : "");
        const char *description = (const char *)sqlite3_column_text(stmt, 7);
        snprintf(list[count].description, sizeof(list[count].description), "%s",
                 description ? description : "");
        count++;
    }

    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_get_budget_transactions_for_month step: %s\n",
                sqlite3_errmsg(db));
        free(list);
        return -1;
    }

    if (count == 0) {
        free(list);
        return 0;
    }

    *out = list;
    return count;
}

int64_t db_insert_transaction(sqlite3 *db, const transaction_t *txn) {
    char norm_date[11];
    if (normalize_txn_date(txn->date, norm_date) < 0)
        return -1;
    char norm_reflection_date[11];
    if (normalize_optional_txn_date(txn->reflection_date, norm_reflection_date) <
        0)
        return -1;

    const char *type_str = transaction_type_to_str(txn->type);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO transactions (amount_cents, type, account_id, category_id, date, reflection_date, payee, description)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_insert_transaction prepare: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, txn->amount_cents);
    sqlite3_bind_text(stmt, 2, type_str, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, txn->account_id);
    if (txn->category_id > 0)
        sqlite3_bind_int64(stmt, 4, txn->category_id);
    else
        sqlite3_bind_null(stmt, 4);
    sqlite3_bind_text(stmt, 5, norm_date, -1, SQLITE_TRANSIENT);
    if (norm_reflection_date[0] != '\0')
        sqlite3_bind_text(stmt, 6, norm_reflection_date, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(stmt, 6);
    if (txn->payee[0] != '\0')
        sqlite3_bind_text(stmt, 7, txn->payee, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 7);
    if (txn->description[0] != '\0')
        sqlite3_bind_text(stmt, 8, txn->description, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 8);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_insert_transaction step: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    return sqlite3_last_insert_rowid(db);
}

int64_t db_insert_transfer(sqlite3 *db, const transaction_t *txn,
                           int64_t to_account_id) {
    if (!txn)
        return -1;
    if (txn->account_id <= 0 || to_account_id <= 0 || txn->account_id == to_account_id)
        return -2;

    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_insert_transfer begin: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    int64_t from_id = 0;
    if (insert_transfer_row(db, txn, txn->account_id, 0, &from_id) < 0)
        goto rollback;

    int64_t to_id = 0;
    if (insert_transfer_row(db, txn, to_account_id, from_id, &to_id) < 0)
        goto rollback;

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db,
                            "UPDATE transactions SET transfer_id = ? WHERE id = ?",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_insert_transfer prepare update source: %s\n",
                sqlite3_errmsg(db));
        goto rollback;
    }
    sqlite3_bind_int64(stmt, 1, from_id);
    sqlite3_bind_int64(stmt, 2, from_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_insert_transfer step update source: %s\n",
                sqlite3_errmsg(db));
        goto rollback;
    }

    rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_insert_transfer commit: %s\n", sqlite3_errmsg(db));
        goto rollback;
    }

    (void)to_id;
    return from_id;

rollback:
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    return -1;
}

int db_get_transaction_by_id(sqlite3 *db, int txn_id, transaction_t *out) {
    if (!out) return -1;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT id, amount_cents, type, account_id, category_id, date, reflection_date, payee, description, transfer_id"
        " FROM transactions WHERE id = ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_get_transaction_by_id prepare: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int(stmt, 1, txn_id);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        out->id = sqlite3_column_int64(stmt, 0);
        out->amount_cents = sqlite3_column_int64(stmt, 1);
        const char *ttype = (const char *)sqlite3_column_text(stmt, 2);
        out->type = transaction_type_from_str(ttype);
        out->account_id = sqlite3_column_int64(stmt, 3);
        if (sqlite3_column_type(stmt, 4) == SQLITE_NULL)
            out->category_id = 0;
        else
            out->category_id = sqlite3_column_int64(stmt, 4);
        const char *date = (const char *)sqlite3_column_text(stmt, 5);
        snprintf(out->date, sizeof(out->date), "%s", date ? date : "");
        const char *reflection_date = (const char *)sqlite3_column_text(stmt, 6);
        snprintf(out->reflection_date, sizeof(out->reflection_date), "%s",
                 reflection_date ? reflection_date : "");
        const char *payee = (const char *)sqlite3_column_text(stmt, 7);
        snprintf(out->payee, sizeof(out->payee), "%s", payee ? payee : "");
        const char *desc = (const char *)sqlite3_column_text(stmt, 8);
        snprintf(out->description, sizeof(out->description), "%s", desc ? desc : "");
        if (sqlite3_column_type(stmt, 9) == SQLITE_NULL)
            out->transfer_id = 0;
        else
            out->transfer_id = sqlite3_column_int64(stmt, 9);
        out->created_at = 0;
        sqlite3_finalize(stmt);
        return 0;
    }

    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE)
        return -2;

    fprintf(stderr, "db_get_transaction_by_id step: %s\n", sqlite3_errmsg(db));
    return -1;
}

int db_get_transfer_counterparty_account(sqlite3 *db, int64_t txn_id,
                                         int64_t *out_account_id) {
    if (!out_account_id)
        return -1;
    *out_account_id = 0;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "SELECT t2.account_id"
        " FROM transactions t1"
        " JOIN transactions t2 ON t2.transfer_id = t1.transfer_id"
        "  AND t2.id != t1.id"
        " WHERE t1.id = ? AND t1.transfer_id IS NOT NULL"
        " LIMIT 1",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_get_transfer_counterparty_account prepare: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, txn_id);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *out_account_id = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return 0;
    }

    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE)
        return -2;
    fprintf(stderr, "db_get_transfer_counterparty_account step: %s\n",
            sqlite3_errmsg(db));
    return -1;
}

int db_update_transfer(sqlite3 *db, const transaction_t *txn,
                       int64_t to_account_id, bool allow_existing_match) {
    if (!txn)
        return -1;
    if (txn->account_id <= 0 || to_account_id <= 0 || txn->account_id == to_account_id)
        return -3;
    char norm_date[11];
    if (normalize_txn_date(txn->date, norm_date) < 0)
        return -1;
    char norm_reflection_date[11];
    if (normalize_optional_txn_date(txn->reflection_date, norm_reflection_date) <
        0)
        return -1;

    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_update_transfer begin: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(
        db, "SELECT transfer_id, type FROM transactions WHERE id = ?", -1, &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_update_transfer prepare load: %s\n", sqlite3_errmsg(db));
        goto rollback;
    }
    sqlite3_bind_int64(stmt, 1, txn->id);

    int64_t source_id = 0;
    transaction_type_t source_type = txn->type;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        if (sqlite3_column_type(stmt, 0) != SQLITE_NULL)
            source_id = sqlite3_column_int64(stmt, 0);
        const char *row_type = (const char *)sqlite3_column_text(stmt, 1);
        if (row_type && strcmp(row_type, "INCOME") == 0)
            source_type = TRANSACTION_INCOME;
        else if (row_type && strcmp(row_type, "EXPENSE") == 0)
            source_type = TRANSACTION_EXPENSE;
        else if (row_type && strcmp(row_type, "TRANSFER") == 0)
            source_type = TRANSACTION_TRANSFER;
    } else if (rc == SQLITE_DONE) {
        sqlite3_finalize(stmt);
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -2;
    } else {
        fprintf(stderr, "db_update_transfer step load: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        goto rollback;
    }
    sqlite3_finalize(stmt);
    stmt = NULL;

    if (source_id <= 0)
        source_id = txn->id;

    // transfer_id is the canonical source row id for transfer direction.
    if (source_id != txn->id) {
        rc = sqlite3_prepare_v2(db, "SELECT 1 FROM transactions WHERE id = ?",
                                -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "db_update_transfer prepare source exists: %s\n",
                    sqlite3_errmsg(db));
            goto rollback;
        }
        sqlite3_bind_int64(stmt, 1, source_id);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        stmt = NULL;
        if (rc == SQLITE_DONE) {
            source_id = txn->id;
        } else if (rc != SQLITE_ROW) {
            fprintf(stderr, "db_update_transfer step source exists: %s\n",
                    sqlite3_errmsg(db));
            goto rollback;
        }
    }

    int64_t mirror_id = 0;
    rc = sqlite3_prepare_v2(
        db,
        "SELECT id FROM transactions"
        " WHERE transfer_id = ? AND id != ?"
        " ORDER BY CASE WHEN account_id = ? THEN 0 ELSE 1 END, id"
        " LIMIT 1",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_update_transfer prepare mirror: %s\n", sqlite3_errmsg(db));
        goto rollback;
    }
    sqlite3_bind_int64(stmt, 1, source_id);
    sqlite3_bind_int64(stmt, 2, source_id);
    sqlite3_bind_int64(stmt, 3, to_account_id);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        mirror_id = sqlite3_column_int64(stmt, 0);
    } else if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_update_transfer step mirror: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        goto rollback;
    }
    sqlite3_finalize(stmt);
    stmt = NULL;

    if (mirror_id <= 0 && allow_existing_match) {
        rc = sqlite3_prepare_v2(
            db,
            "SELECT id, type FROM transactions"
            " WHERE account_id = ?"
            "   AND id != ?"
            "   AND transfer_id IS NULL"
            "   AND type != 'TRANSFER'"
            "   AND amount_cents = ?"
            "   AND ABS(julianday(date) - julianday(?)) <= ?"
            " ORDER BY ABS(julianday(date) - julianday(?)) ASC, id DESC",
            -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "db_update_transfer prepare match existing: %s\n",
                    sqlite3_errmsg(db));
            goto rollback;
        }
        sqlite3_bind_int64(stmt, 1, to_account_id);
        sqlite3_bind_int64(stmt, 2, source_id);
        sqlite3_bind_int64(stmt, 3, txn->amount_cents);
        sqlite3_bind_text(stmt, 4, norm_date, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 5, transfer_match_date_window_days);
        sqlite3_bind_text(stmt, 6, norm_date, -1, SQLITE_TRANSIENT);

        int total = 0;
        int opposite_count = 0;
        int64_t first_match_id = 0;
        int64_t opposite_match_id = 0;

        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            int64_t match_id = sqlite3_column_int64(stmt, 0);
            const char *row_type = (const char *)sqlite3_column_text(stmt, 1);
            transaction_type_t match_type =
                (row_type && strcmp(row_type, "INCOME") == 0)
                    ? TRANSACTION_INCOME
                    : TRANSACTION_EXPENSE;

            total++;
            if (total == 1)
                first_match_id = match_id;

            if ((source_type == TRANSACTION_EXPENSE &&
                 match_type == TRANSACTION_INCOME) ||
                (source_type == TRANSACTION_INCOME &&
                 match_type == TRANSACTION_EXPENSE)) {
                opposite_count++;
                if (opposite_count == 1)
                    opposite_match_id = match_id;
            }
        }

        sqlite3_finalize(stmt);
        stmt = NULL;
        if (rc != SQLITE_DONE) {
            fprintf(stderr, "db_update_transfer step match existing: %s\n",
                    sqlite3_errmsg(db));
            goto rollback;
        }

        if (total == 1) {
            mirror_id = first_match_id;
        } else if (opposite_count == 1) {
            mirror_id = opposite_match_id;
        }
    }

    rc = sqlite3_prepare_v2(
        db,
        "UPDATE transactions"
        " SET amount_cents = ?, type = 'TRANSFER', account_id = ?, category_id = NULL,"
        "     date = ?, reflection_date = ?, payee = ?, description = ?, transfer_id = ?"
        " WHERE id = ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_update_transfer prepare source: %s\n", sqlite3_errmsg(db));
        goto rollback;
    }
    sqlite3_bind_int64(stmt, 1, txn->amount_cents);
    sqlite3_bind_int64(stmt, 2, txn->account_id);
    sqlite3_bind_text(stmt, 3, norm_date, -1, SQLITE_TRANSIENT);
    if (norm_reflection_date[0] != '\0')
        sqlite3_bind_text(stmt, 4, norm_reflection_date, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(stmt, 4);
    bind_text_or_null(stmt, 5, txn->payee);
    bind_text_or_null(stmt, 6, txn->description);
    sqlite3_bind_int64(stmt, 7, source_id);
    sqlite3_bind_int64(stmt, 8, source_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_update_transfer step source: %s\n", sqlite3_errmsg(db));
        goto rollback;
    }

    if (mirror_id > 0) {
        rc = sqlite3_prepare_v2(
            db,
            "UPDATE transactions"
            " SET amount_cents = ?, type = 'TRANSFER', account_id = ?, category_id = NULL,"
            "     date = ?, reflection_date = ?, payee = ?, description = ?, transfer_id = ?"
            " WHERE id = ?",
            -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "db_update_transfer prepare mirror update: %s\n",
                    sqlite3_errmsg(db));
            goto rollback;
        }
        sqlite3_bind_int64(stmt, 1, txn->amount_cents);
        sqlite3_bind_int64(stmt, 2, to_account_id);
        sqlite3_bind_text(stmt, 3, norm_date, -1, SQLITE_TRANSIENT);
        if (norm_reflection_date[0] != '\0')
            sqlite3_bind_text(stmt, 4, norm_reflection_date, -1,
                              SQLITE_TRANSIENT);
        else
            sqlite3_bind_null(stmt, 4);
        bind_text_or_null(stmt, 5, txn->payee);
        bind_text_or_null(stmt, 6, txn->description);
        sqlite3_bind_int64(stmt, 7, source_id);
        sqlite3_bind_int64(stmt, 8, mirror_id);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        stmt = NULL;
        if (rc != SQLITE_DONE) {
            fprintf(stderr, "db_update_transfer step mirror update: %s\n",
                    sqlite3_errmsg(db));
            goto rollback;
        }
    } else {
        if (insert_transfer_row(db, txn, to_account_id, source_id, NULL) < 0)
            goto rollback;
    }

    rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_update_transfer commit: %s\n", sqlite3_errmsg(db));
        goto rollback;
    }

    return 0;

rollback:
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    return -1;
}

int db_delete_transaction(sqlite3 *db, int txn_id) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT transfer_id FROM transactions WHERE id = ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_delete_transaction prepare: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int(stmt, 1, txn_id);

    int64_t transfer_id = 0;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        if (sqlite3_column_type(stmt, 0) != SQLITE_NULL)
            transfer_id = sqlite3_column_int64(stmt, 0);
    } else if (rc == SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return -2;
    } else {
        fprintf(stderr, "db_delete_transaction step: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }

    sqlite3_finalize(stmt);
    stmt = NULL;

    if (transfer_id == 0) {
        rc = sqlite3_prepare_v2(db,
            "DELETE FROM transactions WHERE id = ?",
            -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "db_delete_transaction prepare delete: %s\n", sqlite3_errmsg(db));
            return -1;
        }
        sqlite3_bind_int(stmt, 1, txn_id);
    } else {
        rc = sqlite3_prepare_v2(db,
            "DELETE FROM transactions WHERE transfer_id = ?",
            -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "db_delete_transaction prepare delete: %s\n", sqlite3_errmsg(db));
            return -1;
        }
        sqlite3_bind_int64(stmt, 1, transfer_id);
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_delete_transaction step delete: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    return 0;
}

int db_update_transaction(sqlite3 *db, const transaction_t *txn) {
    if (!txn) return -1;
    char norm_date[11];
    if (normalize_txn_date(txn->date, norm_date) < 0)
        return -1;
    char norm_reflection_date[11];
    if (normalize_optional_txn_date(txn->reflection_date, norm_reflection_date) <
        0)
        return -1;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT transfer_id, account_id FROM transactions WHERE id = ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_update_transaction prepare: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, txn->id);

    int64_t old_transfer_id = 0;
    int64_t old_account_id = 0;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        if (sqlite3_column_type(stmt, 0) != SQLITE_NULL)
            old_transfer_id = sqlite3_column_int64(stmt, 0);
        old_account_id = sqlite3_column_int64(stmt, 1);
    } else if (rc == SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return -2;
    } else {
        fprintf(stderr, "db_update_transaction step: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }

    sqlite3_finalize(stmt);
    stmt = NULL;

    transaction_t normalized = *txn;
    snprintf(normalized.date, sizeof(normalized.date), "%s", norm_date);
    snprintf(normalized.reflection_date, sizeof(normalized.reflection_date), "%s",
             norm_reflection_date);
    if (normalized.transfer_id != 0) {
        normalized.type = TRANSACTION_TRANSFER;
        normalized.category_id = 0;
    }
    if (normalized.type != TRANSACTION_TRANSFER) {
        normalized.transfer_id = 0;
    }

    const char *type_str = transaction_type_to_str(normalized.type);

    rc = sqlite3_prepare_v2(db,
        "UPDATE transactions"
        " SET amount_cents = ?, type = ?, account_id = ?, category_id = ?, date = ?, reflection_date = ?, payee = ?, description = ?, transfer_id = ?"
        " WHERE id = ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_update_transaction prepare update: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, normalized.amount_cents);
    sqlite3_bind_text(stmt, 2, type_str, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, normalized.account_id);
    if (normalized.category_id > 0)
        sqlite3_bind_int64(stmt, 4, normalized.category_id);
    else
        sqlite3_bind_null(stmt, 4);
    sqlite3_bind_text(stmt, 5, normalized.date, -1, SQLITE_STATIC);
    if (normalized.reflection_date[0] != '\0')
        sqlite3_bind_text(stmt, 6, normalized.reflection_date, -1,
                          SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 6);
    if (normalized.payee[0] != '\0')
        sqlite3_bind_text(stmt, 7, normalized.payee, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 7);
    if (normalized.description[0] != '\0')
        sqlite3_bind_text(stmt, 8, normalized.description, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 8);
    if (normalized.transfer_id > 0)
        sqlite3_bind_int64(stmt, 9, normalized.transfer_id);
    else
        sqlite3_bind_null(stmt, 9);
    sqlite3_bind_int64(stmt, 10, normalized.id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_update_transaction step update: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    if (normalized.transfer_id != 0) {
        int count = 0;
        rc = sqlite3_prepare_v2(db,
            "SELECT COUNT(*) FROM transactions WHERE transfer_id = ?",
            -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "db_update_transaction prepare count: %s\n", sqlite3_errmsg(db));
            return -1;
        }
        sqlite3_bind_int64(stmt, 1, normalized.transfer_id);
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW)
            count = sqlite3_column_int(stmt, 0);
        else {
            fprintf(stderr, "db_update_transaction step count: %s\n", sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            return -1;
        }
        sqlite3_finalize(stmt);
        stmt = NULL;

        if (count == 1) {
            rc = sqlite3_prepare_v2(db,
                "UPDATE transactions SET transfer_id = NULL WHERE id = ?",
                -1, &stmt, NULL);
            if (rc != SQLITE_OK) {
                fprintf(stderr, "db_update_transaction prepare heal: %s\n", sqlite3_errmsg(db));
                return -1;
            }
            sqlite3_bind_int64(stmt, 1, normalized.id);
            rc = sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            stmt = NULL;
            if (rc != SQLITE_DONE) {
                fprintf(stderr, "db_update_transaction step heal: %s\n", sqlite3_errmsg(db));
                return -1;
            }
        } else if (count > 1) {
            rc = sqlite3_prepare_v2(db,
                "UPDATE transactions"
                " SET amount_cents = ?, date = ?, reflection_date = ?, payee = ?, description = ?, type = 'TRANSFER', category_id = NULL, account_id = ?"
                " WHERE transfer_id = ? AND id != ?",
                -1, &stmt, NULL);
            if (rc != SQLITE_OK) {
                fprintf(stderr, "db_update_transaction prepare mirror: %s\n", sqlite3_errmsg(db));
                return -1;
            }
            sqlite3_bind_int64(stmt, 1, normalized.amount_cents);
            sqlite3_bind_text(stmt, 2, normalized.date, -1, SQLITE_STATIC);
            if (normalized.reflection_date[0] != '\0')
                sqlite3_bind_text(stmt, 3, normalized.reflection_date, -1,
                                  SQLITE_STATIC);
            else
                sqlite3_bind_null(stmt, 3);
            if (normalized.payee[0] != '\0')
                sqlite3_bind_text(stmt, 4, normalized.payee, -1, SQLITE_STATIC);
            else
                sqlite3_bind_null(stmt, 4);
            if (normalized.description[0] != '\0')
                sqlite3_bind_text(stmt, 5, normalized.description, -1,
                                  SQLITE_STATIC);
            else
                sqlite3_bind_null(stmt, 5);
            sqlite3_bind_int64(stmt, 6, old_account_id);
            sqlite3_bind_int64(stmt, 7, normalized.transfer_id);
            sqlite3_bind_int64(stmt, 8, normalized.id);
            rc = sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            stmt = NULL;
            if (rc != SQLITE_DONE) {
                fprintf(stderr, "db_update_transaction step mirror: %s\n", sqlite3_errmsg(db));
                return -1;
            }
        }
    }

    if (old_transfer_id != 0 && normalized.transfer_id == 0) {
        rc = sqlite3_prepare_v2(db,
            "UPDATE transactions SET transfer_id = NULL WHERE transfer_id = ?",
            -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "db_update_transaction prepare clear: %s\n", sqlite3_errmsg(db));
            return -1;
        }
        sqlite3_bind_int64(stmt, 1, old_transfer_id);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        stmt = NULL;
        if (rc != SQLITE_DONE) {
            fprintf(stderr, "db_update_transaction step clear: %s\n", sqlite3_errmsg(db));
            return -1;
        }
    }

    return 0;
}

int db_get_budget_category_filter_mode(sqlite3 *db,
                                       budget_category_filter_mode_t *out_mode) {
    if (!out_mode)
        return -1;
    *out_mode = BUDGET_CATEGORY_FILTER_EXCLUDE_SELECTED;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db, "SELECT mode FROM budget_filter_settings WHERE id = 1", -1, &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_get_budget_category_filter_mode prepare: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const char *mode = (const char *)sqlite3_column_text(stmt, 0);
        *out_mode = budget_filter_mode_from_str(mode);
        sqlite3_finalize(stmt);
        return 0;
    }

    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE)
        return 0;

    fprintf(stderr, "db_get_budget_category_filter_mode step: %s\n",
            sqlite3_errmsg(db));
    return -1;
}

int db_set_budget_category_filter_mode(sqlite3 *db,
                                       budget_category_filter_mode_t mode) {
    if (mode != BUDGET_CATEGORY_FILTER_EXCLUDE_SELECTED &&
        mode != BUDGET_CATEGORY_FILTER_INCLUDE_SELECTED)
        return -1;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "INSERT INTO budget_filter_settings (id, mode)"
        " VALUES (1, ?)"
        " ON CONFLICT(id)"
        " DO UPDATE SET mode = excluded.mode",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_set_budget_category_filter_mode prepare: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, budget_filter_mode_to_str(mode), -1,
                      SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_set_budget_category_filter_mode step: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

int db_get_budget_category_filter_selected(sqlite3 *db, int64_t **out) {
    if (!out)
        return -1;
    *out = NULL;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "SELECT category_id"
        " FROM budget_category_filters"
        " ORDER BY category_id",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_get_budget_category_filter_selected prepare: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    int capacity = 16;
    int count = 0;
    int64_t *list = malloc((size_t)capacity * sizeof(int64_t));
    if (!list) {
        sqlite3_finalize(stmt);
        return -1;
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (count >= capacity) {
            capacity *= 2;
            int64_t *tmp =
                realloc(list, (size_t)capacity * sizeof(int64_t));
            if (!tmp) {
                free(list);
                sqlite3_finalize(stmt);
                return -1;
            }
            list = tmp;
        }
        list[count++] = sqlite3_column_int64(stmt, 0);
    }

    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_get_budget_category_filter_selected step: %s\n",
                sqlite3_errmsg(db));
        free(list);
        return -1;
    }

    if (count == 0) {
        free(list);
        return 0;
    }

    *out = list;
    return count;
}

int db_set_budget_category_filter_selected(sqlite3 *db, int64_t category_id,
                                           bool selected) {
    if (category_id <= 0)
        return -1;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT 1 FROM categories WHERE id = ?", -1,
                                &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_set_budget_category_filter_selected prepare chk: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, category_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_ROW) {
        if (rc != SQLITE_DONE) {
            fprintf(stderr,
                    "db_set_budget_category_filter_selected step chk: %s\n",
                    sqlite3_errmsg(db));
            return -1;
        }
        return -2;
    }

    if (selected) {
        rc = sqlite3_prepare_v2(
            db,
            "INSERT OR IGNORE INTO budget_category_filters (category_id)"
            " VALUES (?)",
            -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            fprintf(stderr,
                    "db_set_budget_category_filter_selected prepare ins: %s\n",
                    sqlite3_errmsg(db));
            return -1;
        }
    } else {
        rc = sqlite3_prepare_v2(
            db, "DELETE FROM budget_category_filters WHERE category_id = ?", -1,
            &stmt, NULL);
        if (rc != SQLITE_OK) {
            fprintf(stderr,
                    "db_set_budget_category_filter_selected prepare del: %s\n",
                    sqlite3_errmsg(db));
            return -1;
        }
    }

    sqlite3_bind_int64(stmt, 1, category_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_set_budget_category_filter_selected step: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

int db_get_budget_filter_categories(sqlite3 *db, budget_filter_category_t **out) {
    if (!out)
        return -1;
    *out = NULL;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "SELECT c.id, COALESCE(c.parent_id, 0), c.type, c.name"
        " FROM categories c"
        " LEFT JOIN categories p ON p.id = c.parent_id"
        " ORDER BY"
        "   CASE WHEN c.type = 'EXPENSE' THEN 0 ELSE 1 END,"
        "   CASE WHEN c.parent_id IS NULL THEN c.name ELSE p.name END"
        "      COLLATE NOCASE,"
        "   CASE WHEN c.parent_id IS NULL THEN 0 ELSE 1 END,"
        "   c.name COLLATE NOCASE",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_get_budget_filter_categories prepare: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    int capacity = 16;
    int count = 0;
    budget_filter_category_t *list =
        malloc((size_t)capacity * sizeof(budget_filter_category_t));
    if (!list) {
        sqlite3_finalize(stmt);
        return -1;
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (count >= capacity) {
            capacity *= 2;
            budget_filter_category_t *tmp = realloc(
                list, (size_t)capacity * sizeof(budget_filter_category_t));
            if (!tmp) {
                free(list);
                sqlite3_finalize(stmt);
                return -1;
            }
            list = tmp;
        }

        memset(&list[count], 0, sizeof(budget_filter_category_t));
        list[count].id = sqlite3_column_int64(stmt, 0);
        list[count].parent_id = sqlite3_column_int64(stmt, 1);
        const char *ctype = (const char *)sqlite3_column_text(stmt, 2);
        list[count].type =
            (ctype && strcmp(ctype, "INCOME") == 0) ? CATEGORY_INCOME
                                                     : CATEGORY_EXPENSE;
        const char *name = (const char *)sqlite3_column_text(stmt, 3);
        snprintf(list[count].name, sizeof(list[count].name), "%s",
                 name ? name : "");
        count++;
    }

    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_get_budget_filter_categories step: %s\n",
                sqlite3_errmsg(db));
        free(list);
        return -1;
    }

    if (count == 0) {
        free(list);
        return 0;
    }

    *out = list;
    return count;
}

int db_get_budget_rows_for_month(sqlite3 *db, const char *month_ym,
                                 budget_row_t **out) {
    if (!out)
        return -1;
    *out = NULL;

    char norm_month[8];
    if (normalize_budget_month(month_ym, norm_month) < 0)
        return -1;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "WITH RECURSIVE"
        " parents AS ("
        "   SELECT id, name FROM categories WHERE parent_id IS NULL"
        " ),"
        " descendants(parent_id, category_id) AS ("
        "   SELECT p.id, p.id FROM parents p"
        "   UNION ALL"
        "   SELECT d.parent_id, c.id"
        "   FROM descendants d"
        "   JOIN categories c ON c.parent_id = d.category_id"
        " ),"
        " selected_descendants(category_id) AS ("
        "   SELECT category_id FROM budget_category_filters"
        "   UNION"
        "   SELECT c.id FROM categories c"
        "   JOIN selected_descendants sd ON c.parent_id = sd.category_id"
        " ),"
        " filter_mode(include_selected) AS ("
        "   SELECT CASE"
        "     WHEN EXISTS("
        "       SELECT 1 FROM budget_filter_settings bfs"
        "       WHERE bfs.id = 1 AND bfs.mode = 'INCLUDE_SELECTED'"
        "     ) THEN 1 ELSE 0 END"
        " ),"
        " allowed_categories(category_id) AS ("
        "   SELECT c.id"
        "   FROM categories c"
        "   CROSS JOIN filter_mode fm"
        "   WHERE (fm.include_selected = 0"
        "          AND c.id NOT IN (SELECT category_id FROM selected_descendants))"
        "      OR (fm.include_selected = 1"
        "          AND c.id IN (SELECT category_id FROM selected_descendants))"
        " ),"
        " allowed_descendant_counts AS ("
        "   SELECT d.parent_id, COUNT(*) AS allowed_count"
        "   FROM descendants d"
        "   JOIN allowed_categories ac ON ac.category_id = d.category_id"
        "   GROUP BY d.parent_id"
        " ),"
        " monthly_stats AS ("
        "   SELECT d.parent_id,"
        "          COALESCE(SUM(CASE"
        "            WHEN t.type = 'EXPENSE' THEN t.amount_cents"
        "            WHEN t.type = 'INCOME' THEN -t.amount_cents"
        "            ELSE 0"
        "          END), 0) AS net_spent_cents,"
        "          COUNT(t.id) AS txn_count"
        "   FROM descendants d"
        "   LEFT JOIN transactions t"
        "     ON t.category_id = d.category_id"
        "    AND t.category_id IN (SELECT category_id FROM allowed_categories)"
        "    AND substr(COALESCE(t.reflection_date, t.date), 1, 7) = ?"
        "    AND t.type IN ('EXPENSE', 'INCOME')"
        "   GROUP BY d.parent_id"
        " ),"
        " flags AS ("
        "   SELECT p.id AS parent_id,"
        "          EXISTS("
        "            SELECT 1 FROM budgets b"
        "            WHERE b.category_id = p.id AND b.month <= ?"
        "          ) AS has_rule,"
        "          EXISTS("
        "            SELECT 1"
        "            FROM descendants dd"
        "            JOIN allowed_categories ac ON ac.category_id = dd.category_id"
        "            JOIN budgets b2 ON b2.category_id = dd.category_id"
        "            WHERE dd.parent_id = p.id AND b2.month <= ?"
        "          ) AS has_rollup_rule,"
        "          ("
            "            SELECT b3.limit_cents"
            "            FROM budgets b3"
            "            WHERE b3.category_id = p.id AND b3.month <= ?"
            "            ORDER BY b3.month DESC"
            "            LIMIT 1"
        "          ) AS direct_limit_cents,"
        "          ("
        "            SELECT COALESCE(SUM(COALESCE(("
        "              SELECT b4.limit_cents"
        "              FROM budgets b4"
        "              WHERE b4.category_id = dd2.category_id"
        "                AND b4.month <= ?"
        "              ORDER BY b4.month DESC"
        "              LIMIT 1"
        "            ), 0)), 0)"
        "            FROM descendants dd2"
        "            JOIN allowed_categories ac2 ON ac2.category_id = dd2.category_id"
        "            WHERE dd2.parent_id = p.id"
        "          ) AS rollup_limit_cents,"
        "          ("
        "            SELECT COUNT(*) FROM categories c WHERE c.parent_id = p.id"
        "          ) AS child_count"
        "   FROM parents p"
        " )"
        " SELECT p.id, p.name, f.child_count,"
        "        COALESCE(ms.net_spent_cents, 0),"
        "        COALESCE(ms.txn_count, 0),"
        "        COALESCE(f.direct_limit_cents, 0),"
        "        COALESCE(f.rollup_limit_cents, 0),"
        "        f.has_rule,"
        "        f.has_rollup_rule"
        " FROM parents p"
        " JOIN flags f ON f.parent_id = p.id"
        " JOIN allowed_descendant_counts adc ON adc.parent_id = p.id"
        " LEFT JOIN monthly_stats ms ON ms.parent_id = p.id"
        " WHERE adc.allowed_count > 0"
        "   AND (f.has_rule = 1"
        "        OR f.has_rollup_rule = 1"
        "        OR COALESCE(ms.txn_count, 0) > 0)"
        " ORDER BY p.name",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_get_budget_rows_for_month prepare: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, norm_month, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, norm_month, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, norm_month, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, norm_month, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, norm_month, -1, SQLITE_TRANSIENT);

    int capacity = 16;
    int count = 0;
    budget_row_t *list = malloc((size_t)capacity * sizeof(budget_row_t));
    if (!list) {
        sqlite3_finalize(stmt);
        return -1;
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (count >= capacity) {
            capacity *= 2;
            budget_row_t *tmp =
                realloc(list, (size_t)capacity * sizeof(budget_row_t));
            if (!tmp) {
                free(list);
                sqlite3_finalize(stmt);
                return -1;
            }
            list = tmp;
        }

        memset(&list[count], 0, sizeof(budget_row_t));
        list[count].category_id = sqlite3_column_int64(stmt, 0);
        const char *name = (const char *)sqlite3_column_text(stmt, 1);
        snprintf(list[count].category_name, sizeof(list[count].category_name),
                 "%s", name ? name : "");
        list[count].child_count = sqlite3_column_int(stmt, 2);
        list[count].net_spent_cents = sqlite3_column_int64(stmt, 3);
        list[count].txn_count = sqlite3_column_int(stmt, 4);
        list[count].direct_limit_cents = sqlite3_column_int64(stmt, 5);
        list[count].limit_cents = sqlite3_column_int64(stmt, 6);
        list[count].has_rule = sqlite3_column_int(stmt, 7) != 0;
        list[count].has_rollup_rule = sqlite3_column_int(stmt, 8) != 0;
        list[count].parent_category_id = 0;
        compute_budget_utilization(&list[count]);
        count++;
    }

    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_get_budget_rows_for_month step: %s\n",
                sqlite3_errmsg(db));
        free(list);
        return -1;
    }

    if (count == 0) {
        free(list);
        return 0;
    }
    *out = list;
    return count;
}

int db_get_budget_child_rows_for_month(sqlite3 *db, int64_t parent_category_id,
                                       const char *month_ym,
                                       budget_row_t **out) {
    if (!out || parent_category_id <= 0)
        return -1;
    *out = NULL;

    char norm_month[8];
    if (normalize_budget_month(month_ym, norm_month) < 0)
        return -1;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "WITH RECURSIVE"
        " roots AS ("
        "   SELECT id, name FROM categories WHERE parent_id = ?"
        " ),"
        " descendants(root_id, category_id) AS ("
        "   SELECT r.id, r.id FROM roots r"
        "   UNION ALL"
        "   SELECT d.root_id, c.id"
        "   FROM descendants d"
        "   JOIN categories c ON c.parent_id = d.category_id"
        " ),"
        " selected_descendants(category_id) AS ("
        "   SELECT category_id FROM budget_category_filters"
        "   UNION"
        "   SELECT c.id FROM categories c"
        "   JOIN selected_descendants sd ON c.parent_id = sd.category_id"
        " ),"
        " filter_mode(include_selected) AS ("
        "   SELECT CASE"
        "     WHEN EXISTS("
        "       SELECT 1 FROM budget_filter_settings bfs"
        "       WHERE bfs.id = 1 AND bfs.mode = 'INCLUDE_SELECTED'"
        "     ) THEN 1 ELSE 0 END"
        " ),"
        " allowed_categories(category_id) AS ("
        "   SELECT c.id"
        "   FROM categories c"
        "   CROSS JOIN filter_mode fm"
        "   WHERE (fm.include_selected = 0"
        "          AND c.id NOT IN (SELECT category_id FROM selected_descendants))"
        "      OR (fm.include_selected = 1"
        "          AND c.id IN (SELECT category_id FROM selected_descendants))"
        " ),"
        " allowed_descendant_counts AS ("
        "   SELECT d.root_id, COUNT(*) AS allowed_count"
        "   FROM descendants d"
        "   JOIN allowed_categories ac ON ac.category_id = d.category_id"
        "   GROUP BY d.root_id"
        " ),"
        " monthly_stats AS ("
        "   SELECT d.root_id,"
        "          COALESCE(SUM(CASE"
        "            WHEN t.type = 'EXPENSE' THEN t.amount_cents"
        "            WHEN t.type = 'INCOME' THEN -t.amount_cents"
        "            ELSE 0"
        "          END), 0) AS net_spent_cents,"
        "          COUNT(t.id) AS txn_count"
        "   FROM descendants d"
        "   LEFT JOIN transactions t"
        "     ON t.category_id = d.category_id"
        "    AND t.category_id IN (SELECT category_id FROM allowed_categories)"
        "    AND substr(COALESCE(t.reflection_date, t.date), 1, 7) = ?"
        "    AND t.type IN ('EXPENSE', 'INCOME')"
        "   GROUP BY d.root_id"
        " )"
        " SELECT r.id, r.name,"
        "        (SELECT COUNT(*) FROM categories c WHERE c.parent_id = r.id),"
        "        COALESCE(ms.net_spent_cents, 0),"
        "        COALESCE(ms.txn_count, 0),"
        "        COALESCE(("
        "          SELECT b.limit_cents FROM budgets b"
        "          WHERE b.category_id = r.id AND b.month <= ?"
        "          ORDER BY b.month DESC"
        "          LIMIT 1"
        "        ), 0),"
        "        ("
        "          SELECT COALESCE(SUM(COALESCE(("
        "            SELECT b4.limit_cents"
        "            FROM budgets b4"
        "            WHERE b4.category_id = dd2.category_id"
        "              AND b4.month <= ?"
        "            ORDER BY b4.month DESC"
        "            LIMIT 1"
        "          ), 0)), 0)"
        "          FROM descendants dd2"
        "          JOIN allowed_categories ac2 ON ac2.category_id = dd2.category_id"
        "          WHERE dd2.root_id = r.id"
        "        ),"
        "        EXISTS("
        "          SELECT 1 FROM budgets b2"
        "          WHERE b2.category_id = r.id AND b2.month <= ?"
        "        ),"
        "        EXISTS("
        "          SELECT 1"
        "          FROM descendants dd"
        "          JOIN allowed_categories ac ON ac.category_id = dd.category_id"
        "          JOIN budgets b3 ON b3.category_id = dd.category_id"
        "          WHERE dd.root_id = r.id AND b3.month <= ?"
        "        )"
        " FROM roots r"
        " JOIN allowed_descendant_counts adc ON adc.root_id = r.id"
        " LEFT JOIN monthly_stats ms ON ms.root_id = r.id"
        " WHERE adc.allowed_count > 0"
        "   AND (COALESCE(ms.txn_count, 0) > 0"
        "        OR EXISTS("
        "          SELECT 1"
        "          FROM descendants dd"
        "          JOIN allowed_categories ac ON ac.category_id = dd.category_id"
        "          JOIN budgets b3 ON b3.category_id = dd.category_id"
        "          WHERE dd.root_id = r.id AND b3.month <= ?"
        "        ))"
        " ORDER BY r.name",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_get_budget_child_rows_for_month prepare: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, parent_category_id);
    sqlite3_bind_text(stmt, 2, norm_month, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, norm_month, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, norm_month, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, norm_month, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, norm_month, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, norm_month, -1, SQLITE_TRANSIENT);

    int capacity = 8;
    int count = 0;
    budget_row_t *list = malloc((size_t)capacity * sizeof(budget_row_t));
    if (!list) {
        sqlite3_finalize(stmt);
        return -1;
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (count >= capacity) {
            capacity *= 2;
            budget_row_t *tmp =
                realloc(list, (size_t)capacity * sizeof(budget_row_t));
            if (!tmp) {
                free(list);
                sqlite3_finalize(stmt);
                return -1;
            }
            list = tmp;
        }

        memset(&list[count], 0, sizeof(budget_row_t));
        list[count].category_id = sqlite3_column_int64(stmt, 0);
        const char *name = (const char *)sqlite3_column_text(stmt, 1);
        snprintf(list[count].category_name, sizeof(list[count].category_name),
                 "%s", name ? name : "");
        list[count].child_count = sqlite3_column_int(stmt, 2);
        list[count].net_spent_cents = sqlite3_column_int64(stmt, 3);
        list[count].txn_count = sqlite3_column_int(stmt, 4);
        list[count].direct_limit_cents = sqlite3_column_int64(stmt, 5);
        list[count].limit_cents = sqlite3_column_int64(stmt, 6);
        list[count].has_rule = sqlite3_column_int(stmt, 7) != 0;
        list[count].has_rollup_rule = sqlite3_column_int(stmt, 8) != 0;
        list[count].parent_category_id = parent_category_id;
        compute_budget_utilization(&list[count]);
        count++;
    }

    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_get_budget_child_rows_for_month step: %s\n",
                sqlite3_errmsg(db));
        free(list);
        return -1;
    }

    if (count == 0) {
        free(list);
        return 0;
    }
    *out = list;
    return count;
}

int db_get_budget_running_progress_for_year_before_month(
    sqlite3 *db, int64_t category_id, const char *month_ym,
    int64_t *out_actual_cents, int64_t *out_expected_cents) {
    if (!out_actual_cents || !out_expected_cents || category_id <= 0)
        return -1;
    *out_actual_cents = 0;
    *out_expected_cents = 0;

    char norm_month[8];
    if (normalize_budget_month(month_ym, norm_month) < 0)
        return -1;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "WITH RECURSIVE"
        " descendants(category_id) AS ("
        "   SELECT ?1"
        "   UNION ALL"
        "   SELECT c.id"
        "   FROM categories c"
        "   JOIN descendants d ON c.parent_id = d.category_id"
        " ),"
        " selected_descendants(category_id) AS ("
        "   SELECT category_id FROM budget_category_filters"
        "   UNION"
        "   SELECT c.id FROM categories c"
        "   JOIN selected_descendants sd ON c.parent_id = sd.category_id"
        " ),"
        " filter_mode(include_selected) AS ("
        "   SELECT CASE"
        "     WHEN EXISTS("
        "       SELECT 1 FROM budget_filter_settings bfs"
        "       WHERE bfs.id = 1 AND bfs.mode = 'INCLUDE_SELECTED'"
        "     ) THEN 1 ELSE 0 END"
        " ),"
        " allowed_categories(category_id) AS ("
        "   SELECT c.id"
        "   FROM categories c"
        "   CROSS JOIN filter_mode fm"
        "   WHERE (fm.include_selected = 0"
        "          AND c.id NOT IN (SELECT category_id FROM selected_descendants))"
        "      OR (fm.include_selected = 1"
        "          AND c.id IN (SELECT category_id FROM selected_descendants))"
        " ),"
        " view_ctx(view_month, view_month_start, year_start) AS ("
        "   SELECT ?2,"
        "          date(?2 || '-01'),"
        "          date(strftime('%Y-01-01', ?2 || '-01'))"
        " ),"
        " months(month_ym) AS ("
        "   SELECT strftime('%Y-01', view_month_start) FROM view_ctx"
        "   UNION ALL"
        "   SELECT strftime('%Y-%m', date(month_ym || '-01', '+1 month'))"
        "   FROM months"
        "   WHERE month_ym < (SELECT view_month FROM view_ctx)"
        " ),"
        " expected_progress AS ("
        "   SELECT COALESCE(SUM("
        "     ("
        "       SELECT COALESCE(SUM(COALESCE(("
        "         SELECT b.limit_cents"
        "         FROM budgets b"
        "         WHERE b.category_id = d.category_id"
        "           AND b.month <= m.month_ym"
        "         ORDER BY b.month DESC"
        "         LIMIT 1"
        "       ), 0)), 0)"
        "       FROM descendants d"
        "       JOIN allowed_categories ac ON ac.category_id = d.category_id"
        "     )"
        "   ), 0) AS expected_cents"
        "   FROM months m"
        "   JOIN view_ctx vc"
        "   WHERE m.month_ym < vc.view_month"
        " ),"
        " actual_progress AS ("
        "   SELECT COALESCE(SUM(CASE"
        "     WHEN t.type = 'EXPENSE' THEN t.amount_cents"
        "     WHEN t.type = 'INCOME' THEN -t.amount_cents"
        "     ELSE 0"
        "   END), 0) AS actual_cents"
        "   FROM transactions t"
        "   JOIN descendants d ON d.category_id = t.category_id"
        "   JOIN allowed_categories ac ON ac.category_id = t.category_id"
        "   JOIN view_ctx vc"
        "   WHERE t.type IN ('EXPENSE', 'INCOME')"
        "     AND COALESCE(t.reflection_date, t.date) >= vc.year_start"
        "     AND COALESCE(t.reflection_date, t.date) < vc.view_month_start"
        " )"
        " SELECT ap.actual_cents, ep.expected_cents"
        " FROM actual_progress ap"
        " CROSS JOIN expected_progress ep",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr,
                "db_get_budget_running_progress_for_year_before_month prepare: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, category_id);
    sqlite3_bind_text(stmt, 2, norm_month, -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        fprintf(stderr,
                "db_get_budget_running_progress_for_year_before_month step: %s\n",
                sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return -1;
    }

    *out_actual_cents = sqlite3_column_int64(stmt, 0);
    *out_expected_cents = sqlite3_column_int64(stmt, 1);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr,
                "db_get_budget_running_progress_for_year_before_month finalize: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    return 0;
}

int db_set_budget_effective(sqlite3 *db, int64_t category_id,
                            const char *effective_month_ym,
                            int64_t limit_cents) {
    if (category_id <= 0 || limit_cents < 0)
        return -1;

    char norm_month[8];
    if (normalize_budget_month(effective_month_ym, norm_month) < 0)
        return -1;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "INSERT INTO budgets (category_id, month, limit_cents)"
        " VALUES (?, ?, ?)"
        " ON CONFLICT(category_id, month)"
        " DO UPDATE SET limit_cents = excluded.limit_cents",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_set_budget_effective prepare: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, category_id);
    sqlite3_bind_text(stmt, 2, norm_month, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, limit_cents);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_set_budget_effective step: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

int db_get_budget_limit_for_month(sqlite3 *db, int64_t category_id,
                                  const char *month_ym,
                                  int64_t *out_limit_cents) {
    if (category_id <= 0 || !out_limit_cents)
        return -1;
    *out_limit_cents = 0;

    char norm_month[8];
    if (normalize_budget_month(month_ym, norm_month) < 0)
        return -1;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "SELECT limit_cents"
        " FROM budgets"
        " WHERE category_id = ? AND month <= ?"
        " ORDER BY month DESC"
        " LIMIT 1",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_get_budget_limit_for_month prepare: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, category_id);
    sqlite3_bind_text(stmt, 2, norm_month, -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *out_limit_cents = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return 0;
    }

    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE)
        return -2;

    fprintf(stderr, "db_get_budget_limit_for_month step: %s\n",
            sqlite3_errmsg(db));
    return -1;
}
