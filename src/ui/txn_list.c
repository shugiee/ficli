#include "ui/txn_list.h"
#include "db/query.h"
#include "ui/form.h"
#include "models/account.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// Color pair IDs — must match ui.c definitions
#define COLOR_SELECTED 2
#define COLOR_EXPENSE  12
#define COLOR_INCOME   13

// Layout constants: rows consumed above data rows (inside box border)
// row 0: top border
// row 1: account tabs
// row 2: blank separator
// row 3: column headers
// row 4: horizontal rule
// data starts at row 5
#define DATA_ROW_START 5

struct txn_list_state {
    sqlite3 *db;
    account_t *accounts;
    int account_count;
    int account_sel;

    txn_row_t *transactions;
    int txn_count;

    int cursor;
    int scroll_offset;
    bool dirty;
};

static bool confirm_delete(WINDOW *parent) {
    int ph, pw;
    getmaxyx(parent, ph, pw);

    int win_h = 7;
    int win_w = 42;
    if (ph < win_h) win_h = ph;
    if (pw < win_w) win_w = pw;
    if (win_h < 4 || win_w < 20)
        return false;

    int py, px;
    getbegyx(parent, py, px);
    int win_y = py + (ph - win_h) / 2;
    int win_x = px + (pw - win_w) / 2;

    WINDOW *w = newwin(win_h, win_w, win_y, win_x);
    keypad(w, TRUE);
    wbkgd(w, COLOR_PAIR(10));
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
    if (ls->account_count < 0) ls->account_count = 0;

    if (ls->account_sel >= ls->account_count)
        ls->account_sel = 0;

    if (ls->account_count > 0) {
        int64_t acct_id = ls->accounts[ls->account_sel].id;
        ls->txn_count = db_get_transactions(ls->db, acct_id, &ls->transactions);
        if (ls->txn_count < 0) ls->txn_count = 0;
    }

    ls->dirty = false;
}

txn_list_state_t *txn_list_create(sqlite3 *db) {
    txn_list_state_t *ls = calloc(1, sizeof(*ls));
    if (!ls) return NULL;
    ls->db = db;
    ls->dirty = true;
    return ls;
}

void txn_list_destroy(txn_list_state_t *ls) {
    if (!ls) return;
    free(ls->accounts);
    free(ls->transactions);
    free(ls);
}

