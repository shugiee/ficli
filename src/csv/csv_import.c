#include "csv/csv_import.h"
#include "db/query.h"
#include "models/account.h"
#include "models/transaction.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_COLS 32
#define LINE_BUF 4096
#define FIELD_BUF 4096

// Parse one CSV line into fields[]. Each fields[i] points into buf.
// Returns number of fields parsed.
static int csv_parse_line(const char *line, char **fields, int max_fields,
                          char *buf, int buflen) {
    int nfields = 0;
    int bi = 0;
    const char *p = line;

    while (*p && nfields < max_fields) {
        if (bi >= buflen - 1)
            break;
        fields[nfields++] = buf + bi;

        if (*p == '"') {
            p++;
            while (*p) {
                if (*p == '"') {
                    if (*(p + 1) == '"') {
                        if (bi < buflen - 1)
                            buf[bi++] = '"';
                        p += 2;
                    } else {
                        p++;
                        break;
                    }
                } else {
                    if (bi < buflen - 1)
                        buf[bi++] = *p;
                    p++;
                }
            }
            while (*p && *p != ',')
                p++;
        } else {
            while (*p && *p != ',') {
                if (bi < buflen - 1)
                    buf[bi++] = *p;
                p++;
            }
        }

        if (bi < buflen)
            buf[bi++] = '\0';
        if (*p == ',')
            p++;
    }

    return nfields;
}

// Lowercase + trim whitespace for column-name matching.
static void normalize_col(const char *src, char *dst, int dstlen) {
    while (*src == ' ' || *src == '\t')
        src++;
    int di = 0;
    while (*src && di < dstlen - 1) {
        char c = *src++;
        if (c >= 'A' && c <= 'Z')
            c += 32;
        dst[di++] = c;
    }
    while (di > 0 && (dst[di - 1] == ' ' || dst[di - 1] == '\t'))
        di--;
    dst[di] = '\0';
}

// Normalize date to YYYY-MM-DD. Handles MM/DD/YYYY, MM/DD/YY, and
// YYYY-MM-DD input, and also QIF style M/D'YY.
// Returns true on success, false if format unrecognized.
static bool normalize_date(const char *src, char *dst) {
    if (!src || !dst)
        return false;

    char buf[64];
    snprintf(buf, sizeof(buf), "%s", src);

    // Trim surrounding whitespace
    char *start = buf;
    while (*start && isspace((unsigned char)*start))
        start++;
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)*(end - 1)))
        *(--end) = '\0';

    int y = 0, m = 0, d = 0;
    int len = (int)strlen(start);
    if (len == 10 && start[4] == '-' && start[7] == '-') {
        if (sscanf(start, "%4d-%2d-%2d", &y, &m, &d) != 3)
            return false;
    } else {
        char trailing = '\0';
        if (sscanf(start, "%d/%d/%d%c", &m, &d, &y, &trailing) != 3 &&
            sscanf(start, "%d/%d'%d%c", &m, &d, &y, &trailing) != 3) {
            return false;
        }
        if (y < 100)
            y += (y >= 70) ? 1900 : 2000;
    }

    if (m < 1 || m > 12 || d < 1 || d > 31 || y < 1900)
        return false;

    struct tm tmv = {0};
    tmv.tm_year = y - 1900;
    tmv.tm_mon = m - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = 12;
    tmv.tm_isdst = -1;
    if (mktime(&tmv) == (time_t)-1)
        return false;
    if (tmv.tm_year != y - 1900 || tmv.tm_mon != m - 1 || tmv.tm_mday != d)
        return false;

    return strftime(dst, 11, "%Y-%m-%d", &tmv) == 10;
}

