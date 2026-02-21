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

    int util = drow->row.utilization_bps;
    for (int i = 0; i < width; i++)
        mvwaddch(win, row, col + i, ' ');

    if (util < 0)
        return;

    int fill_width = width;
    if (util <= 10000)
        fill_width = (util * width) / 10000;
    if (util > 0 && fill_width == 0)
        fill_width = 1;
    if (fill_width > width)
        fill_width = width;
    if (fill_width < 0)
        fill_width = 0;

    wattron(win, COLOR_PAIR(row_color_pair(drow)));
    for (int i = 0; i < fill_width; i++)
        mvwaddch(win, row, col + i, '#');
    if (util > 10000 && fill_width > 0)
        mvwaddch(win, row, col + fill_width - 1, '+');
    wattroff(win, COLOR_PAIR(row_color_pair(drow)));

    wattron(win, A_DIM);
    for (int i = fill_width; i < width; i++)
        mvwaddch(win, row, col + i, '.');
    wattroff(win, A_DIM);
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
    if (ls->edit_mode)
        mvwprintw(win, title_row, w - 28, "Enter:Save Esc:Cancel");
    else
        mvwprintw(win, title_row, w - 40, "h/l:Month  r:Now  Enter/e:Edit");

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
    int cat_w = avail / 3;
    if (cat_w < 12)
        cat_w = 12;
    if (cat_w > 28)
        cat_w = 28;
    int bar_w = avail - cat_w - budget_w - net_w - pct_w - 4;
    if (bar_w < 8) {
        int needed = 8 - bar_w;
        cat_w -= needed;
        if (cat_w < 12)
            cat_w = 12;
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

    int visible_rows = h - 1 - data_row_start;
    if (visible_rows < 1)
        visible_rows = 1;

    if (ls->row_count <= 0) {
        mvwprintw(win, data_row_start, 2, "No active categories in %s", ls->month);
        curs_set(0);
        return;
    }

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

        mvwprintw(win, row, left, "%-*s", avail, "");

        if (selected) {
            if (!focused)
                wattron(win, A_DIM);
            wattron(win, A_REVERSE);
        }

        char category[80];
        if (drow->is_parent)
            snprintf(category, sizeof(category), "%s", drow->row.category_name);
        else
            snprintf(category, sizeof(category), "  - %s", drow->row.category_name);
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

        char net_str[24];
        format_cents_plain(drow->row.net_spent_cents, true, net_str,
                           sizeof(net_str));
        if (drow->row.net_spent_cents > 0)
            wattron(win, COLOR_PAIR(COLOR_EXPENSE));
        else if (drow->row.net_spent_cents < 0)
            wattron(win, COLOR_PAIR(COLOR_INCOME));
        mvwprintw(win, row, net_col, "%*.*s", net_w, net_w, net_str);
        if (drow->row.net_spent_cents > 0)
            wattroff(win, COLOR_PAIR(COLOR_EXPENSE));
        else if (drow->row.net_spent_cents < 0)
            wattroff(win, COLOR_PAIR(COLOR_INCOME));

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
    }

    if (ls->edit_mode && focused && ls->cursor >= 0 && ls->cursor < ls->row_count &&
        ls->rows[ls->cursor].is_parent) {
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
    return "q:Quit  h/l:Month  r:Current month  Up/Down:Navigate  Enter/e:Edit parent budget  Esc:Sidebar";
}

void budget_list_mark_dirty(budget_list_state_t *ls) {
    if (ls)
        ls->dirty = true;
}
