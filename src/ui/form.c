#include "ui/form.h"
#include "ui/colors.h"
#include "ui/resize.h"
#include "db/query.h"
#include "models/account.h"
#include "models/category.h"
#include "models/transaction.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FORM_WIDTH 56
#define FORM_HEIGHT 23
#define CATEGORY_FORM_WIDTH 56
#define CATEGORY_FORM_HEIGHT 12
#define CATEGORY_FIELD_ROW 2
#define LABEL_COL 2
#define FIELD_COL 21
#define FIELD_WIDTH 30
#define MAX_DROP 5

enum {
    FIELD_TYPE,
    FIELD_AMOUNT,
    FIELD_ACCOUNT,
    FIELD_CATEGORY,
    FIELD_DATE,
    FIELD_REFLECTION_DATE,
    FIELD_PAYEE,
    FIELD_DESC,
    FIELD_SUBMIT,
    FIELD_COUNT
};


typedef struct {
    WINDOW *win;
    sqlite3 *db;
    transaction_t *txn;
    bool is_edit;

    int current_field;
    bool dropdown_open;
    int dropdown_sel;
    int dropdown_scroll;
    char dropdown_filter[64];
    int dropdown_filter_len;

    // Type toggle
    transaction_type_t txn_type;
    int64_t transfer_id;

    // Text fields
    char amount[32];
    int amount_pos;
    char date[11];
    int date_pos;
    char reflection_date[11];
    int reflection_date_pos;
    char payee[128];
    int payee_pos;
    char desc[256];
    int desc_pos;

    // Account dropdown
    account_t *accounts;
    int account_count;
    int account_sel;
    int transfer_account_sel;

    // Category dropdown
    category_t *categories;
    int category_count;
    int category_sel;

    // Error message
    char error[64];

    // Post-save category propagation offer state
    bool offer_category_propagation;
    char offer_payee[128];
    transaction_type_t offer_type;
    int64_t offer_category_id;
} form_state_t;

static int first_other_account_index(const form_state_t *fs, int current_idx) {
    for (int i = 0; i < fs->account_count; i++) {
        if (i != current_idx)
            return i;
    }
    return current_idx;
}

static bool field_hidden(const form_state_t *fs, int field) {
    return (field == FIELD_PAYEE && fs->txn_type == TRANSACTION_TRANSFER);
}

static void move_to_next_field(form_state_t *fs) {
    int f = fs->current_field;
    while (f < FIELD_COUNT - 1) {
        f++;
        if (!field_hidden(fs, f)) {
            fs->current_field = f;
            return;
        }
    }
}

static void move_to_prev_field(form_state_t *fs) {
    int f = fs->current_field;
    while (f > 0) {
        f--;
        if (!field_hidden(fs, f)) {
            fs->current_field = f;
            return;
        }
    }
}

static int next_account_index(const form_state_t *fs, int current, int delta,
                              int avoid_idx) {
    if (fs->account_count <= 0)
        return current;
    int idx = current;
    for (int i = 0; i < fs->account_count; i++) {
        idx = (idx + delta + fs->account_count) % fs->account_count;
        if (idx != avoid_idx)
            return idx;
    }
    return current;
}

static bool field_is_text_entry(const form_state_t *fs, int field) {
    if (field == FIELD_AMOUNT || field == FIELD_DATE ||
        field == FIELD_REFLECTION_DATE || field == FIELD_DESC)
        return true;
    if (field == FIELD_PAYEE)
        return !field_hidden(fs, FIELD_PAYEE);
    return false;
}

static bool contains_case_insensitive(const char *haystack,
                                      const char *needle) {
    if (!haystack || !needle)
        return false;
    if (needle[0] == '\0')
        return true;

    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen > hlen)
        return false;

    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t j = 0;
        while (j < nlen) {
            unsigned char hc = (unsigned char)haystack[i + j];
            unsigned char nc = (unsigned char)needle[j];
            if (tolower(hc) != tolower(nc))
                break;
            j++;
        }
        if (j == nlen)
            return true;
    }
    return false;
}

static int form_dropdown_count(const form_state_t *fs) {
    if (fs->current_field == FIELD_ACCOUNT)
        return fs->account_count;
    if (fs->current_field == FIELD_CATEGORY) {
        if (fs->txn_type == TRANSACTION_TRANSFER)
            return fs->account_count;
        return fs->category_count + 1;
    }
    return 0;
}

static const char *form_dropdown_item_name(const form_state_t *fs, int idx) {
    if (fs->current_field == FIELD_ACCOUNT)
        return fs->accounts[idx].name;
    if (fs->txn_type == TRANSACTION_TRANSFER)
        return fs->accounts[idx].name;
    if (idx == fs->category_count)
        return "<Add category>";
    return fs->categories[idx].name;
}

static void form_reset_dropdown_filter(form_state_t *fs) {
    fs->dropdown_filter_len = 0;
    fs->dropdown_filter[0] = '\0';
}

static bool form_dropdown_find_match(const form_state_t *fs, const char *query,
                                     int *out_idx) {
    if (!out_idx || !query || query[0] == '\0')
        return false;

    int count = form_dropdown_count(fs);
    for (int i = 0; i < count; i++) {
        if (contains_case_insensitive(form_dropdown_item_name(fs, i), query)) {
            *out_idx = i;
            return true;
        }
    }
    return false;
}

static bool form_dropdown_handle_filter_key(form_state_t *fs, int ch) {
    if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
        if (fs->dropdown_filter_len > 0) {
            fs->dropdown_filter_len--;
            fs->dropdown_filter[fs->dropdown_filter_len] = '\0';
            if (fs->dropdown_filter_len > 0) {
                int idx = fs->dropdown_sel;
                if (form_dropdown_find_match(fs, fs->dropdown_filter, &idx))
                    fs->dropdown_sel = idx;
            }
        }
        return true;
    }

    if (!isprint((unsigned char)ch))
        return false;

    if (fs->dropdown_filter_len >= (int)sizeof(fs->dropdown_filter) - 1)
        return true;

    char candidate[sizeof(fs->dropdown_filter)];
    memcpy(candidate, fs->dropdown_filter, (size_t)fs->dropdown_filter_len);
    candidate[fs->dropdown_filter_len] = (char)ch;
    candidate[fs->dropdown_filter_len + 1] = '\0';

    int idx = fs->dropdown_sel;
    if (form_dropdown_find_match(fs, candidate, &idx)) {
        fs->dropdown_filter_len++;
        fs->dropdown_filter[fs->dropdown_filter_len - 1] = (char)ch;
        fs->dropdown_filter[fs->dropdown_filter_len] = '\0';
        fs->dropdown_sel = idx;
        return true;
    }

    candidate[0] = (char)ch;
    candidate[1] = '\0';
    idx = fs->dropdown_sel;
    if (form_dropdown_find_match(fs, candidate, &idx)) {
        fs->dropdown_filter_len = 1;
        fs->dropdown_filter[0] = (char)ch;
        fs->dropdown_filter[1] = '\0';
        fs->dropdown_sel = idx;
    }
    return true;
}

static void form_load_categories(form_state_t *fs) {
    free(fs->categories);
    fs->categories = NULL;
    fs->category_count = 0;
    fs->category_sel = 0;

    if (fs->txn_type == TRANSACTION_TRANSFER)
        return;

    category_type_t ctype = (fs->txn_type == TRANSACTION_INCOME)
                                ? CATEGORY_INCOME
                                : CATEGORY_EXPENSE;
    int count = db_get_categories(fs->db, ctype, &fs->categories);
    if (count > 0) {
        fs->category_count = count;
    }
}

static void format_amount_string(int64_t cents, char *buf, size_t buflen) {
    int64_t abs_cents = cents < 0 ? -cents : cents;
    int64_t whole = abs_cents / 100;
    int64_t frac = abs_cents % 100;
    snprintf(buf, buflen, "%ld.%02ld", (long)whole, (long)frac);
}

static void form_init_state(form_state_t *fs, sqlite3 *db, transaction_t *txn,
                            bool is_edit) {
    memset(fs, 0, sizeof(*fs));
    fs->db = db;
    fs->txn = txn;
    fs->is_edit = is_edit;
    fs->txn_type = TRANSACTION_EXPENSE;

    // Load accounts
    int count = db_get_accounts(db, &fs->accounts);
    if (count > 0) {
        fs->account_count = count;
        fs->transfer_account_sel = (count > 1) ? 1 : 0;
    }

    // Default date to today
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char datebuf[32];
    snprintf(datebuf, sizeof(datebuf), "%04d-%02d-%02d", tm->tm_year + 1900,
             tm->tm_mon + 1, tm->tm_mday);
    memcpy(fs->date, datebuf, 10);
    fs->date[10] = '\0';
    fs->date_pos = 10;
    fs->reflection_date[0] = '\0';
    fs->reflection_date_pos = 0;

    if (is_edit && txn) {
        fs->txn_type = txn->type;
        fs->transfer_id = txn->transfer_id;
        format_amount_string(txn->amount_cents, fs->amount, sizeof(fs->amount));
        fs->amount_pos = (int)strlen(fs->amount);
        if (txn->date[0] != '\0') {
            snprintf(fs->date, sizeof(fs->date), "%s", txn->date);
            fs->date_pos = (int)strlen(fs->date);
        }
        if (txn->reflection_date[0] != '\0') {
            snprintf(fs->reflection_date, sizeof(fs->reflection_date), "%s",
                     txn->reflection_date);
            fs->reflection_date_pos = (int)strlen(fs->reflection_date);
        }
        if (txn->payee[0] != '\0') {
            snprintf(fs->payee, sizeof(fs->payee), "%s", txn->payee);
            fs->payee_pos = (int)strlen(fs->payee);
        }
        if (txn->description[0] != '\0') {
            snprintf(fs->desc, sizeof(fs->desc), "%s", txn->description);
            fs->desc_pos = (int)strlen(fs->desc);
        }
    }

    // Load categories for current type
    form_load_categories(fs);

    if (is_edit && txn) {
        for (int i = 0; i < fs->account_count; i++) {
            if (fs->accounts[i].id == txn->account_id) {
                fs->account_sel = i;
                break;
            }
        }
        if (fs->txn_type != TRANSACTION_TRANSFER && txn->category_id > 0) {
            for (int i = 0; i < fs->category_count; i++) {
                if (fs->categories[i].id == txn->category_id) {
                    fs->category_sel = i;
                    break;
                }
            }
        } else if (fs->txn_type == TRANSACTION_TRANSFER) {
            int64_t other_account_id = 0;
            if (db_get_transfer_counterparty_account(fs->db, txn->id,
                                                     &other_account_id) == 0) {
                for (int i = 0; i < fs->account_count; i++) {
                    if (fs->accounts[i].id == other_account_id) {
                        fs->transfer_account_sel = i;
                        break;
                    }
                }
            } else {
                fs->transfer_account_sel =
                    first_other_account_index(fs, fs->account_sel);
            }
        }
    }
}

