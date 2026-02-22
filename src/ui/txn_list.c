#include "ui/txn_list.h"
#include "db/query.h"
#include "models/account.h"
#include "ui/colors.h"
#include "ui/form.h"
#include "ui/resize.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAP_WIDTH 3
#define DATE_COL_WIDTH 10
#define REFLECTION_DATE_COL_WIDTH 10
#define TYPE_COL_WIDTH 8
#define CATEGORY_COL_WIDTH 20
#define AMOUNT_COL_WIDTH 13
#define PAYEE_COL_WIDTH 24
#define BALANCE_CHART_LOOKBACK_DAYS 90
#define CHART_PLOT_HEIGHT 6
#define CHART_MIN_WIDTH 56
#define CHART_SCALE_CAP_CENTS 100000
// Description column takes remaining width, but enforce a minimum for usability
#define DESC_COL_MIN_WIDTH 4

// Layout constants: rows consumed above data rows (inside box border)
// row 0: top border
// row 1: account tabs
// row 2: spacer
// row 3: spacer
// row 4: summary line
// row 5: spacer
// row 6: spacer
// row 7+: optional chart block
// filter, headers, rule, and data start rows are computed per-window
#define SUMMARY_ROW 4
#define BASE_FILTER_ROW 7
#define BASE_HEADER_ROW 8
#define BASE_RULE_ROW 9
#define BASE_DATA_ROW_START 10

typedef enum {
    SORT_DATE,
    SORT_TYPE,
    SORT_CATEGORY,
    SORT_AMOUNT,
    SORT_PAYEE,
    SORT_DESCRIPTION,
    SORT_COUNT,
} sort_col_t;

struct txn_list_state {
    sqlite3 *db;
    account_t *accounts;
    int account_count;
    int account_sel;

    txn_row_t *transactions;
    int txn_count;

    int64_t *selected_ids;
    int selected_count;
    int selected_capacity;

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

    int64_t balance_cents;
    balance_point_t *balance_series;
    int balance_series_count;
    int64_t month_net_cents;
    int64_t month_income_cents;
    int64_t month_expense_cents;

    int cursor;
    int scroll_offset;
    int next_reload_cursor;
    int64_t next_reload_focus_txn_id;
    bool center_cursor_next_draw;
    bool dirty;
};

static bool txn_list_is_selected(const txn_list_state_t *ls, int64_t id) {
    if (!ls || ls->selected_count <= 0 || !ls->selected_ids)
        return false;
    for (int i = 0; i < ls->selected_count; i++) {
        if (ls->selected_ids[i] == id)
            return true;
    }
    return false;
}

static void txn_list_clear_selected(txn_list_state_t *ls) {
    if (!ls)
        return;
    ls->selected_count = 0;
}

static void txn_list_toggle_selected(txn_list_state_t *ls, int64_t id) {
    if (!ls)
        return;
    for (int i = 0; i < ls->selected_count; i++) {
        if (ls->selected_ids[i] == id) {
            ls->selected_ids[i] = ls->selected_ids[ls->selected_count - 1];
            ls->selected_count--;
            return;
        }
    }
    if (ls->selected_count >= ls->selected_capacity) {
        int new_cap = (ls->selected_capacity == 0) ? 8 : ls->selected_capacity * 2;
        int64_t *tmp = realloc(ls->selected_ids, new_cap * sizeof(int64_t));
        if (!tmp)
            return;
        ls->selected_ids = tmp;
        ls->selected_capacity = new_cap;
    }
    ls->selected_ids[ls->selected_count++] = id;
}

static void txn_list_select_id(txn_list_state_t *ls, int64_t id) {
    if (!ls || id <= 0)
        return;
    if (txn_list_is_selected(ls, id))
        return;
    if (ls->selected_count >= ls->selected_capacity) {
        int new_cap = (ls->selected_capacity == 0) ? 8 : ls->selected_capacity * 2;
        int64_t *tmp = realloc(ls->selected_ids, new_cap * sizeof(int64_t));
        if (!tmp)
            return;
        ls->selected_ids = tmp;
        ls->selected_capacity = new_cap;
    }
    ls->selected_ids[ls->selected_count++] = id;
}

static int64_t txn_list_template_id(const txn_list_state_t *ls) {
    if (!ls || ls->display_count <= 0)
        return 0;
    int64_t current_id = ls->display[ls->cursor].id;
    if (ls->selected_count > 0) {
        if (txn_list_is_selected(ls, current_id))
            return current_id;
        return ls->selected_ids[0];
    }
    return current_id;
}

