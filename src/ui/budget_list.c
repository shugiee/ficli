#include "ui/budget_list.h"

#include "db/query.h"
#include "ui/colors.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    budget_row_t row;
    bool is_parent;
} budget_display_row_t;

struct budget_list_state {
    sqlite3 *db;
    char month[8]; // "YYYY-MM"

    budget_display_row_t *rows;
    int row_count;
    int row_capacity;

    int cursor;
    int scroll_offset;

    bool edit_mode;
    char edit_buf[32];
    int edit_pos;

    budget_txn_row_t *related_txns;
    int related_txn_count;
    bool related_visible;
    int64_t related_category_id;
    char related_category_name[64];

    char message[128];
    bool dirty;
};

static void set_current_month(char out[8]) {
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(out, 8, "%Y-%m", &tmv);
}

static bool parse_month_ym(const char *month, int *out_y, int *out_m) {
    if (!month || strlen(month) != 7 || month[4] != '-')
        return false;
    int y = 0;
    int m = 0;
    if (sscanf(month, "%4d-%2d", &y, &m) != 2)
        return false;
    if (y < 1900 || m < 1 || m > 12)
        return false;
    if (out_y)
        *out_y = y;
    if (out_m)
        *out_m = m;
    return true;
}

static bool month_shift(char month[8], int delta) {
    int y = 0;
    int m = 0;
    if (!parse_month_ym(month, &y, &m))
        return false;

    struct tm tmv = {0};
    tmv.tm_year = y - 1900;
    tmv.tm_mon = (m - 1) + delta;
    tmv.tm_mday = 1;
    tmv.tm_hour = 12;
    tmv.tm_isdst = -1;
    if (mktime(&tmv) == (time_t)-1)
        return false;

    if (strftime(month, 8, "%Y-%m", &tmv) != 7)
        return false;
    return true;
}

static void format_cents_plain(int64_t cents, bool show_plus, char *buf, int n) {
    int64_t abs_cents = cents < 0 ? -cents : cents;
    int64_t whole = abs_cents / 100;
    int64_t frac = abs_cents % 100;

    char raw[32];
    snprintf(raw, sizeof(raw), "%ld", (long)whole);
    int rawlen = (int)strlen(raw);

    char grouped[48];
    int gi = 0;
    for (int i = 0; i < rawlen; i++) {
        if (i > 0 && (rawlen - i) % 3 == 0)
            grouped[gi++] = ',';
        grouped[gi++] = raw[i];
    }
    grouped[gi] = '\0';

    if (cents < 0)
        snprintf(buf, n, "-%s.%02ld", grouped, (long)frac);
    else if (show_plus)
        snprintf(buf, n, "+%s.%02ld", grouped, (long)frac);
    else
        snprintf(buf, n, "%s.%02ld", grouped, (long)frac);
}

static void format_budget_value(int64_t cents, char *buf, int n) {
    int64_t whole = cents / 100;
    int64_t frac = cents % 100;
    if (frac < 0)
        frac = -frac;
    if (whole < 0)
        whole = -whole;
    snprintf(buf, n, "%ld.%02ld", (long)whole, (long)frac);
}

static bool parse_budget_input_cents(const char *buf, int64_t *out_cents) {
    if (!buf || !out_cents)
        return false;

    while (isspace((unsigned char)*buf))
        buf++;
    if (*buf == '\0')
        return false;

    int64_t whole = 0;
    int64_t frac = 0;
    int frac_digits = 0;
    bool seen_dot = false;
    bool seen_digit = false;

    for (const char *p = buf; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (isspace(c))
            continue;
        if (c == '.') {
            if (seen_dot)
                return false;
            seen_dot = true;
            continue;
        }
        if (!isdigit(c))
            return false;

        seen_digit = true;
        int d = c - '0';
        if (!seen_dot) {
            if (whole > (INT64_MAX - d) / 10)
                return false;
            whole = whole * 10 + d;
        } else {
            if (frac_digits >= 2)
                return false;
            frac = frac * 10 + d;
            frac_digits++;
        }
    }

    if (!seen_digit)
        return false;
    if (frac_digits == 1)
        frac *= 10;

    if (whole > (INT64_MAX - frac) / 100)
        return false;
    *out_cents = whole * 100 + frac;
    return true;
}