static void form_cleanup_state(form_state_t *fs) {
    free(fs->accounts);
    free(fs->categories);
    if (fs->win) {
        delwin(fs->win);
    }
}

static const char *field_labels[FIELD_SUBMIT] = {
    "Type", "Amount", "Account", "Category", "Transaction Date",
    "Reflection Date", "Payee", "Description"};

static int field_row(int field) {
    // Row within form window for each field (after title and border)
    return 2 + field * 2;
}

static void form_draw(form_state_t *fs) {
    WINDOW *w = fs->win;
    werase(w);
    wbkgd(w, COLOR_PAIR(COLOR_FORM));
    box(w, 0, 0);

    // Title
    const char *title =
        fs->is_edit ? " Edit Transaction " : " Add Transaction ";
    int tw = (int)strlen(title);
    int ww = getmaxx(w);
    mvwprintw(w, 0, (ww - tw) / 2, "%s", title);

    for (int i = 0; i < FIELD_SUBMIT; i++) {
        if (field_hidden(fs, i))
            continue;
        int row = field_row(i);
        bool active = (i == fs->current_field && !fs->dropdown_open);

        // Label
        if (i == FIELD_CATEGORY && fs->txn_type == TRANSACTION_TRANSFER)
            mvwprintw(w, row, LABEL_COL, "To Account:");
        else
            mvwprintw(w, row, LABEL_COL, "%s:", field_labels[i]);

        // Field value
        if (active)
            wattron(w, COLOR_PAIR(COLOR_FORM_ACTIVE));

        // Draw field background
        mvwprintw(w, row, FIELD_COL, "%-*s", FIELD_WIDTH, "");

        switch (i) {
        case FIELD_TYPE:
            if (fs->txn_type == TRANSACTION_EXPENSE)
                mvwprintw(w, row, FIELD_COL, "< Expense >");
            else if (fs->txn_type == TRANSACTION_INCOME)
                mvwprintw(w, row, FIELD_COL, "< Income >");
            else
                mvwprintw(w, row, FIELD_COL, "< Transfer >");
            break;
        case FIELD_AMOUNT:
            mvwprintw(w, row, FIELD_COL, "%s", fs->amount);
            break;
        case FIELD_ACCOUNT:
            if (fs->account_count > 0)
                mvwprintw(w, row, FIELD_COL, "▾ %s",
                          fs->accounts[fs->account_sel].name);
            else
                mvwprintw(w, row, FIELD_COL, "(none)");
            break;
        case FIELD_CATEGORY:
            if (fs->txn_type == TRANSACTION_TRANSFER) {
                if (fs->account_count > 1) {
                    mvwprintw(w, row, FIELD_COL, "▾ %s",
                              fs->accounts[fs->transfer_account_sel].name);
                } else {
                    mvwprintw(w, row, FIELD_COL, "(none)");
                }
            } else if (fs->category_count > 0) {
                mvwprintw(w, row, FIELD_COL, "▾ %s",
                          fs->categories[fs->category_sel].name);
            } else {
                mvwprintw(w, row, FIELD_COL, "(none)");
            }
            break;
        case FIELD_DATE:
            mvwprintw(w, row, FIELD_COL, "%s", fs->date);
            break;
        case FIELD_REFLECTION_DATE:
            mvwprintw(w, row, FIELD_COL, "%s", fs->reflection_date);
            break;
        case FIELD_PAYEE:
            mvwprintw(w, row, FIELD_COL, "%s", fs->payee);
            break;
        case FIELD_DESC:
            mvwprintw(w, row, FIELD_COL, "%s", fs->desc);
            break;
        }

        if (active)
            wattroff(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
    }

    // Submit button
    int submit_row = field_row(FIELD_SUBMIT);
    bool submit_active =
        (fs->current_field == FIELD_SUBMIT && !fs->dropdown_open);
    const char *btn = "[ Submit ]";
    int btn_len = (int)strlen(btn);
    if (submit_active)
        wattron(w, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);
    mvwprintw(w, submit_row, (ww - btn_len) / 2, "%s", btn);
    if (submit_active)
        wattroff(w, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);

    // Error message
    if (fs->error[0] != '\0') {
        wattron(w, A_BOLD);
        mvwprintw(w, FORM_HEIGHT - 2, LABEL_COL, "%s", fs->error);
        wattroff(w, A_BOLD);
    }

    // Footer hints
    mvwprintw(w, FORM_HEIGHT - 1, 2, " C-s:Save  Esc:Cancel  n:New Category ");

    // Position cursor on active text field
    if (!fs->dropdown_open) {
        if (fs->current_field == FIELD_SUBMIT ||
            !field_is_text_entry(fs, fs->current_field)) {
            curs_set(0);
        } else {
            curs_set(1);
            int row = field_row(fs->current_field);
            switch (fs->current_field) {
            case FIELD_AMOUNT:
                wmove(w, row, FIELD_COL + fs->amount_pos);
                break;
            case FIELD_DATE:
                wmove(w, row, FIELD_COL + fs->date_pos);
                break;
            case FIELD_REFLECTION_DATE:
                wmove(w, row, FIELD_COL + fs->reflection_date_pos);
                break;
            case FIELD_PAYEE:
                wmove(w, row, FIELD_COL + fs->payee_pos);
                break;
            case FIELD_DESC:
                wmove(w, row, FIELD_COL + fs->desc_pos);
                break;
            default:
                wmove(w, row, FIELD_COL);
                break;
            }
        }
    }

    wnoutrefresh(w);
}

static void form_draw_dropdown(form_state_t *fs) {
    WINDOW *w = fs->win;
    int base_row;
    int count;
    if (fs->current_field == FIELD_ACCOUNT) {
        base_row = field_row(FIELD_ACCOUNT) + 1;
        count = fs->account_count;
    } else {
        base_row = field_row(FIELD_CATEGORY) + 1;
        count = (fs->txn_type == TRANSACTION_TRANSFER) ? fs->account_count
                                                       : fs->category_count + 1;
    }

    int visible = count < MAX_DROP ? count : MAX_DROP;

    // Clamp scroll
    if (fs->dropdown_sel < fs->dropdown_scroll)
        fs->dropdown_scroll = fs->dropdown_sel;
    if (fs->dropdown_sel >= fs->dropdown_scroll + visible)
        fs->dropdown_scroll = fs->dropdown_sel - visible + 1;

    for (int i = 0; i < visible; i++) {
        int idx = fs->dropdown_scroll + i;
        bool selected = (idx == fs->dropdown_sel);

        if (selected) {
            wattron(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
        } else {
            wattron(w, COLOR_PAIR(COLOR_FORM_DROPDOWN));
        }

        const char *name = form_dropdown_item_name(fs, idx);

        mvwprintw(w, base_row + i, FIELD_COL, "%-*s", FIELD_WIDTH, name);

        if (selected) {
            wattroff(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
        } else {
            wattroff(w, COLOR_PAIR(COLOR_FORM_DROPDOWN));
        }
    }

    // Show scroll indicators
    if (fs->dropdown_scroll > 0)
        mvwaddch(w, base_row, FIELD_COL + FIELD_WIDTH, ACS_UARROW);
    if (fs->dropdown_scroll + visible < count)
        mvwaddch(w, base_row + visible - 1, FIELD_COL + FIELD_WIDTH,
                 ACS_DARROW);

    wmove(w, base_row + fs->dropdown_sel - fs->dropdown_scroll, FIELD_COL);
    wnoutrefresh(w);
}

static void form_open_dropdown(form_state_t *fs) {
    int count = 0;
    int sel = 0;

    if (fs->current_field == FIELD_ACCOUNT) {
        count = fs->account_count;
        sel = fs->account_sel;
    } else if (fs->current_field == FIELD_CATEGORY) {
        if (fs->txn_type == TRANSACTION_TRANSFER) {
            count = fs->account_count;
            sel = fs->transfer_account_sel;
        } else {
            count = fs->category_count + 1;
            sel = fs->category_sel;
        }
    }

    if (count == 0)
        return;

    fs->dropdown_open = true;
    fs->dropdown_sel = sel;
    fs->dropdown_scroll = 0;
    form_reset_dropdown_filter(fs);
    // Ensure selected item is visible
    int visible = count < MAX_DROP ? count : MAX_DROP;
    if (sel >= visible)
        fs->dropdown_scroll = sel - visible + 1;
}

static void form_close_dropdown(form_state_t *fs, bool accept) {
    if (accept) {
        if (fs->current_field == FIELD_ACCOUNT) {
            fs->account_sel = fs->dropdown_sel;
            if (fs->txn_type == TRANSACTION_TRANSFER &&
                fs->transfer_account_sel == fs->account_sel &&
                fs->account_count > 1) {
                fs->transfer_account_sel =
                    first_other_account_index(fs, fs->account_sel);
            }
        } else if (fs->current_field == FIELD_CATEGORY) {
            if (fs->txn_type == TRANSACTION_TRANSFER)
                fs->transfer_account_sel = fs->dropdown_sel;
            else
                fs->category_sel = fs->dropdown_sel;
        }
    }
    fs->dropdown_open = false;
    fs->dropdown_scroll = 0;
    form_reset_dropdown_filter(fs);
}

static void maybe_propagate_category_to_payee(WINDOW *parent, form_state_t *fs);

static void category_form_draw(form_state_t *fs) {
    WINDOW *w = fs->win;
    werase(w);
    wbkgd(w, COLOR_PAIR(COLOR_FORM));
    box(w, 0, 0);

    const char *title = " Edit Category ";
    int ww = getmaxx(w);
    int tw = (int)strlen(title);
    mvwprintw(w, 0, (ww - tw) / 2, "%s", title);

    mvwprintw(w, CATEGORY_FIELD_ROW, LABEL_COL, "Category:");
    if (!fs->dropdown_open)
        wattron(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
    mvwprintw(w, CATEGORY_FIELD_ROW, FIELD_COL, "%-*s", FIELD_WIDTH, "");
    if (fs->category_count > 0) {
        mvwprintw(w, CATEGORY_FIELD_ROW, FIELD_COL, "▾ %s",
                  fs->categories[fs->category_sel].name);
    } else {
        mvwprintw(w, CATEGORY_FIELD_ROW, FIELD_COL, "(none)");
    }
    if (!fs->dropdown_open)
        wattroff(w, COLOR_PAIR(COLOR_FORM_ACTIVE));

    if (fs->error[0] != '\0') {
        wattron(w, A_BOLD);
        mvwprintw(w, CATEGORY_FORM_HEIGHT - 2, LABEL_COL, "%s", fs->error);
        wattroff(w, A_BOLD);
    }

    mvwprintw(w, CATEGORY_FORM_HEIGHT - 1, 2,
              " \u2191\u2193:Select  Enter:Apply  n:New Category  Esc:Cancel ");
    curs_set(0);
    wnoutrefresh(w);
}

static void category_form_draw_dropdown(form_state_t *fs) {
    WINDOW *w = fs->win;
    int count = fs->category_count + 1;
    int base_row = CATEGORY_FIELD_ROW + 1;
    int visible = count < MAX_DROP ? count : MAX_DROP;

    if (fs->dropdown_sel < fs->dropdown_scroll)
        fs->dropdown_scroll = fs->dropdown_sel;
    if (fs->dropdown_sel >= fs->dropdown_scroll + visible)
        fs->dropdown_scroll = fs->dropdown_sel - visible + 1;

    for (int i = 0; i < visible; i++) {
        int idx = fs->dropdown_scroll + i;
        bool selected = (idx == fs->dropdown_sel);
        if (selected) {
            wattron(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
        } else {
            wattron(w, COLOR_PAIR(COLOR_FORM_DROPDOWN));
        }

        const char *name =
            (idx == fs->category_count) ? "<Add category>"
                                        : fs->categories[idx].name;
        mvwprintw(w, base_row + i, FIELD_COL, "%-*s", FIELD_WIDTH, name);

        if (selected) {
            wattroff(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
        } else {
            wattroff(w, COLOR_PAIR(COLOR_FORM_DROPDOWN));
        }
    }

    if (fs->dropdown_scroll > 0)
        mvwaddch(w, base_row, FIELD_COL + FIELD_WIDTH, ACS_UARROW);
    if (fs->dropdown_scroll + visible < count)
        mvwaddch(w, base_row + visible - 1, FIELD_COL + FIELD_WIDTH,
                 ACS_DARROW);

    wnoutrefresh(w);
}

static bool form_save_category_only(WINDOW *parent, form_state_t *fs) {
    fs->error[0] = '\0';
    fs->offer_category_propagation = false;

    if (!fs->txn || fs->txn_type == TRANSACTION_TRANSFER ||
        fs->category_count <= 0) {
        snprintf(fs->error, sizeof(fs->error), "No category available");
        return false;
    }

    int64_t prior_category_id = fs->txn->category_id;
    transaction_t txn = *fs->txn;
    txn.category_id = fs->categories[fs->category_sel].id;
    txn.transfer_id = 0;

    int rc = db_update_transaction(fs->db, &txn);
    if (rc == -2) {
        snprintf(fs->error, sizeof(fs->error), "Transaction not found");
        return false;
    }
    if (rc < 0) {
        snprintf(fs->error, sizeof(fs->error), "Database error");
        return false;
    }

    *fs->txn = txn;

    if (prior_category_id <= 0 && txn.category_id > 0 && txn.payee[0] != '\0') {
        fs->offer_category_propagation = true;
        fs->offer_type = txn.type;
        fs->offer_category_id = txn.category_id;
        snprintf(fs->offer_payee, sizeof(fs->offer_payee), "%s", txn.payee);
    }
    maybe_propagate_category_to_payee(parent, fs);
    return true;
}

static transaction_type_t next_type(transaction_type_t type) {
    if (type == TRANSACTION_EXPENSE)
        return TRANSACTION_INCOME;
    if (type == TRANSACTION_INCOME)
        return TRANSACTION_TRANSFER;
    return TRANSACTION_EXPENSE;
}

static transaction_type_t prev_type(transaction_type_t type) {
    if (type == TRANSACTION_EXPENSE)
        return TRANSACTION_TRANSFER;
    if (type == TRANSACTION_INCOME)
        return TRANSACTION_EXPENSE;
    return TRANSACTION_INCOME;
}

// Parse amount string to cents. Returns -1 on invalid input.
static int64_t parse_amount_cents(const char *str) {
    if (str[0] == '\0')
        return -1;

    // Find decimal point
    const char *dot = strchr(str, '.');
    int64_t whole = 0;
    int64_t frac = 0;

    if (dot) {
        // Parse whole part
        for (const char *p = str; p < dot; p++) {
            if (!isdigit((unsigned char)*p))
                return -1;
            whole = whole * 10 + (*p - '0');
        }
        // Parse fractional part (up to 2 digits)
        int frac_digits = 0;
        for (const char *p = dot + 1; *p; p++) {
            if (!isdigit((unsigned char)*p))
                return -1;
            if (frac_digits < 2) {
                frac = frac * 10 + (*p - '0');
                frac_digits++;
            } else {
                return -1; // too many decimal places
            }
        }
        // Pad to 2 decimal places
        if (frac_digits == 1)
            frac *= 10;
    } else {
        // No decimal point - whole number
        for (const char *p = str; *p; p++) {
            if (!isdigit((unsigned char)*p))
                return -1;
            whole = whole * 10 + (*p - '0');
        }
    }

    return whole * 100 + frac;
}

static bool validate_date(const char *str) {
    if (strlen(str) != 10)
        return false;
    // Check format: YYYY-MM-DD
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) {
            if (str[i] != '-')
                return false;
        } else {
            if (!isdigit((unsigned char)str[i]))
                return false;
        }
    }
    return true;
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

static bool parse_category_path(const char *input, char *parent, size_t parent_sz,
                                char *child, size_t child_sz, bool *has_parent) {
    if (!input || !parent || !child || !has_parent || parent_sz == 0 ||
        child_sz == 0)
        return false;

    parent[0] = '\0';
    child[0] = '\0';
    *has_parent = false;

    char buf[128];
    snprintf(buf, sizeof(buf), "%s", input);
    trim_whitespace_in_place(buf);
    if (buf[0] == '\0')
        return false;

    char *first_colon = strchr(buf, ':');
    if (!first_colon) {
        snprintf(child, child_sz, "%s", buf);
        return true;
    }
    if (strchr(first_colon + 1, ':'))
        return false;

    *first_colon = '\0';
    char *child_part = first_colon + 1;
    trim_whitespace_in_place(buf);
    trim_whitespace_in_place(child_part);
    if (buf[0] == '\0' || child_part[0] == '\0')
        return false;

    snprintf(parent, parent_sz, "%s", buf);
    snprintf(child, child_sz, "%s", child_part);
    *has_parent = true;
    return true;
}

static bool prompt_category_path(WINDOW *parent, category_type_t ctype, char *out,
                                 size_t out_sz) {
    if (!parent || !out || out_sz == 0)
        return false;

    int ph, pw;
    getmaxyx(parent, ph, pw);
    int win_h = 8;
    int win_w = 68;
    if (ph < win_h)
        win_h = ph;
    if (pw < win_w)
        win_w = pw;
    if (win_h < 6 || win_w < 42)
        return false;

    int py, px;
    getbegyx(parent, py, px);
    int win_y = py + (ph - win_h) / 2;
    int win_x = px + (pw - win_w) / 2;

    WINDOW *w = newwin(win_h, win_w, win_y, win_x);
    keypad(w, TRUE);
    wbkgd(w, COLOR_PAIR(COLOR_FORM));

    char buf[64] = {0};
    int pos = 0;
    bool submitted = false;
    bool done = false;

    while (!done) {
        werase(w);
        box(w, 0, 0);
        mvwprintw(w, 1, 2, "New %s category:",
                  ctype == CATEGORY_INCOME ? "income" : "expense");
        mvwprintw(w, 2, 2, "Use Parent:Child for sub-categories");
        mvwprintw(w, 4, 2, "%-*s", win_w - 4, "");
        mvwprintw(w, 4, 2, "%s", buf);
        mvwprintw(w, win_h - 2, 2, "Enter:Create  Esc:Cancel");
        wmove(w, 4, 2 + pos);
        curs_set(1);
        wrefresh(w);

        int ch = wgetch(w);
        if (ui_requeue_resize_event(ch)) {
            done = true;
            submitted = false;
            continue;
        }
        if (ch == 27 || ch == KEY_EXIT) {
            flushinp();
            done = true;
            submitted = false;
            continue;
        }
        if (ch == '\n') {
            submitted = true;
            done = true;
            continue;
        }
        if (ch == KEY_LEFT) {
            if (pos > 0)
                pos--;
            continue;
        }
        if (ch == KEY_RIGHT) {
            int len = (int)strlen(buf);
            if (pos < len)
                pos++;
            continue;
        }
        if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
            int len = (int)strlen(buf);
            if (pos > 0) {
                memmove(&buf[pos - 1], &buf[pos], (size_t)(len - pos + 1));
                pos--;
            }
            continue;
        }
        if (isprint(ch)) {
            int len = (int)strlen(buf);
            if (len < (int)sizeof(buf) - 1) {
                memmove(&buf[pos + 1], &buf[pos], (size_t)(len - pos + 1));
                buf[pos] = (char)ch;
                pos++;
            }
        }
    }

    curs_set(0);
    delwin(w);
    touchwin(parent);

    if (!submitted)
        return false;

    snprintf(out, out_sz, "%s", buf);
    trim_whitespace_in_place(out);
    return out[0] != '\0';
}

static bool form_create_category_on_the_fly(WINDOW *parent, form_state_t *fs) {
    if (!fs || fs->txn_type == TRANSACTION_TRANSFER)
        return false;

    category_type_t ctype = (fs->txn_type == TRANSACTION_INCOME)
                                ? CATEGORY_INCOME
                                : CATEGORY_EXPENSE;
    char input[64];
    if (!prompt_category_path(parent, ctype, input, sizeof(input)))
        return false;

    char parent_name[64];
    char child_name[64];
    bool has_parent = false;
    if (!parse_category_path(input, parent_name, sizeof(parent_name), child_name,
                             sizeof(child_name), &has_parent)) {
        snprintf(fs->error, sizeof(fs->error), "Invalid category path");
        return false;
    }

    int64_t category_id = 0;
    if (has_parent) {
        int64_t parent_id =
            db_get_or_create_category(fs->db, ctype, parent_name, 0);
        if (parent_id <= 0) {
            snprintf(fs->error, sizeof(fs->error), "Database error");
            return false;
        }
        category_id =
            db_get_or_create_category(fs->db, ctype, child_name, parent_id);
    } else {
        category_id = db_get_or_create_category(fs->db, ctype, child_name, 0);
    }

    if (category_id <= 0) {
        snprintf(fs->error, sizeof(fs->error), "Database error");
        return false;
    }

    form_load_categories(fs);
    for (int i = 0; i < fs->category_count; i++) {
        if (fs->categories[i].id == category_id) {
            fs->category_sel = i;
            return true;
        }
    }

    snprintf(fs->error, sizeof(fs->error), "Category created but not loaded");
    return false;
}

static bool confirm_apply_category_to_payee(WINDOW *parent, const char *payee,
                                            int64_t match_count) {
    int ph, pw;
    getmaxyx(parent, ph, pw);

    int win_h = 8;
    int win_w = 64;
    if (ph < win_h)
        win_h = ph;
    if (pw < win_w)
        win_w = pw;
    if (win_h < 5 || win_w < 34)
        return false;

    int py, px;
    getbegyx(parent, py, px);
    int win_y = py + (ph - win_h) / 2;
    int win_x = px + (pw - win_w) / 2;

    WINDOW *w = newwin(win_h, win_w, win_y, win_x);
    keypad(w, TRUE);
    wbkgd(w, COLOR_PAIR(COLOR_FORM));
    box(w, 0, 0);

    char payee_line[96];
    snprintf(payee_line, sizeof(payee_line), "Payee '%-.32s' has %ld uncategorized match%s.",
             payee, (long)match_count, match_count == 1 ? "" : "es");
    mvwprintw(w, 1, 2, "Apply this category to matching transactions?");
    mvwprintw(w, 3, 2, "%s", payee_line);
    mvwprintw(w, win_h - 2, 2, "y:Apply  n:Skip");
    wrefresh(w);

    bool confirm = false;
    bool done = false;
    while (!done) {
        int ch = wgetch(w);
        if (ui_requeue_resize_event(ch)) {
            done = true;
            confirm = false;
            continue;
        }
        switch (ch) {
        case 'y':
        case 'Y':
            confirm = true;
            done = true;
            break;
        case 'n':
        case 'N':
        case 27:
            confirm = false;
            done = true;
            break;
        }
    }

    delwin(w);
    touchwin(parent);
    return confirm;
}

static void maybe_propagate_category_to_payee(WINDOW *parent, form_state_t *fs) {
    if (!fs->offer_category_propagation)
        return;
    fs->offer_category_propagation = false;

    int64_t match_count = 0;
    if (db_count_uncategorized_by_payee(fs->db, fs->offer_payee, fs->offer_type,
                                        &match_count) < 0)
        return;
    if (match_count <= 0)
        return;
    if (!confirm_apply_category_to_payee(parent, fs->offer_payee, match_count))
        return;

    db_apply_category_to_uncategorized_by_payee(fs->db, fs->offer_payee,
                                                fs->offer_type,
                                                fs->offer_category_id);
}

static bool form_validate_and_save(form_state_t *fs) {
    fs->error[0] = '\0';
    fs->offer_category_propagation = false;

    int64_t prior_category_id = 0;
    if (fs->is_edit && fs->txn)
        prior_category_id = fs->txn->category_id;

    // Validate amount
    int64_t cents = parse_amount_cents(fs->amount);
    if (cents <= 0) {
        snprintf(fs->error, sizeof(fs->error), "Invalid amount");
        fs->current_field = FIELD_AMOUNT;
        return false;
    }

    // Validate date
    if (!validate_date(fs->date)) {
        snprintf(fs->error, sizeof(fs->error), "Invalid date (YYYY-MM-DD)");
        fs->current_field = FIELD_DATE;
        return false;
    }
    if (fs->reflection_date[0] != '\0' &&
        !validate_date(fs->reflection_date)) {
        snprintf(fs->error, sizeof(fs->error),
                 "Invalid reflection date (YYYY-MM-DD)");
        fs->current_field = FIELD_REFLECTION_DATE;
        return false;
    }

    // Build transaction
    transaction_t txn = {0};
    if (fs->is_edit && fs->txn)
        txn.id = fs->txn->id;
    txn.amount_cents = cents;
    txn.type = fs->txn_type;
    if (fs->account_count > 0)
        txn.account_id = fs->accounts[fs->account_sel].id;
    if (fs->txn_type != TRANSACTION_TRANSFER && fs->category_count > 0)
        txn.category_id = fs->categories[fs->category_sel].id;
    if (fs->txn_type == TRANSACTION_TRANSFER) {
        if (fs->account_count < 2) {
            snprintf(fs->error, sizeof(fs->error), "Need at least 2 accounts");
            fs->current_field = FIELD_ACCOUNT;
            return false;
        }
        int64_t to_account_id = fs->accounts[fs->transfer_account_sel].id;
        if (txn.account_id == to_account_id) {
            snprintf(fs->error, sizeof(fs->error), "From/To account must differ");
            fs->current_field = FIELD_CATEGORY;
            return false;
        }
    }
    snprintf(txn.date, sizeof(txn.date), "%s", fs->date);
    snprintf(txn.reflection_date, sizeof(txn.reflection_date), "%s",
             fs->reflection_date);
    if (fs->txn_type == TRANSACTION_TRANSFER)
        txn.payee[0] = '\0';
    else
        snprintf(txn.payee, sizeof(txn.payee), "%s", fs->payee);
    snprintf(txn.description, sizeof(txn.description), "%s", fs->desc);

    if (fs->is_edit) {
        if (fs->txn_type == TRANSACTION_TRANSFER) {
            int64_t to_account_id = fs->accounts[fs->transfer_account_sel].id;
            int rc = db_update_transfer(fs->db, &txn, to_account_id);
            if (rc == -2) {
                snprintf(fs->error, sizeof(fs->error), "Transaction not found");
                return false;
            }
            if (rc == -3) {
                snprintf(fs->error, sizeof(fs->error), "From/To account must differ");
                fs->current_field = FIELD_CATEGORY;
                return false;
            }
            if (rc < 0) {
                snprintf(fs->error, sizeof(fs->error), "Database error");
                return false;
            }
            txn.transfer_id = (fs->transfer_id > 0) ? fs->transfer_id : txn.id;
        } else {
            txn.transfer_id = 0;
            int rc = db_update_transaction(fs->db, &txn);
            if (rc == -2) {
                snprintf(fs->error, sizeof(fs->error), "Transaction not found");
                return false;
            }
            if (rc < 0) {
                snprintf(fs->error, sizeof(fs->error), "Database error");
                return false;
            }
        }
    } else {
        int64_t row_id = -1;
        if (fs->txn_type == TRANSACTION_TRANSFER) {
            int64_t to_account_id = fs->accounts[fs->transfer_account_sel].id;
            row_id = db_insert_transfer(fs->db, &txn, to_account_id);
        } else {
            row_id = db_insert_transaction(fs->db, &txn);
        }
        if (row_id < 0) {
            snprintf(fs->error, sizeof(fs->error), "Database error");
            return false;
        }
        txn.id = row_id;
        if (fs->txn_type == TRANSACTION_TRANSFER)
            txn.transfer_id = row_id;
    }

    if (fs->txn)
        *fs->txn = txn;

    // Editing an uncategorized non-transfer transaction is the manual
    // categorization workflow where propagation is useful.
    if (fs->is_edit && prior_category_id <= 0 && txn.category_id > 0 &&
        txn.type != TRANSACTION_TRANSFER && txn.payee[0] != '\0') {
        fs->offer_category_propagation = true;
        fs->offer_type = txn.type;
        fs->offer_category_id = txn.category_id;
        snprintf(fs->offer_payee, sizeof(fs->offer_payee), "%s", txn.payee);
    }

    return true;
}

static void handle_text_input(char *buf, int *pos, int maxlen, int ch,
                              bool digits_only) {
    int len = (int)strlen(buf);

    if (ch == KEY_LEFT) {
        if (*pos > 0)
            (*pos)--;
    } else if (ch == KEY_RIGHT) {
        if (*pos < len)
            (*pos)++;
    } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
        if (*pos > 0) {
            memmove(&buf[*pos - 1], &buf[*pos], len - *pos + 1);
            (*pos)--;
        }
    } else if (isprint(ch) && len < maxlen - 1) {
        if (digits_only) {
            if (ch == '.') {
                // Only allow one decimal point
                if (strchr(buf, '.'))
                    return;
            } else if (!isdigit(ch)) {
                return;
            }
        }
        memmove(&buf[*pos + 1], &buf[*pos], len - *pos + 1);
        buf[*pos] = (char)ch;
        (*pos)++;
    }
}

static void handle_date_input(char *buf, int *pos, int ch) {
    int len = (int)strlen(buf);

    if (ch == KEY_LEFT) {
        if (*pos > 0)
            (*pos)--;
    } else if (ch == KEY_RIGHT) {
        if (*pos < len)
            (*pos)++;
    } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
        if (*pos > 0) {
            memmove(&buf[*pos - 1], &buf[*pos], len - *pos + 1);
            (*pos)--;
        }
    } else if ((isdigit(ch) || ch == '-') && len < 10) {
        memmove(&buf[*pos + 1], &buf[*pos], len - *pos + 1);
        buf[*pos] = (char)ch;
        (*pos)++;
    }
}

#define ACCOUNT_FORM_WIDTH 56
#define ACCOUNT_FORM_HEIGHT 13

enum {
    ACCOUNT_FIELD_NAME,
    ACCOUNT_FIELD_TYPE,
    ACCOUNT_FIELD_CARD,
    ACCOUNT_FIELD_SUBMIT,
    ACCOUNT_FIELD_COUNT
};

typedef struct {
    WINDOW *win;
    sqlite3 *db;
    account_t *account;
    bool is_edit;

    int current_field;
    char name[64];
    int name_pos;
    account_type_t type;
    char card_last4[5];
    int card_last4_pos;
    char error[64];
} account_form_state_t;

static const char *account_type_labels[] = {"Cash",           "Checking",
                                            "Savings",        "Credit Card",
                                            "Physical Asset", "Investment"};

static account_type_t next_account_type(account_type_t type) {
    return (account_type_t)((type + 1) % ACCOUNT_TYPE_COUNT);
}

static account_type_t prev_account_type(account_type_t type) {
    return (account_type_t)((type + ACCOUNT_TYPE_COUNT - 1) % ACCOUNT_TYPE_COUNT);
}

static int account_field_row(int field) { return 2 + field * 2; }

static void account_form_clamp_field(account_form_state_t *fs) {
    if (fs->type != ACCOUNT_CREDIT_CARD && fs->current_field == ACCOUNT_FIELD_CARD)
        fs->current_field = ACCOUNT_FIELD_SUBMIT;
}

static void account_form_next_field(account_form_state_t *fs) {
    if (fs->current_field >= ACCOUNT_FIELD_COUNT - 1)
        return;
    fs->current_field++;
    account_form_clamp_field(fs);
}

static void account_form_prev_field(account_form_state_t *fs) {
    if (fs->current_field <= 0)
        return;
    fs->current_field--;
    if (fs->type != ACCOUNT_CREDIT_CARD &&
        fs->current_field == ACCOUNT_FIELD_CARD) {
        fs->current_field = ACCOUNT_FIELD_TYPE;
    }
}

static void account_form_init(account_form_state_t *fs, sqlite3 *db,
                              account_t *account, bool is_edit) {
    memset(fs, 0, sizeof(*fs));
    fs->db = db;
    fs->account = account;
    fs->is_edit = is_edit;
    fs->type = ACCOUNT_CASH;
    fs->current_field = ACCOUNT_FIELD_NAME;

    if (is_edit && account) {
        snprintf(fs->name, sizeof(fs->name), "%s", account->name);
        fs->name_pos = (int)strlen(fs->name);
        fs->type = account->type;
        snprintf(fs->card_last4, sizeof(fs->card_last4), "%s",
                 account->card_last4);
        fs->card_last4_pos = (int)strlen(fs->card_last4);
    }
}

static void account_form_draw(account_form_state_t *fs) {
    WINDOW *w = fs->win;
    werase(w);
    wbkgd(w, COLOR_PAIR(COLOR_FORM));
    box(w, 0, 0);

    const char *title = fs->is_edit ? " Edit Account " : " Add Account ";
    int tw = (int)strlen(title);
    int ww = getmaxx(w);
    mvwprintw(w, 0, (ww - tw) / 2, "%s", title);

    mvwprintw(w, account_field_row(ACCOUNT_FIELD_NAME), LABEL_COL, "Name:");
    mvwprintw(w, account_field_row(ACCOUNT_FIELD_TYPE), LABEL_COL, "Type:");
    mvwprintw(w, account_field_row(ACCOUNT_FIELD_CARD), LABEL_COL, "Card last 4:");

    bool name_active = (fs->current_field == ACCOUNT_FIELD_NAME);
    if (name_active)
        wattron(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
    mvwprintw(w, account_field_row(ACCOUNT_FIELD_NAME), FIELD_COL, "%-*s",
              FIELD_WIDTH, "");
    mvwprintw(w, account_field_row(ACCOUNT_FIELD_NAME), FIELD_COL, "%s", fs->name);
    if (name_active)
        wattroff(w, COLOR_PAIR(COLOR_FORM_ACTIVE));

    bool type_active = (fs->current_field == ACCOUNT_FIELD_TYPE);
    if (type_active)
        wattron(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
    mvwprintw(w, account_field_row(ACCOUNT_FIELD_TYPE), FIELD_COL, "%-*s",
              FIELD_WIDTH, "");
    mvwprintw(w, account_field_row(ACCOUNT_FIELD_TYPE), FIELD_COL, "< %-16s >",
              account_type_labels[fs->type]);
    if (type_active)
        wattroff(w, COLOR_PAIR(COLOR_FORM_ACTIVE));

    bool card_active =
        (fs->current_field == ACCOUNT_FIELD_CARD && fs->type == ACCOUNT_CREDIT_CARD);
    if (card_active)
        wattron(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
    mvwprintw(w, account_field_row(ACCOUNT_FIELD_CARD), FIELD_COL, "%-*s",
              FIELD_WIDTH, "");
    if (fs->type == ACCOUNT_CREDIT_CARD) {
        mvwprintw(w, account_field_row(ACCOUNT_FIELD_CARD), FIELD_COL, "%-4s",
                  fs->card_last4);
    } else {
        wattron(w, A_DIM);
        mvwprintw(w, account_field_row(ACCOUNT_FIELD_CARD), FIELD_COL, "(n/a)");
        wattroff(w, A_DIM);
    }
    if (card_active)
        wattroff(w, COLOR_PAIR(COLOR_FORM_ACTIVE));

    const char *btn = "[ Submit ]";
    int btn_len = (int)strlen(btn);
    bool submit_active = (fs->current_field == ACCOUNT_FIELD_SUBMIT);
    if (submit_active)
        wattron(w, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);
    mvwprintw(w, account_field_row(ACCOUNT_FIELD_SUBMIT), (ww - btn_len) / 2,
              "%s", btn);
    if (submit_active)
        wattroff(w, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);

    if (fs->error[0] != '\0') {
        wattron(w, A_BOLD);
        mvwprintw(w, ACCOUNT_FORM_HEIGHT - 2, LABEL_COL, "%s", fs->error);
        wattroff(w, A_BOLD);
    }

    mvwprintw(w, ACCOUNT_FORM_HEIGHT - 1, 2, " C-s:Save  Esc:Cancel ");

    if (fs->current_field == ACCOUNT_FIELD_SUBMIT) {
        curs_set(0);
    } else {
        curs_set(1);
        if (fs->current_field == ACCOUNT_FIELD_NAME) {
            wmove(w, account_field_row(ACCOUNT_FIELD_NAME), FIELD_COL + fs->name_pos);
        } else if (fs->current_field == ACCOUNT_FIELD_TYPE) {
            wmove(w, account_field_row(ACCOUNT_FIELD_TYPE), FIELD_COL);
        } else if (fs->current_field == ACCOUNT_FIELD_CARD &&
                   fs->type == ACCOUNT_CREDIT_CARD) {
            wmove(w, account_field_row(ACCOUNT_FIELD_CARD),
                  FIELD_COL + fs->card_last4_pos);
        } else {
            wmove(w, account_field_row(ACCOUNT_FIELD_CARD), FIELD_COL);
        }
    }

    wrefresh(w);
}

static void account_handle_card_input(account_form_state_t *fs, int ch) {
    int len = (int)strlen(fs->card_last4);

    if (ch == KEY_LEFT) {
        if (fs->card_last4_pos > 0)
            fs->card_last4_pos--;
    } else if (ch == KEY_RIGHT) {
        if (fs->card_last4_pos < len)
            fs->card_last4_pos++;
    } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
        if (fs->card_last4_pos > 0) {
            memmove(&fs->card_last4[fs->card_last4_pos - 1],
                    &fs->card_last4[fs->card_last4_pos],
                    len - fs->card_last4_pos + 1);
            fs->card_last4_pos--;
        }
    } else if (isdigit(ch) && len < 4) {
        memmove(&fs->card_last4[fs->card_last4_pos + 1],
                &fs->card_last4[fs->card_last4_pos],
                len - fs->card_last4_pos + 1);
        fs->card_last4[fs->card_last4_pos] = (char)ch;
        fs->card_last4_pos++;
    }
}

static bool account_form_validate_and_save(account_form_state_t *fs) {
    fs->error[0] = '\0';

    if (fs->name[0] == '\0') {
        snprintf(fs->error, sizeof(fs->error), "Name cannot be empty");
        fs->current_field = ACCOUNT_FIELD_NAME;
        return false;
    }

    account_t updated = {0};
    if (fs->is_edit && fs->account)
        updated.id = fs->account->id;
    updated.type = fs->type;
    snprintf(updated.name, sizeof(updated.name), "%s", fs->name);
    if (updated.type == ACCOUNT_CREDIT_CARD) {
        snprintf(updated.card_last4, sizeof(updated.card_last4), "%s",
                 fs->card_last4);
    }

    if (fs->is_edit) {
        int rc = db_update_account(fs->db, &updated);
        if (rc == -2) {
            snprintf(fs->error, sizeof(fs->error), "Name already exists");
            return false;
        }
        if (rc < 0) {
            snprintf(fs->error, sizeof(fs->error), "Database error");
            return false;
        }
    } else {
        int64_t id = db_insert_account(fs->db, updated.name, updated.type,
                                       updated.card_last4);
        if (id == -2) {
            snprintf(fs->error, sizeof(fs->error), "Name already exists");
            return false;
        }
        if (id < 0) {
            snprintf(fs->error, sizeof(fs->error), "Database error");
            return false;
        }
        updated.id = id;
    }

    if (fs->account)
        *fs->account = updated;
    return true;
}

form_result_t form_account(WINDOW *parent, sqlite3 *db, account_t *account,
                           bool is_edit) {
    int ph, pw;
    getmaxyx(parent, ph, pw);
    if (ph < ACCOUNT_FORM_HEIGHT || pw < ACCOUNT_FORM_WIDTH)
        return FORM_CANCELLED;

    account_form_state_t fs;
    account_form_init(&fs, db, account, is_edit);

    int start_y, start_x;
    getbegyx(parent, start_y, start_x);
    int form_y = start_y + (ph - ACCOUNT_FORM_HEIGHT) / 2;
    int form_x = start_x + (pw - ACCOUNT_FORM_WIDTH) / 2;
    fs.win = newwin(ACCOUNT_FORM_HEIGHT, ACCOUNT_FORM_WIDTH, form_y, form_x);
    keypad(fs.win, TRUE);
    curs_set(1);

    form_result_t result = FORM_CANCELLED;
    bool done = false;
    while (!done) {
        account_form_draw(&fs);
        int ch = wgetch(fs.win);
        fs.error[0] = '\0';

        switch (ch) {
        case 27:
            done = true;
            break;
        case 19: // Ctrl+S
            if (account_form_validate_and_save(&fs)) {
                result = FORM_SAVED;
                done = true;
            }
            break;
        case '\t':
        case KEY_DOWN:
            account_form_next_field(&fs);
            break;
        case KEY_BTAB:
        case KEY_UP:
            account_form_prev_field(&fs);
            break;
        case '\n':
        case ' ':
            if (fs.current_field == ACCOUNT_FIELD_SUBMIT) {
                if (account_form_validate_and_save(&fs)) {
                    result = FORM_SAVED;
                    done = true;
                }
            } else if (fs.current_field == ACCOUNT_FIELD_TYPE) {
                fs.type = next_account_type(fs.type);
                if (fs.type != ACCOUNT_CREDIT_CARD) {
                    fs.card_last4[0] = '\0';
                    fs.card_last4_pos = 0;
                    account_form_clamp_field(&fs);
                }
            } else if (fs.current_field == ACCOUNT_FIELD_NAME) {
                handle_text_input(fs.name, &fs.name_pos, (int)sizeof(fs.name), ch,
                                  false);
            } else if (fs.current_field == ACCOUNT_FIELD_CARD &&
                       fs.type == ACCOUNT_CREDIT_CARD) {
                account_handle_card_input(&fs, ch);
            }
            break;
        case KEY_LEFT:
            if (fs.current_field == ACCOUNT_FIELD_TYPE) {
                fs.type = prev_account_type(fs.type);
                if (fs.type != ACCOUNT_CREDIT_CARD) {
                    fs.card_last4[0] = '\0';
                    fs.card_last4_pos = 0;
                    account_form_clamp_field(&fs);
                }
            } else if (fs.current_field == ACCOUNT_FIELD_NAME) {
                handle_text_input(fs.name, &fs.name_pos, (int)sizeof(fs.name), ch,
                                  false);
            } else if (fs.current_field == ACCOUNT_FIELD_CARD &&
                       fs.type == ACCOUNT_CREDIT_CARD) {
                account_handle_card_input(&fs, ch);
            }
            break;
        case KEY_RIGHT:
            if (fs.current_field == ACCOUNT_FIELD_TYPE) {
                fs.type = next_account_type(fs.type);
                if (fs.type != ACCOUNT_CREDIT_CARD) {
                    fs.card_last4[0] = '\0';
                    fs.card_last4_pos = 0;
                    account_form_clamp_field(&fs);
                }
            } else if (fs.current_field == ACCOUNT_FIELD_NAME) {
                handle_text_input(fs.name, &fs.name_pos, (int)sizeof(fs.name), ch,
                                  false);
            } else if (fs.current_field == ACCOUNT_FIELD_CARD &&
                       fs.type == ACCOUNT_CREDIT_CARD) {
                account_handle_card_input(&fs, ch);
            }
            break;
        case KEY_RESIZE:
            (void)ui_requeue_resize_event(ch);
            done = true;
            break;
        default:
            if (fs.current_field == ACCOUNT_FIELD_NAME) {
                handle_text_input(fs.name, &fs.name_pos, (int)sizeof(fs.name), ch,
                                  false);
            } else if (fs.current_field == ACCOUNT_FIELD_CARD &&
                       fs.type == ACCOUNT_CREDIT_CARD) {
                account_handle_card_input(&fs, ch);
            }
            break;
        }
    }

    curs_set(0);
    delwin(fs.win);
    return result;
}

#define CATEGORY_EDIT_FORM_WIDTH 56
#define CATEGORY_EDIT_FORM_HEIGHT 11

enum {
    CATEGORY_FIELD_NAME,
    CATEGORY_FIELD_TYPE,
    CATEGORY_FIELD_SUBMIT,
    CATEGORY_FIELD_COUNT
};

typedef struct {
    WINDOW *win;
    sqlite3 *db;
    category_t *category;
    bool is_edit;

    int current_field;
    char path[128];
    int path_pos;
    category_type_t type;
    char error[80];
} category_form_state_t;

static const char *category_type_labels[] = {"Expense", "Income"};

static category_type_t next_category_type(category_type_t type) {
    return (type == CATEGORY_INCOME) ? CATEGORY_EXPENSE : CATEGORY_INCOME;
}

static category_type_t prev_category_type(category_type_t type) {
    return next_category_type(type);
}

static int category_field_row(int field) { return 2 + field * 2; }

static void category_edit_form_next_field(category_form_state_t *fs) {
    if (fs->current_field < CATEGORY_FIELD_COUNT - 1)
        fs->current_field++;
}

static void category_edit_form_prev_field(category_form_state_t *fs) {
    if (fs->current_field > 0)
        fs->current_field--;
}

static void category_edit_form_init(category_form_state_t *fs, sqlite3 *db,
                                    category_t *category, bool is_edit) {
    memset(fs, 0, sizeof(*fs));
    fs->db = db;
    fs->category = category;
    fs->is_edit = is_edit;
    fs->current_field = CATEGORY_FIELD_NAME;
    fs->type = CATEGORY_EXPENSE;

    if (is_edit && category) {
        snprintf(fs->path, sizeof(fs->path), "%s", category->name);
        fs->path_pos = (int)strlen(fs->path);
        fs->type = category->type;
    }
}

static void category_edit_form_draw(category_form_state_t *fs) {
    WINDOW *w = fs->win;
    werase(w);
    wbkgd(w, COLOR_PAIR(COLOR_FORM));
    box(w, 0, 0);

    const char *title = fs->is_edit ? " Edit Category " : " Add Category ";
    int tw = (int)strlen(title);
    int ww = getmaxx(w);
    mvwprintw(w, 0, (ww - tw) / 2, "%s", title);

    mvwprintw(w, category_field_row(CATEGORY_FIELD_NAME), LABEL_COL, "Name:");
    mvwprintw(w, category_field_row(CATEGORY_FIELD_TYPE), LABEL_COL, "Type:");

    bool path_active = (fs->current_field == CATEGORY_FIELD_NAME);
    if (path_active)
        wattron(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
    mvwprintw(w, category_field_row(CATEGORY_FIELD_NAME), FIELD_COL, "%-*s",
              FIELD_WIDTH, "");
    mvwprintw(w, category_field_row(CATEGORY_FIELD_NAME), FIELD_COL, "%s",
              fs->path);
    if (path_active)
        wattroff(w, COLOR_PAIR(COLOR_FORM_ACTIVE));

    bool type_active = (fs->current_field == CATEGORY_FIELD_TYPE);
    if (type_active)
        wattron(w, COLOR_PAIR(COLOR_FORM_ACTIVE));
    mvwprintw(w, category_field_row(CATEGORY_FIELD_TYPE), FIELD_COL, "%-*s",
              FIELD_WIDTH, "");
    mvwprintw(w, category_field_row(CATEGORY_FIELD_TYPE), FIELD_COL, "< %-8s >",
              category_type_labels[fs->type]);
    if (type_active)
        wattroff(w, COLOR_PAIR(COLOR_FORM_ACTIVE));

    const char *btn = "[ Submit ]";
    int btn_len = (int)strlen(btn);
    bool submit_active = (fs->current_field == CATEGORY_FIELD_SUBMIT);
    if (submit_active)
        wattron(w, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);
    mvwprintw(w, category_field_row(CATEGORY_FIELD_SUBMIT), (ww - btn_len) / 2,
              "%s", btn);
    if (submit_active)
        wattroff(w, COLOR_PAIR(COLOR_FORM_ACTIVE) | A_BOLD);

    mvwprintw(w, CATEGORY_EDIT_FORM_HEIGHT - 2, LABEL_COL,
              "Use Parent:Child for sub-categories");
    if (fs->error[0] != '\0') {
        wattron(w, A_BOLD);
        mvwprintw(w, 1, LABEL_COL, "%s", fs->error);
        wattroff(w, A_BOLD);
    }

    mvwprintw(w, CATEGORY_EDIT_FORM_HEIGHT - 1, 2, " C-s:Save  Esc:Cancel ");

    if (fs->current_field == CATEGORY_FIELD_SUBMIT) {
        curs_set(0);
    } else {
        curs_set(1);
        if (fs->current_field == CATEGORY_FIELD_NAME)
            wmove(w, category_field_row(CATEGORY_FIELD_NAME),
                  FIELD_COL + fs->path_pos);
        else
            wmove(w, category_field_row(CATEGORY_FIELD_TYPE), FIELD_COL);
    }

    wrefresh(w);
}

static bool category_edit_form_validate_and_save(category_form_state_t *fs) {
    fs->error[0] = '\0';

    char parent_name[64];
    char child_name[64];
    bool has_parent = false;
    if (!parse_category_path(fs->path, parent_name, sizeof(parent_name), child_name,
                             sizeof(child_name), &has_parent)) {
        snprintf(fs->error, sizeof(fs->error), "Invalid category path");
        fs->current_field = CATEGORY_FIELD_NAME;
        return false;
    }

    int64_t parent_id = 0;
    if (has_parent) {
        parent_id = db_get_or_create_category(fs->db, fs->type, parent_name, 0);
        if (parent_id <= 0) {
            snprintf(fs->error, sizeof(fs->error), "Database error");
            return false;
        }
    }

    if (fs->is_edit) {
        category_t updated = {0};
        if (fs->category)
            updated.id = fs->category->id;
        updated.type = fs->type;
        updated.parent_id = parent_id;
        snprintf(updated.name, sizeof(updated.name), "%s", child_name);
        int rc = db_update_category(fs->db, &updated);
        if (rc == -2) {
            snprintf(fs->error, sizeof(fs->error), "Category already exists");
            return false;
        }
        if (rc < 0) {
            snprintf(fs->error, sizeof(fs->error), "Database error");
            return false;
        }
        if (fs->category) {
            *fs->category = updated;
            snprintf(fs->category->name, sizeof(fs->category->name), "%s",
                     child_name);
        }
    } else {
        int64_t id =
            db_get_or_create_category(fs->db, fs->type, child_name, parent_id);
        if (id <= 0) {
            snprintf(fs->error, sizeof(fs->error), "Database error");
            return false;
        }
        if (fs->category) {
            fs->category->id = id;
            fs->category->type = fs->type;
            fs->category->parent_id = parent_id;
            snprintf(fs->category->name, sizeof(fs->category->name), "%s",
                     child_name);
        }
    }

    return true;
}

form_result_t form_category(WINDOW *parent, sqlite3 *db, category_t *category,
                            bool is_edit) {
    int ph, pw;
    getmaxyx(parent, ph, pw);
    if (ph < CATEGORY_EDIT_FORM_HEIGHT || pw < CATEGORY_EDIT_FORM_WIDTH)
        return FORM_CANCELLED;

    category_form_state_t fs;
    category_edit_form_init(&fs, db, category, is_edit);

    int start_y, start_x;
    getbegyx(parent, start_y, start_x);
    int form_y = start_y + (ph - CATEGORY_EDIT_FORM_HEIGHT) / 2;
    int form_x = start_x + (pw - CATEGORY_EDIT_FORM_WIDTH) / 2;
    fs.win = newwin(CATEGORY_EDIT_FORM_HEIGHT, CATEGORY_EDIT_FORM_WIDTH, form_y,
                    form_x);
    keypad(fs.win, TRUE);
    curs_set(1);

    form_result_t result = FORM_CANCELLED;
    bool done = false;
    while (!done) {
        category_edit_form_draw(&fs);
        int ch = wgetch(fs.win);
        fs.error[0] = '\0';

        switch (ch) {
        case 27:
        case KEY_EXIT:
            done = true;
            break;
        case 19: // Ctrl+S
            if (category_edit_form_validate_and_save(&fs)) {
                result = FORM_SAVED;
                done = true;
            }
            break;
        case '\t':
        case KEY_DOWN:
            category_edit_form_next_field(&fs);
            break;
        case KEY_BTAB:
        case KEY_UP:
            category_edit_form_prev_field(&fs);
            break;
        case '\n':
        case ' ':
            if (fs.current_field == CATEGORY_FIELD_SUBMIT) {
                if (category_edit_form_validate_and_save(&fs)) {
                    result = FORM_SAVED;
                    done = true;
                }
            } else if (fs.current_field == CATEGORY_FIELD_TYPE) {
                fs.type = next_category_type(fs.type);
            } else if (fs.current_field == CATEGORY_FIELD_NAME) {
                handle_text_input(fs.path, &fs.path_pos, (int)sizeof(fs.path), ch,
                                  false);
            }
            break;
        case KEY_LEFT:
            if (fs.current_field == CATEGORY_FIELD_TYPE) {
                fs.type = prev_category_type(fs.type);
            } else if (fs.current_field == CATEGORY_FIELD_NAME) {
                handle_text_input(fs.path, &fs.path_pos, (int)sizeof(fs.path), ch,
                                  false);
            }
            break;
        case KEY_RIGHT:
            if (fs.current_field == CATEGORY_FIELD_TYPE) {
                fs.type = next_category_type(fs.type);
            } else if (fs.current_field == CATEGORY_FIELD_NAME) {
                handle_text_input(fs.path, &fs.path_pos, (int)sizeof(fs.path), ch,
                                  false);
            }
            break;
        case KEY_RESIZE:
            (void)ui_requeue_resize_event(ch);
            done = true;
            break;
        default:
            if (fs.current_field == CATEGORY_FIELD_NAME) {
                handle_text_input(fs.path, &fs.path_pos, (int)sizeof(fs.path), ch,
                                  false);
            }
            break;
        }
    }

    curs_set(0);
    delwin(fs.win);
    return result;
}

form_result_t form_transaction(WINDOW *parent, sqlite3 *db, transaction_t *txn,
                               bool is_edit) {
    int ph, pw;
    getmaxyx(parent, ph, pw);

    // Check minimum size
    if (ph < FORM_HEIGHT || pw < FORM_WIDTH) {
        return FORM_CANCELLED;
    }

    form_state_t fs;
    form_init_state(&fs, db, txn, is_edit);

    // Center the form over the parent content window
    int start_y, start_x;
    getbegyx(parent, start_y, start_x);
    int form_y = start_y + (ph - FORM_HEIGHT) / 2;
    int form_x = start_x + (pw - FORM_WIDTH) / 2;

    fs.win = newwin(FORM_HEIGHT, FORM_WIDTH, form_y, form_x);
    keypad(fs.win, TRUE);
    curs_set(1);

    form_result_t result = FORM_CANCELLED;
    bool done = false;

    while (!done) {
        form_draw(&fs);
        if (fs.dropdown_open)
            form_draw_dropdown(&fs);
        doupdate();

        int ch = wgetch(fs.win);
        fs.error[0] = '\0';

        if (fs.dropdown_open) {
            if (ui_requeue_resize_event(ch)) {
                done = true;
                continue;
            }
            int count = form_dropdown_count(&fs);
            if (form_dropdown_handle_filter_key(&fs, ch))
                continue;
            switch (ch) {
            case KEY_UP:
                if (fs.dropdown_sel > 0)
                    fs.dropdown_sel--;
                break;
            case KEY_DOWN:
                if (fs.dropdown_sel < count - 1)
                    fs.dropdown_sel++;
                break;
            case '\n':
                if (fs.current_field == FIELD_CATEGORY &&
                    fs.txn_type != TRANSACTION_TRANSFER &&
                    fs.dropdown_sel == fs.category_count) {
                    form_close_dropdown(&fs, false);
                    form_create_category_on_the_fly(parent, &fs);
                } else {
                    form_close_dropdown(&fs, true);
                }
                break;
            case 27: // Escape
            case KEY_EXIT:
                form_close_dropdown(&fs, false);
                break;
            }
            continue;
        }

        switch (ch) {
        case 27: // Escape
            done = true;
            break;

        case 19: // Ctrl+S
            if (form_validate_and_save(&fs)) {
                maybe_propagate_category_to_payee(parent, &fs);
                result = FORM_SAVED;
                done = true;
            }
            break;

        case '\t':
        case KEY_DOWN:
            move_to_next_field(&fs);
            break;

        case KEY_BTAB:
        case KEY_UP:
            move_to_prev_field(&fs);
            break;

        case '\n':
            if (fs.current_field == FIELD_SUBMIT) {
                if (form_validate_and_save(&fs)) {
                    maybe_propagate_category_to_payee(parent, &fs);
                    result = FORM_SAVED;
                    done = true;
                }
            } else if (fs.current_field == FIELD_ACCOUNT ||
                       fs.current_field == FIELD_CATEGORY) {
                form_open_dropdown(&fs);
            }
            break;

        case 'n':
        case 'N':
            if (fs.current_field == FIELD_CATEGORY &&
                fs.txn_type != TRANSACTION_TRANSFER) {
                form_create_category_on_the_fly(parent, &fs);
            } else if (fs.current_field == FIELD_PAYEE) {
                handle_text_input(fs.payee, &fs.payee_pos, (int)sizeof(fs.payee),
                                  ch, false);
            } else if (fs.current_field == FIELD_DESC) {
                handle_text_input(fs.desc, &fs.desc_pos, (int)sizeof(fs.desc),
                                  ch, false);
            }
            break;

        case ' ':
            if (fs.current_field == FIELD_SUBMIT) {
                if (form_validate_and_save(&fs)) {
                    maybe_propagate_category_to_payee(parent, &fs);
                    result = FORM_SAVED;
                    done = true;
                }
            } else if (fs.current_field == FIELD_TYPE) {
                fs.txn_type = next_type(fs.txn_type);
                form_load_categories(&fs);
            } else if (fs.current_field == FIELD_ACCOUNT ||
                       fs.current_field == FIELD_CATEGORY) {
                form_open_dropdown(&fs);
            } else if (fs.current_field == FIELD_PAYEE) {
                handle_text_input(fs.payee, &fs.payee_pos, (int)sizeof(fs.payee),
                                  ch, false);
            } else if (fs.current_field == FIELD_DESC) {
                handle_text_input(fs.desc, &fs.desc_pos, (int)sizeof(fs.desc),
                                  ch, false);
            }
            break;

        case KEY_LEFT:
            if (fs.current_field == FIELD_TYPE) {
                fs.txn_type = prev_type(fs.txn_type);
                form_load_categories(&fs);
                if (field_hidden(&fs, fs.current_field))
                    move_to_next_field(&fs);
            } else if (fs.current_field == FIELD_ACCOUNT) {
                fs.account_sel = next_account_index(
                    &fs, fs.account_sel, -1,
                    (fs.txn_type == TRANSACTION_TRANSFER) ? fs.transfer_account_sel : -1);
            } else if (fs.current_field == FIELD_CATEGORY &&
                       fs.txn_type == TRANSACTION_TRANSFER) {
                fs.transfer_account_sel =
                    next_account_index(&fs, fs.transfer_account_sel, -1, fs.account_sel);
            } else if (fs.current_field == FIELD_AMOUNT) {
                handle_text_input(fs.amount, &fs.amount_pos,
                                  (int)sizeof(fs.amount), ch, true);
            } else if (fs.current_field == FIELD_DATE) {
                handle_date_input(fs.date, &fs.date_pos, ch);
            } else if (fs.current_field == FIELD_REFLECTION_DATE) {
                handle_date_input(fs.reflection_date, &fs.reflection_date_pos,
                                  ch);
            } else if (fs.current_field == FIELD_PAYEE) {
                handle_text_input(fs.payee, &fs.payee_pos,
                                  (int)sizeof(fs.payee), ch, false);
            } else if (fs.current_field == FIELD_DESC) {
                handle_text_input(fs.desc, &fs.desc_pos, (int)sizeof(fs.desc),
                                  ch, false);
            }
            break;

        case KEY_RIGHT:
            if (fs.current_field == FIELD_TYPE) {
                fs.txn_type = next_type(fs.txn_type);
                form_load_categories(&fs);
                if (field_hidden(&fs, fs.current_field))
                    move_to_next_field(&fs);
            } else if (fs.current_field == FIELD_ACCOUNT) {
                fs.account_sel = next_account_index(
                    &fs, fs.account_sel, 1,
                    (fs.txn_type == TRANSACTION_TRANSFER) ? fs.transfer_account_sel : -1);
            } else if (fs.current_field == FIELD_CATEGORY &&
                       fs.txn_type == TRANSACTION_TRANSFER) {
                fs.transfer_account_sel =
                    next_account_index(&fs, fs.transfer_account_sel, 1, fs.account_sel);
            } else if (fs.current_field == FIELD_AMOUNT) {
                handle_text_input(fs.amount, &fs.amount_pos,
                                  (int)sizeof(fs.amount), ch, true);
            } else if (fs.current_field == FIELD_DATE) {
                handle_date_input(fs.date, &fs.date_pos, ch);
            } else if (fs.current_field == FIELD_REFLECTION_DATE) {
                handle_date_input(fs.reflection_date, &fs.reflection_date_pos,
                                  ch);
            } else if (fs.current_field == FIELD_PAYEE) {
                handle_text_input(fs.payee, &fs.payee_pos,
                                  (int)sizeof(fs.payee), ch, false);
            } else if (fs.current_field == FIELD_DESC) {
                handle_text_input(fs.desc, &fs.desc_pos, (int)sizeof(fs.desc),
                                  ch, false);
            }
            break;

        case KEY_RESIZE:
            (void)ui_requeue_resize_event(ch);
            done = true;
            break;

        default:
            // Text input for text fields
            if (fs.current_field == FIELD_AMOUNT) {
                handle_text_input(fs.amount, &fs.amount_pos,
                                  (int)sizeof(fs.amount), ch, true);
            } else if (fs.current_field == FIELD_DATE) {
                handle_date_input(fs.date, &fs.date_pos, ch);
            } else if (fs.current_field == FIELD_REFLECTION_DATE) {
                handle_date_input(fs.reflection_date, &fs.reflection_date_pos,
                                  ch);
            } else if (fs.current_field == FIELD_PAYEE) {
                handle_text_input(fs.payee, &fs.payee_pos,
                                  (int)sizeof(fs.payee), ch, false);
            } else if (fs.current_field == FIELD_DESC) {
                handle_text_input(fs.desc, &fs.desc_pos, (int)sizeof(fs.desc),
                                  ch, false);
            }
            break;
        }
    }

    curs_set(0);
    form_cleanup_state(&fs);
    return result;
}

form_result_t form_transaction_category(WINDOW *parent, sqlite3 *db,
                                        transaction_t *txn) {
    int ph, pw;
    getmaxyx(parent, ph, pw);
    if (ph < CATEGORY_FORM_HEIGHT || pw < CATEGORY_FORM_WIDTH || !txn)
        return FORM_CANCELLED;
    if (txn->type == TRANSACTION_TRANSFER)
        return FORM_CANCELLED;

    form_state_t fs;
    form_init_state(&fs, db, txn, true);
    fs.current_field = FIELD_CATEGORY;
    fs.error[0] = '\0';

    int start_y, start_x;
    getbegyx(parent, start_y, start_x);
    int form_y = start_y + (ph - CATEGORY_FORM_HEIGHT) / 2;
    int form_x = start_x + (pw - CATEGORY_FORM_WIDTH) / 2;

    fs.win = newwin(CATEGORY_FORM_HEIGHT, CATEGORY_FORM_WIDTH, form_y, form_x);
    keypad(fs.win, TRUE);
    curs_set(0);

    form_result_t result = FORM_CANCELLED;
    bool done = false;
    form_open_dropdown(&fs);

    while (!done) {
        category_form_draw(&fs);
        if (fs.dropdown_open)
            category_form_draw_dropdown(&fs);
        doupdate();

        int ch = wgetch(fs.win);
        fs.error[0] = '\0';

        if (fs.dropdown_open) {
            if (ui_requeue_resize_event(ch)) {
                done = true;
                continue;
            }
            int count = fs.category_count + 1;
            if (ch == KEY_UP || ch == 'k') {
                if (fs.dropdown_sel > 0)
                    fs.dropdown_sel--;
                continue;
            }
            if (ch == KEY_DOWN || ch == 'j') {
                if (fs.dropdown_sel < count - 1)
                    fs.dropdown_sel++;
                continue;
            }
            if (ch == '\n') {
                if (fs.dropdown_sel == fs.category_count) {
                    form_close_dropdown(&fs, false);
                    if (form_create_category_on_the_fly(parent, &fs)) {
                        if (form_save_category_only(parent, &fs)) {
                            result = FORM_SAVED;
                            done = true;
                        }
                    } else if (fs.error[0] == '\0') {
                        form_open_dropdown(&fs);
                    }
                } else {
                    fs.category_sel = fs.dropdown_sel;
                    if (form_save_category_only(parent, &fs)) {
                        result = FORM_SAVED;
                        done = true;
                    }
                }
                continue;
            }
            if (ch == 27 || ch == KEY_EXIT) {
                done = true;
                continue;
            }
            if (form_dropdown_handle_filter_key(&fs, ch))
                continue;
            continue;
        }

        switch (ch) {
        case 27:
        case KEY_EXIT:
            done = true;
            break;
        case 'n':
        case 'N':
            if (form_create_category_on_the_fly(parent, &fs)) {
                if (form_save_category_only(parent, &fs)) {
                    result = FORM_SAVED;
                    done = true;
                }
            }
            break;
        case '\n':
        case ' ':
            form_open_dropdown(&fs);
            break;
        case KEY_RESIZE:
            (void)ui_requeue_resize_event(ch);
            done = true;
            break;
        default:
            break;
        }
    }

    curs_set(0);
    form_cleanup_state(&fs);
    return result;
}