static void format_amount(int64_t cents, transaction_type_t type, char *buf, int buflen) {
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

void txn_list_draw(txn_list_state_t *ls, WINDOW *win, bool focused) {
    if (ls->dirty) reload(ls);

    int h, w;
    getmaxyx(win, h, w);

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

    // -- Column headers (row 3) --
    // Column layout: Date(10) gap(1) Type(8) gap(1) Category(16) gap(1) Amount(10) gap(1) Desc(rest)
    int desc_w = w - 2 - 10 - 1 - 8 - 1 - 16 - 1 - 10 - 1;
    if (desc_w < 4) desc_w = 4;

    wattron(win, A_BOLD);
    mvwprintw(win, 3, 2, "%-10s %-8s %-16s %10s %-*s",
              "Date", "Type", "Category", "Amount", desc_w, "Description");
    wattroff(win, A_BOLD);

    // -- Horizontal rule (row 4) --
    wmove(win, 4, 2);
    for (int i = 2; i < w - 2; i++)
        waddch(win, ACS_HLINE);

    // -- Data rows --
    int visible_rows = h - 2 - DATA_ROW_START + 1;
    // h-2 is the last usable row inside box (row h-1 is bottom border)
    // data starts at DATA_ROW_START, so visible = (h - 2) - DATA_ROW_START + 1
    // simplify: visible_rows = h - 1 - DATA_ROW_START
    visible_rows = h - 1 - DATA_ROW_START;
    if (visible_rows < 1) visible_rows = 1;

    if (ls->txn_count == 0) {
        const char *msg = "No transactions";
        int mlen = (int)strlen(msg);
        int row = DATA_ROW_START + visible_rows / 2;
        if (row >= h - 1) row = DATA_ROW_START;
        mvwprintw(win, row, (w - mlen) / 2, "%s", msg);
        return;
    }

    // Clamp cursor/scroll
    if (ls->cursor < 0) ls->cursor = 0;
    if (ls->cursor >= ls->txn_count) ls->cursor = ls->txn_count - 1;

    if (ls->cursor < ls->scroll_offset)
        ls->scroll_offset = ls->cursor;
    if (ls->cursor >= ls->scroll_offset + visible_rows)
        ls->scroll_offset = ls->cursor - visible_rows + 1;
    if (ls->scroll_offset < 0) ls->scroll_offset = 0;

    for (int i = 0; i < visible_rows; i++) {
        int idx = ls->scroll_offset + i;
        if (idx >= ls->txn_count) break;

        txn_row_t *t = &ls->transactions[idx];
        int row = DATA_ROW_START + i;

        const char *type_str;
        switch (t->type) {
        case TRANSACTION_INCOME:   type_str = "Income";   break;
        case TRANSACTION_TRANSFER: type_str = "Transfer"; break;
        default:                   type_str = "Expense";  break;
        }

        char amt[24];
        format_amount(t->amount_cents, t->type, amt, sizeof(amt));

        // Truncate description to fit
        char desc_buf[256];
        snprintf(desc_buf, sizeof(desc_buf), "%-*.*s", desc_w, desc_w, t->description);

        bool selected = (idx == ls->cursor);
        if (selected) {
            if (!focused) wattron(win, A_DIM);
            wattron(win, A_REVERSE);
        }

        // Print the base row (clears it)
        mvwprintw(win, row, 2, "%-10s %-8s %-16.16s",
                  t->date, type_str, t->category_name);

        // Amount with color
        int amount_col = 2 + 10 + 1 + 8 + 1 + 16 + 1;
        int color = (t->type == TRANSACTION_EXPENSE) ? COLOR_EXPENSE : COLOR_INCOME;
        if (selected) {
            // Keep reverse, add color
            wattron(win, COLOR_PAIR(color));
            mvwprintw(win, row, amount_col, "%10s", amt);
            wattroff(win, COLOR_PAIR(color));
        } else {
            wattron(win, COLOR_PAIR(color));
            mvwprintw(win, row, amount_col, "%10s", amt);
            wattroff(win, COLOR_PAIR(color));
        }

        // Description
        mvwprintw(win, row, amount_col + 10 + 1, "%s", desc_buf);

        if (selected) {
            wattroff(win, A_REVERSE);
            if (!focused) wattroff(win, A_DIM);
        }
    }
}

bool txn_list_handle_input(txn_list_state_t *ls, WINDOW *parent, int ch) {
    int visible_rows = 20; // will be corrected by draw, but we need a sensible default

    switch (ch) {
    case KEY_UP:
    case 'k':
        if (ls->cursor > 0) ls->cursor--;
        return true;
    case KEY_DOWN:
    case 'j':
        if (ls->cursor < ls->txn_count - 1) ls->cursor++;
        return true;
    case KEY_HOME:
    case 'g':
        ls->cursor = 0;
        return true;
    case KEY_END:
    case 'G':
        ls->cursor = ls->txn_count > 0 ? ls->txn_count - 1 : 0;
        return true;
    case KEY_PPAGE:
        ls->cursor -= visible_rows;
        if (ls->cursor < 0) ls->cursor = 0;
        return true;
    case KEY_NPAGE:
        ls->cursor += visible_rows;
        if (ls->cursor >= ls->txn_count) ls->cursor = ls->txn_count > 0 ? ls->txn_count - 1 : 0;
        return true;
    case 'e':
        if (ls->txn_count <= 0)
            return true;
        {
            transaction_t txn = {0};
            int rc = db_get_transaction_by_id(ls->db, (int)ls->transactions[ls->cursor].id, &txn);
            if (rc == 0) {
                form_result_t res = form_transaction(parent, ls->db, &txn, true);
                if (res == FORM_SAVED)
                    ls->dirty = true;
            } else {
                ls->dirty = true;
            }
        }
        return true;
    case 'd':
        if (ls->txn_count <= 0)
            return true;
        if (confirm_delete(parent)) {
            int rc = db_delete_transaction(ls->db, (int)ls->transactions[ls->cursor].id);
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
    if (ls->txn_count > 0)
        return "\u2191\u2193 move  1-9 account  e edit  d delete  a add  \u2190 back";
    return "\u2191\u2193 move  1-9 account  a add  \u2190 back";
}

void txn_list_mark_dirty(txn_list_state_t *ls) {
    if (ls) ls->dirty = true;
}
