#include "ui/budget_list.h"

#include "db/query.h"
#include "ui/colors.h"
#include "ui/form.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    budget_row_t row;
    bool is_parent;
    bool has_running_delta;
    int64_t running_delta_cents;
} budget_display_row_t;

typedef struct {
    budget_filter_category_t category;
    bool selected;
} budget_filter_row_t;

struct budget_list_state {
    sqlite3 *db;
    char month[8]; // "YYYY-MM"

    budget_display_row_t *rows;
    int row_count;
    int row_capacity;
    int *budgeted_indices;
    int budgeted_count;
    int budgeted_capacity;
    int *unbudgeted_indices;
    int unbudgeted_count;
    int unbudgeted_capacity;

    int cursor;
    int budgeted_scroll;
    int unbudgeted_scroll;

    bool edit_mode;
    char edit_buf[32];
    int edit_pos;

    budget_txn_row_t *related_txns;
    int related_txn_count;
    bool related_visible;
    bool related_focus;
    int related_cursor;
    int related_scroll;
    int64_t related_category_id;
    char related_category_name[64];

    char message[128];
    bool dirty;

    int64_t total_budget_cents;
    int64_t total_spent_cents;
    int total_utilization_bps;
    int expected_bps;
    bool has_total_budget;
    bool has_total_running_delta;
    int64_t total_running_delta_cents;

    budget_filter_row_t *filter_rows;
    int filter_row_count;
    int filter_row_capacity;
    bool filter_panel_open;
    int filter_cursor;
    int filter_scroll;
    budget_category_filter_mode_t filter_mode;
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

static int compare_month_ym(const char *lhs, const char *rhs) {
    int lhs_y = 0, lhs_m = 0, rhs_y = 0, rhs_m = 0;
    if (!parse_month_ym(lhs, &lhs_y, &lhs_m))
        return 0;
    if (!parse_month_ym(rhs, &rhs_y, &rhs_m))
        return 0;
    if (lhs_y != rhs_y)
        return (lhs_y < rhs_y) ? -1 : 1;
    if (lhs_m != rhs_m)
        return (lhs_m < rhs_m) ? -1 : 1;
    return 0;
}

static int days_in_month(int year, int month) {
    if (year < 1900 || month < 1 || month > 12)
        return 30;

    struct tm tmv = {0};
    tmv.tm_year = year - 1900;
    tmv.tm_mon = month;
    tmv.tm_mday = 0;
    tmv.tm_hour = 12;
    tmv.tm_isdst = -1;
    if (mktime(&tmv) == (time_t)-1)
        return 30;
    if (tmv.tm_mday < 28 || tmv.tm_mday > 31)
        return 30;
    return tmv.tm_mday;
}

static int compute_expected_bps_for_view_month(const char *view_month) {
    if (!view_month)
        return 0;

    char current_month[8];
    set_current_month(current_month);

    int cmp = compare_month_ym(view_month, current_month);
    if (cmp < 0)
        return 10000;
    if (cmp > 0)
        return 0;

    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);

    int dim = days_in_month(tmv.tm_year + 1900, tmv.tm_mon + 1);
    if (dim <= 0)
        return 0;
    if (tmv.tm_mday < 1)
        return 0;
    if (tmv.tm_mday >= dim)
        return 10000;

    int64_t bps = ((int64_t)tmv.tm_mday * 10000) / dim;
    if (bps < 0)
        bps = 0;
    if (bps > 10000)
        bps = 10000;
    return (int)bps;
}

static void compute_total_progress_summary(budget_list_state_t *ls) {
    if (!ls)
        return;

    ls->total_budget_cents = 0;
    ls->total_spent_cents = 0;
    ls->total_utilization_bps = -1;
    ls->has_total_budget = false;
    ls->expected_bps = compute_expected_bps_for_view_month(ls->month);

    for (int i = 0; i < ls->row_count; i++) {
        budget_display_row_t *drow = &ls->rows[i];
        if (!drow->is_parent || !drow->row.has_rollup_rule ||
            drow->row.limit_cents <= 0)
            continue;

        int64_t spent = drow->row.net_spent_cents;
        if (spent < 0)
            spent = 0;

        ls->has_total_budget = true;
        if (ls->total_budget_cents <= INT64_MAX - drow->row.limit_cents)
            ls->total_budget_cents += drow->row.limit_cents;
        else
            ls->total_budget_cents = INT64_MAX;

        if (ls->total_spent_cents <= INT64_MAX - spent)
            ls->total_spent_cents += spent;
        else
            ls->total_spent_cents = INT64_MAX;
    }

    if (!ls->has_total_budget || ls->total_budget_cents <= 0) {
        ls->has_total_budget = false;
        ls->total_budget_cents = 0;
        ls->total_spent_cents = 0;
        ls->total_utilization_bps = -1;
        return;
    }

    if (ls->total_spent_cents > INT64_MAX / 10000) {
        ls->total_utilization_bps = INT_MAX;
        return;
    }

    int64_t util = (ls->total_spent_cents * 10000) / ls->total_budget_cents;
    if (util > INT_MAX)
        util = INT_MAX;
    ls->total_utilization_bps = (int)util;
}

static int64_t saturating_add_i64(int64_t lhs, int64_t rhs) {
    if (rhs > 0 && lhs > INT64_MAX - rhs)
        return INT64_MAX;
    if (rhs < 0 && lhs < INT64_MIN - rhs)
        return INT64_MIN;
    return lhs + rhs;
}

static int64_t saturating_sub_i64(int64_t lhs, int64_t rhs) {
    if (rhs == INT64_MIN) {
        if (lhs >= 0)
            return INT64_MAX;
        return lhs - rhs;
    }
    return saturating_add_i64(lhs, -rhs);
}