static int txn_list_display_index_by_id(const txn_list_state_t *ls, int64_t id) {
    if (!ls || id <= 0 || !ls->display)
        return -1;
    for (int i = 0; i < ls->display_count; i++) {
        if (ls->display[i].id == id)
            return i;
    }
    return -1;
}

static bool txn_list_apply_template_to_selected(txn_list_state_t *ls,
                                                const transaction_t *tmpl,
                                                int64_t tmpl_id,
                                                int64_t transfer_to_account_id) {
    if (!ls || !tmpl || ls->selected_count <= 0)
        return false;
    bool updated = false;
    for (int i = 0; i < ls->selected_count; i++) {
        int64_t id = ls->selected_ids[i];
        if (id == tmpl_id)
            continue;
        transaction_t txn = *tmpl;
        txn.id = id;
        if (tmpl->type != TRANSACTION_TRANSFER)
            txn.transfer_id = 0;

        int rc = 0;
        if (tmpl->type == TRANSACTION_TRANSFER) {
            if (transfer_to_account_id <= 0)
                continue;
            rc = db_update_transfer(ls->db, &txn, transfer_to_account_id);
        } else {
            rc = db_update_transaction(ls->db, &txn);
        }

        if (rc == 0)
            updated = true;
    }
    return updated;
}

static bool txn_list_apply_category_to_selected(txn_list_state_t *ls,
                                                int64_t tmpl_id,
                                                int64_t category_id) {
    if (!ls || ls->selected_count <= 0)
        return false;
    bool updated = false;
    for (int i = 0; i < ls->selected_count; i++) {
        int64_t id = ls->selected_ids[i];
        if (id == tmpl_id)
            continue;
        transaction_t txn = {0};
        int rc = db_get_transaction_by_id(ls->db, (int)id, &txn);
        if (rc != 0)
            continue;
        if (txn.type == TRANSACTION_TRANSFER)
            continue;
        txn.category_id = category_id;
        txn.transfer_id = 0;
        rc = db_update_transaction(ls->db, &txn);
        if (rc == 0)
            updated = true;
    }
    return updated;
}

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
        if (ui_requeue_resize_event(ch)) {
            confirm = false;
            done = true;
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

static void format_signed_cents(int64_t cents, bool show_plus, char *buf,
                                int buflen) {
    int64_t abs_cents = cents < 0 ? -cents : cents;
    int64_t whole = abs_cents / 100;
    int64_t frac = abs_cents % 100;

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

    if (cents < 0)
        snprintf(buf, buflen, "-%s.%02ld", formatted, (long)frac);
    else if (show_plus)
        snprintf(buf, buflen, "+%s.%02ld", formatted, (long)frac);
    else
        snprintf(buf, buflen, "%s.%02ld", formatted, (long)frac);
}

static void format_axis_date_short(const char *iso, char *buf, int buflen) {
    static const char *months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int y = 0, m = 0, d = 0;
    if (!iso || sscanf(iso, "%4d-%2d-%2d", &y, &m, &d) != 3 || m < 1 ||
        m > 12 || d < 1 || d > 31) {
        snprintf(buf, buflen, "%s", iso ? iso : "");
        return;
    }
    snprintf(buf, buflen, "%s %d", months[m - 1], d);
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
    if (contains_icase(t->reflection_date, filter))
        return true;
    if (contains_icase(t->effective_date, filter))
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
    if (contains_icase(t->payee, filter))
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
        cmp = strcmp(ta->effective_date, tb->effective_date);
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
    case SORT_PAYEE:
        cmp = strcmp(ta->payee, tb->payee);
        break;
    default:
        break;
    }

    return ls->sort_asc ? cmp : -cmp;
}

typedef struct {
    bool show_chart;
    bool chart_hidden_small;
    int chart_title_row;
    int chart_plot_row_start;
    int chart_axis_row;
    int filter_row;
    int header_row;
    int rule_row;
    int data_row_start;
} txn_layout_t;

static txn_layout_t txn_list_layout_for_window(int h, int w) {
    txn_layout_t layout = {
        .show_chart = false,
        .chart_hidden_small = false,
        .chart_title_row = 6,
        .chart_plot_row_start = 7,
        .chart_axis_row = 7 + CHART_PLOT_HEIGHT,
        .filter_row = BASE_FILTER_ROW,
        .header_row = BASE_HEADER_ROW,
        .rule_row = BASE_RULE_ROW,
        .data_row_start = BASE_DATA_ROW_START,
    };

    int chart_row_offset = CHART_PLOT_HEIGHT + 2;
    int chart_data_start = BASE_DATA_ROW_START + chart_row_offset;
    bool enough_width = (w >= CHART_MIN_WIDTH);
    bool enough_height = (h - 1 - chart_data_start >= 1);

    layout.chart_hidden_small = !enough_width || !enough_height;
    if (!layout.chart_hidden_small) {
        layout.show_chart = true;
        layout.filter_row = BASE_FILTER_ROW + chart_row_offset;
        layout.header_row = BASE_HEADER_ROW + chart_row_offset;
        layout.rule_row = BASE_RULE_ROW + chart_row_offset;
        layout.data_row_start = chart_data_start;
    }

    return layout;
}