static int append_row(budget_list_state_t *ls, const budget_row_t *row,
                      bool is_parent) {
    if (!ls || !row)
        return -1;
    if (ls->row_count >= ls->row_capacity) {
        int next_capacity = (ls->row_capacity <= 0) ? 16 : ls->row_capacity * 2;
        budget_display_row_t *tmp =
            realloc(ls->rows, (size_t)next_capacity * sizeof(*tmp));
        if (!tmp)
            return -1;
        ls->rows = tmp;
        ls->row_capacity = next_capacity;
    }
    ls->rows[ls->row_count].row = *row;
    ls->rows[ls->row_count].is_parent = is_parent;
    ls->row_count++;
    return 0;
}

static void clear_related_transactions(budget_list_state_t *ls) {
    if (!ls)
        return;
    free(ls->related_txns);
    ls->related_txns = NULL;
    ls->related_txn_count = 0;
}

static void refresh_related_transactions(budget_list_state_t *ls) {
    if (!ls || !ls->related_visible || ls->related_category_id <= 0)
        return;

    budget_txn_row_t *related = NULL;
    int related_count = db_get_budget_transactions_for_month(
        ls->db, ls->related_category_id, ls->month, &related);
    if (related_count < 0) {
        snprintf(ls->message, sizeof(ls->message),
                 "Error loading matching transactions");
        return;
    }

    clear_related_transactions(ls);
    ls->related_txns = related;
    ls->related_txn_count = related_count;
}

static void reload_rows(budget_list_state_t *ls) {
    if (!ls)
        return;

    free(ls->rows);
    ls->rows = NULL;
    ls->row_count = 0;
    ls->row_capacity = 0;

    budget_row_t *parents = NULL;
    int parent_count = db_get_budget_rows_for_month(ls->db, ls->month, &parents);
    if (parent_count < 0) {
        snprintf(ls->message, sizeof(ls->message), "Error loading budgets");
        ls->dirty = false;
        return;
    }

    for (int i = 0; i < parent_count; i++) {
        if (append_row(ls, &parents[i], true) < 0) {
            snprintf(ls->message, sizeof(ls->message), "Out of memory");
            break;
        }

        budget_row_t *children = NULL;
        int child_count = db_get_budget_child_rows_for_month(
            ls->db, parents[i].category_id, ls->month, &children);
        if (child_count < 0)
            continue;

        for (int j = 0; j < child_count; j++) {
            if (append_row(ls, &children[j], false) < 0) {
                snprintf(ls->message, sizeof(ls->message), "Out of memory");
                break;
            }
        }
        free(children);
    }

    free(parents);

    if (ls->row_count <= 0) {
        ls->cursor = 0;
        ls->scroll_offset = 0;
    } else if (ls->cursor >= ls->row_count) {
        ls->cursor = ls->row_count - 1;
    } else if (ls->cursor < 0) {
        ls->cursor = 0;
    }

    if (ls->related_visible && ls->related_category_id > 0) {
        for (int i = 0; i < ls->row_count; i++) {
            if (ls->rows[i].row.category_id == ls->related_category_id) {
                snprintf(ls->related_category_name,
                         sizeof(ls->related_category_name), "%s",
                         ls->rows[i].row.category_name);
                break;
            }
        }
        refresh_related_transactions(ls);
    }

    ls->dirty = false;
}

budget_list_state_t *budget_list_create(sqlite3 *db) {
    budget_list_state_t *ls = calloc(1, sizeof(*ls));
    if (!ls)
        return NULL;
    ls->db = db;
    set_current_month(ls->month);
    ls->dirty = true;
    return ls;
}

void budget_list_destroy(budget_list_state_t *ls) {
    if (!ls)
        return;
    free(ls->rows);
    clear_related_transactions(ls);
    free(ls);
}

static int row_color_pair(const budget_display_row_t *row) {
    if (!row)
        return COLOR_NORMAL;
    int util = row->row.utilization_bps;
    if (util < 0)
        return COLOR_NORMAL;
    if (util <= 10000)
        return COLOR_INCOME;
    if (util <= 12500)
        return COLOR_WARNING;
    return COLOR_EXPENSE;
}

