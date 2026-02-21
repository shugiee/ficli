#include "db/query.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *account_type_db_strings[] = {
    "CASH", "CHECKING", "SAVINGS", "CREDIT_CARD", "PHYSICAL_ASSET", "INVESTMENT"
};

static const char *transaction_type_db_strings[] = {
    "EXPENSE", "INCOME", "TRANSFER"
};

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
        "  WHEN type = 'TRANSFER' THEN CASE"
        "    WHEN transfer_id IS NOT NULL AND id = transfer_id THEN -amount_cents"
        "    ELSE amount_cents"
        "  END"
        "  ELSE 0"
        " END), 0)"
        " FROM transactions"
        " WHERE account_id = ?"
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
        "   AND date < date('now', 'localtime', ?)",
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
        "SELECT date,"
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
        "   AND date >= date('now', 'localtime', ?)"
        "   AND date <= date('now', 'localtime')"
        " GROUP BY date"
        " ORDER BY date",
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
                       int64_t to_account_id) {
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
        db, "SELECT transfer_id FROM transactions WHERE id = ?", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_update_transfer prepare load: %s\n", sqlite3_errmsg(db));
        goto rollback;
    }
    sqlite3_bind_int64(stmt, 1, txn->id);

    int64_t transfer_id = 0;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        if (sqlite3_column_type(stmt, 0) != SQLITE_NULL)
            transfer_id = sqlite3_column_int64(stmt, 0);
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

    if (transfer_id <= 0)
        transfer_id = txn->id;

    int64_t mirror_id = 0;
    rc = sqlite3_prepare_v2(
        db,
        "SELECT id FROM transactions WHERE transfer_id = ? AND id != ? LIMIT 1",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_update_transfer prepare mirror: %s\n", sqlite3_errmsg(db));
        goto rollback;
    }
    sqlite3_bind_int64(stmt, 1, transfer_id);
    sqlite3_bind_int64(stmt, 2, txn->id);
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
    sqlite3_bind_int64(stmt, 7, transfer_id);
    sqlite3_bind_int64(stmt, 8, txn->id);
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
        sqlite3_bind_int64(stmt, 7, transfer_id);
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
        if (insert_transfer_row(db, txn, to_account_id, transfer_id, NULL) < 0)
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