// Parse a dollar amount string into cents. Strips $, commas, spaces.
// Handles negatives via leading '-' or parentheses notation.
// Returns true if a non-zero value was parsed.
static bool parse_csv_amount(const char *src, int64_t *out) {
    char buf[64];
    int bi = 0;
    bool negative = false;

    for (const char *p = src; *p && bi < (int)sizeof(buf) - 1; p++) {
        if (*p == '(') { negative = true; continue; }
        if (*p == ')') continue;
        if (*p == '-' && bi == 0) { negative = true; continue; }
        if (*p == '$' || *p == ',' || *p == ' ') continue;
        buf[bi++] = *p;
    }
    buf[bi] = '\0';

    if (bi == 0) {
        *out = 0;
        return false;
    }

    char *dot = strchr(buf, '.');
    int64_t whole = 0, frac = 0;
    if (dot) {
        *dot = '\0';
        if (buf[0] != '\0')
            whole = (int64_t)atoll(buf);
        const char *dp = dot + 1;
        int dplen = (int)strlen(dp);
        if (dplen >= 2) {
            char fb[3] = {dp[0], dp[1], '\0'};
            frac = (int64_t)atoll(fb);
        } else if (dplen == 1) {
            frac = (int64_t)(dp[0] - '0') * 10;
        }
    } else {
        whole = (int64_t)atoll(buf);
    }

    *out = whole * 100 + frac;
    if (negative)
        *out = -(*out);
    return (*out != 0);
}

// Extract the last 4 digits from a card number string.
static void extract_last4(const char *card_str, char *out) {
    int len = (int)strlen(card_str);
    char digits[5];
    int count = 0;
    for (int i = len - 1; i >= 0 && count < 4; i--) {
        if (card_str[i] >= '0' && card_str[i] <= '9')
            digits[count++] = card_str[i];
    }
    if (count == 4) {
        out[0] = digits[3];
        out[1] = digits[2];
        out[2] = digits[1];
        out[3] = digits[0];
        out[4] = '\0';
    } else {
        out[0] = '\0';
    }
}

static void strip_eol(char *line) {
    int len = (int)strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        line[--len] = '\0';
}

static void trim_whitespace_in_place(char *s) {
    if (!s)
        return;

    int len = (int)strlen(s);
    int start = 0;
    while (start < len && isspace((unsigned char)s[start]))
        start++;

    int end = len;
    while (end > start && isspace((unsigned char)s[end - 1]))
        end--;

    if (start > 0)
        memmove(s, s + start, (size_t)(end - start));
    s[end - start] = '\0';
}

static bool copy_import_category(const char *src, char *dst, size_t dst_sz) {
    if (!src || !dst || dst_sz == 0)
        return false;
    snprintf(dst, dst_sz, "%s", src);
    trim_whitespace_in_place(dst);
    return dst[0] != '\0';
}

static bool ensure_result_capacity(csv_parse_result_t *result, int *capacity) {
    if (result->row_count < *capacity)
        return true;
    *capacity *= 2;
    csv_row_t *tmp = realloc(result->rows, (size_t)(*capacity) * sizeof(csv_row_t));
    if (!tmp)
        return false;
    result->rows = tmp;
    return true;
}

