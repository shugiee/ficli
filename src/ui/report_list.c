#include "ui/report_list.h"

#include "db/query.h"
#include "ui/colors.h"
#include "ui/form.h"

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

    budget_txn_row_t *related_txns;
    int related_txn_count;
    bool related_visible;
    bool related_focus;
    int related_cursor;
    int related_scroll_offset;
    report_group_t related_group;
    report_period_t related_period;
    char related_label[128];

    char message[96];
    bool dirty;
};

static report_list_state_t *g_sort_ctx = NULL;

static void format_cents(int64_t cents, bool show_plus, char *buf, int n);

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

static void clear_related_transactions(report_list_state_t *ls) {
    if (!ls)
        return;
    free(ls->related_txns);
    ls->related_txns = NULL;
    ls->related_txn_count = 0;
    ls->related_cursor = 0;
    ls->related_scroll_offset = 0;
    ls->related_focus = false;
}

static void hide_related_transactions(report_list_state_t *ls) {
    if (!ls)
        return;
    clear_related_transactions(ls);
    ls->related_visible = false;
    ls->related_label[0] = '\0';
}

static void format_related_amount(const budget_txn_row_t *txn, char *buf, int n) {
    if (!txn || !buf || n <= 0)
        return;

    int64_t signed_cents = txn->amount_cents;
    if (txn->type == TRANSACTION_EXPENSE)
        signed_cents = -signed_cents;
    format_cents(signed_cents, false, buf, n);
}