static void draw_bar(WINDOW *win, int row, int col, int width,
                     const budget_display_row_t *drow) {
    if (!win || !drow || width <= 0)
        return;

    static const char *bar_fill = "◼";
    const int max_bps = 15000;
    const int warn_bps = 10000;
    const int danger_bps = 12500;

    int util = drow->row.utilization_bps;
    for (int i = 0; i < width; i++)
        mvwaddch(win, row, col + i, ' ');

    if (util < 0)
        return;

    int clamped_util = util;
    if (clamped_util > max_bps)
        clamped_util = max_bps;

    int green_bps = clamped_util < warn_bps ? clamped_util : warn_bps;
    int yellow_bps = clamped_util < danger_bps ? clamped_util : danger_bps;
    int red_bps = clamped_util;

    int green_cols =
        (int)(((int64_t)green_bps * width + max_bps - 1) / max_bps);
    int yellow_cols =
        (int)(((int64_t)yellow_bps * width + max_bps - 1) / max_bps);
    int red_cols = (int)(((int64_t)red_bps * width + max_bps - 1) / max_bps);

    if (green_bps <= 0)
        green_cols = 0;
    if (yellow_bps <= 0)
        yellow_cols = 0;
    if (red_bps <= 0)
        red_cols = 0;

    if (green_cols > width)
        green_cols = width;
    if (yellow_cols > width)
        yellow_cols = width;
    if (red_cols > width)
        red_cols = width;

    if (yellow_cols < green_cols)
        yellow_cols = green_cols;
    if (red_cols < yellow_cols)
        red_cols = yellow_cols;

    if (green_cols > 0) {
        wattron(win, COLOR_PAIR(COLOR_INCOME));
        for (int i = 0; i < green_cols; i++)
            mvwaddstr(win, row, col + i, bar_fill);
        wattroff(win, COLOR_PAIR(COLOR_INCOME));
    }

    if (yellow_cols > green_cols) {
        wattron(win, COLOR_PAIR(COLOR_WARNING));
        for (int i = green_cols; i < yellow_cols; i++)
            mvwaddstr(win, row, col + i, bar_fill);
        wattroff(win, COLOR_PAIR(COLOR_WARNING));
    }

    if (red_cols > yellow_cols) {
        wattron(win, COLOR_PAIR(COLOR_EXPENSE));
        for (int i = yellow_cols; i < red_cols; i++)
            mvwaddstr(win, row, col + i, bar_fill);
        wattroff(win, COLOR_PAIR(COLOR_EXPENSE));
    }
}