static csv_parse_result_t csv_parse_stream(FILE *f) {
    csv_parse_result_t result = {0};
    result.type = CSV_TYPE_UNKNOWN;

    char line[LINE_BUF];
    line[0] = '\0';

    // Read and parse header row
    while (fgets(line, sizeof(line), f)) {
        strip_eol(line);
        if (line[0] != '\0')
            break;
    }
    if (line[0] == '\0') {
        snprintf(result.error, sizeof(result.error), "File is empty");
        return result;
    }

    char hdr_buf[FIELD_BUF];
    char *hdr_fields[MAX_COLS];
    int ncols = csv_parse_line(line, hdr_fields, MAX_COLS, hdr_buf, sizeof(hdr_buf));

    int col_date = -1, col_card = -1;
    int col_debit = -1, col_credit = -1;
    int col_amount = -1, col_txn_type = -1, col_desc = -1, col_txn_desc = -1;
    int col_category = -1;

    for (int i = 0; i < ncols; i++) {
        char norm[128];
        normalize_col(hdr_fields[i], norm, sizeof(norm));

        if (col_date < 0 &&
            (strcmp(norm, "transaction date") == 0 || strcmp(norm, "date") == 0)) {
            col_date = i;
        } else if (col_card < 0 && strstr(norm, "card")) {
            col_card = i;
        } else if (strcmp(norm, "debit") == 0) {
            col_debit = i;
        } else if (strcmp(norm, "credit") == 0) {
            col_credit = i;
        } else if (col_amount < 0 &&
                   (strcmp(norm, "transaction amount") == 0 ||
                    strcmp(norm, "amount") == 0)) {
            col_amount = i;
        } else if (col_txn_type < 0 &&
                   (strcmp(norm, "transaction type") == 0 ||
                    strcmp(norm, "type") == 0)) {
            col_txn_type = i;
        } else if (col_txn_desc < 0 &&
                   strcmp(norm, "transaction description") == 0) {
            col_txn_desc = i;
        } else if (col_desc < 0 &&
                   (strcmp(norm, "description") == 0 ||
                    strcmp(norm, "memo") == 0 || strcmp(norm, "payee") == 0 ||
                    strcmp(norm, "merchant") == 0)) {
            col_desc = i;
        } else if (col_category < 0 &&
                   (strcmp(norm, "category") == 0 ||
                    strcmp(norm, "transaction category") == 0)) {
            col_category = i;
        }
    }

    if (col_date < 0) {
        snprintf(result.error, sizeof(result.error), "No date column found");
        return result;
    }

    result.type = (col_card >= 0) ? CSV_TYPE_CREDIT_CARD : CSV_TYPE_CHECKING_SAVINGS;

    int capacity = 32;
    result.rows = malloc(capacity * sizeof(csv_row_t));
    if (!result.rows) {
        snprintf(result.error, sizeof(result.error), "Out of memory");
        result.type = CSV_TYPE_UNKNOWN;
        return result;
    }

    while (fgets(line, sizeof(line), f)) {
        strip_eol(line);
        if (line[0] == '\0')
            continue;

        char row_buf[FIELD_BUF];
        char *row_fields[MAX_COLS];
        int rnc = csv_parse_line(line, row_fields, MAX_COLS, row_buf, sizeof(row_buf));

        if (!ensure_result_capacity(&result, &capacity)) {
            snprintf(result.error, sizeof(result.error), "Out of memory");
            free(result.rows);
            result.rows = NULL;
            result.row_count = 0;
            result.type = CSV_TYPE_UNKNOWN;
            return result;
        }

        csv_row_t *row = &result.rows[result.row_count];
        memset(row, 0, sizeof(*row));

        // Date (required)
        if (col_date < rnc && row_fields[col_date][0]) {
            if (!normalize_date(row_fields[col_date], row->date)) {
                snprintf(row->date, sizeof(row->date), "%.10s", row_fields[col_date]);
            }
        } else {
            continue;
        }

        // Payee: CC uses "Description" column; checking/savings uses "Transaction Description"
        if (result.type == CSV_TYPE_CREDIT_CARD) {
            if (col_desc >= 0 && col_desc < rnc)
                snprintf(row->payee, sizeof(row->payee), "%s", row_fields[col_desc]);
        } else {
            if (col_txn_desc >= 0 && col_txn_desc < rnc)
                snprintf(row->payee, sizeof(row->payee), "%s", row_fields[col_txn_desc]);
            else if (col_desc >= 0 && col_desc < rnc)
                snprintf(row->payee, sizeof(row->payee), "%s", row_fields[col_desc]);
        }

        if (col_category >= 0 && col_category < rnc) {
            row->has_category = copy_import_category(
                row_fields[col_category], row->category, sizeof(row->category));
        }

        if (result.type == CSV_TYPE_CREDIT_CARD) {
            // Card last 4
            if (col_card >= 0 && col_card < rnc)
                extract_last4(row_fields[col_card], row->card_last4);

            // Debit → EXPENSE, Credit → INCOME
            int64_t debit_cents = 0, credit_cents = 0;
            if (col_debit >= 0 && col_debit < rnc && row_fields[col_debit][0])
                parse_csv_amount(row_fields[col_debit], &debit_cents);
            if (col_credit >= 0 && col_credit < rnc && row_fields[col_credit][0])
                parse_csv_amount(row_fields[col_credit], &credit_cents);

            if (debit_cents > 0) {
                row->amount_cents = debit_cents;
                row->type = TRANSACTION_EXPENSE;
            } else if (credit_cents > 0) {
                row->amount_cents = credit_cents;
                row->type = TRANSACTION_INCOME;
            } else {
                continue; // skip rows with no amount
            }
        } else {
            // Checking/savings: single amount column
            int64_t amount = 0;
            if (col_amount >= 0 && col_amount < rnc && row_fields[col_amount][0]) {
                parse_csv_amount(row_fields[col_amount], &amount);
            } else {
                continue;
            }

            if (col_txn_type >= 0 && col_txn_type < rnc &&
                row_fields[col_txn_type][0]) {
                char norm_type[64];
                normalize_col(row_fields[col_txn_type], norm_type, sizeof(norm_type));
                if (strstr(norm_type, "credit") || strstr(norm_type, "deposit") ||
                    strstr(norm_type, "income")) {
                    row->type = TRANSACTION_INCOME;
                } else {
                    row->type = TRANSACTION_EXPENSE;
                }
                row->amount_cents = amount < 0 ? -amount : amount;
            } else {
                // Use sign of amount
                if (amount >= 0) {
                    row->type = TRANSACTION_INCOME;
                    row->amount_cents = amount;
                } else {
                    row->type = TRANSACTION_EXPENSE;
                    row->amount_cents = -amount;
                }
            }
        }

        result.row_count++;
    }

    if (result.row_count == 0 && result.error[0] == '\0')
        snprintf(result.error, sizeof(result.error), "No transactions found in file");

    return result;
}

