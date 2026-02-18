#include "ui/txn_list.h"
#include "ui/colors.h"
#include "db/query.h"
#include "models/account.h"
#include "ui/form.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define GAP_WIDTH 3
#define DATE_COL_WIDTH 12
#define TYPE_COL_WIDTH 8
#define CATEGORY_COL_WIDTH 16
#define AMOUNT_COL_WIDTH 13
// Description column takes remaining width, but enforce a minimum for usability
#define DESC_COL_MIN_WIDTH 4

// Layout constants: rows consumed above data rows (inside box border)
// row 0: top border
// row 1: account tabs
// row 2: filter bar (shown when active or has text)
// row 3: column headers
// row 4: horizontal rule
// data starts at row 5
#define DATA_ROW_START 5

typedef enum {
    SORT_DATE,
    SORT_AMOUNT,
    SORT_CATEGORY,
    SORT_DESCRIPTION,
    SORT_TYPE,
    SORT_COUNT
} sort_col_t;

struct txn_list_state {
    sqlite3 *db;
    account_t *accounts;
    int account_count;
    int account_sel;

    txn_row_t *transactions;
    int txn_count;

    // Sort
    sort_col_t sort_col;
    bool sort_asc;

    // Filter
    bool filter_active;
    char filter_buf[128];
    int filter_len;

    // Derived display (sorted + filtered copy of transactions)
    txn_row_t *display;
    int display_count;

    int cursor;
    int scroll_offset;
    bool dirty;
};

