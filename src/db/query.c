#include "db/query.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int db_get_accounts(sqlite3 *db, account_t **out) {
    *out = NULL;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT id, name, type FROM accounts ORDER BY name", -1, &stmt, NULL);
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
        count++;
    }

    sqlite3_finalize(stmt);
    *out = list;
    return count;
}

int64_t db_insert_account(sqlite3 *db, const char *name, account_type_t type) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO accounts (name, type) VALUES (?, ?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_insert_account prepare: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, account_type_db_strings[type], -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_insert_account step: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    return sqlite3_last_insert_rowid(db);
}

int db_get_categories(sqlite3 *db, category_type_t type, category_t **out) {
    *out = NULL;

    const char *type_str = (type == CATEGORY_EXPENSE) ? "EXPENSE" : "INCOME";

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
        "  CASE WHEN p.name IS NOT NULL THEN p.name || ':' || c.name"
        "       ELSE COALESCE(c.name, '') END,"
        "  COALESCE(t.description, '')"
        " FROM transactions t"
        " LEFT JOIN categories c ON t.category_id = c.id"
        " LEFT JOIN categories p ON c.parent_id = p.id"
        " WHERE t.account_id = ?"
        " ORDER BY t.date DESC, t.id DESC",
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
        const char *cat = (const char *)sqlite3_column_text(stmt, 4);
        snprintf(list[count].category_name, sizeof(list[count].category_name), "%s", cat ? cat : "");
        const char *desc = (const char *)sqlite3_column_text(stmt, 5);
        snprintf(list[count].description, sizeof(list[count].description), "%s", desc ? desc : "");
        count++;
    }

    sqlite3_finalize(stmt);
    *out = list;
    return count;
}

int64_t db_insert_transaction(sqlite3 *db, const transaction_t *txn) {
    const char *type_str = transaction_type_to_str(txn->type);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO transactions (amount_cents, type, account_id, category_id, date, description)"
        " VALUES (?, ?, ?, ?, ?, ?)",
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
    sqlite3_bind_text(stmt, 5, txn->date, -1, SQLITE_STATIC);
    if (txn->description[0] != '\0')
        sqlite3_bind_text(stmt, 6, txn->description, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 6);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_insert_transaction step: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    return sqlite3_last_insert_rowid(db);
}

int db_get_transaction_by_id(sqlite3 *db, int txn_id, transaction_t *out) {
    if (!out) return -1;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT id, amount_cents, type, account_id, category_id, date, description, transfer_id"
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
        const char *desc = (const char *)sqlite3_column_text(stmt, 6);
        snprintf(out->description, sizeof(out->description), "%s", desc ? desc : "");
        if (sqlite3_column_type(stmt, 7) == SQLITE_NULL)
            out->transfer_id = 0;
        else
            out->transfer_id = sqlite3_column_int64(stmt, 7);
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
        " SET amount_cents = ?, type = ?, account_id = ?, category_id = ?, date = ?, description = ?, transfer_id = ?"
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
    if (normalized.description[0] != '\0')
        sqlite3_bind_text(stmt, 6, normalized.description, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 6);
    if (normalized.transfer_id > 0)
        sqlite3_bind_int64(stmt, 7, normalized.transfer_id);
    else
        sqlite3_bind_null(stmt, 7);
    sqlite3_bind_int64(stmt, 8, normalized.id);

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
                " SET amount_cents = ?, date = ?, description = ?, type = 'TRANSFER', category_id = NULL, account_id = ?"
                " WHERE transfer_id = ? AND id != ?",
                -1, &stmt, NULL);
            if (rc != SQLITE_OK) {
                fprintf(stderr, "db_update_transaction prepare mirror: %s\n", sqlite3_errmsg(db));
                return -1;
            }
            sqlite3_bind_int64(stmt, 1, normalized.amount_cents);
            sqlite3_bind_text(stmt, 2, normalized.date, -1, SQLITE_STATIC);
            if (normalized.description[0] != '\0')
                sqlite3_bind_text(stmt, 3, normalized.description, -1, SQLITE_STATIC);
            else
                sqlite3_bind_null(stmt, 3);
            sqlite3_bind_int64(stmt, 4, old_account_id);
            sqlite3_bind_int64(stmt, 5, normalized.transfer_id);
            sqlite3_bind_int64(stmt, 6, normalized.id);
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
