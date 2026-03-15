#include "ui/report_list.h"

#include "db/query.h"
#include "ui/colors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef enum {
    REPORT_SORT_NAME = 0,
    REPORT_SORT_EXPENSE,
    REPORT_SORT_INCOME,
    REPORT_SORT_NET,
    REPORT_SORT_TXNS,
    REPORT_SORT_COUNT,
} report_sort_col_t;

struct report_list_state {
    sqlite3 *db;
    report_group_t group;
    report_period_t period;
    report_sort_col_t sort_col;
    bool sort_asc;

    report_row_t *rows;
    int row_count;

    int cursor;
    int scroll_offset;
    char message[96];
    bool dirty;
};

static report_list_state_t *g_sort_ctx = NULL;

static const char *group_label(report_group_t group) {
    return group == REPORT_GROUP_PAYEE ? "Payee" : "Category";
}

static const char *period_label(report_period_t period) {
    switch (period) {
    case REPORT_PERIOD_THIS_MONTH:
        return "This Month";
    case REPORT_PERIOD_LAST_30_DAYS:
        return "Last 30 Days";
    case REPORT_PERIOD_YTD:
        return "YTD";
    case REPORT_PERIOD_LAST_12_MONTHS:
        return "Last 12 Months";
    default:
        return "Unknown";
    }
}

static const char *sort_label(report_sort_col_t sort_col) {
    switch (sort_col) {
    case REPORT_SORT_NAME:
        return "Name";
    case REPORT_SORT_EXPENSE:
        return "Expense";
    case REPORT_SORT_INCOME:
        return "Income";
    case REPORT_SORT_NET:
        return "Net";
    case REPORT_SORT_TXNS:
        return "Txns";
    default:
        return "Unknown";
    }
}

static int compare_rows(const void *a, const void *b) {
    const report_row_t *ra = (const report_row_t *)a;
    const report_row_t *rb = (const report_row_t *)b;
    report_list_state_t *ls = g_sort_ctx;

    if (!ls)
        return 0;

    int cmp = 0;
    switch (ls->sort_col) {
    case REPORT_SORT_NAME:
        cmp = strcasecmp(ra->label, rb->label);
        break;
    case REPORT_SORT_EXPENSE:
        if (ra->expense_cents < rb->expense_cents)
            cmp = -1;
        else if (ra->expense_cents > rb->expense_cents)
            cmp = 1;
        break;
    case REPORT_SORT_INCOME:
        if (ra->income_cents < rb->income_cents)
            cmp = -1;
        else if (ra->income_cents > rb->income_cents)
            cmp = 1;
        break;
    case REPORT_SORT_NET:
        if (ra->net_cents < rb->net_cents)
            cmp = -1;
        else if (ra->net_cents > rb->net_cents)
            cmp = 1;
        break;
    case REPORT_SORT_TXNS:
        if (ra->txn_count < rb->txn_count)
            cmp = -1;
        else if (ra->txn_count > rb->txn_count)
            cmp = 1;
        break;
    default:
        break;
    }

    if (cmp == 0)
        cmp = strcasecmp(ra->label, rb->label);

    return ls->sort_asc ? cmp : -cmp;
}

static void sort_rows(report_list_state_t *ls) {
    if (!ls || !ls->rows || ls->row_count <= 1)
        return;
    g_sort_ctx = ls;
    qsort(ls->rows, (size_t)ls->row_count, sizeof(report_row_t), compare_rows);
    g_sort_ctx = NULL;
}

static void format_cents(int64_t cents, bool show_plus, char *buf, int n) {
    int64_t abs_cents = cents < 0 ? -cents : cents;
    int64_t whole = abs_cents / 100;
    int64_t frac = abs_cents % 100;

    char raw[32];
    snprintf(raw, sizeof(raw), "%ld", (long)whole);
    int raw_len = (int)strlen(raw);

    char formatted[48];
    int fi = 0;
    for (int i = 0; i < raw_len; i++) {
        if (i > 0 && (raw_len - i) % 3 == 0)
            formatted[fi++] = ',';
        formatted[fi++] = raw[i];
    }
    formatted[fi] = '\0';

    if (cents < 0)
        snprintf(buf, n, "-%s.%02ld", formatted, (long)frac);
    else if (show_plus)
        snprintf(buf, n, "+%s.%02ld", formatted, (long)frac);
    else
        snprintf(buf, n, "%s.%02ld", formatted, (long)frac);
}