static void compute_running_delta_summary(budget_list_state_t *ls) {
    if (!ls)
        return;

    bool had_error = false;
    ls->has_total_running_delta = false;
    ls->total_running_delta_cents = 0;

    for (int i = 0; i < ls->row_count; i++) {
        budget_display_row_t *drow = &ls->rows[i];
        drow->has_running_delta = false;
        drow->running_delta_cents = 0;

        if (!drow->row.has_rollup_rule || drow->row.limit_cents <= 0)
            continue;

        int64_t actual_cents = 0;
        int64_t expected_cents = 0;
        if (db_get_budget_running_progress_for_year_before_month(
                ls->db, drow->row.category_id, ls->month, &actual_cents,
                &expected_cents) < 0) {
            had_error = true;
            continue;
        }

        drow->has_running_delta = true;
        drow->running_delta_cents =
            saturating_sub_i64(expected_cents, actual_cents);

        if (!drow->is_parent)
            continue;
        if (!ls->has_total_running_delta)
            ls->has_total_running_delta = true;
        ls->total_running_delta_cents = saturating_add_i64(
            ls->total_running_delta_cents, drow->running_delta_cents);
    }

    if (had_error && ls->message[0] == '\0')
        snprintf(ls->message, sizeof(ls->message),
                 "Error loading running surplus/deficit");
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

static void clear_category_sections(budget_list_state_t *ls) {
    if (!ls)
        return;
    free(ls->budgeted_indices);
    ls->budgeted_indices = NULL;
    ls->budgeted_count = 0;
    ls->budgeted_capacity = 0;
    free(ls->unbudgeted_indices);
    ls->unbudgeted_indices = NULL;
    ls->unbudgeted_count = 0;
    ls->unbudgeted_capacity = 0;
    ls->budgeted_scroll = 0;
    ls->unbudgeted_scroll = 0;
}

static int append_category_index(int **indices, int *count, int *capacity, int idx) {
    if (!indices || !count || !capacity || idx < 0)
        return -1;
    if (*count >= *capacity) {
        int next_capacity = (*capacity <= 0) ? 16 : (*capacity * 2);
        int *tmp = realloc(*indices, (size_t)next_capacity * sizeof(*tmp));
        if (!tmp)
            return -1;
        *indices = tmp;
        *capacity = next_capacity;
    }
    (*indices)[*count] = idx;
    (*count)++;
    return 0;
}

static int find_parent_row_index(const budget_list_state_t *ls, int64_t parent_category_id) {
    if (!ls || parent_category_id <= 0)
        return -1;
    for (int i = 0; i < ls->row_count; i++) {
        if (ls->rows[i].is_parent &&
            ls->rows[i].row.category_id == parent_category_id)
            return i;
    }
    return -1;
}

static int rebuild_category_sections(budget_list_state_t *ls) {
    if (!ls)
        return -1;
    clear_category_sections(ls);

    if (ls->row_count <= 0)
        return 0;

    bool *budgeted_mark = calloc((size_t)ls->row_count, sizeof(*budgeted_mark));
    bool *unbudgeted_mark =
        calloc((size_t)ls->row_count, sizeof(*unbudgeted_mark));
    if (!budgeted_mark || !unbudgeted_mark) {
        free(budgeted_mark);
        free(unbudgeted_mark);
        return -1;
    }

    for (int i = 0; i < ls->row_count; i++) {
        const budget_display_row_t *drow = &ls->rows[i];
        bool budgeted_row = drow->is_parent ? drow->row.has_rollup_rule
                                            : drow->row.has_rule;
        if (budgeted_row) {
            budgeted_mark[i] = true;
            continue;
        }
        if (drow->row.txn_count > 0)
            unbudgeted_mark[i] = true;
    }

    // If a parent has a direct budget, or any child has a budget,
    // show the entire parent+children family in the budgeted table.
    for (int i = 0; i < ls->row_count; i++) {
        const budget_display_row_t *parent = &ls->rows[i];
        if (!parent->is_parent)
            continue;

        bool family_budgeted = parent->row.has_rule;
        if (!family_budgeted) {
            for (int j = 0; j < ls->row_count; j++) {
                const budget_display_row_t *child = &ls->rows[j];
                if (child->is_parent ||
                    child->row.parent_category_id != parent->row.category_id)
                    continue;
                if (child->row.has_rollup_rule) {
                    family_budgeted = true;
                    break;
                }
            }
        }

        if (!family_budgeted)
            continue;

        budgeted_mark[i] = true;
        unbudgeted_mark[i] = false;
        for (int j = 0; j < ls->row_count; j++) {
            const budget_display_row_t *child = &ls->rows[j];
            if (child->is_parent ||
                child->row.parent_category_id != parent->row.category_id)
                continue;
            budgeted_mark[j] = true;
            unbudgeted_mark[j] = false;
        }
    }

    // If an unbudgeted child is shown, also show its parent for context.
    // Parent context is suppressed when that parent is already budgeted.
    for (int i = 0; i < ls->row_count; i++) {
        const budget_display_row_t *drow = &ls->rows[i];
        if (drow->is_parent || drow->row.parent_category_id <= 0 ||
            !unbudgeted_mark[i])
            continue;

        int parent_idx = find_parent_row_index(ls, drow->row.parent_category_id);
        if (parent_idx < 0)
            continue;

        if (!budgeted_mark[parent_idx])
            unbudgeted_mark[parent_idx] = true;
    }

    for (int i = 0; i < ls->row_count; i++) {
        if (budgeted_mark[i]) {
            if (append_category_index(&ls->budgeted_indices, &ls->budgeted_count,
                                      &ls->budgeted_capacity, i) < 0) {
                free(budgeted_mark);
                free(unbudgeted_mark);
                return -1;
            }
        }
        if (unbudgeted_mark[i]) {
            if (append_category_index(&ls->unbudgeted_indices, &ls->unbudgeted_count,
                                      &ls->unbudgeted_capacity, i) < 0) {
                free(budgeted_mark);
                free(unbudgeted_mark);
                return -1;
            }
        }
    }

    free(budgeted_mark);
    free(unbudgeted_mark);
    return 0;
}

static int selectable_row_count(const budget_list_state_t *ls) {
    if (!ls)
        return 0;
    return ls->budgeted_count + ls->unbudgeted_count;
}

static int row_index_for_selection(const budget_list_state_t *ls, int selection_idx) {
    if (!ls || selection_idx < 0)
        return -1;
    if (selection_idx < ls->budgeted_count) {
        if (!ls->budgeted_indices)
            return -1;
        return ls->budgeted_indices[selection_idx];
    }

    int unbudgeted_idx = selection_idx - ls->budgeted_count;
    if (unbudgeted_idx < 0 || unbudgeted_idx >= ls->unbudgeted_count ||
        !ls->unbudgeted_indices)
        return -1;
    return ls->unbudgeted_indices[unbudgeted_idx];
}

static budget_display_row_t *selected_display_row(budget_list_state_t *ls) {
    if (!ls)
        return NULL;
    int row_idx = row_index_for_selection(ls, ls->cursor);
    if (row_idx < 0 || row_idx >= ls->row_count)
        return NULL;
    return &ls->rows[row_idx];
}

static void clear_filter_rows(budget_list_state_t *ls) {
    if (!ls)
        return;
    free(ls->filter_rows);
    ls->filter_rows = NULL;
    ls->filter_row_count = 0;
    ls->filter_row_capacity = 0;
    ls->filter_cursor = 0;
    ls->filter_scroll = 0;
}

static bool selected_ids_contains(const int64_t *ids, int count, int64_t id) {
    if (!ids || count <= 0 || id <= 0)
        return false;
    for (int i = 0; i < count; i++) {
        if (ids[i] == id)
            return true;
    }
    return false;
}

static int reload_filter_rows(budget_list_state_t *ls) {
    if (!ls)
        return -1;

    clear_filter_rows(ls);

    budget_category_filter_mode_t mode = BUDGET_CATEGORY_FILTER_EXCLUDE_SELECTED;
    if (db_get_budget_category_filter_mode(ls->db, &mode) < 0)
        return -1;
    ls->filter_mode = mode;

    budget_filter_category_t *categories = NULL;
    int category_count = db_get_budget_filter_categories(ls->db, &categories);
    if (category_count < 0)
        return -1;

    int64_t *selected_ids = NULL;
    int selected_count =
        db_get_budget_category_filter_selected(ls->db, &selected_ids);
    if (selected_count < 0) {
        free(categories);
        return -1;
    }

    if (category_count > 0) {
        ls->filter_rows = calloc((size_t)category_count, sizeof(*ls->filter_rows));
        if (!ls->filter_rows) {
            free(categories);
            free(selected_ids);
            return -1;
        }
        ls->filter_row_capacity = category_count;
        for (int i = 0; i < category_count; i++) {
            ls->filter_rows[i].category = categories[i];
            ls->filter_rows[i].selected = selected_ids_contains(
                selected_ids, selected_count, categories[i].id);
        }
        ls->filter_row_count = category_count;
    }

    free(categories);
    free(selected_ids);
    if (ls->filter_cursor >= ls->filter_row_count)
        ls->filter_cursor = ls->filter_row_count > 0 ? ls->filter_row_count - 1 : 0;
    if (ls->filter_cursor < 0)
        ls->filter_cursor = 0;
    if (ls->filter_scroll < 0)
        ls->filter_scroll = 0;
    return 0;
}

static const char *
filter_mode_label(budget_category_filter_mode_t mode) {
    return (mode == BUDGET_CATEGORY_FILTER_INCLUDE_SELECTED)
               ? "Only include selected categories"
               : "Exclude selected categories";
}

static void clamp_related_selection(budget_list_state_t *ls) {
    if (!ls)
        return;
    if (ls->related_txn_count <= 0) {
        ls->related_cursor = 0;
        ls->related_scroll = 0;
        ls->related_focus = false;
        return;
    }
    if (ls->related_cursor < 0)
        ls->related_cursor = 0;
    if (ls->related_cursor >= ls->related_txn_count)
        ls->related_cursor = ls->related_txn_count - 1;
    if (ls->related_scroll < 0)
        ls->related_scroll = 0;
    if (ls->related_scroll >= ls->related_txn_count)
        ls->related_scroll = ls->related_txn_count - 1;
}

static void clear_related_transactions(budget_list_state_t *ls) {
    if (!ls)
        return;
    free(ls->related_txns);
    ls->related_txns = NULL;
    ls->related_txn_count = 0;
    ls->related_cursor = 0;
    ls->related_scroll = 0;
    ls->related_focus = false;
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
    clamp_related_selection(ls);
}

static void reload_rows(budget_list_state_t *ls) {
    if (!ls)
        return;

    ls->total_budget_cents = 0;
    ls->total_spent_cents = 0;
    ls->total_utilization_bps = -1;
    ls->has_total_budget = false;
    ls->expected_bps = compute_expected_bps_for_view_month(ls->month);

    free(ls->rows);
    ls->rows = NULL;
    ls->row_count = 0;
    ls->row_capacity = 0;
    clear_category_sections(ls);

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
    if (rebuild_category_sections(ls) < 0) {
        snprintf(ls->message, sizeof(ls->message), "Out of memory");
        clear_category_sections(ls);
    }
    compute_running_delta_summary(ls);
    compute_total_progress_summary(ls);

    int selectable_count = selectable_row_count(ls);
    if (selectable_count <= 0) {
        ls->cursor = 0;
        ls->budgeted_scroll = 0;
        ls->unbudgeted_scroll = 0;
    } else if (ls->cursor >= selectable_count) {
        ls->cursor = selectable_count - 1;
    } else if (ls->cursor < 0) {
        ls->cursor = 0;
    }

    if (ls->related_visible && ls->related_category_id > 0) {
        bool found_related_category = false;
        for (int i = 0; i < selectable_count; i++) {
            int row_idx = row_index_for_selection(ls, i);
            if (row_idx >= 0 &&
                ls->rows[row_idx].row.category_id == ls->related_category_id) {
                snprintf(ls->related_category_name,
                         sizeof(ls->related_category_name), "%s",
                         ls->rows[row_idx].row.category_name);
                found_related_category = true;
                break;
            }
        }
        if (found_related_category) {
            refresh_related_transactions(ls);
        } else {
            clear_related_transactions(ls);
            ls->related_visible = false;
            ls->related_focus = false;
            ls->related_category_id = 0;
            ls->related_category_name[0] = '\0';
        }
    } else {
        ls->related_focus = false;
    }

    ls->dirty = false;
}

budget_list_state_t *budget_list_create(sqlite3 *db) {
    budget_list_state_t *ls = calloc(1, sizeof(*ls));
    if (!ls)
        return NULL;
    ls->db = db;
    set_current_month(ls->month);
    ls->filter_mode = BUDGET_CATEGORY_FILTER_EXCLUDE_SELECTED;
    ls->dirty = true;
    return ls;
}

void budget_list_destroy(budget_list_state_t *ls) {
    if (!ls)
        return;
    free(ls->rows);
    clear_category_sections(ls);
    clear_related_transactions(ls);
    clear_filter_rows(ls);
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

static int utilization_highlight_color_pair(int utilization_bps) {
    if (utilization_bps <= 10000)
        return COLOR_HILITE_GOOD;
    if (utilization_bps <= 12500)
        return COLOR_HILITE_WARN;
    return COLOR_HILITE_BAD;
}

static int running_delta_color_pair(int64_t running_delta_cents) {
    if (running_delta_cents > 0)
        return COLOR_INCOME;
    if (running_delta_cents < 0)
        return COLOR_EXPENSE;
    return COLOR_NORMAL;
}

static int draw_clipped_text(WINDOW *win, int row, int col, int max_col,
                             const char *text) {
    if (!win || !text || col >= max_col)
        return col;
    int room = max_col - col;
    if (room <= 0)
        return col;
    int len = (int)strlen(text);
    if (len > room)
        len = room;
    if (len > 0)
        mvwaddnstr(win, row, col, text, len);
    return col + len;
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

static void draw_total_bar_with_expected(WINDOW *win, int row, int col, int width,
                                         int util_bps, int expected_bps) {
    if (!win || width <= 0)
        return;

    static const char *bar_fill = "◼";
    const int max_bps = 15000;
    const int warn_bps = 10000;
    const int danger_bps = 12500;

    for (int i = 0; i < width; i++)
        mvwaddch(win, row, col + i, ' ');

    if (util_bps >= 0) {
        int clamped_util = util_bps;
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

    int clamped_expected = expected_bps;
    if (clamped_expected < 0)
        clamped_expected = 0;
    if (clamped_expected > 10000)
        clamped_expected = 10000;
    int marker_col =
        (int)(((int64_t)clamped_expected * width + max_bps - 1) / max_bps);
    if (marker_col < 0)
        marker_col = 0;
    if (marker_col >= width)
        marker_col = width - 1;

    wattron(win, COLOR_PAIR(COLOR_INFO));
    wattron(win, A_BOLD);
    mvwaddch(win, row, col + marker_col, '|');
    wattroff(win, A_BOLD);
    wattroff(win, COLOR_PAIR(COLOR_INFO));
}

static void draw_total_bar_axis_labels(WINDOW *win, int row, int col, int width) {
    if (!win || width <= 0)
        return;

    const int max_bps = 15000;
    int zero_col = col;
    int hundred_col = col + (int)(((int64_t)10000 * width + max_bps - 1) / max_bps) - 2;
    int onefifty_col = col + width - 4;

    if (hundred_col < col)
        hundred_col = col;
    if (hundred_col > col + width - 4)
        hundred_col = col + width - 4;
    if (onefifty_col < col)
        onefifty_col = col;

    mvwprintw(win, row, col, "%-*s", width, "");
    mvwprintw(win, row, zero_col, "0%%");
    if (width >= 12)
        mvwprintw(win, row, hundred_col, "100%%");
    mvwprintw(win, row, onefifty_col, "150%%");
}

static void draw_expected_marker_label(WINDOW *win, int row, int col, int width,
                                       int expected_bps) {
    if (!win || width <= 0)
        return;

    const int max_bps = 15000;
    int clamped_expected = expected_bps;
    if (clamped_expected < 0)
        clamped_expected = 0;
    if (clamped_expected > 10000)
        clamped_expected = 10000;

    int marker_offset =
        (int)(((int64_t)clamped_expected * width + max_bps - 1) / max_bps);
    if (marker_offset < 0)
        marker_offset = 0;
    if (marker_offset >= width)
        marker_offset = width - 1;

    const char *label = "Expected";
    int label_len = (int)strlen(label);
    int label_col = col + marker_offset - (label_len / 2);
    if (label_col < col)
        label_col = col;
    if (label_col + label_len > col + width)
        label_col = col + width - label_len;
    if (label_col < col)
        label_col = col;

    mvwprintw(win, row, col, "%-*s", width, "");
    wattron(win, COLOR_PAIR(COLOR_INFO));
    mvwprintw(win, row, label_col, "%s", label);
    wattroff(win, COLOR_PAIR(COLOR_INFO));
}

static const char *category_type_short(category_type_t type) {
    return (type == CATEGORY_INCOME) ? "Inc" : "Exp";
}

static void draw_filter_panel_box(WINDOW *win, int top, int left, int height,
                                  int width) {
    if (!win || height < 3 || width < 3)
        return;
    int bottom = top + height - 1;
    int right = left + width - 1;

    for (int r = top; r <= bottom; r++)
        mvwprintw(win, r, left, "%-*s", width, "");
    for (int c = left + 1; c < right; c++) {
        mvwaddch(win, top, c, ACS_HLINE);
        mvwaddch(win, bottom, c, ACS_HLINE);
    }
    for (int r = top + 1; r < bottom; r++) {
        mvwaddch(win, r, left, ACS_VLINE);
        mvwaddch(win, r, right, ACS_VLINE);
    }
    mvwaddch(win, top, left, ACS_ULCORNER);
    mvwaddch(win, top, right, ACS_URCORNER);
    mvwaddch(win, bottom, left, ACS_LLCORNER);
    mvwaddch(win, bottom, right, ACS_LRCORNER);
}

static void draw_filter_panel(budget_list_state_t *ls, WINDOW *win) {
    if (!ls || !win || !ls->filter_panel_open)
        return;

    int h, w;
    getmaxyx(win, h, w);
    int panel_h = h - 4;
    int panel_w = w - 8;
    if (panel_h > 24)
        panel_h = 24;
    if (panel_w > 84)
        panel_w = 84;
    if (panel_h < 12 || panel_w < 38)
        return;

    int top = (h - panel_h) / 2;
    int left = (w - panel_w) / 2;
    int right = left + panel_w - 1;
    int bottom = top + panel_h - 1;

    // Keep all panel content on the modal color pair so text rows do not
    // punch through with the underlying screen background.
    wattron(win, COLOR_PAIR(COLOR_FORM));
    draw_filter_panel_box(win, top, left, panel_h, panel_w);

    wattron(win, A_BOLD);
    mvwprintw(win, top + 1, left + 2, "Budget Category Filter");
    wattroff(win, A_BOLD);

    const char *mode = filter_mode_label(ls->filter_mode);
    mvwprintw(win, top + 2, left + 2, "Mode: %s", mode);
    wattron(win, A_DIM);
    mvwprintw(win, top + 3, left + 2,
              "m:toggle mode  Space/Enter:toggle category");
    wattroff(win, A_DIM);

    int list_top = top + 5;
    int list_bottom = bottom - 3;
    int list_rows = list_bottom - list_top + 1;
    if (list_rows < 1)
        list_rows = 1;
    int list_w = panel_w - 4;
    if (list_w < 1)
        list_w = 1;

    if (ls->filter_cursor < ls->filter_scroll)
        ls->filter_scroll = ls->filter_cursor;
    if (ls->filter_cursor >= ls->filter_scroll + list_rows)
        ls->filter_scroll = ls->filter_cursor - list_rows + 1;
    if (ls->filter_scroll < 0)
        ls->filter_scroll = 0;

    if (ls->filter_row_count <= 0) {
        wattron(win, A_DIM);
        mvwprintw(win, list_top, left + 2, "No categories available");
        wattroff(win, A_DIM);
    } else {
        for (int i = 0; i < list_rows; i++) {
            int idx = ls->filter_scroll + i;
            int row = list_top + i;
            mvwprintw(win, row, left + 2, "%-*s", list_w, "");
            if (idx >= ls->filter_row_count)
                continue;

            bool selected_row = (idx == ls->filter_cursor);
            if (selected_row)
                wattron(win, A_REVERSE);

            const budget_filter_row_t *frow = &ls->filter_rows[idx];
            const char *mark = frow->selected ? "x" : " ";
            if (frow->category.parent_id > 0) {
                char label[128];
                snprintf(label, sizeof(label), "[%s]   - %s", mark,
                         frow->category.name);
                mvwprintw(win, row, left + 2, "%-*.*s", list_w, list_w, label);
            } else {
                char label[128];
                snprintf(label, sizeof(label), "[%s] [%s] %s", mark,
                         category_type_short(frow->category.type),
                         frow->category.name);
                mvwprintw(win, row, left + 2, "%-*.*s", list_w, list_w, label);
            }

            if (selected_row)
                wattroff(win, A_REVERSE);
        }
    }

    if (ls->filter_scroll > 0)
        mvwaddch(win, list_top, right - 1, ACS_UARROW);
    if (ls->filter_scroll + list_rows < ls->filter_row_count)
        mvwaddch(win, list_bottom, right - 1, ACS_DARROW);

    wattron(win, A_DIM);
    mvwprintw(win, bottom - 1, left + 2, "Esc/f:close");
    wattroff(win, A_DIM);
    wattroff(win, COLOR_PAIR(COLOR_FORM));
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
                                              int visible_rows,
                                              int selected_idx, int scroll,
                                              bool section_focused) {
    if (!ls || !win || visible_rows <= 0)
        return;

    if (separator_row >= 0) {
        for (int col = left; col < left + avail; col++)
            mvwaddch(win, separator_row, col, ACS_HLINE);
    }

    int data_row_end = data_row_start + visible_rows - 1;
    for (int row = title_row; row <= data_row_end; row++)
        mvwprintw(win, row, left, "%-*s", avail, "");

    char title[256];
    if (!ls->related_visible) {
        snprintf(title, sizeof(title),
                 "Matching Transactions (press Enter on a category row)");
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
    mvwprintw(win, header_row, amount_col, "%*.*s", amount_w, amount_w, "Amount");
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

    if (scroll < 0)
        scroll = 0;
    if (scroll >= ls->related_txn_count)
        scroll = ls->related_txn_count - 1;

    int shown = ls->related_txn_count - scroll;
    if (shown > visible_rows)
        shown = visible_rows;

    for (int i = 0; i < shown; i++) {
        int row = data_row_start + i;
        int txn_idx = scroll + i;
        const budget_txn_row_t *txn = &ls->related_txns[txn_idx];
        bool selected = (txn_idx == selected_idx);

        if (selected) {
            if (!section_focused)
                wattron(win, A_DIM);
            wattron(win, A_REVERSE);
        }

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

        if (selected) {
            wattroff(win, A_REVERSE);
            if (!section_focused)
                wattroff(win, A_DIM);
        }
    }
}

static void show_related_transactions_for_cursor(budget_list_state_t *ls) {
    if (!ls)
        return;

    budget_display_row_t *drow = selected_display_row(ls);
    if (!drow)
        return;
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
    ls->related_focus = (related_count > 0);
    ls->related_cursor = 0;
    ls->related_scroll = 0;
    ls->related_category_id = drow->row.category_id;
    snprintf(ls->related_category_name, sizeof(ls->related_category_name), "%s",
             drow->row.category_name);
    clamp_related_selection(ls);
}

static bool edit_selected_related_transaction(budget_list_state_t *ls, WINDOW *parent) {
    if (!ls || !parent || !ls->related_focus || ls->related_txn_count <= 0 ||
        !ls->related_txns)
        return false;
    clamp_related_selection(ls);
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

static void draw_budget_category_row(
    budget_list_state_t *ls, WINDOW *win, int row, int left, int avail,
    int category_col, int cat_w, int budget_col, int budget_w, int running_col,
    int running_w, int net_col, int net_w, int pct_col, int pct_w, int bar_col,
    int bar_w, budget_display_row_t *drow, bool selected, bool focused) {
    if (!ls || !win || !drow)
        return;

    if (selected) {
        if (!focused)
            wattron(win, A_DIM);
        wattron(win, A_REVERSE);
    }
    mvwprintw(win, row, left, "%-*s", avail, "");

    char category[80];
    if (drow->is_parent) {
        snprintf(category, sizeof(category), "%s", drow->row.category_name);
    } else {
        int parent_idx = find_parent_row_index(ls, drow->row.parent_category_id);
        if (parent_idx >= 0) {
            snprintf(category, sizeof(category), "%.*s:%.*s", 39,
                     ls->rows[parent_idx].row.category_name,
                     39,
                     drow->row.category_name);
        } else {
            snprintf(category, sizeof(category), "%s", drow->row.category_name);
        }
    }
    mvwprintw(win, row, category_col, "%-*.*s", cat_w, cat_w, category);

    char budget_str[24];
    bool row_has_budget = drow->is_parent ? drow->row.has_rollup_rule
                                          : drow->row.has_rule;
    if (ls->edit_mode && selected) {
        mvwprintw(win, row, budget_col, "%-*s", budget_w, "");
        mvwprintw(win, row, budget_col, "%-*.*s", budget_w, budget_w,
                  ls->edit_buf);
    } else if (row_has_budget) {
        format_cents_plain(drow->row.limit_cents, false, budget_str,
                           sizeof(budget_str));
        mvwprintw(win, row, budget_col, "%*.*s", budget_w, budget_w, budget_str);
    } else {
        wattron(win, A_DIM);
        mvwprintw(win, row, budget_col, "%*s", budget_w, "--");
        wattroff(win, A_DIM);
    }

    if (drow->has_running_delta) {
        char running_str[24];
        format_cents_plain(drow->running_delta_cents, true, running_str,
                           sizeof(running_str));
        wattron(win, COLOR_PAIR(running_delta_color_pair(drow->running_delta_cents)));
        mvwprintw(win, row, running_col, "%*.*s", running_w, running_w, running_str);
        wattroff(win,
                 COLOR_PAIR(running_delta_color_pair(drow->running_delta_cents)));
    } else {
        wattron(win, A_DIM);
        mvwprintw(win, row, running_col, "%*s", running_w, "--");
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
}

static void keep_section_scroll_visible(int *scroll, int count, int visible_rows,
                                        int selected_idx) {
    if (!scroll)
        return;
    if (count <= 0 || visible_rows <= 0) {
        *scroll = 0;
        return;
    }
    if (*scroll < 0)
        *scroll = 0;
    int max_scroll = count - visible_rows;
    if (max_scroll < 0)
        max_scroll = 0;
    if (*scroll > max_scroll)
        *scroll = max_scroll;
    if (selected_idx < 0 || selected_idx >= count)
        return;
    if (selected_idx < *scroll)
        *scroll = selected_idx;
    if (selected_idx >= *scroll + visible_rows)
        *scroll = selected_idx - visible_rows + 1;
    if (*scroll < 0)
        *scroll = 0;
    if (*scroll > max_scroll)
        *scroll = max_scroll;
}

void budget_list_draw(budget_list_state_t *ls, WINDOW *win, bool focused) {
    if (!ls || !win)
        return;
    if (ls->dirty)
        reload_rows(ls);

    int h, w;
    getmaxyx(win, h, w);
    if (h < 13 || w < 44) {
        mvwprintw(win, 1, 2, "Window too small for Budgets");
        curs_set(0);
        return;
    }

    int title_row = 1;
    int msg_row = 2;
    int summary_row = 3;
    int progress_label_row = 5;
    int progress_row = 6;
    int progress_axis_row = 7;
    int tables_start_row = 9;

    mvwprintw(win, title_row, 2, "Budgets  Month:%s", ls->month);

    mvwprintw(win, msg_row, 2, "%-*s", w - 4, "");
    if (ls->message[0] != '\0')
        mvwprintw(win, msg_row, 2, "%s", ls->message);

    int left = 2;
    int avail = w - 4;
    if (avail < 20)
        return;

    mvwprintw(win, summary_row, left, "%-*s", avail, "");
    if (ls->has_total_budget) {
        char spent_str[24];
        char budget_str[24];
        format_cents_plain(ls->total_spent_cents, false, spent_str,
                           sizeof(spent_str));
        format_cents_plain(ls->total_budget_cents, false, budget_str,
                           sizeof(budget_str));

        int util_whole = ls->total_utilization_bps / 100;
        int util_frac = (ls->total_utilization_bps % 100) / 10;
        int expected_whole = ls->expected_bps / 100;
        int expected_frac = (ls->expected_bps % 100) / 10;

        char util_text[24];
        snprintf(util_text, sizeof(util_text), "%d.%d%%", util_whole, util_frac);

        int col = left;
        int max_col = left + avail;
        char summary_prefix[128];
        snprintf(summary_prefix, sizeof(summary_prefix), "Total: %s / %s  ",
                 spent_str, budget_str);
        col = draw_clipped_text(win, summary_row, col, max_col, summary_prefix);

        if (col < max_col) {
            int util_color_pair =
                utilization_highlight_color_pair(ls->total_utilization_bps);
            wattron(win, COLOR_PAIR(util_color_pair) | A_BOLD);
            col = draw_clipped_text(win, summary_row, col, max_col, util_text);
            wattroff(win, COLOR_PAIR(util_color_pair) | A_BOLD);
        }

        char summary_suffix[128];
        snprintf(summary_suffix, sizeof(summary_suffix), "  Expected: %d.%d%%",
                 expected_whole, expected_frac);
        col = draw_clipped_text(win, summary_row, col, max_col, summary_suffix);

        if (ls->has_total_running_delta) {
            char running_str[24];
            format_cents_plain(ls->total_running_delta_cents, true, running_str,
                               sizeof(running_str));
            col = draw_clipped_text(win, summary_row, col, max_col, "  Running ");
            if (col < max_col) {
                char running_text[64];
                snprintf(running_text, sizeof(running_text), "%s %s",
                         running_str,
                         ls->total_running_delta_cents >= 0 ? "surplus"
                                                             : "deficit");
                int running_color = ls->total_running_delta_cents >= 0
                                        ? COLOR_INCOME
                                        : COLOR_EXPENSE;
                wattron(win, COLOR_PAIR(running_color));
                col = draw_clipped_text(win, summary_row, col, max_col, running_text);
                wattroff(win, COLOR_PAIR(running_color));
            }
        }
    } else {
        mvwprintw(win, summary_row, left, "%-*.*s", avail, avail,
                  "Total: -- (no parent budget limits)");
    }

    mvwprintw(win, progress_row, left, "%-*s", avail, "");
    mvwprintw(win, progress_label_row, left, "%-*s", avail, "");
    mvwprintw(win, progress_axis_row, left, "%-*s", avail, "");
    int progress_label_w = 9;
    int progress_bar_col = left + progress_label_w + 1;
    int progress_bar_w = avail - progress_label_w - 1;

    mvwprintw(win, progress_row, left, "%-*s", progress_label_w, "Progress");
    if (ls->has_total_budget && progress_bar_w >= 10) {
        draw_expected_marker_label(win, progress_label_row, progress_bar_col,
                                   progress_bar_w, ls->expected_bps);
        draw_total_bar_with_expected(win, progress_row, progress_bar_col,
                                     progress_bar_w, ls->total_utilization_bps,
                                     ls->expected_bps);
        draw_total_bar_axis_labels(win, progress_axis_row, progress_bar_col,
                                   progress_bar_w);
    } else if (progress_bar_w >= 10) {
        mvwprintw(win, progress_row, progress_bar_col, "%-*s", progress_bar_w,
                  "(no parent budget limits)");
    }

    int budget_w = 12;
    int running_w = 12;
    int net_w = 12;
    int pct_w = 7;
    int min_cat_w = 10;
    int min_bar_w = 10; // 150% / 10 bars = 15% per bar max
    int cat_w = avail / 3;
    if (cat_w < min_cat_w)
        cat_w = min_cat_w;
    if (cat_w > 28)
        cat_w = 28;
    int bar_w = avail - cat_w - budget_w - running_w - net_w - pct_w - 5;
    if (bar_w < min_bar_w) {
        int needed = min_bar_w - bar_w;
        cat_w -= needed;
        if (cat_w < min_cat_w)
            cat_w = min_cat_w;
        bar_w = avail - cat_w - budget_w - running_w - net_w - pct_w - 5;
    }
    if (bar_w < 0)
        bar_w = 0;

    int category_col = left;
    int budget_col = category_col + cat_w + 1;
    int running_col = budget_col + budget_w + 1;
    int net_col = running_col + running_w + 1;
    int pct_col = net_col + net_w + 1;
    int bar_col = pct_col + pct_w + 1;

    int body_start_row = tables_start_row;
    int body_end_row = h - 2;
    int body_rows = body_end_row - body_start_row + 1;
    if (body_rows < 1)
        body_rows = 1;

    const int table_fixed_rows = 3; // title + header + rule
    const int related_fixed_rows = 3; // title + header + rule
    const int table_gap_rows = 4;
    const int min_table_data_rows = 1;

    int budgeted_needed = ls->budgeted_count > 0 ? ls->budgeted_count : 1;
    int unbudgeted_needed = ls->unbudgeted_count > 0 ? ls->unbudgeted_count : 1;
    int related_needed = 1;
    if (ls->related_visible && ls->related_txn_count > 0)
        related_needed = ls->related_txn_count;

    int min_required_rows = table_fixed_rows + min_table_data_rows +
                            table_gap_rows + table_fixed_rows +
                            min_table_data_rows + table_gap_rows +
                            related_fixed_rows + min_table_data_rows;
    if (body_rows < min_required_rows) {
        mvwprintw(win, tables_start_row, left, "Window too small for budget tables");
        curs_set(0);
        if (ls->filter_panel_open)
            draw_filter_panel(ls, win);
        return;
    }

    int budgeted_visible_rows = min_table_data_rows;
    int unbudgeted_visible_rows = min_table_data_rows;
    int related_visible_rows = min_table_data_rows;
    int spare_rows = body_rows - min_required_rows;

    int add = related_needed - related_visible_rows;
    if (add > 0) {
        if (add > spare_rows)
            add = spare_rows;
        related_visible_rows += add;
        spare_rows -= add;
    }

    add = budgeted_needed - budgeted_visible_rows;
    if (add > 0) {
        if (add > spare_rows)
            add = spare_rows;
        budgeted_visible_rows += add;
        spare_rows -= add;
    }

    add = unbudgeted_needed - unbudgeted_visible_rows;
    if (add > 0) {
        if (add > spare_rows)
            add = spare_rows;
        unbudgeted_visible_rows += add;
        spare_rows -= add;
    }

    int total_selectable = selectable_row_count(ls);
    if (total_selectable <= 0) {
        ls->cursor = 0;
    } else {
        if (ls->cursor < 0)
            ls->cursor = 0;
        if (ls->cursor >= total_selectable)
            ls->cursor = total_selectable - 1;
    }
    bool cursor_in_budgeted = ls->cursor < ls->budgeted_count;
    int cursor_budgeted_idx = cursor_in_budgeted ? ls->cursor : -1;
    int cursor_unbudgeted_idx =
        cursor_in_budgeted ? -1 : ls->cursor - ls->budgeted_count;

    keep_section_scroll_visible(&ls->budgeted_scroll, ls->budgeted_count,
                                budgeted_visible_rows, cursor_budgeted_idx);
    keep_section_scroll_visible(&ls->unbudgeted_scroll, ls->unbudgeted_count,
                                unbudgeted_visible_rows, cursor_unbudgeted_idx);
    int related_selected_idx = ls->related_focus ? ls->related_cursor : -1;
    keep_section_scroll_visible(&ls->related_scroll, ls->related_txn_count,
                                related_visible_rows, related_selected_idx);

    int selected_draw_row = -1;

    int budgeted_title_row = body_start_row;
    int budgeted_header_row = budgeted_title_row + 1;
    int budgeted_rule_row = budgeted_header_row + 1;
    int budgeted_data_row_start = budgeted_rule_row + 1;
    int unbudgeted_title_row =
        budgeted_data_row_start + budgeted_visible_rows + table_gap_rows;
    int unbudgeted_header_row = unbudgeted_title_row + 1;
    int unbudgeted_rule_row = unbudgeted_header_row + 1;
    int unbudgeted_data_row_start = unbudgeted_rule_row + 1;
    int related_title_row =
        unbudgeted_data_row_start + unbudgeted_visible_rows + table_gap_rows;
    int related_header_row = related_title_row + 1;
    int related_rule_row = related_header_row + 1;
    int related_data_row_start = related_rule_row + 1;

    for (int row = body_start_row; row <= body_end_row; row++)
        mvwprintw(win, row, left, "%-*s", avail, "");

    wattron(win, A_BOLD);
    mvwprintw(win, budgeted_title_row, left, "Budgeted Categories");
    mvwprintw(win, unbudgeted_title_row, left, "Unbudgeted Categories");
    wattroff(win, A_BOLD);

    wattron(win, A_BOLD);
    mvwprintw(win, budgeted_header_row, category_col, "%-*s", cat_w, "Category");
    mvwprintw(win, budgeted_header_row, budget_col, "%*.*s", budget_w, budget_w,
              "Budget");
    mvwprintw(win, budgeted_header_row, running_col, "%*.*s", running_w,
              running_w, "Running +/-");
    mvwprintw(win, budgeted_header_row, net_col, "%*.*s", net_w, net_w, "Net");
    mvwprintw(win, budgeted_header_row, pct_col, "%*.*s", pct_w, pct_w, "%");
    if (bar_w > 0)
        mvwprintw(win, budgeted_header_row, bar_col, "%-*s", bar_w, "Progress");

    mvwprintw(win, unbudgeted_header_row, category_col, "%-*s", cat_w, "Category");
    mvwprintw(win, unbudgeted_header_row, budget_col, "%*.*s", budget_w,
              budget_w, "Budget");
    mvwprintw(win, unbudgeted_header_row, running_col, "%*.*s", running_w,
              running_w, "Running +/-");
    mvwprintw(win, unbudgeted_header_row, net_col, "%*.*s", net_w, net_w, "Net");
    mvwprintw(win, unbudgeted_header_row, pct_col, "%*.*s", pct_w, pct_w, "%");
    if (bar_w > 0)
        mvwprintw(win, unbudgeted_header_row, bar_col, "%-*s", bar_w, "Progress");
    wattroff(win, A_BOLD);

    for (int col = left; col < w - 2; col++) {
        mvwaddch(win, budgeted_rule_row, col, ACS_HLINE);
        mvwaddch(win, unbudgeted_rule_row, col, ACS_HLINE);
    }

    if (ls->budgeted_count <= 0) {
        wattron(win, A_DIM);
        mvwprintw(win, budgeted_data_row_start, left, "No categories with budgets set");
        wattroff(win, A_DIM);
    } else {
        for (int i = 0; i < budgeted_visible_rows; i++) {
            int section_idx = ls->budgeted_scroll + i;
            if (section_idx >= ls->budgeted_count)
                break;
            int row_idx = ls->budgeted_indices[section_idx];
            if (row_idx < 0 || row_idx >= ls->row_count)
                continue;
            int draw_row = budgeted_data_row_start + i;
            int selection_idx = section_idx;
            bool selected = (!ls->related_focus && selection_idx == ls->cursor);
            draw_budget_category_row(
                ls, win, draw_row, left, avail, category_col, cat_w, budget_col,
                budget_w, running_col, running_w, net_col, net_w, pct_col, pct_w,
                bar_col, bar_w, &ls->rows[row_idx], selected, focused);
            if (selected)
                selected_draw_row = draw_row;
        }
    }

    if (ls->unbudgeted_count <= 0) {
        wattron(win, A_DIM);
        mvwprintw(win, unbudgeted_data_row_start, left,
                  "No unbudgeted categories with transactions in %s", ls->month);
        wattroff(win, A_DIM);
    } else {
        for (int i = 0; i < unbudgeted_visible_rows; i++) {
            int section_idx = ls->unbudgeted_scroll + i;
            if (section_idx >= ls->unbudgeted_count)
                break;
            int row_idx = ls->unbudgeted_indices[section_idx];
            if (row_idx < 0 || row_idx >= ls->row_count)
                continue;
            int draw_row = unbudgeted_data_row_start + i;
            int selection_idx = ls->budgeted_count + section_idx;
            bool selected = (!ls->related_focus && selection_idx == ls->cursor);
            draw_budget_category_row(
                ls, win, draw_row, left, avail, category_col, cat_w, budget_col,
                budget_w, running_col, running_w, net_col, net_w, pct_col, pct_w,
                bar_col, bar_w, &ls->rows[row_idx], selected, focused);
            if (selected)
                selected_draw_row = draw_row;
        }
    }

    if (related_visible_rows > 0) {
        draw_related_transactions_section(
            ls, win, left, avail, -1, related_title_row,
            related_header_row, related_rule_row, related_data_row_start,
            related_visible_rows, related_selected_idx, ls->related_scroll,
            focused && ls->related_focus);
    }

    if (ls->filter_panel_open) {
        draw_filter_panel(ls, win);
        curs_set(0);
        return;
    }

    budget_display_row_t *selected_row = selected_display_row(ls);
    if (ls->edit_mode && focused && selected_row && selected_draw_row >= 0) {
        int cursor_col = budget_col + ls->edit_pos;
        if (cursor_col > budget_col + budget_w - 1)
            cursor_col = budget_col + budget_w - 1;
        wmove(win, selected_draw_row, cursor_col);
        curs_set(1);
    } else {
        curs_set(0);
    }
}

static void begin_inline_edit(budget_list_state_t *ls) {
    if (!ls)
        return;
    budget_display_row_t *drow = selected_display_row(ls);
    if (!drow)
        return;

    ls->edit_mode = true;
    if (drow->row.has_rule)
        format_budget_value(drow->row.direct_limit_cents, ls->edit_buf,
                            sizeof(ls->edit_buf));
    else
        ls->edit_buf[0] = '\0';
    ls->edit_pos = (int)strlen(ls->edit_buf);
}

static void open_filter_panel(budget_list_state_t *ls) {
    if (!ls)
        return;
    if (reload_filter_rows(ls) < 0) {
        snprintf(ls->message, sizeof(ls->message), "Error loading category filters");
        return;
    }
    ls->filter_panel_open = true;
}

static bool handle_filter_panel_key(budget_list_state_t *ls, int ch) {
    if (!ls || !ls->filter_panel_open)
        return false;

    switch (ch) {
    case 27:
    case 'f':
        ls->filter_panel_open = false;
        return true;
    case KEY_UP:
    case 'k':
        if (ls->filter_row_count > 0 && ls->filter_cursor > 0)
            ls->filter_cursor--;
        return true;
    case KEY_DOWN:
    case 'j':
        if (ls->filter_row_count > 0 && ls->filter_cursor < ls->filter_row_count - 1)
            ls->filter_cursor++;
        return true;
    case KEY_HOME:
    case 'g':
        ls->filter_cursor = 0;
        return true;
    case KEY_END:
    case 'G':
        if (ls->filter_row_count > 0)
            ls->filter_cursor = ls->filter_row_count - 1;
        return true;
    case KEY_NPAGE:
        if (ls->filter_row_count > 0) {
            ls->filter_cursor += 8;
            if (ls->filter_cursor >= ls->filter_row_count)
                ls->filter_cursor = ls->filter_row_count - 1;
        }
        return true;
    case KEY_PPAGE:
        if (ls->filter_row_count > 0) {
            ls->filter_cursor -= 8;
            if (ls->filter_cursor < 0)
                ls->filter_cursor = 0;
        }
        return true;
    case 'm': {
        budget_category_filter_mode_t next_mode =
            (ls->filter_mode == BUDGET_CATEGORY_FILTER_INCLUDE_SELECTED)
                ? BUDGET_CATEGORY_FILTER_EXCLUDE_SELECTED
                : BUDGET_CATEGORY_FILTER_INCLUDE_SELECTED;
        if (db_set_budget_category_filter_mode(ls->db, next_mode) < 0) {
            snprintf(ls->message, sizeof(ls->message), "Error saving filter mode");
            return true;
        }
        ls->filter_mode = next_mode;
        ls->dirty = true;
        snprintf(ls->message, sizeof(ls->message), "%s",
                 filter_mode_label(next_mode));
        return true;
    }
    case ' ':
    case '\n': {
        if (ls->filter_row_count <= 0 || ls->filter_cursor < 0 ||
            ls->filter_cursor >= ls->filter_row_count)
            return true;
        budget_filter_row_t *row = &ls->filter_rows[ls->filter_cursor];
        if (db_set_budget_category_filter_selected(ls->db, row->category.id,
                                                   !row->selected) < 0) {
            snprintf(ls->message, sizeof(ls->message),
                     "Error saving category filter");
            return true;
        }
        row->selected = !row->selected;
        ls->dirty = true;
        return true;
    }
    default:
        return false;
    }
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
    if (!ls)
        return false;
    ls->message[0] = '\0';

    if (ls->dirty)
        reload_rows(ls);

    int selectable_count = selectable_row_count(ls);

    if (ls->edit_mode) {
        if (ch == 27) {
            ls->edit_mode = false;
            snprintf(ls->message, sizeof(ls->message), "Edit cancelled");
            return true;
        }
        if (ch == '\n') {
            budget_display_row_t *selected = selected_display_row(ls);
            if (!selected) {
                ls->edit_mode = false;
                return true;
            }

            int64_t cents = 0;
            if (!parse_budget_input_cents(ls->edit_buf, &cents)) {
                snprintf(ls->message, sizeof(ls->message), "Invalid amount");
                return true;
            }

            int rc = db_set_budget_effective(ls->db, selected->row.category_id,
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

    if (ls->filter_panel_open)
        return handle_filter_panel_key(ls, ch);

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
        case KEY_HOME:
        case 'g':
            ls->related_cursor = 0;
            return true;
        case KEY_END:
        case 'G':
            if (ls->related_txn_count > 0)
                ls->related_cursor = ls->related_txn_count - 1;
            return true;
        case KEY_NPAGE:
            if (ls->related_txn_count > 0) {
                ls->related_cursor += 10;
                if (ls->related_cursor >= ls->related_txn_count)
                    ls->related_cursor = ls->related_txn_count - 1;
            }
            return true;
        case KEY_PPAGE:
            if (ls->related_txn_count > 0) {
                ls->related_cursor -= 10;
                if (ls->related_cursor < 0)
                    ls->related_cursor = 0;
            }
            return true;
        case '\t':
        case 27:
        case '\n':
            ls->related_focus = false;
            return true;
        case 'e':
            return edit_selected_related_transaction(ls, parent);
        default:
            break;
        }
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
        if (selectable_count > 0 && ls->cursor > 0)
            ls->cursor--;
        return true;
    case KEY_DOWN:
    case 'j':
        if (selectable_count > 0 && ls->cursor < selectable_count - 1)
            ls->cursor++;
        return true;
    case KEY_HOME:
    case 'g':
        ls->cursor = 0;
        return true;
    case KEY_END:
    case 'G':
        if (selectable_count > 0)
            ls->cursor = selectable_count - 1;
        return true;
    case KEY_NPAGE:
        if (selectable_count > 0) {
            ls->cursor += 10;
            if (ls->cursor >= selectable_count)
                ls->cursor = selectable_count - 1;
        }
        return true;
    case KEY_PPAGE:
        if (selectable_count > 0) {
            ls->cursor -= 10;
            if (ls->cursor < 0)
                ls->cursor = 0;
        }
        return true;
    case '\n':
        if (selectable_count > 0)
            show_related_transactions_for_cursor(ls);
        return true;
    case 'e':
        if (selectable_count > 0)
            begin_inline_edit(ls);
        return true;
    case 'f':
        open_filter_panel(ls);
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
    if (ls->filter_panel_open)
        return "q:Quit  Up/Down:Navigate  Space/Enter:Toggle category  m:Toggle mode  Esc/f:Close filter";
    if (ls->related_focus)
        return "q:Quit  Up/Down:Navigate transactions  e:Edit transaction  Enter/Tab/Esc:Back to categories";
    return "q:Quit  h/l:Month  r:Current month  Up/Down:Navigate  Enter:Show matches  e:Edit budget  f:Filter categories  Esc:Sidebar";
}

void budget_list_mark_dirty(budget_list_state_t *ls) {
    if (ls) {
        ls->dirty = true;
        if (ls->filter_panel_open && reload_filter_rows(ls) < 0) {
            snprintf(ls->message, sizeof(ls->message),
                     "Error loading category filters");
            ls->filter_panel_open = false;
        }
    }
}