static bool confirm_delete(WINDOW *parent) {
    int ph, pw;
    getmaxyx(parent, ph, pw);

    int win_h = 7;
    int win_w = 42;
    if (ph < win_h)
        win_h = ph;
    if (pw < win_w)
        win_w = pw;
    if (win_h < 4 || win_w < 20)
        return false;

    int py, px;
    getbegyx(parent, py, px);
    int win_y = py + (ph - win_h) / 2;
    int win_x = px + (pw - win_w) / 2;

    WINDOW *w = newwin(win_h, win_w, win_y, win_x);
    keypad(w, TRUE);
    wbkgd(w, COLOR_PAIR(COLOR_FORM));
    box(w, 0, 0);

    mvwprintw(w, 1, 2, "Delete transaction?");
    mvwprintw(w, win_h - 2, 2, "y:Delete  n:Cancel");
    wrefresh(w);

    bool confirm = false;
    bool done = false;
    while (!done) {
        int ch = wgetch(w);
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

static void format_amount(int64_t cents, transaction_type_t type, char *buf,
                          int buflen) {
    int64_t abs_cents = cents < 0 ? -cents : cents;
    int64_t whole = abs_cents / 100;
    int64_t frac = abs_cents % 100;

    // Add thousand separators
    char raw[32];
    snprintf(raw, sizeof(raw), "%ld", (long)whole);
    int rawlen = (int)strlen(raw);

    char formatted[48];
    int fi = 0;
    for (int i = 0; i < rawlen; i++) {
        if (i > 0 && (rawlen - i) % 3 == 0)
            formatted[fi++] = ',';
        formatted[fi++] = raw[i];
    }
    formatted[fi] = '\0';

    if (type == TRANSACTION_EXPENSE)
        snprintf(buf, buflen, "-%s.%02ld", formatted, (long)frac);
    else
        snprintf(buf, buflen, "%s.%02ld", formatted, (long)frac);
}

// Case-insensitive substring search (avoids _GNU_SOURCE dependency on
// strcasestr)
static bool contains_icase(const char *haystack, const char *needle) {
    if (!needle || needle[0] == '\0')
        return true;
    if (!haystack)
        return false;
    int nlen = (int)strlen(needle);
    int hlen = (int)strlen(haystack);
    for (int i = 0; i <= hlen - nlen; i++) {
        bool match = true;
        for (int j = 0; j < nlen; j++) {
            char h = haystack[i + j];
            char n = needle[j];
            if (h >= 'A' && h <= 'Z')
                h += 32;
            if (n >= 'A' && n <= 'Z')
                n += 32;
            if (h != n) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

// Returns true if transaction matches the filter (empty filter matches all)
static bool matches_filter(const txn_row_t *t, const char *filter) {
    if (!filter || filter[0] == '\0')
        return true;
    if (contains_icase(t->date, filter))
        return true;

    const char *type_str;
    switch (t->type) {
    case TRANSACTION_INCOME:
        type_str = "Income";
        break;
    case TRANSACTION_TRANSFER:
        type_str = "Transfer";
        break;
    default:
        type_str = "Expense";
        break;
    }
    if (contains_icase(type_str, filter))
        return true;
    if (contains_icase(t->category_name, filter))
        return true;
    if (contains_icase(t->description, filter))
        return true;

    char amt[24];
    format_amount(t->amount_cents, t->type, amt, sizeof(amt));
    if (contains_icase(amt, filter))
        return true;

    return false;
}

// Static context for qsort comparator (app is single-threaded)
static txn_list_state_t *g_sort_ctx;

static int compare_txn(const void *a, const void *b) {
    const txn_row_t *ta = (const txn_row_t *)a;
    const txn_row_t *tb = (const txn_row_t *)b;
    txn_list_state_t *ls = g_sort_ctx;

    int cmp = 0;
    switch (ls->sort_col) {
    case SORT_DATE:
        cmp = strcmp(ta->date, tb->date);
        break;
    case SORT_AMOUNT:
        if (ta->amount_cents < tb->amount_cents)
            cmp = -1;
        else if (ta->amount_cents > tb->amount_cents)
            cmp = 1;
        break;
    case SORT_CATEGORY:
        cmp = strcmp(ta->category_name, tb->category_name);
        break;
    case SORT_DESCRIPTION:
        cmp = strcmp(ta->description, tb->description);
        break;
    case SORT_TYPE:
        cmp = (int)ta->type - (int)tb->type;
        break;
    default:
        break;
    }

    return ls->sort_asc ? cmp : -cmp;
}

// Rebuild display array: filter transactions then sort
static void rebuild_display(txn_list_state_t *ls) {
    free(ls->display);
    ls->display = NULL;
    ls->display_count = 0;

    if (ls->txn_count == 0)
        return;

    txn_row_t *tmp = malloc(ls->txn_count * sizeof(txn_row_t));
    if (!tmp)
        return;

    int count = 0;
    for (int i = 0; i < ls->txn_count; i++) {
        if (matches_filter(&ls->transactions[i], ls->filter_buf))
            tmp[count++] = ls->transactions[i];
    }

    if (count > 1) {
        g_sort_ctx = ls;
        qsort(tmp, count, sizeof(txn_row_t), compare_txn);
    }

    ls->display = tmp;
    ls->display_count = count;

    // Clamp cursor into new range
    if (ls->cursor < 0)
        ls->cursor = 0;
    if (ls->cursor >= ls->display_count)
        ls->cursor = ls->display_count > 0 ? ls->display_count - 1 : 0;
}

static void reload(txn_list_state_t *ls) {
    free(ls->transactions);
    ls->transactions = NULL;
    ls->txn_count = 0;
    ls->cursor = 0;
    ls->scroll_offset = 0;

    // Reload accounts
    free(ls->accounts);
    ls->accounts = NULL;
    ls->account_count = db_get_accounts(ls->db, &ls->accounts);
    if (ls->account_count < 0)
        ls->account_count = 0;

    if (ls->account_sel >= ls->account_count)
        ls->account_sel = 0;

    if (ls->account_count > 0) {
        int64_t acct_id = ls->accounts[ls->account_sel].id;
        ls->txn_count = db_get_transactions(ls->db, acct_id, &ls->transactions);
        if (ls->txn_count < 0)
            ls->txn_count = 0;
    }

    ls->dirty = false;
    rebuild_display(ls);
}

txn_list_state_t *txn_list_create(sqlite3 *db) {
    txn_list_state_t *ls = calloc(1, sizeof(*ls));
    if (!ls)
        return NULL;
    ls->db = db;
    ls->sort_col = SORT_DATE;
    ls->sort_asc = false;
    ls->dirty = true;
    return ls;
}

void txn_list_destroy(txn_list_state_t *ls) {
    if (!ls)
        return;
    free(ls->accounts);
    free(ls->transactions);
    free(ls->display);
    free(ls);
}

void txn_list_draw(txn_list_state_t *ls, WINDOW *win, bool focused) {
    if (ls->dirty)
        reload(ls);

    int h, w;
    getmaxyx(win, h, w);

    // Show terminal cursor only when filter bar is active
    curs_set(ls->filter_active ? 1 : 0);

    // -- Account tabs (row 1 inside box) --
    int col = 2;
    for (int i = 0; i < ls->account_count && col < w - 2; i++) {
        char label[80];
        snprintf(label, sizeof(label), "%d:%s", i + 1, ls->accounts[i].name);
        int len = (int)strlen(label);

        if (i == ls->account_sel) {
            wattron(win, COLOR_PAIR(COLOR_SELECTED));
            mvwprintw(win, 1, col, "%s", label);
            wattroff(win, COLOR_PAIR(COLOR_SELECTED));
        } else {
            mvwprintw(win, 1, col, "%s", label);
        }
        col += len + 2;
    }

    // -- Filter bar (row 2): shown when active or text is present --
    if (ls->filter_active || ls->filter_len > 0) {
        if (ls->filter_active)
            wattron(win, A_BOLD);
        mvwprintw(win, 2, 2, "Filter: %s", ls->filter_buf);
        if (ls->filter_active)
            wattroff(win, A_BOLD);
    }

    // -- Column headers (row 3) with sort direction indicator --
    // Column layout: Date(12) gap(3) Type(8) gap(3) Category(16) gap(3)
    // Amount(13) gap(3) Desc(rest)
    int desc_w = w - 2 - DATE_COL_WIDTH - GAP_WIDTH - TYPE_COL_WIDTH -
                 GAP_WIDTH - CATEGORY_COL_WIDTH - GAP_WIDTH - AMOUNT_COL_WIDTH -
                 GAP_WIDTH - 2;
    if (desc_w < DESC_COL_MIN_WIDTH)
        desc_w = DESC_COL_MIN_WIDTH;

    // Print each header at its exact column to avoid UTF-8
    // byte-vs-display-width padding issues, then overlay the sort indicator
    // right after the active label text.
    const char *ind_str = ls->sort_asc ? "\u2191" : "\u2193";
    wattron(win, A_BOLD);
    mvwprintw(win, 3, 2, "%-*s", DATE_COL_WIDTH, "Date");
    mvwprintw(win, 3, 17, "%-*s", TYPE_COL_WIDTH, "Type");
    mvwprintw(win, 3, 28, "%-*s", CATEGORY_COL_WIDTH, "Category");
    mvwprintw(win, 3, 47, "%-*s", AMOUNT_COL_WIDTH, "Amount");
    mvwprintw(win, 3, 63, "%-*s", desc_w, "Description");
    switch (ls->sort_col) {
    case SORT_DATE:
        mvwprintw(win, 3, 6, "%s", ind_str);
        break;
    case SORT_TYPE:
        mvwprintw(win, 3, 21, "%s", ind_str);
        break;
    case SORT_CATEGORY:
        mvwprintw(win, 3, 36, "%s", ind_str);
        break;
    case SORT_AMOUNT:
        mvwprintw(win, 3, 53, "%s", ind_str);
        break;
    case SORT_DESCRIPTION:
        mvwprintw(win, 3, 71, "%s", ind_str);
        break;
    default:
        break;
    }
    wattroff(win, A_BOLD);

    // -- Horizontal rule (row 4) --
    wmove(win, 4, 2);
    for (int i = 2; i < w - 2; i++)
        waddch(win, ACS_HLINE);

    // -- Data rows --
    int visible_rows = h - 1 - DATA_ROW_START;
    if (visible_rows < 1)
        visible_rows = 1;

    if (ls->display_count == 0) {
        const char *msg =
            (ls->filter_len > 0) ? "No matches" : "No transactions";
        int mlen = (int)strlen(msg);
        int row = DATA_ROW_START + visible_rows / 2;
        if (row >= h - 1)
            row = DATA_ROW_START;
        mvwprintw(win, row, (w - mlen) / 2, "%s", msg);
        if (ls->filter_active)
            wmove(win, 2, 2 + 8 + ls->filter_len);
        return;
    }

    // Clamp cursor/scroll
    if (ls->cursor < 0)
        ls->cursor = 0;
    if (ls->cursor >= ls->display_count)
        ls->cursor = ls->display_count - 1;

    if (ls->cursor < ls->scroll_offset)
        ls->scroll_offset = ls->cursor;
    if (ls->cursor >= ls->scroll_offset + visible_rows)
        ls->scroll_offset = ls->cursor - visible_rows + 1;
    if (ls->scroll_offset < 0)
        ls->scroll_offset = 0;

    for (int i = 0; i < visible_rows; i++) {
        int idx = ls->scroll_offset + i;
        if (idx >= ls->display_count)
            break;

        txn_row_t *t = &ls->display[idx];
        int row = DATA_ROW_START + i;

        const char *type_str;
        switch (t->type) {
        case TRANSACTION_INCOME:
            type_str = "Income";
            break;
        case TRANSACTION_TRANSFER:
            type_str = "Transfer";
            break;
        default:
            type_str = "Expense";
            break;
        }

        char amt[24];
        format_amount(t->amount_cents, t->type, amt, sizeof(amt));

        // Truncate description to fit
        char desc_buf[256];
        snprintf(desc_buf, sizeof(desc_buf), "%-*.*s", desc_w, desc_w,
                 t->description);

        bool selected = (idx == ls->cursor);
        if (selected) {
            if (!focused)
                wattron(win, A_DIM);
            wattron(win, A_REVERSE);
        }

        // Print the base row (clears it)
        mvwprintw(win, row, 2, "%-12s   %-8s   %-16.16s", t->date, type_str,
                  t->category_name);

        // Amount with color
        int amount_col = 2 + 12 + 3 + 8 + 3 + 16 + 3;
        int color =
            (t->type == TRANSACTION_EXPENSE) ? COLOR_EXPENSE : COLOR_INCOME;
        wattron(win, COLOR_PAIR(color));
        mvwprintw(win, row, amount_col, "%13s", amt);
        wattroff(win, COLOR_PAIR(color));

        // Description
        mvwprintw(win, row, amount_col + 13 + 3, "%s", desc_buf);

        if (selected) {
            wattroff(win, A_REVERSE);
            if (!focused)
                wattroff(win, A_DIM);
        }
    }

    // Place terminal cursor in filter bar when active
    if (ls->filter_active)
        wmove(win, 2, 2 + 8 + ls->filter_len);
}

bool txn_list_handle_input(txn_list_state_t *ls, WINDOW *parent, int ch) {
    int visible_rows =
        20; // will be corrected by draw, but we need a sensible default

    // --- Filter mode: handle text input before anything else ---
    if (ls->filter_active) {
        if (ch == '\n') {
            ls->filter_active = false;
            return true;
        }
        if (ch == 27) { // ESC: clear filter and close bar
            ls->filter_buf[0] = '\0';
            ls->filter_len = 0;
            ls->filter_active = false;
            ls->cursor = 0;
            ls->scroll_offset = 0;
            rebuild_display(ls);
            return true;
        }
        if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
            if (ls->filter_len > 0) {
                ls->filter_len--;
                ls->filter_buf[ls->filter_len] = '\0';
                ls->cursor = 0;
                ls->scroll_offset = 0;
                rebuild_display(ls);
            }
            return true;
        }
        if (ch >= 32 && ch < 127) { // printable ASCII
            if (ls->filter_len < (int)sizeof(ls->filter_buf) - 1) {
                ls->filter_buf[ls->filter_len++] = (char)ch;
                ls->filter_buf[ls->filter_len] = '\0';
                ls->cursor = 0;
                ls->scroll_offset = 0;
                rebuild_display(ls);
            }
            return true;
        }
        return true; // swallow all other keys while filtering
    }

    // --- Normal mode ---
    switch (ch) {
    case KEY_UP:
    case 'k':
        if (ls->cursor > 0)
            ls->cursor--;
        return true;
    case KEY_DOWN:
    case 'j':
        if (ls->cursor < ls->display_count - 1)
            ls->cursor++;
        return true;
    case KEY_HOME:
    case 'g':
        ls->cursor = 0;
        return true;
    case KEY_END:
    case 'G':
        ls->cursor = ls->display_count > 0 ? ls->display_count - 1 : 0;
        return true;
    case KEY_PPAGE:
        ls->cursor -= visible_rows;
        if (ls->cursor < 0)
            ls->cursor = 0;
        return true;
    case KEY_NPAGE:
        ls->cursor += visible_rows;
        if (ls->cursor >= ls->display_count)
            ls->cursor = ls->display_count > 0 ? ls->display_count - 1 : 0;
        return true;
    case '/':
        ls->filter_active = true;
        return true;
    case 's':
        ls->sort_col = (sort_col_t)((ls->sort_col + 1) % SORT_COUNT);
        rebuild_display(ls);
        return true;
    case 'S':
        ls->sort_asc = !ls->sort_asc;
        rebuild_display(ls);
        return true;
    case 'e':
        if (ls->display_count <= 0)
            return true;
        {
            transaction_t txn = {0};
            int rc = db_get_transaction_by_id(
                ls->db, (int)ls->display[ls->cursor].id, &txn);
            if (rc == 0) {
                form_result_t res =
                    form_transaction(parent, ls->db, &txn, true);
                if (res == FORM_SAVED)
                    ls->dirty = true;
            } else {
                ls->dirty = true;
            }
        }
        return true;
    case 'd':
        if (ls->display_count <= 0)
            return true;
        if (confirm_delete(parent)) {
            int rc =
                db_delete_transaction(ls->db, (int)ls->display[ls->cursor].id);
            if (rc == 0) {
                if (ls->cursor > 0)
                    ls->cursor--;
                ls->dirty = true;
            } else if (rc == -2) {
                ls->dirty = true;
            }
        }
        return true;
    default:
        // Account switching: '1'-'9'
        if (ch >= '1' && ch <= '9') {
            int idx = ch - '1';
            if (idx < ls->account_count && idx != ls->account_sel) {
                ls->account_sel = idx;
                ls->dirty = true;
            }
            return true;
        }
        return false;
    }
}

const char *txn_list_status_hint(const txn_list_state_t *ls) {
    static char buf[256];

    if (ls->filter_active)
        return "Type to filter  Enter:done  Esc:clear";

    if (ls->txn_count == 0)
        return "1-9 acct  a add  /filter  s sort  \u2190 back";

    const char *filter_tag = ls->filter_len > 0 ? "/filter[on]" : "/filter";
    snprintf(buf, sizeof(buf),
             "\u2191\u2193 move  e edit  d delete  %s  s sort  S dir  1-9 acct "
             " a add  \u2190 back",
             filter_tag);
    return buf;
}

void txn_list_mark_dirty(txn_list_state_t *ls) {
    if (ls)
        ls->dirty = true;
}