static void compose_related_details(const budget_txn_row_t *txn, char *buf,
                                    int n) {
    if (!txn || !buf || n <= 0)
        return;

    char memo[512];
    if (txn->payee[0] != '\0' && txn->description[0] != '\0') {
        snprintf(memo, sizeof(memo), "%s | %s", txn->payee, txn->description);
    } else if (txn->payee[0] != '\0') {
        snprintf(memo, sizeof(memo), "%s", txn->payee);
    } else if (txn->description[0] != '\0') {
        snprintf(memo, sizeof(memo), "%s", txn->description);
    } else {
        memo[0] = '\0';
    }

    if (txn->category_name[0] != '\0' && memo[0] != '\0')
        snprintf(buf, (size_t)n, "%s: %s", txn->category_name, memo);
    else if (txn->category_name[0] != '\0')
        snprintf(buf, (size_t)n, "%s", txn->category_name);
    else if (memo[0] != '\0')
        snprintf(buf, (size_t)n, "%s", memo);
    else
        snprintf(buf, (size_t)n, "(no details)");
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

static void clamp_related_cursor_and_scroll(report_list_state_t *ls,
                                            int visible_rows) {
    if (!ls)
        return;
    if (ls->related_txn_count <= 0) {
        ls->related_cursor = 0;
        ls->related_scroll_offset = 0;
        return;
    }

    if (ls->related_cursor < 0)
        ls->related_cursor = 0;
    if (ls->related_cursor >= ls->related_txn_count)
        ls->related_cursor = ls->related_txn_count - 1;

    if (visible_rows < 1)
        visible_rows = 1;

    if (ls->related_cursor < ls->related_scroll_offset)
        ls->related_scroll_offset = ls->related_cursor;
    else if (ls->related_cursor >= ls->related_scroll_offset + visible_rows)
        ls->related_scroll_offset = ls->related_cursor - visible_rows + 1;

    if (ls->related_scroll_offset < 0)
        ls->related_scroll_offset = 0;
    int max_scroll = ls->related_txn_count - visible_rows;
    if (max_scroll < 0)
        max_scroll = 0;
    if (ls->related_scroll_offset > max_scroll)
        ls->related_scroll_offset = max_scroll;
}

static bool refresh_related_transactions(report_list_state_t *ls) {
    if (!ls || !ls->related_visible || ls->related_label[0] == '\0')
        return true;

    budget_txn_row_t *related = NULL;
    int related_count = db_get_report_transactions(
        ls->db, ls->related_group, ls->related_period, ls->related_label,
        &related);
    if (related_count < 0) {
        snprintf(ls->message, sizeof(ls->message),
                 "Error loading matching transactions");
        free(related);
        return false;
    }

    clear_related_transactions(ls);
    ls->related_txns = related;
    ls->related_txn_count = related_count;
    ls->related_focus = (related_count > 0);
    return true;
}

static void show_related_transactions_for_cursor(report_list_state_t *ls) {
    if (!ls || ls->row_count <= 0 || ls->cursor < 0 || ls->cursor >= ls->row_count)
        return;

    const report_row_t *rr = &ls->rows[ls->cursor];
    ls->related_visible = true;
    ls->related_group = ls->group;
    ls->related_period = ls->period;
    snprintf(ls->related_label, sizeof(ls->related_label), "%s", rr->label);

    if (!refresh_related_transactions(ls))
        return;

    ls->related_cursor = 0;
    ls->related_scroll_offset = 0;
    ls->related_focus = (ls->related_txn_count > 0);
}

static bool edit_selected_related_transaction(report_list_state_t *ls,
                                              WINDOW *parent) {
    if (!ls || !parent || !ls->related_focus || ls->related_txn_count <= 0 ||
        !ls->related_txns)
        return false;

    if (ls->related_cursor < 0 || ls->related_cursor >= ls->related_txn_count)
        return false;

    int64_t txn_id = ls->related_txns[ls->related_cursor].id;
    if (txn_id <= 0)
        return false;

    transaction_t txn = {0};
    int rc = db_get_transaction_by_id(ls->db, (int)txn_id, &txn);
    if (rc != 0) {
        snprintf(ls->message, sizeof(ls->message), "Error loading transaction");
        ls->dirty = true;
        return true;
    }

    form_result_t res = form_transaction(parent, ls->db, &txn, true);
    if (res == FORM_SAVED) {
        ls->dirty = true;
        snprintf(ls->message, sizeof(ls->message), "Saved transaction");
    }
    return true;
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

    if (ls->related_visible) {
        if (ls->related_group == ls->group && ls->related_period == ls->period)
            refresh_related_transactions(ls);
        else
            hide_related_transactions(ls);
    }

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
    ls->related_group = ls->group;
    ls->related_period = ls->period;
    ls->dirty = true;
    return ls;
}

void report_list_destroy(report_list_state_t *ls) {
    if (!ls)
        return;
    free(ls->rows);
    free(ls->related_txns);
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

    int related_title_row = -1;
    int related_header_row = -1;
    int related_rule_row = -1;
    int related_data_row_start = -1;
    int related_data_row_end = -1;
    bool show_related_section = (h >= 20);
    if (show_related_section) {
        int body_start = data_row_start;
        int body_end = h - 2;
        int total_body_rows = body_end - body_start + 1;
        int gap_rows = 2;
        int related_data_rows = total_body_rows / 3;
        if (related_data_rows < 3)
            related_data_rows = 3;

        int summary_data_rows =
            total_body_rows - gap_rows - 5 - related_data_rows;
        while (summary_data_rows < 3 && related_data_rows > 3) {
            related_data_rows--;
            summary_data_rows =
                total_body_rows - gap_rows - 5 - related_data_rows;
        }
        if (summary_data_rows < 3)
            show_related_section = false;

        if (show_related_section) {
            data_row_end = data_row_start + summary_data_rows - 1;
            related_title_row = data_row_end + gap_rows + 1;
            related_header_row = related_title_row + 1;
            related_rule_row = related_title_row + 2;
            related_data_row_start = related_title_row + 3;
            related_data_row_end = body_end;
        }
    }

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
    } else {
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
                      rr->label, amount_w, amount_w, expense, amount_w,
                      amount_w, income);

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

            mvwprintw(
                win, row,
                2 + name_w + 1 + amount_w + 1 + amount_w + 1 + amount_w + 1,
                "%*d", txn_w, rr->txn_count);

            if (selected && focused)
                wattroff(win, COLOR_PAIR(COLOR_SELECTED));
        }
    }

    if (!show_related_section)
        return;

    int left = 2;
    int avail = w - 4;

    char related_title[256];
    if (!ls->related_visible) {
        snprintf(related_title, sizeof(related_title),
                 "Matching Transactions (press Enter on a report row)");
    } else if (ls->related_txn_count > 0) {
        snprintf(related_title, sizeof(related_title),
                 "Matching Transactions - %s (%d)", ls->related_label,
                 ls->related_txn_count);
    } else {
        snprintf(related_title, sizeof(related_title),
                 "Matching Transactions - %s (0)", ls->related_label);
    }

    wattron(win, A_BOLD);
    mvwprintw(win, related_title_row, left, "%*s", avail, "");
    mvwprintw(win, related_title_row, left, "%-*.*s", avail, avail, related_title);
    wattroff(win, A_BOLD);

    int date_w = 10;
    int amount2_w = 11;
    int account_w = (avail >= 64) ? 14 : ((avail >= 52) ? 10 : 0);
    int details_w =
        avail - date_w - 1 - amount2_w - 1 - (account_w > 0 ? account_w + 1 : 0);
    if (details_w < 8) {
        account_w = 0;
        details_w = avail - date_w - 1 - amount2_w - 1;
    }
    if (details_w < 8)
        details_w = 8;

    int date_col = left;
    int amount_col = date_col + date_w + 1;
    int account_col = amount_col + amount2_w + 1;
    int details_col = account_col + (account_w > 0 ? account_w + 1 : 0);

    wattron(win, A_BOLD);
    mvwprintw(win, related_header_row, date_col, "%-*s", date_w, "Date");
    mvwprintw(win, related_header_row, amount_col, "%*.*s", amount2_w, amount2_w,
              "Amount");
    if (account_w > 0)
        mvwprintw(win, related_header_row, account_col, "%-*s", account_w,
                  "Account");
    mvwprintw(win, related_header_row, details_col, "%-*s", details_w,
              "Details");
    wattroff(win, A_BOLD);

    mvwhline(win, related_rule_row, left, ACS_HLINE, avail);

    int related_data_rows = related_data_row_end - related_data_row_start + 1;
    if (related_data_rows < 1)
        related_data_rows = 1;

    if (!ls->related_visible) {
        wattron(win, A_DIM);
        mvwprintw(win, related_data_row_start, left,
                  "Press Enter to show matching transactions");
        wattroff(win, A_DIM);
        return;
    }

    if (ls->related_txn_count <= 0) {
        wattron(win, A_DIM);
        mvwprintw(win, related_data_row_start, left,
                  "No matching transactions for %s", ls->related_label);
        wattroff(win, A_DIM);
        return;
    }

    clamp_related_cursor_and_scroll(ls, related_data_rows);
    for (int i = 0; i < related_data_rows; i++) {
        int idx = ls->related_scroll_offset + i;
        if (idx >= ls->related_txn_count)
            break;

        int row = related_data_row_start + i;
        const budget_txn_row_t *txn = &ls->related_txns[idx];
        bool selected = (ls->related_focus && idx == ls->related_cursor);

        if (selected) {
            if (!focused)
                wattron(win, A_DIM);
            wattron(win, A_REVERSE);
        }

        mvwprintw(win, row, left, "%*s", avail, "");
        mvwprintw(win, row, date_col, "%-*.*s", date_w, date_w,
                  txn->effective_date);

        char amount_buf[24];
        format_related_amount(txn, amount_buf, sizeof(amount_buf));
        if (txn->type == TRANSACTION_EXPENSE)
            wattron(win, COLOR_PAIR(COLOR_EXPENSE));
        else
            wattron(win, COLOR_PAIR(COLOR_INCOME));
        mvwprintw(win, row, amount_col, "%*.*s", amount2_w, amount2_w,
                  amount_buf);
        if (txn->type == TRANSACTION_EXPENSE)
            wattroff(win, COLOR_PAIR(COLOR_EXPENSE));
        else
            wattroff(win, COLOR_PAIR(COLOR_INCOME));

        if (account_w > 0)
            mvwprintw(win, row, account_col, "%-*.*s", account_w, account_w,
                      txn->account_name);

        char details[512];
        compose_related_details(txn, details, sizeof(details));
        mvwprintw(win, row, details_col, "%-*.*s", details_w, details_w,
                  details);

        if (selected) {
            wattroff(win, A_REVERSE);
            if (!focused)
                wattroff(win, A_DIM);
        }
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
        hide_related_transactions(ls);
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
    hide_related_transactions(ls);
}