static void clamp_cursor_and_scroll(report_list_state_t *ls, int visible_rows) {
    if (!ls)
        return;
    if (ls->row_count <= 0) {
        ls->cursor = 0;
        ls->scroll_offset = 0;
        return;
    }

    if (ls->cursor < 0)
        ls->cursor = 0;
    if (ls->cursor >= ls->row_count)
        ls->cursor = ls->row_count - 1;

    if (visible_rows < 1)
        visible_rows = 1;

    if (ls->cursor < ls->scroll_offset)
        ls->scroll_offset = ls->cursor;
    else if (ls->cursor >= ls->scroll_offset + visible_rows)
        ls->scroll_offset = ls->cursor - visible_rows + 1;

    if (ls->scroll_offset < 0)
        ls->scroll_offset = 0;
    int max_scroll = ls->row_count - visible_rows;
    if (max_scroll < 0)
        max_scroll = 0;
    if (ls->scroll_offset > max_scroll)
        ls->scroll_offset = max_scroll;
}

static void reload(report_list_state_t *ls) {
    if (!ls)
        return;

    report_row_t *rows = NULL;
    int count = db_get_report_rows(ls->db, ls->group, ls->period, &rows);
    if (count < 0) {
        snprintf(ls->message, sizeof(ls->message), "Error loading report data");
        free(rows);
        return;
    }

    free(ls->rows);
    ls->rows = rows;
    ls->row_count = count;

    sort_rows(ls);

    if (ls->cursor >= ls->row_count)
        ls->cursor = ls->row_count > 0 ? ls->row_count - 1 : 0;

    if (ls->row_count == 0)
        snprintf(ls->message, sizeof(ls->message), "No rows for this period");
    else
        ls->message[0] = '\0';

    ls->dirty = false;
}

report_list_state_t *report_list_create(sqlite3 *db) {
    report_list_state_t *ls = calloc(1, sizeof(*ls));
    if (!ls)
        return NULL;

    ls->db = db;
    ls->group = REPORT_GROUP_CATEGORY;
    ls->period = REPORT_PERIOD_THIS_MONTH;
    ls->sort_col = REPORT_SORT_EXPENSE;
    ls->sort_asc = false;
    ls->dirty = true;
    return ls;
}

void report_list_destroy(report_list_state_t *ls) {
    if (!ls)
        return;
    free(ls->rows);
    free(ls);
}

void report_list_draw(report_list_state_t *ls, WINDOW *win, bool focused) {
    if (!ls || !win)
        return;
    if (ls->dirty)
        reload(ls);

    int h, w;
    getmaxyx(win, h, w);

    mvwprintw(win, 1, 2, "Reports  View:[%s]  Period:[%s]", group_label(ls->group),
              period_label(ls->period));
    mvwprintw(win, 2, 2, "Sort:%s %s", sort_label(ls->sort_col),
              ls->sort_asc ? "asc" : "desc");

    if (ls->message[0] != '\0')
        mvwprintw(win, 3, 2, "%s", ls->message);

    int header_row = 4;
    int rule_row = 5;
    int data_row_start = 6;
    int data_row_end = h - 2;
    int visible_rows = data_row_end - data_row_start + 1;
    if (visible_rows < 1)
        visible_rows = 1;

    int table_w = w - 4;
    int amount_w = 11;
    int txn_w = 5;
    int fixed_w = amount_w + amount_w + amount_w + txn_w + 4;
    int name_w = table_w - fixed_w;
    if (name_w < 8)
        name_w = 8;

    clamp_cursor_and_scroll(ls, visible_rows);

    wattron(win, A_BOLD);
    mvwprintw(win, header_row, 2, "%-*.*s %*s %*s %*s %*s", name_w, name_w,
              "Name", amount_w, "Expense", amount_w, "Income", amount_w,
              "Net", txn_w, "Txns");
    wattroff(win, A_BOLD);
    mvwhline(win, rule_row, 2, ACS_HLINE, w - 4);

    if (ls->row_count == 0) {
        mvwprintw(win, data_row_start, 2, "No matching rows");
        return;
    }

    for (int i = 0; i < visible_rows; i++) {
        int idx = ls->scroll_offset + i;
        if (idx >= ls->row_count)
            break;
        int row = data_row_start + i;
        const report_row_t *rr = &ls->rows[idx];

        char expense[24], income[24], net[24];
        format_cents(rr->expense_cents, false, expense, sizeof(expense));
        format_cents(rr->income_cents, false, income, sizeof(income));
        format_cents(rr->net_cents, true, net, sizeof(net));

        bool selected = (idx == ls->cursor);
        if (selected && focused)
            wattron(win, COLOR_PAIR(COLOR_SELECTED));

        mvwprintw(win, row, 2, "%-*.*s %*.*s %*.*s ", name_w, name_w,
                  rr->label, amount_w, amount_w, expense, amount_w, amount_w,
                  income);

        if (rr->net_cents < 0)
            wattron(win, COLOR_PAIR(COLOR_EXPENSE));
        else if (rr->net_cents > 0)
            wattron(win, COLOR_PAIR(COLOR_INCOME));
        mvwprintw(win, row, 2 + name_w + 1 + amount_w + 1 + amount_w + 1,
                  "%*.*s", amount_w, amount_w, net);
        if (rr->net_cents != 0) {
            if (rr->net_cents < 0)
                wattroff(win, COLOR_PAIR(COLOR_EXPENSE));
            else
                wattroff(win, COLOR_PAIR(COLOR_INCOME));
        }

        mvwprintw(win, row,
                  2 + name_w + 1 + amount_w + 1 + amount_w + 1 + amount_w + 1,
                  "%*d", txn_w, rr->txn_count);

        if (selected && focused)
            wattroff(win, COLOR_PAIR(COLOR_SELECTED));
    }
}