static bool qif_type_supports_transactions(const char *type_line) {
    char norm[32];
    normalize_col(type_line, norm, sizeof(norm));
    return strcmp(norm, "ccard") == 0 ||
           strcmp(norm, "bank") == 0 ||
           strcmp(norm, "cash") == 0;
}

static csv_parse_result_t qif_parse_stream(FILE *f) {
    csv_parse_result_t result = {0};
    result.type = CSV_TYPE_QIF;

    int capacity = 64;
    result.rows = malloc((size_t)capacity * sizeof(csv_row_t));
    if (!result.rows) {
        snprintf(result.error, sizeof(result.error), "Out of memory");
        result.type = CSV_TYPE_UNKNOWN;
        return result;
    }

    char line[LINE_BUF];
    bool in_account_block = false;
    bool in_txn_block = false;
    char pending_account[64] = "";
    char active_account[64] = "";
    char seen_account[64] = "";
    bool multi_account = false;

    char txn_date[64] = "";
    char txn_amount[64] = "";
    char txn_payee[128] = "";
    char txn_memo[256] = "";
    char txn_category[64] = "";

    while (fgets(line, sizeof(line), f)) {
        strip_eol(line);
        if (line[0] == '\0')
            continue;

        if (line[0] == '!') {
            in_txn_block = false;
            if (strcmp(line, "!Account") == 0) {
                in_account_block = true;
                pending_account[0] = '\0';
            } else if (strncmp(line, "!Type:", 6) == 0) {
                in_account_block = false;
                if (qif_type_supports_transactions(line + 6)) {
                    in_txn_block = true;
                    snprintf(active_account, sizeof(active_account), "%s",
                             pending_account);
                }
            }
            continue;
        }

        if (in_account_block) {
            if (line[0] == 'N') {
                snprintf(pending_account, sizeof(pending_account), "%s", line + 1);
            } else if (line[0] == '^') {
                in_account_block = false;
            }
            continue;
        }

        if (!in_txn_block)
            continue;

        if (line[0] == '^') {
            if (txn_date[0] && txn_amount[0]) {
                int64_t signed_amount = 0;
                if (parse_csv_amount(txn_amount, &signed_amount)) {
                    if (!ensure_result_capacity(&result, &capacity)) {
                        snprintf(result.error, sizeof(result.error), "Out of memory");
                        free(result.rows);
                        result.rows = NULL;
                        result.row_count = 0;
                        result.type = CSV_TYPE_UNKNOWN;
                        return result;
                    }

                    csv_row_t *row = &result.rows[result.row_count];
                    memset(row, 0, sizeof(*row));
                    if (!normalize_date(txn_date, row->date))
                        snprintf(row->date, sizeof(row->date), "%.10s", txn_date);
                    row->type = signed_amount < 0 ? TRANSACTION_EXPENSE
                                                  : TRANSACTION_INCOME;
                    row->amount_cents = signed_amount < 0 ? -signed_amount
                                                          : signed_amount;
                    snprintf(row->payee, sizeof(row->payee), "%s", txn_payee);
                    snprintf(row->description, sizeof(row->description), "%s",
                             txn_memo);
                    if (copy_import_category(txn_category, row->category,
                                             sizeof(row->category)) &&
                        row->category[0] != '[') {
                        row->has_category = true;
                    }
                    result.row_count++;

                    if (active_account[0] != '\0') {
                        if (seen_account[0] == '\0') {
                            snprintf(seen_account, sizeof(seen_account), "%s",
                                     active_account);
                        } else if (strcmp(seen_account, active_account) != 0) {
                            multi_account = true;
                        }
                    }
                }
            }

            txn_date[0] = '\0';
            txn_amount[0] = '\0';
            txn_payee[0] = '\0';
            txn_memo[0] = '\0';
            txn_category[0] = '\0';
            continue;
        }

        switch (line[0]) {
        case 'D':
            snprintf(txn_date, sizeof(txn_date), "%s", line + 1);
            break;
        case 'T':
            snprintf(txn_amount, sizeof(txn_amount), "%s", line + 1);
            break;
        case 'P':
            snprintf(txn_payee, sizeof(txn_payee), "%s", line + 1);
            break;
        case 'M':
            snprintf(txn_memo, sizeof(txn_memo), "%s", line + 1);
            break;
        case 'L':
            snprintf(txn_category, sizeof(txn_category), "%s", line + 1);
            break;
        default:
            break;
        }
    }

    if (multi_account) {
        snprintf(result.error, sizeof(result.error),
                 "QIF import supports one account per file.");
        free(result.rows);
        result.rows = NULL;
        result.row_count = 0;
        result.type = CSV_TYPE_UNKNOWN;
        return result;
    }
    if (seen_account[0] != '\0')
        snprintf(result.source_account, sizeof(result.source_account), "%s",
                 seen_account);
    if (result.row_count == 0 && result.error[0] == '\0')
        snprintf(result.error, sizeof(result.error), "No transactions found in file");

    return result;
}

