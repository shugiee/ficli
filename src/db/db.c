#include "db/db.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int ensure_dir_exists(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);

    // Walk the path and create each component
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static int exec_sql(sqlite3 *db, const char *sql) {
    char *err_msg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }
    return 0;
}

static bool is_new_database(sqlite3 *db) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT name FROM sqlite_master WHERE type='table' AND name='categories'",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return true;
    }
    bool exists = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return !exists;
}

static int create_schema(sqlite3 *db) {
    const char *schema_sql =
        "CREATE TABLE IF NOT EXISTS accounts ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    name TEXT NOT NULL UNIQUE,"
        "    type TEXT NOT NULL DEFAULT 'CASH'"
        "        CHECK(type IN ('CASH','CHECKING','SAVINGS','CREDIT_CARD','PHYSICAL_ASSET','INVESTMENT')),"
        "    card_last4 TEXT"
        ");"

        "CREATE TABLE IF NOT EXISTS categories ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    name TEXT NOT NULL,"
        "    type TEXT NOT NULL CHECK(type IN ('EXPENSE', 'INCOME')),"
        "    parent_id INTEGER,"
        "    UNIQUE(name, parent_id),"
        "    FOREIGN KEY (parent_id) REFERENCES categories(id)"
        ");"

        "CREATE TABLE IF NOT EXISTS transactions ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    amount_cents INTEGER NOT NULL,"
        "    type TEXT NOT NULL CHECK(type IN ('EXPENSE', 'INCOME', 'TRANSFER')),"
        "    account_id INTEGER NOT NULL,"
        "    category_id INTEGER,"
        "    date TEXT NOT NULL,"
        "    payee TEXT,"
        "    description TEXT,"
        "    transfer_id INTEGER,"
        "    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
        "    FOREIGN KEY (account_id) REFERENCES accounts(id),"
        "    FOREIGN KEY (category_id) REFERENCES categories(id)"
        ");"

        "CREATE TABLE IF NOT EXISTS budgets ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    category_id INTEGER NOT NULL,"
        "    month TEXT NOT NULL,"
        "    limit_cents INTEGER NOT NULL,"
        "    UNIQUE(category_id, month),"
        "    FOREIGN KEY (category_id) REFERENCES categories(id)"
        ");"

        "CREATE INDEX IF NOT EXISTS idx_transactions_date ON transactions(date);"
        "CREATE INDEX IF NOT EXISTS idx_transactions_category ON transactions(category_id);"
        "CREATE INDEX IF NOT EXISTS idx_transactions_account ON transactions(account_id);"
        "CREATE INDEX IF NOT EXISTS idx_transactions_transfer ON transactions(transfer_id);"
        "CREATE INDEX IF NOT EXISTS idx_budgets_month ON budgets(month);"
        "CREATE INDEX IF NOT EXISTS idx_categories_parent ON categories(parent_id);";

    return exec_sql(db, schema_sql);
}

static int seed_defaults(sqlite3 *db) {
    const char *seed_sql =
        "INSERT INTO accounts (name, type) VALUES ('Cash', 'CASH');"

        "INSERT INTO categories (name, type, parent_id) VALUES"
        "    ('Groceries', 'EXPENSE', NULL),"
        "    ('Dining Out', 'EXPENSE', NULL),"
        "    ('Transportation', 'EXPENSE', NULL),"
        "    ('Housing', 'EXPENSE', NULL),"
        "    ('Utilities', 'EXPENSE', NULL),"
        "    ('Entertainment', 'EXPENSE', NULL),"
        "    ('Healthcare', 'EXPENSE', NULL),"
        "    ('Shopping', 'EXPENSE', NULL),"
        "    ('Other Expense', 'EXPENSE', NULL),"
        "    ('Salary', 'INCOME', NULL),"
        "    ('Freelance', 'INCOME', NULL),"
        "    ('Investments', 'INCOME', NULL),"
        "    ('Other Income', 'INCOME', NULL);";

    return exec_sql(db, seed_sql);
}

sqlite3 *db_init(const char *path) {
    // Extract directory from path and ensure it exists
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *last_slash = strrchr(dir, '/');
    if (last_slash) {
        *last_slash = '\0';
        if (ensure_dir_exists(dir) != 0) {
            fprintf(stderr, "Failed to create directory: %s\n", dir);
            return NULL;
        }
    }

    sqlite3 *db = NULL;
    int rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to open database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return NULL;
    }

    // Enable foreign key enforcement
    exec_sql(db, "PRAGMA foreign_keys = ON;");

    bool new_db = is_new_database(db);

    if (create_schema(db) != 0) {
        fprintf(stderr, "Failed to create database schema\n");
        sqlite3_close(db);
        return NULL;
    }

    if (new_db) {
        if (seed_defaults(db) != 0) {
            fprintf(stderr, "Failed to seed default data\n");
            sqlite3_close(db);
            return NULL;
        }
    }

    return db;
}

void db_close(sqlite3 *db) {
    if (db) {
        sqlite3_close(db);
    }
}