static void format_related_amount(const budget_txn_row_t *txn, char *buf, int n) {
    if (!txn || !buf || n <= 0)
        return;

    int64_t signed_cents = txn->amount_cents;
    if (txn->type == TRANSACTION_EXPENSE)
        signed_cents = -signed_cents;
    format_cents_plain(signed_cents, false, buf, n);
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

static void draw_related_transactions_section(budget_list_state_t *ls, WINDOW *win,
                                              int left, int avail,
                                              int separator_row, int title_row,
                                              int header_row, int rule_row,
                                              int data_row_start,
                                              int visible_rows) {
    if (!ls || !win || visible_rows <= 0)
        return;

    for (int col = left; col < left + avail; col++)
        mvwaddch(win, separator_row, col, ACS_HLINE);

    int data_row_end = data_row_start + visible_rows - 1;
    for (int row = title_row; row <= data_row_end; row++)
        mvwprintw(win, row, left, "%-*s", avail, "");

    char title[256];
    if (!ls->related_visible) {
        snprintf(title, sizeof(title),
                 "Matching Transactions (press Enter on a budget row)");
    } else if (ls->related_txn_count > visible_rows) {
        snprintf(title, sizeof(title),
                 "Matching Transactions - %s (%d total, showing %d)",
                 ls->related_category_name, ls->related_txn_count, visible_rows);
    } else {
        snprintf(title, sizeof(title), "Matching Transactions - %s (%d)",
                 ls->related_category_name, ls->related_txn_count);
    }

    wattron(win, A_BOLD);
    mvwprintw(win, title_row, left, "%-*.*s", avail, avail, title);
    wattroff(win, A_BOLD);

    int date_w = 10;
    int amount_w = 12;
    int account_w = 12;
    int gap_count = 3;
    int details_w = avail - date_w - amount_w - account_w - gap_count;
    if (details_w < 10) {
        account_w = 8;
        details_w = avail - date_w - amount_w - account_w - gap_count;
    }
    if (details_w < 10) {
        account_w = 0;
        gap_count = 2;
        details_w = avail - date_w - amount_w - gap_count;
    }
    if (details_w < 6)
        details_w = 6;

    int date_col = left;
    int amount_col = date_col + date_w + 1;
    int account_col = amount_col + amount_w + 1;
    int details_col = account_col;
    if (account_w > 0)
        details_col = account_col + account_w + 1;

    wattron(win, A_BOLD);
    mvwprintw(win, header_row, date_col, "%-*s", date_w, "Date");
    mvwprintw(win, header_row, amount_col, "%-*s", amount_w, "Amount");
    if (account_w > 0)
        mvwprintw(win, header_row, account_col, "%-*s", account_w, "Account");
    mvwprintw(win, header_row, details_col, "%-*s", details_w, "Details");
    wattroff(win, A_BOLD);

    for (int col = left; col < left + avail; col++)
        mvwaddch(win, rule_row, col, ACS_HLINE);

    if (!ls->related_visible) {
        wattron(win, A_DIM);
        mvwprintw(win, data_row_start, left, "Press Enter to show matching transactions");
        wattroff(win, A_DIM);
        return;
    }

    if (ls->related_txn_count <= 0) {
        wattron(win, A_DIM);
        mvwprintw(win, data_row_start, left, "No matching transactions for %s in %s",
                  ls->related_category_name, ls->month);
        wattroff(win, A_DIM);
        return;
    }

    int shown = ls->related_txn_count;
    if (shown > visible_rows)
        shown = visible_rows;

    for (int i = 0; i < shown; i++) {
        int row = data_row_start + i;
        const budget_txn_row_t *txn = &ls->related_txns[i];

        mvwprintw(win, row, date_col, "%-*.*s", date_w, date_w,
                  txn->effective_date);

        char amount_str[24];
        format_related_amount(txn, amount_str, sizeof(amount_str));
        int amount_color =
            (txn->type == TRANSACTION_EXPENSE) ? COLOR_EXPENSE : COLOR_INCOME;
        wattron(win, COLOR_PAIR(amount_color));
        mvwprintw(win, row, amount_col, "%*.*s", amount_w, amount_w, amount_str);
        wattroff(win, COLOR_PAIR(amount_color));

        if (account_w > 0)
            mvwprintw(win, row, account_col, "%-*.*s", account_w, account_w,
                      txn->account_name);

        char details[512];
        compose_related_details(txn, details, sizeof(details));
        mvwprintw(win, row, details_col, "%-*.*s", details_w, details_w, details);
    }
}

static void show_related_transactions_for_cursor(budget_list_state_t *ls) {
    if (!ls || ls->cursor < 0 || ls->cursor >= ls->row_count)
        return;

    budget_display_row_t *drow = &ls->rows[ls->cursor];
    budget_txn_row_t *related = NULL;
    int related_count = db_get_budget_transactions_for_month(
        ls->db, drow->row.category_id, ls->month, &related);
    if (related_count < 0) {
        snprintf(ls->message, sizeof(ls->message),
                 "Error loading matching transactions");
        return;
    }

    clear_related_transactions(ls);
    ls->related_txns = related;
    ls->related_txn_count = related_count;
    ls->related_visible = true;
    ls->related_category_id = drow->row.category_id;
    snprintf(ls->related_category_name, sizeof(ls->related_category_name), "%s",
             drow->row.category_name);
}

void budget_list_draw(budget_list_state_t *ls, WINDOW *win, bool focused) {
    if (!ls || !win)
        return;
    if (ls->dirty)
        reload_rows(ls);

    int h, w;
    getmaxyx(win, h, w);
    if (h < 7 || w < 44) {
        mvwprintw(win, 1, 2, "Window too small for Budgets");
        curs_set(0);
        return;
    }

    int title_row = 1;
    int msg_row = 2;
    int header_row = 4;
    int rule_row = 5;
    int data_row_start = 6;

    mvwprintw(win, title_row, 2, "Budgets  Month:%s", ls->month);
    const char *title_hint = ls->edit_mode ? "Enter:Save Esc:Cancel"
                                           : "h/l:Month  r:Now  Enter:Txns  e:Edit";
    int title_hint_col = w - 2 - (int)strlen(title_hint);
    if (title_hint_col < 2)
        title_hint_col = 2;
    mvwprintw(win, title_row, title_hint_col, "%s", title_hint);

    mvwprintw(win, msg_row, 2, "%-*s", w - 4, "");
    if (ls->message[0] != '\0')
        mvwprintw(win, msg_row, 2, "%s", ls->message);

    int left = 2;
    int avail = w - 4;
    if (avail < 20)
        return;

    int budget_w = 12;
    int net_w = 12;
    int pct_w = 7;
    int min_cat_w = 10;
    int min_bar_w = 10; // 150% / 10 bars = 15% per bar max
    int cat_w = avail / 3;
    if (cat_w < min_cat_w)
        cat_w = min_cat_w;
    if (cat_w > 28)
        cat_w = 28;
    int bar_w = avail - cat_w - budget_w - net_w - pct_w - 4;
    if (bar_w < min_bar_w) {
        int needed = min_bar_w - bar_w;
        cat_w -= needed;
        if (cat_w < min_cat_w)
            cat_w = min_cat_w;
        bar_w = avail - cat_w - budget_w - net_w - pct_w - 4;
    }
    if (bar_w < 0)
        bar_w = 0;

    int category_col = left;
    int budget_col = category_col + cat_w + 1;
    int net_col = budget_col + budget_w + 1;
    int pct_col = net_col + net_w + 1;
    int bar_col = pct_col + pct_w + 1;

    wattron(win, A_BOLD);
    mvwprintw(win, header_row, category_col, "%-*s", cat_w, "Category");
    mvwprintw(win, header_row, budget_col, "%-*s", budget_w, "Budget");
    mvwprintw(win, header_row, net_col, "%-*s", net_w, "Net");
    mvwprintw(win, header_row, pct_col, "%-*s", pct_w, "%");
    if (bar_w > 0)
        mvwprintw(win, header_row, bar_col, "%-*s", bar_w, "Progress");
    wattroff(win, A_BOLD);

    for (int col = left; col < w - 2; col++)
        mvwaddch(win, rule_row, col, ACS_HLINE);

    int body_start_row = data_row_start;
    int body_end_row = h - 2;
    int body_rows = body_end_row - body_start_row + 1;
    if (body_rows < 1)
        body_rows = 1;

    int visible_rows = body_rows;
    bool show_related_section = false;
    int related_sep_row = 0;
    int related_title_row = 0;
    int related_header_row = 0;
    int related_rule_row = 0;
    int related_data_row_start = 0;
    int related_visible_rows = 0;
    const int related_gap_rows = 5;
    const int related_fixed_rows = 4; // separator + title + header + rule
    const int related_min_data_rows = 1;
    const int related_desired_data_rows = 5;

    if (body_rows > related_gap_rows + related_fixed_rows + related_min_data_rows) {
        show_related_section = true;
        visible_rows =
            body_rows - related_gap_rows - related_fixed_rows - related_min_data_rows;
    }

    if (visible_rows < 1)
        visible_rows = 1;

    int category_rows_drawn = 0;
    if (ls->row_count <= 0) {
        mvwprintw(win, data_row_start, 2, "No active categories in %s", ls->month);
        category_rows_drawn = 1;
    } else {
        if (ls->cursor < 0)
            ls->cursor = 0;
        if (ls->cursor >= ls->row_count)
            ls->cursor = ls->row_count - 1;

        if (ls->cursor < ls->scroll_offset)
            ls->scroll_offset = ls->cursor;
        if (ls->cursor >= ls->scroll_offset + visible_rows)
            ls->scroll_offset = ls->cursor - visible_rows + 1;
        if (ls->scroll_offset < 0)
            ls->scroll_offset = 0;

        for (int i = 0; i < visible_rows; i++) {
            int idx = ls->scroll_offset + i;
            if (idx >= ls->row_count)
                break;

            int row = data_row_start + i;
            budget_display_row_t *drow = &ls->rows[idx];
            bool selected = (idx == ls->cursor);

            if (selected) {
                if (!focused)
                    wattron(win, A_DIM);
                wattron(win, A_REVERSE);
            }
            mvwprintw(win, row, left, "%-*s", avail, "");

            char category[80];
            if (drow->is_parent)
                snprintf(category, sizeof(category), "%s", drow->row.category_name);
            else
                snprintf(category, sizeof(category), "  - %s",
                         drow->row.category_name);
            mvwprintw(win, row, category_col, "%-*.*s", cat_w, cat_w, category);

            char budget_str[24];
            if (ls->edit_mode && selected && drow->is_parent) {
                mvwprintw(win, row, budget_col, "%-*s", budget_w, "");
                mvwprintw(win, row, budget_col, "%-*.*s", budget_w, budget_w,
                          ls->edit_buf);
            } else if (drow->row.has_rule) {
                format_cents_plain(drow->row.limit_cents, false, budget_str,
                                   sizeof(budget_str));
                mvwprintw(win, row, budget_col, "%*.*s", budget_w, budget_w,
                          budget_str);
            } else {
                wattron(win, A_DIM);
                mvwprintw(win, row, budget_col, "%*s", budget_w, "--");
                wattroff(win, A_DIM);
            }

            int64_t net_abs_cents = drow->row.net_spent_cents;
            if (net_abs_cents < 0)
                net_abs_cents = -net_abs_cents;
            char net_str[24];
            format_cents_plain(net_abs_cents, false, net_str, sizeof(net_str));
            mvwprintw(win, row, net_col, "%*.*s", net_w, net_w, net_str);

            if (drow->row.utilization_bps >= 0) {
                int util = drow->row.utilization_bps;
                int whole = util / 100;
                int frac = (util % 100) / 10;
                char pct[16];
                if (whole < 1000)
                    snprintf(pct, sizeof(pct), "%d.%d%%", whole, frac);
                else
                    snprintf(pct, sizeof(pct), "%d%%", whole);
                wattron(win, COLOR_PAIR(row_color_pair(drow)));
                mvwprintw(win, row, pct_col, "%*.*s", pct_w, pct_w, pct);
                wattroff(win, COLOR_PAIR(row_color_pair(drow)));
            } else {
                wattron(win, A_DIM);
                mvwprintw(win, row, pct_col, "%*s", pct_w, "--");
                wattroff(win, A_DIM);
            }

            if (bar_w > 0)
                draw_bar(win, row, bar_col, bar_w, drow);

            if (selected) {
                wattroff(win, A_REVERSE);
                if (!focused)
                    wattroff(win, A_DIM);
            }
            category_rows_drawn++;
        }
    }

    if (show_related_section) {
        if (category_rows_drawn < 1)
            category_rows_drawn = 1;
        int anchor_row = data_row_start + category_rows_drawn - 1;
        related_sep_row = anchor_row + related_gap_rows + 1;
        related_title_row = related_sep_row + 1;
        related_header_row = related_title_row + 1;
        related_rule_row = related_header_row + 1;
        related_data_row_start = related_rule_row + 1;
        if (related_data_row_start <= body_end_row) {
            related_visible_rows = body_end_row - related_data_row_start + 1;
            if (related_visible_rows > related_desired_data_rows)
                related_visible_rows = related_desired_data_rows;
        } else {
            related_visible_rows = 0;
        }
    }

    if (show_related_section && related_visible_rows > 0) {
        draw_related_transactions_section(
            ls, win, left, avail, related_sep_row, related_title_row,
            related_header_row, related_rule_row, related_data_row_start,
            related_visible_rows);
    }

    if (ls->edit_mode && focused && ls->row_count > 0 && ls->cursor >= 0 &&
        ls->cursor < ls->row_count && ls->rows[ls->cursor].is_parent) {
        int on_screen = ls->cursor - ls->scroll_offset;
        if (on_screen >= 0 && on_screen < visible_rows) {
            int draw_row = data_row_start + on_screen;
            int cursor_col = budget_col + ls->edit_pos;
            if (cursor_col > budget_col + budget_w - 1)
                cursor_col = budget_col + budget_w - 1;
            wmove(win, draw_row, cursor_col);
            curs_set(1);
        } else {
            curs_set(0);
        }
    } else {
        curs_set(0);
    }
}

static void begin_inline_edit(budget_list_state_t *ls) {
    if (!ls || ls->cursor < 0 || ls->cursor >= ls->row_count)
        return;
    budget_display_row_t *drow = &ls->rows[ls->cursor];
    if (!drow->is_parent) {
        snprintf(ls->message, sizeof(ls->message), "Child rows are read-only");
        return;
    }

    ls->edit_mode = true;
    if (drow->row.has_rule)
        format_budget_value(drow->row.limit_cents, ls->edit_buf,
                            sizeof(ls->edit_buf));
    else
        ls->edit_buf[0] = '\0';
    ls->edit_pos = (int)strlen(ls->edit_buf);
}

static bool handle_edit_key(budget_list_state_t *ls, int ch) {
    int len = (int)strlen(ls->edit_buf);

    if (ch == KEY_LEFT) {
        if (ls->edit_pos > 0)
            ls->edit_pos--;
        return true;
    }
    if (ch == KEY_RIGHT) {
        if (ls->edit_pos < len)
            ls->edit_pos++;
        return true;
    }
    if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
        if (ls->edit_pos > 0) {
            memmove(&ls->edit_buf[ls->edit_pos - 1], &ls->edit_buf[ls->edit_pos],
                    (size_t)(len - ls->edit_pos + 1));
            ls->edit_pos--;
        }
        return true;
    }

    if (!((ch >= '0' && ch <= '9') || ch == '.'))
        return false;
    if (len >= (int)sizeof(ls->edit_buf) - 1)
        return true;

    if (ch == '.' && strchr(ls->edit_buf, '.') != NULL)
        return true;

    memmove(&ls->edit_buf[ls->edit_pos + 1], &ls->edit_buf[ls->edit_pos],
            (size_t)(len - ls->edit_pos + 1));
    ls->edit_buf[ls->edit_pos] = (char)ch;
    ls->edit_pos++;
    return true;
}