static bool file_looks_like_qif(FILE *f) {
    char line[LINE_BUF];
    while (fgets(line, sizeof(line), f)) {
        strip_eol(line);
        if (line[0] == '\0')
            continue;
        return line[0] == '!';
    }
    return false;
}

csv_parse_result_t csv_parse_file(const char *path) {
    csv_parse_result_t result = {0};
    result.type = CSV_TYPE_UNKNOWN;

    // Expand leading ~ to $HOME
    char expanded[1024];
    if (path[0] == '~') {
        const char *home = getenv("HOME");
        if (home)
            snprintf(expanded, sizeof(expanded), "%s%s", home, path + 1);
        else
            snprintf(expanded, sizeof(expanded), "%s", path);
    } else {
        snprintf(expanded, sizeof(expanded), "%s", path);
    }

    FILE *f = fopen(expanded, "r");
    if (!f) {
        snprintf(result.error, sizeof(result.error), "Cannot open: %.240s", expanded);
        return result;
    }

    bool is_qif = file_looks_like_qif(f);
    rewind(f);
    result = is_qif ? qif_parse_stream(f) : csv_parse_stream(f);
    fclose(f);
    return result;
}

// Returns true if a CSV row matches an existing transaction (for dedup).
// Match key: date + amount_cents + type + payee.
static bool row_matches_txn(const csv_row_t *row, const txn_row_t *txn) {
    return row->amount_cents == txn->amount_cents &&
           row->type == txn->type &&
           strcmp(row->date, txn->date) == 0 &&
           strcmp(row->payee, txn->payee) == 0;
}

// Per-account cache of existing transactions used for dedup during import.
typedef struct {
    int64_t account_id;
    txn_row_t *txns;
    bool *consumed;
    int count;
} acct_txn_cache_t;

// Find or load a cache entry for account_id. Returns NULL on allocation failure.
// caches must have room for at least one more entry (caller ensures capacity).
static acct_txn_cache_t *get_acct_cache(sqlite3 *db, acct_txn_cache_t *caches,
                                         int *ncaches, int64_t account_id) {
    for (int i = 0; i < *ncaches; i++) {
        if (caches[i].account_id == account_id)
            return &caches[i];
    }

    acct_txn_cache_t *c = &caches[*ncaches];
    c->account_id = account_id;
    c->txns = NULL;
    c->consumed = NULL;
    c->count = 0;

    int cnt = db_get_transactions(db, account_id, &c->txns);
    if (cnt < 0) {
        free(c->txns);
        c->txns = NULL;
        return NULL;
    }
    c->count = cnt;

    c->consumed = calloc(c->count > 0 ? c->count : 1, sizeof(bool));
    if (!c->consumed) {
        free(c->txns);
        c->txns = NULL;
        c->count = 0;
        return NULL;
    }

    (*ncaches)++;
    return c;
}

void csv_parse_result_free(csv_parse_result_t *r) {
    if (!r)
        return;
    free(r->rows);
    r->rows = NULL;
    r->row_count = 0;
    r->type = CSV_TYPE_UNKNOWN;
    r->source_account[0] = '\0';
    r->error[0] = '\0';
}