static int txn_list_visible_rows(WINDOW *win) {
    if (!win)
        return 20;
    int h = 0;
    int w = 0;
    getmaxyx(win, h, w);
    txn_layout_t layout = txn_list_layout_for_window(h, w);
    int visible_rows = h - 1 - layout.data_row_start;
    if (visible_rows < 1)
        visible_rows = 1;
    return visible_rows;
}

static void draw_balance_chart(txn_list_state_t *ls, WINDOW *win, int w,
                               const txn_layout_t *layout) {
    if (!ls || !win || !layout || !layout->show_chart)
        return;
    if (ls->balance_series_count <= 0 || !ls->balance_series)
        return;

    int64_t min_c = ls->balance_series[0].balance_cents;
    int64_t max_c = ls->balance_series[0].balance_cents;
    for (int i = 1; i < ls->balance_series_count; i++) {
        int64_t v = ls->balance_series[i].balance_cents;
        if (v < min_c)
            min_c = v;
        if (v > max_c)
            max_c = v;
    }

    int64_t abs_min = (min_c < 0) ? -min_c : min_c;
    int64_t abs_max = (max_c < 0) ? -max_c : max_c;
    int64_t scale_abs = abs_min > abs_max ? abs_min : abs_max;
    if (scale_abs > CHART_SCALE_CAP_CENTS)
        scale_abs = CHART_SCALE_CAP_CENTS;
    if (scale_abs < 1)
        scale_abs = 1;

    int plot_w = w - 4;
    if (plot_w < 1)
        return;

    int plot_top = layout->chart_plot_row_start;
    int plot_bottom = plot_top + CHART_PLOT_HEIGHT - 1;
    if (plot_bottom < plot_top)
        return;

    int64_t plot_min = min_c;
    int64_t plot_max = max_c;
    if (plot_min < -scale_abs)
        plot_min = -scale_abs;
    if (plot_max > scale_abs)
        plot_max = scale_abs;

    int64_t span = plot_max - plot_min;
    int baseline_y = plot_bottom;
    if (plot_max <= 0) {
        baseline_y = plot_top;
    } else if (plot_min >= 0) {
        baseline_y = plot_bottom;
    } else {
        baseline_y = plot_top +
                     (int)((plot_max * (CHART_PLOT_HEIGHT - 1) + span / 2) / span);
    }
    if (baseline_y < plot_top)
        baseline_y = plot_top;
    if (baseline_y > plot_bottom)
        baseline_y = plot_bottom;

    wattron(win, A_DIM);
    mvwhline(win, baseline_y, 2, ACS_HLINE, plot_w);
    wattroff(win, A_DIM);

    int rows_up = baseline_y - plot_top;
    int rows_down = plot_bottom - baseline_y;
    int64_t pos_max = (plot_max > 0) ? plot_max : 0;
    int64_t neg_abs_max = (plot_min < 0) ? -plot_min : 0;

    // Braille-column partials (4 levels per row) for a thinner btop-like look.
    // Up bars fill from bottom upward; down bars fill from top downward.
    static const char *up_level[5] = {" ", "⡀", "⡄", "⡆", "⡇"};
    static const char *down_level[5] = {" ", "⠁", "⠃", "⠇", "⡇"};

    int pair_width = 2; // Render each sampled point as two adjacent bars.
    int bar_count = (plot_w + pair_width - 1) / pair_width;
    if (bar_count < 1)
        bar_count = 1;

    for (int b = 0; b < bar_count; b++) {
        int col = 2 + b * pair_width;
        if (col >= w - 1)
            break;

        int idx = 0;
        if (bar_count > 1 && ls->balance_series_count > 1) {
            idx = (b * (ls->balance_series_count - 1)) / (bar_count - 1);
        }
        int64_t v = ls->balance_series[idx].balance_cents;
        if (v > scale_abs)
            v = scale_abs;
        if (v < -scale_abs)
            v = -scale_abs;

        if (v >= 0 && rows_up > 0 && pos_max > 0) {
            int units_per_row = 4;
            int units_total = rows_up * units_per_row;
            long double scaled =
                ((long double)v * (long double)units_total) /
                (long double)pos_max;
            int64_t units = (int64_t)(scaled + 0.5L);
            if (units < 1 && v > 0)
                units = 1;
            wattron(win, COLOR_PAIR(COLOR_INCOME));
            for (int r = 0; r < rows_up; r++) {
                int64_t remain = units - (int64_t)r * units_per_row;
                if (remain <= 0)
                    break;
                int level = (remain >= units_per_row) ? units_per_row : (int)remain;
                int y = baseline_y - 1 - r;
                for (int dx = 0; dx < pair_width; dx++) {
                    int x = col + dx;
                    if (x >= 2 + plot_w)
                        break;
                    mvwaddstr(win, y, x, up_level[level]);
                }
            }
            wattroff(win, COLOR_PAIR(COLOR_INCOME));
        } else if (v < 0 && rows_down > 0 && neg_abs_max > 0) {
            int64_t absv = -v;
            int units_per_row = 4;
            int units_total = rows_down * units_per_row;
            long double scaled =
                ((long double)absv * (long double)units_total) /
                (long double)neg_abs_max;
            int64_t units = (int64_t)(scaled + 0.5L);
            if (units < 1)
                units = 1;
            wattron(win, COLOR_PAIR(COLOR_EXPENSE));
            for (int r = 0; r < rows_down; r++) {
                int64_t remain = units - (int64_t)r * units_per_row;
                if (remain <= 0)
                    break;
                int level = (remain >= units_per_row) ? units_per_row : (int)remain;
                int y = baseline_y + 1 + r;
                for (int dx = 0; dx < pair_width; dx++) {
                    int x = col + dx;
                    if (x >= 2 + plot_w)
                        break;
                    mvwaddstr(win, y, x, down_level[level]);
                }
            }
            wattroff(win, COLOR_PAIR(COLOR_EXPENSE));
        }
    }

    char start_label[16];
    format_axis_date_short(ls->balance_series[0].date, start_label,
                           sizeof(start_label));
    const char *end_label = "Today";
    int start_len = (int)strlen(start_label);
    int end_len = (int)strlen(end_label);
    int end_col = w - 2 - end_len;
    if (end_col < 2)
        end_col = 2;
    mvwprintw(win, layout->chart_axis_row, 2, "%s", start_label);
    if (end_col > 2 + start_len + 1)
        mvwprintw(win, layout->chart_axis_row, end_col, "%s", end_label);
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
    free(ls->balance_series);
    ls->balance_series = NULL;
    ls->balance_series_count = 0;
    ls->cursor = (ls->next_reload_cursor >= 0) ? ls->next_reload_cursor : 0;
    ls->scroll_offset = 0;
    txn_list_clear_selected(ls);

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

        if (db_get_account_balance_cents(ls->db, acct_id, &ls->balance_cents) <
            0)
            ls->balance_cents = 0;
        ls->balance_series_count = db_get_account_balance_series(
            ls->db, acct_id, BALANCE_CHART_LOOKBACK_DAYS, &ls->balance_series);
        if (ls->balance_series_count < 0) {
            ls->balance_series_count = 0;
            free(ls->balance_series);
            ls->balance_series = NULL;
        }
        if (db_get_account_month_net_cents(ls->db, acct_id,
                                           &ls->month_net_cents) < 0)
            ls->month_net_cents = 0;
        if (db_get_account_month_income_cents(ls->db, acct_id,
                                              &ls->month_income_cents) < 0)
            ls->month_income_cents = 0;
        if (db_get_account_month_expense_cents(ls->db, acct_id,
                                               &ls->month_expense_cents) < 0)
            ls->month_expense_cents = 0;
    } else {
        ls->balance_cents = 0;
        ls->balance_series_count = 0;
        ls->month_net_cents = 0;
        ls->month_income_cents = 0;
        ls->month_expense_cents = 0;
    }

    ls->dirty = false;
    rebuild_display(ls);
    if (ls->next_reload_focus_txn_id > 0) {
        int idx =
            txn_list_display_index_by_id(ls, ls->next_reload_focus_txn_id);
        if (idx >= 0) {
            ls->cursor = idx;
            ls->center_cursor_next_draw = true;
        }
    }
    if (ls->cursor < 0)
        ls->cursor = 0;
    if (ls->cursor >= ls->display_count)
        ls->cursor = ls->display_count > 0 ? ls->display_count - 1 : 0;
    ls->next_reload_cursor = -1;
    ls->next_reload_focus_txn_id = 0;
}