bool budget_list_handle_input(budget_list_state_t *ls, WINDOW *parent, int ch) {
    (void)parent;
    if (!ls)
        return false;
    ls->message[0] = '\0';

    if (ls->dirty)
        reload_rows(ls);

    if (ls->edit_mode) {
        if (ch == 27) {
            ls->edit_mode = false;
            snprintf(ls->message, sizeof(ls->message), "Edit cancelled");
            return true;
        }
        if (ch == '\n') {
            if (ls->cursor < 0 || ls->cursor >= ls->row_count ||
                !ls->rows[ls->cursor].is_parent) {
                ls->edit_mode = false;
                return true;
            }

            int64_t cents = 0;
            if (!parse_budget_input_cents(ls->edit_buf, &cents)) {
                snprintf(ls->message, sizeof(ls->message), "Invalid amount");
                return true;
            }

            int rc = db_set_budget_effective(ls->db,
                                             ls->rows[ls->cursor].row.category_id,
                                             ls->month, cents);
            if (rc == 0) {
                ls->edit_mode = false;
                ls->dirty = true;
                snprintf(ls->message, sizeof(ls->message),
                         "Saved budget for %s and onward", ls->month);
            } else {
                snprintf(ls->message, sizeof(ls->message), "Error saving budget");
            }
            return true;
        }
        if (handle_edit_key(ls, ch))
            return true;
        return false;
    }

    switch (ch) {
    case KEY_LEFT:
    case 'h':
        if (month_shift(ls->month, -1))
            ls->dirty = true;
        return true;
    case KEY_RIGHT:
    case 'l':
        if (month_shift(ls->month, 1))
            ls->dirty = true;
        return true;
    case 'r':
        set_current_month(ls->month);
        ls->dirty = true;
        return true;
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
        if (ls->row_count > 0) {
            ls->cursor += 10;
            if (ls->cursor >= ls->row_count)
                ls->cursor = ls->row_count - 1;
        }
        return true;
    case KEY_PPAGE:
        if (ls->row_count > 0) {
            ls->cursor -= 10;
            if (ls->cursor < 0)
                ls->cursor = 0;
        }
        return true;
    case '\n':
        if (ls->row_count > 0)
            show_related_transactions_for_cursor(ls);
        return true;
    case 'e':
        if (ls->row_count > 0)
            begin_inline_edit(ls);
        return true;
    default:
        return false;
    }
}

const char *budget_list_status_hint(const budget_list_state_t *ls) {
    if (!ls)
        return "";
    if (ls->edit_mode)
        return "q:Quit  Enter:Save  Esc:Cancel  Left/Right:Move cursor";
    return "q:Quit  h/l:Month  r:Current month  Up/Down:Navigate  Enter:Show matches  e:Edit parent budget  Esc:Sidebar";
}

void budget_list_mark_dirty(budget_list_state_t *ls) {
    if (ls)
        ls->dirty = true;
}