bool report_list_handle_input(report_list_state_t *ls, WINDOW *parent, int ch) {
    if (!ls)
        return false;

    if (ls->related_focus) {
        switch (ch) {
        case KEY_UP:
        case 'k':
            if (ls->related_txn_count > 0 && ls->related_cursor > 0)
                ls->related_cursor--;
            return true;
        case KEY_DOWN:
        case 'j':
            if (ls->related_txn_count > 0 &&
                ls->related_cursor < ls->related_txn_count - 1)
                ls->related_cursor++;
            return true;
        case KEY_NPAGE:
        case 4:
            if (ls->related_txn_count > 0) {
                ls->related_cursor += 10;
                if (ls->related_cursor >= ls->related_txn_count)
                    ls->related_cursor = ls->related_txn_count - 1;
            }
            return true;
        case KEY_PPAGE:
        case 21:
            if (ls->related_txn_count > 0) {
                ls->related_cursor -= 10;
                if (ls->related_cursor < 0)
                    ls->related_cursor = 0;
            }
            return true;
        case 'e':
            return edit_selected_related_transaction(ls, parent);
        case '\t':
        case 27:
        case '\n':
            ls->related_focus = false;
            return true;
        default:
            break;
        }
    }

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
    case '\n':
        show_related_transactions_for_cursor(ls);
        return true;
    default:
        return false;
    }
}

const char *report_list_status_hint(const report_list_state_t *ls) {
    if (!ls)
        return "";
    if (ls->related_focus)
        return "q:Quit  Up/Down:Navigate transactions  e:Edit transaction  Enter/Tab/Esc:Back to report rows";
    return "q:Quit  Up/Down:Navigate  Enter:Show matches  [ ] or /:Category/Payee  p:Period  s:Sort column  S:Sort dir  Esc:Sidebar";
}

void report_list_mark_dirty(report_list_state_t *ls) {
    if (ls)
        ls->dirty = true;
}