txn_list_state_t *txn_list_create(sqlite3 *db) {
    txn_list_state_t *ls = calloc(1, sizeof(*ls));
    if (!ls)
        return NULL;
    ls->db = db;
    ls->sort_col = SORT_DATE;
    ls->sort_asc = false;
    ls->next_reload_cursor = -1;
    ls->next_reload_focus_txn_id = 0;
    ls->center_cursor_next_draw = false;
    ls->dirty = true;
    return ls;
}

void txn_list_destroy(txn_list_state_t *ls) {
    if (!ls)
        return;
    free(ls->accounts);
    free(ls->transactions);
    free(ls->display);
    free(ls->balance_series);
    free(ls->selected_ids);
    free(ls);
}

void txn_list_draw(txn_list_state_t *ls, WINDOW *win, bool focused) {
    if (ls->dirty)
        reload(ls);

    int h, w;
    getmaxyx(win, h, w);
    txn_layout_t layout = txn_list_layout_for_window(h, w);

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

    // -- Summary line (centered, row 4) --
    char balance_buf[24];
    char month_net_buf[24];
    char month_income_buf[24];
    char month_expense_buf[24];
    format_signed_cents(ls->balance_cents, false, balance_buf,
                        sizeof(balance_buf));
    format_signed_cents(ls->month_net_cents, true, month_net_buf,
                        sizeof(month_net_buf));
    format_signed_cents(ls->month_income_cents, false, month_income_buf,
                        sizeof(month_income_buf));
    format_signed_cents(-ls->month_expense_cents, false, month_expense_buf,
                        sizeof(month_expense_buf));

    char summary_text[256];
    int summary_len = snprintf(
        summary_text, sizeof(summary_text),
        "Balance %s   MTD net %s   MTD income %s   MTD expenses %s",
        balance_buf, month_net_buf, month_income_buf, month_expense_buf);
    if (summary_len < 0)
        summary_len = 0;

    if (summary_len > w - 4) {
        summary_len =
            snprintf(summary_text, sizeof(summary_text),
                     "Balance %s   MTD net %s   MTD income %s", balance_buf,
                     month_net_buf, month_income_buf);
    }
    if (summary_len > w - 4) {
        summary_len = snprintf(summary_text, sizeof(summary_text),
                               "Balance %s   MTD net %s", balance_buf,
                               month_net_buf);
    }
    if (summary_len > w - 4) {
        summary_len =
            snprintf(summary_text, sizeof(summary_text), "Balance %s", balance_buf);
    }
    if (summary_len < 0)
        summary_len = 0;

    int summary_col = (w - summary_len) / 2;
    if (summary_col < 2)
        summary_col = 2;

    int summary_col_cur = summary_col;
    wattron(win, A_BOLD);
    mvwprintw(win, SUMMARY_ROW, summary_col_cur, "Balance ");
    wattroff(win, A_BOLD);
    summary_col_cur += 8;
    int balance_color = (ls->balance_cents < 0) ? COLOR_EXPENSE : COLOR_INCOME;
    wattron(win, COLOR_PAIR(balance_color));
    mvwprintw(win, SUMMARY_ROW, summary_col_cur, "%s", balance_buf);
    wattroff(win, COLOR_PAIR(balance_color));
    summary_col_cur += (int)strlen(balance_buf);

    if (strstr(summary_text, "MTD net")) {
        mvwprintw(win, SUMMARY_ROW, summary_col_cur, "   ");
        summary_col_cur += 3;
        wattron(win, A_BOLD);
        mvwprintw(win, SUMMARY_ROW, summary_col_cur, "MTD net ");
        wattroff(win, A_BOLD);
        summary_col_cur += 8;
        int month_color =
            (ls->month_net_cents < 0) ? COLOR_EXPENSE : COLOR_INCOME;
        wattron(win, COLOR_PAIR(month_color));
        mvwprintw(win, SUMMARY_ROW, summary_col_cur, "%s", month_net_buf);
        wattroff(win, COLOR_PAIR(month_color));
        summary_col_cur += (int)strlen(month_net_buf);
    }

    if (strstr(summary_text, "MTD income")) {
        mvwprintw(win, SUMMARY_ROW, summary_col_cur, "   ");
        summary_col_cur += 3;
        wattron(win, A_BOLD);
        mvwprintw(win, SUMMARY_ROW, summary_col_cur, "MTD income ");
        wattroff(win, A_BOLD);
        summary_col_cur += 11;
        wattron(win, COLOR_PAIR(COLOR_INCOME));
        mvwprintw(win, SUMMARY_ROW, summary_col_cur, "%s", month_income_buf);
        wattroff(win, COLOR_PAIR(COLOR_INCOME));
        summary_col_cur += (int)strlen(month_income_buf);
    }

    if (strstr(summary_text, "MTD expenses")) {
        mvwprintw(win, SUMMARY_ROW, summary_col_cur, "   ");
        summary_col_cur += 3;
        wattron(win, A_BOLD);
        mvwprintw(win, SUMMARY_ROW, summary_col_cur, "MTD expenses ");
        wattroff(win, A_BOLD);
        summary_col_cur += 13;
        wattron(win, COLOR_PAIR(COLOR_EXPENSE));
        mvwprintw(win, SUMMARY_ROW, summary_col_cur, "%s", month_expense_buf);
        wattroff(win, COLOR_PAIR(COLOR_EXPENSE));
    }

    if (layout.show_chart) {
        draw_balance_chart(ls, win, w, &layout);
    } else if (layout.chart_hidden_small && ls->account_count > 0) {
        if (w > 4) {
            mvwprintw(win, 6, 2, "%-*.*s", w - 4, w - 4,
                      "Balance chart hidden (window too small)");
        }
    }

    // -- Filter bar: shown when active or text is present --
    if (ls->filter_active || ls->filter_len > 0) {
        if (ls->filter_active)
            wattron(win, A_BOLD);
        mvwprintw(win, layout.filter_row, 2, "Filter: %s", ls->filter_buf);
        if (ls->filter_active)
            wattroff(win, A_BOLD);
    }
    if (ls->selected_count > 0) {
        const char *bulk_msg = "Bulk edit mode (Esc clears)";
        int bulk_len = (int)strlen(bulk_msg);
        int bulk_col = w - bulk_len - 2;
        if (bulk_col < 2)
            bulk_col = 2;
        wattron(win, COLOR_PAIR(COLOR_INFO));
        mvwprintw(win, layout.filter_row, bulk_col, "%s", bulk_msg);
        wattroff(win, COLOR_PAIR(COLOR_INFO));
    }

    // -- Column headers with sort direction indicator --
    int posted_col = 2;
    int reflection_col = posted_col + DATE_COL_WIDTH + GAP_WIDTH;
    int type_col = reflection_col + REFLECTION_DATE_COL_WIDTH + GAP_WIDTH;
    int category_col = type_col + TYPE_COL_WIDTH + GAP_WIDTH;
    int amount_col = category_col + CATEGORY_COL_WIDTH + GAP_WIDTH;
    int payee_col = amount_col + AMOUNT_COL_WIDTH + GAP_WIDTH;
    int desc_col = payee_col + PAYEE_COL_WIDTH + GAP_WIDTH;
    int desc_w = w - 2 - desc_col;
    if (desc_w < DESC_COL_MIN_WIDTH)
        desc_w = DESC_COL_MIN_WIDTH;

    const char *posted_label = "Posted";
    const char *reflection_label = "Reflect";
    const char *type_label = "Type";
    const char *category_label = "Category";
    const char *amount_label = "Amount";
    const char *payee_label = "Payee";
    const char *description_label = "Description";

    const char *ind_str = ls->sort_asc ? "\u2191" : "\u2193";
    wattron(win, A_BOLD);
    mvwprintw(win, layout.header_row, posted_col, "%-*s", DATE_COL_WIDTH,
              posted_label);
    mvwprintw(win, layout.header_row, reflection_col, "%-*s",
              REFLECTION_DATE_COL_WIDTH, reflection_label);
    mvwprintw(win, layout.header_row, type_col, "%-*s", TYPE_COL_WIDTH,
              type_label);
    mvwprintw(win, layout.header_row, category_col, "%-*s", CATEGORY_COL_WIDTH,
              category_label);
    mvwprintw(win, layout.header_row, amount_col, "%-*s", AMOUNT_COL_WIDTH,
              amount_label);
    mvwprintw(win, layout.header_row, payee_col, "%-*s", PAYEE_COL_WIDTH,
              payee_label);
    mvwprintw(win, layout.header_row, desc_col, "%-*s", desc_w,
              description_label);

    int indicator_col = -1;
    switch (ls->sort_col) {
    case SORT_DATE:
        indicator_col = reflection_col + (int)strlen(reflection_label);
        break;
    case SORT_TYPE:
        indicator_col = type_col + (int)strlen(type_label);
        break;
    case SORT_CATEGORY:
        indicator_col = category_col + (int)strlen(category_label);
        break;
    case SORT_AMOUNT:
        indicator_col = amount_col + (int)strlen(amount_label);
        break;
    case SORT_PAYEE:
        indicator_col = payee_col + (int)strlen(payee_label);
        break;
    case SORT_DESCRIPTION:
        indicator_col = desc_col + (int)strlen(description_label);
        break;
    default:
        break;
    }
    if (indicator_col >= 2 && indicator_col < w - 1)
        mvwprintw(win, layout.header_row, indicator_col, "%s", ind_str);
    wattroff(win, A_BOLD);

    // -- Horizontal rule --
    wmove(win, layout.rule_row, 2);
    for (int i = 2; i < w - 2; i++)
        waddch(win, ACS_HLINE);

    // -- Data rows --
    int visible_rows = h - 1 - layout.data_row_start;
    if (visible_rows < 1)
        visible_rows = 1;

    if (ls->display_count == 0) {
        ls->center_cursor_next_draw = false;
        const char *msg =
            (ls->filter_len > 0) ? "No matches" : "No transactions";
        int mlen = (int)strlen(msg);
        int row = layout.data_row_start + visible_rows / 2;
        if (row >= h - 1)
            row = layout.data_row_start;
        mvwprintw(win, row, (w - mlen) / 2, "%s", msg);
        if (ls->filter_active)
            wmove(win, layout.filter_row, 2 + 8 + ls->filter_len);
        return;
    }

    // Clamp cursor/scroll
    if (ls->cursor < 0)
        ls->cursor = 0;
    if (ls->cursor >= ls->display_count)
        ls->cursor = ls->display_count - 1;

    if (ls->center_cursor_next_draw) {
        int max_offset = ls->display_count - visible_rows;
        if (max_offset < 0)
            max_offset = 0;
        int target = ls->cursor - visible_rows / 2;
        if (target < 0)
            target = 0;
        if (target > max_offset)
            target = max_offset;
        ls->scroll_offset = target;
        ls->center_cursor_next_draw = false;
    }

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
        int row = layout.data_row_start + i;

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

        bool cursor_selected = (idx == ls->cursor);
        bool row_selected = txn_list_is_selected(ls, t->id);
        if (cursor_selected) {
            if (!focused)
                wattron(win, A_DIM);
            wattron(win, A_REVERSE);
        }

        mvwaddch(win, row, 1, row_selected ? '*' : ' ');

        if (w > 4)
            mvwprintw(win, row, 2, "%-*s", w - 4, "");

        mvwprintw(win, row, posted_col, "%-*s", DATE_COL_WIDTH, t->date);
        mvwprintw(win, row, reflection_col, "%-*s", REFLECTION_DATE_COL_WIDTH,
                  t->reflection_date);
        mvwprintw(win, row, type_col, "%-*s", TYPE_COL_WIDTH, type_str);
        mvwprintw(win, row, category_col, "%-*.*s", CATEGORY_COL_WIDTH,
                  CATEGORY_COL_WIDTH, t->category_name);

        // Amount with color
        int color =
            (t->type == TRANSACTION_EXPENSE) ? COLOR_EXPENSE : COLOR_INCOME;
        wattron(win, COLOR_PAIR(color));
        mvwprintw(win, row, amount_col, "%*s", AMOUNT_COL_WIDTH, amt);
        wattroff(win, COLOR_PAIR(color));

        mvwprintw(win, row, payee_col, "%-*.*s", PAYEE_COL_WIDTH,
                  PAYEE_COL_WIDTH, t->payee);

        mvwprintw(win, row, desc_col, "%-*.*s", desc_w, desc_w, t->description);

        if (cursor_selected) {
            wattroff(win, A_REVERSE);
            if (!focused)
                wattroff(win, A_DIM);
        }
    }

    // Place terminal cursor in filter bar when active
    if (ls->filter_active)
        wmove(win, layout.filter_row, 2 + 8 + ls->filter_len);
}