static void cycle_group(report_list_state_t *ls, int delta) {
    if (!ls)
        return;
    int next = (int)ls->group + delta;
    if (next < 0)
        next = REPORT_GROUP_PAYEE;
    if (next > REPORT_GROUP_PAYEE)
        next = REPORT_GROUP_CATEGORY;
    if ((report_group_t)next != ls->group) {
        ls->group = (report_group_t)next;
        ls->dirty = true;
        ls->cursor = 0;
        ls->scroll_offset = 0;
    }
}

static void cycle_period(report_list_state_t *ls) {
    if (!ls)
        return;
    int next = (int)ls->period + 1;
    if (next > REPORT_PERIOD_LAST_12_MONTHS)
        next = REPORT_PERIOD_THIS_MONTH;
    ls->period = (report_period_t)next;
    ls->dirty = true;
    ls->cursor = 0;
    ls->scroll_offset = 0;
}

bool report_list_handle_input(report_list_state_t *ls, WINDOW *parent, int ch) {
    (void)parent;
    if (!ls)
        return false;

    switch (ch) {
    case KEY_UP:
    case 'k':
        if (ls->row_count > 0 && ls->cursor > 0)
            ls->cursor--;
        return true;
    case KEY_DOWN:
    case 'j':
        if (ls->row_count > 0 && ls->cursor < ls->row_count - 1)
            ls->cursor++;
        return true;
    case KEY_HOME:
    case 'g':
        ls->cursor = 0;
        return true;
    case KEY_END:
    case 'G':
        if (ls->row_count > 0)
            ls->cursor = ls->row_count - 1;
        return true;
    case KEY_NPAGE:
    case 4:
        if (ls->row_count > 0) {
            ls->cursor += 10;
            if (ls->cursor >= ls->row_count)
                ls->cursor = ls->row_count - 1;
        }
        return true;
    case KEY_PPAGE:
    case 21:
        if (ls->row_count > 0) {
            ls->cursor -= 10;
            if (ls->cursor < 0)
                ls->cursor = 0;
        }
        return true;
    case '[':
        cycle_group(ls, -1);
        return true;
    case ']':
        cycle_group(ls, 1);
        return true;
    case '/':
        cycle_group(ls, 1);
        return true;
    case 'p':
        cycle_period(ls);
        return true;
    case 's':
        ls->sort_col = (report_sort_col_t)(((int)ls->sort_col + 1) %
                                           (int)REPORT_SORT_COUNT);
        sort_rows(ls);
        return true;
    case 'S':
        ls->sort_asc = !ls->sort_asc;
        sort_rows(ls);
        return true;
    default:
        return false;
    }
}

const char *report_list_status_hint(const report_list_state_t *ls) {
    if (!ls)
        return "";
    return "q:Quit  Up/Down:Navigate  [ ] or /:Category/Payee  p:Period  s:Sort column  S:Sort dir  Esc:Sidebar";
}

void report_list_mark_dirty(report_list_state_t *ls) {
    if (ls)
        ls->dirty = true;
}