int csv_import_credit_card(sqlite3 *db, const csv_parse_result_t *r,
                           int *imported, int *skipped) {
    *imported = 0;
    *skipped = 0;

    account_t *accounts = NULL;
    int account_count = db_get_accounts(db, &accounts);
    if (account_count < 0) {
        free(accounts);
        return -1;
    }

    // One cache entry per CC account (at most account_count entries needed).
    acct_txn_cache_t *caches = calloc(account_count > 0 ? account_count : 1,
                                       sizeof(acct_txn_cache_t));
    if (!caches) {
        free(accounts);
        return -1;
    }
    int ncaches = 0;
    int ret = 0;

    for (int i = 0; i < r->row_count; i++) {
        const csv_row_t *row = &r->rows[i];

        int64_t account_id = 0;
        for (int j = 0; j < account_count; j++) {
            if (accounts[j].type == ACCOUNT_CREDIT_CARD &&
                strcmp(accounts[j].card_last4, row->card_last4) == 0) {
                account_id = accounts[j].id;
                break;
            }
        }

        if (account_id == 0) {
            (*skipped)++;
            continue;
        }

        acct_txn_cache_t *cache = get_acct_cache(db, caches, &ncaches, account_id);
        if (!cache) {
            ret = -1;
            goto cleanup;
        }

        // Check for a matching unconsumed existing transaction (dedup).
        bool is_dup = false;
        for (int j = 0; j < cache->count; j++) {
            if (!cache->consumed[j] && row_matches_txn(row, &cache->txns[j])) {
                cache->consumed[j] = true;
                is_dup = true;
                break;
            }
        }
        if (is_dup) {
            (*skipped)++;
            continue;
        }

        transaction_t txn = {0};
        txn.amount_cents = row->amount_cents;
        txn.type = row->type;
        txn.account_id = account_id;
        snprintf(txn.date, sizeof(txn.date), "%s", row->date);
        snprintf(txn.payee, sizeof(txn.payee), "%s", row->payee);
        if (row->has_category) {
            txn.category_id = row->category_id;
        } else {
            if (db_get_most_recent_category_for_payee(
                    db, account_id, row->payee, row->type, &txn.category_id) < 0) {
                ret = -1;
                goto cleanup;
            }
        }

        if (db_insert_transaction(db, &txn) < 0) {
            ret = -1;
            goto cleanup;
        }
        (*imported)++;
    }

cleanup:
    for (int i = 0; i < ncaches; i++) {
        free(caches[i].txns);
        free(caches[i].consumed);
    }
    free(caches);
    free(accounts);
    return ret;
}

int csv_import_checking(sqlite3 *db, const csv_parse_result_t *r,
                        int64_t account_id, int *imported, int *skipped) {
    *imported = 0;
    *skipped = 0;

    txn_row_t *existing = NULL;
    int nexisting = db_get_transactions(db, account_id, &existing);
    if (nexisting < 0) {
        free(existing);
        return -1;
    }

    bool *consumed = calloc(nexisting > 0 ? nexisting : 1, sizeof(bool));
    if (!consumed) {
        free(existing);
        return -1;
    }

    int ret = 0;
    for (int i = 0; i < r->row_count; i++) {
        const csv_row_t *row = &r->rows[i];

        bool is_dup = false;
        for (int j = 0; j < nexisting; j++) {
            if (!consumed[j] && row_matches_txn(row, &existing[j])) {
                consumed[j] = true;
                is_dup = true;
                break;
            }
        }
        if (is_dup) {
            (*skipped)++;
            continue;
        }

        transaction_t txn = {0};
        txn.amount_cents = row->amount_cents;
        txn.type = row->type;
        txn.account_id = account_id;
        snprintf(txn.date, sizeof(txn.date), "%s", row->date);
        snprintf(txn.payee, sizeof(txn.payee), "%s", row->payee);
        if (row->has_category) {
            txn.category_id = row->category_id;
        } else {
            if (db_get_most_recent_category_for_payee(
                    db, account_id, row->payee, row->type, &txn.category_id) < 0) {
                ret = -1;
                break;
            }
        }

        if (db_insert_transaction(db, &txn) < 0) {
            ret = -1;
            break;
        }
        (*imported)++;
    }

    free(existing);
    free(consumed);
    return ret;
}