bool txn_list_handle_input(txn_list_state_t *ls, WINDOW *parent, int ch) {
    int visible_rows = txn_list_visible_rows(parent);
    int half_rows = visible_rows / 2;
    if (half_rows < 1)
        half_rows = 1;

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
    case 27: // Esc clears bulk selection
        if (ls->selected_count > 0) {
            txn_list_clear_selected(ls);
            return true;
        }
        break;
    case KEY_UP:
    case 'k':
        if (ls->cursor > 0)
            ls->cursor--;
        return true;
    case KEY_SR:
        if (ls->display_count <= 0)
            return true;
        txn_list_select_id(ls, ls->display[ls->cursor].id);
        if (ls->cursor > 0)
            ls->cursor--;
        txn_list_select_id(ls, ls->display[ls->cursor].id);
        return true;
    case KEY_DOWN:
    case 'j':
        if (ls->cursor < ls->display_count - 1)
            ls->cursor++;
        return true;
    case KEY_SF:
        if (ls->display_count <= 0)
            return true;
        txn_list_select_id(ls, ls->display[ls->cursor].id);
        if (ls->cursor < ls->display_count - 1)
            ls->cursor++;
        txn_list_select_id(ls, ls->display[ls->cursor].id);
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
    case 4: // Ctrl-D: half-page down
        ls->cursor += half_rows;
        if (ls->cursor >= ls->display_count)
            ls->cursor = ls->display_count > 0 ? ls->display_count - 1 : 0;
        return true;
    case 21: // Ctrl-U: half-page up
        ls->cursor -= half_rows;
        if (ls->cursor < 0)
            ls->cursor = 0;
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
            int64_t tmpl_id = txn_list_template_id(ls);
            if (tmpl_id <= 0)
                return true;
            transaction_t txn = {0};
            int rc = db_get_transaction_by_id(ls->db, (int)tmpl_id, &txn);
            if (rc == 0) {
                form_result_t res =
                    form_transaction(parent, ls->db, &txn, true);
                if (res == FORM_SAVED) {
                    int64_t to_account_id = 0;
                    if (txn.type == TRANSACTION_TRANSFER) {
                        if (db_get_transfer_counterparty_account(
                                ls->db, txn.id, &to_account_id) < 0) {
                            to_account_id = 0;
                        }
                    }
                    txn_list_apply_template_to_selected(
                        ls, &txn, tmpl_id, to_account_id);
                    txn_list_clear_selected(ls);
                    ls->next_reload_focus_txn_id = tmpl_id;
                    ls->dirty = true;
                }
            } else {
                ls->dirty = true;
            }
        }
        return true;
    case 'c':
        if (ls->display_count <= 0)
            return true;
        {
            int64_t tmpl_id = txn_list_template_id(ls);
            if (tmpl_id <= 0)
                return true;
            transaction_t txn = {0};
            int rc = db_get_transaction_by_id(ls->db, (int)tmpl_id, &txn);
            if (rc == 0 && txn.type != TRANSACTION_TRANSFER) {
                form_result_t res = form_transaction_category(parent, ls->db, &txn);
                if (res == FORM_SAVED) {
                    txn_list_apply_category_to_selected(
                        ls, tmpl_id, txn.category_id);
                    txn_list_clear_selected(ls);
                    ls->next_reload_focus_txn_id = tmpl_id;
                    ls->dirty = true;
                }
            } else if (rc != 0) {
                ls->dirty = true;
            }
        }
        return true;
    case ' ':
        if (ls->display_count <= 0)
            return true;
        {
            int64_t id = ls->display[ls->cursor].id;
            txn_list_toggle_selected(ls, id);
        }
        return true;
    case 'd':
        if (ls->display_count <= 0)
            return true;
        if (confirm_delete(parent)) {
            int delete_idx = ls->cursor;
            int rc =
                db_delete_transaction(ls->db, (int)ls->display[ls->cursor].id);
            if (rc == 0) {
                ls->next_reload_cursor = delete_idx;
                ls->next_reload_focus_txn_id = 0;
                ls->dirty = true;
            } else if (rc == -2) {
                ls->next_reload_cursor = delete_idx;
                ls->next_reload_focus_txn_id = 0;
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

    return false;
}

const char *txn_list_status_hint(const txn_list_state_t *ls) {
    static char buf[256];

    if (ls->filter_active)
        return "Type to filter  Enter:done  Esc:clear";

    if (ls->txn_count == 0)
        return "90d chart  1-9 acct  a add  /filter  s sort  \u2190 back";

    const char *filter_tag = ls->filter_len > 0 ? "/filter[on]" : "/filter";
    if (ls->selected_count > 0) {
        snprintf(buf, sizeof(buf),
                 "%d selected  90d chart  \u2191\u2193 move  ^d/^u half-page  space select  e edit  c category  d delete  %s  s sort  S dir  1-9 acct "
                 " a add  \u2190 back",
                 ls->selected_count, filter_tag);
    } else {
        snprintf(buf, sizeof(buf),
                 "90d chart  \u2191\u2193 move  ^d/^u half-page  space select  e edit  c category  d delete  %s  s sort  S dir  1-9 acct "
                 " a add  \u2190 back",
                 filter_tag);
    }
    return buf;
}

void txn_list_mark_dirty(txn_list_state_t *ls) {
    if (ls)
        ls->dirty = true;
}

int64_t txn_list_get_current_account_id(const txn_list_state_t *ls) {
    if (!ls || !ls->accounts || ls->account_count == 0)
        return 0;
    return ls->accounts[ls->account_sel].id;
}
